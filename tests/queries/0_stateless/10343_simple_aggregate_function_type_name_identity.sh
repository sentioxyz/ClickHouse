#!/usr/bin/env bash
# A SimpleAggregateFunction type is created from names: -SimpleState creates it from the name of the result type of its function, and a
# binary type header from the names of its argument types. A native Decimal512 with a precision of at most 76 (64-byte values) has the name
# of a 32-byte Decimal256, so such a type would hold another type than its function expects: -SimpleState of a function whose result is one,
# and a binary SimpleAggregateFunction header with one as argument, are refused. A sum of it is a Decimal512 with the precision of its class,
# which its name identifies, and types created from their names are controls.
#
# The payloads are RowBinaryWithNamesAndTypes with the types in binary format, derived from the encoding by drafts/e6/gen_e6_hex.py (the
# rules of 10335 and 10342): `2c 1e` and the 30 bytes "SentioWide_v1_Decimal512(50,2)" are a native Decimal512(50, 2); `2e 03 any 00 01`
# is SimpleAggregateFunction(any, ...) with one argument.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"

# Native Decimal512(50, 2), one row 123.45
D_P50='0101782c1e53656e74696f576964655f76315f446563696d616c3531322835302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal512(80, 2), one row 123.45
C_P80='0101782c1e53656e74696f576964655f76315f446563696d616c3531322838302c322939300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000'
# Decimal256(50, 2), one row 123.45
C_D256='0101781c32023930000000000000000000000000000000000000000000000000000000000000'
# SimpleAggregateFunction(any, native Decimal512(50, 2)), no rows
D_SIMPLE='0101782e03616e7900012c1e53656e74696f576964655f76315f446563696d616c3531322835302c3229'

DECODE='--input_format_binary_decode_types_in_binary_format 1'
REFUSED='cannot be made for the type Decimal(50, 2)'

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

reject 'anySimpleState of a native Decimal512(50, 2)' INCORRECT_DATA "$REFUSED" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(anySimpleState(x)) FROM format(RowBinaryWithNamesAndTypes, x'$D_P50')"
reject 'binary SimpleAggregateFunction header' INCORRECT_DATA "$REFUSED" \
    $CLICKHOUSE_CLIENT $DECODE --query "SELECT count() FROM format(RowBinaryWithNamesAndTypes, x'$D_SIMPLE')"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(sumSimpleState(x)) FROM format(RowBinaryWithNamesAndTypes, x'$D_P50')"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(anySimpleState(x)) FROM format(RowBinaryWithNamesAndTypes, x'$C_P80')"
$CLICKHOUSE_CLIENT $DECODE --query "SELECT toTypeName(anySimpleState(x)) FROM format(RowBinaryWithNamesAndTypes, x'$C_D256')"

rm -f "${ERR:?}"
