-- uniqHLL12 of UInt512 and Int512 went from its small set to HyperLogLog at the 17th distinct value, and IntHash32 had no 64-byte branch:
-- the server crashed (SIGSEGV). IntHash32 now folds a 512-bit integer like the 128- and 256-bit ones. uniqHLL12 hashes every value with
-- DefaultHash64 first (that hash is kept in the states, also by uniq, so it is not changed here), and DefaultHash64 folds the words of a
-- 512-bit integer like those of a 128-bit one, so the results equal the ones of the narrower integer exactly: UInt512 those of UInt256 and
-- Int512 those of Int128 (also the known weakness of this fold for values of both signs, x and -x-1 hash alike). Up to 16 values the count
-- is exact.

SELECT n,
    uniqHLL12(toUInt512(number)) = uniqHLL12(toUInt256(number)) AS u512_eq_u256,
    uniqHLL12(toInt512(number) - toInt512(500)) = uniqHLL12(toInt128(number) - toInt128(500)) AS i512_eq_i128,
    n > 16 OR uniqHLL12(toUInt512(number)) = uniqExact(toUInt512(number)) AS exact_up_to_16,
    abs(uniqHLL12(toUInt512(number)) - uniqExact(toUInt512(number))) <= 0.1 * n AS within_10_percent
FROM numbers(100000) AS t, (SELECT arrayJoin([1, 2, 16, 17, 20, 1000, 100000]) AS n) AS s
WHERE number < n
GROUP BY n
ORDER BY n;

-- Values that use the high words of the 512-bit integer, and negative values only.
SELECT abs(uniqHLL12(bitShiftLeft(toUInt512(number), 300) + number) - 1000) <= 100,
    abs(uniqHLL12(toInt512(0) - bitShiftLeft(toInt512(number), 400) - toInt512(number) - toInt512(1)) - 1000) <= 100
FROM numbers(1000);

-- States: written, merged and read back.
SELECT abs(finalizeAggregation(s) - 1000) <= 100
FROM (SELECT uniqHLL12State(toUInt512(number)) AS s FROM numbers(1000));
SELECT uniqHLL12Merge(s) = (SELECT uniqHLL12(toUInt256(number)) FROM numbers(3000))
FROM (SELECT uniqHLL12State(toUInt512(number)) AS s FROM numbers(3000) GROUP BY number % 3);
