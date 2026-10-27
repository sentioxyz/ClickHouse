#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database, no-fasttest
# Set and Join tables with data that an older server wrote: a file whose aggregation states hold a Decimal512 value with the legacy
# binary type index 0x39 (made here with the bridge writer mode, which writes the bytes of the Decimal512 fork). Without a stored binary
# type origin such a table loads blocked: it refuses every use of its values with the error of the load, its metadata stays readable,
# and the registration of its origin reads the files again, in order, and unblocks it. A registration that fails on a corrupt file
# changes nothing, and a corrupt file without legacy indexes fails the load as before.
# no-fasttest: the files of the tables are changed on the disk of the server between DETACH and ATTACH.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
LEGACY_JOIN="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_join.bin"
LEGACY_SET="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_set.bin"
REGISTER="MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'"
BLOCKED='was loaded without its data'
RECORD='sentio_binary_type_origin.txt'

# Keep expected server logs in their original form, while query errors stay on stderr.
SERVER_LOG_SEQUENCE=0
expected_server_logs() {
    local logger="$1" table="$2" count="$3"
    shift 3
    SERVER_LOG_SEQUENCE=$((SERVER_LOG_SEQUENCE + 1))
    local log="${CLICKHOUSE_TMP:?}/${CLICKHOUSE_TEST_UNIQUE_NAME:?}.expected_server_${SERVER_LOG_SEQUENCE}.log"
    if ! (set -o noclobber; : > "$log"); then
        echo 'UNEXPECTED existing expected-server log'; exit 1
    fi
    "$@" --server_logs_file "$log"
    local query_rc=$?
    python3 "$CUR_DIR/../sentio_expected_server_logs.py" --log "$log" --database "$CLICKHOUSE_DATABASE" --table "$table" --logger "$logger" --count "$count"
    local log_rc=$?
    if [[ "$query_rc" -ne 0 || "$log_rc" -ne 0 ]]; then
        echo "UNEXPECTED expected-server-log query_rc=$query_rc validation_rc=$log_rc"
        exit 1
    fi
}

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

data_path() {
    # These tables use the default disk: no CREATE statement sets a custom disk.
    # StorageSet/StorageJoin expose their disk-relative path in system.tables.
    local path disk_path
    path=$($CLICKHOUSE_CLIENT --format TSVRaw --query "SELECT data_paths[1] FROM system.tables WHERE database = currentDatabase() AND name = '$1' AND length(data_paths) = 1") || return 1
    if [[ -z "$path" || "$path" == *$'\n'* || "$path" != */ || "/$path" == */../* || "/$path" == */./* ]]; then
        echo 'Expected exactly one non-empty table data path without dot components' >&2
        return 1
    fi
    if [[ "$path" == /* ]]; then
        printf '%s\n' "$path"
        return
    fi
    disk_path=$($CLICKHOUSE_CLIENT --format TSVRaw --query "SELECT path FROM system.disks WHERE name = 'default'") || return 1
    if [[ "$disk_path" != /* || "$disk_path" != */ || "$disk_path" == *$'\n'* || "$disk_path" == */../* || "$disk_path" == */./* ]]; then
        echo 'Expected exactly one absolute default disk path without dot components' >&2
        return 1
    fi
    printf '%s/%s\n' "${disk_path%/}" "$path"
}

# Flips one byte inside the compressed data of a file (after its checksum and header), so that its checksum no longer matches.
corrupt() {
    printf '\xff' | dd of="$1" bs=1 seek=30 count=1 conv=notrunc status=none
}

# Files in the format of the data of Set and Join: compressed Native blocks of the columns of the table.
$CLICKHOUSE_LOCAL --output_format_binary_type_writer_mode sentio_legacy_bridge --query "
    CREATE TABLE src (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Memory;
    INSERT INTO src SELECT 1, groupArrayState(toDecimal512('1.5', 2)::Dynamic);
    SELECT * FROM src FORMAT Native" | ${CLICKHOUSE_COMPRESSOR} > "$LEGACY_JOIN"
$CLICKHOUSE_LOCAL --output_format_binary_type_writer_mode sentio_legacy_bridge --query "
    CREATE TABLE src (s AggregateFunction(groupArray, Dynamic)) ENGINE = Memory;
    INSERT INTO src SELECT groupArrayState(toDecimal512('1.5', 2)::Dynamic);
    SELECT * FROM src FORMAT Native" | ${CLICKHOUSE_COMPRESSOR} > "$LEGACY_SET"

echo '--- Join: blocked'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Join(ANY, LEFT, k)"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_join SELECT 2, groupArrayState(toDecimal512('2.5', 2)::Dynamic)"
JOIN_PATH=$(data_path t_join) || exit 1
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_join"
# The table of an older server: no record, and its second file written by the fork.
rm "${JOIN_PATH:?}${RECORD}"
cp "$LEGACY_JOIN" "${JOIN_PATH:?}2.bin"
expected_server_logs StorageSetOrJoinBase t_join 1 $CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join"
reject 'joinGet' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "SELECT finalizeAggregation(joinGet(t_join, 's', toUInt64(2)))"
reject 'the error of the load' INCORRECT_DATA 'is ambiguous without a declared origin' $CLICKHOUSE_CLIENT --query "SELECT k FROM t_join"
reject 'JOIN' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "SELECT * FROM (SELECT toUInt64(2) AS k) AS l ANY LEFT JOIN t_join USING (k)"
reject 'INSERT' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "INSERT INTO t_join SELECT 3, groupArrayState(toDecimal512('3.5', 2)::Dynamic)"
reject 'mutation' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join DELETE WHERE k = 2 SETTINGS mutations_sync = 1"
expected_server_logs StorageSystemTables t_join 2 $CLICKHOUSE_CLIENT --query "SELECT total_rows IS NULL, total_bytes IS NULL FROM system.tables WHERE database = currentDatabase() AND name = 't_join'"
$CLICKHOUSE_CLIENT --query "SELECT count() FROM system.columns WHERE database = currentDatabase() AND table = 't_join'"

echo '--- Join: registered'
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_join ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join ORDER BY k"
$CLICKHOUSE_CLIENT --query "SELECT finalizeAggregation(joinGet(t_join, 's', toUInt64(1)))"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_join SELECT 3, groupArrayState(toDecimal512('3.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_join"
$CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join ORDER BY k"
reject 'registered twice' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join ${REGISTER}"

echo '--- Join: a registration that fails on a corrupt file changes nothing'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join_partial (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Join(ANY, LEFT, k)"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_join_partial SELECT 2, groupArrayState(toDecimal512('2.5', 2)::Dynamic)"
PARTIAL_PATH=$(data_path t_join_partial) || exit 1
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_join_partial"
rm "${PARTIAL_PATH:?}${RECORD}"
cp "$LEGACY_JOIN" "${PARTIAL_PATH:?}2.bin"
cp "${PARTIAL_PATH:?}1.bin" "${PARTIAL_PATH:?}3.bin"
corrupt "${PARTIAL_PATH:?}3.bin"
expected_server_logs StorageSetOrJoinBase t_join_partial 1 $CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join_partial"
reject 'registration over a corrupt file' CHECKSUM_DOESNT_MATCH '' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join_partial ${REGISTER}"
reject 'still blocked' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "SELECT k FROM t_join_partial"
ls "${PARTIAL_PATH:?}" | grep -v -x 'tmp' | sort
$CLICKHOUSE_CLIENT --query "TRUNCATE TABLE t_join_partial"
$CLICKHOUSE_CLIENT --query "SELECT count() FROM t_join_partial"

echo '--- Join: a corrupt file without legacy indexes fails the load'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join_corrupt (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Join(ANY, LEFT, k)"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_join_corrupt SELECT 2, groupArrayState(toDecimal512('2.5', 2)::Dynamic)"
CORRUPT_PATH=$(data_path t_join_corrupt) || exit 1
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_join_corrupt"
cp "${CORRUPT_PATH:?}1.bin" "${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_good.bin"
corrupt "${CORRUPT_PATH:?}1.bin"
reject 'ATTACH' CHECKSUM_DOESNT_MATCH '' $CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join_corrupt"
cp "${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_good.bin" "${CORRUPT_PATH:?}1.bin"
$CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join_corrupt"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join_corrupt"

echo '--- Set: blocked, then registered'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_set (s AggregateFunction(groupArray, Dynamic)) ENGINE = Set"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_set SELECT groupArrayState(toDecimal512('2.5', 2)::Dynamic)"
SET_PATH=$(data_path t_set) || exit 1
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_set"
rm "${SET_PATH:?}${RECORD}"
cp "$LEGACY_SET" "${SET_PATH:?}2.bin"
expected_server_logs StorageSetOrJoinBase t_set 1 $CLICKHOUSE_CLIENT --query "ATTACH TABLE t_set"
reject 'IN' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "SELECT (SELECT groupArrayState(toDecimal512('1.5', 2)::Dynamic)) IN t_set"
reject 'INSERT' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "INSERT INTO t_set SELECT groupArrayState(toDecimal512('3.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_set ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT (SELECT groupArrayState(toDecimal512('1.5', 2)::Dynamic)) IN t_set, (SELECT groupArrayState(toDecimal512('2.5', 2)::Dynamic)) IN t_set, (SELECT groupArrayState(toDecimal512('3.5', 2)::Dynamic)) IN t_set"

echo '--- a table created on this server is canonical'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join_canonical (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Join(ANY, LEFT, k)"
reject 'canonical' BAD_ARGUMENTS 'has the stored binary type origin official_canonical' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join_canonical ${REGISTER}"

$CLICKHOUSE_CLIENT --query "DROP TABLE t_join; DROP TABLE t_join_partial; DROP TABLE t_join_corrupt; DROP TABLE t_set; DROP TABLE t_join_canonical"
rm -f "${ERR:?}" "${LEGACY_JOIN:?}" "${LEGACY_SET:?}" "${CLICKHOUSE_TMP:?}/${CLICKHOUSE_TEST_UNIQUE_NAME:?}_good.bin"
