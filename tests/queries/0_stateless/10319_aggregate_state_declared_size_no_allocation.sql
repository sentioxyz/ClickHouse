-- Crafted aggregate-function states declare a huge element count (or string length) in a few bytes and carry no
-- payload for it. Reading them must not allocate memory for the declared size before the data is there: under a
-- 100 MB memory limit the result is a read error (the payload is missing) or a size bound, never
-- MEMORY_LIMIT_EXCEEDED. The production build a394e92fb5d allocates first (1-32 GiB requests).
-- Upstream fix: ClickHouse/ClickHouse#117536, 26.3 backport #118529. The states and the legitimate controls are the
-- ones that #117536 added to upstream test 04401_aggregate_function_deserialize_allocation_bomb; that test also
-- expects the per-function size caps of #108465 (never backported to 26.3), so here each crafted state accepts the
-- read errors and the cap error alike, and the controls keep their upstream results.

-- Every blob below declares a size and supplies no payload for it (upstream: a size the caps of #108465 accept,
-- so it is only a declaration and must not become an allocation either). The memory limit is what discriminates: reading the declared size first needs 1-8 GiB and
-- fails with `MEMORY_LIMIT_EXCEEDED`, reading payload-first needs about 30 KiB and reports the
-- truncation. The window is wide on purpose - three orders of magnitude on either side.

-- `StatCommon` `read`: all four consumers, and each of `size_x` / `size_y` on its own.
-- The blob is varint(1<<30), varint(0): exactly the cap, which the strict > admits.
SELECT mannWhitneyUTestMerge(x) FROM (SELECT CAST(unhex('808080800400'), 'AggregateFunction(mannWhitneyUTest, Float64, UInt8)') AS x) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT kolmogorovSmirnovTestMerge(x) FROM (SELECT CAST(unhex('808080800400'), 'AggregateFunction(kolmogorovSmirnovTest, Float64, UInt8)') AS x) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT rankCorrMerge(x) FROM (SELECT CAST(unhex('808080800400'), 'AggregateFunction(rankCorr, Float64, Float64)') AS x) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT finalizeAggregation(CAST(unhex('808080800400'), 'AggregateFunction(largestTriangleThreeBuckets(3), Float64, Float64)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
-- `size_x` = 0, `size_y` = 1<<30: the second limb is independently reachable.
SELECT mannWhitneyUTestMerge(x) FROM (SELECT CAST(unhex('008080808004'), 'AggregateFunction(mannWhitneyUTest, Float64, UInt8)') AS x) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
-- One past the cap still belongs to the cap, not to the read.
SELECT mannWhitneyUTestMerge(x) FROM (SELECT CAST(unhex('818080800400'), 'AggregateFunction(mannWhitneyUTest, Float64, UInt8)') AS x) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }

-- `windowFunnel` keeps two independent state layouts, each reserving behind the same count cap. The
-- header is `sorted` and an 8-byte count of 99999999, which the cap admits, with no events after it.
SELECT finalizeAggregation(CAST(unhex('00FFE0F50500000000'), 'AggregateFunction(windowFunnel(3600), DateTime, UInt8, UInt8)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT finalizeAggregation(CAST(unhex('00FFE0F50500000000'), 'AggregateFunction(windowFunnel(3600, \'strict_once\'), DateTime, UInt8, UInt8)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }

-- `readStringBinaryInto` is shared by `-Distinct`, `groupUniqArray` and generic `groupArrayIntersect`,
-- so one element declaring ~1 GiB is enough whatever the element count says.
SELECT finalizeAggregation(CAST(unhex('01FFFFFFFF03'), 'AggregateFunction(groupUniqArray, String)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }

-- `ColumnString::deserializeAndInsertFromArena`, reached by feeding attacker bytes back through a
-- non-plain column: the element is 8 bytes of `size_t` declaring 2 GiB and nothing after it.
SELECT finalizeAggregation(CAST(unhex('01080000008000000000'), 'AggregateFunction(groupUniqArray, Tuple(String))')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
-- One byte short of the terminator: the payload is complete, so `in.ignore` is what fails.
SELECT finalizeAggregation(CAST(unhex('010A03000000000000006162'), 'AggregateFunction(groupUniqArray, Tuple(String))')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }

-- The caps added earlier still admit their own value, so the four reserves behind them are here too.
SELECT finalizeAggregation(CAST(unhex('00FFC1D72F'), 'AggregateFunction(groupArrayIntersect, Array(UInt64))')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT finalizeAggregation(CAST(unhex('00FFC1D72F'), 'AggregateFunction(groupArrayIntersect, Array(String))')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT finalizeAggregation(CAST(unhex('00FFE0F50500000000'), 'AggregateFunction(sequenceMatch(\'(?1)\'), DateTime, UInt8, UInt8, UInt8)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
-- Elements of the generic path are variable length, so its count and its byte count diverge: one
-- element of 5000000 bytes declaring 99999999 of them reserves 256 MiB when the byte count is the
-- bound, and 16 MiB when the slot size divides it. The limit below sits between the two.
SELECT finalizeAggregation(CAST(unhex('00FFC1D72F') || unhex('C096B102') || repeat(repeat('a', 1000000), 5), 'AggregateFunction(groupArrayIntersect, Array(String))')) SETTINGS max_memory_usage = 200000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT finalizeAggregation(CAST(unhex('10270000000000007b14ae47e17a843f0000000000000000FFE0F50500000000'), 'AggregateFunction(quantileGK(100), Float64)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }

-- Both destinations are `operator new`, which the tracker counts but never refuses, so both sides
-- report the same truncation: these two lines pin the path and the error, not the allocation size.
SELECT finalizeAggregation(CAST(unhex('018080808004'), 'AggregateFunction(groupBitmap, UInt64)')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }
SELECT finalizeAggregation(CAST(unhex('018080808004'), 'AggregateFunction(sumMap, Map(String, UInt64))')) SETTINGS max_memory_usage = 100000000; -- { serverError CANNOT_READ_ALL_DATA, ATTEMPT_TO_READ_AFTER_EOF, TOO_LARGE_ARRAY_SIZE }

-- Legitimate states for the newly touched functions.
SELECT kolmogorovSmirnovTestMerge(s) FROM (SELECT kolmogorovSmirnovTestState(x, y) AS s FROM (SELECT number::Float64 AS x, (number % 2)::UInt8 AS y FROM numbers(100)));
SELECT rankCorrMerge(s) FROM (SELECT rankCorrState(number::Float64, (number * 2)::Float64) AS s FROM numbers(100));
SELECT arraySort(groupUniqArrayMerge(s)) FROM (SELECT groupUniqArrayState(toString(number % 5)) AS s FROM numbers(50));
SELECT arraySort(groupUniqArrayMerge(s)) FROM (SELECT groupUniqArrayState(tuple(toString(number % 4))) AS s FROM numbers(40));
SELECT finalizeAggregation(sumMapState(map(toString(number % 3), number::UInt64))) FROM numbers(9);
SELECT bitmapCardinality(groupBitmapStateMerge(s)) FROM (SELECT groupBitmapState(number::UInt64) AS s FROM numbers(300000));
SELECT windowFunnelMerge(3600)(s) FROM (SELECT windowFunnelState(3600)(toDateTime(number), number = 0, number = 1) AS s FROM numbers(10));
SELECT windowFunnelMerge(3600, 'strict_once')(s) FROM (SELECT windowFunnelState(3600, 'strict_once')(toDateTime(number), number = 0, number = 1) AS s FROM numbers(10));

-- A blob is fully buffered, so it reserves exactly. A state read back from a table arrives in
-- compressed blocks, so the reserve is clamped far below the count and the loop grows as it appends.
DROP TABLE IF EXISTS t_deserialize_allocation_bomb_gk;
CREATE TABLE t_deserialize_allocation_bomb_gk
(
    q AggregateFunction(quantileGK(100), Float64)
)
ENGINE = MergeTree ORDER BY tuple()
SETTINGS min_bytes_for_wide_part = 0,
    min_compress_block_size = 4, max_compress_block_size = 4;

INSERT INTO t_deserialize_allocation_bomb_gk SELECT quantileGKState(100)(number::Float64) FROM numbers(1000);

SELECT quantileGKMerge(100, 0.5)(q) FROM t_deserialize_allocation_bomb_gk;

DROP TABLE t_deserialize_allocation_bomb_gk;

DROP TABLE IF EXISTS t_deserialize_allocation_bomb_map;
CREATE TABLE t_deserialize_allocation_bomb_map
(
    m AggregateFunction(sumMap, Map(String, UInt64))
)
ENGINE = MergeTree ORDER BY tuple()
SETTINGS min_bytes_for_wide_part = 0,
    min_compress_block_size = 4, max_compress_block_size = 4;

INSERT INTO t_deserialize_allocation_bomb_map SELECT sumMapState(map(repeat('k', 300), number::UInt64)) FROM numbers(4);

-- The key is compared by content: one of the right length assembled from the wrong bytes must not pass.
SELECT mapKeys(finalizeAggregation(m))[1] = repeat('k', 300), mapValues(finalizeAggregation(m))[1] FROM t_deserialize_allocation_bomb_map;

DROP TABLE t_deserialize_allocation_bomb_map;

DROP TABLE IF EXISTS t_deserialize_allocation_bomb_bitmap;
CREATE TABLE t_deserialize_allocation_bomb_bitmap
(
    b AggregateFunction(groupBitmap, UInt64)
)
ENGINE = MergeTree ORDER BY tuple()
SETTINGS min_bytes_for_wide_part = 0,
    min_compress_block_size = 4, max_compress_block_size = 4;

-- More than 32 values, so this is a `BitmapKind::Bitmap` state rather than the small set.
INSERT INTO t_deserialize_allocation_bomb_bitmap SELECT groupBitmapState(number::UInt64) FROM numbers(1000);

SELECT groupBitmapMerge(b), arraySum(bitmapToArray(groupBitmapStateMerge(b))) FROM t_deserialize_allocation_bomb_bitmap;

DROP TABLE t_deserialize_allocation_bomb_bitmap;
