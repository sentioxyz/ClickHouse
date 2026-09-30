-- Core capabilities of the 26.3 Decimal512 port that the 26.8 port had lost (restored in round 5). Expected values are
-- derived by hand, or are the result of the narrower type (the fork's rule).

-- LowCardinality composite keys of 33..64 bytes use low_cardinality_keys512 (the LowCardinality-unaware keys512 misread
-- ColumnLowCardinality in the merge paths). a = n % 5, b = n % 3: each (a, b) pair occurs twice in 30 rows.
SELECT a, b, count() AS c FROM (SELECT toLowCardinality(toUInt256(number % 5)) AS a, toLowCardinality(toUInt8(number % 3)) AS b FROM numbers(30)) GROUP BY a, b WITH ROLLUP ORDER BY a, b, c;
SELECT a, b, count() AS c FROM remote('127.0.0.{1,2}', view(SELECT toLowCardinality(toUInt256(number % 5)) AS a, toLowCardinality(toUInt8(number % 3)) AS b FROM numbers(30))) GROUP BY a, b ORDER BY a, b LIMIT 3;
SELECT count(), sum(c) FROM (SELECT count() AS c FROM (SELECT toLowCardinality(toUUID(concat('00000000-0000-0000-0000-00000000000', toString(number % 4)))) AS u1, toLowCardinality(toUUID(concat('00000000-0000-0000-0000-00000000000', toString(number % 3)))) AS u2, toLowCardinality(toUUID(concat('00000000-0000-0000-0000-00000000000', toString(number % 2)))) AS u3 FROM numbers(24)) GROUP BY u1, u2, u3 WITH CUBE);
-- a single LowCardinality((U)Int512) key
SELECT k, count() AS c FROM (SELECT toLowCardinality(toUInt512(number % 3)) AS k FROM numbers(10)) GROUP BY k ORDER BY k;
SELECT k, count() AS c FROM (SELECT toLowCardinality(toInt512(number % 2) - 1) AS k FROM numbers(4)) GROUP BY k WITH TOTALS ORDER BY k;
-- getMostSubtype: the common subtype of (U)Int512 and another integer
SELECT arrayIntersect([toUInt512(1), toUInt512(2)], [1, 2]), toTypeName(arrayIntersect([toUInt512(1), toUInt512(2)], [1, 2]));
SELECT arrayIntersect([toInt512(-1), toInt512(2)], [toInt8(-1), toInt8(3)]);
-- toDecimalString rounding to no fractional digits at scale 154 (10^154 does not fit Int512): same as scale 38
SELECT toDecimalString(CAST('0.6' AS Decimal(154, 154)), 0) = toDecimalString(CAST('0.6' AS Decimal(38, 38)), 0);
SELECT toDecimalString(CAST('-0.6' AS Decimal(154, 154)), 0) = toDecimalString(CAST('-0.6' AS Decimal(38, 38)), 0);
SELECT toDecimalString(CAST('0.4' AS Decimal(154, 154)), 0) = toDecimalString(CAST('0.4' AS Decimal(38, 38)), 0);
-- ColumnArray filter / replicate of 512-bit elements
SELECT a FROM (SELECT [toInt512(number), toInt512(-number)] AS a FROM numbers(5)) WHERE a[1] > 2 ORDER BY a[1];
SELECT a, j FROM (SELECT [toDecimal512(number, 2)] AS a FROM numbers(2)) ARRAY JOIN [1, 2] AS j ORDER BY a[1], j;
