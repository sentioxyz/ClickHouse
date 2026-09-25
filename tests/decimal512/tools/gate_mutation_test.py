#!/usr/bin/env python3
"""gate_mutation_test.py - offline counterexample and mutation tests for scripts/check_gate.py.

PRODUCTION BOUNDARY: builds synthetic evidence in a temp dir; the "binaries" are copies of /usr/bin/true and
/usr/bin/false (hashed and read with readelf, never executed); no engine, no server, no network.

It proves each evidence check is load-bearing: a valid synthetic evidence set passes (quick exit 3 with one open
known defect, a clean quick exit 0, release blocked only by its open known defects, a clean release exit 0), and
every mutation - truncated, duplicated, foreign or stale matrix data, tampered statuses or expectations, another
engine, missing run-time attestation, compat without both cross-version directions or with unbound labels or rc != 0,
protocol steps missing, swapped or DIFF, proofs on the wrong binaries or without a baseline failure, missing proofs
for fixed defects, absent or inconsistent source/image identity, stateless runs outside the pinned selection,
replication steps missing, duplicated or DIFF or run by other binaries, a Keeper that is not the declared one,
performance thresholds loosened or presented as an SLO, too few rounds, verdicts that disagree with the raw timings,
noisy controls, failed queries - adds its specific PROBLEM. It also replays the two counterexamples of the
2026-09-24 acceptance review as pure-function calls, and checks that the gate's recomputation agrees with
run_matrix.py on a stub-engine run.
  gate_mutation_test.py <skill-dir>                  (skill layout: scripts/, tests/matrix/)
  gate_mutation_test.py <fork>/tests/decimal512/tools  (the fork's vendored copy)      exit 0 = all checks passed
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

SK = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), ".."))
if os.path.isfile(os.path.join(SK, "scripts", "check_gate.py")):  # the skill
    GATE_DIR, RUN_MATRIX = os.path.join(SK, "scripts"), os.path.join(SK, "tests", "matrix", "run_matrix.py")
else:  # the fork's vendored copy: tests/decimal512/tools/{check_gate.py,matrix/run_matrix.py}
    GATE_DIR, RUN_MATRIX = SK, os.path.join(SK, "matrix", "run_matrix.py")
GATE = os.path.join(GATE_DIR, "check_gate.py")
sys.path.insert(0, GATE_DIR)
import check_gate as C  # noqa: E402

SRC = "1234567890abcdef1234567890abcdef12345678"
FAILS = []


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def bid(p):
    out = subprocess.run(["readelf", "-n", p], capture_output=True, text=True).stdout
    return re.search(r"Build ID:\s*([0-9a-f]+)", out).group(1)


def jdump(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f)


def lines(path, rows):
    with open(path, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")


# synthetic matrices: name -> [(id, category, expected, got_candidate, got_baseline)]
OKV = {"type": "UInt64", "value": "1"}
BAD = {"type": "UInt64", "value": "2"}
ERR = {"error": "LOGICAL_ERROR"}
MATRICES = {
    "midpoint": [("m00001", "midpoint-dec512", OKV, OKV, BAD), ("m00002", "midpoint-dec512", OKV, OKV, OKV),
                 ("m00003", "midpoint-reject", {"error": "NO_COMMON_TYPE"}, {"error": "NO_COMMON_TYPE"}, {"error": "NO_COMMON_TYPE"})],
    "midpoint_vector": [("m90001", "midpoint-dec512", OKV, OKV, BAD), ("m90002", "midpoint-reject", {"error": "NO_COMMON_TYPE"}, {"error": "NO_COMMON_TYPE"}, {"error": "NO_COMMON_TYPE"})],
    "ops": [("m20000", "arith", OKV, OKV, OKV), ("m20001", "arith", OKV, OKV, OKV), ("m80000", "boundary", {"error_any": ["DECIMAL_OVERFLOW"]}, {"error": "DECIMAL_OVERFLOW"}, {"error": "DECIMAL_OVERFLOW"})],
    "keys": [("m70000", "keys-512", OKV, OKV, BAD), ("m70001", "keys-512", OKV, OKV, OKV), ("m70002", "single-key-512", OKV, ERR, ERR),
             ("m70003", "keys-control", OKV, OKV, OKV)],
    "random": [("m90000", "random-plus", OKV, OKV, BAD), ("m90001", "random-compare", OKV, OKV, OKV)],
    "random_nightly": [("m90000", "random-cast", OKV, OKV, OKV), ("m90001", "random-round", {"error_any": ["DECIMAL_OVERFLOW"]}, {"error": "DECIMAL_OVERFLOW"}, BAD)],
}
SUITE_OF = {"midpoint": "midpoint-matrix", "midpoint_vector": "midpoint-vector-matrix", "ops": "ops-matrix", "keys": "keys-matrix",
            "random": "random-matrix", "random_nightly": "random-nightly-matrix"}
PERF_Q = [("q_changed", "changed", 1.00, 1.02), ("q_control", "control", 0.50, 0.50), ("q_new", "new", None, 0.30)]  # id, category, base, cand


def write_run(d, matrix, who, engine, gotcol):
    """a run_matrix.py-shaped result + attested summary for one engine"""
    rows = MATRICES[matrix]
    res = f"results/{matrix}.{who}.result.jsonl"
    out = []
    for rid, cat, exp, gc, gb in rows:
        got = gc if gotcol == "c" else gb
        out.append({"id": rid, "status": "PASS" if C.expected_ok(exp, got) else "FAIL", "category": cat, "args": [f"f({rid})"], "vals": [],
                    "expected": exp, "got": got})
    lines(os.path.join(d, res), out)
    ids = sorted(r[0] for r in rows)
    summ = {"engine": engine, "total": len(rows), "sql": f"gen/{matrix}.sql", "oracle": f"gen/{matrix}.oracle.jsonl",
            "sql_sha256": sha(os.path.join(d, f"gen/{matrix}.sql")), "oracle_sha256": sha(os.path.join(d, f"gen/{matrix}.oracle.jsonl")),
            "ids_sha256": hashlib.sha256("\n".join(ids).encode()).hexdigest(), "cases": len(rows), "engine_sha256": sha(engine),
            "engine_build_id": bid(engine), "result_sha256": sha(os.path.join(d, res))}
    jdump(os.path.join(d, f"results/{matrix}.{who}.summary.json"), summ)
    return res, f"results/{matrix}.{who}.summary.json"


def build(d):
    for sub in ("gen", "results", "compat", "bin", "replication", "perf"):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    cand, base = os.path.join(d, "bin/cand.bin"), os.path.join(d, "bin/base.bin")
    shutil.copyfile("/usr/bin/true", cand)
    with open(cand, "ab") as f:
        f.write(("\n" + SRC + "\n").encode())
    shutil.copyfile("/usr/bin/false", base)
    lock = {"schema": 1, "matrices": {}}
    for m, rows in MATRICES.items():
        with open(os.path.join(d, f"gen/{m}.sql"), "w") as f:
            for r in rows:
                f.write(f"SELECT '{r[0]}', f({r[0]});\n")
        lines(os.path.join(d, f"gen/{m}.oracle.jsonl"),
              [{"id": r[0], "category": r[1], "args": [f"f({r[0]})"], "vals": [], "expected": r[2]} for r in rows])
        cats = {}
        for r in rows:
            cats[r[1]] = cats.get(r[1], 0) + 1
        lock["matrices"][m] = {"cases": len(rows), "sql_sha256": sha(os.path.join(d, f"gen/{m}.sql")),
                               "oracle_sha256": sha(os.path.join(d, f"gen/{m}.oracle.jsonl")),
                               "ids_sha256": hashlib.sha256("\n".join(sorted(r[0] for r in rows)).encode()).hexdigest(), "categories": cats}
    jdump(os.path.join(d, "cases.lock.json"), lock)
    suites = {}
    for m in MATRICES:
        r, s = write_run(d, m, "tested", cand, "c")
        suites[SUITE_OF[m]] = {"name": SUITE_OF[m], "kind": "matrix", "matrix": m, "sql": f"gen/{m}.sql", "oracle": f"gen/{m}.oracle.jsonl", "result": r, "summary": s}
    for m, t, ctl in (("keys", "keys-512", "keys-control"), ("midpoint", "midpoint-dec512", "midpoint-reject")):
        br, bs = write_run(d, m, "previous", base, "b")
        suites[f"regression-proof:{m}"] = {"name": f"regression-proof:{m}", "kind": "proof", "matrix": m, "targets": [t], "controls": [ctl],
                                           "sql": f"gen/{m}.sql", "oracle": f"gen/{m}.oracle.jsonl", "buggy": {"result": br, "summary": bs},
                                           "fixed": {"result": suites[SUITE_OF[m]]["result"], "summary": suites[SUITE_OF[m]]["summary"]}}
    cs, bs_, cb, bb = sha(cand), sha(base), bid(cand), bid(base)
    idl = f"engine=old sha256={bs_} build_id={bb} path={base}\nengine=tested sha256={cs} build_id={cb} path={cand}\n"
    open(os.path.join(d, "compat/summary.old.txt"), "w").write(idl + "writer=old write_rc=0 self_read_rc=0\nreader=tested rc=0 SAME\nmixed=old rc=0 SAME\nmixed=tested rc=0 SAME\n")
    open(os.path.join(d, "compat/summary.tested.txt"), "w").write(idl + "writer=tested write_rc=0 self_read_rc=0\nreader=old rc=0 SAME\nmixed=tested rc=0 SAME\nmixed=old rc=0 SAME\n")
    suites["compat-disk"] = {"name": "compat-disk", "kind": "compat", "results": ["compat/summary.old.txt", "compat/summary.tested.txt"]}
    with open(os.path.join(d, "results/native_matrix.tsv"), "w") as f:
        f.write("step\tclient\tserver\trc\tresult\n")
        for step, compared in C.protocol_required_steps().items():
            parts = step.split(".")
            f.write(f"{step}{'(SAME)' if compared else ''}\t{parts[1]}\t{parts[-1]}\t0\tok\n")
    open(os.path.join(d, "results/identities.tsv"), "w").write(f"OLD\t{base}\t{bs_}\t{bb}\nNEW\t{cand}\t{cs}\t{cb}\n")
    suites["compat-protocol"] = {"name": "compat-protocol", "kind": "protocol", "result": "results/native_matrix.tsv", "identities": "results/identities.tsv"}
    open(os.path.join(d, "sel.txt"), "w").write("t_ok\nt_excluded  # exclude: KD-T: fixture\n")
    open(os.path.join(d, "ct.log"), "w").write(
        f"# instance=c label=fork-stateless ts=x exe=/x/bin/clickhouse-{cb[:12]} tree=/x\n# cmd: tests/clickhouse-test\n"
        f"Connected to server 26.3.12.1 @ {SRC} fixture\nt_ok:   [ OK ] 0.1 sec.\n1 tests passed. 0 tests skipped. 1.0 s elapsed (MainProcess).\nAll tests have finished.\n")
    suites["fork-stateless"] = {"name": "fork-stateless", "kind": "clickhouse-test", "log": "ct.log", "build_id": cb, "selected": ["t_ok"],
                                "excluded": [{"test": "t_excluded", "reason": "KD-T"}], "selection": "sel.txt", "selection_sha256": sha(os.path.join(d, "sel.txt"))}
    jdump(os.path.join(d, "results/scan.json"), {"summary": {"sites_total": 3, "candidates": 1, "unreviewed": 0, "legacy": 1, "lost_unreviewed": 0, "rev_sha": SRC}})
    suites["dispatch-scan"] = {"name": "dispatch-scan", "kind": "scan", "result": "results/scan.json"}
    # Keeper/replication, shaped like tools/replication/mixed_replication.sh output
    keeper = os.path.join(d, "bin/keeper.bin")
    shutil.copyfile("/usr/bin/env", keeper)  # any ELF with a build-id; hashed, never executed
    ks = sha(keeper)
    open(os.path.join(d, "replication/identities.tsv"), "w").write(
        "role\tpath\tsha256\tbuild_id\tversion\n"
        f"KEEPER\t{keeper}\t{ks}\t{bid(keeper)}\tfixture keeper\nBASELINE\t{base}\t{bs_}\t{bb}\tfixture\nCANDIDATE\t{cand}\t{cs}\t{cb}\tfixture\n")
    with open(os.path.join(d, "replication/steps.tsv"), "w") as f:
        f.write("step\tactor\trc\tverdict\tdetail\n")
        for st in C.REPLICATION_REQUIRED_STEPS:
            same = st.split(".")[0] in ("fetch", "read", "mutation", "replication") or st == "merge.candidate_fetched_by_baseline"
            f.write(f"{st}\tfixture\t0\t{'SAME' if same else 'OK'}\tfixture\n")
        f.write("info.candidate_only_mutation\tcandidate\t0\tINFO\tfixture\n")
    suites["keeper-replication"] = {"name": "keeper-replication", "kind": "replication", "steps": "replication/steps.tsv",
                                    "identities": "replication/identities.tsv", "keeper_expected_sha256": ks, "keeper_basis": "fixture"}
    # performance, shaped like tools/perf/perf_compare.py output
    with open(os.path.join(d, "perf/perf_raw.tsv"), "w") as f:
        f.write("round\trole\tquery\tseconds\n")
        for r in range(5):
            for role, col in (("baseline", 2), ("candidate", 3)):
                for q in PERF_Q:
                    t = q[col]
                    f.write(f"{r}\t{role}\t{q[0]}\t{'' if t is None else f'{t:.3f}'}\n")
    jdump(os.path.join(d, "perf/perf_summary.json"), {
        "schema": 1, "verdict": "PASS", "rounds": 5, "threads": 4,
        "identities": {"baseline": {"path": base, "sha256": bs_, "build_id": bb}, "candidate": {"path": cand, "sha256": cs, "build_id": cb}},
        "thresholds": {"ratio_max": C.PERF_RATIO_MAX, "noise_floor_s": C.PERF_NOISE_FLOOR_S, "control_band": list(C.PERF_CONTROL_BAND),
                       "basis": "engineering judgment (fixture); not a user-approved SLO"},
        "environment": {"loadavg_before": [1.0, 1.0, 1.0], "loadavg_after": [1.0, 1.0, 1.0]},
        "queries": [{"id": q[0], "category": q[1], "sql": "SELECT 1"} for q in PERF_Q],
        "results": [{"query": "q_changed", "verdict": "OK"}, {"query": "q_control", "verdict": "OK"}, {"query": "q_new", "verdict": "NEW (candidate only)"}]})
    suites["performance"] = {"name": "performance", "kind": "perf", "summary": "perf/perf_summary.json", "raw": "perf/perf_raw.tsv"}
    jdump(os.path.join(d, "results/image_inspect.json"), [{"Id": "sha256:" + "a" * 64}])
    open(os.path.join(d, "results/image_binary_sha256.txt"), "w").write(f"{cs}  /usr/bin/clickhouse\n")
    reg = {"defects": [
        {"id": "KD-FIX-KEYS", "status": "fixed", "match": [{"suite": "keys-matrix", "category": "keys-512"}], "proof": {"matrix": "keys", "targets": ["keys-512"]}},
        {"id": "KD-FIX-MID", "status": "fixed", "match": [{"suite": "midpoint-matrix", "category": "midpoint-dec512"}], "proof": {"matrix": "midpoint", "targets": ["midpoint-dec512"]}},
        {"id": "KD-OPEN", "status": "open", "match": [{"suite": "keys-matrix", "category": "single-key-512"}]},
        {"id": "KD-T", "status": "open", "match": [{"suite": "fork-stateless", "test": "t_excluded"}]}]}
    jdump(os.path.join(d, "kd.json"), reg)
    jdump(os.path.join(d, "kd_empty.json"), {"defects": []})
    jdump(os.path.join(d, "kd_fixed.json"), {"defects": [x for x in reg["defects"] if x["status"] == "fixed"]})
    source = {"sha": SRC, "dirty": False, "dirty_attested_by": "fixture operator", "binary_git_hash": SRC, "binary_git_hash_source": "fixture"}
    binary = {"path": "bin/cand.bin", "sha256": cs, "build_id": cb}
    baseline = {"path": "bin/base.bin", "sha256": bs_, "build_id": bb}
    image = {"ref": "fixture:tag", "id": "sha256:" + "a" * 64, "binary_sha256": cs, "inspect": "results/image_inspect.json",
             "binary_sha256_output": "results/image_binary_sha256.txt"}
    quick = {"schema": 2, "tier": "quick", "source": source, "binary": binary, "suites": [suites[k] for k in ("midpoint-matrix", "ops-matrix", "keys-matrix")]}
    release = {"schema": 2, "tier": "release", "source": source, "binary": binary, "baseline": baseline, "image": image,
               "suites": list(suites.values()), "not_run": []}
    jdump(os.path.join(d, "quick.json"), quick)
    jdump(os.path.join(d, "release.json"), release)


def gate(d, manifest="release.json", reg="kd.json"):
    out = os.path.join(d, "gate.out.json")
    p = subprocess.run([sys.executable, GATE, "--manifest", os.path.join(d, manifest), "--known-defects", os.path.join(d, reg),
                        "--case-lock", os.path.join(d, "cases.lock.json"), "--json", out], capture_output=True, text=True)
    try:
        res = json.load(open(out))
    except (OSError, ValueError):
        res = {"problems": [p.stdout[-400:] + p.stderr[-400:]]}
    return p.returncode, res


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"\n     {detail}"))
    if not cond:
        FAILS.append(name)


def edit_json(path, fn):
    obj = json.load(open(path))
    fn(obj)
    jdump(path, obj)


def edit_lines(path, fn):
    rows = [json.loads(l) for l in open(path) if l.strip()]
    lines(path, fn(rows))


def sub(d, f, a, b):
    """replace text in an evidence file; the mutation must really change it"""
    path = os.path.join(d, f)
    text = open(path).read()
    if a not in text:
        raise AssertionError(f"mutation target {a!r} not found in {f}")
    with open(path, "w") as fh:
        fh.write(text.replace(a, b))


def swap_roles(d):
    path = os.path.join(d, "results/identities.tsv")
    text = open(path).read()
    swapped = "".join(("NEW" if l.startswith("OLD") else "OLD") + l[3:] for l in text.splitlines(keepends=True))
    if swapped == text:
        raise AssertionError("identities not swapped")
    with open(path, "w") as fh:
        fh.write(swapped)


def reattest(d, summary, result):
    """keep a mutated result consistent with its summary's result_sha256 (tests the deeper check, not the hash)"""
    edit_json(os.path.join(d, summary), lambda s: s.__setitem__("result_sha256", sha(os.path.join(d, result))))


def main():
    root = tempfile.mkdtemp(prefix="gate-mutation-")
    try:
        base = os.path.join(root, "base")
        os.makedirs(base)
        build(base)
        rc, res = gate(base, "quick.json")
        check("valid quick evidence: exit 3 (one open known defect), no problems", rc == 3 and not res["problems"], f"rc={rc} {res.get('problems')}")
        clean = os.path.join(root, "clean")
        shutil.copytree(base, clean)
        edit_lines(os.path.join(clean, "results/keys.tested.result.jsonl"),
                   lambda rows: [dict(r, got=OKV, status="PASS") if r["id"] == "m70002" else r for r in rows])
        reattest(clean, "results/keys.tested.summary.json", "results/keys.tested.result.jsonl")
        rc, res = gate(clean, "quick.json", "kd_empty.json")
        check("clean quick evidence with an empty registry: exit 0", rc == 0 and not res["problems"], f"rc={rc} {res.get('problems')}")
        rc, res = gate(base)
        basep = set(res["problems"])
        check("valid release evidence: BLOCKED only by its open known defects",
              rc == 1 and basep and all("open known defects" in p for p in basep), f"rc={rc} {sorted(basep)}")
        crel = os.path.join(root, "clean_release")
        shutil.copytree(clean, crel)
        open(os.path.join(crel, "sel.txt"), "w").write("t_ok\n")
        edit_json(os.path.join(crel, "release.json"), lambda m: [s.update(excluded=[], selection_sha256=sha(os.path.join(crel, "sel.txt")))
                                                                for s in m["suites"] if s["name"] == "fork-stateless"])
        rc, res = gate(crel, "release.json", "kd_fixed.json")
        check("clean release evidence (every suite present, only fixed defects with proofs): exit 0", rc == 0 and not res["problems"],
              f"rc={rc} {res.get('problems')}")
        bases = {i["item"]: i["basis"] for i in res.get("identity", [])}
        check("identity bases reported: sha256/build-id/commit string recomputed, GIT_HASH/image/clean tree attested",
              bases.get("binary sha256") == "recomputed" and bases.get("binary build-id") == "recomputed"
              and str(bases.get("source.sha string inside the binary", "")).startswith("recomputed")
              and str(bases.get("binary GIT_HASH", "")).startswith("attested") and str(bases.get("image id", "")).startswith("attested")
              and str(bases.get("clean source tree", "")).startswith("attested"), str(bases))

        def mutate(name, want, fn, manifest="release.json", reg="kd.json"):
            d = os.path.join(root, re.sub(r"\W+", "_", name))
            shutil.copytree(base, d)
            fn(d)
            rc, res = gate(d, manifest, reg)
            new = [p for p in res["problems"] if p not in basep]
            hit = [p for p in new if want in p]
            # the specific check must fire; and a mutation of one suite must not be caught only by collateral damage
            check(f"mutation rejected: {name}", rc == 1 and bool(hit), f"rc={rc} new problems {new[:5]}")

        KR, KS = "results/keys.tested.result.jsonl", "results/keys.tested.summary.json"
        mutate("matrix truncated (a locked case has no row)", "truncated",
               lambda d: (edit_lines(os.path.join(d, KR), lambda rows: rows[:-1]), reattest(d, KS, KR)))
        mutate("matrix duplicate row", "duplicate result id",
               lambda d: (edit_lines(os.path.join(d, KR), lambda rows: rows + [rows[0]]), reattest(d, KS, KR)))
        mutate("matrix foreign row (not a locked case)", "not locked cases",
               lambda d: (edit_lines(os.path.join(d, KR), lambda rows: rows + [dict(rows[0], id="m99999")]), reattest(d, KS, KR)))
        mutate("matrix stale oracle (case data changed after the lock)", "not the locked keys cases",
               lambda d: open(os.path.join(d, "gen/keys.oracle.jsonl"), "a").write("\n"))
        mutate("matrix wrong suite (ops suite pointed at the keys files)", "not the locked ops cases",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: [s.update(sql="gen/keys.sql", oracle="gen/keys.oracle.jsonl", result=KR, summary=KS)
                                                                               for s in m["suites"] if s["name"] == "ops-matrix"]))
        mutate("matrix status tampered (a failing row marked PASS)", "disagrees with the recomputation",
               lambda d: (edit_lines(os.path.join(d, KR), lambda rows: [dict(r, status="PASS") for r in rows]), reattest(d, KS, KR)))
        mutate("matrix expectation edited in a result row", "expectation other than the locked",
               lambda d: (edit_lines(os.path.join(d, KR), lambda rows: [dict(r, expected=r["got"]) if r["id"] == "m70002" else r for r in rows]), reattest(d, KS, KR)))
        mutate("matrix produced by another engine", "artifact mismatch",
               lambda d: edit_json(os.path.join(d, KS), lambda s: s.update(engine_sha256=sha("/usr/bin/false"))))
        mutate("matrix summary without run-time attestation (old format)", "lacks run-time attestation",
               lambda d: jdump(os.path.join(d, KS), {"engine": os.path.join(d, "bin/cand.bin")}))
        mutate("matrix result file replaced after the run", "not the one the runner wrote",
               lambda d: edit_lines(os.path.join(d, KR), lambda rows: rows[::-1]))
        CO, CT = "compat/summary.old.txt", "compat/summary.tested.txt"
        mutate("compat reader rc != 0 although SAME", "rc=9", lambda d: sub(d, CO, "reader=tested rc=0 SAME", "reader=tested rc=9 SAME"))
        mutate("compat only same-version pairs", "missing reader",
               lambda d: (sub(d, CO, "reader=tested rc=0", "reader=old rc=0"), sub(d, CT, "reader=old rc=0", "reader=tested rc=0")))
        mutate("compat labels not bound to identities", "not bound", lambda d: (sub(d, CO, "engine=", "x_engine="), sub(d, CT, "engine=", "x_engine=")))
        mutate("compat mixed merge missing", "missing mixed", lambda d: sub(d, CO, "mixed=tested rc=0 SAME\n", ""))
        mutate("compat writer failed", "did not write", lambda d: sub(d, CT, "write_rc=0", "write_rc=3"))
        mutate("protocol required step missing", "required step remote_ins.NEW.to.OLD missing",
               lambda d: sub(d, "results/native_matrix.tsv", "remote_ins.NEW.to.OLD(SAME)\tNEW\tOLD\t0\tok\n", ""))
        mutate("protocol roles swapped", "NEW is not the candidate", swap_roles)
        mutate("protocol step DIFF", "(not SAME)", lambda d: sub(d, "results/native_matrix.tsv", "sel_dyn.OLD.NEW(SAME)", "sel_dyn.OLD.NEW(DIFF)"))
        mutate("protocol identities missing", "identities file missing",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: [s.pop("identities") for s in m["suites"] if s["name"] == "compat-protocol"]))
        mutate("proof buggy side ran on the candidate", "artifact mismatch",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: [s["buggy"].update(result=KR, summary=KS) for s in m["suites"] if s["name"] == "regression-proof:keys"]))
        mutate("proof target never fails on the baseline", "never fails on the baseline",
               lambda d: (edit_lines(os.path.join(d, "results/keys.previous.result.jsonl"),
                                     lambda rows: [dict(r, got=OKV, status="PASS") if r["category"] == "keys-512" else r for r in rows]),
                          reattest(d, "results/keys.previous.summary.json", "results/keys.previous.result.jsonl")))
        mutate("proof missing for a fixed defect", "no PROVEN regression proof",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: m.update(suites=[s for s in m["suites"] if s["name"] != "regression-proof:midpoint"])))
        mutate("proof control category not in the lock", "has no locked cases",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: [s.update(controls=["no-such"]) for s in m["suites"] if s["name"] == "regression-proof:keys"]))
        mutate("source GIT_HASH attestation absent", "binary_git_hash", lambda d: edit_json(os.path.join(d, "release.json"), lambda m: m["source"].pop("binary_git_hash")))
        mutate("source.sha does not occur in the binary", "does not occur in the binary",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: m["source"].update(sha="f" * 40, binary_git_hash="f" * 40)))
        mutate("clean-tree attestation absent", "clean source tree", lambda d: edit_json(os.path.join(d, "release.json"), lambda m: m["source"].pop("dirty_attested_by")))
        mutate("baseline identity absent", "baseline identity", lambda d: edit_json(os.path.join(d, "release.json"), lambda m: m.pop("baseline")))
        mutate("image id without the raw docker inspect output", "unsupported claim", lambda d: os.remove(os.path.join(d, "results/image_inspect.json")))
        mutate("image binary sha256 not the candidate", "is not the tested binary",
               lambda d: (edit_json(os.path.join(d, "release.json"), lambda m: m["image"].update(binary_sha256="0" * 64)),
                          open(os.path.join(d, "results/image_binary_sha256.txt"), "w").write("0" * 64 + "  /usr/bin/clickhouse\n")))
        mutate("image binary sha256 differs from the kept sha256sum output", "differs from the kept sha256sum output",
               lambda d: open(os.path.join(d, "results/image_binary_sha256.txt"), "w").write("1" * 64 + "  /usr/bin/clickhouse\n"))
        mutate("image absent", "image.id", lambda d: edit_json(os.path.join(d, "release.json"), lambda m: m.pop("image")))
        mutate("stateless test outside the selection ran", "outside the selection",
               lambda d: sub(d, "ct.log", "t_ok:   [ OK ] 0.1 sec.\n", "t_ok:   [ OK ] 0.1 sec.\nt_other:   [ OK ] 0.1 sec.\n"))
        mutate("stateless duplicate result line", "result lines", lambda d: sub(d, "ct.log", "t_ok:   [ OK ] 0.1 sec.\n", "t_ok:   [ OK ] 0.1 sec.\nt_ok:   [ FAIL ] 0.1 sec.\n"))
        mutate("stateless selection file changed", "pinned", lambda d: open(os.path.join(d, "sel.txt"), "a").write("t_new\n"))
        mutate("stateless log without the server's GIT_HASH", "GIT_HASH", lambda d: sub(d, "ct.log", f"Connected to server 26.3.12.1 @ {SRC} fixture\n", ""))
        mutate("stateless log from another server binary", "build-id", lambda d: sub(d, "ct.log", "exe=/x/bin/clickhouse-", "exe=/x/bin/clickhouse-000000000000"))
        RS, RI, PS, PR = "replication/steps.tsv", "replication/identities.tsv", "perf/perf_summary.json", "perf/perf_raw.tsv"
        mutate("replication required step missing", "required step merge.downloaded_by_baseline missing",
               lambda d: sub(d, RS, "merge.downloaded_by_baseline\tfixture\t0\tOK\tfixture\n", ""))
        mutate("replication step DIFF", "step read.after_rollback (fixture) rc=0 DIFF",
               lambda d: sub(d, RS, "read.after_rollback\tfixture\t0\tSAME", "read.after_rollback\tfixture\t0\tDIFF"))
        mutate("replication step duplicated", "appears 2 times", lambda d: open(os.path.join(d, RS), "a").write("keeper.start\tfixture\t0\tOK\tagain\n"))
        mutate("replication candidate is another binary", "CANDIDATE ran sha256",
               lambda d: sub(d, RI, f"CANDIDATE\t{os.path.join(d, 'bin/cand.bin').replace(d, base)}\t{sha(os.path.join(base, 'bin/cand.bin'))}",
                             f"CANDIDATE\t{os.path.join(base, 'bin/cand.bin')}\t{sha(os.path.join(base, 'bin/base.bin'))}"))
        mutate("replication Keeper is not the declared production build", "not the declared production Keeper binary",
               lambda d: edit_json(os.path.join(d, "release.json"), lambda m: [s.update(keeper_expected_sha256="0" * 64) for s in m["suites"] if s["name"] == "keeper-replication"]))
        mutate("performance thresholds loosened", "thresholds", lambda d: edit_json(os.path.join(d, PS), lambda s: s["thresholds"].update(ratio_max=1.5)))
        mutate("performance thresholds presented as an SLO", "engineering judgment",
               lambda d: edit_json(os.path.join(d, PS), lambda s: s["thresholds"].update(basis="approved SLO")))
        mutate("performance: candidate slower than the summary says", "the recomputation says 'SLOWER'",
               lambda d: sub(d, PR, "\tcandidate\tq_changed\t1.020\n", "\tcandidate\tq_changed\t1.500\n"))
        mutate("performance: too few rounds", "runs (need", lambda d: open(os.path.join(d, PR), "w").write(
               "".join(l for l in open(os.path.join(base, PR)) if not l.startswith("4\t"))))
        mutate("performance: baseline is another binary", "BASELINE ran sha256",
               lambda d: edit_json(os.path.join(d, PS), lambda s: s["identities"]["baseline"].update(sha256=sha(os.path.join(base, "bin/cand.bin")))))
        mutate("performance: noisy control", "noisy controls",
               lambda d: (sub(d, PR, "\tcandidate\tq_control\t0.500\n", "\tcandidate\tq_control\t0.800\n"),
                          edit_json(os.path.join(d, PS), lambda s: [r.update(verdict="NOISY") for r in s["results"] if r["query"] == "q_control"])))
        mutate("performance: a new query failed on the candidate", "failed queries",
               lambda d: (sub(d, PR, "\tcandidate\tq_new\t0.300\n", "\tcandidate\tq_new\t\n"),
                          edit_json(os.path.join(d, PS), lambda s: [r.update(verdict="FAILED") for r in s["results"] if r["query"] == "q_new"])))

        # the two counterexamples of the acceptance review, as pure-function calls
        cx = os.path.join(root, "cx")
        os.makedirs(cx)
        open(os.path.join(cx, "r.jsonl"), "w").write('{"id": 0, "status": "PASS"}\n')
        jdump(os.path.join(cx, "s.json"), {"engine": "/bin/true"})
        g = C.Gate(cx, "quick", json.load(open(os.path.join(base, "cases.lock.json"))))
        C.suite_matrix(g, {"name": "ops-matrix", "result": "r.jsonl", "summary": "s.json"}, {})
        check("counterexample 1 (one PASS row, summary {engine:/bin/true}, binary={}) is rejected", bool(g.problems), str(g.notes))
        g = C.Gate(cx, "quick", json.load(open(os.path.join(base, "cases.lock.json"))))
        C.suite_matrix(g, {"name": "ops-matrix", "matrix": "ops", "sql": os.path.join(base, "gen/ops.sql"), "oracle": os.path.join(base, "gen/ops.oracle.jsonl"),
                           "result": "r.jsonl", "summary": "s.json"}, {})
        check("counterexample 1 with locked inputs: truncated, unattested and unbound", any("truncated" in p for p in g.problems)
              and any("attestation" in p for p in g.problems) and any("no expected engine identity" in p for p in g.problems), str(g.problems))
        open(os.path.join(cx, "a.txt"), "w").write("writer=old write_rc=0 self_read_rc=0\nreader=old rc=9 SAME\n")
        open(os.path.join(cx, "b.txt"), "w").write("writer=new write_rc=0 self_read_rc=0\nreader=new rc=9 SAME\n")
        g = C.Gate(cx, "release")
        C.suite_compat(g, {"name": "compat-disk", "results": ["a.txt", "b.txt"]}, {})
        check("counterexample 2 (old->old and new->new, rc=9 SAME) is rejected", any("missing reader of baseline-written data" in p for p in g.problems)
              and any("not bound" in p or "baseline identity" in p for p in g.problems), str(g.problems))

        # the gate's recomputation agrees with run_matrix.py on a real (stub-engine) run
        stub = os.path.join(root, "stub-ch")
        open(stub, "w").write("#!/usr/bin/env python3\n# selftest-fixture: offline stand-in for `clickhouse local` (no server, no network)\n"
                              "import re, sys\nfor l in sys.stdin.read().splitlines():\n    m = re.search(r\"'(m\\d{5})'\", l)\n"
                              "    if m:\n        v = re.search(r'VAL=(\\w+)', l)\n        print(f\"{m.group(1)}\\tUInt64\\t{v.group(1) if v else '1'}\")\n")
        os.chmod(stub, 0o755)
        st = os.path.join(root, "stubrun")
        os.makedirs(st)
        open(os.path.join(st, "s.sql"), "w").write("SELECT 'm00001' /* VAL=1 */;\nSELECT 'm00002' /* VAL=2 */;\n")
        lines(os.path.join(st, "s.jsonl"), [{"id": "m00001", "category": "a", "args": [], "vals": [], "expected": OKV},
                                            {"id": "m00002", "category": "a", "args": [], "vals": [], "expected": OKV}])
        p = subprocess.run([sys.executable, RUN_MATRIX, "--binary", stub, os.path.join(st, "s.sql"),
                            os.path.join(st, "s.jsonl"), os.path.join(st, "r.jsonl")], capture_output=True, text=True)
        open(os.path.join(st, "summary.json"), "w").write(p.stdout)
        smry = json.loads(p.stdout)
        lk = {"matrices": {"stub": {"cases": 2, "sql_sha256": sha(os.path.join(st, "s.sql")), "oracle_sha256": sha(os.path.join(st, "s.jsonl")),
                                    "ids_sha256": hashlib.sha256(b"m00001\nm00002").hexdigest(), "categories": {"a": 2}}}}
        g = C.Gate(st, "quick", lk)
        rows = C.validate_matrix_run(g, "stub", "stub", "s.sql", "s.jsonl", "r.jsonl", "summary.json", {"sha256": sha(stub), "build_id": None})
        check("run_matrix.py attests inputs, ids, engine and result; the gate's recomputation agrees (1 PASS, 1 FAIL, no problems)",
              p.returncode == 1 and all(k in smry for k in C.SUMMARY_ATTESTATION + ("engine_sha256",)) and rows is not None
              and [rows[i]["status"] for i in ("m00001", "m00002")] == ["PASS", "FAIL"] and not g.problems, f"rc={p.returncode} {g.problems} {sorted(smry)}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print(f"\n{'all passed' if not FAILS else str(len(FAILS)) + ' failed: ' + ', '.join(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
