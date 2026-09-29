-- Binary type indexes of the 512-bit types must stay 0x37 (UInt512), 0x38 (Int512) and 0x39 (Decimal512):
-- the Decimal512 fork of 26.3 LTS wrote them into `Dynamic`/`JSON` parts and binary formats.
-- The blob below is `SELECT toInt512(-7) AS a, toUInt512(9) AS b, toDecimal512('-1.25', 2) AS c` in
-- RowBinaryWithNamesAndTypes with output_format_binary_encode_types_in_binary_format = 1, produced by the
-- 26.3 LTS release binary (build id 203677193CDA6BD5FAAC9AB6B125066CFACE0ADB).
SELECT toTypeName(a), toTypeName(b), toTypeName(c), a, b, c
FROM format(RowBinaryWithNamesAndTypes, x'030161016201633837399a02f9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0900000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000083ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff')
SETTINGS input_format_binary_decode_types_in_binary_format = 1;

-- Round trip in the current binary uses the same indexes.
SELECT hex(substring(x, 1, 11)) FROM (SELECT formatRow('RowBinaryWithNamesAndTypes', toInt512(-7) AS a, toUInt512(9) AS b, toDecimal512('-1.25', 2) AS c) AS x) SETTINGS output_format_binary_encode_types_in_binary_format = 1;

-- Values in the shared variant of `Dynamic` carry a binary type index per value. The blob is
-- `SELECT number::UInt32 AS id, multiIf(number % 3 = 0, toInt512(-7)::Dynamic, number % 3 = 1, toUInt512(9)::Dynamic,
-- toDecimal512('-1.25', 2)::Dynamic)::Dynamic(max_types = 1) AS v FROM numbers(3) FORMAT Native` from the same 26.3 binary.
-- With shifted indexes these rows decode without any error as 2^512 - 7 (UInt512) and -8191334 (Int512).
SELECT id, v, dynamicType(v), isDynamicElementInSharedData(v) FROM format(Native, x'02030269640655496e74333200000000010000000200000001761444796e616d6963286d61785f74797065733d3129010000000000000001010755496e7435313200000000000000000001004138f9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff43399a0283ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff09000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000') ORDER BY id;

-- Strided QBit is 0x3A in this fork. Upstream uses 0x37 for it, which is UInt512 here, so binary type encodings
-- of strided QBit are not exchangeable with upstream ClickHouse.
SELECT hex(substring(x, 1, 4)) FROM (SELECT formatRow('RowBinaryWithNamesAndTypes', CAST(range(1, 17)::Array(Float32), 'QBit(Float32, 16, 8)') AS q) AS x) SETTINGS output_format_binary_encode_types_in_binary_format = 1;
