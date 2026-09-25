-- The common supertype of (U)Int512 and another integer type is (U)Int512 when it can hold both values, instead of
-- Variant(...) (the getLeastSupertype ladder used to stop at 256 bits). When no integer type can hold both (Int512 with
-- UInt512, UInt512 with a negative type) there is still no common type. Mixes of narrower types are unchanged.
SELECT toTypeName([toInt512(1), toInt8(2)]), [toInt512(1), toInt8(2)];
SELECT toTypeName([toUInt512(7), toUInt8(3)]), [toUInt512(7), toUInt8(3)];
SELECT toTypeName(if(number = 0, toInt512(-5), toInt64(3))), if(number = 0, toInt512(-5), toInt64(3)) FROM numbers(2);
SELECT toTypeName(x), x FROM (SELECT toInt512(1) AS x UNION ALL SELECT toInt32(2)) ORDER BY x;
SELECT toTypeName(coalesce(CAST(NULL AS Nullable(Int512)), toInt8(5))), coalesce(CAST(NULL AS Nullable(Int512)), toInt8(5));
SELECT toTypeName(greatest(toInt512(-5), toInt8(3))), greatest(toInt512(-5), toInt8(3));
SELECT toTypeName(least(toUInt512(7), toUInt16(3))), least(toUInt512(7), toUInt16(3));
SELECT toTypeName([toInt512(-1), toUInt256('115792089237316195423570985008687907853269984665640564039457584007913129639935')]), [toInt512(-1), toUInt256('115792089237316195423570985008687907853269984665640564039457584007913129639935')];
SELECT toTypeName([toInt512('-6703903964971298549787012499102923063739682910296196688861780721860882015036773488400937149083451713845015929093243025426876941405973284973216824503042048'), toUInt64(18446744073709551615)]), [toInt512('-6703903964971298549787012499102923063739682910296196688861780721860882015036773488400937149083451713845015929093243025426876941405973284973216824503042048'), toUInt64(18446744073709551615)];
SELECT [toInt512(-1), toUInt512(1)] SETTINGS use_variant_as_common_type = 0; -- { serverError NO_COMMON_TYPE }
SELECT [toUInt512(1), toInt8(-1)] SETTINGS use_variant_as_common_type = 0; -- { serverError NO_COMMON_TYPE }
SELECT toTypeName([toUInt512(1), toInt8(-1)]) SETTINGS use_variant_as_common_type = 1;
SELECT [toInt256(-1), toUInt256(1)] SETTINGS use_variant_as_common_type = 0; -- { serverError NO_COMMON_TYPE }
SELECT toTypeName([toInt256(-1), toUInt128(1)]), toTypeName([toUInt256(1), toUInt8(1)]);
