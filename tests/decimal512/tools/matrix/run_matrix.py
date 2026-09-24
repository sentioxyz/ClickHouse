#!/usr/bin/env python3
"""Run a generated matrix (one SELECT per case) on ONE isolated engine and compare with its oracle.

  run_matrix.py --binary <path-to-clickhouse> <matrix.sql> <oracle.jsonl> <result.jsonl>
  run_matrix.py --image <image@sha256:...>    <matrix.sql> <oracle.jsonl> <result.jsonl>

--binary runs `clickhouse local` in a fresh temporary directory; --image runs it in
`docker run --rm --network none`. Both are in-process engines without listening ports, i.e. isolated
from production by construction. All statements run in one batch with --ignore-error; cases missing
from the output are re-run one by one (a case can be missing because it failed or because an earlier case aborted the
batch process): the re-run's own output row is taken if present, else its error code, else `CRASH(signal N)` when the
engine was killed by a signal (e.g. a libc++ hardening assertion), so a batch abort never turns later cases into errors.

Expectations per oracle row: {"type", "value"} | {"error": CODE} | {"error_any": [CODES]} (the error family is the
requirement, e.g. overflow must be rejected, not a specific code). Rows may carry a "category"; the summary counts
pass/fail per category so known-defect classes stay visible instead of being averaged away. The process exits 0 only
when every row passes and at least one row ran; otherwise 1 (a matrix that did not run is not a pass).
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile


def run(args, sql_text, ignore_error):
    extra = ["--ignore-error"] if ignore_error else []
    if args.image:
        cmd = ["docker", "run", "--rm", "--network", "none", "-i", "--entrypoint", "/usr/bin/clickhouse", args.image, "local", "--multiquery"] + extra
        p = subprocess.run(cmd, input=sql_text, capture_output=True, text=True, timeout=3600)
    else:
        with tempfile.TemporaryDirectory(prefix="ch-matrix-") as wd:
            p = subprocess.run([args.binary, "local", "--multiquery"] + extra, input=sql_text, capture_output=True, text=True, timeout=3600, cwd=wd)
    return p.returncode, p.stdout, p.stderr


def parse_rows(out):
    got = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3 and re.fullmatch(r"m\d{5}", parts[0]):
            got[parts[0]] = {"type": parts[1], "value": parts[2]}
    return got


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--binary")
    g.add_argument("--image")
    ap.add_argument("sql")
    ap.add_argument("oracle")
    ap.add_argument("out")
    args = ap.parse_args()
    stmts = [l for l in open(args.sql).read().splitlines() if l.strip()]
    oracle = [json.loads(l) for l in open(args.oracle)]
    rc, out, _ = run(args, "\n".join(stmts) + "\n", True)
    got = parse_rows(out)
    rerun = crashes = 0
    for o in oracle:
        if o["id"] in got:
            continue
        stmt = next(s for s in stmts if f"'{o['id']}'" in s)
        rc1, out1, err1 = run(args, stmt + "\n", False)
        rerun += 1
        row = parse_rows(out1).get(o["id"])
        if row is not None:
            got[o["id"]] = row
            continue
        lines = err1.strip().splitlines()
        m = re.search(r"\(([A-Z_]+)\)\s*$", lines[-1]) if lines else None
        if m:
            got[o["id"]] = {"error": m.group(1)}
        elif rc1 < 0 or (args.image and rc1 > 128):  # killed by a signal (docker reports 128 + N)
            crashes += 1
            why = next((l for l in lines if re.search(r"assert|Received signal|terminate|Abort", l)), lines[-1] if lines else "")
            got[o["id"]] = {"error": f"CRASH(signal {-rc1 if rc1 < 0 else rc1 - 128})", "detail": why.strip()[:200]}
        else:
            got[o["id"]] = {"error": f"unparsed:rc={rc1}:{(lines[-1] if lines else '')[:160]}"}
    res = {"engine": args.binary or args.image, "batch_rc": rc, "rerun_one_by_one": rerun, "crashes": crashes,
           "total": len(oracle), "pass": 0, "fail": 0}
    with open(args.out, "w") as f:
        for o in oracle:
            e, g2 = o["expected"], got[o["id"]]
            if "error" in e:
                ok = g2.get("error") == e["error"]
            elif "error_any" in e:
                ok = g2.get("error") in e["error_any"]
            else:
                ok = g2.get("type") == e["type"] and g2.get("value") == e["value"]
            res["pass" if ok else "fail"] += 1
            cat = o.get("category")
            if cat:
                c = res.setdefault("by_category", {}).setdefault(cat, {"pass": 0, "fail": 0})
                c["pass" if ok else "fail"] += 1
            f.write(json.dumps({"id": o["id"], "status": "PASS" if ok else "FAIL", "category": o.get("category"), "args": o["args"], "vals": o["vals"], "expected": e, "got": g2}) + "\n")
    print(json.dumps(res))
    return 0 if (res["total"] > 0 and res["fail"] == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
