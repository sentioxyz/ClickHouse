#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database, no-fasttest
# Set and Join tables without persistence (SETTINGS persistent = 0) still load the numbered files in their directory, which remain from
# when they had persistence or were written by another server. With files of the Decimal512 fork (aggregation states that hold a
# Decimal512 value with the legacy binary type index 0x39, made here with the bridge writer mode) and without a stored binary type origin,
# such a table loads blocked as a persistent one does, and the registration of its origin reads all its files again, in order: it unblocks
# the table, its values are read, and a reload reads them with the registered origin. A registration that fails on a corrupt file changes
# nothing: the names and the bytes of the files, the blocked state and the absence of a record; once the file is repaired, it reads all
# the files. A table that is not blocked keeps the rows inserted into it, which are in no file, through the registration.
# no-fasttest: the files of the tables are changed on the disk of the server between DETACH and ATTACH.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

PREFIX="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
ERR="${PREFIX}.stderr"
REGISTER="MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'"
BLOCKED='was loaded without its data'
RECORD='sentio_binary_type_origin.txt'

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
    $CLICKHOUSE_CLIENT --query "SELECT arrayJoin(data_paths) FROM system.tables WHERE database = currentDatabase() AND name = '$1'"
}

# Flips one byte inside the compressed data of a file (after its checksum and header), so that its checksum no longer matches.
corrupt() {
    printf '\xff' | dd of="$1" bs=1 seek=30 count=1 conv=notrunc status=none
}

# The table of an older server: no record, and the given files as its numbered files 1.bin, 2.bin, ...
make_old() {
    local table="$1"
    shift
    local dir number=1
    dir=$(data_path "$table")
    $CLICKHOUSE_CLIENT --query "DETACH TABLE $table"
    # A scalar-only UInt64/String table may have no automatic origin record. The legacy fixture needs no record either.
    # Keep the original existence premise for every header-bearing blocked/corrupt table.
    if [[ "$table" == t_join_healthy || "$table" == t_set_healthy ]]; then
        rm -f "${dir:?}${RECORD}"
    else
        rm "${dir:?}${RECORD}"
    fi
    for file in "$@"; do
        cp "$file" "${dir:?}${number}.bin"
        number=$((number + 1))
    done
    $CLICKHOUSE_CLIENT --query "ATTACH TABLE $table"
}

# Files in the format of the data of Set and Join: compressed Native blocks of the columns of the table. The legacy ones hold the bytes of
# the Decimal512 fork; the healthy ones have no binary type headers.
legacy_join() {
    $CLICKHOUSE_LOCAL --output_format_binary_type_writer_mode sentio_legacy_bridge --query "
        CREATE TABLE src (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Memory;
        INSERT INTO src SELECT $1, groupArrayState(toDecimal512('$2', 2)::Dynamic);
        SELECT * FROM src FORMAT Native" | ${CLICKHOUSE_COMPRESSOR} > "$3"
}
legacy_set() {
    $CLICKHOUSE_LOCAL --output_format_binary_type_writer_mode sentio_legacy_bridge --query "
        CREATE TABLE src (s AggregateFunction(groupArray, Dynamic)) ENGINE = Memory;
        INSERT INTO src SELECT groupArrayState(toDecimal512('$1', 2)::Dynamic);
        SELECT * FROM src FORMAT Native" | ${CLICKHOUSE_COMPRESSOR} > "$2"
}
legacy_join 1 1.5 "${PREFIX}_join1.bin"
legacy_join 3 3.5 "${PREFIX}_join3.bin"
legacy_set 1.5 "${PREFIX}_set1.bin"
legacy_set 3.5 "${PREFIX}_set3.bin"
$CLICKHOUSE_LOCAL --query "
    CREATE TABLE src (k UInt64, v String) ENGINE = Memory;
    INSERT INTO src VALUES (1, 'file');
    SELECT * FROM src FORMAT Native" | ${CLICKHOUSE_COMPRESSOR} > "${PREFIX}_healthy_join.bin"
$CLICKHOUSE_LOCAL --query "
    CREATE TABLE src (k UInt64) ENGINE = Memory;
    INSERT INTO src VALUES (1);
    SELECT * FROM src FORMAT Native" | ${CLICKHOUSE_COMPRESSOR} > "${PREFIX}_healthy_set.bin"

JOIN_COLUMNS='(k UInt64, s AggregateFunction(groupArray, Dynamic))'
SET_COLUMNS='(s AggregateFunction(groupArray, Dynamic))'
in_set() {
    $CLICKHOUSE_CLIENT --query "SELECT (SELECT groupArrayState(toDecimal512('1.5', 2)::Dynamic)) IN $1, (SELECT groupArrayState(toDecimal512('3.5', 2)::Dynamic)) IN $1, (SELECT groupArrayState(toDecimal512('4.5', 2)::Dynamic)) IN $1"
}

echo '--- Join: blocked'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join $JOIN_COLUMNS ENGINE = Join(ANY, LEFT, k) SETTINGS persistent = 0"
make_old t_join "${PREFIX}_join1.bin" "${PREFIX}_join3.bin"
reject 'SELECT' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "SELECT k FROM t_join"
reject 'INSERT' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "INSERT INTO t_join SELECT 4, groupArrayState(toDecimal512('4.5', 2)::Dynamic)"

echo '--- Join: registered, then a row in no file, then reloaded'
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_join ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join ORDER BY k"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_join SELECT 4, groupArrayState(toDecimal512('4.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join ORDER BY k"
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_join"
$CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join ORDER BY k"
reject 'registered twice' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join ${REGISTER}"

echo '--- Join: a registration that fails on a corrupt file changes nothing'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join_partial $JOIN_COLUMNS ENGINE = Join(ANY, LEFT, k) SETTINGS persistent = 0"
cp "${PREFIX}_join3.bin" "${PREFIX}_join3_corrupt.bin"
corrupt "${PREFIX}_join3_corrupt.bin"
make_old t_join_partial "${PREFIX}_join1.bin" "${PREFIX}_join3_corrupt.bin"
PARTIAL_PATH=$(data_path t_join_partial)
reject 'registration over a corrupt file' CHECKSUM_DOESNT_MATCH '' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join_partial ${REGISTER}"
reject 'still blocked' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "SELECT k FROM t_join_partial"
ls "${PARTIAL_PATH:?}" | grep -v -x 'tmp' | sort
cmp "${PREFIX}_join1.bin" "${PARTIAL_PATH:?}1.bin" && echo '1.bin: the same bytes'
cmp "${PREFIX}_join3_corrupt.bin" "${PARTIAL_PATH:?}2.bin" && echo '2.bin: the same bytes'
cp "${PREFIX}_join3.bin" "${PARTIAL_PATH:?}2.bin"
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_join_partial ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT k, finalizeAggregation(s) FROM t_join_partial ORDER BY k"

echo '--- Join, not blocked: the registration keeps the rows in no file'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_join_healthy (k UInt64, v String) ENGINE = Join(ANY, LEFT, k) SETTINGS persistent = 0"
make_old t_join_healthy "${PREFIX}_healthy_join.bin"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_join_healthy VALUES (2, 'memory')"
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_join_healthy ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT k, v FROM t_join_healthy ORDER BY k"
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_join_healthy"
$CLICKHOUSE_CLIENT --query "ATTACH TABLE t_join_healthy"
$CLICKHOUSE_CLIENT --query "SELECT k, v FROM t_join_healthy ORDER BY k"
reject 'registered twice' BAD_ARGUMENTS 'has the stored binary type origin sentio_legacy_v0' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_join_healthy ${REGISTER}"

echo '--- Set: blocked, registered, then a row in no file, then reloaded'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_set $SET_COLUMNS ENGINE = Set SETTINGS persistent = 0"
make_old t_set "${PREFIX}_set1.bin" "${PREFIX}_set3.bin"
reject 'IN' INCORRECT_DATA "$BLOCKED" in_set t_set
reject 'INSERT' INCORRECT_DATA "$BLOCKED" $CLICKHOUSE_CLIENT --query "INSERT INTO t_set SELECT groupArrayState(toDecimal512('4.5', 2)::Dynamic)"
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_set ${REGISTER}"
in_set t_set
$CLICKHOUSE_CLIENT --query "INSERT INTO t_set SELECT groupArrayState(toDecimal512('4.5', 2)::Dynamic)"
in_set t_set
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_set"
$CLICKHOUSE_CLIENT --query "ATTACH TABLE t_set"
in_set t_set

echo '--- Set: a registration that fails on a corrupt file changes nothing'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_set_partial $SET_COLUMNS ENGINE = Set SETTINGS persistent = 0"
cp "${PREFIX}_set3.bin" "${PREFIX}_set3_corrupt.bin"
corrupt "${PREFIX}_set3_corrupt.bin"
make_old t_set_partial "${PREFIX}_set1.bin" "${PREFIX}_set3_corrupt.bin"
SET_PARTIAL_PATH=$(data_path t_set_partial)
reject 'registration over a corrupt file' CHECKSUM_DOESNT_MATCH '' $CLICKHOUSE_CLIENT --query "ALTER TABLE t_set_partial ${REGISTER}"
reject 'still blocked' INCORRECT_DATA "$BLOCKED" in_set t_set_partial
ls "${SET_PARTIAL_PATH:?}" | grep -v -x 'tmp' | sort
cmp "${PREFIX}_set1.bin" "${SET_PARTIAL_PATH:?}1.bin" && echo '1.bin: the same bytes'
cmp "${PREFIX}_set3_corrupt.bin" "${SET_PARTIAL_PATH:?}2.bin" && echo '2.bin: the same bytes'
cp "${PREFIX}_set3.bin" "${SET_PARTIAL_PATH:?}2.bin"
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_set_partial ${REGISTER}"
in_set t_set_partial

echo '--- Set, not blocked: the registration keeps the rows in no file'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_set_healthy (k UInt64) ENGINE = Set SETTINGS persistent = 0"
make_old t_set_healthy "${PREFIX}_healthy_set.bin"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_set_healthy VALUES (2)"
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_set_healthy ${REGISTER}"
$CLICKHOUSE_CLIENT --query "SELECT 1 IN t_set_healthy, 2 IN t_set_healthy, 3 IN t_set_healthy"
$CLICKHOUSE_CLIENT --query "DETACH TABLE t_set_healthy"
$CLICKHOUSE_CLIENT --query "ATTACH TABLE t_set_healthy"
$CLICKHOUSE_CLIENT --query "SELECT 1 IN t_set_healthy, 2 IN t_set_healthy, 3 IN t_set_healthy"

$CLICKHOUSE_CLIENT --query "DROP TABLE t_join; DROP TABLE t_join_partial; DROP TABLE t_join_healthy; DROP TABLE t_set; DROP TABLE t_set_partial; DROP TABLE t_set_healthy"
rm -f "${ERR:?}" "${PREFIX:?}_join1.bin" "${PREFIX:?}_join3.bin" "${PREFIX:?}_join3_corrupt.bin" "${PREFIX:?}_set1.bin" "${PREFIX:?}_set3.bin" \
    "${PREFIX:?}_set3_corrupt.bin" "${PREFIX:?}_healthy_join.bin" "${PREFIX:?}_healthy_set.bin"
