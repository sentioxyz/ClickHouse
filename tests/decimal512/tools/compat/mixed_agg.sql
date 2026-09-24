-- Mixed-version merge of aggregate states (upgrade: a new replica merges old parts; rollback: the old binary merges
-- parts written by the new one). Runs on a copy of the writer's data: adds a part with this engine's own states and
-- merges it with the writer's part. Expected output: the writer's own run of this file on its own copy.
INSERT INTO compat.t_agg SELECT number % 3, sumState(toDecimal512(toString(number) || '.1234', 4)), avgState(toDecimal512(toString(number) || '.1234', 4)), minState(toInt512(-toInt64(number))), maxState(toUInt512(number)), uniqExactState(toInt512(number % 7)), groupArrayState(toDecimal512(toString(number) || '.5', 2)), argMaxState(toInt512(-toInt64(number)), toUInt32(number)), quantileExactState(toDecimal512(toString(number) || '.5', 2)), sum(toDecimal512(toString(number) || '.1234', 4)) FROM numbers(30, 30) GROUP BY number % 3;
OPTIMIZE TABLE compat.t_agg FINAL;
SELECT 'agg_mixed', k, toString(sumMerge(s)), toString(avgMerge(a)), toString(minMerge(mn)), toString(maxMerge(mx)), uniqExactMerge(ue), toString(arraySort(groupArrayMerge(ga))), toString(argMaxMerge(am)), toString(quantileExactMerge(q)), toString(sum(ss)) FROM compat.t_agg GROUP BY k ORDER BY k;
SELECT 'agg_mixed_parts', count() FROM system.parts WHERE database = 'compat' AND table = 't_agg' AND active;
