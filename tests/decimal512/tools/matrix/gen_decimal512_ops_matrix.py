#!/usr/bin/env python3
"""Decimal512 operations matrix with an independent oracle.

Extends the midpoint matrix instead of replacing it: the value sets (G.D512, G.OTHER_DECS, G.INT_ORDER/INT_RANGE),
the literal rendering (G.literal/G.sql_type) and the decimal text format (G.fmt_decimal) are imported from
gen_midpoint_matrix.py. What is added is the operation dimension and its oracle.

Dimensions: operation (plus, minus, multiply, less, equals) x Decimal512 scale (0, 2, 65, 100) x other operand
(signed/unsigned integers 8..64 bit with 0/-1/extremes, other decimals 32..256 bit, Decimal512 of other scales,
Float64 with exactly representable values) x operand order x NULL x constant/vector form; plus boundary rows at the
Int512 limit of Decimal512, and Int512/UInt512 used as integers (arithmetic and wrap-around, common supertype,
avg2/midpoint), each next to the same query on (U)Int256 as a control.

Oracle rules, derived from the 26.3 fork source:
  * result type (src/Core/DecimalFunctions.h:449-475 binaryOpResult; FunctionBinaryArithmetic.h:175-176,202-203):
      Decimal op Decimal: wider of the two widths, precision = max precision of that width,
                          scale = sA + sB for multiply, max(sA, sB) otherwise; scale > precision -> ARGUMENT_OUT_OF_BOUND
                          (DataTypeDecimalBase.h:84)
      Decimal op integer: the decimal's type (width, max precision, scale), both operand orders
      Decimal op Float:   Float64
      comparisons:        UInt8 (Decimal vs Float compares as Float64, FunctionsComparison.h:1492)
      Decimal -> Float64: Float64(scaled integer) / Float64(10^scale) (DecimalFunctions.h convertToImpl), so a
                          Decimal(154, 65) holding -1.5 converts to -1.5000000000000002, exactly as Decimal256 does
      any NULL operand:   Nullable(result), value NULL
  * value: exact rational arithmetic on scaled integers; plus/minus scale the narrower operand up first
  * overflow (decimal_check_overflow = 1, Settings.cpp:3806; FunctionBinaryArithmetic.h:741-782): an operand scale-up
    or the result leaving the native integer range of the result width (Int32..Int512) -> DECIMAL_OVERFLOW.
    A silently wrapped value is a FAIL, whatever the binary does. Note: upstream documents that these checks are not
    implemented for Decimal128/Decimal256 (docs/en/sql-reference/data-types/decimal.md:70-72; official 26.8.8.8
    wraps Decimal128/256 multiply, rejects Decimal32/64 multiply overflow). The Decimal512 multiply rows expect the
    Decimal32/64 behaviour; whether Decimal512 must be stricter than upstream's wide decimals is a user decision
    (tests/known_defects.json KD-D512-MUL-OVERFLOW), so these rows are a known-defect class, not a hidden xfail.
  * integers wrap around (two's complement) on overflow, like Int64 does; Int512 must behave like Int64/Int256.
  * a string literal that does not fit the native range is rejected (error_any: the exact code is not pinned).
  * (U)Int512 as an integer behaves like (U)Int256 with the width doubled: same result type rule (NumberTraits.h;
    e.g. unsigned - unsigned -> signed of the same width), wrap-around,
    intDiv truncates toward zero, the common supertype ladder (getLeastSupertype.cpp) continues 256 -> 512, and
    avg2/midpoint of two integers truncates toward zero without overflow (midpoint.h MidpointImpl). This is the
    expectation of the "int512-*"/"midpoint-int512" rows; the "*-control" rows run the same rule on (U)Int256 and
    must pass on every binary, which is what makes the rule an oracle and not a guess. Whether Int512 arithmetic
    should exist at all is a product decision: until it is made and implemented, these rows fail, visibly.

  gen_decimal512_ops_matrix.py <out.sql> <out.oracle.jsonl>
Ids: m2xxxx constant form, m4xxxx vector form (same case), m8xxxx boundary and integer rows. Every oracle row has a
"category" so a run can report known-defect classes separately instead of hiding them: arith, compare, float, null,
boundary, parse-boundary, int512-arith, int512-supertype, midpoint-int512, and the controls int-wrap-control,
int-supertype-control, midpoint-int-control.
"""
import json
import os
import sys
from decimal import Decimal, getcontext

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_midpoint_matrix as G  # noqa: E402

getcontext().prec = 400
OPS = ["plus", "minus", "multiply", "less", "equals"]
FLOAT_VALUES = ["0", "1.5", "-2.25"]
OVERFLOW_CODES = ["DECIMAL_OVERFLOW"]
PARSE_REJECT_CODES = ["DECIMAL_OVERFLOW", "ARGUMENT_OUT_OF_BOUND", "CANNOT_PARSE_NUMBER", "CANNOT_PARSE_TEXT", "CANNOT_CONVERT_TYPE"]


def native_ok(v: int, width: int) -> bool:
    lim = 2 ** (width - 1)
    return -lim <= v < lim


def scaled(v: str, s: int) -> int:
    d = Decimal(v).scaleb(s)
    assert d == d.to_integral_value(), (v, s)
    return int(d)


def fmt_float(f: float) -> str:
    if f == 0:
        return "-0" if str(f).startswith("-") else "0"
    if f == int(f) and abs(f) < 1e15:
        return str(int(f))
    return repr(f)


def type_name(t, nullable: bool) -> str:
    base = {"uint8": "UInt8", "float64": "Float64"}.get(t[0]) or (f"Decimal({t[2]}, {t[3]})" if t[0] == "dec" else t[1])
    return f"Nullable({base})" if nullable else base


def result_type(op, a, b):
    if op in ("less", "equals"):
        return ("uint8",)
    if a[0] == "float" or b[0] == "float":
        return ("float64",)
    if a[0] == "dec" and b[0] == "dec":
        w = max(a[1], b[1])
        p = G.MAXP[f"Decimal{w}"]
        s = a[3] + b[3] if op == "multiply" else max(a[3], b[3])
        if s > p:
            return ("error", "ARGUMENT_OUT_OF_BOUND")
        return ("dec", w, p, s)
    d = a if a[0] == "dec" else b
    return ("dec", d[1], d[2], d[3])


def exact(t, v):
    return Decimal(v) if t[0] in ("dec", "int", "float") else None


def to_float(t, v):
    """Float64 value ClickHouse computes for an operand (decimals via convertToImpl)."""
    if t[0] == "dec":
        return float(scaled(v, t[3])) / float(10 ** t[3])
    return float(Decimal(v))


def expected(op, a, va, b, vb):
    rt = result_type(op, a, b)
    if rt[0] == "error":
        return {"error": rt[1]}
    nullable = va is None or vb is None
    if nullable:
        return {"type": type_name(rt, True), "value": "\\N"}
    if rt[0] == "uint8":
        x, y = exact(a, va), exact(b, vb)
        if a[0] == "float" or b[0] == "float":
            x, y = to_float(a, va), to_float(b, vb)
        r = (x < y) if op == "less" else (x == y)
        return {"type": "UInt8", "value": "1" if r else "0"}
    if rt[0] == "float64":
        x, y = to_float(a, va), to_float(b, vb)
        r = {"plus": x + y, "minus": x - y, "multiply": x * y}[op]
        return {"type": "Float64", "value": fmt_float(r)}
    w, s = rt[1], rt[3]
    if op == "multiply":
        sa = scaled(va, a[3]) if a[0] == "dec" else int(va)
        sb = scaled(vb, b[3]) if b[0] == "dec" else int(vb)
        if not (native_ok(sa, w) and native_ok(sb, w)):
            return {"error_any": OVERFLOW_CODES}
        r = sa * sb
    else:
        ua = scaled(va, a[3]) * 10 ** (s - a[3]) if a[0] == "dec" else int(va) * 10 ** s
        ub = scaled(vb, b[3]) * 10 ** (s - b[3]) if b[0] == "dec" else int(vb) * 10 ** s
        if not (native_ok(ua, w) and native_ok(ub, w)):
            return {"error_any": OVERFLOW_CODES}
        r = ua + ub if op == "plus" else ua - ub
    if not native_ok(r, w):
        return {"error_any": OVERFLOW_CODES}
    return {"type": type_name(rt, False), "value": G.fmt_decimal(r, s)}


def lit(t, v, vector):
    if t[0] == "float":
        x = "CAST(NULL AS Nullable(Float64))" if v is None else f"CAST('{v}' AS Float64)"
    else:
        x = G.literal(t, v)
    return f"materialize({x})" if vector else x


def render(cid, op, a, va, b, vb, vector):
    expr = f"{op}({lit(a, va, vector)}, {lit(b, vb, vector)})"
    tail = " FROM numbers(3) LIMIT 1 BY 1" if vector else ""
    return f"SELECT '{cid}', toTypeName({expr}), {expr}{tail};"


def base_cases():
    out = []
    for (s, dvals) in G.D512:
        d = ("dec", 512, 154, s)
        for op in OPS:
            for name in G.INT_ORDER:
                lo, hi = G.INT_RANGE[name]
                for dv in dvals[:2]:
                    for iv in sorted({0, lo, hi} | ({-1} if lo < 0 else set())):
                        out.append(("arith" if op not in ("less", "equals") else "compare", op, d, dv, ("int", name), str(iv)))
                        out.append(("arith" if op not in ("less", "equals") else "compare", op, ("int", name), str(iv), d, dv))
            for (w, os_, ovals) in G.OTHER_DECS:
                o = ("dec", w, G.MAXP[f"Decimal{w}"], os_)
                for dv in dvals[:2]:
                    for ov in ovals[:2]:
                        cat = "arith" if op not in ("less", "equals") else "compare"
                        out.append((cat, op, d, dv, o, ov))
                        out.append((cat, op, o, ov, d, dv))
            for (s2, dvals2) in G.D512:
                if s2 == s:
                    continue
                d2 = ("dec", 512, 154, s2)
                for dv in dvals[:2]:
                    for dv2 in dvals2[:2]:
                        out.append(("arith" if op not in ("less", "equals") else "compare", op, d, dv, d2, dv2))
            for dv in dvals[:3]:
                for fv in FLOAT_VALUES:
                    out.append(("float", op, d, dv, ("float", "Float64"), fv))
                    out.append(("float", op, ("float", "Float64"), fv, d, dv))
            for name in ("Int64", "UInt8"):
                out.append(("null", op, d, None, ("int", name), "7"))
                out.append(("null", op, ("int", name), "7", d, None))
                out.append(("null", op, d, dvals[1], ("int", name), None))
    return out


def boundary_rows():
    """(category, sql_expression, expected) at the Int512 limit of Decimal512 and integer wrap-around."""
    rows = []
    for s in (0, 2, 65, 100):
        t = f"Decimal(154, {s})"
        mx, mn, ulp = G.fmt_decimal(2 ** 511 - 1, s), G.fmt_decimal(-2 ** 511, s), G.fmt_decimal(1, s)
        c = lambda v: f"CAST('{v}' AS {t})"
        d = ("dec", 512, 154, s)
        for op, x, y in (("plus", mx, "0"), ("plus", mx, ulp), ("minus", mn, ulp), ("minus", mx, mx), ("plus", mn, mx)):
            rows.append(("boundary", f"{op}({c(x)}, {c(y)})", expected(op, d, x, d, y)))
        for iv in ("1", "-1", "2"):
            rows.append(("boundary", f"multiply({c(mx)}, toInt8({iv}))", expected("multiply", d, mx, ("int", "Int8"), iv)))
        rows.append(("boundary", f"multiply({c(mn)}, toInt8(-1))", expected("multiply", d, mn, ("int", "Int8"), "-1")))
        rows.append(("boundary", f"less({c(mn)}, {c(mx)})", expected("less", d, mn, d, mx)))
        # literals just outside the native range must be rejected, never wrapped
        rows.append(("parse-boundary", f"CAST('{G.fmt_decimal(2 ** 511, s)}' AS {t})", {"error_any": PARSE_REJECT_CODES}))
        rows.append(("parse-boundary", f"CAST('{G.fmt_decimal(-2 ** 511 - 1, s)}' AS {t})", {"error_any": PARSE_REJECT_CODES}))
    nines153 = "9" * 153
    d0 = ("dec", 512, 154, 0)
    rows.append(("boundary", f"plus(CAST('{nines153}' AS Decimal(154, 0)), CAST('{nines153}' AS Decimal(154, 0)))", expected("plus", d0, nines153, d0, nines153)))
    rows.append(("boundary", f"multiply(CAST('{nines153}' AS Decimal(154, 0)), toInt8(10))", expected("multiply", d0, nines153, ("int", "Int8"), "10")))
    ten77 = "1" + "0" * 77
    rows.append(("boundary", f"multiply(CAST('{ten77}' AS Decimal(154, 0)), CAST('{ten77}' AS Decimal(154, 0)))", expected("multiply", d0, ten77, d0, ten77)))
    # integer types wrap around; Int512 must do what Int64 and Int256 do
    for name, bits in (("Int64", 64), ("Int256", 256), ("Int512", 512)):
        mx, mn = 2 ** (bits - 1) - 1, -2 ** (bits - 1)
        wrap = lambda v: (v + 2 ** (bits - 1)) % 2 ** bits - 2 ** (bits - 1)
        for op, x, y in (("plus", mx, 1), ("minus", mn, 1), ("multiply", mx, 2)):
            val = wrap({"plus": x + y, "minus": x - y, "multiply": x * y}[op])
            cat = "int512-arith" if bits == 512 else "int-wrap-control"
            rows.append((cat, f"{op}(to{name}('{x}'), to{name}('{y}'))", {"type": name, "value": str(val)}))
    rows += int512_rows()
    rows += wide_rows()
    return rows


def trunc_half(v: int) -> int:
    """v / 2 truncated toward zero (C++ integer division)."""
    return v // 2 if v >= 0 else -((-v) // 2)


def int512_rows():
    """(U)Int512 as integers, each query also on (U)Int256 as its control (see the module docstring)."""
    rows = []
    for w in (256, 512):
        I, U = f"Int{w}", f"UInt{w}"
        arith, sup, mid = (("int-wrap-control", "int-supertype-control", "midpoint-int-control") if w == 256
                           else ("int512-arith", "int512-supertype", "midpoint-int512"))
        imax, imin, umax = 2 ** (w - 1) - 1, -2 ** (w - 1), 2 ** w - 1
        # unsigned wrap-around, intDiv/modulo truncation, mixed widths (the wider type wins, like Int256 op Int8)
        for expr, typ, val in (
            (f"plus(to{U}('{umax}'), to{U}('1'))", U, 0),
            # unsigned - unsigned is SIGNED of the same width from 8 bytes up (NumberTraits.h:32-37 nextSize,
            # 83-89 ResultOfSubtraction): UInt256 - UInt256 -> Int256 on the official 26.8.8.8 build as well
            (f"minus(to{U}('0'), to{U}('1'))", I, -1),
            (f"multiply(to{U}('{umax}'), to{U}('2'))", U, umax - 1),
            (f"intDiv(to{I}('-7'), to{I}('2'))", I, -3),
            (f"modulo(to{I}('-7'), to{I}('2'))", I, -1),
            (f"plus(to{I}('5'), toInt8(-7))", I, -2),
            (f"multiply(to{U}('3'), toUInt8(4))", U, 12),
            (f"minus(materialize(to{I}('{imin + 1}')), to{I}('1'))", I, imin),
        ):
            rows.append((arith, expr, {"type": typ, "value": str(val)}))
        # common supertype: the ladder continues to the 512-bit rung instead of falling back to Variant
        for expr, typ, val in (
            (f"if(materialize(1) = 1, to{I}('-5'), toInt8(3))", I, "-5"),
            (f"arrayElement([to{I}('-5'), toInt8(3)], 2)", I, "3"),
            (f"if(materialize(0) = 1, to{U}('7'), toUInt8(3))", U, "3"),
            (f"if(materialize(1) = 1, to{I}('-5'), toUInt64(3))", I, "-5"),
            (f"coalesce(CAST(NULL AS Nullable({I})), toInt8(5))", I, "5"),
            (f"greatest(to{I}('-5'), toInt8(3))", I, "3"),
            (f"least(to{U}('7'), toUInt16(3))", U, "3"),
        ):
            rows.append((sup, expr, {"type": typ, "value": val}))
        # avg2/midpoint of two integers: truncation toward zero, no overflow at the extremes
        for fn in ("avg2", "midpoint"):
            for a, b in ((1, 3), (1, 2), (-3, 0), (-3, -4), (-1, 2), (imax, imax), (imin, imin), (imax, imin), (imin, -1)):
                rows.append((mid, f"{fn}(to{I}('{a}'), to{I}('{b}'))", {"type": I, "value": str(trunc_half(a + b))}))
            for a, b in ((0, 1), (1, 2), (umax, umax), (umax, umax - 1)):
                rows.append((mid, f"{fn}(to{U}('{a}'), to{U}('{b}'))", {"type": U, "value": str((a + b) // 2)}))
            rows.append((mid, f"{fn}(materialize(to{I}('-3')), to{I}('0'))", {"type": I, "value": "-1"}))
    return rows


def wide_rows():
    """Rows added in 2026-09-25 (appended so that every earlier case id stays the same):
    scale-overflow   an operand scale-up of Decimal512 (plus/minus/compare/divide/CAST between scales) that leaves the
                     Int512 range. Builds without the Int512 multiplication check wrapped silently. The expectation is
                     the exact result (FunctionBinaryArithmetic.h applyScaledWide/applyScaledDivWide compute such rows
                     in 1024 bits; DecimalComparison.h decides comparisons exactly), and DECIMAL_OVERFLOW only where the
                     exact result does not fit (always for CAST). "scale-overflow-control" runs the same shapes on
                     Decimal64, where upstream's rule (DECIMAL_OVERFLOW for any scale-up overflow) stays, on every binary
    convert-wide     (U)Int256/(U)Int512 -> Decimal conversions: the exact value, or DECIMAL_OVERFLOW when it does not fit
                     (accurateCastOrNull: NULL); "convert-wide-control" runs (U)Int256 -> Decimal128/256 on every binary
    parse-boundary   more text inputs at the Int512 limit (154 digits, a scaled 153-digit value, vector form, OrNull)
    scale-154        Decimal(154, 154): 10^154 does not fit Int512, so the scale multiplier of scale 154 cannot be
                     represented (it wrapped to a negative number); the expectation is the mathematical value, and
                     DECIMAL_OVERFLOW only where the exact result does not fit (known_defects.json KD-D512-SCALE-154;
                     tools/scale154/probe_scale154.py covers every path per binary)"""
    rows = []
    d = lambda v, s: f"CAST('{G.fmt_decimal(v, s) if isinstance(v, int) else v}' AS Decimal(154, {s}))"
    ovf = {"error_any": OVERFLOW_CODES}
    dec_t = lambda s: f"Decimal(154, {s})"
    e60, e50 = 10 ** 60, 10 ** 50
    half100 = 5 * 10 ** 99  # 0.5 at scale 100
    so = "scale-overflow"
    rows += [
        (so, f"plus({d(e60, 0)}, {d(half100, 100)})", ovf),
        (so, f"minus({d(half100, 100)}, {d(e60, 0)})", ovf),
        (so, f"plus(materialize({d(e60, 0)}), {d(half100, 100)})", ovf),
        (so, f"plus({d(e50, 0)}, {d(half100, 100)})", {"type": dec_t(100), "value": G.fmt_decimal(e50 * 10 ** 100 + half100, 100)}),
        # 7e60 * 10^100 wraps to a negative Int512: builds without the check answer 1, the exact answer is 0
        (so, f"less({d(7 * e60, 0)}, {d(half100, 100)})", {"type": "UInt8", "value": "0"}),
        (so, f"less(materialize({d(7 * e60, 0)}), {d(half100, 100)})", {"type": "UInt8", "value": "0"}),
        (so, f"equals({d(e60, 0)}, {d(half100, 100)})", {"type": "UInt8", "value": "0"}),
        (so, f"less({d(e50, 0)}, {d(half100, 100)})", {"type": "UInt8", "value": "0"}),
        (so, f"less({d(5 * 10 ** 149, 150)}, toInt64(10000))", {"type": "UInt8", "value": "1"}),
        (so, f"less({d(5 * 10 ** 149, 150)}, toInt64(1000))", {"type": "UInt8", "value": "1"}),
        (so, f"divide({d(10 ** 153, 1)}, {d(1, 1)})", ovf),
        (so, f"divide({d(10 ** 152, 1)}, {d(1, 1)})", {"type": dec_t(1), "value": G.fmt_decimal(10 ** 153, 1)}),
        (so, f"CAST({d(e60, 0)} AS Decimal(154, 100))", ovf),
        (so, f"CAST(materialize({d(e60, 0)}) AS Decimal(154, 100))", ovf),
        (so, f"CAST({d(e50, 0)} AS Decimal(154, 100))", {"type": dec_t(100), "value": G.fmt_decimal(e50 * 10 ** 100, 100)}),
    ]
    c64 = lambda v, s: f"toDecimal64('{v}', {s})"
    sc = "scale-overflow-control"
    rows += [
        (sc, f"plus({c64('100000000000', 0)}, {c64('0.5', 10)})", ovf),
        (sc, f"minus({c64('0.5', 10)}, {c64('100000000000', 0)})", ovf),
        (sc, f"plus({c64('100000', 0)}, {c64('0.5', 10)})", {"type": "Decimal(18, 10)", "value": "100000.5"}),
        (sc, f"less({c64('100000000000', 0)}, {c64('0.5', 10)})", ovf),
        (sc, f"equals({c64('100000000000', 0)}, {c64('0.5', 10)})", ovf),
        (sc, f"less({c64('0.5', 17)}, toInt64(1000))", ovf),
        (sc, f"less({c64('0.5', 15)}, toInt64(1000))", {"type": "UInt8", "value": "1"}),
        (sc, f"divide({c64('99999999999999999.9', 1)}, {c64('0.1', 1)})", ovf),
        (sc, f"divide({c64('9999999999999999.9', 1)}, {c64('0.1', 1)})", {"type": "Decimal(18, 1)", "value": "99999999999999999"}),
        (sc, f"CAST({c64('100000000000', 0)} AS Decimal(18, 10))", ovf),
    ]
    imax, imin, e90 = 2 ** 511 - 1, -2 ** 511, 10 ** 90
    cw = "convert-wide"
    rows += [
        (cw, f"toDecimal512(toUInt512('{imax + 1}'), 0)", ovf),
        (cw, f"toDecimal512(materialize(toUInt512('{imax + 1}')), 0)", ovf),
        (cw, f"toDecimal512(toUInt512('{imax}'), 0)", {"type": dec_t(0), "value": str(imax)}),
        (cw, f"toDecimal512(materialize(toUInt512('{imax}')), 0)", {"type": dec_t(0), "value": str(imax)}),
        (cw, f"toDecimal512(toInt512('{imin}'), 0)", {"type": dec_t(0), "value": str(imin)}),
        (cw, f"toDecimal512(toInt512('{e90}'), 3)", {"type": dec_t(3), "value": G.fmt_decimal(e90 * 1000, 3)}),
        (cw, f"toDecimal512(toInt512('{10 ** 150}'), 5)", ovf),
        (cw, f"toDecimal512(materialize(toInt512('{10 ** 150}')), 5)", ovf),
        (cw, f"toDecimal512(toUInt256('{2 ** 256 - 1}'), 0)", {"type": dec_t(0), "value": str(2 ** 256 - 1)}),
        # CAST from a big integer to a decimal is not supported upstream either (Int128/Int256 -> Decimal raise
        # CANNOT_CONVERT_TYPE on official 26.8.8.8); toDecimalX() is the conversion path for them
        (cw, f"CAST(toInt512('{e90}') AS Decimal(154, 3))", {"error": "CANNOT_CONVERT_TYPE"}),
        (cw, f"toDecimal256(toInt512('{e90}'), 0)", ovf),
        (cw, f"toDecimal256(materialize(toInt512('{e90}')), 0)", ovf),
        (cw, f"toDecimal256(toInt512('-5'), 2)", {"type": "Decimal(76, 2)", "value": "-5"}),
        (cw, f"toDecimal128(toUInt512('{10 ** 30}'), 0)", {"type": "Decimal(38, 0)", "value": str(10 ** 30)}),
        (cw, f"toDecimal64(toInt512('{imin}'), 0)", ovf),
        (cw, f"accurateCastOrNull(toUInt512('{imax + 1}'), 'Decimal(154, 0)')", {"type": "Nullable(Decimal(154, 0))", "value": "\\N"}),
        (cw, f"accurateCastOrNull(materialize(toUInt512('{imax + 1}')), 'Decimal(154, 0)')", {"type": "Nullable(Decimal(154, 0))", "value": "\\N"}),
    ]
    cc = "convert-wide-control"
    rows += [
        (cc, "toDecimal256(toInt256('-5'), 2)", {"type": "Decimal(76, 2)", "value": "-5"}),
        (cc, f"toDecimal128(toInt256('{10 ** 40}'), 0)", ovf),
        (cc, f"toDecimal128(materialize(toInt256('{10 ** 40}')), 0)", ovf),
        (cc, f"toDecimal64(toUInt256('{10 ** 30}'), 0)", ovf),
        (cc, f"toDecimal128(toUInt256('{10 ** 30}'), 0)", {"type": "Decimal(38, 0)", "value": str(10 ** 30)}),
    ]
    pb = "parse-boundary"
    rows += [
        (pb, f"CAST('{'9' * 154}' AS Decimal(154, 0))", {"error_any": PARSE_REJECT_CODES}),
        (pb, f"CAST('{'9' * 153}' AS Decimal(154, 1))", {"error_any": PARSE_REJECT_CODES}),
        (pb, f"CAST(materialize('{imax + 1}') AS Decimal(154, 0))", {"error_any": PARSE_REJECT_CODES}),
        (pb, f"toDecimal512OrNull('{imax + 1}', 0)", {"type": "Nullable(Decimal(154, 0))", "value": "\\N"}),
        (pb, f"toDecimal512OrNull('{imin}', 0)", {"type": "Nullable(Decimal(154, 0))", "value": str(imin)}),
        (pb, f"CAST('{10 ** 152}' AS Decimal(154, 1))", {"type": dec_t(1), "value": str(10 ** 152)}),
    ]
    # intDiv with a decimal operand runs on the decimal path, which has no (U)Int512 case: rejected like plus/minus/
    # multiply/divide (the result-type check used to accept it and execution threw LOGICAL_ERROR); (U)Int256 works
    for q in ("intDiv(toDecimal512('7.5', 1), toInt512('2'))", "intDiv(toInt512('7'), toDecimal512('2.5', 1))",
              "intDivOrZero(toDecimal32('7.5', 1), toUInt512('2'))", "intDiv(materialize(toDecimal256('7.5', 1)), toUInt512('2'))"):
        rows.append(("int512-arith", q, {"error": "ILLEGAL_TYPE_OF_ARGUMENT"}))
    rows.append(("int-wrap-control", "intDiv(toDecimal256('7.5', 1), toInt256('2'))", {"type": "Int256", "value": "3"}))
    # the (U)Int256 rules carried over: a big integer shift amount is not implemented, bitHammingDistance of one width works
    for w, cat in ((512, "int512-arith"), (256, "int-wrap-control")):
        rows.append((cat, f"bitShiftLeft(toInt{w}('7'), toInt128('3'))", {"error": "NOT_IMPLEMENTED"}))
        rows.append((cat, f"bitShiftLeft(toInt{w}('7'), toUInt16(3))", {"type": f"Int{w}", "value": "56"}))
        # BitHammingDistanceImpl: UInt16 for operands of 256 bits and more (a distance up to 512 does not fit UInt8)
        rows.append((cat, f"bitHammingDistance(toInt{w}('7'), toInt{w}('3'))", {"type": "UInt16", "value": "1"}))
    s154 = "scale-154"
    rows += [
        (s154, "toDecimal512('0.5', 154)", {"type": dec_t(154), "value": "0.5"}),
        (s154, "toDecimal512('-0.5', 154)", {"type": dec_t(154), "value": "-0.5"}),
        (s154, "toFloat64(toDecimal512('0.5', 154))", {"type": "Float64", "value": "0.5"}),
        (s154, "CAST(toDecimal512('0.5', 154) AS Decimal(154, 0))", {"type": dec_t(0), "value": "0"}),
        # comparing with an integer needs 1 * 10^154, which leaves Int512: decided exactly
        (s154, "less(toDecimal512('0.5', 154), 1)", {"type": "UInt8", "value": "1"}),
    ]
    # appended 2026-09-25 (scale-154 fix): the scaled intermediate leaves Int512 but the exact result fits
    ex = "scale-overflow"
    q = lambda v, s_: f"CAST('{v}' AS Decimal(154, {s_}))"
    rows += [
        (ex, f"minus({q('0.7', 1)}, {q('0.3', 154)})", {"type": dec_t(154), "value": "0.4"}),
        (ex, f"minus(materialize({q('0.7', 1)}), {q('0.3', 154)})", {"type": dec_t(154), "value": "0.4"}),
        (ex, f"plus({q('-0.6', 154)}, 1)", {"type": dec_t(154), "value": "0.4"}),
        (ex, f"plus({q('0.5', 154)}, 1)", ovf),
        (ex, f"least({q('0.5', 154)}, 1)", {"type": dec_t(154), "value": "0.5"}),
        (ex, f"greatest({q('0.5', 154)}, 1)", ovf),
        # scales adding up to more than 154: rejected at analysis time (upstream rule, every decimal width)
        (ex, f"divide({q('0.01', 2)}, {q('0.5', 154)})", ovf),
        (ex, f"divide(materialize({q('0.01', 2)}), {q('0.5', 154)})", ovf),
        # Decimal(154, 60) division: the dividend is scaled by 10^60 first, which leaves Int512 from 6.7e33 up
        (ex, f"divide({q('1' + '0' * 40, 60)}, {q('2', 60)})", {"type": dec_t(60), "value": "5" + "0" * 39}),
        (ex, f"divide(materialize({q('1' + '0' * 40, 60)}), {q('-2', 60)})", {"type": dec_t(60), "value": "-5" + "0" * 39}),
        (ex, f"divide({q('1' + '0' * 90, 60)}, {q('0.5', 60)})", {"type": dec_t(60), "value": "2" + "0" * 90}),
        (ex, f"divide({q('1' + '0' * 93, 60)}, {q('0.1', 60)})", ovf),
        (ex, f"less(toDecimal128('1', 0), {q('0.5', 154)})", {"type": "UInt8", "value": "0"}),
        (ex, f"greaterOrEquals(materialize({q('0.5', 154)}), toInt256('-1'))", {"type": "UInt8", "value": "1"}),
        (s154, f"toString({q('-0.25', 154)})", {"type": "String", "value": "-0.25"}),
        (s154, f"CAST({q('1', 0)} AS Decimal(154, 154))", ovf),
        (s154, f"toInt64({q('-0.5', 154)})", {"type": "Int64", "value": "0"}),
        (s154, f"trunc({q('0.6', 154)})", {"type": dec_t(154), "value": "0"}),
        (s154, f"round({q('0.6', 154)})", ovf),
    ]
    return rows


def main(sql_path, oracle_path):
    n = 0
    with open(sql_path, "w") as sql, open(oracle_path, "w") as orc:
        base = base_cases()
        for form, offset in (("const", 20000), ("vector", 45000)):
            for i, (cat, op, a, va, b, vb) in enumerate(base):
                cid = f"m{offset + i:05d}"
                sql.write(render(cid, op, a, va, b, vb, form == "vector") + "\n")
                args = [G.sql_type(x) if x[0] != "float" else "Float64" for x in (a, b)]
                orc.write(json.dumps({"id": cid, "category": cat, "form": form, "op": op, "args": args, "vals": [va, vb],
                                      "expected": expected(op, a, va, b, vb)}) + "\n")
                n += 1
        for i, (cat, expr, exp) in enumerate(boundary_rows()):
            cid = f"m{80000 + i:05d}"
            sql.write(f"SELECT '{cid}', toTypeName({expr}), {expr};\n")
            orc.write(json.dumps({"id": cid, "category": cat, "form": "const", "op": expr.split("(")[0], "args": [expr[:60]], "vals": [],
                                  "expected": exp}) + "\n")
            n += 1
    assert len(base_cases()) < 25000
    print(n, "cases")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
