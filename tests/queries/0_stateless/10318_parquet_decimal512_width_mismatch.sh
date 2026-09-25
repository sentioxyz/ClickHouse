#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: Parquet is not built in fasttest.
#
# Decimal512 shapes of two upstream Parquet reader bugs (fixed by the backports of #113046 and #115703):
#  * a FIXED_LEN_BYTE_ARRAY decimal wider than 32 bytes selects the Int512 decoder, but the column was created from the
#    declared (narrower) precision: every value wrote 64 bytes into a 32/8/4-byte slot (wrong values, heap corruption);
#  * a BYTE_ARRAY decimal of precision > 76 encoded with DELTA_BYTE_ARRAY and read under a filter: the filtered decode
#    path treated the Decimal512 column as a String column (crash).
# Fixtures come from tests/decimal512/tools/parquet/parquet_decimal_writer.py; the expected values from its oracle.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

WRITER_DIR="$CURDIR/../../decimal512/tools/parquet"
DATA="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$DATA"
mkdir -p "$DATA"
trap 'rm -rf "$DATA"' EXIT

python3 - "$WRITER_DIR" "$DATA" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[1])
from parquet_decimal_writer import write_file
out = sys.argv[2]
vals = [(-1) ** i * (i * 7919 % 100000 * 100 + i % 100) for i in range(300)]
for name, width, precision in (("flba40_p40", 40, 40), ("flba33_p10", 33, 10), ("flba64_p9", 64, 9)):
    for enc in ("plain", "dict"):
        write_file(f"{out}/{name}_{enc}.parquet", [{"name": "k", "physical": "flba", "type_length": width,
                   "precision": precision, "scale": 2, "encoding": enc, "values": vals}])
# 10^21 (unscaled) does not fit the Int64 of Decimal(10, 2). ClickHouse checks a Decimal's native range, not its declared
# precision (official 26.8.8.8: CAST(toDecimal256('10000000000', 2) AS Decimal(10, 2)) = 10000000000 without an error),
# so a value that only exceeds the precision would be read as is.
write_file(f"{out}/flba40_p10_overflow.parquet", [{"name": "k", "physical": "flba", "type_length": 40, "precision": 10,
           "scale": 2, "encoding": "plain", "values": [10 ** 21] + vals[1:]}])
write_file(f"{out}/ba_delta_p100.parquet", [
    {"name": "keep", "physical": "int32", "encoding": "plain", "values": [int(i % 3 == 0) for i in range(300)]},
    {"name": "value", "physical": "byte_array", "precision": 100, "scale": 2, "encoding": "delta_byte_array",
     "values": vals}])
PYEOF

for f in flba40_p40 flba33_p10 flba64_p9; do
    for enc in plain dict; do
        echo "-- FIXED_LEN_BYTE_ARRAY ${f#flba}, $enc"
        ${CLICKHOUSE_LOCAL} --query "SELECT count(), sum(k), toTypeName(any(k)) FROM file('$DATA/${f}_$enc.parquet', Parquet)" < /dev/null
    done
done

echo '-- a value beyond the range of the column type is an error, not a corrupted read'
${CLICKHOUSE_LOCAL} --query "SELECT count(), sum(k) FROM file('$DATA/flba40_p10_overflow.parquet', Parquet)" < /dev/null 2>&1 | grep -o -m1 'DECIMAL_OVERFLOW'
echo '-- ... and reads losslessly with a wide enough type hint'
${CLICKHOUSE_LOCAL} --query "SELECT count(), sum(k), toTypeName(any(k)) FROM file('$DATA/flba40_p10_overflow.parquet', Parquet, 'k Decimal(154, 2)')" < /dev/null

echo '-- BYTE_ARRAY DECIMAL(100, 2), DELTA_BYTE_ARRAY, under PREWHERE'
${CLICKHOUSE_LOCAL} --query "
SELECT count(), sum(value) FROM file('$DATA/ba_delta_p100.parquet', Parquet)
PREWHERE keep = 1
SETTINGS
    input_format_parquet_max_block_size = 1,
    input_format_parquet_use_offset_index = 0,
    input_format_parquet_filter_push_down = 0,
    input_format_parquet_page_filter_push_down = 0,
    input_format_parquet_bloom_filter_push_down = 0" < /dev/null
${CLICKHOUSE_LOCAL} --query "SELECT count(), sum(value), toTypeName(any(value)) FROM file('$DATA/ba_delta_p100.parquet', Parquet)" < /dev/null
