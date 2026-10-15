#!/usr/bin/env bash
# A schema written with the names of its types, which its reader creates the types from, cannot carry a type that its name does not
# identify: a decimal constructed with a precision of another decimal class (a native Decimal512 with a precision of at most 76) or with a
# scale greater than its precision, or a type that holds one. DESCRIBE, whose result also gives a remote table function its structure
# (getStructureOfRemoteTable), and the structure that fileCluster sends to its workers refuse such a type before they write it. Ordinary
# types are described and sent as before, and JSON tables stay describable.
#
# The payloads are RowBinaryWithNamesAndTypes with the types in binary format, derived from the encoding as in 10335 (one column named x;
# a 512-bit decimal is `2c` and its SentioWide descriptor; a value is the little-endian integer value * 10^scale in sizeof(type) bytes).
# The schema inference of the input keeps the decoded types.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
USER_FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_p50.bin"
USER_FILE_CONTROL="${CLICKHOUSE_TEST_UNIQUE_NAME}_p80.bin"

# D_*: the name of the type of x does not identify it. C_*: controls whose names do.
# Decimal512(50, 2), one row 123.45
D_P50='0101782c1e53656e74696f576964655f76315f446563696d616c3531322835302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal512(80, 90): a scale greater than the precision, no rows
D_S90='0101782c1f53656e74696f576964655f76315f446563696d616c3531322838302c393029'
# Array(Decimal512(50, 2)), no rows
D_ARRAY='0101781e2c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'
# AggregateFunction(sum, Decimal512(50, 2)), no rows
D_STATE='01017825000373756d00012c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'
# Decimal64(5, 2), one row 123.45: an upstream class with a precision of a narrower class
D_D64_P5='0101781a05023930000000000000'
# Decimal512(80, 2), one row 123.45
C_P80='0101782c1e53656e74696f576964655f76315f446563696d616c3531322838302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal256(50, 2), one row 123.45
C_D256='0101781c32023930000000000000000000000000000000000000000000000000000000000000'
# Int32, one row 7
C_INT32='0101780907000000'

DECODE='--input_format_binary_decode_types_in_binary_format 1'
DESCRIBED='cannot be written by its name in the result of DESCRIBE'
SENT='cannot be written by its name in the structure that fileCluster sends to its workers'

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

echo '--- DESCRIBE'
for name in D_P50 D_S90 D_ARRAY D_STATE D_D64_P5; do
    reject "$name" INCORRECT_DATA "$DESCRIBED" $CLICKHOUSE_CLIENT $DECODE --query "DESCRIBE $(source_of "${!name}")"
done
for name in C_P80 C_D256 C_INT32; do
    $CLICKHOUSE_CLIENT $DECODE --describe_compact_output 1 --query "DESCRIBE $(source_of "${!name}")"
done

echo '--- DESCRIBE of a temporary table'
reject 'TEMPORARY TABLE' INCORRECT_DATA "$DESCRIBED" \
    $CLICKHOUSE_CLIENT $DECODE --query "CREATE TEMPORARY TABLE tmp_e7c AS SELECT * FROM $(source_of "$D_P50"); DESCRIBE TABLE tmp_e7c"

echo '--- a table with JSON is described'
$CLICKHOUSE_CLIENT --describe_compact_output 1 --query "CREATE TABLE t_e7c_json (j JSON) ENGINE = Memory; DESCRIBE TABLE t_e7c_json; DROP TABLE t_e7c_json"

echo '--- remote: the structure of a remote table function comes from DESCRIBE on the remote server'
reject 'remote DESCRIBE' INCORRECT_DATA "$DESCRIBED" \
    $CLICKHOUSE_CLIENT $DECODE --query "DESCRIBE remote('127.0.0.2', $(source_of "$D_P50"))"
reject 'remote SELECT' INCORRECT_DATA "$DESCRIBED" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT toString(x) FROM remote('127.0.0.2', $(source_of "$D_P50"))"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), x FROM remote('127.0.0.2', $(source_of "$C_P80"))"

echo '--- fileCluster: the structure that it sends to its workers'
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('$USER_FILE', 'RawBLOB') SELECT unhex('$D_P50') SETTINGS engine_file_truncate_on_insert = 1"
$CLICKHOUSE_CLIENT --query "INSERT INTO FUNCTION file('$USER_FILE_CONTROL', 'RawBLOB') SELECT unhex('$C_P80') SETTINGS engine_file_truncate_on_insert = 1"
reject fileCluster INCORRECT_DATA "$SENT" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT toString(x) FROM fileCluster('test_cluster_two_shards_localhost', '$USER_FILE', 'RowBinaryWithNamesAndTypes')"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(x), byteSize(x), x FROM fileCluster('test_cluster_two_shards_localhost', '$USER_FILE_CONTROL', 'RowBinaryWithNamesAndTypes')"

rm -f "${CLICKHOUSE_USER_FILES:?}/${USER_FILE:?}" "${CLICKHOUSE_USER_FILES:?}/${USER_FILE_CONTROL:?}" "${ERR:?}"
