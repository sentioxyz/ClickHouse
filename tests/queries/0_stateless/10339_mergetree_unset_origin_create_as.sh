#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database
# A MergeTree table without a stored binary type origin (the one that RESTORE creates) gives none to the tables that CREATE ... AS and
# CLONE AS make from it: they are not stamped canonical, their origin can be registered, and parts move between them. A table made from a
# canonical source is canonical, so parts do not move between it and a table without an origin. A registered table refuses a second
# registration. The values read back under every origin.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
REGISTER="MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'"
BACKUP="Disk('backups', '${CLICKHOUSE_TEST_UNIQUE_NAME}')"

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

$CLICKHOUSE_CLIENT --query "CREATE TABLE src (p Int32, s AggregateFunction(groupArray, Dynamic)) ENGINE = MergeTree PARTITION BY p ORDER BY tuple()"
$CLICKHOUSE_CLIENT --query "INSERT INTO src SELECT 1, groupArrayState(toDecimal512('1.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "BACKUP TABLE src TO ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "RESTORE TABLE src AS src_unset FROM ${BACKUP}" > /dev/null

echo '--- made from a table without an origin'
$CLICKHOUSE_CLIENT --query "CREATE TABLE dst_as AS src_unset"
$CLICKHOUSE_CLIENT --query "CREATE TABLE dst_clone CLONE AS src_unset"
$CLICKHOUSE_CLIENT --query "SELECT p, finalizeAggregation(s) FROM dst_clone"
# Both have no origin, as their source: parts move between them.
$CLICKHOUSE_CLIENT --query "ALTER TABLE dst_as ATTACH PARTITION 1 FROM src_unset"
$CLICKHOUSE_CLIENT --query "SELECT p, finalizeAggregation(s) FROM dst_as"
$CLICKHOUSE_CLIENT --query "ALTER TABLE dst_as ${REGISTER}"
reject 'registered twice' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE dst_as ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT p, finalizeAggregation(s) FROM dst_as"
$CLICKHOUSE_CLIENT --query "ALTER TABLE dst_clone ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT p, finalizeAggregation(s) FROM dst_clone"

echo '--- made from a canonical table'
$CLICKHOUSE_CLIENT --query "CREATE TABLE dst_canonical AS src"
reject 'canonical' BAD_ARGUMENTS 'has the stored binary type origin official_canonical' \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE dst_canonical ${REGISTER}"
reject 'parts of a table without an origin' BAD_ARGUMENTS 'Tables have different stored binary type origins' \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE dst_canonical ATTACH PARTITION 1 FROM src_unset"
$CLICKHOUSE_CLIENT --query "ALTER TABLE dst_canonical ATTACH PARTITION 1 FROM src"
$CLICKHOUSE_CLIENT --query "SELECT p, finalizeAggregation(s) FROM dst_canonical"

$CLICKHOUSE_CLIENT --query "DROP TABLE src; DROP TABLE src_unset; DROP TABLE dst_as; DROP TABLE dst_clone; DROP TABLE dst_canonical"
rm -f "${ERR:?}"
