#!/usr/bin/env python3
"""lock_cases.py - pin the generated matrix cases (the input-case manifest that check_gate.py verifies).

PRODUCTION BOUNDARY: runs the generators in this directory into a private temp dir; no engine, no network.

  lock_cases.py --write cases.lock.json    regenerate every matrix and write the lock
  lock_cases.py --check cases.lock.json    regenerate and compare; exit 1 if a generator's output changed

For every matrix the lock records: the generator and its sha256, the number of cases, the sha256 of the generated
SQL and oracle files, the sha256 of the sorted case-id list, and the number of cases per category. check_gate.py
refuses a matrix result whose SQL/oracle differ from the lock (stale or foreign case data), whose ids are not exactly
the locked ids (truncated, duplicated or foreign rows), or whose summary does not attest the same input hashes.
A generator change therefore needs a new lock in the same change; the selftest runs --check.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MATRICES = {  # name -> generator (all write <sql> <oracle>)
    "midpoint": "gen_midpoint_matrix.py",
    "midpoint_vector": "gen_midpoint_vector.py",
    "ops": "gen_decimal512_ops_matrix.py",
    "keys": "gen_composite_keys.py",
}


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ids_sha256(ids) -> str:
    return hashlib.sha256("\n".join(sorted(ids)).encode()).hexdigest()


def describe(name: str, gen: str, sql: str, oracle: str) -> dict:
    rows = [json.loads(l) for l in open(oracle, encoding="utf-8") if l.strip()]
    ids = [r["id"] for r in rows]
    if len(ids) != len(set(ids)):
        raise SystemExit(f"{name}: duplicate case ids in the generated oracle")
    return {"generator": gen, "generator_sha256": sha256_file(os.path.join(HERE, gen)), "cases": len(ids),
            "sql_sha256": sha256_file(sql), "oracle_sha256": sha256_file(oracle), "ids_sha256": ids_sha256(ids),
            "categories": dict(sorted(collections.Counter(r.get("category") for r in rows).items()))}


def generate() -> dict:
    out = {}
    with tempfile.TemporaryDirectory(prefix="lock-cases-") as d:
        for name, gen in MATRICES.items():
            sql, orc = os.path.join(d, f"{name}.sql"), os.path.join(d, f"{name}.oracle.jsonl")
            subprocess.run([sys.executable, os.path.join(HERE, gen), sql, orc], check=True, capture_output=True, cwd=d,
                           env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            out[name] = describe(name, gen, sql, orc)
    return {"schema": 1, "matrices": out}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write")
    g.add_argument("--check")
    a = ap.parse_args()
    lock = generate()
    if a.write:
        with open(a.write, "w", encoding="utf-8") as f:
            json.dump(lock, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"wrote {a.write}: " + ", ".join(f"{k} {v['cases']}" for k, v in lock["matrices"].items()))
        return 0
    old = json.load(open(a.check, encoding="utf-8"))
    diffs = [k for k in set(old.get("matrices", {})) | set(lock["matrices"]) if old.get("matrices", {}).get(k) != lock["matrices"].get(k)]
    if diffs:
        print(f"lock {a.check} is stale for: {sorted(diffs)} (regenerate with --write in the same change)")
        return 1
    print(f"lock {a.check} matches the generators: " + ", ".join(f"{k} {v['cases']}" for k, v in lock["matrices"].items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
