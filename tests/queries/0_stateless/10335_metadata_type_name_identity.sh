#!/usr/bin/env bash
# Tags: no-replicated-database
# A table definition keeps the type of each column by its name, and the table is loaded with the types created from these names. A decimal
# that a binary type decoder constructs with a precision of another decimal class (a native Decimal512 with a precision of at most 76) or
# with a scale greater than its precision has the name of another type or of no type, and so has any type that holds one. Such a type is
# refused before a definition is stored: CREATE TABLE AS SELECT, CREATE TABLE AS a table function, a storage that infers its columns,
# CREATE VIEW, a refreshable materialized view, ALTER MODIFY QUERY. A declared type converts the values to it. A temporary table (no
# stored definition) and the formats with the types in binary format keep the type exactly; written by its name it is refused.
#
# The payloads are RowBinaryWithNamesAndTypes with the types in binary format, derived from the encoding (DataTypesBinaryEncoding.h/.cpp and
# SentioWideTypeEncoding.cpp): `01 01 78` is one column named x; `2c 1e` and the 30 bytes "SentioWide_v1_Decimal512(50,2)" are a native
# Decimal512(50, 2); `1a 05 02` is a native Decimal64(5, 2); containers and states wrap these. A value is the little-endian integer
# value * 10^scale in sizeof(type) bytes: 123.45 is 0x3039 in 64 bytes for Decimal512.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
FILE="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.bin"
USER_FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_p50.bin"
USER_FILE_CONTROL="${CLICKHOUSE_TEST_UNIQUE_NAME}_p80.bin"

# D_*: the name of the type of x does not identify it. C_*: controls whose names do.
# Decimal512(50, 2), one row 123.45
D_P50='0101782c1e53656e74696f576964655f76315f446563696d616c3531322835302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal512(80, 90): a scale greater than the precision, no rows
D_S90='0101782c1f53656e74696f576964655f76315f446563696d616c3531322838302c393029'
# Array(Decimal512(50, 2)), no rows
D_ARRAY='0101781e2c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'
# Tuple(a Decimal512(50, 2), b String), no rows
D_TUPLE='010178200201612c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229016215'
# Nullable(Decimal512(50, 2)), no rows
D_NULLABLE='010178232c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'
# Map(String, Decimal512(50, 2)), no rows
D_MAP='01017827152c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'
# Variant(Decimal512(50, 2), String), no rows
D_VARIANT='0101782a022c1e53656e74696f576964655f76315f446563696d616c3531322835302c322915'
# AggregateFunction(sum, Decimal512(50, 2)), no rows
D_STATE='01017825000373756d00012c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'
# Decimal64(5, 2), one row 123.45: an upstream class with a precision of a narrower class
D_D64_P5='0101781a05023930000000000000'
# Decimal32(5, 7): an upstream class with a scale greater than the precision, no rows
D_D32_S7='010178190507'
# Decimal512(80, 2), one row 123.45
C_P80='0101782c1e53656e74696f576964655f76315f446563696d616c3531322838302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal256(50, 2), one row 123.45
C_D256='0101781c32023930000000000000000000000000000000000000000000000000000000000000'
# Int32, one row 7
C_INT32='0101780907000000'

DECODE='--input_format_binary_decode_types_in_binary_format 1'
STORED='this name does not identify the type'
WRITTEN='cannot be written by its name: the name does not identify it'

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

source_of() {
    echo "format(RowBinaryWithNamesAndTypes, x'$1')"
}

# A result column is sent to clickhouse-client by the name of its type, so a type that its name does not identify is shown through
# clickhouse-local, or converted to a String on the server.
echo '--- decoded types: the name of a native Decimal512(50, 2) is the name of Decimal256(50, 2)'
for data in "$D_P50" "$C_P80" "$C_D256" "$D_D64_P5" "$C_INT32"; do
    $CLICKHOUSE_LOCAL $DECODE --query "SELECT toTypeName(x), byteSize(x), x FROM $(source_of "$data")"
done

echo '--- CREATE TABLE AS SELECT'
for name in D_P50 D_S90 D_ARRAY D_TUPLE D_NULLABLE D_MAP D_VARIANT D_STATE D_D64_P5 D_D32_S7; do
    reject "$name" DATA_TYPE_CANNOT_BE_USED_IN_TABLES "$STORED" \
        $CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b ENGINE = MergeTree ORDER BY tuple() AS SELECT * FROM $(source_of "${!name}")"
done

echo '--- CREATE VIEW, a refreshable materialized view with an inner table'
reject VIEW DATA_TYPE_CANNOT_BE_USED_IN_TABLES "$STORED" \
    $CLICKHOUSE_CLIENT $DECODE --query "CREATE VIEW v_e7b AS SELECT * FROM $(source_of "$D_P50")"
reject 'MATERIALIZED VIEW' DATA_TYPE_CANNOT_BE_USED_IN_TABLES "$STORED" \
    $CLICKHOUSE_CLIENT $DECODE --query "CREATE MATERIALIZED VIEW mv_e7b REFRESH EVERY 1 YEAR ENGINE = MergeTree ORDER BY tuple() EMPTY AS SELECT * FROM $(source_of "$D_P50")"

echo '--- a table function and a storage that infer their columns'
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('$USER_FILE', 'RawBLOB') SELECT unhex('$D_P50') SETTINGS engine_file_truncate_on_insert = 1"
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('$USER_FILE_CONTROL', 'RawBLOB') SELECT unhex('$C_P80') SETTINGS engine_file_truncate_on_insert = 1"
reject 'AS file()' DATA_TYPE_CANNOT_BE_USED_IN_TABLES "$STORED" \
    $CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b AS file('$USER_FILE', 'RowBinaryWithNamesAndTypes')"
reject 'ENGINE = File' DATA_TYPE_CANNOT_BE_USED_IN_TABLES "$STORED" \
    $CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b ENGINE = File(RowBinaryWithNamesAndTypes, '$USER_FILE')"
# Nothing was created, and the refused storage left its file as it was.
$CLICKHOUSE_CLIENT --query "SELECT count() FROM system.tables WHERE database = currentDatabase() AND (name IN ('t_e7b', 'v_e7b', 'mv_e7b') OR name LIKE '.inner%')"
$CLICKHOUSE_CLIENT --query "SELECT hex(raw_blob) = upper('$D_P50') FROM file('$USER_FILE', 'RawBLOB')"
$CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b_file_control ENGINE = File(RowBinaryWithNamesAndTypes, '$USER_FILE_CONTROL')"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), x FROM t_e7b_file_control"
$CLICKHOUSE_CLIENT --query "SELECT position(create_table_query, 'Decimal(80, 2)') > 0 FROM system.tables WHERE database = currentDatabase() AND name = 't_e7b_file_control'"

echo '--- ALTER MODIFY QUERY of a refreshable materialized view'
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_e7b_target (x Int32) ENGINE = MergeTree ORDER BY tuple()"
$CLICKHOUSE_CLIENT --query "CREATE MATERIALIZED VIEW mv_e7b_to REFRESH EVERY 1 YEAR APPEND TO t_e7b_target EMPTY AS SELECT toInt32(1) AS x"
reject 'MODIFY QUERY' DATA_TYPE_CANNOT_BE_USED_IN_TABLES "$STORED" \
    $CLICKHOUSE_CLIENT $DECODE --query "ALTER TABLE mv_e7b_to MODIFY QUERY SELECT * FROM $(source_of "$D_P50")"
$CLICKHOUSE_CLIENT --query "SELECT position(create_table_query, 'toInt32(1)') > 0 FROM system.tables WHERE database = currentDatabase() AND name = 'mv_e7b_to'"

echo '--- a declared type, kept over DETACH and ATTACH'
$CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b_declared (x Decimal(80, 2)) ENGINE = MergeTree ORDER BY tuple() AS SELECT * FROM $(source_of "$D_P50")"
$CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b_declared_256 (x Decimal(50, 2)) ENGINE = MergeTree ORDER BY tuple() AS SELECT * FROM $(source_of "$D_P50")"
for table in t_e7b_declared t_e7b_declared_256; do
    $CLICKHOUSE_CLIENT --query "DETACH TABLE $table"
    $CLICKHOUSE_CLIENT --query "ATTACH TABLE $table"
    $CLICKHOUSE_CLIENT --query "SELECT toTypeName(x), byteSize(x), x FROM $table"
done

echo '--- controls: types identified by their names, kept over DETACH and ATTACH'
for name in C_P80 C_D256 C_INT32; do
    $CLICKHOUSE_CLIENT $DECODE --query "CREATE TABLE t_e7b_$name ENGINE = MergeTree ORDER BY tuple() AS SELECT * FROM $(source_of "${!name}")"
    $CLICKHOUSE_CLIENT --query "DETACH TABLE t_e7b_$name"
    $CLICKHOUSE_CLIENT --query "ATTACH TABLE t_e7b_$name"
    $CLICKHOUSE_CLIENT --query "SELECT toTypeName(x), byteSize(x), x FROM t_e7b_$name"
done

echo '--- a temporary table stores no definition'
$CLICKHOUSE_CLIENT $DECODE --query "CREATE TEMPORARY TABLE tmp_e7b AS SELECT * FROM $(source_of "$D_P50"); SELECT toTypeName(x), byteSize(x), toString(x) FROM tmp_e7b"

echo '--- the formats with the types in binary format keep the type exactly; by its name it is refused'
$CLICKHOUSE_LOCAL $DECODE --output_format_binary_encode_types_in_binary_format 1 --query "SELECT * FROM $(source_of "$D_P50") FORMAT RowBinaryWithNamesAndTypes" > "$FILE"
$CLICKHOUSE_LOCAL --input-format RawBLOB --query "SELECT hex(raw_blob) = upper('$D_P50') FROM table" < "$FILE"
reject 'RowBinaryWithNamesAndTypes by name' INCORRECT_DATA "$WRITTEN" \
    $CLICKHOUSE_LOCAL $DECODE --query "SELECT * FROM $(source_of "$D_P50") FORMAT RowBinaryWithNamesAndTypes"
$CLICKHOUSE_LOCAL $DECODE --output_format_native_encode_types_in_binary_format 1 --query "SELECT * FROM $(source_of "$D_P50") FORMAT Native" > "$FILE"
$CLICKHOUSE_LOCAL --input-format Native --input_format_native_decode_types_in_binary_format 1 --query "SELECT toTypeName(x), byteSize(x), x FROM table" < "$FILE"
reject 'Native by name' INCORRECT_DATA "$WRITTEN" \
    $CLICKHOUSE_LOCAL $DECODE --query "SELECT * FROM $(source_of "$D_P50") FORMAT Native"
reject 'result sent by name' INCORRECT_DATA "$WRITTEN" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT x FROM $(source_of "$D_P50")"

$CLICKHOUSE_CLIENT --query "DROP TABLE mv_e7b_to"
$CLICKHOUSE_CLIENT --query "DROP TABLE t_e7b_target"
$CLICKHOUSE_CLIENT --query "DROP TABLE t_e7b_file_control"
for table in t_e7b_declared t_e7b_declared_256 t_e7b_C_P80 t_e7b_C_D256 t_e7b_C_INT32; do
    $CLICKHOUSE_CLIENT --query "DROP TABLE $table"
done
rm -f "${CLICKHOUSE_USER_FILES:?}/${USER_FILE:?}" "${CLICKHOUSE_USER_FILES:?}/${USER_FILE_CONTROL:?}" "${ERR:?}" "${FILE:?}"
