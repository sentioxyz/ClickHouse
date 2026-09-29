-- Map lookups with 512-bit keys: the typed key matcher of findKeyPositionsDispatch (26.8) and the generic fallback
-- must agree; missing keys give the default of the value type.
SELECT map(toInt512(1), 'a', toInt512(-2), 'b')[toInt512(-2)];
SELECT map(toUInt512(3), 10, toUInt512(4), 20)[toUInt512(5)];
SELECT mapContains(map(toDecimal512('1.5', 2), 1), toDecimal512('1.5', 2));
SELECT mapContains(map(toDecimal512('1.5', 2), 1), toDecimal512('1.25', 2));
SELECT m[toInt512(1)] FROM (SELECT map(toInt512(number), number * 10) AS m FROM numbers(3)) ORDER BY m[toInt512(1)];
SELECT map(toUInt512('115792089237316195423570985008687907853269984665640564039457584007913129639936'), 'big')[toUInt512('115792089237316195423570985008687907853269984665640564039457584007913129639936')];
