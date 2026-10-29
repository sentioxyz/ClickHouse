#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# A strided QBit expands a tiny binary type header into `element_size * (dimension / stride)` hidden FixedString streams.
# Such a header must be charged against `input_format_binary_max_type_complexity`, otherwise a few bytes decode into an
# unreasonably wide type. The payloads below are RowBinaryWithNamesAndTypes headers (one column named `x`) whose type is a
# binary-encoded QBit with an explicit stride.
#
# In this fork the meaning of the index depends on the declared origin (`input_format_binary_type_origin`): strided QBit is 0x37 with
# `official_canonical` (as upstream) and 0x3A with `sentio_legacy_v0` (the index the fork used before); 0x37 alone is the fork's legacy
# UInt512, and 0x3A without an origin is refused. Each check prints the decoded type, `complexity limit` for the expected refusal, or the
# error, so that an accepted header is shown, not inferred from a missing message.

check() {
    local out
    out=$($CLICKHOUSE_CLIENT --query "$1" 2>&1)
    if [[ "$out" == *'Binary type decoding complexity limit exceeded'* ]]; then
        echo "complexity limit"
    elif [[ "$out" == *'Unknown type code: 0x3a'* ]]; then
        echo "unknown type code 0x3a"
    elif [[ "$out" == *'Exception'* ]]; then
        echo "error: $(echo "$out" | grep -o 'Code: [0-9]*' | tail -1)"
    else
        echo "$out" | cut -f1,2
    fi
}

for case in "37 official_canonical" "3a sentio_legacy_v0"; do
    read -r tag origin <<< "$case"
    echo "--- 0x${tag} ${origin}"
    S="input_format_binary_decode_types_in_binary_format = 1, input_format_binary_type_origin = '${origin}'"

    # QBit(Float64, 512, 8) is a valid type: 512 / 8 = 64 stride groups, well under MAX_STRIDE_GROUPS = 1024. But it materializes
    # element_size * num_strides = 64 * 64 = 4096 FixedString streams, so it must be rejected under the budget 1000.
    check "DESCRIBE format(RowBinaryWithNamesAndTypes, x'010178${tag}0e800408') SETTINGS ${S}, input_format_binary_max_type_complexity = 1000"

    # The same header decodes once the budget is raised above the hidden stream count, proving the rejection comes from the
    # complexity budget rather than the MAX_STRIDE_GROUPS cap.
    check "DESCRIBE format(RowBinaryWithNamesAndTypes, x'010178${tag}0e800408') SETTINGS ${S}, input_format_binary_max_type_complexity = 100000"

    # A small strided QBit stays well within the default budget: QBit(Float32, 16, 8) has 32 * (16 / 8) = 64 streams.
    check "DESCRIBE format(RowBinaryWithNamesAndTypes, x'010178${tag}0d1008') SETTINGS ${S}"
done

echo "--- 0x3a without an origin"
check "DESCRIBE format(RowBinaryWithNamesAndTypes, x'0101783a0d1008') SETTINGS input_format_binary_decode_types_in_binary_format = 1"
