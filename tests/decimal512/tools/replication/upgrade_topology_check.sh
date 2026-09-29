#!/usr/bin/env bash
# upgrade_topology_check.sh - isolated validation of an upgrade topology in which no distributed query spans both builds
# (the resolution candidate for KD-D512-MIXED-GROUPBY-NULLABLE), and of the ways it can break.
#
# PRODUCTION BOUNDARY: four isolated loopback instances started (and proven isolated) by isolated_ch.sh. Nothing else is
# contacted. This validates a procedure; it changes no deployment and approves no rollout.
#
#   upgrade_topology_check.sh <out-dir> --baseline <old clickhouse> --candidate <new clickhouse> [--tree <source tree>]
#
# Layout: two shards with two replicas each, the same data as mixed_distributed_groupby.sh (groupby_oracle.py):
#   shard 1 (part 0): a = old replica ("group A"), c = new replica ("group B")
#   shard 2 (part 1): d = old replica ("group A"), b = new replica ("group B")
# Procedure under test: upgrade group B while group A serves every distributed query; then switch the coordinator AND the
# shard list to group B at once; then upgrade group A. Checks (every key shape of the oracle, single- and two-level):
#   phase_A          coordinator a, shards a,d (group A only)  -> the old build's own behaviour (recorded, not judged)
#   phase_B          coordinator c, shards c,b (group B only)  -> must equal the oracle
#   phase_B_other    coordinator b, shards b,c                 -> must equal the oracle (either group-B coordinator)
#   hazard_failover  coordinator c, shards 'c|a,b|d' (cross-group replicas, load_balancing=in_order) with b stopped:
#                    shard 2 falls back to the old replica d -> mixed execution (expected: not the oracle)
#   guard_group_only coordinator c, shards c,b with b stopped -> the query must fail, never return a result
#   hazard_old_coord coordinator a (old), shards c,b (new)     -> mixed execution (expected: not the oracle)
# Exit 0 when phase_B/phase_B_other equal the oracle for every case, guard_group_only fails closed, and both hazards are
# demonstrated (so the rules they motivate are real); 1 otherwise; 2 usage, lock or isolation problems.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ISO=${CH_ISO_SCRIPT:-}
[ -n "$ISO" ] || for c in "$HERE/../../scripts/isolated_ch.sh" "$HERE/../isolated_ch.sh"; do [ -f "$c" ] && ISO=$c && break; done
[ -f "$ISO" ] || { echo "ERROR: isolated_ch.sh not found (set CH_ISO_SCRIPT)" >&2; exit 2; }
LOCK=${GROUPBY_LOCK:-$HERE/groupby_cases.lock.json}
[ $# -ge 1 ] || { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
OUT=$1; shift
BASE= CAND= TREE=$(cd "$HERE/../../../.." && pwd)
while [ $# -gt 0 ]; do
  case $1 in --baseline) BASE=$(readlink -f "$2"); shift 2;; --candidate) CAND=$(readlink -f "$2"); shift 2;; --tree) TREE=$2; shift 2;; *) exit 2;; esac
done
[ -x "$BASE" ] && [ -x "$CAND" ] || { echo "ERROR: --baseline and --candidate must be executables" >&2; exit 2; }
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: $OUT is not empty" >&2; exit 2; fi
mkdir -p "$OUT/results" "$OUT/logs"; OUT=$(cd "$OUT" && pwd)
python3 "$HERE/groupby_oracle.py" check "$LOCK" > "$OUT/logs/lock_check.txt" 2>&1 || { echo "ERROR: lock does not match the oracle" >&2; exit 2; }
export CH_ISO_ROOT=$OUT/iso CH_ISO_LOGS=$OUT/logs
declare -A PORT=([a]=39000 [b]=49000 [c]=59000 [d]=29000) BIN=([a]=$BASE [b]=$CAND [c]=$CAND [d]=$BASE)
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$OUT/run.log" >&2; }
cleanup() { for i in a b c d; do bash "$ISO" stop "$i" >> "$OUT/logs/stop.log" 2>&1; done; rm -f "$OUT"/iso/bin/clickhouse-*; }
trap cleanup EXIT
printf 'role\tpath\tsha256\tbuild_id\n' > "$OUT/identities.tsv"
for r in "baseline $BASE" "candidate $CAND"; do set -- $r
  printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$(sha256sum "$2" | cut -d' ' -f1)" "$(readelf -n "$2" | awk '/Build ID/{print $3}')" >> "$OUT/identities.tsv"
done
start() { bash "$ISO" start "$1" "${BIN[$1]}" "$TREE" >> "$OUT/logs/start_$1.log" 2>&1; }
for i in a b c d; do start "$i" || { log "REFUSE: instance $i did not start"; exit 2; }; done
q() { "$CAND" client --host 127.0.0.1 --port "${PORT[$1]}" --query "$2" < /dev/null; }

SCHEMA="k_nu256 Nullable(UInt256), k_ni64_1 Nullable(Int64), k_ni64_2 Nullable(Int64), k_ni64_3 Nullable(Int64), k_ni64_4 Nullable(Int64),
        k_u256 UInt256, k_u128 UInt128, k_i512 Int512, k_d512 Decimal(154, 60), k_nd76 Nullable(Decimal(76, 30)), k_u64 UInt64, v UInt64"
for i in a b c d; do q "$i" "DROP TABLE IF EXISTS gb" && q "$i" "CREATE TABLE gb ($SCHEMA) ENGINE = MergeTree ORDER BY tuple()" || exit 1; done
fill() {
  q "$1" "INSERT INTO gb SELECT
      if(number % 4 = 0, NULL, toUInt256(number % 50) * toUInt256('340282366920938463463374607431768211456')),
      if(number % 5 = 0, NULL, number % 7), if(number % 6 = 0, NULL, number % 3), number % 2, if(number % 9 = 0, NULL, number % 5),
      toUInt256(number % 40), toUInt128(number % 3), toInt512(toInt64(number % 30) - 15), toDecimal512(toString(number % 25) || '.5', 60),
      if(number % 3 = 0, NULL, toDecimal256(toString(number % 11), 30)), number % 13, number
    FROM numbers($2 * 100000, 100000)"
}
fill a 0 && fill c 0 && fill d 1 && fill b 1 || { log "ERROR: insert"; exit 1; }

declare -A KEYS=([nullable_u256_single]="k_nu256" [nullable_i64_x4]="k_ni64_1, k_ni64_2, k_ni64_3, k_ni64_4" [u256_u128_48b]="k_u256, k_u128"
                 [int512_single]="k_i512" [decimal512_single]="k_d512" [u64_nullable_d76]="k_u64, k_nd76" [u256_u64_40b]="k_u256, k_u64")
declare -A SETS=([single_level]="" [two_level]="group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1")
declare -A EXPECT
while IFS=$'\t' read -r shape sha; do EXPECT[$shape]=$sha; done < <(python3 -c '
import json, sys
seen = {}
for c in json.load(open(sys.argv[1]))["cases"]:
    seen[c["shape"]] = c["expected_sha256"]
for s, h in seen.items(): print(f"{s}\t{h}")' "$LOCK")

p(){ echo "127.0.0.1:${PORT[$1]}"; }
printf 'check\tshape\tvariant\tcoordinator\tshards\toutcome\tresult_sha256\n' > "$OUT/results/checks.tsv"
run_check() {  # <check> <coordinator> <remote hosts expr> <extra settings>
  local check=$1 coord=$2 hosts=$3 extra=$4
  for shape in "${!KEYS[@]}"; do
    for var in single_level two_level; do
      local keys=${KEYS[$shape]} f=$OUT/results/$check.$shape.$var.tsv s=${SETS[$var]}
      [ -n "$extra" ] && s="${s:+$s, }$extra"
      q "$coord" "SELECT $keys, count() AS c, sum(v) AS s FROM remote('$hosts', currentDatabase(), gb) GROUP BY $keys ORDER BY $keys, c, s ${s:+SETTINGS $s}" \
        > "$f" 2> "${f%.tsv}.err"
      local got; got=$(sha256sum < "$f" | cut -c1-64)
      local outcome
      if [ -s "${f%.tsv}.err" ]; then outcome=ERROR; elif [ "$got" = "${EXPECT[$shape]}" ]; then outcome=ORACLE; else outcome=WRONG; fi
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$check" "$shape" "$var" "$coord" "$hosts" "$outcome" "$got" >> "$OUT/results/checks.tsv"
    done
  done
}
run_check phase_A a "$(p a),$(p d)" ""
run_check phase_B c "$(p c),$(p b)" ""
run_check phase_B_other b "$(p b),$(p c)" ""
run_check hazard_old_coord a "$(p c),$(p b)" ""
run_check cross_group_healthy c "$(p c)|$(p a),$(p b)|$(p d)" "load_balancing = 'in_order'"
bash "$ISO" stop b >> "$OUT/logs/stop.log" 2>&1
run_check hazard_failover c "$(p c)|$(p a),$(p b)|$(p d)" "load_balancing = 'in_order'"
run_check guard_group_only c "$(p c),$(p b)" "connections_with_failover_max_tries = 1, connect_timeout_with_failover_ms = 200"

python3 - "$OUT/results/checks.tsv" "$OUT/results/verdict.txt" <<'EOF'
import collections, sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1])][1:]
by = collections.defaultdict(collections.Counter)
for r in rows:
    by[r[0]][r[5]] += 1
nullable = ("nullable_u256_single", "nullable_i64_x4", "u64_nullable_d76")
def all_are(check, outcome, shapes=None):
    rs = [r for r in rows if r[0] == check and (shapes is None or r[1] in shapes)]
    return bool(rs) and all(r[5] == outcome for r in rs)
def any_is(check, outcome, shapes=None):
    return any(r[0] == check and r[5] == outcome and (shapes is None or r[1] in shapes) for r in rows)
v = {
    "phase_B equals the oracle (group B only, coordinator c)": all_are("phase_B", "ORACLE"),
    "phase_B_other equals the oracle (group B only, coordinator b)": all_are("phase_B_other", "ORACLE"),
    "cross-group replica list while group B is healthy equals the oracle": all_are("cross_group_healthy", "ORACLE"),
    "hazard: cross-group failover (b stopped) gives wrong nullable-key results": any_is("hazard_failover", "WRONG", nullable),
    "hazard: old coordinator over group B gives wrong nullable-key results": any_is("hazard_old_coord", "WRONG", nullable),
    "guard: group-only list with b stopped fails, never returns a result": all_are("guard_group_only", "ERROR"),
}
with open(sys.argv[2], "w") as f:
    for check, c in sorted(by.items()):
        f.write(f"{check}: {dict(c)}\n")
    for k, ok in v.items():
        f.write(f"{'OK  ' if ok else 'FAIL'} {k}\n")
    f.write("RESULT: " + ("PASS" if all(v.values()) else "FAIL") + "\n")
print(open(sys.argv[2]).read())
sys.exit(0 if all(v.values()) else 1)
EOF
rc=$?
log "topology check rc=$rc"
exit $rc
