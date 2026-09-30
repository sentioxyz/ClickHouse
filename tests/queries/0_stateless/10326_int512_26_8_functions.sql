-- 26.8 functions whose dispatch stopped at 256 bits, and intDiv monotonicity for UInt512 keys (index analysis).
-- Expected values: the 512-bit result equals the 256-bit one (the fork's rule), or is derived by hand.

-- intDiv(UInt512 key, signed constant): the dividend is cast to Int512, so 2^511 and above divide as negative numbers.
-- The primary key analysis must not treat the function as monotonic across 2^511 (`UInt256(1) << 511` was 0).
DROP TABLE IF EXISTS t_10326_u512;
CREATE TABLE t_10326_u512 (k UInt512) ENGINE = MergeTree ORDER BY k SETTINGS index_granularity = 1;
INSERT INTO t_10326_u512 SELECT arrayJoin([toUInt512(1), bitShiftLeft(toUInt512(1), 511), bitOr(bitShiftLeft(toUInt512(1), 511), toUInt512(1))]);
SELECT count() FROM t_10326_u512 WHERE intDiv(k, toInt8(3)) < 0;
SELECT countIf(intDiv(k, toInt8(3)) < 0) FROM t_10326_u512;
DROP TABLE t_10326_u512;

DROP TABLE IF EXISTS t_10326_i64;
CREATE TABLE t_10326_i64 (k Int64) ENGINE = MergeTree ORDER BY k SETTINGS index_granularity = 1;
INSERT INTO t_10326_i64 SELECT number FROM numbers(100);
-- a UInt512 constant divisor below 2^511 is positive: intDiv is increasing
SELECT count() FROM t_10326_i64 WHERE intDiv(k, toUInt512(2)) = 5;
-- a UInt512 constant dividend below 2^511 is positive: intDiv(c, k) decreases in k > 0
SELECT count() FROM t_10326_i64 WHERE k > 0 AND intDiv(toUInt512(100), k) = 50;
DROP TABLE t_10326_i64;

-- (U)Int512 -> Time64 / DateTime64 saturate like (U)Int256
SELECT toTime64(toInt512(4000000), 0) = toTime64(toInt256(4000000), 0), toTime64(toUInt512(4000000), 0) = toTime64(toUInt256(4000000), 0);
SELECT toDateTime64(toInt512('100000000000000000000'), 3) = toDateTime64(toInt256('100000000000000000000'), 3);
SELECT toDateTime64(toUInt512('100000000000000000000'), 3) = toDateTime64(toUInt256('100000000000000000000'), 3);

-- bitmaskToList / bitmaskToArray / bitPositionsToArray
SELECT bitmaskToList(toUInt512(50)), bitmaskToList(toInt512(50)), bitmaskToArray(toUInt512(50)), bitmaskToArray(toInt512(50));
SELECT bitPositionsToArray(toUInt512(50)), bitPositionsToArray(bitShiftLeft(toUInt512(1), 500));
SELECT bitmaskToList(bitShiftLeft(toUInt512(1), 500)) = toString(bitShiftLeft(toUInt512(1), 500));

-- gini, arrayAutocorrelation on 512-bit and Decimal512 values
SELECT (SELECT gini(toDecimal512(number, 2)) FROM numbers(10)) = (SELECT gini(toDecimal256(number, 2)) FROM numbers(10));
SELECT arrayAutocorrelation([toUInt512(1), toUInt512(2), toUInt512(4)]) = arrayAutocorrelation([toUInt256(1), toUInt256(2), toUInt256(4)]);
SELECT arrayAutocorrelation(materialize([toInt512(1), toInt512(2), toInt512(4)])) = arrayAutocorrelation(materialize([toInt256(1), toInt256(2), toInt256(4)]));
SELECT arrayAutocorrelation(materialize([toDecimal512('1.5', 1), toDecimal512('2.5', 1), toDecimal512('4', 1)])) = arrayAutocorrelation(materialize([toDecimal256('1.5', 1), toDecimal256('2.5', 1), toDecimal256('4', 1)]));
SELECT arrayAutocorrelation([toDecimal512('1.5', 1), toDecimal512('2.5', 1), toDecimal512('4', 1)], materialize(toUInt8(2))) = arrayAutocorrelation([toDecimal256('1.5', 1), toDecimal256('2.5', 1), toDecimal256('4', 1)], materialize(toUInt8(2)));
