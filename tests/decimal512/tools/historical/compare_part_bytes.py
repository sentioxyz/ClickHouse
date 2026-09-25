#!/usr/bin/env python3
"""compare_part_bytes.py - byte-compare the MergeTree parts two builds wrote from the same statements.

PRODUCTION BOUNDARY: reads two local `clickhouse local --path` directories; writes nothing but its report to stdout.

  compare_part_bytes.py <data-dir A> <data-dir B> [--skip-table name ...]

Tables are matched by name (metadata/<db>/<table>.sql holds the UUID of the table's store directory), parts by name,
files by name. Identical bytes for the same data mean identical on-disk layout: column files (.bin), marks, primary
and skip indexes, partition/minmax files, serialization and checksum files. A difference is listed with its file.
Exit 0 when every compared file is identical, 1 otherwise.
"""
from __future__ import annotations

import hashlib
import os
import re
import sys

PART_RE = re.compile(r"^[^_]+(_\d+){3}(_\d+)?$")


def tables(data: str) -> dict[str, str]:
    out = {}
    mdir = os.path.join(data, "metadata")
    for db in sorted(os.listdir(mdir)):
        dpath = os.path.join(mdir, db)
        if not os.path.isdir(dpath) or db in ("system", "information_schema", "INFORMATION_SCHEMA"):
            continue
        for f in sorted(os.listdir(dpath)):
            if not f.endswith(".sql"):
                continue
            text = open(os.path.join(dpath, f), encoding="utf-8", errors="replace").read()
            m = re.search(r"UUID '([0-9a-f-]{36})'", text)
            if m:
                u = m.group(1)
                out[f"{db}.{f[:-4]}"] = os.path.join(data, "store", u[:3], u)
    return out


def parts(tdir: str) -> dict[str, dict[str, str]]:
    out = {}
    if not os.path.isdir(tdir):
        return out
    for p in sorted(os.listdir(tdir)):
        pp = os.path.join(tdir, p)
        if os.path.isdir(pp) and PART_RE.match(p):
            files = {}
            for dp, dn, fn in os.walk(pp):
                for f in fn:
                    fp = os.path.join(dp, f)
                    files[os.path.relpath(fp, pp)] = hashlib.sha256(open(fp, "rb").read()).hexdigest()
            out[p] = files
    return out


def main() -> int:
    a, b = sys.argv[1], sys.argv[2]
    skip = set(sys.argv[sys.argv.index("--skip-table") + 1:]) if "--skip-table" in sys.argv else set()
    ta, tb = tables(a), tables(b)
    same = diff = 0
    problems = []
    for t in sorted(set(ta) | set(tb)):
        if t in skip or t.split(".", 1)[1] in skip:
            print(f"skip {t} (by request)")
            continue
        if t not in ta or t not in tb:
            problems.append(f"{t}: only in {'A' if t in ta else 'B'}")
            continue
        pa, pb = parts(ta[t]), parts(tb[t])
        if set(pa) != set(pb):
            problems.append(f"{t}: parts differ: A {sorted(pa)} B {sorted(pb)}")
            continue
        tsame = tdiff = 0
        for p in sorted(pa):
            for f in sorted(set(pa[p]) | set(pb[p])):
                if pa[p].get(f) == pb[p].get(f):
                    tsame += 1
                else:
                    tdiff += 1
                    problems.append(f"{t}/{p}/{f}: {'missing in B' if f not in pb[p] else 'missing in A' if f not in pa[p] else 'bytes differ'}")
        same += tsame
        diff += tdiff
        print(f"{t}: {len(pa)} part(s), {tsame} file(s) identical, {tdiff} different")
    for x in problems:
        print(f"  DIFF {x}")
    print(f"total: {same} identical file(s), {diff} different, {len(problems)} problem(s)")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
