#!/usr/bin/env python3
"""Run a generated matrix (one SELECT per case) on ONE isolated engine and compare with its oracle.

  run_matrix.py --binary <path-to-clickhouse> [--workers N] <matrix.sql> <oracle.jsonl> <result.jsonl>
  run_matrix.py --image <image@sha256:...>    [--workers N] <matrix.sql> <oracle.jsonl> <result.jsonl>

--binary runs `clickhouse local` in a fresh temporary directory; --image runs it in
`docker run --rm --network none`. Both are in-process engines without listening ports, i.e. isolated
from production by construction. All statements run in one batch with --ignore-error; cases missing
from the output are re-run one by one (a case can be missing because it failed or because an earlier case aborted the
batch process): the re-run's own output row is taken if present, else its error code, else `CRASH(signal N)` when the
engine was killed by a signal (e.g. a libc++ hardening assertion), so a batch abort never turns later cases into errors.
--workers N (default 1): the batch is split into N contiguous parts and the re-runs are spread over N workers, each
engine process in its own temporary directory (no listening ports), with the same per-process timeout. Every line of a
matrix is one self-contained case, so the per-case result does not depend on the split; the result file is written in
oracle order either way. The summary records the worker count.
Engine configuration: every engine process gets `background_schedule_pool_size=4` (ENGINE_CONFIG below). clickhouse local
otherwise starts all 512 threads of that pool (521 threads per process; with 8 workers the task hit a 4096 pids limit
and processes queued on the CPU quota); the pool runs background table tasks and no matrix query uses it. With it, a
process has 13 threads and needs 0.13 s instead of 0.35 s of CPU to start (measured 2026-09-25); results are unchanged
(serial/parallel and before/after comparison of all matrices). The summary records it.

Expectations per oracle row: {"type", "value"} | {"error": CODE} | {"error_any": [CODES]} (the error family is the
requirement, e.g. overflow must be rejected, not a specific code). Rows may carry a "category"; the summary counts
pass/fail per category so known-defect classes stay visible instead of being averaged away. The process exits 0 only
when every row passes and at least one row ran; otherwise 1 (a matrix that did not run is not a pass); 2 when the
oracle is invalid (duplicate case ids).

Run-time attestation in the summary (what check_gate.py ties the rows to): the sha256 of the SQL and oracle files that
were executed, the sha256 of the sorted case-id list, the number of cases, the engine identity measured just before the
run (--binary: sha256 and GNU build-id of the file; --image: the local image id, read with `docker image inspect`,
never pulled), the sha256 of this runner, the python version and the time zone, and the sha256 of the result file
written. These are the runner's statements; the gate recomputes what it
can (file hashes, ids, pass/fail from expected vs got) and treats the engine output itself as attested.
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile


# server configuration overrides for every engine process (passed after `--`); no query setting is changed
ENGINE_CONFIG = ["--background_schedule_pool_size=4"]


def run(args, sql_text, ignore_error):
    extra = ["--ignore-error"] if ignore_error else []
    if args.image:
        cmd = ["docker", "run", "--rm", "--network", "none", "-i", "--entrypoint", "/usr/bin/clickhouse", args.image, "local", "--multiquery"] + extra + ["--"] + ENGINE_CONFIG
        p = subprocess.run(cmd, input=sql_text, capture_output=True, text=True, timeout=3600)
    else:
        with tempfile.TemporaryDirectory(prefix="ch-matrix-") as wd:
            p = subprocess.run([args.binary, "local", "--multiquery"] + extra + ["--"] + ENGINE_CONFIG, input=sql_text, capture_output=True,
                               text=True, timeout=3600, cwd=wd)
    return p.returncode, p.stdout, p.stderr


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def engine_identity(args):
    if args.image:
        p = subprocess.run(["docker", "image", "inspect", "--format", "{{.Id}}", args.image], capture_output=True, text=True, timeout=120)
        return {"engine_image_id": p.stdout.strip() or None}
    out = subprocess.run(["readelf", "-n", args.binary], capture_output=True, text=True, timeout=120).stdout
    m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
    return {"engine_sha256": sha256_file(args.binary), "engine_build_id": m.group(1) if m else None}


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
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("sql")
    ap.add_argument("oracle")
    ap.add_argument("out")
    args = ap.parse_args()
    stmts = [l for l in open(args.sql).read().splitlines() if l.strip()]
    oracle = [json.loads(l) for l in open(args.oracle)]
    ids = [o["id"] for o in oracle]
    if len(ids) != len(set(ids)):
        print(json.dumps({"error": "duplicate case ids in the oracle", "oracle": args.oracle}))
        return 2
    attest = {"sql": args.sql, "oracle": args.oracle, "sql_sha256": sha256_file(args.sql), "oracle_sha256": sha256_file(args.oracle),
              "ids_sha256": hashlib.sha256("\n".join(sorted(ids)).encode()).hexdigest(), "cases": len(ids),
              "attested_at": datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
              "runner_sha256": sha256_file(os.path.abspath(__file__)), "python": platform.python_version(),
              "tz": os.environ.get("TZ") or os.path.realpath("/etc/localtime"), "engine_config": ENGINE_CONFIG}
    attest.update(engine_identity(args))
    workers = max(1, args.workers)
    # the batch: one process, or `workers` processes over contiguous parts of the statement list
    parts = [stmts[i * len(stmts) // workers:(i + 1) * len(stmts) // workers] for i in range(workers)]
    parts = [p for p in parts if p]
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        batches = list(pool.map(lambda p: run(args, "\n".join(p) + "\n", True), parts))
    rc = next((b[0] for b in batches if b[0] != 0), 0)
    got = {}
    for _, out, _ in batches:
        got.update(parse_rows(out))

    # the first statement that contains a case's quoted id (as before, without rescanning all statements per case)
    stmt_by_id = {}
    for st in stmts:
        for cid in re.findall(r"'(m\d{5})'", st):
            stmt_by_id.setdefault(cid, st)

    def rerun_case(o):
        stmt = stmt_by_id[o["id"]]
        rc1, out1, err1 = run(args, stmt + "\n", False)
        row = parse_rows(out1).get(o["id"])
        if row is not None:
            return row, False
        lines = err1.strip().splitlines()
        m = re.search(r"\(([A-Z_]+)\)\s*$", lines[-1]) if lines else None
        if m:
            return {"error": m.group(1)}, False
        if rc1 < 0 or (args.image and rc1 > 128):  # killed by a signal (docker reports 128 + N)
            why = next((l for l in lines if re.search(r"assert|Received signal|terminate|Abort", l)), lines[-1] if lines else "")
            return {"error": f"CRASH(signal {-rc1 if rc1 < 0 else rc1 - 128})", "detail": why.strip()[:200]}, True
        return {"error": f"unparsed:rc={rc1}:{(lines[-1] if lines else '')[:160]}"}, False

    missing = [o for o in oracle if o["id"] not in got]
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        reruns = list(pool.map(rerun_case, missing))
    crashes = 0
    for o, (row, crashed) in zip(missing, reruns):
        got[o["id"]] = row
        crashes += crashed
    rerun = len(missing)
    res = {"engine": args.binary or args.image, "batch_rc": rc, "rerun_one_by_one": rerun, "crashes": crashes,
           "total": len(oracle), "pass": 0, "fail": 0, "workers": workers, "batch_parts": len(parts)}
    res.update(attest)
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
    res["result_sha256"] = sha256_file(args.out)
    print(json.dumps(res))
    return 0 if (res["total"] > 0 and res["fail"] == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
