#!/usr/bin/env bash
# mixed_distributed_groupby.sh - distributed GROUP BY across shards of two ClickHouse builds (the state of a rolling
# upgrade), checked against independent expectations.
#
# Why: the KeysNullMap fix (bae5ba481f3) changed how nullable fixed keys of 29..64 bytes are packed (the old build had
# no room for the null bitmap), which changes their hash and two-level bucket. Partial aggregates of a distributed query
# are merged by bucket, so shards of the two builds can split one key over two buckets. This check measures that, for
# both coordinator directions, single-level and two-level aggregation, with ordinary key types as controls.
#
# PRODUCTION BOUNDARY: four isolated loopback instances started (and proven isolated) by isolated_ch.sh: a and d run the
# baseline ("old"), b and c the candidate ("new"). Queries go only to them. Nothing else is contacted.
#
#   mixed_distributed_groupby.sh <out-dir> --baseline <old clickhouse> --candidate <new clickhouse> [--tree <source tree>]
#   (--tree: a ClickHouse source tree for isolated_ch.sh; default: the tree this script is in, under tests/decimal512/tools)
#
# Cases come from groupby_cases.lock.json (groupby_oracle.py: 7 key shapes x 4 aggregation variants x 4 topologies).
# Every shard pair holds the same data (n = 0..199999 split in two parts), so each case has one correct answer, computed
# by groupby_oracle.py without any ClickHouse build. Topologies (initiator over two shards):
#   new_only        new over new+new   (after the upgrade)
#   mixed_init_new  new over old+new   (rolling upgrade, new coordinator)
#   mixed_init_old  old over old+new   (rolling upgrade, old coordinator)
#   old_only        old over old+old   (before the upgrade: recorded, not gated)
# results/cases.tsv: case, topology, shape, variant, gated, status (PASS = result sha256 equals the oracle's), groups,
# duplicate keys, sha256s. Exit 0 when every gated case passes; 1 otherwise; 2 usage, lock or isolation problems.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
# isolated_ch.sh: CH_ISO_SCRIPT, else the skill layout (scripts/), else the tests/decimal512/tools layout
ISO=${CH_ISO_SCRIPT:-}
[ -n "$ISO" ] || for c in "$HERE/../../scripts/isolated_ch.sh" "$HERE/../isolated_ch.sh"; do [ -f "$c" ] && ISO=$c && break; done
[ -f "$ISO" ] || { echo "ERROR: isolated_ch.sh not found (set CH_ISO_SCRIPT)" >&2; exit 2; }
LOCK=${GROUPBY_LOCK:-$HERE/groupby_cases.lock.json}
ORACLE=$HERE/groupby_oracle.py
[ $# -ge 1 ] || { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
OUT=$1; shift
BASE= CAND= TREE=$(cd "$HERE/../../../.." && pwd)
while [ $# -gt 0 ]; do
  case $1 in --baseline) BASE=$(readlink -f "$2"); shift 2;; --candidate) CAND=$(readlink -f "$2"); shift 2;; --tree) TREE=$2; shift 2;; *) exit 2;; esac
done
[ -x "$BASE" ] && [ -x "$CAND" ] || { echo "ERROR: --baseline and --candidate must be executables" >&2; exit 2; }
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: $OUT is not empty" >&2; exit 2; fi
mkdir -p "$OUT/results" "$OUT/logs"; OUT=$(cd "$OUT" && pwd)
python3 "$ORACLE" check "$LOCK" > "$OUT/logs/lock_check.txt" 2>&1 || { echo "ERROR: $LOCK does not match groupby_oracle.py" >&2; exit 2; }
cp "$LOCK" "$OUT/groupby_cases.lock.json"
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

# the same deterministic data on every shard pair: a and c get part 0, b and d get part 1 (the INSERTs run on each
# instance's own server, so they use only functions the baseline has: no 512-bit arithmetic). groupby_oracle.py row()
# must stay identical to these formulas.
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

declare -A KEYS=(
  [nullable_u256_single]="k_nu256"
  [nullable_i64_x4]="k_ni64_1, k_ni64_2, k_ni64_3, k_ni64_4"
  [u256_u128_48b]="k_u256, k_u128"
  [int512_single]="k_i512"
  [decimal512_single]="k_d512"
  [u64_nullable_d76]="k_u64, k_nd76"
  [u256_u64_40b]="k_u256, k_u64"
)
declare -A SETS=(
  [default]=""
  [two_level]="SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1"
  [two_level_memory_efficient]="SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1, distributed_aggregation_memory_efficient = 1"
  [two_level_not_memory_efficient]="SETTINGS group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1, distributed_aggregation_memory_efficient = 0"
)
# topology -> initiator instance and its two shards (old = a/d, new = b/c; each pair holds part 0 and part 1)
declare -A INIT=([new_only]=b [mixed_init_new]=b [mixed_init_old]=a [old_only]=a)
declare -A SHARDS=([new_only]="b c" [mixed_init_new]="a b" [mixed_init_old]="a b" [old_only]="a d")

printf 'case\ttopology\tshape\tvariant\tgated\tstatus\tgroups\tduplicate_keys\tresult_sha256\texpected_sha256\n' > "$OUT/results/cases.tsv"
bad=0
while IFS=$'\t' read -r cid topo shape var gated want; do
  keys=${KEYS[$shape]:-}; [ -n "$keys" ] || { log "ERROR: unknown shape $shape in the lock"; exit 2; }
  [ -n "${INIT[$topo]:-}" ] || { log "ERROR: unknown topology $topo in the lock"; exit 2; }
  set -- ${SHARDS[$topo]}
  hosts="127.0.0.1:${PORT[$1]},127.0.0.1:${PORT[$2]}"
  f=$OUT/results/${cid//:/.}.tsv
  q "${INIT[$topo]}" "SELECT $keys, count() AS c, sum(v) AS s FROM remote('$hosts', currentDatabase(), gb) GROUP BY $keys ORDER BY $keys, c, s ${SETS[$var]}" \
    > "$f" 2> "${f%.tsv}.err"
  got=$(sha256sum < "$f" | cut -c1-64)
  nkeys=$(echo "$keys" | tr ',' '\n' | wc -l)
  dups=$(cut -f1-"$nkeys" "$f" | sort | uniq -d | wc -l)
  if [ -s "${f%.tsv}.err" ]; then status=ERROR; elif [ "$got" = "$want" ]; then status=PASS; else status=FAIL; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$cid" "$topo" "$shape" "$var" "$gated" "$status" "$(wc -l < "$f")" "$dups" "$got" "$want" >> "$OUT/results/cases.tsv"
  [ "$gated" = True ] && [ "$status" != PASS ] && bad=$((bad + 1))
done < <(python3 -c '
import json, sys
for c in json.load(open(sys.argv[1]))["cases"]:
    print("\t".join([c["id"], c["topology"], c["shape"], c["variant"], str(c["gated"]), c["expected_sha256"]]))' "$LOCK")

python3 - "$OUT/results/cases.tsv" > "$OUT/results/summary.txt" <<'EOF'
import collections, sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])][1:]
by, dmax = collections.defaultdict(collections.Counter), collections.defaultdict(int)
for r in rows:
    by[(r[1], r[2])][r[5]] += 1
    dmax[(r[1], r[2])] = max(dmax[(r[1], r[2])], int(r[7]))
print("topology\tshape\tresult per variant (of 4)\tduplicate keys (max)")
for (t, s), c in sorted(by.items()):
    print(f"{t}\t{s}\t{dict(c)}\t{dmax[(t, s)]}")
EOF
column -t -s $'\t' "$OUT/results/summary.txt" | tee "$OUT/results/summary_table.txt" >&2
log "gated cases not passing: $bad"
[ $bad = 0 ] && exit 0 || exit 1
