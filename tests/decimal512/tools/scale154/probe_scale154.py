#!/usr/bin/env python3
"""probe_scale154.py - Decimal(154, 154) and precision-154 paths, with expectations from Python integers.

PRODUCTION BOUNDARY: runs only `clickhouse local` (stdin=/dev/null, no config, no network) with the given binaries.

  probe_scale154.py cases <out.tsv>                       write the case list (id, path, sql, expected)
  probe_scale154.py run <label> <binary> <out-dir>        run every case separately, write <label>.results.tsv
  probe_scale154.py summary <out-dir> <label>...          PASS/FAIL table per path and binary (exit 1 if a label fails)

Root cause these cases exercise: Decimal512 allows precision and scale 154, but 10^154 > 2^511 - 1 (about 6.7e153).
Every computation that needs 10^154 (the scale multiplier of scale 154: whole/fractional split, float conversion, a
scale-up to 154 or a scale-down from 154 by 154, comparison with an integer, rounding to a whole number) got the
wrapped value of 10^154 mod 2^512. The expected value of a case is the mathematical one; DECIMAL_OVERFLOW (or a parse
rejection) where the exact result does not fit the result type. "control-256" cases run the same shapes on
Decimal(76, 76), where 10^76 fits Int256, and pass on every build.

A result is compared as text: the type name and the value exactly as `clickhouse local` prints it (TSV).
"""
from __future__ import annotations

import os
import subprocess
import sys
from fractions import Fraction

MAX = 2 ** 511 - 1
MIN = -2 ** 511
OVF = "error:DECIMAL_OVERFLOW"
ERR_PARSE = "error:ARGUMENT_OUT_OF_BOUND|CANNOT_PARSE_NUMBER|CANNOT_PARSE_TEXT|DECIMAL_OVERFLOW"


def fmt(raw: int, scale: int) -> str:
    """ClickHouse text of a decimal (no trailing zeros, the default of toString / TSV output)."""
    neg = raw < 0
    a = -raw if neg else raw
    whole, frac = divmod(a, 10 ** scale)
    s = str(whole)
    if scale and frac:
        s += "." + str(frac).rjust(scale, "0").rstrip("0")
    return ("-" if neg else "") + s


def lit(raw: int, scale: int, p: int = 154) -> str:
    return f"CAST('{fmt(raw, scale)}' AS Decimal({p}, {scale}))"


def d(v: str, s: int, p: int = 154) -> str:
    return f"CAST('{v}' AS Decimal({p}, {s}))"


def raw_of(v: str, s: int) -> int:
    f = Fraction(v)
    r = f * 10 ** s
    assert r.denominator == 1, (v, s)
    return int(r)


def approx(raw: int, scale: int, p: int = 154) -> tuple[str, str]:
    """expected Decimal(p, scale) result of a float conversion: equal to raw within a relative 1e-15 (the double to wide
    integer conversion of ClickHouse is not exact above 2^53)"""
    return (f"Decimal({p}, {scale})", "~" + fmt(raw, scale))


def res(raw: int, scale: int, p: int = 154) -> tuple[str, str]:
    """expected (type, value) of a Decimal(p, scale) result, or overflow when raw leaves the native range"""
    lo, hi = (MIN, MAX) if p > 76 else (-2 ** 255, 2 ** 255 - 1)
    if raw < lo or raw > hi:
        return ("", OVF)
    return (f"Decimal({p}, {scale})", fmt(raw, scale))


def f2d(fv: float, s: int) -> int:
    """raw value of CAST(<Float64 fv> AS Decimal(_, s)): fv * double(10^s), then the exact integer of that double"""
    return int(fv * float(10 ** s))


def d2f(raw: int, s: int) -> str:
    """text of toFloat64(<decimal raw at scale s>): double(raw) / double(10^s), shortest round-trip text"""
    return repr(float(raw) / float(10 ** s))


def tdiv(a: int, b: int) -> int:
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def cases() -> list[tuple[str, str, str, str, str]]:
    """(id, path, sql, expected type, expected value)"""
    out = []

    def add(path, sql, exp):
        out.append((f"s{len(out):03d}", path, sql, exp[0], exp[1]))

    half = raw_of("0.5", 154)
    # parse / format
    add("format", d("0.5", 154), res(half, 154))
    add("format", d("-0.5", 154), res(-half, 154))
    add("format", lit(MAX, 154), res(MAX, 154))
    add("format", lit(MIN, 154), res(MIN, 154))
    add("format", lit(MAX - 1, 154), res(MAX - 1, 154))
    add("format", lit(1, 154), res(1, 154))
    add("format", f"toString({d('-0.25', 154)})", ("String", "-0.25"))
    add("format", f"toDecimalString({d('0.123456789', 154)}, 5)", ("String", "0.12346"))
    add("format", f"toDecimalString({lit(MIN, 154)}, 3)", ("String", "-0.670"))
    add("parse", d("1", 154), ("", ERR_PARSE))
    add("parse", d("0.6703903964971298549787012499102923063739682910296196688861780721860882015036773488400937149083451713845015929093243025426876941405973284973216824503042048", 154), ("", ERR_PARSE))
    add("parse", f"toDecimal512OrNull('0.5', 154)", ("Nullable(Decimal(154, 154))", "0.5"))
    add("parse", f"toDecimal512('0', 154)", res(0, 154))
    # float
    add("float", f"toFloat64({d('0.5', 154)})", ("Float64", d2f(half, 154)))
    add("float", f"toFloat64({d('-0.125', 154)})", ("Float64", d2f(raw_of("-0.125", 154), 154)))
    add("float", f"toFloat64({lit(MAX, 154)})", ("Float64", d2f(MAX, 154)))
    add("float", f"CAST(toFloat64(0.5) AS Decimal(154, 154))", approx(half, 154))
    add("float", f"toDecimal512(toFloat64(-0.25), 154)", approx(raw_of("-0.25", 154), 154))
    add("float", f"CAST(materialize(toFloat64(0.5)) AS Decimal(154, 154))", approx(half, 154))
    add("float", f"CAST(toFloat64(1) AS Decimal(154, 154))", ("", OVF))
    # cast between scales and to/from integers
    add("cast", f"CAST({d('0.5', 154)} AS Decimal(154, 0))", res(0, 0))
    add("cast", f"CAST(materialize({d('0.5', 154)}) AS Decimal(154, 0))", res(0, 0))
    add("cast", f"CAST({d('-0.5', 154)} AS Decimal(154, 1))", res(-5, 1))
    add("cast", f"CAST({d('0.5', 154)} AS Decimal(76, 76))", res(raw_of("0.5", 76), 76, 76))
    add("cast", f"CAST({d('0', 0)} AS Decimal(154, 154))", res(0, 154))
    add("cast", f"CAST({d('1', 0)} AS Decimal(154, 154))", ("", OVF))
    add("cast", f"CAST(materialize({d('1', 0)}) AS Decimal(154, 154))", ("", OVF))
    add("cast", f"accurateCastOrNull({d('1', 0)}, 'Decimal(154, 154)')", ("Nullable(Decimal(154, 154))", "\\N"))
    add("cast", f"toInt64({d('0.5', 154)})", ("Int64", "0"))
    add("cast", f"toInt64({lit(MIN, 154)})", ("Int64", "0"))
    add("cast", f"CAST(toInt32(0) AS Decimal(154, 154))", res(0, 154))
    add("cast", f"CAST(toInt32(1) AS Decimal(154, 154))", ("", OVF))
    add("cast", f"toDecimal512(toInt8(-1), 154)", ("", OVF))
    # comparisons: the exact answer (a scale-up by 10^154 leaves Int512 for every non-zero integer)
    add("compare", f"less({d('0.5', 154)}, 1)", ("UInt8", "1"))
    add("compare", f"less(materialize({d('0.5', 154)}), 1)", ("UInt8", "1"))
    add("compare", f"greater({d('-0.5', 154)}, -1)", ("UInt8", "1"))
    add("compare", f"equals({d('0', 154)}, 0)", ("UInt8", "1"))
    add("compare", f"less({lit(MAX, 154)}, 1)", ("UInt8", "1"))
    add("compare", f"equals({lit(MAX, 154)}, toInt512('1'))", ("UInt8", "0"))
    add("compare", f"less({d('0.5', 154)}, {d('0.6', 1)})", ("UInt8", "1"))
    add("compare", f"less({d('0.5', 154)}, {d('1', 0)})", ("UInt8", "1"))
    add("compare", f"greater({d('1', 0)}, materialize({d('0.5', 154)}))", ("UInt8", "1"))
    add("compare", f"{d('0.5', 154)} IN ({d('0.5', 154)}, {d('0.25', 154)})", ("UInt8", "1"))
    # arithmetic
    add("arith", f"plus({d('-0.6', 154)}, 1)", res(raw_of("0.4", 154), 154))
    add("arith", f"plus(materialize({d('-0.6', 154)}), 1)", res(raw_of("0.4", 154), 154))
    add("arith", f"minus({d('0.7', 1)}, {d('0.3', 154)})", res(raw_of("0.4", 154), 154))
    add("arith", f"minus({d('0.3', 154)}, {d('0.7', 1)})", res(raw_of("-0.4", 154), 154))
    add("arith", f"plus({d('0.5', 154)}, {d('0.1', 154)})", res(raw_of("0.6", 154), 154))
    add("arith", f"plus({d('0.5', 154)}, 1)", ("", OVF))
    # Decimal / Decimal whose scales add up to more than 154 is rejected at analysis time (upstream rule for every
    # decimal width with decimal_check_overflow = 1); within the rule, a dividend scale-up that leaves Int512 is exact
    add("arith", f"divide({d('0.01', 2)}, {d('0.5', 154)})", ("", OVF))
    add("arith", f"divide(materialize({d('0.01', 2)}), {d('0.5', 154)})", ("", OVF))
    add("arith", f"divide({d('0.3', 154)}, 2)", res(raw_of("0.15", 154), 154))
    add("arith", f"divide({d('0.3', 154)}, {d('0.5', 154)})", ("", OVF))
    add("arith", f"divide({d('1' + '0' * 39, 60)}, {d('2', 60)})", res(5 * 10 ** 38 * 10 ** 60, 60))
    add("arith", f"multiply({d('0.5', 77)}, {d('0.5', 77)})", res(raw_of("0.25", 154), 154))
    add("arith", f"least({d('0.5', 154)}, 1)", res(half, 154))
    add("arith", f"greatest({d('-0.5', 154)}, -1)", res(-half, 154))
    add("arith", f"greatest({d('0.5', 154)}, 1)", ("", OVF))
    # rounding to whole numbers needs 10^154
    add("round", f"round({d('0.4', 154)})", res(0, 154))
    add("round", f"round({d('0.6', 154)})", ("", OVF))
    add("round", f"floor({d('0.4', 154)})", res(0, 154))
    add("round", f"floor({d('-0.4', 154)})", ("", OVF))
    add("round", f"ceil({d('-0.4', 154)})", res(0, 154))
    add("round", f"trunc({d('0.6', 154)})", res(0, 154))
    add("round", f"trunc(materialize({lit(MIN, 154)}))", res(0, 154))
    add("round", f"roundBankers({d('0.5', 154)})", res(0, 154))
    add("round", f"round({d('0.123456', 154)}, 3)", res(raw_of("0.123", 154), 154))
    # precision 154 near the Int512 limit: a rounded result can leave the range
    add("round", f"round({lit(MAX, 0)}, -153)", ("", OVF))
    add("round", f"round({lit(10 ** 153 * 6 + 1, 0)}, -153)", res(10 ** 153 * 6, 0))
    # aggregates and helpers
    add("aggregate", f"(SELECT avg(x) FROM (SELECT arrayJoin([{d('0.125', 154)}, {d('0.375', 154)}]) AS x))",
        ("Nullable(Float64)", repr(float(raw_of("0.5", 154)) / float(10 ** 154) / 2)))
    add("aggregate", f"(SELECT sum(x) FROM (SELECT arrayJoin([{d('0.2', 154)}, {d('0.4', 154)}]) AS x))",
        ("Nullable(Decimal(154, 154))", fmt(raw_of("0.6", 154), 154)))
    add("aggregate", f"isDecimalOverflow({lit(MAX, 154)})", ("UInt8", "0"))
    add("aggregate", f"isDecimalOverflow({lit(MIN, 0)})", ("UInt8", "0"))
    # precision 154 with scale 0: the max whole value is the Int512 range (10^154 - 1 does not fit)
    add("field", f"{d('5', 0)} IN (5, 6)", ("UInt8", "1"))
    add("field", f"{d('5', 1)} IN (5)", ("UInt8", "1"))
    # Decimal(76, 76) controls: 10^76 fits Int256, same shapes pass on every build
    h76 = raw_of("0.5", 76)
    add("control-256", d("0.5", 76, 76), res(h76, 76, 76))
    add("control-256", f"toFloat64({d('0.5', 76, 76)})", ("Float64", d2f(h76, 76)))
    add("control-256", f"CAST(toFloat64(0.5) AS Decimal(76, 76))", approx(h76, 76, 76))
    add("control-256", f"CAST({d('0.5', 76, 76)} AS Decimal(76, 0))", res(0, 0, 76))
    add("control-256", f"less({d('0.5', 76, 76)}, 1)", ("UInt8", "1"))
    add("control-256", f"plus({d('-0.6', 76, 76)}, 1)", res(raw_of("0.4", 76), 76, 76))
    add("control-256", f"round({d('0.4', 76, 76)})", res(0, 76, 76))
    return out


def run(label: str, binary: str, out_dir: str) -> None:
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    for cid, path, sql, et, ev in cases():
        q = f"SELECT toTypeName({sql}) AS t, {sql} AS v FORMAT TSV"
        p = subprocess.run([binary, "local", "--query", q], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=300)
        if p.returncode == 0:
            t, _, v = p.stdout.rstrip("\n").partition("\t")
            got = (t, v)
        else:
            import re
            m = re.search(r"\(([A-Z_]+)\)", p.stderr)
            got = ("", "error:" + (m.group(1) if m else f"rc{p.returncode}"))
        if ev.startswith("error:"):
            ok = got[1].startswith("error:") and got[1][6:] in ev[6:].split("|")
        elif ev.startswith("~"):
            try:
                g, e = Fraction(got[1]), Fraction(ev[1:])
                ok = got[0] == et and abs(g - e) <= abs(e) / 10 ** 15
            except (ValueError, ZeroDivisionError):
                ok = False
        else:
            ok = got == (et, ev)
        rows.append((cid, path, "PASS" if ok else "FAIL", got[0], got[1][:400], et, ev[:400], sql))
    with open(os.path.join(out_dir, f"{label}.results.tsv"), "w") as f:
        f.write("id\tpath\tverdict\tgot_type\tgot_value\texpected_type\texpected_value\tsql\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    bad = sum(r[2] == "FAIL" for r in rows)
    print(f"{label}: {len(rows) - bad}/{len(rows)} pass")


def summary(out_dir: str, labels: list[str]) -> int:
    table = {}
    paths = []
    for lab in labels:
        for line in open(os.path.join(out_dir, f"{lab}.results.tsv")).read().splitlines()[1:]:
            c = line.split("\t")
            if c[1] not in paths:
                paths.append(c[1])
            table.setdefault((c[1], lab), [0, 0])
            table[(c[1], lab)][0 if c[2] == "PASS" else 1] += 1
    print("path\t" + "\t".join(labels))
    rc = 0
    for p in paths:
        cells = []
        for lab in labels:
            ok, bad = table.get((p, lab), [0, 0])
            cells.append(f"{ok}/{ok + bad}")
        print(p + "\t" + "\t".join(cells))
    for lab in labels:
        if any(table.get((p, lab), [0, 0])[1] for p in paths):
            rc = 1
    return rc


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "cases":
        with open(sys.argv[2], "w") as f:
            f.write("id\tpath\texpected_type\texpected_value\tsql\n")
            for cid, path, sql, et, ev in cases():
                f.write(f"{cid}\t{path}\t{et}\t{ev}\t{sql}\n")
        return 0
    if len(sys.argv) == 5 and sys.argv[1] == "run":
        run(sys.argv[2], sys.argv[3], sys.argv[4])
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "summary":
        return summary(sys.argv[2], sys.argv[3:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
