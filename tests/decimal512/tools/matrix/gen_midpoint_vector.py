#!/usr/bin/env python3
"""Vector (non-constant) form of the midpoint matrix: every argument goes through materialize() and the query
reads FROM numbers(3) LIMIT 1 BY 1, so the function runs on full columns instead of constants. Case ids are the
constant ids with the prefix m0 -> m9; the oracle is the same as for the constant form.
  gen_midpoint_vector.py <matrix_vector.sql> <matrix_vector.oracle.jsonl>
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_midpoint_matrix as G  # noqa: E402


def lit(a, v):
    return f"CAST('{v}' AS {a[1]})" if a[0] == "other" else G.literal(a, v)


if __name__ == "__main__":
    cs = G.cases()
    with open(sys.argv[1], "w") as sql, open(sys.argv[2], "w") as orc:
        for cid, args, vals in cs:
            vid = "m9" + cid[2:]
            exp = {"error": "NO_COMMON_TYPE"} if any(a[0] == "other" for a in args) else G.expected(args, vals)
            expr = "midpoint(" + ", ".join(f"materialize({lit(a, v)})" for a, v in zip(args, vals)) + ")"
            sql.write(f"SELECT '{vid}', toTypeName({expr}), {expr} FROM numbers(3) LIMIT 1 BY 1;\n")
            orc.write(json.dumps({"id": vid, "category": "midpoint-reject" if "error" in exp else "midpoint-dec512",
                                  "args": [G.sql_type(a) if a[0] != "other" else a[1] for a in args], "vals": vals, "expected": exp}) + "\n")
    print(len(cs), "cases")
