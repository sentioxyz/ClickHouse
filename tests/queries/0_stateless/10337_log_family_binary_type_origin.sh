#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database
# The stored binary type origin of Log, TinyLog and StripeLog tables. An original CREATE stamps the canonical origin, which the registration
# cannot change. A table that RESTORE creates has no origin, since the backup of its files does not tell it, and the registration states
# sentio_legacy_v0 once; TRUNCATE and RENAME keep the origin, and a table without a UUID cannot be registered. A backup is appended to an
# existing table with an origin only if none of its columns can hold binary type headers, and the refusal comes before anything is
# appended. The values that this server wrote read back under every origin.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
REGISTER="MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'"
USER_NAME="u_${CLICKHOUSE_TEST_UNIQUE_NAME}"
ORDINARY_DATABASE="${CLICKHOUSE_DATABASE}_ordinary"

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

for engine in Log TinyLog StripeLog; do
    t="t_${engine}"
    backup="Disk('backups', '${CLICKHOUSE_TEST_UNIQUE_NAME}_${engine}')"
    echo "--- ${engine}"

    $CLICKHOUSE_CLIENT --query "CREATE TABLE ${t} (id Int32, s AggregateFunction(groupArray, Dynamic)) ENGINE = ${engine}"
    $CLICKHOUSE_CLIENT --query "CREATE TABLE ${t}_plain (id Int32, v String) ENGINE = ${engine}"
    $CLICKHOUSE_CLIENT --query "INSERT INTO ${t} SELECT 1, groupArrayState(toDecimal512('1.5', 2)::Dynamic)"
    $CLICKHOUSE_CLIENT --query "INSERT INTO ${t}_plain VALUES (1, 'a')"
    reject 'stamped by CREATE' BAD_ARGUMENTS 'has the stored binary type origin official_canonical' \
        $CLICKHOUSE_CLIENT --query "ALTER TABLE ${t} ${REGISTER}"
    $CLICKHOUSE_CLIENT --query "SELECT id, finalizeAggregation(s) FROM ${t}"

    $CLICKHOUSE_CLIENT --query "BACKUP TABLE ${t}, TABLE ${t}_plain TO ${backup}" > /dev/null

    # Appended to the existing tables: refused for the column that can hold binary type headers, done for the other table.
    reject 'restored into the stamped table' INCORRECT_DATA 'can have binary type headers' \
        $CLICKHOUSE_CLIENT --query "RESTORE TABLE ${t} FROM ${backup} SETTINGS allow_non_empty_tables = true"
    $CLICKHOUSE_CLIENT --query "SELECT count() FROM ${t}"
    $CLICKHOUSE_CLIENT --query "RESTORE TABLE ${t}_plain FROM ${backup} SETTINGS allow_non_empty_tables = true" > /dev/null
    $CLICKHOUSE_CLIENT --query "SELECT count() FROM ${t}_plain"

    # A table that RESTORE creates has no origin: the registration states it once, alone.
    $CLICKHOUSE_CLIENT --query "RESTORE TABLE ${t} AS ${t}_restored FROM ${backup}" > /dev/null
    $CLICKHOUSE_CLIENT --query "SELECT id, finalizeAggregation(s) FROM ${t}_restored"
    reject 'combined with another command' BAD_ARGUMENTS 'cannot be combined with other commands' \
        $CLICKHOUSE_CLIENT --query "ALTER TABLE ${t}_restored ${REGISTER}, MODIFY COMMENT 'c'"
    $CLICKHOUSE_CLIENT --query "ALTER TABLE ${t}_restored ${REGISTER}"
    reject 'registered twice' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' \
        $CLICKHOUSE_CLIENT --query "ALTER TABLE ${t}_restored ${REGISTER}"
    $CLICKHOUSE_CLIENT --query "SELECT id, finalizeAggregation(s) FROM ${t}_restored"

    # TRUNCATE and RENAME keep the origin.
    $CLICKHOUSE_CLIENT --query "TRUNCATE TABLE ${t}_restored"
    $CLICKHOUSE_CLIENT --query "RENAME TABLE ${t}_restored TO ${t}_renamed"
    reject 'kept over TRUNCATE and RENAME' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' \
        $CLICKHOUSE_CLIENT --query "ALTER TABLE ${t}_renamed ${REGISTER}"

    $CLICKHOUSE_CLIENT --query "DROP TABLE ${t}; DROP TABLE ${t}_plain; DROP TABLE ${t}_renamed"
done

echo '--- the grant'
$CLICKHOUSE_CLIENT --query "DROP USER IF EXISTS ${USER_NAME}; CREATE USER ${USER_NAME} IDENTIFIED WITH no_password; GRANT ALTER, SELECT ON ${CLICKHOUSE_DATABASE}.* TO ${USER_NAME}"
$CLICKHOUSE_CLIENT --query "RESTORE TABLE t_Log AS t_grant FROM Disk('backups', '${CLICKHOUSE_TEST_UNIQUE_NAME}_Log')" > /dev/null
reject 'without the grant' ACCESS_DENIED 'SENTIO REGISTER BINARY TYPE ORIGIN' \
    $CLICKHOUSE_CLIENT --user "${USER_NAME}" --query "ALTER TABLE ${CLICKHOUSE_DATABASE}.t_grant ${REGISTER}"
$CLICKHOUSE_CLIENT --query "GRANT SENTIO REGISTER BINARY TYPE ORIGIN ON ${CLICKHOUSE_DATABASE}.t_grant TO ${USER_NAME}"
$CLICKHOUSE_CLIENT --user "${USER_NAME}" --query "ALTER TABLE ${CLICKHOUSE_DATABASE}.t_grant ${REGISTER}"
reject 'with the grant, twice' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' \
    $CLICKHOUSE_CLIENT --user "${USER_NAME}" --query "ALTER TABLE ${CLICKHOUSE_DATABASE}.t_grant ${REGISTER}"
$CLICKHOUSE_CLIENT --query "DROP TABLE t_grant; DROP USER ${USER_NAME}"

echo '--- a table without a UUID'
$CLICKHOUSE_CLIENT --allow_deprecated_database_ordinary 1 --query "CREATE DATABASE ${ORDINARY_DATABASE} ENGINE = Ordinary"
$CLICKHOUSE_CLIENT --query "CREATE TABLE ${ORDINARY_DATABASE}.t (id Int32) ENGINE = Log"
reject 'no UUID' BAD_ARGUMENTS 'has no UUID' $CLICKHOUSE_CLIENT --query "ALTER TABLE ${ORDINARY_DATABASE}.t ${REGISTER}"
$CLICKHOUSE_CLIENT --query "DROP DATABASE ${ORDINARY_DATABASE}"

rm -f "${ERR:?}"
