#!/usr/bin/env python3
"""check_gate.py - decide a quick / full / release gate from an evidence manifest, fail-closed.

PRODUCTION BOUNDARY: reads local files, hashes them and reads ELF notes (`readelf -n`) only. It runs no query, starts
no engine or container, pulls nothing and connects nowhere.

  check_gate.py --manifest evidence.json --known-defects known_defects.json --case-lock cases.lock.json
                [--tier quick|full|nightly|release] [--json out]

Exit codes: 0 PASS; 3 PASS WITH OPEN KNOWN DEFECTS (quick/full only: nothing unexpected failed, but the build is
not releasable); 1 FAIL or BLOCKED; 2 usage or unreadable input.

Every piece of identity evidence is reported with its basis (output section "identity", --json "identity"):
  recomputed  computed by the gate now from local bytes: sha256 of files, GNU build-id, case ids and hashes of the
              SQL/oracle files, PASS/FAIL of every row from the locked expectation and the recorded output, and
              whether the claimed commit id occurs as a string inside the binary
  attested    a statement recorded by a runner at run time (the engine output `got`, the engine sha256 measured
              before the run, `GIT_HASH` read through `clickhouse local`, the image id and the sha256 of the binary
              inside the image) or by the operator (clean source tree). Required where listed, cross-checked
              against the raw outputs kept next to them, and never reported as independent or cryptographic proof
  missing     required evidence absent: always a failure

Rules (each violation is printed as a PROBLEM and fails the gate):
  * matrices: the SQL and oracle files must be the locked ones (`--case-lock`, written by tests/matrix/lock_cases.py);
    the result must contain every locked case id exactly once and nothing else; each row must carry the locked
    expectation; the summary must attest the same input hashes, the result file's sha256 and the engine identity,
    and that identity must be the gated binary. Row PASS/FAIL is recomputed from the expectation and the recorded
    output; a recorded status that disagrees is a failure. The engine output itself is the runner's attestation.
  * regression proofs are recomputed from the two validated result files: the buggy side must be the declared
    `baseline`, the fixed side the gated binary; each target category must exist in the lock, fail at least once
    on the baseline and never on the candidate; controls pass on both; nothing else newly fails. A release needs a
    PROVEN proof for every registry entry with status `fixed` that names a proof.
  * compatibility: identity lines bind every engine label to a sha256; the gate requires baseline->candidate and
    candidate->baseline reads and both mixed merges with rc 0 and SAME, not merely two writers. Protocol: identities
    bind OLD to the baseline and NEW to the candidate, and every required step is present once, rc 0, SAME where
    compared.
  * stateless tests: the selection is re-derived from the pinned selection file; every selected test has exactly
    one result line, no test outside the selection ran, skips fail, and the log names the server's build.
  * Keeper/replication (tools/replication/mixed_replication.sh): identities bind BASELINE to the baseline and CANDIDATE to
    the gated binary (KEEPER is reported, and compared with the declared production Keeper binary when given); every
    required step is present once with rc 0 and SAME/OK; any DIFF/ERROR/REFUSED/NOT_FETCHED step fails.
  * performance (tools/perf/perf_compare.py): identities as above; the thresholds must be the fixed ones below and are
    reported as engineering judgment, not an SLO; medians, ratios and verdicts are recomputed from perf_raw.tsv (at
    least 5 rounds per build and query); a SLOWER query, a NOISY control or a failed query fails the gate.
  * mixed-groupby (tools/replication/mixed_distributed_groupby.sh): identities as above; the case lock must be the tree's
    groupby_cases.lock.json (expectations from groupby_oracle.py); every locked case once; PASS/FAIL recomputed from
    each case's result file; gated cases (new_only, mixed_init_new, mixed_init_old) are registry rows.
  * missing evidence, zero cases, `not_run` entries (release), unreviewed dispatch-scan sites fail.
  * known defects (registry): a recomputed failure that matches an OPEN defect is known-open, any other failure is
    unexpected. An open defect whose checks ran and all passed is stale; a selector that matches no row of a suite
    that ran is out of sync. quick/full exit 3 while defects are open; release is BLOCKED.
Manifest format: references/migration-ci-process.md (section "Evidence manifest").
"""
from __future__ import annotations

import argparse
import datetime
import collections
import hashlib
import json
import mmap
import os
import re
import statistics
import subprocess
import sys

REQUIRED = {
    "quick": ["midpoint-matrix", "ops-matrix", "keys-matrix", "appsql-matrix"],
    "full": ["midpoint-matrix", "midpoint-vector-matrix", "ops-matrix", "keys-matrix", "appsql-matrix", "random-matrix", "fork-stateless",
             "dispatch-scan"],
    "nightly": ["midpoint-matrix", "midpoint-vector-matrix", "ops-matrix", "keys-matrix", "appsql-matrix", "random-matrix",
                "random-nightly-matrix", "fork-stateless", "dispatch-scan"],
    "release": ["midpoint-matrix", "midpoint-vector-matrix", "ops-matrix", "keys-matrix", "appsql-matrix", "random-matrix",
                "random-nightly-matrix", "fork-stateless", "dispatch-scan",
                "regression-proof", "compat-disk", "compat-protocol", "keeper-replication", "performance", "mixed-groupby",
                "upgrade-path"],
}
# the fork keeps it next to this script (tools/replication/), the skill under tests/replication/
GROUPBY_LOCK = next((p for p in (os.path.join(os.path.dirname(os.path.abspath(__file__)), "replication", "groupby_cases.lock.json"),
                                 os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "replication", "groupby_cases.lock.json"))
                     if os.path.isfile(p)), os.path.join(os.path.dirname(os.path.abspath(__file__)), "replication", "groupby_cases.lock.json"))
HEX40, HEX64 = re.compile(r"^[0-9a-f]{40}$"), re.compile(r"^[0-9a-f]{64}$")
# run_matrix.py: the fork keeps it under tools/matrix/, the skill under tests/matrix/; results must come from this runner
RUNNER = next((p for p in (os.path.join(os.path.dirname(os.path.abspath(__file__)), "matrix", "run_matrix.py"),
                           os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "matrix", "run_matrix.py"))
               if os.path.isfile(p)), None)
# paths a checks tree may change against the binary's commit without a rebuild: none of them is compiled into the binary
HARNESS_PREFIXES = ("tests/", "docs/", ".github/")
SUMMARY_ATTESTATION = ("sql_sha256", "oracle_sha256", "ids_sha256", "cases", "result_sha256")
REPLICATION_REQUIRED_STEPS = ("keeper.start", "r1.start.baseline", "r2.start.baseline", "create", "fetch.baseline_to_baseline",
                              "r2.start.candidate", "read.after_upgrade", "fetch.baseline_to_candidate", "fetch.candidate_to_baseline",
                              "mutation.baseline_initiated", "mutation.candidate_initiated", "merge.candidate_fetched_by_baseline",
                              "merge.downloaded_by_baseline", "r2.rollback.baseline", "read.after_rollback", "replication.after_rollback")
REPLICATION_BAD_VERDICTS = ("DIFF", "ERROR", "REFUSED", "NOT_FETCHED")
# performance thresholds: engineering judgment (initial, 2026-09-25), NOT a user-approved SLO; perf_compare.py uses the same
PERF_RATIO_MAX, PERF_NOISE_FLOOR_S, PERF_CONTROL_BAND, PERF_MIN_ROUNDS = 1.10, 0.010, (0.90, 1.10), 5
PROTOCOL_ROLES = ("OLD", "NEW")


def protocol_required_steps() -> dict:
    """native_compat.sh steps between OLD and NEW: name -> must the step carry a (SAME) comparison."""
    steps = {}
    for c in PROTOCOL_ROLES:
        for s in PROTOCOL_ROLES:
            steps[f"sel_dyn.{c}.{s}"] = True
            steps[f"sel_plain.{c}.{s}"] = False
            steps[f"ins_dyn.{c}.{s}"] = True
    for x, y in (("OLD", "NEW"), ("NEW", "OLD")):
        steps[f"remote_sel.{x}.from.{y}"] = True
        steps[f"remote_ins.{x}.to.{y}"] = True
    return steps


def expected_ok(e: dict, got: dict) -> bool:
    """The comparison run_matrix.py applies, recomputed here from the locked expectation."""
    if "error" in e:
        return got.get("error") == e["error"]
    if "error_any" in e:
        return got.get("error") in e["error_any"]
    return got.get("type") == e.get("type") and got.get("value") == e.get("value")


class Gate:
    def __init__(self, base: str, tier: str, lock: dict | None = None):
        self.base = base
        self.tier = tier
        self.lock = (lock or {}).get("matrices", {})
        self.problems: list[str] = []
        self.notes: list[str] = []
        self.failures: list[dict] = []   # recomputed failures {"suite", "id"/"test", "category", "op"}
        self.rows_seen: list[dict] = []  # every row/test with its status, for registry staleness
        self.identity: list[dict] = []   # {"item", "value", "basis", "status"}
        self.proofs: dict[str, dict] = {}
        self.binary: dict = {}
        self.baseline: dict = {}
        self.binary_git_hash: str | None = None
        self.run_started_at = None                # evidence.run_started_at: results attested earlier must be marked reused
        self.runner_sha256 = None                 # sha256 of the run_matrix.py next to this gate
        self.reused: list[str] = []
        self._cache: dict[str, tuple[str, str | None]] = {}

    def path(self, p: str) -> str:
        return p if os.path.isabs(p) else os.path.join(self.base, p)

    def ident(self, p: str) -> tuple[str, str | None]:
        """sha256 and GNU build-id of a local file (recomputed)."""
        real = os.path.realpath(p)
        if real not in self._cache:
            h = hashlib.sha256()
            with open(real, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            bid = None
            try:
                out = subprocess.run(["readelf", "-n", real], capture_output=True, text=True, timeout=120).stdout
                m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
                bid = m.group(1) if m else None
            except (OSError, subprocess.TimeoutExpired):
                pass
            self._cache[real] = (h.hexdigest(), bid)
        return self._cache[real]

    def sha(self, p: str) -> str:
        return self.ident(p)[0]

    def record(self, item: str, value, basis: str, status: str) -> None:
        self.identity.append({"item": item, "value": value, "basis": basis, "status": status})


def harness_paths_ok(files: list) -> list:
    """the paths (of a diff between the binary's commit and the checks' tree) that could change the binary"""
    return [f for f in files if not str(f).startswith(HARNESS_PREFIXES)]


def parse_ts(ts):
    try:
        return datetime.datetime.fromisoformat(str(ts).replace("Z", "+00:00"))
    except ValueError:
        return None


def cache_key_of(material: dict) -> str:
    """the same key as matrix_cache.py"""
    return hashlib.sha256(json.dumps(material, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_contains(path: str, needle: bytes) -> bool:
    with open(path, "rb") as f:
        if os.fstat(f.fileno()).st_size == 0:
            return False
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            return m.find(needle) != -1


def verify_file_identity(g: Gate, label: str, claim: dict) -> dict:
    """Recompute sha256/build-id of a declared binary and compare with its claims; {} when unusable."""
    bp = (claim or {}).get("path")
    if not bp or not os.path.isfile(g.path(bp)):
        g.problems.append(f"{label}.path missing or not a file: {bp}")
        g.record(f"{label} sha256", None, "missing", "FAIL")
        return {}
    sha, bid = g.ident(g.path(bp))
    ok_sha = claim.get("sha256") == sha
    g.record(f"{label} sha256", sha, "recomputed", "OK" if ok_sha else "MISMATCH")
    if not ok_sha:
        g.problems.append(f"{label} sha256 mismatch: manifest {claim.get('sha256')} recomputed {sha}")
    if not bid:
        g.problems.append(f"{label} has no GNU build-id (identity evidence missing)")
        g.record(f"{label} build-id", None, "missing", "FAIL")
    else:
        ok_bid = claim.get("build_id") == bid
        g.record(f"{label} build-id", bid, "recomputed", "OK" if ok_bid else "MISMATCH")
        if not ok_bid:
            g.problems.append(f"{label} build-id mismatch: manifest {claim.get('build_id')} recomputed {bid}")
    return {"sha256": sha, "build_id": bid, "path": bp}


def check_identity(g: Gate, m: dict, tier: str) -> dict:
    src = m.get("source") or {}
    sha_claim = str(src.get("sha", ""))
    if not HEX40.match(sha_claim):
        g.problems.append("source.sha is not a 40-hex commit id")
    binary = verify_file_identity(g, "binary", m.get("binary") or {})
    g.binary = binary
    # the commit id: runner attestation (GIT_HASH read through clickhouse local) and an independent byte scan
    attested = src.get("binary_git_hash")
    g.binary_git_hash = attested if attested and HEX40.match(str(attested)) else None
    if not g.binary_git_hash:
        g.problems.append("source.binary_git_hash (the GIT_HASH the binary reports) is missing: identity evidence absent")
        g.record("binary GIT_HASH", attested, "missing", "FAIL")
    else:
        same = attested == sha_claim
        g.record("binary GIT_HASH", attested, f"attested ({src.get('binary_git_hash_source') or 'runner'})", "OK" if same else "MISMATCH")
        if not same:
            (g.problems if tier == "release" else g.notes).append(
                f"the binary reports GIT_HASH {attested}, not source.sha {sha_claim} (stale build directory or wrong binary)")
    if binary and HEX40.match(sha_claim):
        found = file_contains(g.path(binary["path"]), sha_claim.encode())
        g.record("source.sha string inside the binary", found, "recomputed (byte scan; GIT_HASH is written at configure time, "
                 "so this ties the binary to the commit string, not to a build of that commit)", "OK" if found else "ABSENT")
        if not found:
            (g.problems if tier == "release" else g.notes).append(f"source.sha {sha_claim} does not occur in the binary")
    if tier == "release":
        if src.get("dirty") is False and src.get("dirty_attested_by"):
            g.record("clean source tree", True, f"attested ({src['dirty_attested_by']})", "OK")
        else:
            g.problems.append("release needs a clean source tree attested by the operator (source.dirty false with dirty_attested_by)")
            g.record("clean source tree", src.get("dirty"), "missing", "FAIL")
    elif src.get("dirty"):
        g.notes.append("source tree is dirty (allowed below release)")
    check_harness_tree(g, m, sha_claim, tier)
    if m.get("baseline"):
        g.baseline = verify_file_identity(g, "baseline", m["baseline"])
        if g.baseline and binary and g.baseline["sha256"] == binary["sha256"]:
            g.problems.append("baseline and candidate are the same file content")
    elif tier == "release":
        g.problems.append("release needs a baseline identity (the previous production build) for proofs and compatibility")
        g.record("baseline sha256", None, "missing", "FAIL")
    if tier == "release":
        check_image(g, m.get("image") or {}, binary)
    return binary


def check_image(g: Gate, img: dict, binary: dict) -> None:
    """Image identity is the runner's attestation (docker image inspect / sha256sum in a --network none container,
    never pulled); the gate cross-checks it against the raw outputs kept as evidence and does not run docker."""
    iid, isha = str(img.get("id", "")), img.get("binary_sha256")
    if not re.match(r"^sha256:[0-9a-f]{64}$", iid):
        g.problems.append("release needs image.id (sha256:<64 hex>) of the local image; a tag is not an identity")
        g.record("image id", img.get("id"), "missing", "FAIL")
        return
    raw_ok = True
    try:
        inspect = json.load(open(g.path(img["inspect"]), encoding="utf-8"))
        raw_id = (inspect[0] if isinstance(inspect, list) else inspect).get("Id")
        if raw_id != iid:
            raw_ok = False
            g.problems.append(f"image.id {iid} differs from the kept `docker image inspect` output ({raw_id})")
    except (KeyError, OSError, ValueError, IndexError, AttributeError):
        raw_ok = False
        g.problems.append("image.inspect (raw `docker image inspect` output) missing or unreadable: the image id is an unsupported claim")
    g.record("image id", iid, "attested (runner: docker image inspect, local only)", "OK" if raw_ok else "UNSUPPORTED")
    try:
        raw_sha = open(g.path(img["binary_sha256_output"]), encoding="utf-8").read().split()[0]
    except (KeyError, OSError, IndexError):
        raw_sha = None
        g.problems.append("image.binary_sha256_output (raw sha256sum output) missing: the image's binary hash is an unsupported claim")
    ok = bool(isha) and raw_sha == isha and binary and isha == binary.get("sha256")
    g.record("sha256 of /usr/bin/clickhouse in the image", isha, "attested (runner: sha256sum in a --network none container)", "OK" if ok else "MISMATCH")
    if raw_sha is not None and raw_sha != isha:
        g.problems.append(f"image.binary_sha256 {isha} differs from the kept sha256sum output {raw_sha}")
    if binary and isha != binary.get("sha256"):
        g.problems.append(f"image.binary_sha256 {isha} is not the tested binary {binary.get('sha256')}")


def check_harness_tree(g: Gate, m: dict, sha_claim: str, tier: str) -> None:
    """The checks may come from a newer commit than the binary only if the difference cannot change the binary."""
    tests = m.get("tests") or {}
    tsha = str(tests.get("sha") or "")
    if not HEX40.match(tsha) or not HEX40.match(sha_claim) or tsha == sha_claim:
        return
    hd = m.get("harness_diff") or {}
    files = hd.get("files")
    if hd.get("from") != sha_claim or hd.get("to") != tsha or not isinstance(files, list):
        (g.problems if tier == "release" else g.notes).append(
            f"the checks ran from tree {tsha[:11]} but the binary is from {sha_claim[:11]}, and no diff between them was recorded")
        g.record("checks tree", tsha, "missing harness diff", "FAIL")
        return
    basis, recomputed = "attested (run_checks.sh)", None
    repo = tests.get("repo")
    if repo and os.path.isdir(repo):
        p = subprocess.run(["git", "-C", repo, "diff", "--name-only", sha_claim, tsha], capture_output=True, text=True, timeout=120,
                           env=dict(os.environ, GIT_NO_LAZY_FETCH="1", GIT_OPTIONAL_LOCKS="0"))
        if p.returncode == 0:
            recomputed, basis = [l for l in p.stdout.splitlines() if l], "recomputed (git diff --name-only)"
    if recomputed is not None and sorted(recomputed) != sorted(files):
        g.problems.append(f"the recorded harness diff {sha_claim[:11]}..{tsha[:11]} is not what git reports")
    bad = harness_paths_ok(recomputed if recomputed is not None else files)
    if bad:
        g.problems.append(f"the checks' tree {tsha[:11]} differs from the binary's commit {sha_claim[:11]} in paths that can change the "
                          f"binary (rebuild needed): {bad[:5]}")
    g.record("checks tree", tsha, f"{basis}; differs from the binary's commit only in {', '.join(HARNESS_PREFIXES)}: "
             f"{len(recomputed if recomputed is not None else files)} file(s)", "FAIL" if bad else "OK")


def check_reuse_and_age(g: Gate, label: str, sm: dict, reuse: str | None, expect: dict, sql_sha: str, orc_sha: str,
                        res_sha: str, summary: str) -> None:
    """A result older than the run must be declared as reused, and the reuse record must bind the same engine, inputs,
    runner and files; results of another runner version never count."""
    if "runner_sha256" in sm:
        if g.runner_sha256 and sm["runner_sha256"] != g.runner_sha256:
            g.problems.append(f"{label}: produced by run_matrix.py {str(sm['runner_sha256'])[:12]}, not the current runner "
                              f"{g.runner_sha256[:12]} (stale results of another runner version)")
    elif reuse:
        g.problems.append(f"{label}: reused results without a runner attestation")
    if reuse:
        try:
            rec = json.load(open(g.path(reuse), encoding="utf-8"))
        except (OSError, ValueError) as e:
            g.problems.append(f"{label}: reuse record {reuse} unreadable: {e}")
            return
        km = rec.get("key_material") or {}
        want = {"engine_sha256": (expect or {}).get("sha256"), "engine_build_id": (expect or {}).get("build_id"),
                "sql_sha256": sql_sha, "oracle_sha256": orc_sha, "runner_sha256": g.runner_sha256 or sm.get("runner_sha256")}
        bad = [k for k, v in want.items() if km.get(k) != v]
        if rec.get("reused") is not True:
            bad.append("reused flag")
        if cache_key_of(km) != rec.get("cache_key"):
            bad.append("cache key")
        files = rec.get("files") or {}
        if files.get("result_sha256") != res_sha or files.get("summary_sha256") != g.sha(g.path(summary)):
            bad.append("file hashes")
        if bad:
            g.problems.append(f"{label}: the reuse record does not bind this evidence ({bad}): cached results cannot be trusted")
            return
        origin = rec.get("origin") or {}
        g.reused.append(label)
        g.notes.append(f"{label}: REUSED historical evidence (not re-run by this run): created {rec.get('created_at')} by "
                       f"{origin.get('out') or origin}, key {str(rec.get('cache_key'))[:12]}; engine, inputs, runner and files verified")
        return
    if g.run_started_at:
        att = parse_ts(sm.get("attested_at"))
        if att is None or att < g.run_started_at:
            g.problems.append(f"{label}: results attested at {sm.get('attested_at')}, before this run started "
                              f"({g.run_started_at.isoformat()}), and not declared as reused (stale evidence presented as new)")


def validate_matrix_run(g: Gate, label: str, matrix: str, sql: str, oracle: str, result: str, summary: str,
                        expect: dict, reuse: str | None = None) -> dict | None:
    """Exact coverage and identity of one run_matrix.py run; returns {id: {status, category, op}} with recomputed
    statuses, or None when the rows cannot be trusted at all."""
    n0 = len(g.problems)
    lock = g.lock.get(matrix)
    if not lock:
        g.problems.append(f"{label}: matrix {matrix!r} is not in the case lock (unknown or foreign case data)")
        return None
    for what, p in (("sql", sql), ("oracle", oracle), ("result", result), ("summary", summary)):
        if not p or not os.path.isfile(g.path(p)):
            g.problems.append(f"{label}: {what} file missing ({p})")
    if len(g.problems) > n0:
        return None
    sql_sha, orc_sha, res_sha = g.sha(g.path(sql)), g.sha(g.path(oracle)), g.sha(g.path(result))
    if sql_sha != lock["sql_sha256"] or orc_sha != lock["oracle_sha256"]:
        g.problems.append(f"{label}: SQL/oracle are not the locked {matrix} cases (stale or foreign case data)")
    orc = [json.loads(l) for l in open(g.path(oracle), encoding="utf-8") if l.strip()]
    oids = [o.get("id") for o in orc]
    if len(oids) != len(set(oids)):
        g.problems.append(f"{label}: the oracle has duplicate case ids")
    oracle_by_id = {o.get("id"): o for o in orc}
    if len(oracle_by_id) != lock["cases"] or hashlib.sha256("\n".join(sorted(map(str, oracle_by_id))).encode()).hexdigest() != lock["ids_sha256"]:
        g.problems.append(f"{label}: oracle case ids are not the {lock['cases']} locked ids")
    sm = json.load(open(g.path(summary), encoding="utf-8"))
    absent = [k for k in SUMMARY_ATTESTATION if k not in sm]
    if absent:
        g.problems.append(f"{label}: the summary lacks run-time attestation {absent} (results cannot be tied to inputs)")
    else:
        if (sm["sql_sha256"], sm["oracle_sha256"]) != (sql_sha, orc_sha):
            g.problems.append(f"{label}: the runner executed other SQL/oracle files than the ones given")
        if sm["result_sha256"] != res_sha:
            g.problems.append(f"{label}: the result file is not the one the runner wrote (sha256 differs)")
        if sm["cases"] != lock["cases"] or sm["ids_sha256"] != lock["ids_sha256"]:
            g.problems.append(f"{label}: the runner attests {sm['cases']} cases / other ids than the lock")
    if not expect:
        g.problems.append(f"{label}: no expected engine identity to bind the results to")
    elif sm.get("engine_sha256") != expect.get("sha256"):
        g.problems.append(f"{label}: results come from engine sha256 {sm.get('engine_sha256')}, not {expect.get('sha256')} (artifact mismatch or no run-time identity)")
    elif sm.get("engine_build_id") != expect.get("build_id"):
        g.problems.append(f"{label}: engine build-id {sm.get('engine_build_id')} attested at run time is not {expect.get('build_id')}")
    check_reuse_and_age(g, label, sm, reuse, expect, sql_sha, orc_sha, res_sha, summary)
    rows, dup, bad_exp, bad_cat, disagree, weird = {}, set(), 0, 0, 0, 0
    for line in open(g.path(result), encoding="utf-8"):
        if not line.strip():
            continue
        r = json.loads(line)
        rid = r.get("id")
        if rid in rows:
            dup.add(rid)
            continue
        rows[rid] = r
    if dup:
        g.problems.append(f"{label}: {len(dup)} duplicate result id(s), e.g. {sorted(map(str, dup))[:3]}")
    missing = set(oracle_by_id) - set(rows)
    extra = set(rows) - set(oracle_by_id)
    if missing:
        g.problems.append(f"{label}: truncated: {len(missing)} locked case(s) have no result row, e.g. {sorted(map(str, missing))[:3]}")
    if extra:
        g.problems.append(f"{label}: {len(extra)} result row(s) are not locked cases (foreign suite or stale data), e.g. {sorted(map(str, extra))[:3]}")
    out = {}
    for rid, r in rows.items():
        o = oracle_by_id.get(rid)
        if o is None:
            continue
        if r.get("expected") != o.get("expected"):
            bad_exp += 1
        if r.get("category") != o.get("category"):
            bad_cat += 1
        if r.get("status") not in ("PASS", "FAIL") or not isinstance(r.get("got"), dict):
            weird += 1
        status = "PASS" if isinstance(r.get("got"), dict) and expected_ok(o["expected"], r["got"]) else "FAIL"
        if r.get("status") != status:
            disagree += 1
        out[rid] = {"status": status, "category": o.get("category"), "op": o.get("op") or (o.get("args") or [""])[0].split("(")[0]}
    for n, what in ((bad_exp, "carry an expectation other than the locked one"), (bad_cat, "carry another category than the lock"),
                    (weird, "have no PASS/FAIL status or no recorded output (skips are not passes)"),
                    (disagree, "have a recorded status that disagrees with the recomputation from expected vs got")):
        if n:
            g.problems.append(f"{label}: {n} row(s) {what}")
    return out


def suite_matrix(g: Gate, s: dict, binary: dict) -> None:
    need = [k for k in ("matrix", "sql", "oracle", "result", "summary") if not s.get(k)]
    if need:
        g.problems.append(f"{s.get('name')}: matrix suite lacks {need} (cannot verify case coverage or identity)")
        return
    rows = validate_matrix_run(g, s["name"], s["matrix"], s["sql"], s["oracle"], s["result"], s["summary"], binary, s.get("reuse"))
    if rows is None:
        return
    for rid, r in rows.items():
        rec = {"suite": s["name"], "id": rid, "category": r["category"], "op": r["op"], "status": r["status"]}
        g.rows_seen.append(rec)
        if r["status"] == "FAIL":
            g.failures.append(rec)
    g.notes.append(f"{s['name']}: {sum(r['status'] == 'PASS' for r in rows.values())}/{len(rows)} pass (recomputed; {len(rows)} of {g.lock[s['matrix']]['cases']} locked cases)")


def suite_proof(g: Gate, s: dict, binary: dict) -> None:
    """Regression proof recomputed from both validated result files; the regression_proof.py report is only compared."""
    name = s.get("name")
    need = [k for k in ("matrix", "targets", "sql", "oracle", "buggy", "fixed") if not s.get(k)]
    if need:
        g.problems.append(f"{name}: proof suite lacks {need}")
        return
    if not g.baseline:
        g.problems.append(f"{name}: no verified baseline identity to bind the buggy side to")
    lock = g.lock.get(s["matrix"], {})
    b = validate_matrix_run(g, f"{name} (buggy)", s["matrix"], s["sql"], s["oracle"], s["buggy"].get("result"), s["buggy"].get("summary"), g.baseline,
                            s["buggy"].get("reuse"))
    f = validate_matrix_run(g, f"{name} (fixed)", s["matrix"], s["sql"], s["oracle"], s["fixed"].get("result"), s["fixed"].get("summary"), binary,
                            s["fixed"].get("reuse"))
    proven = b is not None and f is not None and bool(g.baseline)
    if b is None or f is None:
        g.proofs[name] = {"matrix": s["matrix"], "targets": set(s["targets"]), "proven": False}
        return
    cats = lock.get("categories", {})
    for c in list(s["targets"]) + list(s.get("controls") or []):
        if not cats.get(c):
            proven = False
            g.problems.append(f"{name}: category {c!r} has no locked cases in matrix {s['matrix']!r}")
    fails = lambda rows, c: sum(1 for r in rows.values() if r["category"] == c and r["status"] == "FAIL")
    for c in s["targets"]:
        if fails(b, c) < 1:
            proven = False
            g.problems.append(f"{name}: target {c!r} never fails on the baseline: the check does not detect the bug")
        if fails(f, c):
            proven = False
            g.problems.append(f"{name}: target {c!r} fails {fails(f, c)} time(s) on the candidate")
    for c in s.get("controls") or []:
        if fails(b, c) or fails(f, c):
            proven = False
            g.problems.append(f"{name}: control {c!r} fails (baseline {fails(b, c)}, candidate {fails(f, c)})")
    broken = sorted(i for i in f if f[i]["status"] == "FAIL" and b.get(i, {}).get("status") == "PASS" and f[i]["category"] not in s["targets"])
    if broken:
        proven = False
        g.problems.append(f"{name}: {len(broken)} case(s) pass on the baseline but fail on the candidate, e.g. {broken[:3]}")
    if s.get("report"):
        try:
            rep = json.load(open(g.path(s["report"]), encoding="utf-8")).get("verdict")
            if (rep == "PROVEN") != proven:
                g.problems.append(f"{name}: regression_proof.py says {rep}, the gate's recomputation says {'PROVEN' if proven else 'NOT PROVEN'}")
        except (OSError, ValueError):
            g.problems.append(f"{name}: proof report {s['report']} unreadable")
    g.proofs[name] = {"matrix": s["matrix"], "targets": set(s["targets"]), "proven": proven}
    g.notes.append(f"{name}: {'PROVEN' if proven else 'NOT PROVEN'} (recomputed: targets {s['targets']}, baseline {g.baseline.get('sha256', '?')[:12]} -> candidate {binary.get('sha256', '?')[:12]})")


def suite_compat(g: Gate, s: dict, binary: dict) -> None:
    """run_compat2.sh summaries. Engine labels are bound to identities by `engine=<label> sha256=<hex>` lines."""
    name = s.get("name")
    base = g.baseline
    if not binary or not base:
        g.problems.append(f"{name}: needs the candidate and a verified baseline identity to bind engine labels to")
    role_of = {}
    if binary:
        role_of[binary["sha256"]] = "candidate"
    if base:
        role_of[base["sha256"]] = "baseline"
    seen, writers = {}, set()
    for path in s.get("results") or []:
        text = open(g.path(path), encoding="utf-8").read()
        labels = {lab: sha for lab, sha in re.findall(r"^engine=(\S+) sha256=([0-9a-f]{64})\b", text, re.M)}
        w = re.search(r"^writer=(\S+) write_rc=(\d+) self_read_rc=(\d+)", text, re.M)
        if not w:
            g.problems.append(f"{name}: {path}: no writer line")
            continue
        wrole = role_of.get(labels.get(w.group(1)))
        if not wrole:
            g.problems.append(f"{name}: {path}: writer {w.group(1)!r} is not bound to the candidate or the baseline (no matching engine= identity)")
            continue
        if w.group(2) != "0" or w.group(3) != "0":
            g.problems.append(f"{name}: {path}: {wrole} did not write and read back cleanly (write_rc={w.group(2)} self_read_rc={w.group(3)})")
            continue
        writers.add(wrole)
        for kind, lab, rc, verdict in re.findall(r"^(reader|mixed)=(\S+) rc=(\d+) (\S+)", text, re.M):
            rrole = role_of.get(labels.get(lab))
            if not rrole:
                g.problems.append(f"{name}: {path}: {kind} {lab!r} is not bound to the candidate or the baseline")
                continue
            key = (kind, wrole, rrole)
            ok = rc == "0" and verdict == "SAME"
            seen[key] = seen.get(key, True) and ok
            g.rows_seen.append({"suite": name, "id": f"{kind}:{wrole}->{rrole}", "status": "PASS" if ok else "FAIL"})
            if not ok:
                g.problems.append(f"{name}: {kind} {wrole} data read by {rrole}: rc={rc} {verdict}")
    for key in (("reader", "baseline", "candidate"), ("reader", "candidate", "baseline"),
                ("mixed", "baseline", "candidate"), ("mixed", "candidate", "baseline")):
        if key not in seen:
            g.problems.append(f"{name}: missing {key[0]} of {key[1]}-written data by the {key[2]} (both directions are required)")
    g.notes.append(f"{name}: writers {sorted(writers)}; verified {sorted(f'{k}:{w}->{r}' for (k, w, r), ok in seen.items() if ok)}")


def suite_protocol(g: Gate, s: dict, binary: dict) -> None:
    """tests/compat/native_compat.sh: native_matrix.tsv plus identities.tsv (role, path, sha256, build-id)."""
    name = s.get("name")
    ids = {}
    try:
        for line in open(g.path(s["identities"]), encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 4 and parts[0] in ("OLD", "NEW", "UP"):
                ids[parts[0]] = {"path": parts[1], "sha256": parts[2], "build_id": parts[3]}
    except (KeyError, OSError):
        g.problems.append(f"{name}: identities file missing: OLD/NEW cannot be bound to the baseline and the candidate")
    for role, want, what in (("NEW", binary, "candidate"), ("OLD", g.baseline, "baseline")):
        if not want or ids.get(role, {}).get("sha256") != want.get("sha256"):
            g.problems.append(f"{name}: {role} is not the {what} (sha256 {ids.get(role, {}).get('sha256')})")
    lines = open(g.path(s["result"]), encoding="utf-8").read().splitlines()[1:]
    got = collections.defaultdict(list)
    for line in lines:
        step, client, server, rc, _ = (line.split("\t") + [""] * 5)[:5]
        got[step.split("(")[0]].append((step, rc))
    for step, compared in protocol_required_steps().items():
        entries = got.get(step, [])
        if not entries:
            g.problems.append(f"{name}: required step {step} missing")
            continue
        if len(entries) > 1:
            g.problems.append(f"{name}: step {step} appears {len(entries)} times")
        full, rc = entries[0]
        ok = rc == "0" and (not compared or full.endswith("(SAME)"))
        g.rows_seen.append({"suite": name, "id": step, "status": "PASS" if ok else "FAIL"})
        if not ok:
            g.problems.append(f"{name}: {full} rc={rc}" + (" (not SAME)" if compared and rc == "0" else ""))
    other = sorted(k for k in got if k not in protocol_required_steps())
    g.notes.append(f"{name}: {len(protocol_required_steps())} required OLD/NEW steps checked; other steps not gated: {other[:6]}")


def selection_from_file(path: str) -> tuple[list, list]:
    sel, exc = [], []
    for line in open(path, encoding="utf-8"):
        name = line.split("#", 1)[0].strip()
        if not name:
            continue
        if "# exclude:" in line:
            exc.append(name)
        else:
            sel.append(name)
    return sel, exc


def suite_clickhouse_test(g: Gate, s: dict, binary: dict) -> None:
    name = s.get("name")
    text = open(g.path(s["log"]), encoding="utf-8", errors="replace").read()
    selected = s.get("selected") or []
    excluded = [x.get("test") for x in s.get("excluded") or []]
    if not selected:
        g.problems.append(f"{name}: no selected test list (cannot prove the selector ran what it should)")
    if s.get("selection"):
        p = g.path(s["selection"])
        if not os.path.isfile(p) or g.sha(p) != s.get("selection_sha256"):
            g.problems.append(f"{name}: selection file missing or not the pinned one (sha256)")
        else:
            fsel, fexc = selection_from_file(p)
            if sorted(fsel) != sorted(selected) or sorted(fexc) != sorted(excluded):
                g.problems.append(f"{name}: selected/excluded tests differ from the pinned selection file")
    elif g.tier != "quick":
        g.problems.append(f"{name}: no pinned selection file (selection, selection_sha256)")
    exe = re.search(r"^# instance=\S+ .*exe=\S*clickhouse-([0-9a-f]{12})\b", text, re.M)
    if not exe or not binary or not str(binary.get("build_id", "")).startswith(exe.group(1)):
        g.problems.append(f"{name}: the log does not name a server binary with the candidate's build-id")
    conn = re.search(r"^Connected to server \S+ @ ([0-9a-f]{40})", text, re.M)
    if not conn or (g.binary_git_hash and conn.group(1) != g.binary_git_hash):
        g.problems.append(f"{name}: the log does not show the server reporting the candidate's GIT_HASH")
    m = re.findall(r"(?:Having (\d+) errors! )?(\d+) tests passed\. (\d+) tests skipped\.", text)
    if not m or "All tests have finished." not in text:
        g.problems.append(f"{name}: no complete clickhouse-test summary in the log (run interrupted or never started)")
        return
    errors, passed, skipped = (sum(int(x[i] or 0) for x in m) for i in range(3))
    status = collections.defaultdict(list)
    for tname, st in re.findall(r"^(?:\[[^\]]*\]\s+)?(?:[\d:]+\s+)?(\S+?):\s+\[\s*(OK|FAIL|SKIPPED|UNKNOWN|BROKEN)\s*\]", text, re.M):
        status[tname].append(st)
    if passed + errors == 0:
        g.problems.append(f"{name}: zero tests ran")
    if skipped:
        g.problems.append(f"{name}: {skipped} test(s) skipped (a skip is not a pass)")
    outside = sorted(t for t in status if t not in selected)
    if outside:
        g.problems.append(f"{name}: {len(outside)} test(s) outside the selection ran, e.g. {outside[:3]}")
    for x in s.get("excluded") or []:  # not run on purpose: a failure of its open known defect, never a pass
        rec = {"suite": name, "test": x.get("test"), "status": "FAIL", "excluded": x.get("reason")}
        g.rows_seen.append(rec)
        g.failures.append(rec)
    for t in selected:
        st = status.get(t)
        if not st:
            g.problems.append(f"{name}: selected test {t} has no result line (not run)")
            continue
        if len(st) > 1:
            g.problems.append(f"{name}: selected test {t} has {len(st)} result lines")
        rec = {"suite": name, "test": t, "status": "PASS" if st[0] == "OK" else "FAIL"}
        g.rows_seen.append(rec)
        if st[0] != "OK":
            g.failures.append(rec)
    g.notes.append(f"{name}: {passed} passed, {errors} failed, {skipped} skipped of {len(selected)} selected")


def suite_scan(g: Gate, s: dict, binary: dict) -> None:
    d = json.load(open(g.path(s["result"]), encoding="utf-8"))
    sm = d.get("summary") or {}
    if sm.get("sites_total", 0) == 0:
        g.problems.append(f"{s['name']}: scanned zero sites")
    if sm.get("unreviewed", 1):
        g.problems.append(f"{s['name']}: {sm.get('unreviewed')} 256-bit dispatch candidate(s) not in the baseline (new sites must be reviewed)")
    if g.tier == "release" and sm.get("lost_unreviewed", 1):
        g.problems.append(f"{s['name']}: {sm.get('lost_unreviewed')} lost 512-bit site(s) (covered in the old fork) without a review decision")
    g.notes.append(f"{s['name']}: {sm.get('candidates')} candidates, {sm.get('unreviewed')} not in baseline, "
                   f"{sm.get('legacy')} legacy (debt), {sm.get('lost_unreviewed')} lost unreviewed, rev {sm.get('rev_sha', '')[:11]}")


def bind_roles(g: Gate, name: str, ids: dict, binary: dict) -> None:
    """BASELINE must be the verified baseline, CANDIDATE the gated binary (sha256 recorded by the runner before the run)."""
    for role, want, what in (("BASELINE", g.baseline, "baseline"), ("CANDIDATE", binary, "candidate")):
        got = (ids.get(role) or {}).get("sha256")
        if not want or got != want.get("sha256"):
            g.problems.append(f"{name}: {role} ran sha256 {got}, not the {what} {(want or {}).get('sha256')}")


def suite_replication(g: Gate, s: dict, binary: dict) -> None:
    """tools/replication/mixed_replication.sh: identities.tsv and steps.tsv."""
    name = s.get("name")
    ids = {}
    for line in open(g.path(s["identities"]), encoding="utf-8").read().splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 4:
            ids[parts[0]] = {"path": parts[1], "sha256": parts[2], "build_id": parts[3], "version": parts[4] if len(parts) > 4 else ""}
    bind_roles(g, name, ids, binary)
    keeper = ids.get("KEEPER") or {}
    if not keeper.get("sha256"):
        g.problems.append(f"{name}: no KEEPER identity")
    want_keeper = s.get("keeper_expected_sha256")
    if want_keeper:
        ok = keeper.get("sha256") == want_keeper
        g.record("Keeper binary in the replication check", keeper.get("sha256"), f"attested ({s.get('keeper_basis') or 'operator'})", "OK" if ok else "MISMATCH")
        if not ok:
            g.problems.append(f"{name}: Keeper sha256 {keeper.get('sha256')} is not the declared production Keeper binary {want_keeper}")
    rows = [l.split("\t") for l in open(g.path(s["steps"]), encoding="utf-8").read().splitlines()[1:] if l.strip()]
    by_step = collections.defaultdict(list)
    for r in rows:
        r = (r + [""] * 5)[:5]
        by_step[r[0]].append(r)
        if r[3] in REPLICATION_BAD_VERDICTS:
            g.problems.append(f"{name}: step {r[0]} ({r[1]}) rc={r[2]} {r[3]}: {r[4][:160]}")
    for st in REPLICATION_REQUIRED_STEPS:
        got = by_step.get(st, [])
        if not got:
            g.problems.append(f"{name}: required step {st} missing")
            continue
        if len(got) > 1:
            g.problems.append(f"{name}: step {st} appears {len(got)} times")
        ok = got[0][2] == "0" and got[0][3] in ("SAME", "OK")
        g.rows_seen.append({"suite": name, "id": st, "status": "PASS" if ok else "FAIL"})
        if not ok:
            g.problems.append(f"{name}: required step {st} rc={got[0][2]} {got[0][3]}")
    info = [f"{r[0]}: {r[4][:200]}" for r in rows if (r + [""] * 4)[3] == "INFO"]
    g.notes.append(f"{name}: {len(REPLICATION_REQUIRED_STEPS)} required steps checked (Keeper {keeper.get('version', '?')} sha256 {str(keeper.get('sha256'))[:12]}); "
                   f"informational: {info if info else 'none'}")


def suite_groupby(g: Gate, s: dict, binary: dict) -> None:
    """tools/replication/mixed_distributed_groupby.sh: identities.tsv, results/cases.tsv, the per-case result files and
    the case lock the run used. The lock must be the tree's groupby_cases.lock.json (ci_harness checks it against
    groupby_oracle.py) with an intact lock_sha256; every locked case has exactly one row; PASS/FAIL is recomputed from the
    sha256 of each case's result file against the locked expectation (a query error is a FAIL). Gated topologies
    (new_only, mixed_init_new, mixed_init_old) feed the registry as {category: topology, op: shape}; old_only rows
    (the baseline's own behaviour) are reported only."""
    name = s.get("name")
    ids = {}
    for line in open(g.path(s["identities"]), encoding="utf-8").read().splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 4:
            ids[parts[0].upper()] = {"path": parts[1], "sha256": parts[2], "build_id": parts[3]}
    bind_roles(g, name, ids, binary)
    lock = json.load(open(g.path(s["lock"]), encoding="utf-8"))
    tree_lock = json.load(open(GROUPBY_LOCK, encoding="utf-8"))
    if lock != tree_lock:
        g.problems.append(f"{name}: the run used a case lock that differs from {GROUPBY_LOCK}")
        return
    body = {k: v for k, v in lock.items() if k != "lock_sha256"}
    if hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest() != lock.get("lock_sha256"):
        g.problems.append(f"{name}: case lock content does not match its lock_sha256")
        return
    cases = {c["id"]: c for c in lock["cases"]}
    rows = collections.defaultdict(list)
    for line in open(g.path(s["cases"]), encoding="utf-8").read().splitlines()[1:]:
        r = line.split("\t")
        if len(r) >= 10:
            rows[r[0]].append(r)
    extra = sorted(set(rows) - set(cases))
    if extra:
        g.problems.append(f"{name}: {len(extra)} row(s) are not locked cases, e.g. {extra[:3]}")
    counts = collections.Counter()
    for cid, c in cases.items():
        got = rows.get(cid, [])
        if len(got) != 1:
            g.problems.append(f"{name}: locked case {cid} has {len(got)} result rows")
            continue
        r = got[0]
        f = g.path(os.path.join(s["results_dir"], cid.replace(":", ".") + ".tsv"))
        err = f[:-4] + ".err"
        actual = hashlib.sha256(open(f, "rb").read()).hexdigest() if os.path.isfile(f) else None
        errored = os.path.isfile(err) and os.path.getsize(err) > 0
        ok = actual is not None and not errored and actual == c["expected_sha256"]
        if actual is None:
            g.problems.append(f"{name}: result file of {cid} missing")
        if r[5] != ("PASS" if ok else ("ERROR" if errored else "FAIL")) or (actual and r[8] != actual):
            g.problems.append(f"{name}: recorded status/sha256 of {cid} ({r[5]}, {r[8][:12]}) disagree with the recomputation")
        counts[(c["topology"], "PASS" if ok else "FAIL")] += 1
        if c["gated"]:
            rec = {"suite": name, "id": cid, "category": c["topology"], "op": c["shape"], "status": "PASS" if ok else "FAIL"}
            g.rows_seen.append(rec)
            if not ok:
                g.failures.append(rec)
    summary = ", ".join(f"{t} {counts[(t, 'PASS')]}/{counts[(t, 'PASS')] + counts[(t, 'FAIL')]}" for t in lock["topologies"])
    g.notes.append(f"{name}: {summary} match the oracle (recomputed from the result files; old_only is not gated)")


UPGRADE_PATH_PHASES = ("P0", "P1", "P2", "P3", "P4", "P4b", "F1", "R1a", "R1", "N1", "P5", "P6")
UPGRADE_PATH_CANDIDATE_PHASES = ("P4", "P5", "P6")  # candidate-only application paths: every row must equal the oracle
UPGRADE_PATH_BAD = {"WRONG", "VIOLATION", "DIFF", "ERROR", "NOT_APPLIED", "STILL_ON", "NOT_RESTORED", "UNEXPECTED_OK", "REFUSED"}


def suite_upgrade_path(g: Gate, s: dict, binary: dict) -> None:
    """tools/replication/upgrade_path_check.sh: acceptance of the upgrade PATH (version groups switched with the
    application's addresses, a per-group remote_url_allow_hosts fence, fault injection, rollback). Recomputed here:
    the spans of every phase from its query_log rows (a query whose parts ran on two builds), the oracle outcome of every
    candidate-only row from its result sha256 against the tree's case lock, and the fail-closed, fence and replica
    outcomes. A PASS accepts the procedure only; it does not close KD-D512-MIXED-GROUPBY-NULLABLE, whose mixed-groupby
    checks keep failing and keep the release BLOCKED."""
    name = s.get("name")
    n0 = len(g.problems)
    ids = {}
    for line in open(g.path(s["identities"]), encoding="utf-8").read().splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 4:
            ids[parts[0].upper()] = {"path": parts[1], "sha256": parts[2], "build_id": parts[3]}
    bind_roles(g, name, ids, binary)
    builds = {ids[r]["build_id"][:12] for r in ("BASELINE", "CANDIDATE") if r in ids}
    d = g.path(s["results_dir"])
    spans_total = 0
    for ph in UPGRADE_PATH_PHASES:
        f = os.path.join(d, f"querylog_{ph}.tsv")
        if not os.path.isfile(f):
            g.problems.append(f"{name}: query_log of phase {ph} missing (spans cannot be checked)")
            continue
        by, nodes = collections.defaultdict(set), collections.defaultdict(set)
        for line in open(f, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 8:
                continue
            node, build, iq = parts[0], parts[1][:12], parts[2]
            by[iq].add(build)
            nodes[iq].add(node)
            if build not in builds:
                g.problems.append(f"{name}: phase {ph} ran on build {build}, neither the baseline nor the candidate")
                break
        spanning = [iq for iq, b in by.items() if len(b) > 1]
        multi = sum(1 for iq in by if len(nodes[iq]) > 1)
        if ph == "N1":
            if not spanning:
                g.problems.append(f"{name}: the negative control found no query spanning both builds (the detector would miss the hazard)")
        else:
            spans_total += len(spanning)
            if spanning:
                g.problems.append(f"{name}: {len(spanning)} quer(ies) of phase {ph} ran on both builds, e.g. {spanning[:2]}")
            if ph != "F1" and multi == 0:
                g.problems.append(f"{name}: no multi-node query observed in phase {ph} (the span check would be vacuous)")
    expect = {}
    for c in json.load(open(GROUPBY_LOCK, encoding="utf-8"))["cases"]:
        expect[c["shape"]] = c["expected_sha256"]
    counts = collections.Counter()
    phases_seen = set()
    for line in open(os.path.join(d, "checks.tsv"), encoding="utf-8").read().splitlines()[1:]:
        r = line.split("\t")
        if len(r) < 8:
            g.problems.append(f"{name}: malformed checks row {r[:3]}")
            continue
        ph, check, cfg, shape, var, outcome, detail, sha = r[:8]
        phases_seen.add(ph)
        base = outcome.split("(")[0]
        counts[base] += 1
        if base in UPGRADE_PATH_BAD:
            g.problems.append(f"{name}: {ph} {check} {cfg} {shape} {var}: {outcome}")
        if check in ("fanout", "local", "write"):
            if ph in UPGRADE_PATH_CANDIDATE_PHASES and (sha != expect.get(shape) or base != "ORACLE"):
                g.problems.append(f"{name}: candidate-only {ph} {check} {shape} {var} does not equal the oracle (sha {sha[:12]}, recorded {outcome})")
            if ph == "F1" and base != "FAILED_CLOSED":
                g.problems.append(f"{name}: F1 {check} {shape} {var} with the active replica down was {outcome}, not failed closed")
    missing = [p for p in UPGRADE_PATH_PHASES if p not in phases_seen and p not in ("R1a",)]
    if missing:
        g.problems.append(f"{name}: phases without checks: {missing}")
    for need in ("FAILED_CLOSED", "REJECTED", "SAME"):
        if not counts[need]:
            g.problems.append(f"{name}: no {need} outcome (fail-closed, fence or replica checks absent)")
    try:
        verdict = json.load(open(os.path.join(d, "summary.json"), encoding="utf-8")).get("verdict")
    except (OSError, ValueError):
        verdict = None
    ok = len(g.problems) == n0
    if ok != (verdict == "PASS"):
        g.problems.append(f"{name}: the harness verdict {verdict} disagrees with the gate's recomputation ({'PASS' if ok else 'FAIL'})")
        ok = False
    g.notes.append(f"{name}: procedure acceptance {'PASS' if ok else 'FAIL'} (recomputed: {len(UPGRADE_PATH_PHASES)} phases, "
                   f"{spans_total} queries spanning builds outside the negative control, {counts['FAILED_CLOSED']} failed closed, "
                   f"{counts['REJECTED']} fence rejections, {counts['SAME']} replica comparisons); it does not close "
                   f"KD-D512-MIXED-GROUPBY-NULLABLE")


def suite_perf(g: Gate, s: dict, binary: dict) -> None:
    """tools/perf/perf_compare.py: verdicts recomputed from perf_raw.tsv with the fixed thresholds."""
    name = s.get("name")
    sm = json.load(open(g.path(s["summary"]), encoding="utf-8"))
    ids = {k.upper(): v for k, v in (sm.get("identities") or {}).items()}
    bind_roles(g, name, ids, binary)
    th = sm.get("thresholds") or {}
    if (th.get("ratio_max"), th.get("noise_floor_s"), tuple(th.get("control_band") or ())) != (PERF_RATIO_MAX, PERF_NOISE_FLOOR_S, PERF_CONTROL_BAND):
        g.problems.append(f"{name}: thresholds {th} are not the gate's ({PERF_RATIO_MAX}, {PERF_NOISE_FLOOR_S}, {PERF_CONTROL_BAND})")
    if "engineering judgment" not in str(th.get("basis", "")):
        g.problems.append(f"{name}: the thresholds are not labelled as engineering judgment (they are not an approved SLO)")
    raw = collections.defaultdict(list)
    for line in open(g.path(s["raw"]), encoding="utf-8").read().splitlines()[1:]:
        rnd, role, qid, sec = (line.split("\t") + [""] * 4)[:4]
        raw[(role, qid)].append(float(sec) if sec else None)
    qs = sm.get("queries") or []
    if not qs:
        g.problems.append(f"{name}: zero queries")
    reported = {r.get("query"): r.get("verdict") for r in sm.get("results") or []}
    slower, noisy, failed = [], [], []
    for q in qs:
        qid, cat = q.get("id"), q.get("category")
        med = {}
        for role in ("baseline", "candidate"):
            ts = raw.get((role, qid), [])
            if len(ts) < PERF_MIN_ROUNDS or len(ts) != sm.get("rounds"):
                g.problems.append(f"{name}: {qid} has {len(ts)} {role} runs (need {sm.get('rounds')} >= {PERF_MIN_ROUNDS})")
            med[role] = statistics.median(ts) if ts and None not in ts else None
        b, c = med["baseline"], med["candidate"]
        if cat == "new":
            v = "NEW (candidate only)" if c is not None else "FAILED"
        elif b is None or c is None:
            v = "FAILED"
        else:
            ratio = c / b if b > 0 else float("inf")
            if cat == "control":
                v = "OK" if PERF_CONTROL_BAND[0] <= ratio <= PERF_CONTROL_BAND[1] or abs(c - b) <= PERF_NOISE_FLOOR_S else "NOISY"
            else:
                v = "SLOWER" if ratio > PERF_RATIO_MAX and c - b > PERF_NOISE_FLOOR_S else "OK"
        if reported.get(qid) != v:
            g.problems.append(f"{name}: {qid}: perf_compare.py says {reported.get(qid)!r}, the recomputation says {v!r}")
        (slower if v == "SLOWER" else noisy if v == "NOISY" else failed if v == "FAILED" else []).append(qid)
        g.rows_seen.append({"suite": name, "id": qid, "status": "PASS" if v in ("OK", "NEW (candidate only)") else "FAIL"})
    for what, lst in (("slower than the baseline beyond the threshold", slower), ("noisy controls (inconclusive)", noisy), ("failed queries", failed)):
        if lst:
            g.problems.append(f"{name}: {what}: {lst}")
    g.notes.append(f"{name}: {len(qs)} queries, {sm.get('rounds')} rounds, {sm.get('threads')} threads; slower {slower}, noisy {noisy}, failed {failed}; "
                   f"thresholds {PERF_RATIO_MAX}x/{PERF_NOISE_FLOOR_S * 1000:.0f} ms ({th.get('basis')}); load {sm.get('environment', {}).get('loadavg_before')} -> {sm.get('environment', {}).get('loadavg_after')}")


KINDS = {"upgrade-path": suite_upgrade_path, "matrix": suite_matrix, "clickhouse-test": suite_clickhouse_test, "proof": suite_proof, "compat": suite_compat,
         "protocol": suite_protocol, "scan": suite_scan, "replication": suite_replication, "perf": suite_perf,
         "groupby": suite_groupby}


def matches(sel: dict, rec: dict) -> bool:
    if sel.get("suite") != rec.get("suite"):
        return False
    for k in ("category", "op", "test"):
        if k in sel and sel[k] != rec.get(k):
            return False
    if "id_prefix" in sel and not str(rec.get("id", "")).startswith(sel["id_prefix"]):
        return False
    return "ids" not in sel or rec.get("id") in sel["ids"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--known-defects", required=True)
    ap.add_argument("--case-lock", required=True)
    ap.add_argument("--tier", choices=sorted(REQUIRED))
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        m = json.load(open(a.manifest, encoding="utf-8"))
        reg = json.load(open(a.known_defects, encoding="utf-8"))
        lock = json.load(open(a.case_lock, encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    tier = a.tier or m.get("tier")
    if tier not in REQUIRED:
        print(f"ERROR: unknown tier {tier!r}", file=sys.stderr)
        return 2
    if a.tier and m.get("tier") and m["tier"] != a.tier:
        print(f"ERROR: manifest was produced for tier {m['tier']!r}, not {a.tier!r}", file=sys.stderr)
        return 2
    g = Gate(os.path.dirname(os.path.abspath(a.manifest)), tier, lock)
    g.run_started_at = parse_ts(m["run_started_at"]) if m.get("run_started_at") else None
    g.runner_sha256 = g.sha(RUNNER) if RUNNER else None
    binary = check_identity(g, m, tier)
    suites = {s.get("name"): s for s in m.get("suites") or []}
    for name in REQUIRED[tier]:  # "regression-proof" is satisfied by "regression-proof:keys" etc.
        if not any(n == name or str(n).startswith(name + ":") for n in suites):
            nr = next((x for x in m.get("not_run") or [] if x.get("name") == name), None)
            g.problems.append(f"required suite {name!r} missing" + (f" (declared not run: {nr.get('reason')})" if nr else " (missing evidence)"))
    for s in suites.values():
        fn = KINDS.get(s.get("kind"))
        if not fn:
            g.problems.append(f"suite {s.get('name')!r}: unknown kind {s.get('kind')!r}")
            continue
        try:
            fn(g, s, binary)
        except (OSError, KeyError, TypeError, ValueError, AttributeError) as e:
            g.problems.append(f"suite {s.get('name')!r}: unreadable evidence: {e!r}")
    for nr in m.get("not_run") or []:
        (g.problems if tier == "release" else g.notes).append(f"not run: {nr.get('name')}: {nr.get('reason')}")
    open_hits: dict[str, int] = {}
    ran = set(suites)
    for d in reg.get("defects") or []:
        sels = d.get("match") or []
        seen = [r for r in g.rows_seen if any(matches(s, r) for s in sels)]
        failing = [r for r in g.failures if any(matches(s, r) for s in sels)]
        for s in sels:
            if s.get("suite") in ran and not any(matches(s, r) for r in g.rows_seen):
                g.problems.append(f"known defect {d['id']}: selector {s} matches no row of suite {s['suite']} (registry out of sync)")
        if d.get("status") == "open":
            # a vacuous test (runs no query, e.g. a disabled script that prints its reference) is not a pass
            for r in seen:
                if r["status"] == "PASS" and any(s.get("vacuous") and matches(s, r) for s in sels):
                    r["status"] = "FAIL"
                    g.failures.append(r)
                    failing.append(r)
            for r in failing:
                r["known"] = d["id"]
            if seen and not failing:
                g.problems.append(f"known defect {d['id']} is open but all {len(seen)} of its checks pass: mark it fixed (with evidence) in the registry")
            if failing:
                open_hits[d["id"]] = len(failing)
        elif d.get("status") == "fixed" and d.get("proof") and tier == "release":
            spec = d["proof"]
            if not any(p["proven"] and p["matrix"] == spec.get("matrix") and set(spec.get("targets") or []) <= p["targets"] for p in g.proofs.values()):
                g.problems.append(f"fixed defect {d['id']}: no PROVEN regression proof for matrix {spec.get('matrix')!r} targets {spec.get('targets')}")
    unexpected = [r for r in g.failures if "known" not in r]
    if unexpected:
        by = {}
        for r in unexpected:
            by.setdefault(r["suite"], []).append(r.get("id") or r.get("test"))
        for suite, ids in by.items():
            g.problems.append(f"{suite}: {len(ids)} unexpected failure(s), e.g. {ids[:5]}")
    if g.problems:
        verdict, rc = ("BLOCKED" if tier == "release" else "FAIL"), 1
    elif open_hits:
        verdict, rc = (("BLOCKED", 1) if tier == "release" else ("PASS WITH OPEN KNOWN DEFECTS (not releasable)", 3))
        if tier == "release":
            g.problems.append(f"open known defects: {open_hits}")
    else:
        verdict, rc = "PASS", 0
    print(f"# gate {tier}: {verdict}")
    print(f"- source {(m.get('source') or {}).get('sha')} binary {binary.get('sha256', '?')[:16]}... build-id {binary.get('build_id')}")
    print("- identity (basis: recomputed = computed by the gate now; attested = recorded by a runner/operator, not proof):")
    for i in g.identity:
        print(f"    {i['item']}: {i['value']} [{i['basis']}] {i['status']}")
    for n in g.notes:
        print(f"- {n}")
    for d, n in sorted(open_hits.items()):
        title = next((x.get("title") for x in reg.get("defects") or [] if x["id"] == d), "")
        print(f"- OPEN KNOWN DEFECT {d}: {n} failing check(s): {title}")
    for p in g.problems:
        print(f"- PROBLEM: {p}")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"tier": tier, "verdict": verdict, "exit": rc, "binary": binary, "baseline": g.baseline,
                       "identity": g.identity, "open_known_defects": open_hits, "problems": g.problems, "notes": g.notes,
                       "proofs": {k: dict(v, targets=sorted(v["targets"])) for k, v in g.proofs.items()}}, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
