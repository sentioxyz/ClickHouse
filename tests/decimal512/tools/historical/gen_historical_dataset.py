#!/usr/bin/env python3
"""gen_historical_dataset.py - a historical Decimal512/Int512 dataset and its independent oracle.

PRODUCTION BOUNDARY: pure Python; writes files into <out-dir> only. The SQL it writes is meant for `clickhouse local
--path <private dir>` of an isolated copy (tools/historical/hist_run.sh), never for a server that holds real data.

  gen_historical_dataset.py <out-dir>
     create.sql      schema and data, to be written by the OLD production binary (the frozen historical dataset)
     rewrite.sql     what a NEW binary does to a copy (new rows, merges, a mutation, a projection), before the old
                     binary reads it again (rollback after a rewrite)
     verify_initial.sql, verify_after_rewrite.sql   one statement per check, each tagged '<check id>' in its first column
     oracle.json     per table: column types and the exact raw integers of every row (initial and after rewrite);
                     per check: the expected output computed here with Python integers from those raw values

Every value is chosen here and inserted from its exact text (never from a float). The raw stored integer of a
Decimal(P, S) value v is v * 10^S; the oracle formats raw integers itself, so a binary that stores the right integer
but prints it wrongly (Decimal(154, 154)) fails the formatted checks and passes the raw ones: the two are reported
separately. The table `hist.oldbug` stores what the OLD binary writes through its known defective paths (a wrapped
parse, a wrapped multiplication, a wrapped scale-up, a truncating conversion); the oracle holds both the intended
mathematical value and the raw value the old binary stores, which is what the rows prove: a value inside the
Int512 range is not therefore correct.
"""
from __future__ import annotations

import json
import os
import sys

IMAX, IMIN, UMAX = 2 ** 511 - 1, -2 ** 511, 2 ** 512 - 1


def wrap(v: int, bits: int = 512) -> int:
    return (v + 2 ** (bits - 1)) % 2 ** bits - 2 ** (bits - 1)


def fmt(raw: int, scale: int, trailing: bool = True) -> str:
    """Text of a decimal with this raw integer and scale: with all `scale` digits (trailing=True: SQL literals), or as
    toString() prints it (trailing=False: no trailing zeros, whatever output_format_decimal_trailing_zeros says)."""
    neg = raw < 0
    s = str(abs(raw))
    if scale > 0:
        s = s.rjust(scale + 1, "0")
        ip, fp = s[:-scale], s[-scale:]
        if not trailing:
            fp = fp.rstrip("0")
        s = ip + ("." + fp if fp else "")
    return ("-" if neg else "") + s


def lit_dec(raw: int, scale: int, p: int = 154) -> str:
    return f"CAST('{fmt(raw, scale)}' AS Decimal({p}, {scale}))"


def main(out: str) -> None:
    os.makedirs(out, exist_ok=True)
    create, rewrite, verify = [], [], []
    oracle = {"tables": {}, "checks": []}
    S = lambda *a: create.append(" ".join(a))

    # ---- T1 scales: legal extremes of every scale of precision 154, with 128/256-bit controls, Wide and Compact
    cols = [("id", "UInt32", None), ("d0", "Decimal(154, 0)", 0), ("d1", "Decimal(154, 1)", 1), ("d18", "Decimal(154, 18)", 18),
            ("d76", "Decimal(154, 76)", 76), ("d153", "Decimal(154, 153)", 153), ("d154", "Decimal(154, 154)", 154),
            ("n154", "Nullable(Decimal(154, 154))", 154), ("c128", "Decimal(38, 10)", 10), ("c256", "Decimal(76, 38)", 38)]
    lim = {"c128": 10 ** 38 - 1, "c256": 10 ** 76 - 1}
    families = []
    # raw values per row: (label, function(column) -> raw or None)
    def famrow(label, f):
        families.append((label, f))
    famrow("zero", lambda c: 0)
    famrow("plus_ulp", lambda c: 1)
    famrow("minus_ulp", lambda c: -1)
    famrow("token_amount", lambda c: 123456789 * 10 ** min(18, scale_of(c)) + 7)
    famrow("negative_token_amount", lambda c: -(987654321 * 10 ** min(18, scale_of(c)) + 3))
    famrow("near_max", lambda c: lim.get(c, IMAX) - 10 ** 5)
    famrow("near_min", lambda c: -lim.get(c, IMAX) + 10 ** 5 if c in lim else IMIN + 10 ** 5)
    famrow("max", lambda c: lim.get(c, IMAX))
    famrow("min", lambda c: -lim[c] if c in lim else IMIN)
    famrow("half_of_scale154_unit", lambda c: 5 * 10 ** 153 if scale_of(c) == 154 else 5 * 10 ** (scale_of(c) - 1) if scale_of(c) > 0 else 5)
    famrow("null_or_small", lambda c: None if c == "n154" else 42)

    def scale_of(c):
        return dict((n, s) for n, t, s in cols)[c]

    rows = []
    for i, (label, f) in enumerate(families):
        rows.append([i] + [f(c) for c, t, s in cols[1:]])
    for tname, extra in (("scales_wide", "SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0"), ("scales_compact", "")):
        S(f"CREATE TABLE hist.{tname} ({', '.join(f'{c} {t}' for c, t, s in cols)}) ENGINE = MergeTree ORDER BY id {extra};")
        vals = []
        for r in rows:
            parts = [str(r[0])]
            for (c, t, s), v in zip(cols[1:], r[1:]):
                p = 38 if c == "c128" else 76 if c == "c256" else 154
                parts.append("NULL" if v is None else lit_dec(v, s, p))
            vals.append("(" + ", ".join(parts) + ")")
        S(f"INSERT INTO hist.{tname} VALUES {', '.join(vals)};")
        oracle["tables"][tname] = {"columns": [[c, t] for c, t, s in cols], "rows": rows, "labels": [l for l, _ in families]}

    # ---- T2 integers
    icols = [("id", "UInt32"), ("i", "Int512"), ("u", "UInt512"), ("ni", "Nullable(Int512)"), ("nu", "Nullable(UInt512)"),
             ("c_i256", "Int256"), ("c_u256", "UInt256"), ("c_i128", "Int128")]
    irows = [[0, 0, 0, 0, 0, 0, 0, 0], [1, 1, 1, None, None, 1, 1, 1], [2, -1, UMAX, -1, UMAX, -1, 2 ** 256 - 1, -1],
             [3, IMAX, 2 ** 511, IMAX, 2 ** 511, 2 ** 255 - 1, 2 ** 255, 2 ** 127 - 1], [4, IMIN, 2 ** 511 - 1, IMIN, None, -2 ** 255, 0, -2 ** 127],
             [5, 10 ** 150, 10 ** 154, None, 10 ** 154, 10 ** 76, 10 ** 77, 10 ** 38], [6, -10 ** 150, 1, -10 ** 150, 1, -10 ** 76, 1, -10 ** 38]]
    S(f"CREATE TABLE hist.ints ({', '.join(f'{c} {t}' for c, t in icols)}) ENGINE = MergeTree ORDER BY id;")
    ivals = []
    for r in irows:
        parts = [str(r[0])]
        for (c, t), v in zip(icols[1:], r[1:]):
            base = t.replace("Nullable(", "").rstrip(")")
            parts.append("NULL" if v is None else f"to{base}('{v}')")
        ivals.append("(" + ", ".join(parts) + ")")
    S(f"INSERT INTO hist.ints VALUES {', '.join(ivals)};")
    oracle["tables"]["ints"] = {"columns": [list(x) for x in icols], "rows": irows}

    # ---- T3 keys: Decimal512 partition key, sort/primary key, skip indexes, small granules
    kcols = [("id", "UInt32"), ("p", "Decimal(154, 0)"), ("k", "Decimal(154, 18)"), ("ik", "Int512"), ("nk", "Nullable(Decimal(154, 2))")]
    krows = []
    for n in range(240):
        p = [0, 1, -1, IMAX][n % 4]
        k = (n - 120) * 10 ** 30 + n * 7 if n not in (0, 239) else (IMIN if n == 0 else IMAX)
        ik = [IMIN, -1, 0, 1, IMAX][n % 5] if n % 7 == 0 else n * 10 ** 140 - 10 ** 150
        nk = None if n % 6 == 0 else (n - 100) * 10 ** 100 + n
        krows.append([n, p, k, ik, nk])
    S("CREATE TABLE hist.keys (id UInt32, p Decimal(154, 0), k Decimal(154, 18), ik Int512, nk Nullable(Decimal(154, 2)), "
      "INDEX i_minmax ik TYPE minmax GRANULARITY 1, INDEX i_set k TYPE set(100) GRANULARITY 1, INDEX i_bf ik TYPE bloom_filter GRANULARITY 1) "
      "ENGINE = MergeTree PARTITION BY p ORDER BY (k, id) PRIMARY KEY k SETTINGS index_granularity = 8;")
    for chunk in (krows[:120], krows[120:]):  # two inserts: several parts per partition
        S("INSERT INTO hist.keys VALUES " + ", ".join(
            f"({r[0]}, {lit_dec(r[1], 0)}, {lit_dec(r[2], 18)}, toInt512('{r[3]}'), {'NULL' if r[4] is None else lit_dec(r[4], 2)})" for r in chunk) + ";")
    oracle["tables"]["keys"] = {"columns": [list(x) for x in kcols], "rows": krows}

    # ---- T4 aggregate states
    agg_src = [(k % 3, (k - 5) * 10 ** 40 + k, -k * 10 ** 100, k * 10 ** 120, (k % 4) * 10 ** 140, k * 10 ** 150 - 7 * 10 ** 149, (k * 3 - 10) * 10 ** 20) for k in range(12)]
    S("CREATE TABLE hist.agg (k UInt8, s AggregateFunction(sum, Decimal(154, 18)), mn AggregateFunction(min, Int512), mx AggregateFunction(max, UInt512), "
      "ue AggregateFunction(uniqExact, Int512), ga AggregateFunction(groupArray, Decimal(154, 154)), q AggregateFunction(quantileExact, Decimal(154, 2)), "
      "ss SimpleAggregateFunction(sum, Decimal(154, 18))) ENGINE = AggregatingMergeTree ORDER BY k;")
    S("INSERT INTO hist.agg SELECT k, sumState(s), minState(mn), maxState(mx), uniqExactState(ue), groupArrayState(ga), quantileExactState(q), sum(s) FROM (SELECT * FROM values("
      "'k UInt8, s Decimal(154, 18), mn Int512, mx UInt512, ue Int512, ga Decimal(154, 154), q Decimal(154, 2)', "
      + ", ".join(f"({a}, '{fmt(b, 18)}', '{c}', '{d}', '{e}', '{fmt(f_ % 10 ** 153, 154)}', '{fmt(g, 2)}')" for a, b, c, d, e, f_, g in agg_src)
      + ")) GROUP BY k;")
    oracle["tables"]["agg_src"] = {"rows": [list(x) for x in agg_src]}

    # ---- T5 materialized view and projection
    S("CREATE TABLE hist.mv_src (k UInt8, v Decimal(154, 18)) ENGINE = MergeTree ORDER BY k;")
    S("CREATE TABLE hist.mv_sum (k UInt8, v Decimal(154, 18), c UInt64) ENGINE = SummingMergeTree ORDER BY k;")
    S("CREATE MATERIALIZED VIEW hist.mv TO hist.mv_sum AS SELECT k, sum(v) AS v, count() AS c FROM hist.mv_src GROUP BY k;")
    mv_rows = [(k % 4, (k * 37 - 200) * 10 ** 60 + k) for k in range(40)]
    S("INSERT INTO hist.mv_src VALUES " + ", ".join(f"({a}, {lit_dec(b, 18)})" for a, b in mv_rows) + ";")
    S("CREATE TABLE hist.proj (k UInt8, v Decimal(154, 18), PROJECTION p_sum (SELECT k, sum(v) GROUP BY k)) ENGINE = MergeTree ORDER BY tuple();")
    S("INSERT INTO hist.proj VALUES " + ", ".join(f"({a}, {lit_dec(b, 18)})" for a, b in mv_rows) + ";")
    oracle["tables"]["mv_src"] = {"rows": [list(x) for x in mv_rows]}

    # ---- T6 Dynamic / JSON / nested (JSON numbers are quoted: the JSON type parses an unquoted number as Float64 before
    #      casting it to a typed Decimal path, which loses digits beyond about 17 significant ones - upstream behaviour,
    #      reported separately; the dataset stores exact values to test decoding)
    S("CREATE TABLE hist.dyn (id UInt32, dy Dynamic, dys Dynamic(max_types = 1), j JSON(a Decimal(154, 3)), arr Array(Decimal(154, 18)), "
      "m Map(String, Int512), t Tuple(x Decimal(154, 154), y Nullable(Int512))) ENGINE = MergeTree ORDER BY id;")
    dyn_vals = []
    dyn_rows = []
    for n in range(9):
        kind = n % 3
        dv = [f"CAST(toInt512('{IMIN + n}') AS Dynamic)", f"CAST(toUInt512('{UMAX - n}') AS Dynamic)", f"CAST({lit_dec(IMAX - n, 2)} AS Dynamic)"][kind]
        ja = (n - 4) * 10 ** 140 + n
        arr = [IMAX - n, IMIN + n, n]
        mv = n * 10 ** 150 - 10 ** 152
        tx, ty = (n - 4) * 10 ** 152, (None if n % 2 else IMIN + n)
        dyn_vals.append(f"({n}, {dv}, {dv}, CAST('{{\"a\": \"{fmt(ja, 3)}\", \"b\": {n}}}' AS JSON(a Decimal(154, 3))), "
                        f"[{', '.join(lit_dec(x, 18) for x in arr)}], map('k', toInt512('{mv}')), "
                        f"tuple({lit_dec(tx, 154)}, {'NULL' if ty is None else f'toInt512({chr(39)}{ty}{chr(39)})'}))")
        dyn_rows.append({"id": n, "kind": ["Int512", "UInt512", "Decimal(154, 2)"][kind], "dy_raw": [IMIN + n, UMAX - n, IMAX - n][kind],
                         "ja": ja, "arr": arr, "m": mv, "tx": tx, "ty": ty})
    S("INSERT INTO hist.dyn VALUES " + ", ".join(dyn_vals) + ";")
    oracle["tables"]["dyn"] = {"rows": dyn_rows}

    # ---- T7 Replacing / Summing merges
    S("CREATE TABLE hist.replacing (k Int512, ver UInt64, v Decimal(154, 18)) ENGINE = ReplacingMergeTree(ver) ORDER BY k;")
    rep = [(IMIN, 1, 5), (IMIN, 2, -7), (0, 1, IMAX), (0, 3, IMIN), (IMAX, 2, 11), (IMAX, 1, 13)]
    for r in rep:
        S(f"INSERT INTO hist.replacing VALUES (toInt512('{r[0]}'), {r[1]}, {lit_dec(r[2], 18)});")
    S("CREATE TABLE hist.summing (k Decimal(154, 0), v Decimal(154, 18)) ENGINE = SummingMergeTree ORDER BY k;")
    summ = [(IMIN, 10 ** 150), (IMIN, -3 * 10 ** 149), (IMAX, 7), (IMAX, -7), (0, 10 ** 100), (0, 10 ** 100)]
    for r in summ:
        S(f"INSERT INTO hist.summing VALUES ({lit_dec(r[0], 0)}, {lit_dec(r[1], 18)});")
    oracle["tables"]["replacing"] = {"rows": [list(x) for x in rep]}
    oracle["tables"]["summing"] = {"rows": [list(x) for x in summ]}

    # ---- T9 every codec the fork accepts on 512-bit columns (Delta needs an explicit byte size; DoubleDelta, Gorilla
    #      and T64 are rejected for these types at CREATE, so no historical part can use them)
    ccols = [("id", "UInt32", ""), ("dz", "Decimal(154, 18)", "ZSTD(3)"), ("dd", "Decimal(154, 18)", "Delta(8), ZSTD(1)"),
             ("dg", "Decimal(154, 18)", "GCD, LZ4"), ("dh", "Decimal(154, 18)", "LZ4HC(9)"), ("dn", "Decimal(154, 18)", "NONE"),
             ("ig", "Int512", "GCD, LZ4"), ("i8", "Int512", "Delta(8), ZSTD(1)"), ("ug", "UInt512", "GCD, ZSTD(1)")]
    crows = []
    for n in range(300):
        dv = (n - 150) * 7 * 10 ** 40 if n not in (0, 299) else (IMIN if n == 0 else IMAX)
        iv = (n - 150) * 3 * 10 ** 120 if n not in (1, 298) else (IMIN if n == 1 else IMAX)
        uv = n * 5 * 10 ** 130 if n != 297 else UMAX
        crows.append([n, dv, dv, dv, dv, dv, iv, iv, uv])
    S("CREATE TABLE hist.codecs (" + ", ".join(f"{c} {t}" + (f" CODEC({k})" if k else "") for c, t, k in ccols) + ") ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 64;")
    S("INSERT INTO hist.codecs VALUES " + ", ".join(
        f"({r[0]}, {lit_dec(r[1], 18)}, {lit_dec(r[2], 18)}, {lit_dec(r[3], 18)}, {lit_dec(r[4], 18)}, {lit_dec(r[5], 18)}, toInt512('{r[6]}'), toInt512('{r[7]}'), toUInt512('{r[8]}'))"
        for r in crows) + ";")
    oracle["tables"]["codecs"] = {"columns": [[c, t] for c, t, k in ccols], "rows": crows}

    # ---- T8 values the OLD binary stores through its defective paths (intended vs stored)
    ob = [
        ("parse_wrap_scale0", "d0", 0, f"CAST('{2 ** 511}' AS Decimal(154, 0))", 2 ** 511, wrap(2 ** 511)),
        ("parse_wrap_153_digits_scale1", "d1", 1, f"CAST('{'9' * 153}' AS Decimal(154, 1))", (10 ** 153 - 1) * 10, wrap((10 ** 153 - 1) * 10)),
        ("multiply_wrap", "d0", 0, f"CAST('{IMAX}' AS Decimal(154, 0)) * toInt8(2)", IMAX * 2, wrap(IMAX * 2)),
        ("scaleup_wrap_plus", "d100", 100, f"plus(CAST('{10 ** 60}' AS Decimal(154, 0)), CAST('0.5' AS Decimal(154, 100)))",
         10 ** 160 + 5 * 10 ** 99, wrap(wrap(10 ** 160) + 5 * 10 ** 99)),
        ("int512_to_decimal256_truncation", "c256", 0, f"toDecimal256(toInt512('{10 ** 90}'), 0)", 10 ** 90, wrap(10 ** 90, 256)),
        ("uint512_to_decimal512_wrap", "d0", 0, f"toDecimal512(toUInt512('{2 ** 511}'), 0)", 2 ** 511, wrap(2 ** 511)),
        ("uint256_to_decimal256_upstream", "c256", 0, f"toDecimal256(toUInt256('{2 ** 256 - 1}'), 0)", 2 ** 256 - 1, -1),
    ]
    S("CREATE TABLE hist.oldbug (id UInt32, path String, d0 Decimal(154, 0), d1 Decimal(154, 1), d100 Decimal(154, 100), c256 Decimal(76, 0)) ENGINE = MergeTree ORDER BY id;")
    obrows = []
    for n, (path, col, scale, expr, intended, stored) in enumerate(ob):
        vals = {"d0": "0", "d1": "0", "d100": "0", "c256": "0"}
        vals[col] = expr
        S(f"INSERT INTO hist.oldbug VALUES ({n}, '{path}', {vals['d0']}, {vals['d1']}, {vals['d100']}, {vals['c256']});")
        obrows.append({"id": n, "path": path, "column": col, "scale": scale, "intended_raw": intended, "old_binary_stores_raw": stored,
                       "fits_int512": IMIN <= intended <= IMAX if col != "c256" else -2 ** 255 <= intended < 2 ** 255})
    oracle["tables"]["oldbug"] = {"rows": obrows}

    # ---- rewrite by the NEW binary: new rows, merges, a mutation, a projection materialized, more MV input
    rewrite.append("INSERT INTO hist.scales_wide SELECT id + 100, d0, d1, d18, d76, d153, d154, n154, c128, c256 FROM hist.scales_wide;")
    rewrite.append("INSERT INTO hist.keys SELECT id + 1000, p, k, ik, nk FROM hist.keys WHERE id % 3 = 0;")
    rewrite.append("INSERT INTO hist.mv_src VALUES " + ", ".join(f"({a}, {lit_dec(b, 18)})" for a, b in mv_rows[:10]) + ";")
    rewrite.append("INSERT INTO hist.agg SELECT k, sumState(s), minState(mn), maxState(mx), uniqExactState(ue), groupArrayState(ga), quantileExactState(q), sum(s) FROM (SELECT * FROM values("
                   "'k UInt8, s Decimal(154, 18), mn Int512, mx UInt512, ue Int512, ga Decimal(154, 154), q Decimal(154, 2)', "
                   + ", ".join(f"({a}, '{fmt(b, 18)}', '{c}', '{d}', '{e}', '{fmt(f_ % 10 ** 153, 154)}', '{fmt(g, 2)}')" for a, b, c, d, e, f_, g in agg_src[:6])
                   + ")) GROUP BY k;")
    for t in ("scales_wide", "scales_compact", "ints", "keys", "agg", "mv_sum", "proj", "dyn", "replacing", "summing", "oldbug", "codecs"):
        rewrite.append(f"OPTIMIZE TABLE hist.{t} FINAL;")
    rewrite.append("ALTER TABLE hist.keys UPDATE nk = NULL WHERE id % 10 = 1 SETTINGS mutations_sync = 2;")
    rewrite.append("ALTER TABLE hist.proj MATERIALIZE PROJECTION p_sum SETTINGS mutations_sync = 2;")

    # expected state after the rewrite (for the rollback read)
    rows_after = rows + [[r[0] + 100] + r[1:] for r in rows]
    krows_after = [([r[0], r[1], r[2], r[3], None if r[0] % 10 == 1 else r[4]]) for r in krows] + \
                  [[r[0] + 1000, r[1], r[2], r[3], None if (r[0] + 1000) % 10 == 1 else r[4]] for r in krows if r[0] % 3 == 0]
    oracle["after_rewrite"] = {"scales_wide_rows": rows_after, "keys_rows": krows_after, "mv_extra": [list(x) for x in mv_rows[:10]],
                               "agg_extra": [list(x) for x in agg_src[:6]]}

    # ---- checks, per phase: '<id>' first, expected computed here from the raw integers of that phase
    def build_checks(st):
        fmt_s = lambda raw, scale: fmt(raw, scale, trailing=False)
        out_checks = []

        def add(cid, sql, expected):
            out_checks.append({"id": cid, "sql": sql, "expected": expected})
        for t in ("scales_wide", "scales_compact"):
            trows = st[t]
            for c, ty, sc in cols[1:]:
                ci = [x[0] for x in cols].index(c)
                nn = [r[ci] for r in trows if r[ci] is not None]
                add(f"{t}.{c}.count_nonnull", f"count({c}) FROM hist.{t}", str(len(nn)))
                add(f"{t}.{c}.min_text", f"toString(min({c})) FROM hist.{t}", fmt_s(min(nn), sc))
                add(f"{t}.{c}.max_text", f"toString(max({c})) FROM hist.{t}", fmt_s(max(nn), sc))
                add(f"{t}.{c}.sorted_text", f"arrayStringConcat(arrayMap(x -> toString(x), groupArray({c})), '|') FROM (SELECT {c} FROM hist.{t} WHERE {c} IS NOT NULL ORDER BY {c}, id)",
                    "|".join(fmt_s(v, sc) for v in sorted(nn)))
                add(f"{t}.{c}.count_positive", f"countIf({c} > 0) FROM hist.{t}", str(sum(1 for v in nn if v > 0)))
        for c, ty in icols[1:]:
            ci = [x[0] for x in icols].index(c)
            nn = [r[ci] for r in st["ints"] if r[ci] is not None]
            add(f"ints.{c}.sorted", f"arrayStringConcat(arrayMap(x -> toString(x), arraySort(groupArray({c}))), '|') FROM hist.ints", "|".join(str(v) for v in sorted(nn)))
        kr = st["keys"]
        lo, hi = -10 * 10 ** 30 * 10 ** 18, 10 * 10 ** 30 * 10 ** 18
        add("keys.count", "count() FROM hist.keys", str(len(kr)))
        add("keys.pk_range_count", f"count() FROM hist.keys WHERE k BETWEEN {lit_dec(lo, 18)} AND {lit_dec(hi, 18)}", str(sum(1 for r in kr if lo <= r[2] <= hi)))
        add("keys.bloom_eq_min", f"count() FROM hist.keys WHERE ik = toInt512('{IMIN}')", str(sum(1 for r in kr if r[3] == IMIN)))
        add("keys.minmax_gt", f"count() FROM hist.keys WHERE ik > toInt512('{-10 ** 149}')", str(sum(1 for r in kr if r[3] > -10 ** 149)))
        add("keys.partition_max", f"count() FROM hist.keys WHERE p = {lit_dec(IMAX, 0)}", str(sum(1 for r in kr if r[1] == IMAX)))
        add("keys.partitions", "arrayStringConcat(arraySort(groupArray(toString(p))), '|') FROM (SELECT DISTINCT p FROM hist.keys)", "|".join(sorted(str(x) for x in {r[1] for r in kr})))
        add("keys.group_nullable_key", "count() FROM (SELECT nk, count() FROM hist.keys GROUP BY nk)", str(len({r[4] for r in kr})))
        add("keys.null_count", "countIf(isNull(nk)) FROM hist.keys", str(sum(1 for r in kr if r[4] is None)))
        # the extreme keys are excluded by value: sum() over Decimal512 wraps on overflow without an error (so does
        # upstream sum() over Decimal256 and Int64), which a sum that includes min/max would hit
        add("keys.sum_k", f"toString(sum(k)) FROM hist.keys WHERE k != {lit_dec(IMIN, 18)} AND k != {lit_dec(IMAX, 18)}",
            fmt_s(sum(r[2] for r in kr if r[2] not in (IMIN, IMAX)), 18))
        by = {}
        for a, b, c, d, e, f_, g in st["agg_src"]:
            by.setdefault(a, []).append((b, c, d, e, f_ % 10 ** 153, g))
        for k, lst in sorted(by.items()):
            add(f"agg.k{k}.sum", f"toString(sumMerge(s)) FROM hist.agg WHERE k = {k}", fmt_s(sum(x[0] for x in lst), 18))
            add(f"agg.k{k}.min", f"toString(minMerge(mn)) FROM hist.agg WHERE k = {k}", str(min(x[1] for x in lst)))
            add(f"agg.k{k}.max", f"toString(maxMerge(mx)) FROM hist.agg WHERE k = {k}", str(max(x[2] for x in lst)))
            add(f"agg.k{k}.uniq", f"toString(uniqExactMerge(ue)) FROM hist.agg WHERE k = {k}", str(len({x[3] for x in lst})))
            add(f"agg.k{k}.garray_sorted", f"arrayStringConcat(arrayMap(x -> toString(x), arraySort(groupArrayMerge(ga))), '|') FROM hist.agg WHERE k = {k}",
                "|".join(fmt_s(v, 154) for v in sorted(x[4] for x in lst)))
            qs = sorted(x[5] for x in lst)
            add(f"agg.k{k}.simple_sum", f"toString(sum(ss)) FROM hist.agg WHERE k = {k}", fmt_s(sum(x[0] for x in lst), 18))
        mv = {}
        for a, b in st["mv_src"]:
            mv[a] = mv.get(a, 0) + b
        for k in sorted(mv):
            add(f"mv.k{k}.sum", f"toString(sum(v)) FROM hist.mv_sum WHERE k = {k}", fmt_s(mv[k], 18))
        pj = {}
        for a, b in st["proj"]:
            pj[a] = pj.get(a, 0) + b
        for k in sorted(pj):
            add(f"proj.k{k}.sum", f"toString(sum(v)) FROM hist.proj WHERE k = {k}", fmt_s(pj[k], 18))
        for r in st["dyn"]:
            n = r["id"]
            scale = 2 if r["kind"].startswith("Decimal") else 0
            add(f"dyn.{n}.type", f"dynamicType(dy) FROM hist.dyn WHERE id = {n}", r["kind"])
            add(f"dyn.{n}.value", f"toString(dy) FROM hist.dyn WHERE id = {n}", fmt_s(r["dy_raw"], scale) if scale else str(r["dy_raw"]))
            add(f"dyn.{n}.shared_value", f"toString(dys) FROM hist.dyn WHERE id = {n}", fmt_s(r["dy_raw"], scale) if scale else str(r["dy_raw"]))
            add(f"dyn.{n}.json_a", f"toString(j.a) FROM hist.dyn WHERE id = {n}", fmt_s(r["ja"], 3))
            add(f"dyn.{n}.arr", f"arrayStringConcat(arrayMap(x -> toString(x), arr), '|') FROM hist.dyn WHERE id = {n}", "|".join(fmt_s(x, 18) for x in r["arr"]))
            add(f"dyn.{n}.map", f"toString(m['k']) FROM hist.dyn WHERE id = {n}", str(r["m"]))
            add(f"dyn.{n}.tuple_x", f"toString(t.x) FROM hist.dyn WHERE id = {n}", fmt_s(r["tx"], 154))
            add(f"dyn.{n}.tuple_y", f"toString(t.y) FROM hist.dyn WHERE id = {n}", "\\N" if r["ty"] is None else str(r["ty"]))
        latest = {}
        for k, ver, v in st["replacing"]:
            if k not in latest or ver > latest[k][0]:
                latest[k] = (ver, v)
        add("replacing.final", "arrayStringConcat(groupArray(concat(toString(k), ':', toString(v))), '|') FROM (SELECT k, v FROM hist.replacing FINAL ORDER BY k)",
            "|".join(f"{k}:{fmt_s(latest[k][1], 18)}" for k in sorted(latest)))
        sm = {}
        for k, v in st["summing"]:
            sm[k] = sm.get(k, 0) + v
        # SummingMergeTree drops a row whose sums are all zero when it merges: compare only non-zero groups in both phases
        add("summing.final", "arrayStringConcat(groupArray(concat(toString(k), ':', toString(v))), '|') FROM (SELECT k, sum(v) AS v FROM hist.summing GROUP BY k HAVING v != 0 ORDER BY k)",
            "|".join(f"{k}:{fmt_s(v, 18)}" for k, v in sorted(sm.items()) if v != 0))
        for c, ty, k in ccols[1:]:
            ci = [x[0] for x in ccols].index(c)
            sc = 18 if ty.startswith("Decimal") else 0
            vals = [r[ci] for r in st["codecs"]]
            f2 = (lambda v: fmt_s(v, sc)) if sc else str
            add(f"codecs.{c}.min_max", f"concat(toString(min({c})), '|', toString(max({c}))) FROM hist.codecs", f"{f2(min(vals))}|{f2(max(vals))}")
            inner = [v for v in vals if v not in (IMIN, IMAX, UMAX)]
            add(f"codecs.{c}.sum_inner", f"toString(sum({c})) FROM hist.codecs WHERE {c} != {lit_dec(IMIN, 18) if sc else f'toInt512({chr(39)}{IMIN}{chr(39)})' if ty == 'Int512' else f'toUInt512({chr(39)}{UMAX}{chr(39)})'}"
                + (f" AND {c} != {lit_dec(IMAX, 18) if sc else f'toInt512({chr(39)}{IMAX}{chr(39)})'}" if ty != "UInt512" else ""), f2(sum(inner)))
            add(f"codecs.{c}.point", f"toString({c}) FROM hist.codecs WHERE id = 137", f2(vals[137]))
        for r in st["oldbug"]:
            add(f"oldbug.{r['path']}.stored", f"toString({r['column']}) FROM hist.oldbug WHERE id = {r['id']}", fmt_s(r["old_binary_stores_raw"], r["scale"]))
        return out_checks

    state0 = {"scales_wide": rows, "scales_compact": rows, "ints": irows, "keys": krows, "agg_src": agg_src, "mv_src": mv_rows, "proj": mv_rows,
              "dyn": dyn_rows, "replacing": rep, "summing": summ, "oldbug": obrows, "codecs": crows}
    state1 = dict(state0, scales_wide=rows_after, keys=krows_after, agg_src=agg_src + agg_src[:6], mv_src=mv_rows + mv_rows[:10])
    phases = {"initial": build_checks(state0), "after_rewrite": build_checks(state1)}
    oracle["checks"] = phases
    typemaps = {"scales_wide": cols, "scales_compact": cols, "ints": icols, "keys": kcols, "codecs": ccols}
    oracle["dumps"] = {t: {"types": [x[1] for x in tm], "rows": {"initial": state0[t], "after_rewrite": state1[t]}} for t, tm in typemaps.items()}
    for ph, lst in phases.items():
        with open(os.path.join(out, f"verify_{ph}.sql"), "w") as f:
            f.write("SET max_threads = 1;\n")
            for chk in lst:
                f.write(f"SELECT '{chk['id']}', {chk['sql']};\n")
    verify.extend([])
    open(os.path.join(out, "create.sql"), "w").write("CREATE DATABASE hist;\n" + "\n".join(create) + "\n")
    open(os.path.join(out, "rewrite.sql"), "w").write("\n".join(rewrite) + "\n")
    with open(os.path.join(out, "oracle.json"), "w") as f:
        json.dump(oracle, f, default=str)
    print(f"{len(create)} create statements, {len(rewrite)} rewrite statements, " + ", ".join(f"{k}: {len(v)} checks" for k, v in oracle["checks"].items()))


if __name__ == "__main__":
    main(sys.argv[1])
