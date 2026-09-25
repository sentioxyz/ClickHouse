#!/usr/bin/env bash
# hist_run.sh - historical-dataset experiments on private copies (clickhouse local --path; no ports, no network).
#
# PRODUCTION BOUNDARY: runs `clickhouse local` only, on directories created under the given output paths, with
# stdin=/dev/null. It connects nowhere, never touches real data and never deletes anything outside its output paths.
#
#   hist_run.sh generate <gen-dir> <out-dir> <old-binary>
#       the OLD binary executes create.sql statement by statement into <out-dir>/data (a failing statement is recorded
#       in create_log.tsv as an unsupported combination, never skipped silently); then the dataset is frozen:
#       manifest.sha256 (every file) and dataset.sha256, plus RowBinary dumps of the dumped tables by the OLD binary
#   hist_run.sh verify <data-dir> <gen-dir> <binary> <label> <phase> <out-dir>
#       copies <data-dir>, runs verify_<phase>.sql and the RowBinary dumps with <binary>, then hist_check.py compares
#       both with oracle.json (phase: initial | after_rewrite); the frozen <data-dir> is never modified
#   hist_run.sh rewrite <data-dir> <gen-dir> <binary> <out-dir>
#       copies <data-dir> to <out-dir>/data and runs rewrite.sql there with <binary> (new rows, merges, a mutation,
#       a projection); read the result with `verify ... after_rewrite` (e.g. by the old binary: rollback after a rewrite)
# Exit: generate/rewrite 0 when every statement succeeded, 1 otherwise; verify returns hist_check.py's exit (0 all
# checks pass, 1 some fail); 2 usage.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
DUMP_TABLES="scales_wide scales_compact ints keys codecs"
chq() {  # <binary> <data dir> <sql...>: one clickhouse local process on the private data directory
  local b=$1 d=$2; shift 2
  (cd "$d/.." && timeout 600 "$b" local --path "$d" "$@" < /dev/null)
}
ident() { printf '%s\t%s\t%s\n' "$(readlink -f "$1")" "$(sha256sum "$1" | cut -d' ' -f1)" "$(readelf -n "$1" 2>/dev/null | awk '/Build ID/{print $3}')"; }
run_statements() {  # <binary> <data dir> <sql file> <log tsv>: statement by statement, rc and first error line per statement
  local b=$1 d=$2 f=$3 log=$4 n=0 bad=0
  printf 'n\trc\terror\tstatement\n' > "$log"
  while IFS= read -r st; do
    [ -n "$st" ] || continue
    n=$((n + 1))
    # through a file, not --query: a long INSERT exceeds the 128 KiB limit of one command-line argument (exec fails, rc 126)
    printf '%s\n' "$st" > "$d/../statement.sql"
    err=$(chq "$b" "$d" --queries-file "$d/../statement.sql" 2>&1 >/dev/null); rc=$?
    [ $rc = 0 ] || bad=$((bad + 1))
    printf '%s\t%s\t%s\t%s\n' "$n" "$rc" "$(echo "$err" | grep -m1 -oE 'Code: [0-9]+\. DB::Exception: .{0,180}' | tr '\t' ' ')" "$(echo "$st" | cut -c1-160)" >> "$log"
  done < "$f"
  echo "$n statements, $bad failed" >&2
  return $([ $bad = 0 ] && echo 0 || echo 1)
}
dumps() {  # <binary> <data dir> <out dir>
  mkdir -p "$3"
  for t in $DUMP_TABLES; do
    chq "$1" "$2" --query "SELECT * FROM hist.$t ORDER BY id FORMAT RowBinary" > "$3/$t.rowbinary" 2> "$3/$t.err"
    echo "$t rc=$? $(sha256sum < "$3/$t.rowbinary" | cut -c1-64)" >> "$3/dumps.txt"
  done
}
case ${1:-} in
generate)
  [ $# = 4 ] || usage
  GEN=$(readlink -f "$2"); OUT=$3; B=$(readlink -f "$4")
  [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ] && { echo "ERROR: $OUT is not empty" >&2; exit 2; }
  mkdir -p "$OUT/data"; OUT=$(cd "$OUT" && pwd)
  ident "$B" > "$OUT/writer_identity.tsv"
  run_statements "$B" "$OUT/data" "$GEN/create.sql" "$OUT/create_log.tsv"; rc=$?
  (cd "$OUT/data" && find . -type f ! -path './tmp/*' ! -name '*.log' ! -name 'status' | LC_ALL=C sort | xargs sha256sum) > "$OUT/manifest.sha256"
  sha256sum "$OUT/manifest.sha256" | cut -d' ' -f1 > "$OUT/dataset.sha256"
  cp "$GEN/oracle.json" "$GEN/create.sql" "$OUT/"
  sha256sum "$OUT/oracle.json" "$OUT/create.sql" > "$OUT/inputs.sha256"
  dumps "$B" "$OUT/data" "$OUT/dumps.writer"
  echo "frozen dataset $(cat "$OUT/dataset.sha256") ($(wc -l < "$OUT/manifest.sha256") files), writer $(cut -f2 "$OUT/writer_identity.tsv" | cut -c1-16)" >&2
  exit $rc
  ;;
verify)
  [ $# = 7 ] || usage
  DATA=$(readlink -f "$2"); GEN=$(readlink -f "$3"); B=$(readlink -f "$4"); L=$5; PH=$6; OUT=$7
  mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); W=$OUT/work.$L
  rm -rf "$W"; mkdir -p "$W"; cp -a "$DATA" "$W/data"
  ident "$B" > "$OUT/$L.identity.tsv"
  chq "$B" "$W/data" --multiquery --ignore-error --queries-file "$GEN/verify_$PH.sql" > "$OUT/$L.results.tsv" 2> "$OUT/$L.errors.log"
  dumps "$B" "$W/data" "$OUT/$L.dumps"
  python3 "$HERE/hist_check.py" "$GEN/oracle.json" "$PH" "$OUT/$L.results.tsv" "$OUT/$L.dumps" "$L" > "$OUT/$L.check.txt"; rc=$?
  rm -rf "$W"
  tail -3 "$OUT/$L.check.txt" >&2
  exit $rc
  ;;
rewrite)
  [ $# = 5 ] || usage
  DATA=$(readlink -f "$2"); GEN=$(readlink -f "$3"); B=$(readlink -f "$4"); OUT=$5
  [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ] && { echo "ERROR: $OUT is not empty" >&2; exit 2; }
  mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); cp -a "$DATA" "$OUT/data"
  ident "$B" > "$OUT/rewriter_identity.tsv"
  run_statements "$B" "$OUT/data" "$GEN/rewrite.sql" "$OUT/rewrite_log.tsv"; exit $?
  ;;
*) usage;;
esac
