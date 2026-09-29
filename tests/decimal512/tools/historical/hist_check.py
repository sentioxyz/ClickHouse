#!/usr/bin/env python3
"""hist_check.py - compare one binary's reading of the historical dataset with the independent oracle.

PRODUCTION BOUNDARY: reads local files only.

  hist_check.py <oracle.json> <phase> <results.tsv> <dumps dir> <label>

Two independent verdicts, reported separately:
  raw        the RowBinary dumps decoded here: every stored integer (Decimal as its raw scaled integer, (U)IntN, NULL
             flags) must be exactly the oracle's value - this is what the storage holds (layer 3: written values);
  formatted  every verify check ('<id>' + text): formatting, comparisons, sorting, aggregation, keys, indexes, states,
             Dynamic/JSON reads (layer 4: results after reading), against expectations computed from the raw integers.
A check without a result row failed with an error (see <label>.errors.log). Exit 0 only if both verdicts are clean.
"""
from __future__ import annotations

import json
import os
import sys

WIDTH = {"UInt32": (4, False), "Int128": (16, True), "Int256": (32, True), "UInt256": (32, False), "Int512": (64, True), "UInt512": (64, False)}


def width_of(t: str):
    if t.startswith("Decimal("):
        p = int(t[8:].split(",")[0])
        return (4 if p <= 9 else 8 if p <= 18 else 16 if p <= 38 else 32 if p <= 76 else 64), True
    return WIDTH[t]


def decode(buf: bytes, types: list[str]) -> list[list]:
    rows, pos = [], 0
    while pos < len(buf):
        row = []
        for t in types:
            nullable = t.startswith("Nullable(")
            base = t[9:-1] if nullable else t
            if nullable:
                flag = buf[pos]
                pos += 1
                if flag:
                    row.append(None)
                    continue
            w, signed = width_of(base)
            row.append(int.from_bytes(buf[pos:pos + w], "little", signed=signed))
            pos += w
        rows.append(row)
    return rows


def main() -> int:
    oracle_p, phase, results_p, dumps_d, label = sys.argv[1:6]
    oracle = json.load(open(oracle_p))
    checks = oracle["checks"][phase]
    got = {}
    for line in open(results_p, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if "\t" in line:
            k, v = line.split("\t", 1)
            got[k] = v
    fmt_fail, fmt_err = [], []
    for c in checks:
        if c["id"] not in got:
            fmt_err.append(c["id"])
        elif got[c["id"]] != c["expected"]:
            fmt_fail.append(c["id"])
    raw_fail, raw_notes = [], []
    for t, spec in oracle["dumps"].items():
        p = os.path.join(dumps_d, f"{t}.rowbinary")
        exp_rows = [[int(x) if isinstance(x, str) and x.lstrip("-").isdigit() else x for x in r] for r in spec["rows"][phase]]
        try:
            rows = decode(open(p, "rb").read(), spec["types"])
        except (OSError, IndexError, KeyError) as e:
            raw_fail.append(f"{t}: cannot decode ({e!r})")
            continue
        if len(rows) != len(exp_rows):
            raw_fail.append(f"{t}: {len(rows)} rows, oracle {len(exp_rows)}")
            continue
        exp_sorted = sorted(exp_rows, key=lambda r: r[0])
        bad = [i for i, (a, b) in enumerate(zip(rows, exp_sorted)) if a != b]
        if bad:
            raw_fail.append(f"{t}: {len(bad)} row(s) differ, first id {exp_sorted[bad[0]][0]}")
        raw_notes.append(f"{t}: {len(rows)} rows compared")
    print(f"# historical dataset read by {label} (phase {phase})")
    print(f"raw stored integers: {'OK' if not raw_fail else 'DIFF'}; " + "; ".join(raw_notes))
    for f in raw_fail:
        print(f"  RAW {f}")
    by_area = {}
    for c in checks:
        area = c["id"].split(".")[0]
        st = "ERR" if c["id"] in fmt_err else "FAIL" if c["id"] in fmt_fail else "PASS"
        by_area.setdefault(area, {}).setdefault(st, 0)
        by_area[area][st] += 1
    print("checks by area: " + "; ".join(f"{a} {d}" for a, d in sorted(by_area.items())))
    for cid in fmt_fail:
        exp = next(c["expected"] for c in checks if c["id"] == cid)
        print(f"  FAIL {cid}: got {got[cid][:120]!r} expected {exp[:120]!r}")
    for cid in fmt_err:
        print(f"  ERR  {cid}: no result (error, see errors.log)")
    ok = not raw_fail and not fmt_fail and not fmt_err
    print(f"verdict {label} {phase}: raw {'OK' if not raw_fail else 'DIFF'}, checks {len(checks) - len(fmt_fail) - len(fmt_err)}/{len(checks)} pass"
          f" ({len(fmt_fail)} wrong, {len(fmt_err)} errors) -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
