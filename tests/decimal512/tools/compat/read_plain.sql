-- Columns whose meaning does not depend on the binary type origin (plain 512-bit columns, aggregate states, the part list):
-- every build must read them exactly as the writer did, in both directions.
SELECT 'parts', table, part_type, count() FROM system.parts WHERE database = 'compat' AND active GROUP BY table, part_type ORDER BY table, part_type;
SELECT 'plain', count(), sum(cityHash64(s, n, lc, m)), toString(sum(d)), toString(sum(i)), toString(sum(u)), toString(sum(nd)), countIf(isNull(nd)) FROM compat.t_plain;
SELECT 'agg', k, toString(sumMerge(s)), toString(avgMerge(a)), toString(minMerge(mn)), toString(maxMerge(mx)), uniqExactMerge(ue), toString(arraySort(groupArrayMerge(ga))), toString(argMaxMerge(am)), toString(quantileExactMerge(q)), toString(sum(ss)) FROM compat.t_agg GROUP BY k ORDER BY k;
