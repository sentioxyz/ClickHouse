#!/usr/bin/env python3
"""Application-SQL matrix: queries that sentio-core's event-log segmentation adaptor generates, on inline data, with an
independent oracle.

The keys matrix covers the KeysNullMap packing defect (KD-KEYSNULLMAP-512) with minimal shapes; these are real
application queries that reach it (found 2026-09-26). A multi-event segmentation query wraps every column in
toNullable() and selects NULL for a field that an event lacks, so a BigInt breakdown next to the hour bucket is a
40-byte nullable fixed key. On the 26.3 production build a394e92fb5d the GROUP BY merges NULL with 0 (and can move a
value into another key column), and the DISTINCT of the rolling unique-user and cumulative COUNT queries aborts (libc++
hardening assertion, zero-sized KeysNullMap); a build with the KeysNullMap fix returns the oracle.

Input: appsql_sentio_core.json next to this script, the SQL recorded from sentio-core (commit, test file and patch
hashes in its "source"). A case takes one recorded query verbatim, replaces the table names `segcompat_event_A` and
`segcompat_event_B` by inline values() tables holding the rows below, folds whitespace into single spaces and reduces the
result to one String: the rows "timestamp|agg|label" sorted and joined by ';' (NULL written as NULL). The "extended"
cases first apply the hand-made changes in EXTEND: they show that the query-level technique also fixes the paths the
application patch leaves alone; no application version generates them.

Oracle: computed here from the rows and the semantics of each query shape - GROUP BY with NULL as a group of its own;
a running window per label over hour buckets whose peers share a value; a LEFT JOIN of the pre-range aggregate in which
NULL never matches and an unmatched Nullable aggregate is NULL (count() defaults to 0); a 7-day RANGE frame in
descending date order; the first day per user and label - not recorded from a binary.

  gen_app_sql.py <out.sql> <out.oracle.jsonl>
Ids: m60000.. Categories:
  appsql-keysnullmap  application SQL (sentio-core as is, and proposed patch v1 for the DISTINCT query) that the
                      production build answers wrongly or aborts on: the regression-proof targets
  appsql-workaround   the same queries with the proposed patch v2's compatibility key: right on both builds
  appsql-extension    the hand-made extensions: right on both builds
  appsql-control      shapes outside the affected key range: right on both builds
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KEY = ", toLowCardinality(materialize(''))"
LC = ", toLowCardinality(materialize('')) AS _segcompat_k"
START, END = "2025-01-01 00:00:00", "2025-01-31 00:00:00"  # the adaptor tests' time range (mock.NewTimeRange)

META = ["`distinctId` String", "`meta.block_hash` String", "`meta.block_number` Int64", "`meta.chain` String",
        "`meta.log_index` Int64", "`meta.transaction_hash` String", "`meta.transaction_index` Int64",
        "`timestamp` DateTime64(6, \\'UTC\\')"]
# columns as sentio-core creates them (driver/timeseries/clickhouse metaToTable: dbTypeMapping, never Nullable)
STRUCT = {"A": META + ["`bf` Decimal(76, 30)", "`bi` Int256", "`i` Int64", "`s` String"],
          "B": META + ["`i` Int64", "`s` String"]}
# event A: ts, bi, i, s, bf, user; event B has no bi and no bf
ROWS = {
    "A": [("2025-01-01 10:15:00", 0, 0, "x", 0, "u1"), ("2025-01-01 10:16:00", 0, 0, "x", 0, "u2"),
          ("2025-01-01 10:17:00", 0, 0, "x", 0, "u3"), ("2025-01-01 10:45:00", 5, 5, "y", 5, "u1"),
          ("2025-01-01 10:46:00", 5, 5, "y", 5, "u4"), ("2025-01-02 03:00:00", 0, 0, "x", 0, "u5"),
          ("2024-12-31 23:00:00", 0, 7, "x", 0, "u6")],
    "B": [("2025-01-01 10:20:00", None, 1, "x", None, "u7"), ("2025-01-01 10:21:00", None, 1, "x", None, "u8"),
          ("2025-01-01 10:22:00", None, 1, "x", None, "u9"), ("2025-01-01 10:23:00", None, 1, "x", None, "u10"),
          ("2025-01-02 03:30:00", None, 1, "x", None, "u11"), ("2024-12-31 22:00:00", None, 3, "x", None, "u12")],
}
FIELD = {"bi": 1, "i": 2, "s": 3, "bf": 4}


def values_table(event):
    body, n = [], {"A": 0, "B": 100}[event]
    for ts, bi, i, s, bf, user in ROWS[event]:
        n += 1
        meta = f"'{user}', 'h{n}', {n}, '1', {n}, 't{n}', 0, '{ts}'"
        body.append(f"({meta}, {bf}, {bi}, {i}, '{s}')" if event == "A" else f"({meta}, {i}, '{s}')")
    return f"values('{', '.join(STRUCT[event])}', {', '.join(body)})"


def rows(events, when):
    out = []
    for e in events:
        for r in ROWS[e]:
            ts = r[0]
            if when == "range" and not (START <= ts <= END) or when == "before" and not ts < START:
                continue
            out.append({"ts": ts, "user": r[5], "bi": r[1], "i": r[2], "s": r[3], "bf": r[4]})
    return out


def hour(ts):
    return ts[:13] + ":00:00"


def day(ts):
    return ts[:10] + " 00:00:00"


def text(v):
    return "NULL" if v is None else str(v)


def canon(result):
    return ";".join(sorted("|".join(text(x) for x in row) for row in result))


def grouped(label, agg, events=("A", "B")):
    g = {}
    for r in rows(events, "range"):
        g.setdefault((hour(r["ts"]), r[label] if label else None), []).append(r["i"])
    fn = {"count": len, "sum": sum, "min": min}[agg]
    return [(b, fn(v), lab) if label else (b, fn(v)) for (b, lab), v in g.items()]


def cumulative(agg):
    """_pre_agg_ (per label over rows before the range, at the start bucket) UNION ALL the DISTINCT running values"""
    pre = {}
    for r in rows(("A", "B"), "before"):
        pre.setdefault(r["bi"], []).append(r["i"])
    fn = len if agg == "count" else sum
    out = [(hour(START), fn(v), lab) for lab, v in pre.items()]
    per = {}
    for r in rows(("A", "B"), "range"):
        per.setdefault(r["bi"], {}).setdefault(hour(r["ts"]), []).append(r["i"])
    main = set()
    for lab, buckets in per.items():
        running = 0
        base = fn(pre[lab]) if lab is not None and lab in pre else (0 if agg == "count" else None)
        for b in sorted(buckets):
            running += len(buckets[b]) if agg == "count" else sum(buckets[b])
            main.add((b, None if base is None else base + running, lab))
    return out + sorted(main, key=repr)


def lifetime():
    first = {}
    for r in rows(("A", "B"), "all"):
        k = (r["user"], r["bi"])
        first[k] = min(first.get(k, day(r["ts"])), day(r["ts"]))
    per = {}
    for (_, lab), d in first.items():
        per.setdefault(lab, {}).setdefault(d, 0)
        per[lab][d] += 1
    out = []
    for lab, days in per.items():
        total = 0
        for d in sorted(days):
            total += days[d]
            if START <= d <= END:
                out.append((d, total, lab))
    return out


def rolling():
    """uniq users per label over the dates [d, d + 6 days] (ORDER BY date DESC RANGE BETWEEN 6 PRECEDING AND CURRENT
    ROW); one row per label and day in the range"""
    import datetime
    users = {}
    for r in rows(("A", "B"), "all"):
        users.setdefault(r["bi"], {}).setdefault(r["ts"][:10], set()).add(r["user"])
    out = []
    for lab, days in users.items():
        for d in days:
            d0 = datetime.date.fromisoformat(d)
            seen = set().union(*(u for x, u in days.items() if d0 <= datetime.date.fromisoformat(x) <= d0 + datetime.timedelta(days=6)))
            if START <= d + " 00:00:00" <= END:
                out.append((d + " 00:00:00", len(seen), lab))
    return out


EXTEND = {
    "multi_cumulative_sum.extended": ("multi_cumulative_sum.v2", [
        ("FROM _before_ GROUP BY `bi`,`timestamp`)", "FROM _before_ GROUP BY `bi`,`timestamp`" + KEY + ")")]),
    "multi_lifetime_unique.extended": ("multi_lifetime_unique.v2", [
        ("SELECT DISTINCT count() as count,timestamp,`bi` FROM user_first_time GROUP BY timestamp,`bi`)",
         "SELECT DISTINCT count() as count,timestamp,`bi`" + LC + " FROM user_first_time GROUP BY timestamp,`bi`" + KEY + ")")]),
    "multi_rolling_unique.extended": ("multi_rolling_unique.v2", [
        ("AS rollup_aggr ,`bi` FROM _main_)", "AS rollup_aggr ,`bi`" + LC + " FROM _main_)"),
        ("rollup_aggr,`bi` FROM rollup_table WHERE", "rollup_aggr,`bi`" + LC + " FROM rollup_table WHERE")]),
}
# id, recorded query, category, label column of the result (None: no label), oracle
CASES = [
    ("m60000", "multi_total_bigint_null.orig", "appsql-keysnullmap", "bi", lambda: grouped("bi", "count")),
    ("m60001", "multi_prop_sum.orig", "appsql-keysnullmap", "bi", lambda: grouped("bi", "sum")),
    ("m60002", "multi_prop_min.orig", "appsql-keysnullmap", "bi", lambda: grouped("bi", "min")),
    ("m60003", "multi_distinct_bigint.orig", "appsql-keysnullmap", "bi", lambda: grouped("bi", "count")),
    ("m60004", "multi_distinct_bigint.v1", "appsql-keysnullmap", "bi", lambda: grouped("bi", "count")),
    ("m60005", "multi_rolling_unique.orig", "appsql-keysnullmap", "bi", rolling),
    ("m60006", "multi_cumulative_count.orig", "appsql-keysnullmap", "bi", lambda: cumulative("count")),
    ("m60007", "multi_cumulative_sum.orig", "appsql-keysnullmap", "bi", lambda: cumulative("sum")),
    ("m60008", "multi_lifetime_unique.orig", "appsql-keysnullmap", "bi", lifetime),
    ("m60010", "multi_total_bigint_null.v2", "appsql-workaround", "bi", lambda: grouped("bi", "count")),
    ("m60011", "multi_prop_sum.v2", "appsql-workaround", "bi", lambda: grouped("bi", "sum")),
    ("m60012", "multi_prop_min.v2", "appsql-workaround", "bi", lambda: grouped("bi", "min")),
    ("m60020", "multi_cumulative_sum.extended", "appsql-extension", "bi", lambda: cumulative("sum")),
    ("m60021", "multi_lifetime_unique.extended", "appsql-extension", "bi", lifetime),
    ("m60022", "multi_rolling_unique.extended", "appsql-extension", "bi", rolling),
    ("m60030", "multi_total_string.orig", "appsql-control", "s", lambda: grouped("s", "count")),
    ("m60031", "multi_total_bigfloat.orig", "appsql-control", "bf", lambda: grouped("bf", "count")),
    ("m60032", "single_total_bigint.orig", "appsql-control", "bi", lambda: grouped("bi", "count", events=("A",))),
    ("m60033", "multi_distinct.orig", "appsql-control", "i", lambda: grouped("i", "count")),
    ("m60034", "multi_total.orig", "appsql-control", None, lambda: grouped(None, "count")),
]


def query(recorded, name):
    if name in EXTEND:
        base, changes = EXTEND[name]
        sql = recorded[base]
        for old, new in changes:
            assert sql.count(old) == 1, (name, old)
            sql = sql.replace(old, new)
    else:
        sql = recorded[name]
    for event in ("A", "B"):
        sql = sql.replace(f"`segcompat_event_{event}`", values_table(event))
    assert "segcompat_event_" not in sql, name
    return re.sub(r"\s+", " ", sql).strip()


def main(sql_path, oracle_path):
    recorded = json.load(open(os.path.join(HERE, "appsql_sentio_core.json"), encoding="utf-8"))["sql"]
    with open(sql_path, "w") as out, open(oracle_path, "w") as orc:
        for cid, name, cat, label, oracle in CASES:
            parts = ["ifNull(toString(timestamp), 'NULL')", "ifNull(toString(agg), 'NULL')"]
            if label:
                parts.append(f"ifNull(toString(`{label}`), 'NULL')")
            row = "concat(" + ", '|', ".join(parts) + ")"
            out.write(f"SELECT '{cid}', toTypeName(c), c FROM (SELECT arrayStringConcat(arraySort(groupArray({row})), ';') AS c "
                      f"FROM ({query(recorded, name)}));\n")
            orc.write(json.dumps({"id": cid, "category": cat, "form": "app", "args": [name], "vals": [],
                                  "expected": {"type": "String", "value": canon(oracle())}}) + "\n")
    print(len(CASES), "cases")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
