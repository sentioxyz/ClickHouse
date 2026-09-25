#!/usr/bin/env python3
"""groupby_oracle.py - independent expectations for mixed_distributed_groupby.sh (standard library only).

The check's data is deterministic: each shard pair holds `numbers(0, 100000)` and `numbers(100000, 100000)`, so every
two-shard cluster aggregates n = 0..199999 with the column formulas below (the same as the INSERT in
mixed_distributed_groupby.sh). This computes the mathematically correct result of each key shape's
`SELECT <keys>, count(), sum(v) ... GROUP BY <keys> ORDER BY <keys>, c, s` in ClickHouse's TSV text
(NULL as \\N, NULLs last, Decimal trailing zeros trimmed), without any ClickHouse build.

  groupby_oracle.py expected <out-dir>       write <shape>.expected.tsv for every shape
  groupby_oracle.py lock <lock.json>         write the case lock (cases x expected sha256)
  groupby_oracle.py check <lock.json>        recompute and compare with the lock (exit 1 on any difference)
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
from decimal import Decimal

ROWS = 200000
P128 = 1 << 128

SHAPES = {  # shape -> key columns (in GROUP BY order)
    "nullable_u256_single": ["k_nu256"],
    "nullable_i64_x4": ["k_ni64_1", "k_ni64_2", "k_ni64_3", "k_ni64_4"],
    "u256_u128_48b": ["k_u256", "k_u128"],
    "int512_single": ["k_i512"],
    "decimal512_single": ["k_d512"],
    "u64_nullable_d76": ["k_u64", "k_nd76"],
    "u256_u64_40b": ["k_u256", "k_u64"],
}
NULLABLE_29_64 = ("nullable_u256_single", "nullable_i64_x4", "u64_nullable_d76")  # KeysNullMap shapes (nullable_keys512)
VARIANTS = ("default", "two_level", "two_level_memory_efficient", "two_level_not_memory_efficient")
# topology -> (initiator role, shard roles); old = baseline build, new = candidate build
TOPOLOGIES = {
    "new_only": ("new", ("new", "new")),
    "mixed_init_new": ("new", ("old", "new")),
    "mixed_init_old": ("old", ("old", "new")),
    "old_only": ("old", ("old", "old")),
}
GATED = ("new_only", "mixed_init_new", "mixed_init_old")  # old_only is recorded (baseline behaviour), not gated


def row(n: int) -> dict:
    """Column values of row n (None = NULL), exactly as the INSERT computes them."""
    return {
        "k_nu256": None if n % 4 == 0 else (n % 50) * P128,
        "k_ni64_1": None if n % 5 == 0 else n % 7,
        "k_ni64_2": None if n % 6 == 0 else n % 3,
        "k_ni64_3": n % 2,
        "k_ni64_4": None if n % 9 == 0 else n % 5,
        "k_u256": n % 40,
        "k_u128": n % 3,
        "k_i512": (n % 30) - 15,
        "k_d512": Decimal(f"{n % 25}.5"),
        "k_nd76": None if n % 3 == 0 else Decimal(n % 11),
        "k_u64": n % 13,
        "v": n,
    }


def fmt(v) -> str:
    if v is None:
        return "\\N"
    if isinstance(v, Decimal):
        s = format(v.normalize(), "f")
        return "0" if s in ("-0", "0") else s
    return str(v)


def sort_key(t):
    # ORDER BY k1, k2, ... ASC with NULLS LAST, then count, then sum
    keys, c, s = t
    return tuple((1, 0) if k is None else (0, k) for k in keys) + (c, s)


def expected(shape: str) -> str:
    cols = SHAPES[shape]
    agg = {}
    for n in range(ROWS):
        r = row(n)
        k = tuple(r[c] for c in cols)
        c, s = agg.get(k, (0, 0))
        agg[k] = (c + 1, s + r["v"])
    out = sorted(((k, c, s) for k, (c, s) in agg.items()), key=sort_key)
    return "".join("\t".join([fmt(x) for x in k] + [str(c), str(s)]) + "\n" for k, c, s in out)


def cases(sha: dict) -> list:
    res = []
    for topo, (init, shards) in TOPOLOGIES.items():
        for shape in SHAPES:
            for var in VARIANTS:
                res.append({"id": f"{topo}:{shape}:{var}", "topology": topo, "initiator": init, "shards": list(shards),
                            "shape": shape, "variant": var, "gated": topo in GATED,
                            "nullable_29_64": shape in NULLABLE_29_64, "expected_sha256": sha[shape]})
    return res


def lock_doc() -> dict:
    exp = {s: expected(s) for s in SHAPES}
    sha = {s: hashlib.sha256(t.encode()).hexdigest() for s, t in exp.items()}
    body = {"about": "mixed_distributed_groupby.sh cases; expectations computed by groupby_oracle.py (no ClickHouse build)",
            "rows": ROWS, "shapes": SHAPES, "variants": list(VARIANTS), "topologies": {k: [v[0], list(v[1])] for k, v in TOPOLOGIES.items()},
            "gated_topologies": list(GATED), "expected_groups": {s: t.count("\n") for s, t in exp.items()},
            "cases": cases(sha)}
    body["lock_sha256"] = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()
    return body


def main(argv) -> int:
    if len(argv) == 3 and argv[1] == "expected":
        os.makedirs(argv[2], exist_ok=True)
        for s in SHAPES:
            text = expected(s)
            open(os.path.join(argv[2], f"{s}.expected.tsv"), "w").write(text)
            print(s, text.count("\n"), hashlib.sha256(text.encode()).hexdigest())
        return 0
    if len(argv) == 3 and argv[1] == "lock":
        json.dump(lock_doc(), open(argv[2], "w"), indent=1, sort_keys=True)
        open(argv[2], "a").write("\n")
        return 0
    if len(argv) == 3 and argv[1] == "check":
        have = json.load(open(argv[2]))
        want = lock_doc()
        if have != want:
            print("lock differs from the oracle", file=sys.stderr)
            return 1
        print(f"lock matches the oracle: {len(want['cases'])} cases, lock_sha256 {want['lock_sha256'][:16]}")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
