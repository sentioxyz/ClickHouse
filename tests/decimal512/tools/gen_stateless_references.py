#!/usr/bin/env python3
"""gen_stateless_references.py - write the stateless tests 10310-10316 (.sql and .reference) from an independent oracle.

PRODUCTION BOUNDARY: pure Python; no engine, no server, no network.

Every expected value is computed here with Python big integers and exact decimal formatting, from the documented
rules (NumberTraits result types, two's-complement wrap-around, truncating division, the getLeastSupertype ladder,
midpoint.h, the Int512 range of Decimal512), never recorded from a ClickHouse binary. Error cases carry serverError
annotations and print nothing. ci_harness.sh regenerates the files and requires them to be byte-identical to the
committed ones, so a reference can only change together with this oracle.

  gen_stateless_references.py <out-dir>      (the committed copies live in tests/queries/0_stateless)
"""
import os, sys
from fractions import Fraction
OUT = sys.argv[1]
I_MAX, I_MIN, U_MAX = 2**511 - 1, -2**511, 2**512 - 1
def wrap_s(v, bits=512): return (v + 2**(bits - 1)) % 2**bits - 2**(bits - 1)
def wrap_u(v, bits=512): return v % 2**bits
def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q
def trunc_mod(a, b): return a - b * trunc_div(a, b)
def half(v): return v // 2 if v >= 0 else -((-v) // 2)

def write(name, header, rows):
    sql, ref = [header.rstrip() + "\n"], []
    for q, r in rows:
        sql.append(q.rstrip() + "\n")
        if r is not None:
            ref.append(r + "\n")
    open(os.path.join(OUT, name + ".sql"), "w").write("".join(sql))
    open(os.path.join(OUT, name + ".reference"), "w").write("".join(ref))

# 1. (U)Int512 arithmetic
rows = []
def arith(expr, typ, val):
    rows.append((f"SELECT toTypeName({expr}), {expr};", f"{typ}\t{val}"))
I, U = "toInt512", "toUInt512"
arith(f"{I}(1) + {I}(2)", "Int512", 3)
arith(f"{I}('{I_MAX}') + {I}(1)", "Int512", wrap_s(I_MAX + 1))
arith(f"{I}('{I_MIN}') - {I}(1)", "Int512", wrap_s(I_MIN - 1))
arith(f"{I}('{I_MAX}') * {I}(2)", "Int512", wrap_s(I_MAX * 2))
arith(f"{U}('{U_MAX}') + {U}(1)", "UInt512", wrap_u(U_MAX + 1))
arith(f"{U}(0) - {U}(1)", "Int512", -1)
arith(f"{U}('{U_MAX}') * {U}(2)", "UInt512", wrap_u(U_MAX * 2))
arith(f"{I}(5) + toInt8(-7)", "Int512", -2)
arith(f"{U}(3) * toUInt8(4)", "UInt512", 12)
arith(f"{I}(-1) + toUInt64(2)", "Int512", 1)
arith(f"intDiv({I}(-7), {I}(2))", "Int512", trunc_div(-7, 2))
arith(f"modulo({I}(-7), {I}(2))", "Int512", trunc_mod(-7, 2))
arith(f"intDiv({U}(7), {U}(2))", "UInt512", 3)
arith(f"{I}(7) / 2", "Float64", "3.5")
arith(f"bitAnd({I}(6), toInt8(3))", "Int512", 2)
arith(f"bitShiftLeft({I}(1), 500)", "Int512", 2**500)
arith(f"bitShiftRight({I}(-8), 1)", "Int512", -4)
arith(f"gcd({I}(12), {I}(18))", "Int512", 6)
rows.append((f"SELECT {I}('{I_MAX}') > {I}('{I_MAX - 1}'), {I}(-1) = toInt8(-1), {U}('{U_MAX}') > {I}(0);", "1\t1\t1"))
rows.append((f"SELECT toTypeName(materialize({I}('{I_MAX}')) + {I}(1)), materialize({I}('{I_MAX}')) + {I}(1);", f"Int512\t{wrap_s(I_MAX + 1)}"))
rows.append((f"SELECT toTypeName(x * y), x * y FROM (SELECT materialize({U}('{U_MAX}')) AS x, materialize({U}(2)) AS y);", f"UInt512\t{wrap_u(U_MAX * 2)}"))
# the same operations on (U)Int256 give the 256-bit analogue (the rule the 512-bit rows follow)
rows.append(("SELECT toTypeName(toUInt256(0) - toUInt256(1)), toUInt256(0) - toUInt256(1), toTypeName(intDiv(toInt256(-7), toInt256(2))), intDiv(toInt256(-7), toInt256(2));", "Int256\t-1\tInt256\t-3"))
rows.append((f"SELECT {I}(1) + 1.5; -- {{ serverError ILLEGAL_TYPE_OF_ARGUMENT }}", None))
rows.append((f"SELECT toDecimal512('1.5', 1) + {I}(2); -- {{ serverError ILLEGAL_TYPE_OF_ARGUMENT }}", None))
# intDiv/intDivOrZero with a decimal operand run on the decimal path, which has no (U)Int512 case: rejected like the
# other decimal operations (the result-type check used to accept them and execution then threw LOGICAL_ERROR)
for q in (f"intDiv(toDecimal512('7.5', 1), {I}(2))", f"intDiv({I}(7), toDecimal512('2.5', 1))", f"intDivOrZero(toDecimal32('7.5', 1), {U}(2))",
          f"intDiv(materialize(toDecimal256('7.5', 1)), materialize({U}(2)))"):
    rows.append((f"SELECT {q}; -- {{ serverError ILLEGAL_TYPE_OF_ARGUMENT }}", None))
rows.append(("SELECT toTypeName(intDiv(toDecimal256('7.5', 1), toInt256(2))), intDiv(toDecimal256('7.5', 1), toInt256(2));", "Int256\t3"))
# as for (U)Int256: a big integer shift amount is not implemented, bitHammingDistance needs two big integers of one width
rows.append((f"SELECT bitShiftLeft({I}(7), toInt128(3)); -- {{ serverError NOT_IMPLEMENTED }}", None))
rows.append(("SELECT bitShiftLeft(toInt256(7), toInt128(3)); -- { serverError NOT_IMPLEMENTED }", None))
# BitHammingDistanceImpl: UInt16 for operands of 256 bits and more
rows.append((f"SELECT toTypeName(bitHammingDistance({I}(7), {I}(3))), bitHammingDistance({I}(7), {I}(3));", "UInt16\t1"))
write("10310_int512_arithmetic", "-- (U)Int512 integer arithmetic behaves like (U)Int256: the same result-type rules (NumberTraits), two's-complement\n-- wrap-around on overflow, intDiv/modulo truncating toward zero, division giving Float64. As for Int256, adding a\n-- float is rejected. Decimal op (U)Int512 stays rejected, intDiv included (unlike Int256: narrow decimal results would\n-- truncate silently).\n-- Expected values were computed with Python big integers.", rows)

# 2. common supertype
rows = []
rows.append(("SELECT toTypeName([toInt512(1), toInt8(2)]), [toInt512(1), toInt8(2)];", "Array(Int512)\t[1,2]"))
rows.append(("SELECT toTypeName([toUInt512(7), toUInt8(3)]), [toUInt512(7), toUInt8(3)];", "Array(UInt512)\t[7,3]"))
rows.append(("SELECT toTypeName(if(number = 0, toInt512(-5), toInt64(3))), if(number = 0, toInt512(-5), toInt64(3)) FROM numbers(2);", "Int512\t-5\nInt512\t3"))
rows.append(("SELECT toTypeName(x), x FROM (SELECT toInt512(1) AS x UNION ALL SELECT toInt32(2)) ORDER BY x;", "Int512\t1\nInt512\t2"))
rows.append(("SELECT toTypeName(coalesce(CAST(NULL AS Nullable(Int512)), toInt8(5))), coalesce(CAST(NULL AS Nullable(Int512)), toInt8(5));", "Int512\t5"))
rows.append(("SELECT toTypeName(greatest(toInt512(-5), toInt8(3))), greatest(toInt512(-5), toInt8(3));", "Int512\t3"))
rows.append(("SELECT toTypeName(least(toUInt512(7), toUInt16(3))), least(toUInt512(7), toUInt16(3));", "UInt512\t3"))
# signed + unsigned at the extremes: the result must hold both, never a narrower type
u256 = 2**256 - 1
rows.append((f"SELECT toTypeName([toInt512(-1), toUInt256('{u256}')]), [toInt512(-1), toUInt256('{u256}')];", f"Array(Int512)\t[-1,{u256}]"))
rows.append((f"SELECT toTypeName([toInt512('{I_MIN}'), toUInt64(18446744073709551615)]), [toInt512('{I_MIN}'), toUInt64(18446744073709551615)];", f"Array(Int512)\t[{I_MIN},18446744073709551615]"))
rows.append(("SELECT [toInt512(-1), toUInt512(1)] SETTINGS use_variant_as_common_type = 0; -- { serverError NO_COMMON_TYPE }", None))
rows.append(("SELECT [toUInt512(1), toInt8(-1)] SETTINGS use_variant_as_common_type = 0; -- { serverError NO_COMMON_TYPE }", None))
rows.append(("SELECT toTypeName([toUInt512(1), toInt8(-1)]) SETTINGS use_variant_as_common_type = 1;", "Array(Variant(Int8, UInt512))"))
# mixes of 256-bit and narrower types keep their upstream result
rows.append(("SELECT [toInt256(-1), toUInt256(1)] SETTINGS use_variant_as_common_type = 0; -- { serverError NO_COMMON_TYPE }", None))
rows.append(("SELECT toTypeName([toInt256(-1), toUInt128(1)]), toTypeName([toUInt256(1), toUInt8(1)]);", "Array(Int256)\tArray(UInt256)"))
write("10311_int512_common_supertype", "-- The common supertype of (U)Int512 and another integer type is (U)Int512 when it can hold both values, instead of\n-- Variant(...) (the getLeastSupertype ladder used to stop at 256 bits). When no integer type can hold both (Int512 with\n-- UInt512, UInt512 with a negative type) there is still no common type. Mixes of narrower types are unchanged.", rows)

# 3. single keys
rows = []
for t, label in (("Int512", "Int512"), ("UInt512", "UInt512"), ("Nullable(Int512)", "Nullable(Int512)"), ("Nullable(UInt512)", "Nullable(UInt512)")):
    rows.append((f"SELECT '{label} IN', count() FROM (SELECT CAST(number AS {t}) AS k FROM numbers(100)) WHERE k IN (SELECT CAST(number * 2 AS {t}) FROM numbers(50));", f"{label} IN\t50"))
    rows.append((f"SELECT '{label} DISTINCT', count() FROM (SELECT DISTINCT CAST(number % 7 AS {t}) AS k FROM numbers(100));", f"{label} DISTINCT\t7"))
    rows.append((f"SELECT '{label} GROUP BY', count() FROM (SELECT CAST(number % 7 AS {t}) AS k FROM numbers(100) GROUP BY k);", f"{label} GROUP BY\t7"))
    for alg in ("hash", "parallel_hash"):
        rows.append((f"SELECT '{label} JOIN {alg}', count() FROM (SELECT CAST(number AS {t}) AS k FROM numbers(100)) AS l INNER JOIN (SELECT CAST(number * 2 AS {t}) AS k FROM numbers(50)) AS r USING (k) SETTINGS join_algorithm = '{alg}';", f"{label} JOIN {alg}\t50"))
rows.append((f"SELECT count() FROM (SELECT DISTINCT k FROM values('k Int512', ('{I_MIN}'), ('{I_MAX}'), (0), (-1), (0)));", "4"))
rows.append((f"SELECT k FROM values('k Int512', ('{I_MIN}'), ('{I_MAX}'), (0)) WHERE k IN (SELECT arrayJoin([toInt512('{I_MIN}'), toInt512(0)])) ORDER BY k;", f"{I_MIN}\n0"))
rows.append((f"SELECT l.k FROM values('k UInt512', ('{U_MAX}'), (1)) AS l INNER JOIN values('k UInt512', ('{U_MAX}'), (2)) AS r USING (k);", f"{U_MAX}"))
rows.append(("SELECT count() FROM (SELECT DISTINCT k FROM values('k Nullable(Int512)', (NULL), (0), (NULL), (0), (1)));", "3"))
rows.append(("SELECT count() FROM values('k Nullable(Int512)', (NULL), (0), (1)) WHERE k IN (SELECT CAST(NULL AS Nullable(Int512))) SETTINGS transform_null_in = 1;", "1"))
rows.append(("SELECT count() FROM values('k Nullable(Int512)', (NULL), (0), (1)) WHERE k IN (SELECT CAST(NULL AS Nullable(Int512))) SETTINGS transform_null_in = 0;", "0"))
write("10312_int512_single_key", "-- A single (U)Int512 key, nullable or not, in IN / DISTINCT / GROUP BY / JOIN (hash and parallel_hash). It used to\n-- throw LOGICAL_ERROR (\"Numeric column has sizeOfField not in 1, 2, 4, 8, 16, 32\") in sets and joins.", rows)

# 4. midpoint / avg2
rows = []
for fn in ("avg2", "midpoint"):
    for a, b in ((1, 3), (1, 2), (-3, 0), (-3, -4), (-1, 2), (I_MAX, I_MAX), (I_MIN, I_MIN), (I_MAX, I_MIN), (I_MIN, -1)):
        rows.append((f"SELECT toTypeName({fn}(toInt512('{a}'), toInt512('{b}'))), {fn}(toInt512('{a}'), toInt512('{b}'));", f"Int512\t{half(a + b)}"))
    for a, b in ((0, 1), (1, 2), (U_MAX, U_MAX), (U_MAX, U_MAX - 1)):
        rows.append((f"SELECT toTypeName({fn}(toUInt512('{a}'), toUInt512('{b}'))), {fn}(toUInt512('{a}'), toUInt512('{b}'));", f"UInt512\t{(a + b) // 2}"))
    rows.append((f"SELECT {fn}(materialize(toInt512(-3)), toInt512(0)), {fn}(materialize(toInt512('{I_MAX}')), materialize(toInt512('{I_MIN}')));", "-1\t0"))
rows.append(("SELECT avg2(toInt256(-3), toInt256(0)), midpoint(toInt256(1), toInt256(2));", "-1\t1"))
write("10313_int512_midpoint", "-- avg2/midpoint of two (U)Int512: result type (U)Int512, truncation toward zero without overflow at the extremes,\n-- exactly as for (U)Int256 (midpoint.h MidpointImpl). It used to throw LOGICAL_ERROR.", rows)

# 5. Decimal512 range
rows = []
def dec(v, s):
    neg = v < 0; v = abs(v); t = str(v)
    if s:
        t = t.rjust(s + 1, "0"); t = t[:-s] + "." + t[-s:]
    return ("-" if neg else "") + t
for s in (0, 2, 65):
    t = f"Decimal(154, {s})"
    rows.append((f"SELECT CAST('{dec(I_MAX, s)}' AS {t}), CAST('{dec(I_MIN, s)}' AS {t});", f"{dec(I_MAX, s)}\t{dec(I_MIN, s)}"))
    rows.append((f"SELECT CAST('{dec(I_MAX + 1, s)}' AS {t}); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW }}", None))
    rows.append((f"SELECT CAST('{dec(I_MIN - 1, s)}' AS {t}); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW }}", None))
    rows.append((f"SELECT toDecimal512('{dec(I_MAX + 1, s)}', {s}); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW }}", None))
    rows.append((f"SELECT toDecimal512OrNull('{dec(I_MAX + 1, s)}', {s}), toDecimal512OrZero('{dec(I_MIN - 1, s)}', {s});", f"\\N\t{dec(0, s)}"))
nines = "9" * 154
rows.append((f"SELECT CAST('{nines}' AS Decimal(154, 0)); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST('{'9' * 153}' AS Decimal(154, 1)); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST('{'1' + '0' * 152}' AS Decimal(154, 1));", f"{'1' + '0' * 152}.0"))
rows.append((f"SELECT toDecimal512(toUInt512('{I_MAX + 1}'), 0); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT toDecimal512(materialize(toUInt512('{I_MAX + 1}')), 0); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT toDecimal512(toUInt512('{I_MAX}'), 0), toDecimal512(toInt512('{I_MIN}'), 0);", f"{I_MAX}\t{I_MIN}"))
rows.append((f"SELECT toDecimal512(toInt512('1{'0' * 90}'), 0), toDecimal512(materialize(toInt512('1{'0' * 90}')), 3);", f"1{'0' * 90}\t1{'0' * 90}.000"))
rows.append((f"SELECT toDecimal256(toInt512('1{'0' * 90}'), 0); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT toDecimal256(materialize(toInt512('1{'0' * 90}')), 0); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT toDecimal512(toInt512('1{'0' * 150}'), 5); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT toDecimal512(toDecimal256('{'9' * 76}', 0), 78); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT toDecimal512(toDecimal256('{'9' * 76}', 0), 77);", f"{'9' * 76}.{'0' * 77}"))
rows.insert(0, ("SET output_format_decimal_trailing_zeros = 1;", None))
rows.append((f"SELECT * FROM format(CSV, 'x Decimal(154, 0)', '{I_MAX + 1}'); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW, CANNOT_PARSE_INPUT_ASSERTION_FAILED }}", None))
rows.append((f"SELECT * FROM format(CSV, 'x Decimal(154, 0)', '{I_MIN}');", f"{I_MIN}"))
rows.append((f"SELECT CAST(materialize('{I_MAX + 1}') AS Decimal(154, 0)); -- {{ serverError ARGUMENT_OUT_OF_BOUND, DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST(materialize('{dec(I_MIN, 2)}') AS Decimal(154, 2));", f"{dec(I_MIN, 2)}"))
write("10314_decimal512_int512_range", "-- Decimal512 keeps precision 154, but its values must fit Int512 (|v| < 2^511, about 6.7e153). Values outside that\n-- range are rejected when parsed, cast or scaled, instead of wrapping silently. Conversions from (U)Int512 go through\n-- an exact 512-bit intermediate (they used to be truncated to 256 bits).", rows)

# 6. Decimal512 multiply overflow
rows = []
t0 = "Decimal(154, 0)"
rows.append((f"SELECT multiply(CAST('{I_MAX}' AS {t0}), toInt8(2)); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST('{I_MAX}' AS {t0}) * CAST('2' AS {t0}); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST('1{'0' * 77}' AS {t0}) * CAST('1{'0' * 77}' AS {t0}); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST('{I_MIN}' AS {t0}) * toInt8(-1); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT materialize(CAST('{I_MAX}' AS {t0})) * toInt8(2); -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT CAST('1{'0' * 76}' AS {t0}) * CAST('1{'0' * 77}' AS {t0}), CAST('{I_MIN}' AS {t0}) * toInt8(1), CAST('{I_MAX}' AS {t0}) * toInt8(-1);", f"1{'0' * 153}\t{I_MIN}\t{-I_MAX}"))
rows.append((f"SELECT CAST('1.5' AS Decimal(154, 70)) * CAST('2.5' AS Decimal(154, 70));", f"3.75{'0' * 138}"))
rows.append((f"SELECT CAST('{I_MAX}' AS {t0}) * toInt8(2) SETTINGS decimal_check_overflow = 0;", f"{wrap_s(I_MAX * 2)}"))
# operand scale-ups (plus/minus/compare/divide) are multiplications too: same rule
e60, half100 = 10 ** 60, 5 * 10 ** 99
pl = f"plus(CAST('{e60}' AS {t0}), CAST('0.5' AS Decimal(154, 100)))"
rows.append((f"SELECT {pl}; -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT {pl} SETTINGS decimal_check_overflow = 0;", dec(wrap_s(wrap_s(e60 * 10 ** 100) + half100), 100)))
ls = f"less(CAST('{7 * e60}' AS {t0}), CAST('0.5' AS Decimal(154, 100)))"
# a Decimal512 comparison is decided exactly even when the scale-up leaves Int512 (in both overflow modes)
rows.append((f"SELECT {ls};", str(int(7 * e60 < Fraction(1, 2)))))
rows.append((f"SELECT {ls} SETTINGS decimal_check_overflow = 0;", str(int(7 * e60 < Fraction(1, 2)))))
# plus/minus/divide compute a scale-up that leaves Int512 exactly (1024 bits); only a result outside Int512 overflows
rows.append((f"SELECT minus(CAST('{7 * 10 ** 53}' AS {t0}), CAST('{3 * 10 ** 53}' AS Decimal(154, 100)));", dec(4 * 10 ** 53 * 10 ** 100, 100)))
rows.append((f"SELECT divide(CAST('1{'0' * 40}' AS Decimal(154, 60)), CAST('2' AS Decimal(154, 60)));", dec(5 * 10 ** 39 * 10 ** 60, 60)))
dv = f"divide(CAST('{10 ** 152}' AS Decimal(154, 1)), CAST('0.1' AS Decimal(154, 1)))"
rows.append((f"SELECT {dv}; -- {{ serverError DECIMAL_OVERFLOW }}", None))
rows.append((f"SELECT {dv} SETTINGS decimal_check_overflow = 0;", dec(wrap_s(10 ** 153 * 10), 1)))
rows.append((f"SELECT divide(CAST('{10 ** 151}' AS Decimal(154, 1)), CAST('0.1' AS Decimal(154, 1)));", dec(10 ** 153, 1)))
rows.insert(0, ("SET output_format_decimal_trailing_zeros = 1;", None))
rows.insert(1, ("SET decimal_check_overflow = 1;", None))
n76 = 10**76 - 1
rows.append((f"SELECT toDecimal256('{n76}', 0) * toInt8(10);", f"{wrap_s(n76 * 10, 256)}"))
e40 = 10 ** 40
rows.append((f"SELECT plus(toDecimal256('{e40}', 0), toDecimal256('0.5', 40));", dec(wrap_s(wrap_s(e40 * 10 ** 40, 256) + 5 * 10 ** 39, 256), 40)))
write("10315_decimal512_multiply_overflow", "-- Decimal512 multiplication detects overflow when decimal_check_overflow = 1 (the default), like Decimal32/Decimal64,\n-- instead of wrapping silently; with decimal_check_overflow = 0 it wraps as documented. Decimal128/Decimal256 keep the\n-- upstream behaviour (no overflow check, docs/en/sql-reference/data-types/decimal.md), shown by the last rows. An operand\n-- scale-up of plus/minus/divide that leaves Int512 is computed exactly and overflows only if the result does not fit;\n-- comparisons are always exact.", rows)
# 7. Decimal(154, 154) and precision 154: 10^154 does not fit Int512 (it wrapped to a negative number)
rows = [("SET decimal_check_overflow = 1;", None)]
def fmt(raw, s):
    neg = raw < 0; a = abs(raw); w, f = divmod(a, 10 ** s)
    t = str(w) + ("." + str(f).rjust(s, "0").rstrip("0") if s and f else "")
    return ("-" if neg else "") + t
def c(v, s): return f"CAST('{v}' AS Decimal(154, {s}))"
def raw(v, s):
    r = Fraction(v) * 10 ** s
    assert r.denominator == 1
    return int(r)
H = raw("0.5", 154)
cm, cn = c(fmt(I_MAX, 154), 154), c(fmt(I_MIN, 154), 154)
q = lambda expr, val: rows.append((f"SELECT {expr};", val))
err = lambda expr, codes="DECIMAL_OVERFLOW": rows.append((f"SELECT {expr}; -- {{ serverError {codes} }}", None))
q(f"{c('0.5', 154)}, {c('-0.5', 154)}", "0.5\t-0.5")
q(f"{cm}, {cn}", f"{fmt(I_MAX, 154)}\t{fmt(I_MIN, 154)}")
q(f"materialize({cn}), toString({c('-0.25', 154)})", f"{fmt(I_MIN, 154)}\t-0.25")
q(f"toDecimalString({c('0.123456789', 154)}, 5), toDecimalString({cn}, 3)", "0.12346\t-0.670")
err(c("1", 154), "ARGUMENT_OUT_OF_BOUND, CANNOT_PARSE_NUMBER, DECIMAL_OVERFLOW")
err(c(fmt(I_MAX + 1, 154), 154), "ARGUMENT_OUT_OF_BOUND, CANNOT_PARSE_NUMBER, DECIMAL_OVERFLOW")
q(f"abs(toFloat64({c('0.5', 154)}) - 0.5) < 1e-15, abs(toFloat64({cm}) - 0.6703903964971298) < 1e-15", "1\t1")
q(f"abs(toFloat64(CAST(toFloat64(0.5) AS Decimal(154, 154))) - 0.5) < 1e-15, abs(toFloat64(toDecimal512(materialize(toFloat64(-0.25)), 154)) + 0.25) < 1e-15", "1\t1")
err("CAST(toFloat64(1) AS Decimal(154, 154))")
q(f"CAST({c('0.5', 154)} AS Decimal(154, 0)), CAST(materialize({c('-0.5', 154)}) AS Decimal(154, 1)), CAST({c('0.5', 154)} AS Decimal(76, 76))", "0\t-0.5\t0.5")
q(f"CAST({c('0', 0)} AS Decimal(154, 154)), accurateCastOrNull({c('1', 0)}, 'Decimal(154, 154)'), toInt64({cn}), toInt64({c('0.5', 154)})", "0\t\\N\t0\t0")
err(f"CAST({c('1', 0)} AS Decimal(154, 154))")
err(f"CAST(materialize({c('-1', 0)}) AS Decimal(154, 154))")
err("CAST(toInt32(1) AS Decimal(154, 154))")
q(f"less({c('0.5', 154)}, 1), less(materialize({c('0.5', 154)}), 1), greater({c('-0.5', 154)}, -1), equals({c('0', 154)}, 0)", "1\t1\t1\t1")
q(f"less({cm}, 1), equals({cm}, toInt512(1)), less({c('0.5', 154)}, {c('1', 0)}), greater({c('1', 0)}, materialize({cn}))", "1\t0\t1\t1")
q(f"less(toDecimal128('1', 0), {c('0.5', 154)}), greaterOrEquals(materialize({c('0.5', 154)}), toInt256(-1))", "0\t1")
q(f"plus({c('-0.6', 154)}, 1), plus(materialize({c('-0.6', 154)}), 1), minus({c('0.7', 1)}, {c('0.3', 154)}), minus({c('0.3', 154)}, {c('0.7', 1)})", "0.4\t0.4\t0.4\t-0.4")
q(f"plus({c('0.5', 154)}, {c('0.1', 154)}), divide({c('0.3', 154)}, 2), divide({c('1' + '0' * 39, 60)}, {c('2', 60)})", f"0.6\t0.15\t{5 * 10 ** 38}")
# Decimal / Decimal with scales adding up to more than 154: rejected at analysis time (upstream rule, every width)
err(f"divide({c('0.01', 2)}, {c('0.5', 154)})")
err(f"divide({c('0.3', 154)}, {c('0.5', 154)})")
q(f"multiply({c('0.5', 77)}, {c('0.5', 77)}), least({c('0.5', 154)}, 1), greatest({c('-0.5', 154)}, -1)", "0.25\t0.5\t-0.5")
err(f"plus({c('0.5', 154)}, 1)")
err(f"greatest({c('0.5', 154)}, 1)")
q(f"round({c('0.4', 154)}), floor({c('0.4', 154)}), ceil({c('-0.4', 154)}), trunc({c('0.6', 154)}), trunc(materialize({cn})), roundBankers({c('0.5', 154)})", "0\t0\t0\t0\t0\t0")
q(f"round({c('0.123456', 154)}, 3), round({c(str(6 * 10 ** 153 + 1), 0)}, -153)", f"0.123\t{6 * 10 ** 153}")
err(f"round({c('0.6', 154)})")
err(f"floor({c('-0.4', 154)})")
err(f"round({c(str(I_MAX), 0)}, -153)")
q(f"isDecimalOverflow({cm}), isDecimalOverflow({c(str(I_MIN), 0)})", "0\t0")
rows.append(("DROP TABLE IF EXISTS t_10316;", None))
rows.append(("CREATE TABLE t_10316 (k UInt8, d Decimal(154, 154)) ENGINE = MergeTree ORDER BY d;", None))
rows.append((f"INSERT INTO t_10316 VALUES (1, '0.5'), (2, '-0.25'), (3, '{fmt(I_MAX, 154)}'), (4, '{fmt(I_MIN, 154)}'), (5, '0');", None))
vals = sorted([(1, H), (2, raw("-0.25", 154)), (3, I_MAX), (4, I_MIN), (5, 0)], key=lambda x: x[1])
rows.append(("SELECT k, d, d < 1, d > -1, toInt8(d) FROM t_10316 ORDER BY d;", "\n".join(f"{k}\t{fmt(v, 154)}\t1\t1\t0" for k, v in vals)))
rows.append(("SELECT count() FROM t_10316 WHERE d < 1 AND d > -1 AND d > toDecimal512('-0.3', 1);", "4"))
rows.append(("SELECT sum(d), min(d), max(d) FROM t_10316 WHERE k <= 2;", f"{fmt(H + raw('-0.25', 154), 154)}\t-0.25\t0.5"))
rows.append(("DROP TABLE t_10316;", None))
write("10316_decimal512_scale154", "-- Decimal512 keeps precision and scale 154, but 10^154 > 2^511 - 1 (about 6.7e153): the scale multiplier of scale 154\n-- does not fit Int512 and used to wrap to a negative number, which broke printing, float conversion, casts to and from\n-- scale 0, comparisons with integers, arithmetic, rounding, avg and isDecimalOverflow of Decimal(154, 154). The expected\n-- values are the mathematical ones; DECIMAL_OVERFLOW only where the exact result does not fit.", rows)
print("written 7 tests to", OUT)
