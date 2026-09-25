#!/usr/bin/env python3
"""triage_server_tests.py - applicability triage of upstream fixes whose regression tests need a server.

PRODUCTION BOUNDARY: downloads public files of upstream pull requests from raw.githubusercontent.com (read-only,
unauthenticated) and runs them with tests/clickhouse-test against ONE isolated loopback instance at a time, started and
proven isolated by tools/isolated_ch.sh (instance d, optional embedded loopback Keeper). Nothing else is contacted.

  triage_server_tests.py stage <triage.tsv> <diff-cache> <fork-tree> <stage-dir> [--keeper] [--pyarrow]
  triage_server_tests.py run <stage-dir> <out.tsv> <label>=<binary> <label>=<binary> [--keeper] [--python-bin <dir>]

stage: for every `needs-server` test of the local triage (<triage.tsv> from triage_upstream_tests.py) that an isolated
  instance can run (not: external services; ZooKeeper only with --keeper; pyarrow only with --pyarrow), fetch the test and
  its reference from the 26.3 backport PR (refs/pull/<backport>/head; the upstream PR as a fallback), plus the data files
  of the PR the test names, into a private copy of the fork's tests/queries (<stage-dir>/tree/tests/queries). Tests that
  hard-code default ports of other servers are recorded as `refused-port` and not run (isolated_ch.sh would refuse the run).
run: runs the staged tests on each binary (first = the build under triage, second = a build with the fix) and classifies
  like the local triage: reproduced (first fails, second passes), not-reproduced (both pass), inconclusive (second fails),
  skipped (clickhouse-test skipped it on either).
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

RAW = "https://raw.githubusercontent.com/ClickHouse/ClickHouse/refs/pull/{pr}/head/{path}"
HERE = os.path.dirname(os.path.abspath(__file__))
# isolated_ch.sh: CH_ISO_SCRIPT, else next to this script (skill scripts/), else tests/decimal512/tools/
ISO = os.environ.get("CH_ISO_SCRIPT") or next((p for p in (os.path.join(HERE, "isolated_ch.sh"), os.path.join(HERE, "..", "isolated_ch.sh"))
                                              if os.path.isfile(p)), os.path.join(HERE, "..", "isolated_ch.sh"))
NEED = [("keeper", re.compile(r"Replicated|zookeeper|keeper|\{replica\}|\{shard\}|ON CLUSTER|DatabaseReplicated|generateSerialID", re.I)),
        ("external", re.compile(r"\bs3(Cluster)?\(|S3Queue|AzureQueue|azureBlobStorage|hdfs\(|Kafka|RabbitMQ|NATS|\bmysql\(|"
                                r"postgresql\(|mongodb|iceberg|deltaLake|hudi|minio|http://(?!localhost|127)|psql\b|mysql -", re.I)),
        ("pyarrow", re.compile(r"import pyarrow|pyarrow\.", re.I))]
DEFAULT_PORT = re.compile(r"(:|port[ =]*)(9000|9004|9005|9009|8123|9181|2181)\b")


def fetch(pr: str, path: str) -> bytes | None:
    for attempt in range(4):
        try:
            with urllib.request.urlopen(RAW.format(pr=pr, path=path), timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            time.sleep(5 * (attempt + 1))
        except OSError:
            time.sleep(5 * (attempt + 1))
    return None


def diff_files(diff_path: str) -> list[str]:
    return re.findall(r"(?m)^diff --git a/(\S+) b/", open(diff_path, encoding="utf-8", errors="replace").read())


def stage(tsv, cache, fork, out, keeper, pyarrow):
    q = os.path.join(out, "tree", "tests", "queries")
    if not os.path.isdir(q):
        shutil.copytree(os.path.join(fork, "tests", "queries"), q, symlinks=True)
        shutil.copy2(os.path.join(fork, "tests", "clickhouse-test"), os.path.join(out, "tree", "tests", "clickhouse-test"))
    rows = [dict(zip("pr backport tag area effect fork test PROD263 UP2688 cls".split(), l.rstrip("\n").split("\t")))
            for l in open(tsv, encoding="utf-8")][1:]
    res = open(os.path.join(out, "tests.tsv"), "w")
    res.write("pr\tbackport\ttest\tstatus\tsource\tfiles\tneeds\n")
    for r in rows:
        if r["cls"] != "needs-server":
            continue
        pr, bp, t = r["pr"], r["backport"], r["test"]
        files = diff_files(os.path.join(cache, f"{pr}.diff"))
        # every standard file of the test by name (a PR may change only the query file and keep the reference)
        mine = [f"tests/queries/0_stateless/{t}{ext}" for ext in (".sh", ".sql", ".sql.j2", ".expect", ".reference", ".reference.j2")]
        got, src, body = {}, "-", b""
        for source in (bp, pr):
            got = {}
            for p in mine:
                data = fetch(source, p)
                if data is not None:
                    got[p] = data
            if any(p.endswith((".sh", ".sql", ".sql.j2", ".expect")) for p in got) and any(".reference" in p for p in got):
                src = "backport" if source == bp else "pr"
                break
        if src == "-":
            res.write(f"{pr}\t{bp}\t{t}\tfetch-failed\t-\t-\t-\n")
            continue
        body = b"".join(v for p, v in got.items() if ".reference" not in p).decode("utf-8", "replace")
        needs = [n for n, rx in NEED if rx.search(body)]
        if body.count("#!/") and ".sh" in "".join(got):
            needs.append("sh")
        status = "staged"
        if "external" in needs:
            status = "not-runnable-external"
        elif "keeper" in needs and not keeper:
            status = "needs-keeper"
        elif "pyarrow" in needs and not pyarrow:
            status = "needs-pyarrow"
        elif DEFAULT_PORT.search(body):
            status = "refused-port"
        if status == "staged":
            for p, v in got.items():
                dst = os.path.join(out, "tree", p)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.islink(dst):
                    os.unlink(dst)
                open(dst, "wb").write(v)
                if p.endswith(".sh"):
                    os.chmod(dst, 0o755)
            # data files of the PR that the test names (binary fixtures are not in the text diff)
            for p in files:
                if p in mine or not p.startswith("tests/queries/0_stateless/") or os.path.basename(p) not in body \
                    or re.match(r"tests/queries/0_stateless/\d{5}_", p):
                    continue
                data = fetch(bp if src == "backport" else pr, p) or fetch(pr, p)
                if data is not None:
                    dst = os.path.join(out, "tree", p)
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    open(dst, "wb").write(data)
                    got[p] = data
        res.write(f"{pr}\t{bp}\t{t}\t{status}\t{src}\t{','.join(os.path.basename(p) for p in got)}\t{','.join(needs) or '-'}\n")
        res.flush()


def parse_log(log: str) -> dict[str, str]:
    st = {}
    for line in open(log, encoding="utf-8", errors="replace"):
        m = re.match(r"^(\S+):\s+\[ (OK|FAIL|SKIPPED|UNKNOWN) \]", line)
        if m:
            st[m.group(1)] = m.group(2)
    return st


def run(out_dir, out_tsv, labels, keeper, python_bin):
    tree = os.path.join(out_dir, "tree")
    staged = [l.rstrip("\n").split("\t") for l in open(os.path.join(out_dir, "tests.tsv"))][1:]
    staged = [s for s in staged if s[3] == "staged"]
    tests = sorted({s[2] for s in staged})
    env = dict(os.environ, CH_ISO_ROOT=os.path.join(out_dir, "iso"), CH_ISO_LOGS=os.path.join(out_dir, "logs"))
    if keeper:
        env["CH_ISO_KEEPER"] = "1"
    if python_bin:
        env["PATH"] = python_bin + os.pathsep + env["PATH"]
    results = {}
    for label, binary in labels:
        bid = subprocess.run(["readelf", "-n", binary], capture_output=True, text=True).stdout
        bid = re.search(r"Build ID: ([0-9a-f]+)", bid).group(1)[:12]
        r = subprocess.run(["bash", ISO, "start", "d", binary, tree], capture_output=True, text=True, env=env)
        if r.returncode:
            raise SystemExit(f"start {label}: {r.stdout}{r.stderr}")
        try:
            sel = [f"^{t}\\." for t in tests]
            r = subprocess.run(["bash", ISO, "test", "d", tree, f"triage_{label}", bid, "--no-random-settings",
                                "--no-random-merge-tree-settings", "--no-stateful", "-j", "4", *sel],
                               capture_output=True, text=True, env=env)
            log = r.stdout.strip().splitlines()[0] if r.stdout.strip() else ""
            results[label] = parse_log(log) if os.path.isfile(log) else {}
            print(f"{label}: rc={r.returncode} log={log} {r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ''}")
        finally:
            subprocess.run(["bash", ISO, "stop", "d"], capture_output=True, text=True, env=env)
            for f in os.listdir(os.path.join(out_dir, "iso", "bin")):
                os.unlink(os.path.join(out_dir, "iso", "bin", f))
    (l1, _), (l2, _) = labels
    with open(out_tsv, "w") as f:
        f.write(f"pr\tbackport\ttest\t{l1}\t{l2}\tclass\n")
        for pr, bp, t, *_ in staged:
            a, b = results[l1].get(t, "NONE"), results[l2].get(t, "NONE")
            if "SKIPPED" in (a, b) or "NONE" in (a, b):
                cls = "skipped"
            elif b != "OK":
                cls = "inconclusive"
            elif a == "OK":
                cls = "not-reproduced"
            else:
                cls = "reproduced"
            f.write(f"{pr}\t{bp}\t{t}\t{a}\t{b}\t{cls}\n")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "stage" and len(a) >= 5:
        stage(a[1], a[2], a[3], a[4], "--keeper" in a, "--pyarrow" in a)
    elif a and a[0] == "run" and len(a) >= 5:
        labels = [tuple(x.split("=", 1)) for x in a[3:5]]
        py = a[a.index("--python-bin") + 1] if "--python-bin" in a else None
        run(a[1], a[2], labels, "--keeper" in a, py)
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
