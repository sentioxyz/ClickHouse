#!/usr/bin/env python3
"""check_gate.py - decide a quick / full / release gate from an evidence manifest, fail-closed.

PRODUCTION BOUNDARY: reads local files and hashes local binaries only; runs no query, connects nowhere.

  check_gate.py --manifest evidence.json --known-defects known_defects.json [--tier quick|full|release] [--json out]

Exit codes: 0 PASS; 3 PASS WITH OPEN KNOWN DEFECTS (quick/full only: nothing unexpected failed, but the build is
not releasable); 1 FAIL or BLOCKED; 2 usage or unreadable input.

Rules (each violation is printed as a PROBLEM and makes the gate fail):
  * missing evidence is a failure: every suite the tier requires must be in the manifest and must have run; a suite
    with zero rows/tests, or any skipped test, is not a pass; `not_run` entries block a release
  * identity: the manifest names the source SHA (40 hex; release: clean tree), the binary path, sha256 and build-id;
    the gate recomputes sha256/build-id of the binary and of every matrix result's engine, so results produced by a
    different binary than the one being released fail the gate (artifact mismatch). Release also needs an image
    digest and the sha256 of the binary inside that image, equal to the tested binary
  * results are recomputed from the result files (run_matrix.py result.jsonl, clickhouse-test log, proof json,
    compat summaries, scan json), the summaries' counts are not trusted
  * known defects (registry): a failing row that matches an OPEN defect is reported as known-open, any other failing
    row is unexpected. An open defect whose checks ran and all passed is stale (update the registry in the same
    change); a selector that matches no row of a suite that ran is out of sync. Known-open failures never count as a
    pass: quick/full exit 3, release is BLOCKED while any defect is open
Manifest format: see references/migration-ci-process.md (section "Evidence manifest").
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

REQUIRED = {
    "quick": ["midpoint-matrix", "ops-matrix", "keys-matrix"],
    "full": ["midpoint-matrix", "midpoint-vector-matrix", "ops-matrix", "keys-matrix", "fork-stateless", "dispatch-scan"],
    "release": ["midpoint-matrix", "midpoint-vector-matrix", "ops-matrix", "keys-matrix", "fork-stateless", "dispatch-scan",
                "regression-proof", "compat-disk", "compat-protocol", "keeper-replication", "performance"],
}
HEX40, HEX64 = re.compile(r"^[0-9a-f]{40}$"), re.compile(r"^[0-9a-f]{64}$")


class Gate:
    def __init__(self, base: str, tier: str):
        self.base = base
        self.tier = tier
        self.problems: list[str] = []
        self.notes: list[str] = []
        self.failures: list[dict] = []   # {"suite", "id"/"test", "category", "op"}
        self.rows_seen: list[dict] = []  # every row/test with status, for registry staleness
        self._hash_cache: dict[str, tuple[str, str | None]] = {}

    def path(self, p: str) -> str:
        return p if os.path.isabs(p) else os.path.join(self.base, p)

    def ident(self, p: str) -> tuple[str, str | None]:
        real = os.path.realpath(p)
        if real not in self._hash_cache:
            h = hashlib.sha256()
            with open(real, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            bid = None
            try:
                out = subprocess.run(["readelf", "-n", real], capture_output=True, text=True, timeout=60).stdout
                m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
                bid = m.group(1) if m else None
            except (OSError, subprocess.TimeoutExpired):
                pass
            self._hash_cache[real] = (h.hexdigest(), bid)
        return self._hash_cache[real]


def check_identity(g: Gate, m: dict, tier: str) -> dict:
    src, b = m.get("source") or {}, m.get("binary") or {}
    if not HEX40.match(str(src.get("sha", ""))):
        g.problems.append("source.sha is not a 40-hex commit id")
    if tier == "release" and src.get("dirty") is not False:
        g.problems.append("release from a dirty or unknown tree (source.dirty must be false)")
    elif src.get("dirty"):
        g.notes.append("source tree is dirty (allowed below release)")
    bp = b.get("path")
    if not bp or not os.path.isfile(g.path(bp)):
        g.problems.append(f"binary.path missing or not a file: {bp}")
        return {}
    sha, bid = g.ident(g.path(bp))
    if b.get("sha256") != sha:
        g.problems.append(f"binary sha256 mismatch: manifest {b.get('sha256')} actual {sha}")
    if not bid:
        g.problems.append("binary has no GNU build-id (identity evidence missing)")
    elif b.get("build_id") != bid:
        g.problems.append(f"binary build-id mismatch: manifest {b.get('build_id')} actual {bid}")
    git_hash = src.get("binary_git_hash")
    if git_hash and git_hash != src.get("sha"):
        (g.problems if tier == "release" else g.notes).append(
            f"the binary embeds GIT_HASH {git_hash}, not source.sha {src.get('sha')} (stale build directory or wrong binary)")
    if tier == "release":
        img = m.get("image") or {}
        if not re.match(r"^sha256:[0-9a-f]{64}$", str(img.get("id", ""))):
            g.problems.append("release needs image.id (sha256:<64 hex>) of the local image; a tag is not an identity")
        if img.get("binary_sha256") != sha:
            g.problems.append(f"image.binary_sha256 {img.get('binary_sha256')} is not the tested binary {sha}")
    return {"sha256": sha, "build_id": bid, "path": bp}


def suite_matrix(g: Gate, s: dict, binary: dict) -> None:
    rows = []
    with open(g.path(s["result"]), encoding="utf-8") as f:
        rows = [json.loads(l) for l in f if l.strip()]
    if not rows:
        g.problems.append(f"{s['name']}: zero rows (a matrix that did not run is not a pass)")
        return
    engine = None
    if s.get("summary"):
        with open(g.path(s["summary"]), encoding="utf-8") as f:
            engine = json.load(f).get("engine")
    if not engine:
        g.problems.append(f"{s['name']}: no engine recorded (summary missing), cannot tie results to the binary")
    elif binary:
        if not os.path.isfile(engine):
            g.problems.append(f"{s['name']}: engine {engine} no longer exists, identity cannot be verified")
        elif g.ident(engine)[0] != binary["sha256"]:
            g.problems.append(f"{s['name']}: results come from {engine}, not the gated binary (artifact mismatch)")
    bad = [r for r in rows if r.get("status") not in ("PASS", "FAIL")]
    if bad:
        g.problems.append(f"{s['name']}: {len(bad)} row(s) with status other than PASS/FAIL (skips are not passes)")
    for r in rows:
        rec = {"suite": s["name"], "id": r["id"], "category": r.get("category"), "op": r.get("op") or (r.get("args") or [""])[0].split("(")[0], "status": r.get("status")}
        g.rows_seen.append(rec)
        if r.get("status") == "FAIL":
            g.failures.append(rec)
    g.notes.append(f"{s['name']}: {sum(r.get('status') == 'PASS' for r in rows)}/{len(rows)} pass")


def suite_clickhouse_test(g: Gate, s: dict, binary: dict) -> None:
    text = open(g.path(s["log"]), encoding="utf-8", errors="replace").read()
    selected = s.get("selected") or []
    if not selected:
        g.problems.append(f"{s['name']}: no selected test list (cannot prove the selector ran what it should)")
    if binary and s.get("build_id") != binary.get("build_id"):
        g.problems.append(f"{s['name']}: suite build_id {s.get('build_id')} is not the gated binary's {binary.get('build_id')}")
    # with -j every worker prints its own summary line (and MainProcess one for sequential tests): sum them
    m = re.findall(r"(?:Having (\d+) errors! )?(\d+) tests passed\. (\d+) tests skipped\.", text)
    if not m or "All tests have finished." not in text:
        g.problems.append(f"{s['name']}: no complete clickhouse-test summary in the log (run interrupted or never started)")
        return
    errors, passed, skipped = (sum(int(x[i] or 0) for x in m) for i in range(3))
    status = {}
    for name, st in re.findall(r"^(?:\[[^\]]*\]\s+)?(?:[\d:]+\s+)?(\S+?):\s+\[\s*(OK|FAIL|SKIPPED|UNKNOWN|BROKEN)\s*\]", text, re.M):
        status[name] = st
    if passed + errors == 0:
        g.problems.append(f"{s['name']}: zero tests ran")
    if skipped:
        g.problems.append(f"{s['name']}: {skipped} test(s) skipped (a skip is not a pass)")
    for x in s.get("excluded") or []:  # not run on purpose: a failure of its open known defect, never a pass
        rec = {"suite": s["name"], "test": x.get("test"), "status": "FAIL", "excluded": x.get("reason")}
        g.rows_seen.append(rec)
        g.failures.append(rec)
    for t in selected:
        st = status.get(t)
        if st is None:
            g.problems.append(f"{s['name']}: selected test {t} has no result line (not run)")
            continue
        rec = {"suite": s["name"], "test": t, "status": "PASS" if st == "OK" else "FAIL"}
        g.rows_seen.append(rec)
        if st != "OK":
            g.failures.append(rec)
    g.notes.append(f"{s['name']}: {passed} passed, {errors} failed, {skipped} skipped of {len(selected)} selected")


def suite_proof(g: Gate, s: dict, binary: dict) -> None:
    p = json.load(open(g.path(s["result"]), encoding="utf-8"))
    if p.get("verdict") != "PROVEN":
        g.problems.append(f"{s['name']}: regression proof verdict {p.get('verdict')}: {p.get('problems')}")
    if binary and (p.get("fixed") or {}).get("sha256") != binary["sha256"]:
        g.problems.append(f"{s['name']}: the proof's fixed binary is not the gated binary")
    g.notes.append(f"{s['name']}: {p.get('verdict')} ({(p.get('buggy') or {}).get('name')} -> {(p.get('fixed') or {}).get('name')})")


def suite_compat(g: Gate, s: dict, binary: dict) -> None:
    writers = set()
    for path in s.get("results") or []:
        text = open(g.path(path), encoding="utf-8").read()
        w = re.search(r"^writer=(\S+) write_rc=(\d+) self_read_rc=(\d+)", text, re.M)
        readers = re.findall(r"^(?:reader|mixed)=(\S+) rc=(\d+) (\S+)", text, re.M)
        if not w or w.group(2) != "0" or w.group(3) != "0":
            g.problems.append(f"{s['name']}: {path}: writer did not write and read back cleanly")
            continue
        writers.add(w.group(1))
        if not readers:
            g.problems.append(f"{s['name']}: {path}: no reader ran")
        for r, rc, verdict in readers:
            g.rows_seen.append({"suite": s["name"], "id": f"{w.group(1)}->{r}", "status": "PASS" if verdict == "SAME" else "FAIL"})
            if verdict != "SAME":
                g.problems.append(f"{s['name']}: {w.group(1)} -> {r}: {verdict} (rc={rc})")
    if len(writers) < 2:
        g.problems.append(f"{s['name']}: needs both directions (old writes/new reads and new writes/old reads), got writers {sorted(writers)}")
    g.notes.append(f"{s['name']}: writers {sorted(writers)}")


def suite_protocol(g: Gate, s: dict, binary: dict) -> None:
    """tests/compat/native_compat.sh native_matrix.tsv: every step rc 0; compared steps must be SAME."""
    lines = open(g.path(s["result"]), encoding="utf-8").read().splitlines()[1:]
    if not lines:
        g.problems.append(f"{s['name']}: no protocol steps ran")
    pairs = set()
    for line in lines:
        step, client, server, rc, _ = (line.split("\t") + [""] * 5)[:5]
        ok = rc == "0" and "(DIFF)" not in step
        rec = {"suite": s["name"], "id": step.split("(")[0], "status": "PASS" if ok else "FAIL"}
        g.rows_seen.append(rec)
        if not ok:
            g.failures.append(rec)
        if ok and "(SAME)" in step and client != server:
            pairs.add((client, server))
    if not any((b, a) in pairs for a, b in pairs):
        g.problems.append(f"{s['name']}: no verified (SAME) step in both directions between two versions: {sorted(pairs)}")
    g.notes.append(f"{s['name']}: {len(lines)} steps, verified directions {sorted(pairs)}")


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


KINDS = {"matrix": suite_matrix, "clickhouse-test": suite_clickhouse_test, "proof": suite_proof, "compat": suite_compat,
         "protocol": suite_protocol, "scan": suite_scan}


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
    ap.add_argument("--tier", choices=sorted(REQUIRED))
    ap.add_argument("--json")
    a = ap.parse_args()
    try:
        m = json.load(open(a.manifest, encoding="utf-8"))
        reg = json.load(open(a.known_defects, encoding="utf-8"))
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
    g = Gate(os.path.dirname(os.path.abspath(a.manifest)), tier)
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
        except (OSError, KeyError, json.JSONDecodeError) as e:
            g.problems.append(f"suite {s.get('name')!r}: unreadable evidence: {e}")
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
    for n in g.notes:
        print(f"- {n}")
    for d, n in sorted(open_hits.items()):
        title = next((x.get("title") for x in reg.get("defects") or [] if x["id"] == d), "")
        print(f"- OPEN KNOWN DEFECT {d}: {n} failing check(s): {title}")
    for p in g.problems:
        print(f"- PROBLEM: {p}")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"tier": tier, "verdict": verdict, "exit": rc, "binary": binary, "open_known_defects": open_hits,
                       "problems": g.problems, "notes": g.notes}, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
