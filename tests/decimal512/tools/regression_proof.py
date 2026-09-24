#!/usr/bin/env python3
"""regression_proof.py - prove that a check catches a bug: it FAILS on the buggy binary and PASSES on the fixed one.

PRODUCTION BOUNDARY: reads local result files and hashes local binaries only; runs nothing, connects nowhere.

A regression test that passes on both binaries proves nothing (the 2026-09 fork tests passed on the production build
that aborts on 33-64 byte nullable keys). This script compares two run_matrix.py result files of the SAME matrix:

  regression_proof.py --buggy NAME=<result.jsonl> --fixed NAME=<result.jsonl>
                      --target <category> [--target ...] [--control <category> ...]
                      [--buggy-binary <path>] [--fixed-binary <path>] [--min-buggy-fail 1] [--json <out.json>]

PROVEN only if all of these hold (each violated rule is printed; exit 0 PROVEN, 1 NOT PROVEN, 2 usage/input error):
  * both files contain the same case ids (same oracle), and every listed category has rows (zero rows != pass)
  * target categories: at least --min-buggy-fail FAIL rows on the buggy binary, and 0 FAIL rows on the fixed binary
  * control categories: 0 FAIL rows on both (the harness and oracle work on both binaries)
  * no case outside the targets FAILS on the fixed binary while it PASSES on the buggy one (the fix broke nothing)
  * binary identity: with --buggy-binary/--fixed-binary, the sha256 and GNU build-id are recorded and must differ;
    with summaries (<result.jsonl>:<summary.json>) the engine sha256 the runner measured before the run must be that
    binary, and both summaries must attest the same case set (oracle and id-list sha256)
Failures that are identical on both binaries (known open defects) are reported, never counted as proof.
This is a report for people; check_gate.py recomputes the proof itself from the validated result files.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys


def load(spec: str):
    name, _, paths = spec.partition("=")
    if not name or not paths:
        raise ValueError(f"expected NAME=<result.jsonl>[:<summary.json>], got {spec!r}")
    result, _, summary = paths.partition(":")
    rows = {}
    with open(result, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                rows[r["id"]] = r
    sm = {}
    if summary:
        with open(summary, encoding="utf-8") as f:
            sm = json.load(f)
    return name, rows, sm


def identity(path: str | None) -> dict:
    if not path:
        return {}
    ident = {"path": path}
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    ident["sha256"] = h.hexdigest()
    try:
        out = subprocess.run(["readelf", "-n", path], capture_output=True, text=True, timeout=60).stdout
        m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
        ident["build_id"] = m.group(1) if m else None
    except (OSError, subprocess.TimeoutExpired):
        ident["build_id"] = None
    return ident


def counts(rows: dict, cats) -> dict:
    c = {cat: collections.Counter() for cat in cats}
    for r in rows.values():
        if r.get("category") in c:
            c[r["category"]][r["status"]] += 1
    return c


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--buggy", required=True)
    ap.add_argument("--fixed", required=True)
    ap.add_argument("--target", action="append", required=True)
    ap.add_argument("--control", action="append", default=[])
    ap.add_argument("--buggy-binary")
    ap.add_argument("--fixed-binary")
    ap.add_argument("--min-buggy-fail", type=int, default=1)
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        bname, brows, bsum = load(a.buggy)
        fname, frows, fsum = load(a.fixed)
        bengine, fengine = bsum.get("engine"), fsum.get("engine")
        bid, fid = identity(a.buggy_binary), identity(a.fixed_binary)
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    problems = []
    if set(brows) != set(frows):
        problems.append(f"different case ids: {len(set(brows) ^ set(frows))} ids are in only one file (not the same oracle)")
    cats = a.target + a.control
    bc, fc = counts(brows, cats), counts(frows, cats)
    for cat in cats:
        for who, c in ((bname, bc), (fname, fc)):
            if sum(c[cat].values()) == 0:
                problems.append(f"category {cat!r} has no rows on {who} (zero tests is not a pass)")
    for cat in a.target:
        if bc[cat]["FAIL"] < a.min_buggy_fail:
            problems.append(f"target {cat!r}: only {bc[cat]['FAIL']} FAIL on buggy {bname} (need >= {a.min_buggy_fail}): the check does not detect the bug")
        if fc[cat]["FAIL"]:
            problems.append(f"target {cat!r}: {fc[cat]['FAIL']} FAIL on fixed {fname}")
    for cat in a.control:
        for who, c in ((bname, bc), (fname, fc)):
            if c[cat]["FAIL"]:
                problems.append(f"control {cat!r}: {c[cat]['FAIL']} FAIL on {who} (harness or oracle broken)")
    newly_broken = sorted(i for i in set(brows) & set(frows)
                          if frows[i]["status"] == "FAIL" and brows[i]["status"] == "PASS" and frows[i].get("category") not in a.target)
    if newly_broken:
        problems.append(f"{len(newly_broken)} case(s) PASS on buggy but FAIL on fixed (the fix broke them), e.g. {newly_broken[:5]}")
    for who, ident, engine, sm in ((bname, bid, bengine, bsum), (fname, fid, fengine, fsum)):
        if ident and engine and os.path.realpath(engine) != os.path.realpath(ident["path"]):
            problems.append(f"{who}: result engine {engine} is not the binary {ident['path']} (artifact mismatch)")
        if ident and not ident.get("build_id"):
            problems.append(f"{who}: no GNU build-id in {ident['path']} (identity evidence missing)")
        if ident and sm and sm.get("engine_sha256") != ident["sha256"]:
            problems.append(f"{who}: the runner attests engine sha256 {sm.get('engine_sha256')}, not {ident['sha256'][:16]}... (artifact mismatch or no run-time identity)")
    if bsum and fsum and (bsum.get("oracle_sha256"), bsum.get("ids_sha256")) != (fsum.get("oracle_sha256"), fsum.get("ids_sha256")):
        problems.append("the two runs attest different case sets (oracle/id-list sha256)")
    if bid and fid and bid.get("sha256") == fid.get("sha256"):
        problems.append("buggy and fixed binaries are the same file content (sha256)")
    shared = collections.Counter(frows[i].get("category") for i in set(brows) & set(frows)
                                 if frows[i]["status"] == "FAIL" and brows[i]["status"] == "FAIL")
    verdict = "PROVEN" if not problems else "NOT PROVEN"
    report = {"verdict": verdict, "buggy": {"name": bname, "engine": bengine, **bid}, "fixed": {"name": fname, "engine": fengine, **fid},
              "oracle_sha256": fsum.get("oracle_sha256"), "ids_sha256": fsum.get("ids_sha256"),
              "targets": {c: {"buggy": dict(bc[c]), "fixed": dict(fc[c])} for c in a.target},
              "controls": {c: {"buggy": dict(bc[c]), "fixed": dict(fc[c])} for c in a.control},
              "failing_on_both_by_category": dict(shared), "problems": problems}
    print(f"# regression proof: buggy {bname} vs fixed {fname}: {verdict}")
    for c in a.target:
        print(f"- target {c}: buggy {dict(bc[c])} fixed {dict(fc[c])}")
    for c in a.control:
        print(f"- control {c}: buggy {dict(bc[c])} fixed {dict(fc[c])}")
    for who, ident in ((bname, bid), (fname, fid)):
        if ident:
            print(f"- {who}: sha256 {ident['sha256'][:16]}... build-id {ident.get('build_id')}")
    if shared:
        print(f"- failing on BOTH binaries (not fixed by this change, reported, not proof): {dict(shared)}")
    for p in problems:
        print(f"- PROBLEM: {p}")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
    return 0 if verdict == "PROVEN" else 1


if __name__ == "__main__":
    sys.exit(main())
