#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -o pipefail
ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
NATIVE="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.native"
# These legacy type headers have explicit producer provenance. The caller's
# --client-option does not configure CLICKHOUSE_LOCAL; set the origin per probe.
# Unknown remains a separate negative control, and the type budget stays 1000.
reject() {
    local payload="$1" origin="$2" code="$3" name="$4" detail="$5" label="$6"
    local out rc err
    out=$($CLICKHOUSE_LOCAL --query "SELECT * FROM format(RowBinaryWithNamesAndTypes, x'$payload') SETTINGS input_format_binary_decode_types_in_binary_format = 1, input_format_binary_type_origin = '$origin', input_format_binary_max_type_complexity = 1000" 2> "$ERR"); rc=$?
    err=$(cat "$ERR")
    # Schema inference wraps the exact inner error in 636; 124 == 636 % 256,
    # not a timeout result. Do not accept an arbitrary nonzero exit status.
    if [ "$rc" -eq 124 ] && [ -z "$out" ] && [[ "$err" == *"Code: 636."* ]] && [[ "$err" == *CANNOT_EXTRACT_TABLE_STRUCTURE* ]] && [[ "$err" == *"Code: $code."* ]] && [[ "$err" == *"($name)"* ]] && [[ "$err" == *"$detail"* ]]; then
        printf '%s\n' "$label"
    else
        echo "UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $err"
    fi
}

accept_header() {
    local payload="$1" expected="$2" out rc err type
    out=$($CLICKHOUSE_LOCAL --query "DESCRIBE format(RowBinaryWithNamesAndTypes, x'$payload') SETTINGS input_format_binary_decode_types_in_binary_format = 1, input_format_binary_type_origin = 'sentio_legacy_v0', input_format_binary_max_type_complexity = 1000" 2> "$ERR"); rc=$?
    err=$(cat "$ERR")
    type=$(printf '%s\n' "$out" | cut -f1,2)
    if [ "$rc" -eq 0 ] && [ -z "$err" ] && [ "$type" = "$expected" ]; then
        printf '%s\n' "$type"
    else
        echo "UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $err"
    fi
}

# Keep all five original payloads. Bare 0x3A needs one element-type byte;
# 0x3A0D has Float32 but lacks the dimension VarUInt: these are distinct EOFs.
reject 0101783a sentio_legacy_v0 33 CANNOT_READ_ALL_DATA 'Cannot read all data' 'rejected: CANNOT_READ_ALL_DATA'
reject 0101783a0d sentio_legacy_v0 32 ATTEMPT_TO_READ_AFTER_EOF 'Attempt to read after eof' 'rejected: ATTEMPT_TO_READ_AFTER_EOF'
# Complete small headers must decode to the precise type, not just return no rows.
accept_header 0101783a0d1008 $'x\tQBit(Float32, 16, 8)'
accept_header 0101783a0d1010 $'x\tQBit(Float32, 16)'
# Float64/512/8 is shape-valid, but 4096 hidden streams plus three nodes
# exceed the unchanged 1000-node input budget. Do not raise it for this test.
reject 0101783a0e800408 sentio_legacy_v0 117 INCORRECT_DATA 'Binary type decoding complexity limit exceeded: 4099 > 1000' 'rejected: INCORRECT_DATA (complexity 4099 > 1000)'
# Declared provenance must not become a default or be inferred from the payload.
reject 0101783a0d1008 unknown 117 INCORRECT_DATA 'Unknown type code: 0x3a' 'rejected: INCORRECT_DATA (unknown origin)'

# 0x36 (non-strided QBit) still decodes, as QBit(Float32, 16).
out=$($CLICKHOUSE_LOCAL --query "DESCRIBE format(RowBinaryWithNamesAndTypes, x'010178360d10') SETTINGS input_format_binary_decode_types_in_binary_format = 1" 2> "$ERR"); rc=$?
if [ "$rc" -eq 0 ]; then
    printf '%s\n' "$out" | cut -f1,2
else
    echo "UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $(cat "$ERR")"
fi

# A canonical QBit round-trips through Native with binary-encoded types; the producer and the consumer must both succeed.
$CLICKHOUSE_LOCAL --query "SELECT CAST(range(1, 5)::Array(Float32), 'QBit(Float32, 4)') AS q FORMAT Native SETTINGS output_format_native_encode_types_in_binary_format = 1" > "$NATIVE" 2> "$ERR"; prc=$?
perr=$(cat "$ERR")
out=$($CLICKHOUSE_LOCAL --input-format Native --input_format_native_decode_types_in_binary_format=1 --query "SELECT toTypeName(q), CAST(q AS Array(Float32)) FROM table" < "$NATIVE" 2> "$ERR"); crc=$?
if [ "$prc" -eq 0 ] && [ "$crc" -eq 0 ]; then
    printf '%s\n' "$out"
else
    echo "UNEXPECTED producer rc=$prc consumer rc=$crc"; echo "producer stderr: $perr"; echo "consumer stdout: $out"; echo "consumer stderr: $(cat "$ERR")"
fi
rm -f "$ERR" "$NATIVE"
