-- Index analysis evaluates the functions of a key condition at the endpoints of key ranges and at partition values. A function that is
-- undefined there (division by zero, the minimal signed number divided by -1) must not fail a valid query: the row is filtered out by the
-- other condition. The analysis then prunes nothing for that condition, and every other condition still prunes as upstream does
-- (04003, 04073: partition values are evaluated exactly, whatever the monotonicity of the function over its whole argument type).

DROP TABLE IF EXISTS p;
CREATE TABLE p (x Int64) ENGINE = MergeTree PARTITION BY x ORDER BY tuple();
INSERT INTO p SELECT number - 2 FROM numbers(5);

SELECT groupArray(x) FROM (SELECT x FROM p WHERE x = 0 OR intDiv(10, x) > 4 ORDER BY x);
SELECT groupArray(x) FROM (SELECT x FROM p WHERE x = 0 OR modulo(11, x) > 0 ORDER BY x);
SELECT groupArray(x) FROM (SELECT x FROM p WHERE x = 0 OR divide(toDecimal32(10, 2), x) > 4 ORDER BY x);
SELECT groupArray(x) FROM (SELECT x FROM p WHERE x = 0 OR intDiv(toInt512(10), x) > 4 ORDER BY x);
-- Partition values are evaluated exactly: 2 of 5 parts (x = 1 and x = 2: +inf).
SELECT groupArray(x) FROM (SELECT x FROM p WHERE divide(x, 0) > 0 ORDER BY x) SETTINGS force_index_by_date = 0;
SELECT count() FROM p WHERE divide(x, 0) > 0 SETTINGS max_rows_to_read = 2;

DROP TABLE p;

DROP TABLE IF EXISTS k;
CREATE TABLE k (k Int64) ENGINE = MergeTree ORDER BY k SETTINGS index_granularity = 1;
INSERT INTO k VALUES (-9223372036854775808), (1), (2), (10), (20);
SELECT groupArray(k) FROM (SELECT k FROM k WHERE k > 0 AND intDiv(k, -1) < -5 ORDER BY k);
SELECT groupArray(k) FROM (SELECT k FROM k WHERE k > 0 AND intDiv(100, k) = 50 ORDER BY k);
DROP TABLE k;

DROP TABLE IF EXISTS k512;
CREATE TABLE k512 (k UInt512) ENGINE = MergeTree ORDER BY k SETTINGS index_granularity = 1;
INSERT INTO k512 VALUES (0), (1), (2), (3);
SELECT groupArray(k) FROM (SELECT k FROM k512 WHERE k > 0 AND intDiv(toUInt512(100), k) = 50 ORDER BY k);
DROP TABLE k512;
