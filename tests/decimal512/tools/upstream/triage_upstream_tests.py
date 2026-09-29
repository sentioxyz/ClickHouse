#!/usr/bin/env python3
"""triage_upstream_tests.py - applicability triage of upstream fixes by running their own regression tests.

PRODUCTION BOUNDARY: downloads public PR patches from GitHub (read-only, unauthenticated) and runs `clickhouse local`
with a private --path per test, stdin=/dev/null; nothing else. No server, no network access from the engines.

  triage_upstream_tests.py fetch <prs.tsv> <cache-dir>                 download <pr>.diff for every PR (skips cached ones)
  triage_upstream_tests.py run <prs.tsv> <cache-dir> <out.tsv> label=<binary> ...
  triage_upstream_tests.py prs-from-audit <audit.md> <prs.tsv>         PR table of the applicability audit -> TSV

prs.tsv columns: pr, backport, tag, area, effect, fork, trigger (as in the audit table).
For every PR, the stateless tests the PR adds or modifies (tests/queries/0_stateless/*.sql with a .reference) are run
with each binary and compared with the upstream reference (trailing whitespace ignored). Classification per test, with
the first label as the build under triage and the second as a build that contains the fix:
  reproduced        under-triage output differs from the reference, the fixed build matches: the fork is affected
  not-reproduced    both match: this test does not show the bug on the fork (the bug may still need other conditions)
  inconclusive      the fixed build does not match either (clickhouse local differs from the server runner, or the
                    test depends on something newer than the fix) - needs a manual look
  needs-server      .sh tests, or tags/features clickhouse local cannot provide (replication, distributed, zookeeper,
                    S3/HDFS/Kafka, clickhouse-test macros)
Nothing here is a verdict on its own: reproduced items get a manual reproduction and a minimal fix; the others stay
"upstream-known, not reproduced" or "unknown" in the applicability ledger.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

SERVER_TAGS = ("zookeeper", "replica", "shard", "distributed", "no-local", "use-rocksdb", "s3", "hdfs", "kafka", "cluster",
               "long", "race", "no-fasttest")
SERVER_FEATURES = re.compile(r"ReplicatedMergeTree|Distributed\(|remote\(|remoteSecure\(|cluster\(|clusterAllReplicas|"
                             r"s3\(|S3Queue|hdfs\(|Kafka|zookeeper|SYSTEM (SYNC|START|STOP)|ON CLUSTER|test_shard|"
                             r"test_cluster|parallel_replicas|\{CLICKHOUSE_(?!DATABASE)", re.I)
# clickhouse-test passes the test database as the query parameter CLICKHOUSE_DATABASE ({CLICKHOUSE_DATABASE:Identifier});
# clickhouse local gets the same parameters (the database is created first), so such tests run locally too
LOCAL_PARAMS = ["--param_CLICKHOUSE_DATABASE=triage_db", "--param_CLICKHOUSE_DATABASE_1=triage_db_1"]
LOCAL_PRELUDE = "CREATE DATABASE IF NOT EXISTS triage_db; CREATE DATABASE IF NOT EXISTS triage_db_1; USE triage_db;\n"


def read_prs(path):
    rows = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("pr\t") or not line.strip():
            continue
        c = line.rstrip("\n").split("\t")
        rows.append(dict(zip(("pr", "backport", "tag", "area", "effect", "fork", "trigger"), c)))
    return rows


def prs_from_audit(audit, out):
    pat = re.compile(r"^\| \[#(\d+)\]\([^)]*\)(?: bp #(\d+))? \| ([^|]*) \| ([^|]*) \| ([^|]*) \| ([^|]*) \| ([^|]*) \|")
    with open(out, "w", encoding="utf-8") as f:
        f.write("pr\tbackport\ttag\tarea\teffect\tfork\ttrigger\n")
        n = 0
        for line in open(audit, encoding="utf-8"):
            m = pat.match(line)
            if m:
                pr, bp, tag, area, trig, eff, fork = (x.strip() for x in m.groups())
                f.write("\t".join((pr, bp or "", tag, area, eff, fork, trig.replace("\t", " "))) + "\n")
                n += 1
    print(f"{n} PRs")


def fetch(prs, cache):
    os.makedirs(cache, exist_ok=True)
    import time
    for r in prs:
        path = os.path.join(cache, f"{r['pr']}.diff")
        if os.path.exists(path):
            continue
        url = f"https://patch-diff.githubusercontent.com/raw/ClickHouse/ClickHouse/pull/{r['pr']}.diff"
        for attempt in range(5):  # polite: one request at a time, back off on 429/503
            try:
                with urllib.request.urlopen(url, timeout=60) as resp:
                    data = resp.read(20_000_000)
                open(path, "wb").write(data)
                if os.path.exists(path + ".err"):
                    os.remove(path + ".err")
                break
            except Exception as e:  # noqa: BLE001 - recorded, not fatal
                open(path + ".err", "w").write(str(e))
                time.sleep(5 * 2 ** attempt)
        else:
            print(f"#{r['pr']}: {open(path + '.err').read()}", file=sys.stderr)
        time.sleep(2)


def tests_of(diff_text):
    """(name, sql, reference) for every stateless .sql test the patch adds or changes to its final content"""
    files = {}
    for m in re.finditer(r"^diff --git a/(\S+) b/(\S+)\n(.*?)(?=^diff --git |\Z)", diff_text, re.S | re.M):
        path, body = m.group(2), m.group(3)
        if not path.startswith("tests/queries/0_stateless/"):
            continue
        name = os.path.basename(path)
        if "new file mode" not in body:
            files[name] = None  # a modified test: its full new content is not in the patch
            continue
        hunks = body.split("\n@@")[1:]
        lines = []
        for h in hunks:
            for ln in h.split("\n")[1:]:
                if ln.startswith("+"):
                    lines.append(ln[1:])
        files[name] = "\n".join(lines) + ("\n" if lines else "")
    out = []
    for name, content in files.items():
        if name.endswith(".sql"):
            ref = files.get(name[:-4] + ".reference")
            out.append((name[:-4], content, ref))
        elif name.endswith((".sh", ".py", ".j2", ".expect")):
            out.append((name.rsplit(".", 1)[0], None, None))
    return out


def run_one(binary, sql, timeout=120):
    d = tempfile.mkdtemp(prefix="triage_")
    try:
        qf = os.path.join(d, "t.sql")
        open(qf, "w").write(LOCAL_PRELUDE + sql)
        p = subprocess.run([binary, "local", "--path", os.path.join(d, "data"), "--multiquery", "--ignore-error",
                            "--queries-file", qf] + LOCAL_PARAMS, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           timeout=timeout, cwd=d)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    finally:
        shutil.rmtree(d, ignore_errors=True)


def norm(s):
    # clickhouse-test prints the test database as "default"; do the same for the triage database
    s = s.replace("triage_db_1", "default_1").replace("triage_db", "default")
    return "\n".join(ln.rstrip() for ln in s.strip("\n").split("\n"))


def run(prs, cache, out, engines):
    with open(out, "w", encoding="utf-8") as f:
        f.write("pr\tbackport\ttag\tarea\teffect\tfork\ttest\t" + "\t".join(lab for lab, _ in engines) + "\tclass\n")
        for r in prs:
            path = os.path.join(cache, f"{r['pr']}.diff")
            if not os.path.exists(path):
                f.write(f"{r['pr']}\t{r['backport']}\t{r['tag']}\t{r['area']}\t{r['effect']}\t{r['fork']}\t-\t" + "\t".join("-" for _ in engines) + "\tno-patch\n")
                continue
            tests = tests_of(open(path, encoding="utf-8", errors="replace").read())
            if not tests:
                f.write(f"{r['pr']}\t{r['backport']}\t{r['tag']}\t{r['area']}\t{r['effect']}\t{r['fork']}\t-\t" + "\t".join("-" for _ in engines) + "\tno-stateless-test\n")
                continue
            for name, sql, ref in tests:
                if sql is None or ref is None:
                    cls, res = "needs-server", ["-"] * len(engines)
                else:
                    tags = re.search(r"^--\s*Tags:(.*)$", sql, re.M)
                    tagset = tags.group(1).lower() if tags else ""
                    if any(t in tagset for t in SERVER_TAGS) or SERVER_FEATURES.search(sql):
                        cls, res = "needs-server", ["-"] * len(engines)
                    else:
                        res = []
                        for lab, b in engines:
                            rc, so, se = run_one(b, sql)
                            res.append("match" if norm(so) == norm(ref) else ("timeout" if se == "timeout" else "diff"))
                        if len(res) >= 2:
                            cls = ("reproduced" if res[0] != "match" and res[1] == "match" else
                                   "not-reproduced" if res[0] == "match" and res[1] == "match" else "inconclusive")
                        else:
                            cls = "match" if res[0] == "match" else "diff"
                f.write(f"{r['pr']}\t{r['backport']}\t{r['tag']}\t{r['area']}\t{r['effect']}\t{r['fork']}\t{name}\t" + "\t".join(res) + f"\t{cls}\n")
                f.flush()


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "prs-from-audit":
        prs_from_audit(a[1], a[2])
    elif len(a) == 3 and a[0] == "fetch":
        fetch(read_prs(a[1]), a[2])
    elif len(a) >= 5 and a[0] == "run":
        engines = [tuple(x.split("=", 1)) for x in a[4:]]
        run(read_prs(a[1]), a[2], a[3], engines)
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
