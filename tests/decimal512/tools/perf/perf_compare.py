#!/usr/bin/env python3
"""perf_compare.py - reproducible synthetic performance comparison of two ClickHouse builds (baseline vs candidate).

PRODUCTION BOUNDARY: runs `clickhouse local` only (no listening ports, no server, no network), in fresh temporary
directories under --out, with stdin=/dev/null. It reads no credentials and connects nowhere. It is a synthetic
benchmark of this host, never a load test of anything else.

  perf_compare.py --baseline <clickhouse> --candidate <clickhouse> --out <new dir>
                  [--rounds 7] [--threads 4] [--scale 1.0]

The queries run over numbers() only, so every run computes the same data. They cover the code paths the 2026-09-25
Decimal512/Int512 fixes touched (category "changed": text parsing, multiply with the overflow check, operand scale-ups,
conversions, single and composite 64-byte keys), paths they did not touch ("unchanged": Decimal256) and non-decimal
controls ("control"). Queries that only the candidate can run (Int512 arithmetic, single Int512 keys) are timed and
reported as "new", never compared.

Method: one untimed warm-up run per build, then --rounds rounds; each round runs every query once per build (one
`clickhouse local --time` process per query, so a failing query cannot shift the others; `--time` measures the query,
not the process start), and the build order alternates between rounds. Per query and build the median of the rounds
is compared: ratio = candidate / baseline.

Thresholds (ENGINEERING JUDGMENT, an initial choice for this comparison, NOT a user-approved SLO):
  * a compared query is SLOWER when ratio > 1.10 and the median difference is > 10 ms (the timer resolution is 1 ms);
  * the run is NOISY (inconclusive) when a control query differs by more than 10% (ratio outside 0.90..1.10) and by
    more than 10 ms, because then the host, not the build, explains the differences.
Exit: 0 PASS (no SLOWER query, not NOISY), 1 FAIL (SLOWER, NOISY, or a query failed), 2 usage error.
Files: perf_raw.tsv (round, role, query, seconds), perf_results.tsv, perf_summary.json (identities measured before
the run, thresholds and their basis, host load before/after, per-query statistics, verdict).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time

RATIO_MAX = 1.10
NOISE_FLOOR_S = 0.010
CONTROL_BAND = (0.90, 1.10)
THRESHOLD_BASIS = "engineering judgment (initial, 2026-09-25); not a user-approved SLO"

E80 = "1" + "0" * 80


def queries(scale: float) -> list[tuple[str, str, str]]:
    """(id, category, sql) - every query ends with FORMAT Null and fixes max_threads through {threads}."""
    big, med = int(4_000_000 * scale), int(1_000_000 * scale)
    q = [
        ("parse_d512_text", "changed", f"SELECT sum(toDecimal512(concat(toString(number), '.', toString(number % 1000000)), 18)) FROM numbers({big})"),
        ("cast_string_d512_scale20", "changed", f"SELECT count() FROM numbers({big}) WHERE CAST(toString(number * 1000003) AS Decimal(154, 20)) > 0"),
        ("multiply_d512", "changed", f"SELECT sum(x * y) FROM (SELECT toDecimal512(number, 9) AS x, toDecimal512(number % 1000 + 1, 9) AS y FROM numbers({big}))"),
        ("multiply_d512_wide_operand", "changed", f"SELECT sum(toDecimal512(number + 1, 0) * CAST('{E80}' AS Decimal(154, 0))) FROM numbers({med})"),
        ("plus_d512_scaleup", "changed", f"SELECT sum(x + y) FROM (SELECT toDecimal512(number, 2) AS x, toDecimal512(number, 20) AS y FROM numbers({big}))"),
        ("compare_d512_scaleup", "changed", f"SELECT countIf(x < y) FROM (SELECT toDecimal512(number, 2) AS x, toDecimal512(number % 1000, 20) AS y FROM numbers({big}))"),
        ("divide_d512", "changed", f"SELECT sum(x / y) FROM (SELECT toDecimal512(number, 9) AS x, toDecimal512(number % 1000 + 1, 3) AS y FROM numbers({med}))"),
        ("convert_uint64_d512", "changed", f"SELECT sum(toDecimal512(number, 18)) FROM numbers({big})"),
        ("convert_int256_d512", "changed", f"SELECT sum(toDecimal512(toInt256(number), 18)) FROM numbers({big})"),
        ("cast_d512_scaleup", "changed", f"SELECT sum(CAST(toDecimal512(number, 2) AS Decimal(154, 30))) FROM numbers({big})"),
        ("groupby_d512", "changed", f"SELECT count() FROM (SELECT toDecimal512(number % 100000, 2) AS k, count() FROM numbers({big}) GROUP BY k)"),
        ("groupby_nullable_composite_33_64b", "changed", f"SELECT count() FROM (SELECT k1, k2, count() FROM (SELECT if(number % 10 = 0, NULL, toDecimal256(number % 1000, 0)) AS k1, toUInt8(number % 7) AS k2 FROM numbers({big})) GROUP BY k1, k2)"),
        ("join_d512_composite", "changed", f"SELECT count() FROM (SELECT toDecimal512(number, 0) AS k, number % 3 AS c FROM numbers({med})) AS l INNER JOIN (SELECT toDecimal512(number * 2, 0) AS k, (number * 2) % 3 AS c FROM numbers(200000)) AS r ON l.k = r.k AND l.c = r.c"),
        ("sort_d512", "changed", f"SELECT x FROM (SELECT toDecimal512(intHash64(number) % 1000000000, 6) AS x FROM numbers({med})) ORDER BY x LIMIT 10"),
        ("multiply_d256", "unchanged", f"SELECT sum(x * y) FROM (SELECT toDecimal256(number, 9) AS x, toDecimal256(number % 1000 + 1, 9) AS y FROM numbers({big}))"),
        ("parse_d256_text", "unchanged", f"SELECT sum(toDecimal256(toString(number), 18)) FROM numbers({big})"),
        ("int64_arith", "control", f"SELECT sum(number * 3 + intHash64(number) % 7) FROM numbers({big * 5})"),
        ("string_search", "control", f"SELECT count() FROM numbers({big}) WHERE position(toString(number), '77') > 0"),
        ("groupby_uint64", "control", f"SELECT count() FROM (SELECT number % 100000 AS k, count() FROM numbers({big}) GROUP BY k)"),
        ("int512_arith", "new", f"SELECT sum(toInt512(number) * toInt512(3) + toInt512(1)) FROM numbers({big})"),
        ("int512_single_key_in", "new", f"SELECT count() FROM numbers({big}) WHERE toInt512(number) IN (SELECT toInt512(number * 3) FROM numbers(100000))"),
        ("midpoint_int512", "new", f"SELECT sum(midpoint(toInt512(number), toInt512(number + 7))) FROM numbers({big})"),
    ]
    return q


def identity(path: str) -> dict:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    bid = None
    try:
        m = re.search(r"Build ID:\s*([0-9a-f]+)", subprocess.run(["readelf", "-n", path], capture_output=True, text=True, timeout=120).stdout)
        bid = m.group(1) if m else None
    except (OSError, subprocess.TimeoutExpired):
        pass
    return {"path": os.path.realpath(path), "sha256": h.hexdigest(), "build_id": bid}


def run_all(binary: str, sql_list: list[str], out: str, threads: int) -> tuple[list[float | None], str]:
    """One clickhouse local process runs every query; returns per-query seconds (None when the query failed)."""
    times: list[float | None] = []
    wd = tempfile.mkdtemp(prefix="perf.", dir=out)
    err_all = ""
    for sql in sql_list:  # one process per query: a failing query must not shift the timings of the others
        q = f"{sql} SETTINGS max_threads = {threads} FORMAT Null"
        p = subprocess.run([binary, "local", "--time", "--query", q], capture_output=True, text=True, cwd=wd,
                           stdin=subprocess.DEVNULL, timeout=1800)
        lines = [l for l in p.stderr.strip().splitlines() if re.fullmatch(r"\d+\.\d+", l.strip())]
        if p.returncode == 0 and lines:
            times.append(float(lines[-1]))
        else:
            times.append(None)
            m = re.search(r"\(([A-Z_]+)\)", p.stderr)
            err_all += f"{m.group(1) if m else 'rc=' + str(p.returncode)};"
    subprocess.run(["rm", "-rf", wd])
    return times, err_all


def loadavg() -> list[float]:
    try:
        return [float(x) for x in open("/proc/loadavg").read().split()[:3]]
    except OSError:
        return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=7)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--scale", type=float, default=1.0)
    a = ap.parse_args()
    if a.rounds < 5:
        print("ERROR: --rounds must be at least 5 (a median of fewer runs is not a comparison)", file=sys.stderr)
        return 2
    for b in (a.baseline, a.candidate):
        if not (os.path.isfile(b) and os.access(b, os.X_OK)):
            print(f"ERROR: {b} is not an executable", file=sys.stderr)
            return 2
    if os.path.exists(a.out) and os.listdir(a.out):
        print(f"ERROR: --out {a.out} is not empty", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    out = os.path.abspath(a.out)
    qs = queries(a.scale)
    qtext = "\n".join(f"{qid}\t{cat}\t{sql}" for qid, cat, sql in qs)
    roles = {"baseline": identity(a.baseline), "candidate": identity(a.candidate)}
    if roles["baseline"]["sha256"] == roles["candidate"]["sha256"]:
        print("ERROR: baseline and candidate are the same file content", file=sys.stderr)
        return 2
    env = {"host": platform.node(), "nproc": os.cpu_count(), "loadavg_before": loadavg(), "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "cpu": next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), None)}
    sqls = [sql for _, _, sql in qs]
    for role in ("baseline", "candidate"):  # warm-up (page cache of the binary, first-run effects); not recorded
        run_all(roles[role]["path"], sqls, out, a.threads)
    raw = []
    errors = {}
    for r in range(a.rounds):
        order = ("baseline", "candidate") if r % 2 == 0 else ("candidate", "baseline")
        for role in order:
            times, err = run_all(roles[role]["path"], sqls, out, a.threads)
            for (qid, cat, _), t in zip(qs, times):
                raw.append((r, role, qid, t))
                if t is None:
                    errors.setdefault(f"{role}:{qid}", err)
    env["loadavg_after"] = loadavg()
    env["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    with open(os.path.join(out, "perf_raw.tsv"), "w") as f:
        f.write("round\trole\tquery\tseconds\n")
        for r, role, qid, t in raw:
            f.write(f"{r}\t{role}\t{qid}\t{'' if t is None else f'{t:.3f}'}\n")
    results, problems = [], []
    for qid, cat, _ in qs:
        med = {}
        for role in ("baseline", "candidate"):
            ts = [t for r, ro, q, t in raw if ro == role and q == qid]
            ok = [t for t in ts if t is not None]
            med[role] = (statistics.median(ok) if len(ok) == len(ts) and ok else None, (max(ok) - min(ok)) if ok else None)
        b, c = med["baseline"][0], med["candidate"][0]
        if cat == "new":
            verdict = "NEW (candidate only)" if c is not None else "FAILED"
            if c is None:
                problems.append(f"{qid}: the candidate cannot run this query")
            results.append((qid, cat, b, c, None, None, verdict, med))
            continue
        if b is None or c is None:
            problems.append(f"{qid}: a query failed ({'baseline' if b is None else 'candidate'})")
            results.append((qid, cat, b, c, None, None, "FAILED", med))
            continue
        ratio = c / b if b > 0 else float("inf")
        delta = c - b
        if cat == "control":
            verdict = "OK" if CONTROL_BAND[0] <= ratio <= CONTROL_BAND[1] or abs(delta) <= NOISE_FLOOR_S else "NOISY"
        else:
            verdict = "SLOWER" if ratio > RATIO_MAX and delta > NOISE_FLOOR_S else "OK"
        results.append((qid, cat, b, c, ratio, delta, verdict, med))
    slower = [r[0] for r in results if r[6] == "SLOWER"]
    noisy = [r[0] for r in results if r[6] == "NOISY"]
    if slower:
        problems.append(f"slower than the baseline beyond the threshold: {slower}")
    if noisy:
        problems.append(f"control queries outside {CONTROL_BAND}: {noisy} (host noise; the comparison is inconclusive)")
    verdict = "PASS" if not problems else ("NOISY" if noisy and not slower and not [p for p in problems if 'failed' in p or 'cannot' in p] else "FAIL")
    with open(os.path.join(out, "perf_results.tsv"), "w") as f:
        f.write("query\tcategory\tbaseline_median_s\tcandidate_median_s\tratio\tdelta_s\tbaseline_spread_s\tcandidate_spread_s\tverdict\n")
        for qid, cat, b, c, ratio, delta, v, med in results:
            fmt = lambda x, n=3: "" if x is None else f"{x:.{n}f}"
            f.write(f"{qid}\t{cat}\t{fmt(b)}\t{fmt(c)}\t{fmt(ratio)}\t{fmt(delta)}\t{fmt(med['baseline'][1])}\t{fmt(med['candidate'][1])}\t{v}\n")
    summary = {"schema": 1, "verdict": verdict, "problems": problems, "errors": errors,
               "identities": roles, "rounds": a.rounds, "threads": a.threads, "scale": a.scale,
               "queries_sha256": hashlib.sha256(qtext.encode()).hexdigest(), "queries": [{"id": q, "category": c, "sql": s} for q, c, s in qs],
               "thresholds": {"ratio_max": RATIO_MAX, "noise_floor_s": NOISE_FLOOR_S, "control_band": list(CONTROL_BAND), "basis": THRESHOLD_BASIS},
               "environment": env,
               "results": [{"query": q, "category": c, "baseline_median_s": b, "candidate_median_s": cm, "ratio": r, "delta_s": d, "verdict": v}
                           for q, c, b, cm, r, d, v, _ in results]}
    with open(os.path.join(out, "perf_summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(f"# performance {verdict}: {len([r for r in results if r[1] != 'new'])} compared queries, {len(slower)} slower, {len(noisy)} noisy controls; "
          f"thresholds: ratio > {RATIO_MAX} and > {NOISE_FLOOR_S * 1000:.0f} ms ({THRESHOLD_BASIS})")
    for qid, cat, b, c, ratio, delta, v, _ in results:
        print(f"  {qid:36s} {cat:9s} base {'-' if b is None else f'{b:.3f}':>7} cand {'-' if c is None else f'{c:.3f}':>7} ratio {'-' if ratio is None else f'{ratio:.3f}':>6} {v}")
    for p in problems:
        print(f"- PROBLEM: {p}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
