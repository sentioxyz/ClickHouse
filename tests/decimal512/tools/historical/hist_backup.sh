#!/usr/bin/env bash
# hist_backup.sh - BACKUP by one build, RESTORE by another, then the oracle check of the restored historical dataset.
#
# PRODUCTION BOUNDARY: runs `clickhouse local` only, on copies under <out-dir>, stdin=/dev/null; backups are zip files
# under <out-dir> (backups.allowed_path points there). Nothing connects anywhere; nothing outside <out-dir> changes.
#
#   hist_backup.sh <data-dir> <gen-dir> <phase> <out-dir> <writer-binary> <reader-binary> <label>
#       copies <data-dir>, runs BACKUP DATABASE hist with <writer-binary>, RESTOREs it into an empty directory with
#       <reader-binary>, then `hist_run.sh verify` of the restored data with <reader-binary> for <phase>
#       (initial | after_rewrite). The frozen <data-dir> is never modified.
# Exit: the verify exit (0 all checks pass), 1 when BACKUP or RESTORE fails, 2 usage.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# = 7 ] || { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
DATA=$(readlink -f "$1"); GEN=$(readlink -f "$2"); PH=$3; OUT=$4; W=$(readlink -f "$5"); R=$(readlink -f "$6"); L=$7
[ -e "$OUT/$L" ] && { echo "ERROR: $OUT/$L exists" >&2; exit 2; }
mkdir -p "$OUT/$L/backups" && D=$(cd "$OUT/$L" && pwd) && [ -n "$D" ] || exit 2
ident() { printf '%s\t%s\t%s\t%s\n' "$1" "$(readlink -f "$2")" "$(sha256sum "$2" | cut -d' ' -f1)" "$(readelf -n "$2" 2>/dev/null | awk '/Build ID/{print $3}')"; }
{ ident writer "$W"; ident reader "$R"; } > "$D/identities.tsv"
cp -a "$DATA" "$D/src"
(cd "$D" && timeout 600 "$W" local --path "$D/src" --query "BACKUP DATABASE hist TO File('$D/backups/hist.zip')" -- --backups.allowed_path="$D/backups" < /dev/null) > "$D/backup.out" 2> "$D/backup.err"
rc=$?; echo "backup rc=$rc $(cat "$D/backup.out" | tr '\t\n' '  ')" | tee "$D/steps.txt" >&2
[ $rc = 0 ] || exit 1
rm -rf "${D:?}/src"
mkdir -p "$D/restored"
(cd "$D" && timeout 600 "$R" local --path "$D/restored" --query "RESTORE DATABASE hist FROM File('$D/backups/hist.zip')" -- --backups.allowed_path="$D/backups" < /dev/null) > "$D/restore.out" 2> "$D/restore.err"
rc=$?; echo "restore rc=$rc $(cat "$D/restore.out" | tr '\t\n' '  ')" | tee -a "$D/steps.txt" >&2
[ $rc = 0 ] || exit 1
sha256sum "$D/backups/hist.zip" >> "$D/steps.txt"
bash "$HERE/hist_run.sh" verify "$D/restored" "$GEN" "$R" "$L" "$PH" "$D/verify"; rc=$?
rm -rf "${D:?}/restored"
echo "verify rc=$rc" | tee -a "$D/steps.txt" >&2
exit $rc
