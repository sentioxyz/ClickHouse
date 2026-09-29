#!/usr/bin/env python3
"""gen_random_decimal512.py - fixed-seed random differential cases for Decimal512, with an exact Python oracle.

PRODUCTION BOUNDARY: pure Python; writes two files; no engine, no network.

  gen_random_decimal512.py <matrix.sql> <oracle.jsonl> [--seed N] [--cases N]

Same format as gen_decimal512_ops_matrix.py (run_matrix.py runs it, lock_cases.py pins it). The default seed and case
count are fixed, so the output is deterministic and lockable; other seeds are for exploratory nightly runs.

Operands are Decimal(154, s) values built from text, with scales biased to the edges (0, 1, 60, 76, 77, 100, 152, 153,
154) and values biased to the Int512 limits, powers of ten and their neighbours. The oracle implements the intended
semantics with Python integers and fractions (decimal_check_overflow = 1, the default):
  plus/minus    scale max(sa, sb); exact sum; DECIMAL_OVERFLOW only if the exact result leaves Int512
  multiply      scale sa + sb (kept <= 154); exact product; DECIMAL_OVERFLOW if it leaves Int512
  divide        scale sa (sa >= sb here); truncated a * 10^sb / b; DECIMAL_OVERFLOW if it leaves Int512, and for
                sa + sb > 154 (rejected at analysis time, the upstream rule of every decimal width)
  compare       less/equals/greater/lessOrEquals/greaterOrEquals/notEquals, exact (also against Int64/Int256)
  least/greatest  scale max(sa, sb); DECIMAL_OVERFLOW if the chosen operand, scaled, leaves Int512
  cast          CAST to Decimal(154, t): scale up exactly (DECIMAL_OVERFLOW if it leaves Int512) or truncate
  text          toString (ClickHouse text, no trailing zeros)
  round         round(x, N): half away from zero at 10^(s - N); DECIMAL_OVERFLOW if the result leaves Int512
A share of the cases wraps the first operand in materialize() (vector form).
"""
from __future__ import annotations

import json
import random
import sys
from fractions import Fraction

MAX, MIN = 2 ** 511 - 1, -2 ** 511
OVF = {"error_any": ["DECIMAL_OVERFLOW"]}
EDGE_SCALES = [0, 1, 2, 18, 30, 38, 60, 76, 77, 100, 120, 152, 153, 154]


def fits(v: int) -> bool:
    return MIN <= v <= MAX


def fmt(raw: int, s: int) -> str:
    neg = raw < 0
    a = -raw if neg else raw
    w, f = divmod(a, 10 ** s)
    t = str(w) + ("." + str(f).rjust(s, "0").rstrip("0") if s and f else "")
    return ("-" if neg else "") + t


def dec_t(s: int) -> str:
    return f"Decimal(154, {s})"


def lit(raw: int, s: int) -> str:
    return f"CAST('{fmt(raw, s)}' AS {dec_t(s)})"


def res(raw: int, s: int):
    return {"type": dec_t(s), "value": fmt(raw, s)} if fits(raw) else OVF


def tdiv(a: int, b: int) -> int:
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def rnd_scale(r: random.Random) -> int:
    return r.choice(EDGE_SCALES) if r.random() < 0.7 else r.randint(0, 154)


def rnd_raw(r: random.Random) -> int:
    k = r.random()
    if k < 0.15:
        return r.choice([MAX, MIN, MAX - 1, MIN + 1, 0, 1, -1])
    if k < 0.35:
        e = r.randint(0, 153)
        return r.choice([1, -1]) * (10 ** e + r.choice([-1, 0, 1]))
    if k < 0.55:
        return r.randint(-10 ** 6, 10 ** 6)
    if k < 0.75:
        return r.randint(MIN, MAX)
    return r.randint(MIN, MAX) // 10 ** r.randint(1, 150)


def value(raw: int, s: int) -> Fraction:
    return Fraction(raw, 10 ** s)


def cases(seed: int, n: int):
    r = random.Random(seed)
    out = []
    kinds = ["plus", "minus", "multiply", "divide", "compare", "compare-int", "least", "greatest", "cast", "text", "round"]
    while len(out) < n:
        kind = r.choice(kinds)
        sa, sb = rnd_scale(r), rnd_scale(r)
        a, b = rnd_raw(r), rnd_raw(r)
        vec = r.random() < 0.3
        la = f"materialize({lit(a, sa)})" if vec else lit(a, sa)
        lb = lit(b, sb)
        if kind in ("plus", "minus"):
            s = max(sa, sb)
            raw = a * 10 ** (s - sa) + (b if kind == "plus" else -b) * 10 ** (s - sb)
            out.append((kind, f"{kind}({la}, {lb})", res(raw, s)))
        elif kind == "multiply":
            if sa + sb > 154:
                sb = r.randint(0, 154 - sa)
                lb = lit(b, sb)
            out.append((kind, f"multiply({la}, {lb})", res(a * b, sa + sb)))
        elif kind == "divide":
            if sb > sa:
                sa, sb = sb, sa
                la = f"materialize({lit(a, sa)})" if vec else lit(a, sa)
                lb = lit(b, sb)
            if b == 0:
                b = 7
                lb = lit(b, sb)
            # decimal_check_overflow: scales adding up to more than 154 are rejected at analysis time (upstream rule)
            out.append((kind, f"divide({la}, {lb})", OVF if sa + sb > 154 else res(tdiv(a * 10 ** sb, b), sa)))
        elif kind == "compare":
            fn = r.choice(["less", "equals", "greater", "lessOrEquals", "greaterOrEquals", "notEquals"])
            if r.random() < 0.2:  # equal values at different scales (only when the value is exact at the new scale)
                sb2 = r.randint(0, 154)
                t = value(a, sa) * 10 ** sb2
                if t.denominator == 1 and fits(int(t)):
                    b, sb = int(t), sb2
                    lb = lit(b, sb)
            x, y = value(a, sa), value(b, sb)
            v = {"less": x < y, "equals": x == y, "greater": x > y, "lessOrEquals": x <= y,
                 "greaterOrEquals": x >= y, "notEquals": x != y}[fn]
            out.append((kind, f"{fn}({la}, {lb})", {"type": "UInt8", "value": str(int(v))}))
        elif kind == "compare-int":
            fn = r.choice(["less", "equals", "greater", "greaterOrEquals"])
            iv = r.choice([0, 1, -1, 2, r.randint(-10 ** 18, 10 ** 18), r.randint(-2 ** 255, 2 ** 255 - 1)])
            it = "toInt64" if -2 ** 63 <= iv < 2 ** 63 else "toInt256"
            x, y = value(a, sa), Fraction(iv)
            v = {"less": x < y, "equals": x == y, "greater": x > y, "greaterOrEquals": x >= y}[fn]
            out.append((kind, f"{fn}({la}, {it}('{iv}'))", {"type": "UInt8", "value": str(int(v))}))
        elif kind in ("least", "greatest"):
            s = max(sa, sb)
            x, y = a * 10 ** (s - sa), b * 10 ** (s - sb)
            out.append((kind, f"{kind}({la}, {lb})", res(min(x, y) if kind == "least" else max(x, y), s)))
        elif kind == "cast":
            t = rnd_scale(r)
            raw = a * 10 ** (t - sa) if t >= sa else tdiv(a, 10 ** (sa - t))
            out.append((kind, f"CAST({la} AS {dec_t(t)})", res(raw, t)))
        elif kind == "text":
            out.append((kind, f"toString({la})", {"type": "String", "value": fmt(a, sa)}))
        else:  # round
            nd = r.choice([0, 1, 2, -1, -2, -150, -153, sa - 1, sa, sa + 1, r.randint(-5, 154)])
            k = sa - nd
            if k <= 0:
                raw = a
            else:
                m = 10 ** k
                q, rem = divmod(abs(a), m)
                if 2 * rem >= m:
                    q += 1
                raw = (q if a >= 0 else -q) * m
            out.append((kind, f"round({la}, {nd})", res(raw, sa)))
    return out


def main():
    args = sys.argv[1:]
    seed, n = 20260925, 3000
    if "--seed" in args:
        i = args.index("--seed"); seed = int(args[i + 1]); del args[i:i + 2]
    if "--cases" in args:
        i = args.index("--cases"); n = int(args[i + 1]); del args[i:i + 2]
    sql_path, oracle_path = args
    assert n <= 10000, "ids m90000..m99999"
    with open(sql_path, "w") as sql, open(oracle_path, "w") as orc:
        for i, (cat, expr, exp) in enumerate(cases(seed, n)):
            cid = f"m{90000 + i:05d}"  # run_matrix.py parses m + 5 digits; this range is free in every matrix
            sql.write(f"SELECT '{cid}', toTypeName({expr}), {expr};\n")
            orc.write(json.dumps({"id": cid, "category": f"random-{cat}", "form": "vector" if "materialize(" in expr else "const",
                                  "op": expr.split("(")[0], "args": [expr[:60]], "vals": [], "expected": exp}) + "\n")
    print(n, "cases, seed", seed)


if __name__ == "__main__":
    main()
