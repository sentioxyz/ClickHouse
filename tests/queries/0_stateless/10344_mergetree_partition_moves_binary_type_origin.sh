#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database
# REPLACE PARTITION ... FROM and MOVE PARTITION ... TO TABLE, as ATTACH PARTITION ... FROM (10339), need both MergeTree tables to have the
# same determined stored binary type origin: a part without a record of its own is read with the origin of its table, so it keeps its
# meaning only between tables of the same origin. A refused command changes neither table. Parts move between two canonical tables and
# between two tables registered as `sentio_legacy_v0` (registered before the command), and a table registered after a move keeps the
# moved parts readable. The values read back the same everywhere.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
REGISTER="MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'"
BACKUP="Disk('backups', '${CLICKHOUSE_TEST_UNIQUE_NAME}')"
DIFFERENT='Tables have different stored binary type origins'

# A refusal passes only with a non-zero exit code, the expected error code and the expected message; any other outcome prints the exit
# code, stdout and stderr. The arguments after the message are the command.
reject() {
    local label="$1" code="$2" message="$3"
    shift 3
    local out rc err
    out=$("$@" 2> "$ERR"); rc=$?
    err=$(cat "$ERR")
    if [ "$rc" -ne 0 ] && [[ "$err" == *"$code"* ]] && [[ "$err" == *"$message"* ]]; then
        echo "$label: refused"
    else
        echo "$label: UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $err"
    fi
}

contents() {
    for table in "$@"; do
        $CLICKHOUSE_CLIENT --query "SELECT '$table', p, finalizeAggregation(s) FROM $table ORDER BY p"
    done
}

# canon: canonical (created here), partitions 1 and 2. legacy and legacy2: restored from its backup without an origin, then registered.
$CLICKHOUSE_CLIENT --query "CREATE TABLE canon (p Int32, s AggregateFunction(groupArray, Dynamic)) ENGINE = MergeTree PARTITION BY p ORDER BY tuple()"
$CLICKHOUSE_CLIENT --query "INSERT INTO canon SELECT 1, groupArrayState(toDecimal512('1.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "INSERT INTO canon SELECT 2, groupArrayState(toDecimal512('2.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "BACKUP TABLE canon TO ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "RESTORE TABLE canon AS legacy FROM ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "RESTORE TABLE canon AS legacy2 FROM ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "ALTER TABLE legacy ${REGISTER}"
$CLICKHOUSE_CLIENT --query "ALTER TABLE legacy2 ${REGISTER}"
$CLICKHOUSE_CLIENT --query "CREATE TABLE canon_empty AS canon"

echo '--- different origins'
reject 'REPLACE PARTITION FROM a legacy table into an empty canonical one' BAD_ARGUMENTS "$DIFFERENT" \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE canon_empty REPLACE PARTITION 1 FROM legacy"
reject 'REPLACE PARTITION FROM a legacy table into a canonical one with data' BAD_ARGUMENTS "$DIFFERENT" \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE canon REPLACE PARTITION 1 FROM legacy"
reject 'REPLACE PARTITION FROM a canonical table into a legacy one' BAD_ARGUMENTS "$DIFFERENT" \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE legacy REPLACE PARTITION 2 FROM canon"
reject 'MOVE PARTITION TO TABLE from a legacy table to a canonical one' BAD_ARGUMENTS "$DIFFERENT" \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE legacy MOVE PARTITION 2 TO TABLE canon_empty"
reject 'MOVE PARTITION TO TABLE from a canonical table to a legacy one' BAD_ARGUMENTS "$DIFFERENT" \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE canon MOVE PARTITION 1 TO TABLE legacy2"
echo '--- unchanged'
contents canon canon_empty legacy legacy2

echo '--- the same origin'
$CLICKHOUSE_CLIENT --query "ALTER TABLE canon_empty REPLACE PARTITION 1 FROM canon"
$CLICKHOUSE_CLIENT --query "ALTER TABLE canon MOVE PARTITION 2 TO TABLE canon_empty"
$CLICKHOUSE_CLIENT --query "ALTER TABLE legacy2 REPLACE PARTITION 1 FROM legacy"
$CLICKHOUSE_CLIENT --query "ALTER TABLE legacy MOVE PARTITION 2 TO TABLE legacy2"
contents canon canon_empty legacy legacy2

echo '--- registered after a move'
$CLICKHOUSE_CLIENT --query "RESTORE TABLE canon AS unset_a FROM ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "RESTORE TABLE canon AS unset_b FROM ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "ALTER TABLE unset_a MOVE PARTITION 1 TO TABLE unset_b"
$CLICKHOUSE_CLIENT --query "ALTER TABLE unset_b ${REGISTER}"
contents unset_a unset_b
reject 'a registered and an unset table' BAD_ARGUMENTS "$DIFFERENT" \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE unset_b MOVE PARTITION 2 TO TABLE unset_a"
contents unset_a unset_b

$CLICKHOUSE_CLIENT --query "DROP TABLE canon; DROP TABLE canon_empty; DROP TABLE legacy; DROP TABLE legacy2; DROP TABLE unset_a; DROP TABLE unset_b"
rm -f "${ERR:?}"
