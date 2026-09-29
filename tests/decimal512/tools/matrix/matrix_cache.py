#!/usr/bin/env python3
"""matrix_cache.py - reuse a run_matrix.py result only when everything that decides it is byte-identical.

  matrix_cache.py fetch --cache DIR --matrix M --engine BIN --sql S --oracle O --runner RUN_MATRIX_PY \
                        --result OUT.result.jsonl --summary OUT.summary.json --reuse OUT.reuse.json
      exit 0: hit; the cached result and summary are copied to --result/--summary and --reuse records where they come
              from (origin run, creation time) and every hash that was checked. The result is historical evidence:
              it was NOT produced by the current run, and the reuse record says so.
      exit 1: miss (no entry for this key): run the matrix.
      exit 3: an entry exists for this key but fails verification (edited, truncated or foreign files): fail closed,
              never fall back silently.
  matrix_cache.py store --cache DIR --matrix M --engine BIN --sql S --oracle O --runner RUN_MATRIX_PY \
                        --result R --summary SM --origin JSON
      stores a fresh run (only when its summary attests exactly this engine, these inputs, this runner and this result
      file); an existing entry for the key is kept. exit 0 stored or already present, 2 refused.

Key = sha256 of the canonical JSON of: matrix name, engine sha256 and GNU build-id, SQL sha256, oracle sha256,
run_matrix.py sha256, python version, time zone (TZ, else the /etc/localtime target). Worker count and timing are not
part of it: the runner writes the same per-case result for any worker count (every matrix line is one self-contained
case; checked by gate_mutation_test.py and by the serial/parallel comparison of 2026-09-25).
PRODUCTION BOUNDARY: local files only; it never runs an engine and never contacts anything.
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_id(path):
    out = subprocess.run(["readelf", "-n", path], capture_output=True, text=True, timeout=120).stdout
    m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
    return m.group(1) if m else None


def time_zone():
    return os.environ.get("TZ") or os.path.realpath("/etc/localtime")


def key_material(a):
    return {"matrix": a.matrix, "engine_sha256": sha256_file(a.engine), "engine_build_id": build_id(a.engine),
            "sql_sha256": sha256_file(a.sql), "oracle_sha256": sha256_file(a.oracle), "runner_sha256": sha256_file(a.runner),
            "python": platform.python_version(), "tz": time_zone()}


def key_of(material):
    return hashlib.sha256(json.dumps(material, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def summary_binds(summary, material, result_sha):
    """the runner's own attestation must name exactly this engine, these inputs, this runner and this result file"""
    want = {"engine_sha256": material["engine_sha256"], "engine_build_id": material["engine_build_id"],
            "sql_sha256": material["sql_sha256"], "oracle_sha256": material["oracle_sha256"],
            "runner_sha256": material["runner_sha256"], "result_sha256": result_sha}
    return [k for k, v in want.items() if summary.get(k) != v]


def now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


def fetch(a):
    material = key_material(a)
    key = key_of(material)
    entry = os.path.join(a.cache, key)
    if not os.path.isdir(entry):
        print(json.dumps({"cache": "miss", "key": key}))
        return 1
    bad = []
    try:
        manifest = json.load(open(os.path.join(entry, "manifest.json")))
        if manifest.get("key") != key or manifest.get("key_material") != material:
            bad.append("manifest key or key material differs from the recomputed one")
        for name in ("result.jsonl", "summary.json"):
            p = os.path.join(entry, name)
            if not os.path.isfile(p) or sha256_file(p) != manifest.get("files", {}).get(name):
                bad.append(f"{name} does not match the manifest")
        if not bad:
            summary = json.load(open(os.path.join(entry, "summary.json")))
            miss = summary_binds(summary, material, sha256_file(os.path.join(entry, "result.jsonl")))
            if miss:
                bad.append(f"the cached summary does not attest {miss}")
    except (OSError, ValueError) as e:
        bad.append(f"unreadable entry: {e}")
    if bad:
        print(json.dumps({"cache": "invalid", "key": key, "entry": entry, "problems": bad}))
        return 3
    shutil.copyfile(os.path.join(entry, "result.jsonl"), a.result)
    shutil.copyfile(os.path.join(entry, "summary.json"), a.summary)
    record = {"reused": True, "note": "historical evidence reused from an earlier run; not re-run by this run",
              "cache_key": key, "key_material": material, "origin": manifest.get("origin"), "created_at": manifest.get("created_at"),
              "verified_at": now(), "files": {"result_sha256": sha256_file(a.result), "summary_sha256": sha256_file(a.summary)}}
    with open(a.reuse, "w") as f:
        json.dump(record, f, indent=1)
    print(json.dumps({"cache": "hit", "key": key, "origin": manifest.get("origin"), "created_at": manifest.get("created_at")}))
    return 0


def store(a):
    material = key_material(a)
    key = key_of(material)
    try:
        summary = json.load(open(a.summary))
    except (OSError, ValueError) as e:
        print(json.dumps({"cache": "refused", "reason": f"summary unreadable: {e}"}))
        return 2
    miss = summary_binds(summary, material, sha256_file(a.result))
    if miss:
        print(json.dumps({"cache": "refused", "reason": f"the summary does not attest {miss}"}))
        return 2
    entry = os.path.join(a.cache, key)
    if os.path.isdir(entry):
        print(json.dumps({"cache": "present", "key": key}))
        return 0
    os.makedirs(a.cache, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix=f".{key[:12]}.", dir=a.cache)
    shutil.copyfile(a.result, os.path.join(tmp, "result.jsonl"))
    shutil.copyfile(a.summary, os.path.join(tmp, "summary.json"))
    manifest = {"key": key, "key_material": material, "created_at": now(), "origin": json.loads(a.origin),
                "files": {n: sha256_file(os.path.join(tmp, n)) for n in ("result.jsonl", "summary.json")}}
    with open(os.path.join(tmp, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    try:
        os.rename(tmp, entry)
    except OSError:  # another run stored the same key first: keep that entry
        shutil.rmtree(tmp, ignore_errors=True)
    print(json.dumps({"cache": "stored", "key": key}))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["fetch", "store"])
    for k in ("--cache", "--matrix", "--engine", "--sql", "--oracle", "--runner", "--result", "--summary"):
        ap.add_argument(k, required=True)
    ap.add_argument("--reuse")
    ap.add_argument("--origin", default="{}")
    a = ap.parse_args()
    if a.cmd == "fetch":
        if not a.reuse:
            ap.error("fetch needs --reuse")
        return fetch(a)
    return store(a)


if __name__ == "__main__":
    sys.exit(main())
