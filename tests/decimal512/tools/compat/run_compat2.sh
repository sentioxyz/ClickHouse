#!/usr/bin/env bash
# On-disk cross-version compatibility (isolated: `clickhouse local --path <private dir>`, no listening ports).
#   run_compat2.sh <out-dir> <writer-name>=<binary> <reader-name>=<binary>...
# The writer creates the compat.* tables (write_dyn_json.sql) in <out-dir>/data.<writer> and reads them back itself;
# each reader works on its own copy of that directory. read_plain.sql (plain 512-bit columns, aggregate states, parts)
# runs as one file: `reader=<engine> rc=<n> SAME|DIFF|ERROR(...)`. Every line of read_dyn_json.sql (Dynamic/JSON values)
# runs on its own, so one refusal cannot hide what the other queries return:
# `reader_dyn=<engine> query=<tag> rc=<n> SAME|DIFF|ERROR(...)`. The writer's own read is the expected output. Then every engine, the writer included (its run is the
# expected output), merges its own aggregate states into a copy of the writer's AggregatingMergeTree table
# (mixed_agg.sql, OPTIMIZE FINAL): lines `mixed=<engine> rc=<n> SAME|DIFF|ERROR(...)`.
# The summary starts with one `engine=<name> sha256=<hex> build_id=<hex> path=<binary>` line per engine, measured
# before the run: the labels are bound to binaries (check_gate.py requires it), not trusted as names.
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
: > "$OUT/summary.$W.txt"
for kv in "$W=$WB" "$@"; do
  b=$(readlink -f "${kv#*=}")
  printf 'engine=%s sha256=%s build_id=%s path=%s\n' "${kv%%=*}" "$(sha256sum "$b" | cut -d' ' -f1)" \
    "$(readelf -n "$b" 2>/dev/null | awk '/Build ID/{print $3}')" "$b" | tee -a "$OUT/summary.$W.txt"
done
Q=$OUT/dyn_queries; rm -rf "$Q"; mkdir -p "$Q"
grep '^SELECT' "$HERE/read_dyn_json.sql" | while IFS= read -r line; do
  tag=$(sed -E "s/^SELECT '([a-z_]+)'.*/\1/" <<< "$line"); printf '%s\n' "$line" > "$Q/$tag.sql"
done
TAGS=$(ls "$Q" | sed 's/\.sql$//' | sort)
verdict_of() {  # rc expected actual errfile
  if [ "$1" = 0 ] && diff -q "$2" "$3" > /dev/null; then echo SAME
  elif [ "$1" = 0 ]; then echo DIFF
  else echo "ERROR(rc=$1; code mod 256) $(grep -m1 -oE 'Code: [0-9]+\. DB::Exception: .{0,200}' "$4")"; fi
}
wrc=$(run "$WB" "$D" "$HERE/write_dyn_json.sql" "$OUT/write.$W")
rrc=$(run "$WB" "$D" "$HERE/read_plain.sql" "$OUT/read.$W.by.$W")
for t in $TAGS; do x=$(run "$WB" "$D" "$Q/$t.sql" "$OUT/read_dyn.$t.$W.by.$W"); [ "$x" = 0 ] || rrc=$x; done
printf 'writer=%s write_rc=%s self_read_rc=%s\n' "$W" "$wrc" "$rrc" | tee -a "$OUT/summary.$W.txt"
for kv in "$@"; do
  R=${kv%%=*}; RB=${kv#*=}
  C=$OUT/copy.$W.for.$R
  rm -rf "$C"; cp -a "$D" "$C"
  rc=$(run "$RB" "$C" "$HERE/read_plain.sql" "$OUT/read.$W.by.$R")
  verdict=$(verdict_of "$rc" "$OUT/read.$W.by.$W.out" "$OUT/read.$W.by.$R.out" "$OUT/read.$W.by.$R.err")
  printf 'reader=%s rc=%s %s\n' "$R" "$rc" "$verdict" | tee -a "$OUT/summary.$W.txt"
  [ "$verdict" = DIFF ] && diff "$OUT/read.$W.by.$W.out" "$OUT/read.$W.by.$R.out" > "$OUT/diff.$W.by.$R.txt"
  for t in $TAGS; do
    rc=$(run "$RB" "$C" "$Q/$t.sql" "$OUT/read_dyn.$t.$W.by.$R")
    verdict=$(verdict_of "$rc" "$OUT/read_dyn.$t.$W.by.$W.out" "$OUT/read_dyn.$t.$W.by.$R.out" "$OUT/read_dyn.$t.$W.by.$R.err")
    printf 'reader_dyn=%s query=%s rc=%s %s\n' "$R" "$t" "$rc" "$verdict" | tee -a "$OUT/summary.$W.txt"
    [ "$verdict" = DIFF ] && diff "$OUT/read_dyn.$t.$W.by.$W.out" "$OUT/read_dyn.$t.$W.by.$R.out" > "$OUT/diff.dyn.$t.$W.by.$R.txt"
  done
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
