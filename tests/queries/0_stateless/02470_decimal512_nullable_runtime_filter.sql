-- UInt512-backed Decimal512 runtime-filter keys must allocate a non-empty null bitmap.
-- The NULL row exercises BaseStateKeysFixed::createBitmap for a 512-bit fixed key.
SELECT count()
FROM
(
    SELECT if(
        number = 0,
        CAST(NULL AS Nullable(Decimal512(0))),
        CAST(toDecimal512(number, 0) AS Nullable(Decimal512(0)))) AS k
    FROM numbers(1001)
) AS probe
INNER JOIN
(
    SELECT if(
        number = 0,
        CAST(NULL AS Nullable(Decimal512(0))),
        CAST(toDecimal512(number, 0) AS Nullable(Decimal512(0)))) AS k
    FROM numbers(1002)
) AS build
USING (k)
SETTINGS enable_join_runtime_filters = 1, join_runtime_filter_min_probe_rows = 0, max_threads = 1
