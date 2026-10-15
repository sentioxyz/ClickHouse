#!/usr/bin/env bash
# RowBinaryWithNamesAndTypes with the types in binary format: the header holds the types themselves, and their names do not tell every
# type apart. A native Decimal512(50, 2) (64-byte values) is named Decimal(50, 2), the name of the 32-byte Decimal256(50, 2). The structure
# inferred from the header keeps the decoded types (also in arrays, in aggregation states and with a scale greater than the precision),
# and a header type that is named like the type of its column but is another type is refused before any value is read: with an explicit
# structure, on INSERT, with the types of the header not checked otherwise (input_format_with_types_use_header = 0), with the columns
# mapped by position (input_format_with_names_use_header = 0), and in each of several files. Types with other names keep their behaviour,
# a skipped column is skipped with its own type, and types created from their names (a header with type names, Decimal256, a Decimal512
# with a precision above 76) are read as before.
#
# The payloads are derived from the encoding by drafts/e7d/gen_e7d_hex.py (the rules of 10335): `01 01 78` is one column named x;
# `2c 1e` and the 30 bytes "SentioWide_v1_Decimal512(50,2)" are a native Decimal512(50, 2); a value is the little-endian integer
# value * 10^scale in sizeof(type) bytes: 123.45 is 0x3039 in 64 bytes for Decimal512 and in 32 bytes for Decimal256.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
DATA="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.bin"
USER_FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_e7d"

# Native Decimal512(50, 2): one row 123.45; the same with 2.50
D_P50='0101782c1e53656e74696f576964655f76315f446563696d616c3531322835302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
D_P50_B='0101782c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229fa000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Native Decimal512(80, 90): a scale greater than the precision, no rows
D_S90='0101782c1f53656e74696f576964655f76315f446563696d616c3531322838302c393029'
# Array(native Decimal512(50, 2)): one row [123.45]
D_ARRAY='0101781e2c1e53656e74696f576964655f76315f446563696d616c3531322835302c32290139300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# AggregateFunction(sum, native Decimal512(50, 2)): one state with the sum 123.45
D_STATE='01017825000373756d00012c1e53656e74696f576964655f76315f446563696d616c3531322835302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# x native Decimal512(50, 2) and y Int32: one row 123.45, 7
D_TWO='02017801792c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229093930000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000007000000'
# Decimal512(80, 2), created from its name too: one row 123.45
C_P80='0101782c1e53656e74696f576964655f76315f446563696d616c3531322838302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal256(50, 2): one row 123.45
C_D256='0101781c32023930000000000000000000000000000000000000000000000000000000000000'
# Int32: one row 7
C_INT32='0101780907000000'
# The header with type names: x 'Decimal(50, 2)' (Decimal256), one row 123.45
N_D256='0101780e446563696d616c2835302c2032293930000000000000000000000000000000000000000000000000000000000000'

DECODE='--input_format_binary_decode_types_in_binary_format 1'
OTHER='is named Decimal(50, 2), as the type of the column, but it is another type'

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

data() {
    echo "x'$1'"
}

# A result column is sent to clickhouse-client by the name of its type, so a type that its name does not identify is shown through
# toTypeName, byteSize and toString.
echo '--- the inferred structure keeps the decoded types'
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), toString(x) FROM format(RowBinaryWithNamesAndTypes, $(data "$D_P50"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x[1]), toString(x[1]) FROM format(RowBinaryWithNamesAndTypes, $(data "$D_ARRAY"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT byteSize(finalizeAggregation(x)), toString(finalizeAggregation(x)) FROM format(RowBinaryWithNamesAndTypes, $(data "$D_STATE"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT count() FROM format(RowBinaryWithNamesAndTypes, $(data "$D_S90"))"
reject 'DESCRIBE of a scale greater than the precision' INCORRECT_DATA 'cannot be written by its name in the result of DESCRIBE' \
    $CLICKHOUSE_CLIENT $DECODE --query "DESCRIBE format(RowBinaryWithNamesAndTypes, $(data "$D_S90"))"
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('${USER_FILE}_1.bin', 'RawBLOB') SELECT unhex('$D_P50') SETTINGS engine_file_truncate_on_insert = 1"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), toString(x) FROM file('${USER_FILE}_1.bin', RowBinaryWithNamesAndTypes)"

echo '--- the same name for another type is refused'
reject 'explicit structure' INCORRECT_DATA "$OTHER" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT toString(x) FROM format(RowBinaryWithNamesAndTypes, 'x Decimal(50, 2)', $(data "$D_P50"))"
reject 'Array' INCORRECT_DATA 'is named Array(Decimal(50, 2)), as the type of the column, but it is another type' \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT toString(x) FROM format(RowBinaryWithNamesAndTypes, 'x Array(Decimal(50, 2))', $(data "$D_ARRAY"))"
reject 'aggregation state' INCORRECT_DATA 'is named AggregateFunction(sum, Decimal(50, 2)), as the type of the column, but it is another type' \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT toString(finalizeAggregation(x)) FROM format(RowBinaryWithNamesAndTypes, 'x AggregateFunction(sum, Decimal(50, 2))', $(data "$D_STATE"))"
reject 'types of the header not checked' INCORRECT_DATA "$OTHER" \
    $CLICKHOUSE_CLIENT $DECODE --input_format_with_types_use_header 0 --query "SELECT toString(x) FROM format(RowBinaryWithNamesAndTypes, 'x Decimal(50, 2)', $(data "$D_P50"))"
reject 'columns mapped by position' INCORRECT_DATA "$OTHER" \
    $CLICKHOUSE_CLIENT $DECODE --input_format_with_names_use_header 0 --query "SELECT toString(y) FROM format(RowBinaryWithNamesAndTypes, 'y Decimal(50, 2)', $(data "$D_P50"))"
$CLICKHOUSE_CLIENT --query "CREATE TABLE t_e7d (x Decimal(50, 2)) ENGINE = Memory"
$CLICKHOUSE_LOCAL --query "SELECT unhex('$D_P50') FORMAT RawBLOB" > "$DATA"
reject 'INSERT' INCORRECT_DATA "$OTHER" \
    $CLICKHOUSE_CLIENT $DECODE --query "INSERT INTO t_e7d FORMAT RowBinaryWithNamesAndTypes" < "$DATA"
$CLICKHOUSE_CLIENT --query "SELECT count() FROM t_e7d"

echo '--- several files: each header is checked against the structure'
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('${USER_FILE}_2.bin', 'RawBLOB') SELECT unhex('$C_D256') SETTINGS engine_file_truncate_on_insert = 1"
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('${USER_FILE}_3.bin', 'RawBLOB') SELECT unhex('$D_P50_B') SETTINGS engine_file_truncate_on_insert = 1"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT right(_file, 5) AS f, byteSize(x), toString(x) FROM file('${USER_FILE}_{1,3}.bin', RowBinaryWithNamesAndTypes) ORDER BY f"
reject 'a file with the same name for another type' INCORRECT_DATA "$OTHER" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT right(_file, 5) AS f, toString(x) FROM file('${USER_FILE}_{1,2}.bin', RowBinaryWithNamesAndTypes) ORDER BY f"

echo '--- types with other names, skipped columns'
$CLICKHOUSE_CLIENT $DECODE --input_format_with_types_use_header 0 --query "SELECT x FROM format(RowBinaryWithNamesAndTypes, 'x UInt32', $(data "$C_INT32"))"
reject 'another name, checked' INCORRECT_DATA "Type of 'x' must be UInt32, not Int32" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT x FROM format(RowBinaryWithNamesAndTypes, 'x UInt32', $(data "$C_INT32"))"
$CLICKHOUSE_CLIENT $DECODE --input_format_skip_unknown_fields 1 --query "SELECT y FROM format(RowBinaryWithNamesAndTypes, 'y Int32', $(data "$D_TWO"))"

echo '--- types created from their names'
$CLICKHOUSE_CLIENT --query "SELECT toTypeName(x), byteSize(x), x FROM format(RowBinaryWithNamesAndTypes, $(data "$N_D256"))"
$CLICKHOUSE_CLIENT --query "SELECT byteSize(x), x FROM format(RowBinaryWithNamesAndTypes, 'x Decimal(50, 2)', $(data "$N_D256"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), x FROM format(RowBinaryWithNamesAndTypes, $(data "$C_D256"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT byteSize(x), x FROM format(RowBinaryWithNamesAndTypes, 'x Decimal(50, 2)', $(data "$C_D256"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), x FROM format(RowBinaryWithNamesAndTypes, $(data "$C_P80"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT byteSize(x), x FROM format(RowBinaryWithNamesAndTypes, 'x Decimal(80, 2)', $(data "$C_P80"))"
$CLICKHOUSE_LOCAL --query "SELECT unhex('$C_D256') FORMAT RawBLOB" > "$DATA"
$CLICKHOUSE_CLIENT $DECODE --query "INSERT INTO t_e7d FORMAT RowBinaryWithNamesAndTypes" < "$DATA"
$CLICKHOUSE_CLIENT --query "SELECT x FROM t_e7d"

$CLICKHOUSE_CLIENT --query "DROP TABLE t_e7d"
rm -f "${ERR:?}" "${DATA:?}"
