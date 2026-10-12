#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -o pipefail
ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
NATIVE="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.native"
POLICY='Binary type index QBitWithStride (0x3A) is not supported: strided QBit is disabled in the first release of this fork'

# A rejection passes only with a non-zero exit code, SUPPORT_IS_DISABLED and the exact policy message; any other outcome prints the
# exit code, stdout and stderr.
reject() {
    local out rc err
    out=$($CLICKHOUSE_LOCAL --query "$1" 2> "$ERR"); rc=$?
    err=$(cat "$ERR")
    if [ "$rc" -ne 0 ] && [[ "$err" == *SUPPORT_IS_DISABLED* ]] && [[ "$err" == *"$POLICY"* ]]; then
        echo "rejected: SUPPORT_IS_DISABLED"
    else
        echo "UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $err"
    fi
}

# The first release of this fork rejects the binary type index QBitWithStride (0x3A) before it reads the payload. The payloads
# are RowBinaryWithNamesAndTypes headers (one column named `x`) whose type is 0x3A with: no payload, a truncated payload (the
# element type only), QBit(Float32, 16, 8), the canonical QBit(Float32, 16, 16) and QBit(Float64, 512, 8).
for payload in 0101783a 0101783a0d 0101783a0d1008 0101783a0d1010 0101783a0e800408; do
    reject "SELECT * FROM format(RowBinaryWithNamesAndTypes, x'$payload') SETTINGS input_format_binary_decode_types_in_binary_format = 1"
done

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
