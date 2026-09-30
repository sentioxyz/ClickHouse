#!/usr/bin/env bash
# The 26.8 readers with 512-bit targets: ArrowIPC reinterprets 64-byte values as (U)Int512 like it does 32-byte values as
# (U)Int256, the native ORC reader matches 512-bit Variant alternatives in a union, and Parquet rejects a nonzero-scale
# DECIMAL read as (U)Int512 like it does for (U)Int256 instead of dropping the fraction. Expected values: the 256-bit
# behaviour (the fork's rule) or derived by hand. Files get unique names and are removed.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

F="${CLICKHOUSE_TEST_UNIQUE_NAME}"

echo "-- arrow int512 roundtrip"
$CLICKHOUSE_CLIENT --query="INSERT INTO FUNCTION file('${F}_i.arrow', 'Arrow', 'x Int512, y UInt512') SETTINGS engine_file_truncate_on_insert = 1 SELECT toInt512('-12345678901234567890123456789012345678901234567890123456789012345678901234567890'), bitShiftLeft(toUInt512(1), 511)"
$CLICKHOUSE_CLIENT --query="SELECT x, y FROM file('${F}_i.arrow', 'Arrow', 'x Int512, y UInt512')"

echo "-- arrow fixed_size_binary(64) as Int512 is reinterpreted like fixed_size_binary(32) as Int256"
$CLICKHOUSE_CLIENT --query="INSERT INTO FUNCTION file('${F}_fs.arrow', 'Arrow', 'a FixedString(32), b FixedString(64)') SETTINGS engine_file_truncate_on_insert = 1 SELECT CAST('12' AS FixedString(32)), CAST('12' AS FixedString(64))"
$CLICKHOUSE_CLIENT --query="SELECT a, b, toInt512(a) = b FROM file('${F}_fs.arrow', 'Arrow', 'a Int256, b Int512')"

echo "-- orc union with a 512-bit alternative"
$CLICKHOUSE_CLIENT --query="INSERT INTO FUNCTION file('${F}_v.orc', 'ORC', 'v Variant(Int512, Float64), w Variant(Decimal(154, 3), Float64)') SETTINGS engine_file_truncate_on_insert = 1 SELECT toInt512('-7')::Variant(Int512, Float64), toDecimal512('1.5', 3)::Variant(Decimal(154, 3), Float64)"
$CLICKHOUSE_CLIENT --query="SELECT v, variantType(v), w, variantType(w) FROM file('${F}_v.orc', 'ORC', 'v Variant(Int512, Float64), w Variant(Decimal(154, 3), Float64)')"

echo "-- parquet decimal with a nonzero scale read as (U)Int512 is rejected like (U)Int256"
$CLICKHOUSE_CLIENT --query="INSERT INTO FUNCTION file('${F}_d.parquet', 'Parquet', 'd Decimal(40, 2)') SETTINGS engine_file_truncate_on_insert = 1 SELECT toDecimal256('123.45', 2)"
$CLICKHOUSE_CLIENT --query="SELECT d FROM file('${F}_d.parquet', 'Parquet', 'd Int256')" 2>&1 | grep -o -m1 'INCORRECT_DATA'
$CLICKHOUSE_CLIENT --query="SELECT d FROM file('${F}_d.parquet', 'Parquet', 'd Int512')" 2>&1 | grep -o -m1 'INCORRECT_DATA'
$CLICKHOUSE_CLIENT --query="SELECT d FROM file('${F}_d.parquet', 'Parquet', 'd Decimal(154, 2)')"

rm -f "${USER_FILES_PATH:?}/${F}"_*
