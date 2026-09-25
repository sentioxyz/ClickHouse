#!/usr/bin/env bash
# mixed_distributed_groupby.sh - distributed GROUP BY across two ClickHouse builds (a rolling upgrade: one shard on the
# baseline, one on the candidate), compared with the same query on candidate-only shards.
#
# Why: the fork added 512-bit fixed-key aggregation methods (keys512, nullable_keys512) and the KeysNullMap fix changes
# which method nullable 29..64-byte keys use. Partially aggregated blocks travel between shards as key columns, but
# two-level aggregation with distributed_aggregation_memory_efficient = 1 merges bucket by bucket and so assumes every
# shard puts a key into the same bucket; a method change between versions could split one key over buckets and return
# duplicate groups.
#
# PRODUCTION BOUNDARY: four isolated loopback instances started (and proven isolated) by isolated_ch.sh: a and d =
# baseline, b and c = candidate. Queries go only to them. Nothing else is contacted.
#
#   mixed_distributed_groupby.sh <out-dir> --baseline <old clickhouse> --candidate <new clickhouse> [--tree <source tree>]
#   (--tree: a ClickHouse source tree for isolated_ch.sh; default: the tree this script is in, under tests/decimal512/tools)
#
# Per key shape and setting variant (the same data on every shard pair: a and c hold part 0, b and d part 1):
#   mixed, initiator a (baseline) or b (candidate), shards a + b  -> the rolling-upgrade state
#   candidate reference: initiator b, shards b + c (after the upgrade)
#   baseline reference:  initiator a, shards a + d (before the upgrade)
# Verdicts per mixed row: SAME (equals the candidate reference), AS_BASELINE (equals the baseline reference: the
# pre-upgrade behaviour, wrong only where the baseline itself is wrong), NEW_DUPLICATES (a key appears twice although
# neither homogeneous cluster returns duplicates: a mixed-version merge problem), DIFF (anything else). Shapes with a
# known single-node defect of the baseline (KD-KEYSNULLMAP-512: nullable fixed keys of 29..64 bytes) are marked: the
# baseline shard's partial aggregation is itself wrong there, so SAME/AS_BASELINE/DIFF are all possible and expected.
# Exit 0 when every row of an unaffected shape is SAME and no row has NEW_DUPLICATES; 1 otherwise (a mixed-version
# merge problem); 2 usage or isolation refused.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
# isolated_ch.sh: CH_ISO_SCRIPT, else the skill layout (scripts/), else the tests/decimal512/tools layout
ISO=${CH_ISO_SCRIPT:-}
[ -n "$ISO" ] || for c in "$HERE/../../scripts/isolated_ch.sh" "$HERE/../isolated_ch.sh"; do [ -f "$c" ] && ISO=$c && break; done
[ -f "$ISO" ] || { echo "ERROR: isolated_ch.sh not found (set CH_ISO_SCRIPT)" >&2; exit 2; }
[ $# -ge 1 ] || { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
OUT=$1; shift
BASE= CAND= TREE=$(cd "$HERE/../../../.." && pwd)
while [ $# -gt 0 ]; do
  case $1 in --baseline) BASE=$(readlink -f "$2"); shift 2;; --candidate) CAND=$(readlink -f "$2"); shift 2;; --tree) TREE=$2; shift 2;; *) exit 2;; esac
done
[ -x "$BASE" ] && [ -x "$CAND" ] || { echo "ERROR: --baseline and --candidate must be executables" >&2; exit 2; }
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: $OUT is not empty" >&2; exit 2; fi
mkdir -p "$OUT/results" "$OUT/logs"; OUT=$(cd "$OUT" && pwd)
export CH_ISO_ROOT=$OUT/iso CH_ISO_LOGS=$OUT/logs
declare -A PORT=([a]=39000 [b]=49000 [c]=59000 [d]=29000) BIN=([a]=$BASE [b]=$CAND [c]=$CAND [d]=$BASE)
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$OUT/run.log" >&2; }
cleanup() { for i in a b c d; do bash "$ISO" stop "$i" >> "$OUT/logs/stop.log" 2>&1; done; rm -f "$OUT"/iso/bin/clickhouse-*; }
trap cleanup EXIT
printf 'role\tpath\tsha256\tbuild_id\n' > "$OUT/identities.tsv"
for r in "baseline $BASE" "candidate $CAND"; do set -- $r
  printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$(sha256sum "$2" | cut -d' ' -f1)" "$(readelf -n "$2" | awk '/Build ID/{print $3}')" >> "$OUT/identities.tsv"
done
for i in a b c d; do
  bash "$ISO" start "$i" "${BIN[$i]}" "$TREE" > "$OUT/logs/start_$i.log" 2>&1 || { log "REFUSE: instance $i did not start (see logs/start_$i.log)"; exit 2; }
done
q() {  # <instance> <sql...>: one query on that instance with the candidate client
  "$CAND" client --host 127.0.0.1 --port "${PORT[$1]}" --query "$2" < /dev/null
}

# the same deterministic data on every shard pair: a and c get part 0, b gets part 1 (the INSERTs run on each instance's
# own server, so they use only functions the baseline has: no 512-bit arithmetic)
SCHEMA="k_nu256 Nullable(UInt256), k_ni64_1 Nullable(Int64), k_ni64_2 Nullable(Int64), k_ni64_3 Nullable(Int64), k_ni64_4 Nullable(Int64),
        k_u256 UInt256, k_u128 UInt128, k_i512 Int512, k_d512 Decimal(154, 60), k_nd76 Nullable(Decimal(76, 30)), k_u64 UInt64, v UInt64"
for i in a b c d; do
  q "$i" "DROP TABLE IF EXISTS gb" && q "$i" "CREATE TABLE gb ($SCHEMA) ENGINE = MergeTree ORDER BY tuple()" || { log "ERROR: create on $i"; exit 1; }
done
fill() {  # <instance> <part>
  q "$1" "INSERT INTO gb SELECT
      if(number % 4 = 0, NULL, toUInt256(number % 50) * toUInt256('340282366920938463463374607431768211456')),
      if(number % 5 = 0, NULL, number % 7), if(number % 6 = 0, NULL, number % 3), number % 2, if(number % 9 = 0, NULL, number % 5),
      toUInt256(number % 40), toUInt128(number % 3), toInt512(toInt64(number % 30) - 15), toDecimal512(toString(number % 25) || '.5', 60),
      if(number % 3 = 0, NULL, toDecimal256(toString(number % 11), 30)), number % 13, number
    FROM numbers($2 * 100000, 100000)"
}
fill a 0 && fill b 1 && fill c 0 && fill d 1 || { log "ERROR: insert"; exit 1; }

# key shape | GROUP BY list | known single-node defect of the baseline for this shape
SHAPES=(
  "nullable_u256_single|k_nu256|KD-KEYSNULLMAP-512"
  "nullable_i64_x4|k_ni64_1, k_ni64_2, k_ni64_3, k_ni64_4|KD-KEYSNULLMAP-512"
  "u256_u128_48b|k_u256, k_u128|"
  "int512_single|k_i512|"
  "decimal512_single|k_d512|"
  "u64_nullable_d76|k_u64, k_nd76|KD-KEYSNULLMAP-512"
  "u256_u64_40b|k_u256, k_u64|"
)
VARIANTS=(
  "default|"
  "two_level|SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1"
  "two_level_memory_efficient|SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1, distributed_aggregation_memory_efficient = 1"
  "two_level_not_memory_efficient|SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1, distributed_aggregation_memory_efficient = 0"
)
printf 'shape\tvariant\tinitiator\tverdict\tgroups\tresult_sha256\tcandidate_ref_sha256\tbaseline_ref_sha256\tref_dups(cand,base)\tknown_baseline_defect\n' > "$OUT/results/matrix.tsv"
bad=0
for s in "${SHAPES[@]}"; do
  IFS='|' read -r sname keys kd <<< "$s"
  for v in "${VARIANTS[@]}"; do
    IFS='|' read -r vname vset <<< "$v"
    sql() { echo "SELECT $keys, count() AS c, sum(v) AS s FROM remote('$1', currentDatabase(), gb) GROUP BY $keys ORDER BY $keys, c, s $vset"; }
    nkeys=$(echo "$keys" | tr ',' '\n' | wc -l)
    dups_of() { cut -f1-"$nkeys" "$1" | sort | uniq -d | wc -l; }
    q b "$(sql "127.0.0.1:${PORT[b]},127.0.0.1:${PORT[c]}")" > "$OUT/results/${sname}.${vname}.ref_candidate.tsv" 2> "$OUT/results/${sname}.${vname}.ref_candidate.err"
    q a "$(sql "127.0.0.1:${PORT[a]},127.0.0.1:${PORT[d]}")" > "$OUT/results/${sname}.${vname}.ref_baseline.tsv" 2> "$OUT/results/${sname}.${vname}.ref_baseline.err"
    refc=$(sha256sum < "$OUT/results/${sname}.${vname}.ref_candidate.tsv" | cut -c1-64)
    refb=$(sha256sum < "$OUT/results/${sname}.${vname}.ref_baseline.tsv" | cut -c1-64)
    dc=$(dups_of "$OUT/results/${sname}.${vname}.ref_candidate.tsv"); db=$(dups_of "$OUT/results/${sname}.${vname}.ref_baseline.tsv")
    for init in a b; do
      f=$OUT/results/${sname}.${vname}.init_$init.tsv
      q "$init" "$(sql "127.0.0.1:${PORT[a]},127.0.0.1:${PORT[b]}")" > "$f" 2> "${f%.tsv}.err"
      got=$(sha256sum < "$f" | cut -c1-64)
      dups=$(dups_of "$f")
      if [ -s "${f%.tsv}.err" ] || [ -s "$OUT/results/${sname}.${vname}.ref_candidate.err" ]; then verdict=ERROR
      elif [ "$got" = "$refc" ]; then verdict=SAME
      elif [ "$got" = "$refb" ]; then verdict=AS_BASELINE
      elif [ "$dups" -gt 0 ] && [ "$dc" = 0 ] && [ "$db" = 0 ]; then verdict=NEW_DUPLICATES
      else verdict=DIFF; fi
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$sname" "$vname" "$init" "$verdict" "$(wc -l < "$f")" "$got" "$refc" "$refb" "$dc,$db" "${kd:--}" >> "$OUT/results/matrix.tsv"
      if [ "$verdict" = NEW_DUPLICATES ] || [ "$verdict" = ERROR ] || { [ "$verdict" != SAME ] && [ -z "$kd" ]; }; then bad=$((bad + 1)); fi
    done
  done
done
column -t -s $'\t' "$OUT/results/matrix.tsv" | tee "$OUT/results/matrix.txt" >&2
log "rows needing attention: $bad"
[ $bad = 0 ] && exit 0 || exit 1
