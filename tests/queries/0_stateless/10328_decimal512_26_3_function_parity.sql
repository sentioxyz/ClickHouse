-- Functions of the 26.3 Decimal512 port that the 26.8 port had lost (1f0fb3353cd hunks restored in round 5).
-- Expected values are derived by hand (and are the 26.3 results).

-- uniqUpTo keeps 64-bit hashes of (U)Int512 values as for the 128/256-bit types: the state is 1 + 8 * n bytes
SELECT length(toString(uniqUpToState(2)(toUInt512('1')))), length(toString(uniqUpToState(2)(toInt512('-1'))));
SELECT uniqUpTo(3)(toUInt512(number % 2)), uniqUpTo(3)(toInt512(number % 5)) FROM numbers(100);
-- arrayLevenshteinDistanceWeighted / arraySimilarity with 512-bit weights
SELECT arrayLevenshteinDistanceWeighted(['a', 'b'], ['a', 'c'], [toUInt512(1), toUInt512(2)], [toUInt512(1), toUInt512(3)]);
SELECT arrayLevenshteinDistanceWeighted([1, 2], [1, 3], [toInt512(1), toInt512(2)], [toInt512(1), toInt512(3)]);
SELECT arrayLevenshteinDistanceWeighted([toDecimal512('1.5', 2)], [toDecimal512('2.5', 2)], [toUInt512(1)], [toUInt512(1)]);
SELECT round(arraySimilarity(['a', 'b'], ['a', 'c'], [toUInt512(1), toUInt512(2)], [toUInt512(1), toUInt512(3)]), 6);
-- reinterpretAsUInt512 / reinterpretAsInt512
SELECT reinterpretAsUInt512('a'), reinterpretAsInt512(toInt8(-1)), toTypeName(reinterpretAsInt512(toInt8(-1)));
-- runningDifference on (U)Int512
SELECT groupArray(d) FROM (SELECT runningDifference(toUInt512(number * number)) AS d FROM numbers(4)) SETTINGS allow_deprecated_error_prone_window_functions = 1;
SELECT toTypeName(runningDifference(toInt512(1))) SETTINGS allow_deprecated_error_prone_window_functions = 1;
-- roundDown on Decimal512
SELECT roundDown(toDecimal512('1.5', 2), [toDecimal512('1', 2), toDecimal512('2', 2)]), roundDown(toDecimal512('2.25', 2), [toDecimal512('1', 2), toDecimal512('2', 2)]);
-- sparkbar with a UInt512 x argument: same bar as with UInt256
SELECT sparkbar(3)(toUInt512(number), number) = sparkbar(3)(toUInt256(number), number) FROM numbers(3);
-- the arraySort, array() and element access typed paths (hash values of the typed path: 02459_decimal512_hash_functions)
SELECT arraySort([toDecimal512('2.5', 2), NULL, toDecimal512('1.5', 2)]), arrayReverseSort([toDecimal512('2.5', 2), toDecimal512('-1.5', 2)]);
SELECT [materialize(toDecimal512('1.5', 2)), materialize(toDecimal512('2.5', 2))], arrayElement([toDecimal512('1.5', 2), toDecimal512('2.5', 2)], materialize(2));
-- icebergHash rejects Decimal512 with the Iceberg spec message, as it does Decimal256
SELECT icebergHash(toDecimal512('1.5', 2)); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
