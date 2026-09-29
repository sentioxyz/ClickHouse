-- Written by one binary, read by another (26.3 LTS fork <-> 26.8 fork). MergeTree defaults (Dynamic/JSON serialization v3).
CREATE DATABASE IF NOT EXISTS compat;
CREATE TABLE compat.t_dyn (id UInt32, v Dynamic) ENGINE = MergeTree ORDER BY id;
INSERT INTO compat.t_dyn SELECT number, multiIf(number % 3 = 0, toInt512(-toInt64(number))::Dynamic, number % 3 = 1, toUInt512(number)::Dynamic, toDecimal512(toString(number) || '.25', 2)::Dynamic) FROM numbers(30);
-- max_types = 1 forces two of the three types into the shared variant, which stores a binary type code per value
CREATE TABLE compat.t_dyn_shared (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id;
INSERT INTO compat.t_dyn_shared SELECT id, v FROM compat.t_dyn;
CREATE TABLE compat.t_json (id UInt32, j JSON(a Decimal512(3))) ENGINE = MergeTree ORDER BY id;
INSERT INTO compat.t_json SELECT number, CAST(concat('{"a": ', toString(number), '.125, "b": ', toString(number * 7), '}'), 'JSON(a Decimal512(3))') FROM numbers(10);
-- same data in Wide parts (the default thresholds make these tiny inserts Compact parts)
CREATE TABLE compat.t_dyn_wide (id UInt32, v Dynamic) ENGINE = MergeTree ORDER BY id SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;
INSERT INTO compat.t_dyn_wide SELECT id, v FROM compat.t_dyn;
CREATE TABLE compat.t_dyn_shared_wide (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;
INSERT INTO compat.t_dyn_shared_wide SELECT id, v FROM compat.t_dyn;
-- Dynamic nested in Array/Map/Tuple, and a plain table (rollback also depends on non-Dynamic serialization defaults)
CREATE TABLE compat.t_nested (id UInt32, a Array(Dynamic), m Map(String, Dynamic), t Tuple(x Dynamic, y UInt8)) ENGINE = MergeTree ORDER BY id;
INSERT INTO compat.t_nested SELECT id, [v, v], map('k', v), tuple(v, 1) FROM compat.t_dyn;
CREATE TABLE compat.t_plain (id UInt32, s String, n Nullable(Int64), lc LowCardinality(String), m Map(String, UInt64), d Decimal512(4), i Int512, u UInt512, nd Nullable(Decimal512(2))) ENGINE = MergeTree ORDER BY id;
INSERT INTO compat.t_plain SELECT number, toString(number), if(number % 4 = 0, NULL, -toInt64(number)), toString(number % 3), map(toString(number), number), toDecimal512(toString(number) || '.1234', 4), toInt512(-toInt64(number)), toUInt512(number), if(number % 5 = 0, NULL, toDecimal512(toString(number) || '.5', 2)) FROM numbers(50);
SELECT 'written', (SELECT count() FROM compat.t_dyn), (SELECT count() FROM compat.t_dyn_shared), (SELECT count() FROM compat.t_json), (SELECT count() FROM compat.t_nested), (SELECT count() FROM compat.t_plain);
-- aggregate states of 512-bit types, persisted in AggregatingMergeTree parts (one part: no background merge can change the part list between runs)
CREATE TABLE compat.t_agg (k UInt8, s AggregateFunction(sum, Decimal512(4)), a AggregateFunction(avg, Decimal512(4)), mn AggregateFunction(min, Int512), mx AggregateFunction(max, UInt512), ue AggregateFunction(uniqExact, Int512), ga AggregateFunction(groupArray, Decimal512(2)), am AggregateFunction(argMax, Int512, UInt32), q AggregateFunction(quantileExact, Decimal512(2)), ss SimpleAggregateFunction(sum, Decimal512(4))) ENGINE = AggregatingMergeTree ORDER BY k;
INSERT INTO compat.t_agg SELECT number % 3, sumState(toDecimal512(toString(number) || '.1234', 4)), avgState(toDecimal512(toString(number) || '.1234', 4)), minState(toInt512(-toInt64(number))), maxState(toUInt512(number)), uniqExactState(toInt512(number % 7)), groupArrayState(toDecimal512(toString(number) || '.5', 2)), argMaxState(toInt512(-toInt64(number)), toUInt32(number)), quantileExactState(toDecimal512(toString(number) || '.5', 2)), sum(toDecimal512(toString(number) || '.1234', 4)) FROM numbers(30) GROUP BY number % 3;
