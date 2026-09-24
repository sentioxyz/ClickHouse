#!/usr/bin/env python3
"""Composite and single-key matrix (fixed-size key packing, NULL vs 0) with an independent oracle.

Generalizes tests/probes/nullable_fixed_keys_512_bitmap.sql: instead of one shape and one NULL position it covers
key shapes whose fixed size lands in the 512-bit packed-key range (33..64 bytes, plus a <=32-byte control shape and
a non-nullable 40-byte shape), every NULL position, and GROUP BY / DISTINCT / IN (transform_null_in 0 and 1) / JOIN.
It also covers a single non-nullable and nullable (U)Int512 / Decimal512 key in IN, DISTINCT, GROUP BY and JOIN.

Oracle: rows are tuples in Python with None for NULL; the result of each query is computed from SQL semantics,
not recorded from a binary:
  GROUP BY / DISTINCT     NULL is one group value, distinct from 0
  IN, transform_null_in=0 a tuple containing NULL never matches
  IN, transform_null_in=1 NULL matches NULL
  JOIN USING              a key containing NULL never matches
Output rows use the run_matrix.py format: `SELECT 'mNNNNN', toTypeName(c), c FROM (SELECT count() AS c ...)`.

  gen_composite_keys.py <out.sql> <out.oracle.jsonl>
Ids: m70000.. (categories: keys-512, keys-control, single-key-512, single-key-control).
"""
import json
import sys

SHAPES = [
    ("5xNullable(UInt64) 40B", ["UInt64"] * 5, True, "keys-512"),
    ("4xNullable(UInt64)+Nullable(UInt32) 36B", ["UInt64"] * 4 + ["UInt32"], True, "keys-512"),
    ("7xNullable(UInt64) 56B", ["UInt64"] * 7, True, "keys-512"),
    ("8xNullable(Int64) 64B", ["Int64"] * 8, True, "keys-512"),
    ("Nullable(UInt256)+Nullable(UInt64) 40B", ["UInt256", "UInt64"], True, "keys-512"),
    ("2xNullable(Int128)+Nullable(UInt8) 33B", ["Int128", "Int128", "UInt8"], True, "keys-512"),
    ("Nullable(Decimal256(2))+Nullable(UInt64) 40B", ["Decimal256(2)", "UInt64"], True, "keys-512"),
    ("3xNullable(UInt64) 24B control", ["UInt64"] * 3, True, "keys-control"),
    ("5xUInt64 40B non-nullable", ["UInt64"] * 5, False, "keys-512"),
]


def col_types(types, nullable):
    return ", ".join(f"k{i} {'Nullable(' + t + ')' if nullable else t}" for i, t in enumerate(types))


def values_expr(types, nullable, rows):
    def v(x):
        return "NULL" if x is None else str(x)
    body = ", ".join("(" + ", ".join(v(x) for x in r) + ")" for r in rows)
    return f"values('{col_types(types, nullable)}', {body})"


def keys(n):
    return ", ".join(f"k{i}" for i in range(n))


def in_match(row, s, tni):
    for r in s:
        if all((a is None and b is None and tni) or (a is not None and b is not None and a == b) for a, b in zip(row, r)):
            return True
    return False


def join_count(left, right):
    return sum(1 for l in left for r in right if all(a is not None and b is not None and a == b for a, b in zip(l, r)))


def cases():
    out = []
    for name, types, nullable, cat in SHAPES:
        n = len(types)
        base = tuple(1 for _ in types)
        positions = range(n)
        for p in positions:
            other = None if nullable else 2      # the value that must stay distinct from 0 at position p
            r_other = tuple(other if i == p else x for i, x in enumerate(base))
            r_zero = tuple(0 if i == p else x for i, x in enumerate(base))
            rows = [base, r_other, r_zero, r_zero]
            ks = keys(n)
            t = values_expr(types, nullable, rows)
            tag = f"{name} pos{p}"
            out.append((cat, f"{tag} GROUP BY", f"SELECT count() AS c FROM (SELECT {ks} FROM {t} GROUP BY {ks})", len(set(rows))))
            out.append((cat, f"{tag} DISTINCT", f"SELECT count() AS c FROM (SELECT DISTINCT {ks} FROM {t})", len(set(rows))))
            for tni in (0, 1):
                probe, sset = [r_other, r_zero], [r_other]
                q = (f"SELECT count() AS c FROM {values_expr(types, nullable, probe)} WHERE ({ks}) IN "
                     f"(SELECT {ks} FROM {values_expr(types, nullable, sset)}) SETTINGS transform_null_in = {tni}")
                out.append((cat, f"{tag} IN other-set tni={tni}", q, sum(in_match(r, sset, tni) for r in probe)))
                sset2 = [r_zero]
                q2 = (f"SELECT count() AS c FROM {values_expr(types, nullable, probe)} WHERE ({ks}) IN "
                      f"(SELECT {ks} FROM {values_expr(types, nullable, sset2)}) SETTINGS transform_null_in = {tni}")
                out.append((cat, f"{tag} IN zero-set tni={tni}", q2, sum(in_match(r, sset2, tni) for r in probe)))
            for lname, lrows, rname, rrows in (("zero", [r_zero], "other", [r_other]), ("zero", [r_zero], "zero", [r_zero]),
                                               ("other", [r_other], "other", [r_other])):
                q = (f"SELECT count() AS c FROM {values_expr(types, nullable, lrows)} AS l INNER JOIN "
                     f"{values_expr(types, nullable, rrows)} AS r USING ({ks})")
                out.append((cat, f"{tag} JOIN {lname}-{rname}", q, join_count(lrows, rrows)))
    # single wide keys: 100 probe rows 0..99, set/build side = even numbers 0..98
    probe = list(range(100))
    build = [2 * i for i in range(50)]
    for tname, cat in (("Int512", "single-key-512"), ("UInt512", "single-key-512"), ("Decimal512(0)", "single-key-512"),
                       ("Int256", "single-key-control"), ("UInt256", "single-key-control")):
        for nullable in (False, True):
            t = f"Nullable({tname})" if nullable else tname
            cast = lambda e: f"CAST({e} AS {t})"
            tag = f"single {t}"
            out.append((cat, f"{tag} IN", f"SELECT count() AS c FROM (SELECT {cast('number')} AS k FROM numbers(100)) WHERE k IN (SELECT {cast('number * 2')} FROM numbers(50))",
                        sum(1 for x in probe if x in set(build))))
            out.append((cat, f"{tag} DISTINCT", f"SELECT count() AS c FROM (SELECT DISTINCT {cast('number % 7')} AS k FROM numbers(100))", len({x % 7 for x in probe})))
            out.append((cat, f"{tag} GROUP BY", f"SELECT count() AS c FROM (SELECT {cast('number % 7')} AS k FROM numbers(100) GROUP BY k)", len({x % 7 for x in probe})))
            out.append((cat, f"{tag} JOIN", f"SELECT count() AS c FROM (SELECT {cast('number')} AS k FROM numbers(100)) AS l INNER JOIN (SELECT {cast('number * 2')} AS k FROM numbers(50)) AS r USING (k)",
                        join_count([(x,) for x in probe], [(x,) for x in build])))
    return out


def main(sql_path, oracle_path):
    cs = cases()
    assert len(cs) < 10000
    with open(sql_path, "w") as sql, open(oracle_path, "w") as orc:
        for i, (cat, label, q, want) in enumerate(cs):
            cid = f"m{70000 + i:05d}"
            sql.write(f"SELECT '{cid}', toTypeName(c), c FROM ({q});\n")
            orc.write(json.dumps({"id": cid, "category": cat, "form": "vector", "args": [label], "vals": [],
                                  "expected": {"type": "UInt64", "value": str(want)}}) + "\n")
    print(len(cs), "cases")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
