#!/usr/bin/env python3
"""scan_wide_dispatch.py - find 256-bit dispatch sites that have no 512-bit counterpart (heuristic, read-only).

PRODUCTION BOUNDARY: reads a local git repository only (GIT_OPTIONAL_LOCKS=0, GIT_NO_LAZY_FETCH=1); never touches
production, never fetches, never checks out.

Why: Decimal512 port gaps compile cleanly and fail at run time. Almost every one found in the 2026-09 26.3 -> 26.8
port was a place that lists `Int256`/`UInt256`/`Decimal256` (a type list, a `castTypeToEither`, a switch, a
dispatch macro) or dispatches on a 32-byte size, but not the 512-bit case.

Two kinds of candidate sites, scanned in one revision with `git grep` (no checkout needed):
  type-256-without-512   a line mentioning a 256-bit type, also inside identifiers (`DataTypeInt256`,
                         `ColumnDecimal<Decimal256>`, `toInt256`), whose 512-bit counterpart OF THE SAME FAMILY is
                         not within +-WINDOW lines of the same file: integer (U)Int256 needs (U)Int512, Decimal256
                         needs Decimal512. Family matching matters: `IntegerTypes = TypeList<..., DataTypeInt256>`
                         next to `DecimalTypes = TypeList<..., DataTypeDecimal512>` is still a missing Int512.
  size-32-without-64     `case 32:` / `sizeof(...) == 32` / `== 32 ?` / `<something>size<...> == 32` with no `64`
                         counterpart within +-WINDOW lines
A candidate is a place to review, not a proven bug: many 256-only sites are legitimately 256-only.
Validated against the defects found in 2026-09 (see references/migration-ci-process.md): the scan flags KeysNullMap
(fixed), the GCD codec (fixed), the single-key method choosers in HashJoin/SetVariants, the integer list of
FunctionBinaryArithmetic and the getLeastSupertype ladder (all open), and not the Aggregator chooser (has 512).

Modes:
  --rev R                       candidates in R (paths default: src)
  --compare-rev OLD             also mark candidates of R that were COVERED in OLD (the same 256 line had a 512
                                counterpart in the old fork): the strongest signal of a lost port hunk
  --baseline reviewed.json      {site_key: {"decision": "...", "evidence": "..."}}; unreviewed candidates are listed
                                separately and make --fail-on-unreviewed exit 1 (for CI). Decision "legacy" marks a
                                candidate that existed when the baseline was started and was not reviewed one by one:
                                it is counted as debt, never as reviewed, and a LOST candidate (see --compare-rev)
                                with no decision or "legacy" is counted in "lost_unreviewed" (release gates require 0)
  --write-baseline out.json     write every current candidate with decision "legacy" (the start of a ratchet: from
                                then on only new candidates fail) and exit; review decisions replace "legacy" later
  --json                        machine-readable output
A site key is `<path>|<kind>|<normalized 256 line>` (line numbers excluded, so keys survive unrelated edits). With
--compare-rev a site also matches an old site whose line is the same once its 512-bit names are removed (a port
that drops `DataTypeInt512` from a type list changes the line itself); such matches are reported as `old_match: shape`.
Exit codes: 0 ok, 1 unreviewed candidates with --fail-on-unreviewed, 2 usage or git error.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import re
import subprocess
import sys

ENV = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0")
# (?<![0-9])/(?![0-9]) instead of \b: the 256-bit name is usually inside an identifier (DataTypeInt256, toUInt256).
FAMILIES = {
    "int": (re.compile(r"(?<![0-9])(U?Int256|wide::integer<256)(?![0-9])"),
            re.compile(r"(?<![0-9])(U?Int512|wide::integer<512)(?![0-9])")),
    "decimal": (re.compile(r"(?<![0-9])Decimal256(?![0-9])"), re.compile(r"(?<![0-9])Decimal512(?![0-9])")),
}
S32 = re.compile(r"(\bcase\s+32\s*:|sizeof\s*\([^)]*\)\s*==\s*32\b|==\s*32\s*\?|\b\w*[Ss]ize\w*(\(\))?\s*==\s*32\b)")
S64 = re.compile(r"(\bcase\s+64\s*:|sizeof\s*\([^)]*\)\s*==\s*64\b|==\s*64\s*\?|\b64\b)")
T512_TOKEN = re.compile(r"\b[\w:]*(?:U?Int512|Decimal512|wide::integer<512[^>]*>)[\w:]*(?:<[^<>]*>)?")
SOURCE_EXT = (".h", ".hpp", ".cpp", ".cc", ".inl")


def git(repo: str, *args: str) -> str:
    p = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, env=ENV)
    if p.returncode not in (0, 1):  # git grep exits 1 when nothing matches
        raise RuntimeError(f"git {' '.join(args[:3])}: {p.stderr.strip()[:300]}")
    return p.stdout


def norm(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def files_with_matches(repo: str, rev: str, paths: list[str]) -> list[str]:
    out = git(repo, "grep", "-l", "-E", r"Int256|Decimal256|wide::integer<256|case 32:|== 32", rev, "--", *paths)
    return sorted({l.split(":", 1)[1] for l in out.splitlines() if l.endswith(SOURCE_EXT) and ":" in l})


def scan_file(text: str, window: int) -> list[dict]:
    lines = text.splitlines()
    sites = []
    for i, line in enumerate(lines):
        if line.lstrip().startswith(("//", "*", "/*")):
            continue
        lo, hi = max(0, i - window), min(len(lines), i + window + 1)
        ctx = "\n".join(lines[lo:hi])
        fams = [f for f, (p256, _) in FAMILIES.items() if p256.search(line)]
        if fams:
            missing = [f for f in fams if not FAMILIES[f][1].search(ctx)]
            sites.append({"line": i + 1, "kind": "type-256-without-512", "covered": not missing, "missing": missing,
                          "text": norm(line)[:200]})
        if S32.search(line):
            # the 64 counterpart may be on the same line (`size == 32 || size == 64`), so only the 32 match is removed
            near = [S32.sub(" ", l) if j == i else l for j, l in enumerate(lines[lo:hi], lo)]
            covered = bool(S64.search("\n".join(near)))
            sites.append({"line": i + 1, "kind": "size-32-without-64", "covered": covered, "missing": [] if covered else ["64"],
                          "text": norm(line)[:200]})
    return sites


def shape(text: str) -> str:
    """The line without its 512-bit names (and the separators they leave behind)."""
    t = T512_TOKEN.sub("", text)
    for a, b in ((r",\s*,", ","), (r",\s*([>)\]])", r"\1"), (r"([<(\[])\s*,", r"\1"), (r"\s+", " ")):
        t = re.sub(a, b, t)
    return t.strip()


def scan_rev(repo: str, rev: str, paths: list[str], window: int) -> dict:
    result = {}
    for path in files_with_matches(repo, rev, paths):
        text = git(repo, "show", f"{rev}:{path}")
        for s in scan_file(text, window):
            key = f"{path}|{s['kind']}|{s['text']}"
            if key in result:  # identical lines in one file are one site: uncovered if any occurrence is uncovered
                r = result[key]
                r["lines"].append(s["line"])
                r["missing"] = sorted(set(r["missing"]) | set(s["missing"]))
                r["covered"] = r["covered"] and s["covered"]
                continue
            s.update(path=path, key=key, id=hashlib.sha1(key.encode()).hexdigest()[:10], lines=[s["line"]])
            result[key] = s
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--rev", required=True)
    ap.add_argument("--compare-rev")
    ap.add_argument("--paths", default="src")
    ap.add_argument("--window", type=int, default=3)
    ap.add_argument("--baseline")
    ap.add_argument("--fail-on-unreviewed", action="store_true")
    ap.add_argument("--write-baseline")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    paths = args.paths.split(",")
    try:
        rev_sha = git(args.repo, "rev-parse", f"{args.rev}^{{commit}}").strip()
        new = scan_rev(args.repo, args.rev, paths, args.window)
        old = scan_rev(args.repo, args.compare_rev, paths, args.window) if args.compare_rev else None
    except RuntimeError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    baseline = json.load(open(args.baseline, encoding="utf-8")) if args.baseline else {}
    candidates = [s for s in new.values() if not s["covered"]]
    old_shapes: dict = {}
    for o in (old or {}).values():
        k = (o["path"], o["kind"], shape(o["text"]))
        old_shapes[k] = old_shapes.get(k, False) or o["covered"]
    for s in candidates:
        o = old.get(s["key"]) if old is not None else None
        s["old"] = None if old is None else ("covered-in-old" if o and o["covered"] else ("absent-in-old" if not o else "uncovered-in-old"))
        if old is not None and s["old"] == "absent-in-old" and old_shapes.get((s["path"], s["kind"], shape(s["text"]))):
            s["old"], s["old_match"] = "covered-in-old", "shape"
        s["decision"] = baseline.get(s["key"], {}).get("decision")
    if args.write_baseline:
        with open(args.write_baseline, "w", encoding="utf-8") as f:
            json.dump({s["key"]: {"decision": "legacy", "evidence": f"present at {rev_sha[:11]} when the baseline was started"}
                       for s in sorted(candidates, key=lambda s: s["key"])}, f, indent=1, sort_keys=True)
        print(f"wrote {len(candidates)} candidate(s) as 'legacy' to {args.write_baseline}")
        return 0
    lost = [s for s in candidates if s.get("old") == "covered-in-old"]
    unreviewed = [s for s in candidates if not s["decision"]]
    legacy = [s for s in candidates if s["decision"] == "legacy"]
    lost_unreviewed = [s for s in lost if s["decision"] in (None, "legacy")]
    by_kind = collections.Counter(s["kind"] for s in candidates)
    summary = {"repo": args.repo, "rev": args.rev, "rev_sha": rev_sha, "compare_rev": args.compare_rev, "paths": paths,
               "window": args.window, "sites_total": len(new), "candidates": len(candidates), "by_kind": dict(by_kind),
               "lost_512_vs_compare_rev": len(lost), "unreviewed": len(unreviewed), "legacy": len(legacy),
               "lost_unreviewed": len(lost_unreviewed)}
    if args.json:
        print(json.dumps({"summary": summary, "candidates": sorted(candidates, key=lambda s: (s["path"], s["line"]))}, indent=1))
    else:
        print(f"# 256-bit dispatch scan of `{args.rev}` ({rev_sha[:11]}), paths {paths}, window +-{args.window}")
        print(f"- sites mentioning 256-bit types or 32-byte sizes: {len(new)}; without a 512/64 counterpart: {len(candidates)} {dict(by_kind)}")
        if old is not None:
            print(f"- compared with `{args.compare_rev}`: {len(lost)} candidate(s) had a 512/64 counterpart there (lost in the new revision)")
        print(f"- in baseline: {len(candidates) - len(unreviewed)} (of which legacy, not reviewed one by one: {len(legacy)}); "
              f"not in baseline: {len(unreviewed)}; lost and not reviewed: {len(lost_unreviewed)}")
        if lost:
            print("\n## Lost 512-bit handling (covered in the compare revision, uncovered now) - review first")
            for s in sorted(lost, key=lambda s: (s["path"], s["line"])):
                print(f"- `{s['path']}:{s['line']}` [{s['kind']}; missing {','.join(s['missing'])}] {s['text'][:150]}")
        print("\n## Candidates per file")
        per_file = collections.defaultdict(list)
        for s in candidates:
            per_file[s["path"]].append(s)
        for path in sorted(per_file, key=lambda p: -len(per_file[p])):
            ss = sorted(per_file[path], key=lambda s: s["line"])
            print(f"- `{path}`: {len(ss)} (lines {', '.join(str(s['line']) for s in ss[:12])}{' ...' if len(ss) > 12 else ''})")
    return 1 if (args.fail_on_unreviewed and unreviewed) else 0


if __name__ == "__main__":
    sys.exit(main())
