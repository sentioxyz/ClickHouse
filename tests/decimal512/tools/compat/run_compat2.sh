#!/usr/bin/env bash
# On-disk cross-version compatibility (isolated: `clickhouse local --path <private dir>`, no listening ports).
#   run_compat2.sh <out-dir> <writer-name>=<binary> <reader-name>=<binary>...
# The writer creates the compat.* tables (write_dyn_json.sql) in <out-dir>/data.<writer> and reads them back itself;
# each reader works on its own copy of that directory and runs read_dyn_json.sql. The writer's own read is the
# expected output; reader outputs are diffed against it. Then every engine, the writer included (its run is the
# expected output), merges its own aggregate states into a copy of the writer's AggregatingMergeTree table
# (mixed_agg.sql, OPTIMIZE FINAL): lines `mixed=<engine> rc=<n> SAME|DIFF|ERROR(...)`.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1; shift
mkdir -p "$OUT"
W=${1%%=*}; WB=${1#*=}; shift
run() {  # binary, data dir, sql file, out prefix
  (cd "$2" && timeout 600 "$1" local --path "$2" --multiquery --queries-file "$3" < /dev/null > "$4.out" 2> "$4.err"); echo $?
}
D=$OUT/data.$W
rm -rf "$D"; mkdir -p "$D"
wrc=$(run "$WB" "$D" "$HERE/write_dyn_json.sql" "$OUT/write.$W")
rrc=$(run "$WB" "$D" "$HERE/read_dyn_json.sql" "$OUT/read.$W.by.$W")
printf 'writer=%s write_rc=%s self_read_rc=%s\n' "$W" "$wrc" "$rrc" | tee "$OUT/summary.$W.txt"
for kv in "$@"; do
  R=${kv%%=*}; RB=${kv#*=}
  C=$OUT/copy.$W.for.$R
  rm -rf "$C"; cp -a "$D" "$C"
  rc=$(run "$RB" "$C" "$HERE/read_dyn_json.sql" "$OUT/read.$W.by.$R")
  if [ "$rc" = 0 ] && diff -q "$OUT/read.$W.by.$W.out" "$OUT/read.$W.by.$R.out" > /dev/null; then verdict=SAME
  elif [ "$rc" = 0 ]; then verdict=DIFF
  else verdict="ERROR(rc=$rc; code mod 256) $(grep -m1 -oE 'Code: [0-9]+\. DB::Exception: .{0,200}' "$OUT/read.$W.by.$R.err")"; fi
  printf 'reader=%s rc=%s %s\n' "$R" "$rc" "$verdict" | tee -a "$OUT/summary.$W.txt"
  [ "$verdict" = DIFF ] && diff "$OUT/read.$W.by.$W.out" "$OUT/read.$W.by.$R.out" > "$OUT/diff.$W.by.$R.txt"
  rm -rf "$C"
done
for kv in "$W=$WB" "$@"; do
  R=${kv%%=*}; RB=${kv#*=}
  C=$OUT/mixed.$W.by.$R
  rm -rf "$C"; cp -a "$D" "$C"
  rc=$(run "$RB" "$C" "$HERE/mixed_agg.sql" "$OUT/mixed.$W.by.$R")
  if [ "$rc" = 0 ] && diff -q "$OUT/mixed.$W.by.$W.out" "$OUT/mixed.$W.by.$R.out" > /dev/null; then verdict=SAME
  elif [ "$rc" = 0 ]; then verdict=DIFF
  else verdict="ERROR(rc=$rc; code mod 256) $(grep -m1 -oE 'Code: [0-9]+\. DB::Exception: .{0,200}' "$OUT/mixed.$W.by.$R.err")"; fi
  printf 'mixed=%s rc=%s %s\n' "$R" "$rc" "$verdict" | tee -a "$OUT/summary.$W.txt"
  [ "$verdict" = DIFF ] && diff "$OUT/mixed.$W.by.$W.out" "$OUT/mixed.$W.by.$R.out" > "$OUT/diff.mixed.$W.by.$R.txt"
  rm -rf "$C"
done
