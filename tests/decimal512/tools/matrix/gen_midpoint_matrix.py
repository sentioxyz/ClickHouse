#!/usr/bin/env python3
"""Generate the Decimal512 mixed-type midpoint/avg2 matrix and its independent oracle.

Oracle rules (ClickHouse port semantics, cited from source):
  * result type = getLeastSupertype (src/DataTypes/getLeastSupertype.cpp:841-905):
      Decimal + {Int8..UInt64}: scale = max decimal scale; precision = scale + leastDecimalPrecisionFor(max_int)
      (Int8/UInt8 3, Int16/UInt16 5, Int32/UInt32 10, Int64 19, UInt64 20; max_int is the LAST present in
      order Int8,UInt8,Int16,UInt16,Int32,UInt32,Int64,UInt64); scale 0 + Int32 -> P 9, scale 0 + Int64 -> P 18;
      Decimal512 if any Decimal512 or P > 76, else Decimal256 if any Decimal256 or P > 38, ... ;
      P > 154 -> NO_COMMON_TYPE; Decimal + {Int128, Int256, UInt128, UInt256, Int512, UInt512, Float*} -> NO_COMMON_TYPE.
  * value (src/Functions/midpoint.h calculateForDecimalType): each argument is cast to the result
    type, the scaled integers are summed, and the sum is divided by the number of non-NULL arguments with
    truncation toward zero; NULL arguments are ignored; all-NULL -> NULL.
"""
import itertools, json, sys
from decimal import Decimal, getcontext
getcontext().prec = 400

INT_DIGITS = {"Int8": 3, "UInt8": 3, "Int16": 5, "UInt16": 5, "Int32": 10, "UInt32": 10, "Int64": 19, "UInt64": 20}
INT_ORDER = ["Int8", "UInt8", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64"]
INT_RANGE = {"Int8": (-2**7, 2**7 - 1), "UInt8": (0, 2**8 - 1), "Int16": (-2**15, 2**15 - 1), "UInt16": (0, 2**16 - 1),
             "Int32": (-2**31, 2**31 - 1), "UInt32": (0, 2**32 - 1), "Int64": (-2**63, 2**63 - 1), "UInt64": (0, 2**64 - 1)}
NO_DECIMAL_SUPERTYPE = ["Int128", "UInt128", "Int256", "UInt256", "Int512", "UInt512", "Float32", "Float64", "BFloat16"]
MAXP = {"Decimal32": 9, "Decimal64": 18, "Decimal128": 38, "Decimal256": 76, "Decimal512": 154}
ORDER = ["Decimal32", "Decimal64", "Decimal128", "Decimal256", "Decimal512"]


def dec_type(width, scale):
    return (f"Decimal{width}", MAXP[f"Decimal{width}"], scale)


def supertype(args):
    """args: list of ('dec', width, P, S) | ('int', name) -> ('dec', width, P, S) or ('error', 'NO_COMMON_TYPE')"""
    decs = [a for a in args if a[0] == "dec"]
    ints = [a[1] for a in args if a[0] == "int"]
    others = [a for a in args if a[0] not in ("dec", "int")]
    if others:
        return ("error", "NO_COMMON_TYPE")
    max_scale = max(a[3] for a in decs)
    max_int = None
    for name in INT_ORDER:
        if name in ints:
            max_int = name
    p = max_scale + (INT_DIGITS[max_int] if max_int else 0)
    if max_scale == 0 and max_int == "Int32":
        p = 9
    elif max_scale == 0 and max_int == "Int64":
        p = 18
    if p > 154:
        return ("error", "NO_COMMON_TYPE")
    widths = {a[1] for a in decs}
    if 512 in widths or p > 76:
        return ("dec", 512, 154, max_scale)
    if 256 in widths or p > 38:
        return ("dec", 256, 76, max_scale)
    if 128 in widths or p > 18:
        return ("dec", 128, 38, max_scale)
    if 64 in widths or p > 9:
        return ("dec", 64, 18, max_scale)
    return ("dec", 32, 9, max_scale)


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def fmt_decimal(scaled, scale):
    """ClickHouse text output for Decimal with output_format_decimal_trailing_zeros = 0."""
    neg = scaled < 0
    s = str(abs(scaled))
    if scale > 0:
        s = s.rjust(scale + 1, "0")
        ip, fp = s[:-scale], s[-scale:].rstrip("0")
        s = ip + ("." + fp if fp else "")
    return ("-" if neg and s not in ("0",) else "") + s


def sql_type(a):
    if a[0] == "dec":
        return f"Decimal({a[2]}, {a[3]})"
    return a[1]


def literal(a, v):
    """exact SQL literal of value v (Decimal or int or None) for argument type a"""
    t = sql_type(a)
    if v is None:
        return f"CAST(NULL AS Nullable({t}))"
    if a[0] == "dec":
        return f"CAST('{v}' AS {t})"
    return f"CAST('{v}' AS {t})"


def expected(args, vals):
    st = supertype(args)
    if st[0] == "error":
        return {"error": st[1]}
    scale = st[3]
    tot, cnt = 0, 0
    for a, v in zip(args, vals):
        if v is None:
            continue
        d = Decimal(v)
        scaled = int(d.scaleb(scale).to_integral_value())  # exact: inputs have scale <= result scale
        tot += scaled
        cnt += 1
    nullable = any(v is None for v in vals)
    tname = f"Decimal({st[2]}, {st[3]})"
    if nullable:
        tname = f"Nullable({tname})"
    if cnt == 0:
        return {"type": tname, "value": "\\N"}
    res = trunc_div(tot, cnt)
    limit = 2**511 if st[1] == 512 else 2**(st[1] - 1)
    if not (-limit <= tot < limit):
        return {"type": tname, "overflow": True}
    return {"type": tname, "value": fmt_decimal(res, scale)}


D512 = [(0, ["0", "1", "-1", "123456789012345678901234567890", "-99999999999999999999"]),
        (2, ["0", "1.5", "-2.25", "0.01", "-0.01", "12345678901234567890.99"]),
        (65, ["0", "1", "-1.5", "0.00000000000000000000000000000000000000000000000000000000000000001"]),
        (100, ["0", "1", "-0.5", "3.1415926535897932384626433832795028841971693993751058209749445923078164062862"])]
OTHER_DECS = [(32, 4, ["0", "1.2345", "-99999.9999"]), (64, 10, ["0", "-1.0000000001", "12345678.9"]),
              (128, 20, ["0", "1.00000000000000000001"]), (256, 40, ["0", "-1.5", "999999999999999999999999999999999999.9999999999999999999999999999999999999999"]),
              (256, 65, ["1", "-0.5"])]


def cases():
    out = []
    cid = 0
    for (s, dvals) in D512:
        d = ("dec", 512, 154, s)
        # Decimal512 x signed/unsigned integers (both orders; includes extremes and negatives)
        for name in INT_ORDER:
            lo, hi = INT_RANGE[name]
            for dv in dvals[:3]:
                for iv in sorted({0, 1, lo, hi} | ({-1} if lo < 0 else set())):
                    for order in (0, 1):
                        args = [d, ("int", name)] if order == 0 else [("int", name), d]
                        vals = [dv, str(iv)] if order == 0 else [str(iv), dv]
                        out.append((f"m{cid:05d}", args, vals)); cid += 1
        # Decimal512 x other decimals
        for (w, os_, ovals) in OTHER_DECS:
            o = ("dec", w, MAXP[f"Decimal{w}"], os_)
            for dv in dvals[:2]:
                for ov in ovals:
                    for order in (0, 1):
                        args = [d, o] if order == 0 else [o, d]
                        vals = [dv, ov] if order == 0 else [ov, dv]
                        out.append((f"m{cid:05d}", args, vals)); cid += 1
        # Decimal512 x Decimal512 of another scale
        for (s2, dvals2) in D512:
            if s2 == s:
                continue
            d2 = ("dec", 512, 154, s2)
            for dv in dvals[:2]:
                for dv2 in dvals2[:2]:
                    out.append((f"m{cid:05d}", [d, d2], [dv, dv2])); cid += 1
        # expected rejections
        for name in NO_DECIMAL_SUPERTYPE:
            for order in (0, 1):
                args = [d, ("other", name)] if order == 0 else [("other", name), d]
                vals = [dvals[1], "1"] if order == 0 else ["1", dvals[1]]
                out.append((f"m{cid:05d}", args, vals)); cid += 1
        # NULLs (ignored per row; all-NULL -> NULL)
        for name in ("Int64", "UInt8"):
            out.append((f"m{cid:05d}", [d, ("int", name)], [None, "7"])); cid += 1
            out.append((f"m{cid:05d}", [("int", name), d], ["7", None])); cid += 1
            out.append((f"m{cid:05d}", [d, ("int", name)], [None, None])); cid += 1
            out.append((f"m{cid:05d}", [d, ("int", name)], [dvals[1], None])); cid += 1
    # the Decimal256 -> Decimal512 promotion that motivated 4d99aa0e0af
    d256 = ("dec", 256, 76, 65)
    for name in INT_ORDER:
        lo, hi = INT_RANGE[name]
        for iv in (0, 1, lo, hi):
            for order in (0, 1):
                args = [d256, ("int", name)] if order == 0 else [("int", name), d256]
                vals = ["1", str(iv)] if order == 0 else [str(iv), "1"]
                out.append((f"m{cid:05d}", args, vals)); cid += 1
    return out


def render(c):
    cid, args, vals = c
    lits = []
    for a, v in zip(args, vals):
        if a[0] == "other":
            t = a[1]
            lits.append(f"CAST('{v}' AS {t})")
        else:
            lits.append(literal(a, v))
    expr = f"midpoint({', '.join(lits)})"
    return f"SELECT '{cid}', toTypeName({expr}), {expr};"


if __name__ == "__main__":
    cs = cases()
    with open(sys.argv[1], "w") as sql, open(sys.argv[2], "w") as orc:
        for c in cs:
            cid, args, vals = c
            if any(a[0] == "other" for a in args):
                exp = {"error": "NO_COMMON_TYPE"}
            else:
                exp = expected(args, vals)
            sql.write(render(c) + "\n")
            orc.write(json.dumps({"id": cid, "category": "midpoint-reject" if "error" in exp else "midpoint-dec512",
                                  "args": [sql_type(a) if a[0] != "other" else a[1] for a in args], "vals": vals, "expected": exp}) + "\n")
    print(len(cs), "cases")
