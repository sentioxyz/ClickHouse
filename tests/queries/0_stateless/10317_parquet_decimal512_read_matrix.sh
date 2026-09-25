#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: Parquet is not built in fasttest.
#
# Decimal512 columns in Parquet, read through every value offset of the big-endian decoder: FIXED_LEN_BYTE_ARRAY widths
# 1..64 and BYTE_ARRAY values of 0..64 bytes (BigEndianHelper<Int512> reads up to 64 bytes before a value and relies on
# the left padding of the reader's buffers), with PLAIN / BYTE_STREAM_SPLIT / DELTA_BYTE_ARRAY / dictionary encodings,
# uncompressed and GZIP pages (the first value then starts the decompressed buffer), data page V1 and V2, REQUIRED and
# OPTIONAL columns. The fixtures and the expected text come from an independent writer and oracle
# (tests/decimal512/tools/parquet/parquet_decimal_writer.py, standard library only).

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

WRITER="$CURDIR/../../decimal512/tools/parquet/parquet_decimal_writer.py"
DATA="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$DATA"
mkdir -p "$DATA"
trap 'rm -rf "$DATA"' EXIT

python3 "$WRITER" matrix "$DATA" > /dev/null

while read -r name; do
    q=""
    while IFS=$'\t' read -r col _; do
        q+="SELECT '$col', arrayStringConcat(arrayMap(x -> x.2, arraySort(groupArray((rn, ifNull(toString($col), 'NULL'))))), ',') FROM file('$DATA/$name.parquet', Parquet);"
    done < "$DATA/$name.expected.tsv"
    if ${CLICKHOUSE_LOCAL} --multiquery --query "$q" < /dev/null 2>&1 | diff - "$DATA/$name.expected.tsv" > "$DATA/$name.diff"; then
        echo "$name: OK"
    else
        echo "$name: MISMATCH"
        head -n 5 "$DATA/$name.diff" | cut -c1-200
    fi
done < "$DATA/fixtures.txt"
