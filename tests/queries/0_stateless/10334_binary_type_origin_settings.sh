#!/usr/bin/env bash
# The declared origin of binary type headers (`input_format_binary_type_origin`) and their writer mode
# (`output_format_binary_type_writer_mode`), with data written by the Decimal512 fork of 26.3 LTS.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
FILE="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.bin"
TABLE=t_binary_type_origin

# The blobs of 10311, written by the 26.3 LTS release binary (build id 203677193CDA6BD5FAAC9AB6B125066CFACE0ADB):
# - ROW_BINARY: `SELECT toInt512(-7) AS a, toUInt512(9) AS b, toDecimal512('-1.25', 2) AS c` in RowBinaryWithNamesAndTypes with the
#   types in binary format: 0x38 (Int512), 0x37 (UInt512) and 0x39 0x9A 0x02 (Decimal512(154, 2));
# - NATIVE: `id UInt32, v Dynamic(max_types = 1)` in Native with the types as names; the shared variant of `v` has an Int512 (0x38)
#   and a Decimal512 (0x39) value, and the UInt512 value is in its regular variant.
ROW_BINARY='030161016201633837399a02f9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000083ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff'
NATIVE='02030269640655496e74333200000000010000000200000001761444796e616d6963286d61785f74797065733d3129010000000000000001010755496e7435313200000000000000000001004138f9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff43399a0283ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff09000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
STRUCTURE='id UInt32, v Dynamic(max_types = 1)'

AMBIGUOUS='Binary type index 0x38 is ambiguous without a declared origin'
UNASSIGNED='Unknown type code: 0x38'

# A rejection passes only with a non-zero exit code, INCORRECT_DATA and the expected message; any other outcome prints the exit code,
# stdout and stderr. The arguments after the message are the command.
reject() {
    local message="$1"
    shift
    local out rc err
    out=$("$@" 2> "$ERR"); rc=$?
    err=$(cat "$ERR")
    if [ "$rc" -ne 0 ] && [[ "$err" == *INCORRECT_DATA* ]] && [[ "$err" == *"$message"* ]]; then
        echo "rejected: $message"
    else
        echo "UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $err"
    fi
}

echo '--- settings'
$CLICKHOUSE_CLIENT --query "SELECT name, value FROM system.settings WHERE name IN ('input_format_binary_type_origin', 'output_format_binary_type_writer_mode') ORDER BY name"
# `compatibility` never declares an origin or the bridge mode.
$CLICKHOUSE_CLIENT --query "SELECT getSetting('input_format_binary_type_origin'), getSetting('output_format_binary_type_writer_mode') SETTINGS compatibility = '26.3'"
$CLICKHOUSE_CLIENT --query "SELECT 1 SETTINGS input_format_binary_type_origin = 'guess'" 2>&1 | grep -o -m1 'BAD_ARGUMENTS'

echo '--- RowBinaryWithNamesAndTypes of the fork'
reject "$AMBIGUOUS" $CLICKHOUSE_LOCAL --query "SELECT * FROM format(RowBinaryWithNamesAndTypes, x'$ROW_BINARY') SETTINGS input_format_binary_decode_types_in_binary_format = 1"
reject "$UNASSIGNED" $CLICKHOUSE_LOCAL --query "SELECT * FROM format(RowBinaryWithNamesAndTypes, x'$ROW_BINARY') SETTINGS input_format_binary_decode_types_in_binary_format = 1, input_format_binary_type_origin = 'official_canonical'"
$CLICKHOUSE_LOCAL --query "SELECT toTypeName(a), toTypeName(b), toTypeName(c), a, b, c FROM format(RowBinaryWithNamesAndTypes, x'$ROW_BINARY') SETTINGS input_format_binary_decode_types_in_binary_format = 1, input_format_binary_type_origin = 'sentio_legacy_v0'"

echo '--- written canonically, then as a bridge'
QUERY="SELECT toInt512(-7) AS a, toUInt512(9) AS b, toDecimal512('-1.25', 2) AS c FORMAT RowBinaryWithNamesAndTypes"
$CLICKHOUSE_LOCAL --output_format_binary_encode_types_in_binary_format 1 --query "$QUERY" > "$FILE"
$CLICKHOUSE_LOCAL --input-format RawBLOB --query "SELECT position(raw_blob, 'SentioWide_v1_Int512') > 0, position(raw_blob, 'SentioWide_v1_UInt512') > 0, position(raw_blob, 'SentioWide_v1_Decimal512(154,2)') > 0 FROM table" < "$FILE"
$CLICKHOUSE_LOCAL --input-format RowBinaryWithNamesAndTypes --input_format_binary_decode_types_in_binary_format 1 --query "SELECT toTypeName(a), toTypeName(b), toTypeName(c), a, b, c FROM table" < "$FILE"
# The bridge writes exactly the bytes of the fork.
$CLICKHOUSE_LOCAL --output_format_binary_encode_types_in_binary_format 1 --output_format_binary_type_writer_mode sentio_legacy_bridge --query "$QUERY" > "$FILE"
$CLICKHOUSE_LOCAL --input-format RawBLOB --query "SELECT hex(raw_blob) = upper('$ROW_BINARY') FROM table" < "$FILE"

echo '--- Native of the fork: nothing is stored or copied without a declared origin'
$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS $TABLE"
$CLICKHOUSE_CLIENT --query "CREATE TABLE $TABLE ($STRUCTURE) ENGINE = MergeTree ORDER BY id"
reject "$AMBIGUOUS" $CLICKHOUSE_CLIENT --query "INSERT INTO $TABLE SELECT * FROM format(Native, '$STRUCTURE', x'$NATIVE')"
reject "$AMBIGUOUS" $CLICKHOUSE_CLIENT --query "SELECT * FROM format(Native, '$STRUCTURE', x'$NATIVE') FORMAT Native"
reject "$UNASSIGNED" $CLICKHOUSE_CLIENT --query "INSERT INTO $TABLE SELECT * FROM format(Native, '$STRUCTURE', x'$NATIVE') SETTINGS input_format_binary_type_origin = 'official_canonical'"
$CLICKHOUSE_CLIENT --query "SELECT count() FROM $TABLE"

echo '--- with the declared legacy origin'
$CLICKHOUSE_CLIENT --query "INSERT INTO $TABLE SELECT * FROM format(Native, '$STRUCTURE', x'$NATIVE') SETTINGS input_format_binary_type_origin = 'sentio_legacy_v0'"
$CLICKHOUSE_CLIENT --query "SELECT id, v, dynamicType(v), isDynamicElementInSharedData(v) FROM $TABLE ORDER BY id"

echo '--- exported canonically, imported without a declared origin'
$CLICKHOUSE_CLIENT --query "SELECT * FROM $TABLE ORDER BY id FORMAT Native" > "$FILE"
$CLICKHOUSE_LOCAL --input-format RawBLOB --query "SELECT position(raw_blob, 'SentioWide_v1_Int512') > 0, position(raw_blob, 'SentioWide_v1_Decimal512(154,2)') > 0, position(raw_blob, unhex('399A02')) = 0 FROM table" < "$FILE"
$CLICKHOUSE_CLIENT --query "TRUNCATE TABLE $TABLE"
$CLICKHOUSE_CLIENT --query "INSERT INTO $TABLE FORMAT Native" < "$FILE"
$CLICKHOUSE_CLIENT --query "SELECT id, v, dynamicType(v), isDynamicElementInSharedData(v) FROM $TABLE ORDER BY id"

echo '--- exported as a bridge: the indexes of the fork, read only with the declared legacy origin'
${CLICKHOUSE_CURL} -sS "${CLICKHOUSE_URL}&output_format_binary_type_writer_mode=sentio_legacy_bridge" -d "SELECT * FROM $TABLE ORDER BY id FORMAT Native" > "$FILE"
$CLICKHOUSE_LOCAL --input-format RawBLOB --query "SELECT position(raw_blob, 'SentioWide_') = 0, position(raw_blob, unhex('399A02')) > 0 FROM table" < "$FILE"
reject "$AMBIGUOUS" $CLICKHOUSE_LOCAL --input-format Native --query "SELECT * FROM table" < "$FILE"
$CLICKHOUSE_LOCAL --input-format Native --input_format_binary_type_origin sentio_legacy_v0 --query "SELECT id, v, dynamicType(v) FROM table ORDER BY id" < "$FILE"

$CLICKHOUSE_CLIENT --query "DROP TABLE $TABLE"
rm -f "$ERR" "$FILE"
