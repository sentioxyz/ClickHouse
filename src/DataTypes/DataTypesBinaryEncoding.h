#pragma once

#include <DataTypes/IDataType.h>
#include <Interpreters/Context_fwd.h>

namespace DB
{

enum class BinaryTypeIndex : uint8_t
{
    Nothing = 0x00,
    UInt8 = 0x01,
    UInt16 = 0x02,
    UInt32 = 0x03,
    UInt64 = 0x04,
    UInt128 = 0x05,
    UInt256 = 0x06,
    Int8 = 0x07,
    Int16 = 0x08,
    Int32 = 0x09,
    Int64 = 0x0A,
    Int128 = 0x0B,
    Int256 = 0x0C,
    Float32 = 0x0D,
    Float64 = 0x0E,
    Date = 0x0F,
    Date32 = 0x10,
    DateTimeUTC = 0x11,
    DateTimeWithTimezone = 0x12,
    DateTime64UTC = 0x13,
    DateTime64WithTimezone = 0x14,
    String = 0x15,
    FixedString = 0x16,
    Enum8 = 0x17,
    Enum16 = 0x18,
    Decimal32 = 0x19,
    Decimal64 = 0x1A,
    Decimal128 = 0x1B,
    Decimal256 = 0x1C,
    UUID = 0x1D,
    Array = 0x1E,
    UnnamedTuple = 0x1F,
    NamedTuple = 0x20,
    Set = 0x21,
    Interval = 0x22,
    Nullable = 0x23,
    Function = 0x24,
    AggregateFunction = 0x25,
    LowCardinality = 0x26,
    Map = 0x27,
    IPv4 = 0x28,
    IPv6 = 0x29,
    Variant = 0x2A,
    Dynamic = 0x2B,
    Custom = 0x2C,
    Bool = 0x2D,
    SimpleAggregateFunction = 0x2E,
    Nested = 0x2F,
    JSON = 0x30,
    BFloat16 = 0x31,
    Time = 0x32,
    /* The reason behind putting Time64 to 0x34 instead of 0x33 is following:
    Originally, there were Time and Time64 with (and without) timezones, which were making the following indexing:
    TimeUTC = 0x32
    TimeWithTimezone = 0x33
    Time64UTC = 0x34
    Time64WithTimezone = 0x35

    After that timezones became forbidden for Time[64] types, so we removed those types from here.
    But we need to make the indexing consistent to ensure backwards compatibility.

    Please don't use 0x33 and 0x35, because older client might try to serialise data as TimeWithTimezone/Time64WithTimezone, and newer server would deserialise them as incorrect types. */
    Time64 = 0x34,
    /// reserved = 0x35
    QBit = 0x36,
    /// QBit with an explicit stride parameter (stride != dimension). Non-strided QBit keeps using 0x36 for backward compatibility.
    QBitWithStride = 0x37
};

/// Maximum value of BinaryTypeIndex + 1, used for sizing the index array in SimpleDataTypesCache.
inline constexpr size_t BINARY_TYPE_INDEX_SIZE = 0x38;

/// The indexes the Decimal512 fork of 26.3 LTS (and this fork before the SentioWide descriptors) wrote for its 512-bit types and for
/// strided `QBit`. They are not `BinaryTypeIndex` values: upstream 0x37 is strided `QBit`, and 0x38-0x3A are unassigned. They are read
/// only for `BinaryTypeOrigin::SentioLegacyV0` and written only by `BinaryTypeWriterMode::SentioLegacyBridge` and by
/// encodeDataTypeForHashCalculation (the historical fingerprint).
namespace SentioLegacyBinaryTypeIndex
{
    inline constexpr uint8_t UInt512 = 0x37;
    inline constexpr uint8_t Int512 = 0x38;
    /// Followed by <uint8_precision><uint8_scale>.
    inline constexpr uint8_t Decimal512 = 0x39;
    /// The fork's strided `QBit` index; strided `QBit` is disabled in this fork, so it is refused.
    inline constexpr uint8_t QBitWithStride = 0x3A;
}

/**

Binary encoding for ClickHouse data types:
|---------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| ClickHouse data type                                                                                    | Binary encoding                                                                                                                                                                                                                                                                                                                                                          |
|---------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Nothing                                                                                                 | 0x00                                                                                                                                                                                                                                                                                                                                                                     |
| UInt8                                                                                                   | 0x01                                                                                                                                                                                                                                                                                                                                                                     |
| UInt16                                                                                                  | 0x02                                                                                                                                                                                                                                                                                                                                                                     |
| UInt32                                                                                                  | 0x03                                                                                                                                                                                                                                                                                                                                                                     |
| UInt64                                                                                                  | 0x04                                                                                                                                                                                                                                                                                                                                                                     |
| UInt128                                                                                                 | 0x05                                                                                                                                                                                                                                                                                                                                                                     |
| UInt256                                                                                                 | 0x06                                                                                                                                                                                                                                                                                                                                                                     |
| Int8                                                                                                    | 0x07                                                                                                                                                                                                                                                                                                                                                                     |
| Int16                                                                                                   | 0x08                                                                                                                                                                                                                                                                                                                                                                     |
| Int32                                                                                                   | 0x09                                                                                                                                                                                                                                                                                                                                                                     |
| Int64                                                                                                   | 0x0A                                                                                                                                                                                                                                                                                                                                                                     |
| Int128                                                                                                  | 0x0B                                                                                                                                                                                                                                                                                                                                                                     |
| Int256                                                                                                  | 0x0C                                                                                                                                                                                                                                                                                                                                                                     |
| Float32                                                                                                 | 0x0D                                                                                                                                                                                                                                                                                                                                                                     |
| Float64                                                                                                 | 0x0E                                                                                                                                                                                                                                                                                                                                                                     |
| Date                                                                                                    | 0x0F                                                                                                                                                                                                                                                                                                                                                                     |
| Date32                                                                                                  | 0x10                                                                                                                                                                                                                                                                                                                                                                     |
| DateTime                                                                                                | 0x11                                                                                                                                                                                                                                                                                                                                                                     |
| DateTime(time_zone)                                                                                     | 0x12<var_uint_time_zone_name_size><time_zone_name_data>                                                                                                                                                                                                                                                                                                                  |
| DateTime64(P)                                                                                           | 0x13<uint8_precision>                                                                                                                                                                                                                                                                                                                                                    |
| DateTime64(P, time_zone)                                                                                | 0x14<uint8_precision><var_uint_time_zone_name_size><time_zone_name_data>                                                                                                                                                                                                                                                                                                 |
| String                                                                                                  | 0x15                                                                                                                                                                                                                                                                                                                                                                     |
| FixedString(N)                                                                                          | 0x16<var_uint_size>                                                                                                                                                                                                                                                                                                                                                      |
| Enum8                                                                                                   | 0x17<var_uint_number_of_elements><var_uint_name_size_1><name_data_1><int8_value_1>...<var_uint_name_size_N><name_data_N><int8_value_N>                                                                                                                                                                                                                                   |
| Enum16                                                                                                  | 0x18<var_uint_number_of_elements><var_uint_name_size_1><name_data_1><int16_little_endian_value_1>...><var_uint_name_size_N><name_data_N><int16_little_endian_value_N>                                                                                                                                                                                                    |
| Decimal32(P, S)                                                                                         | 0x19<uint8_precision><uint8_scale>                                                                                                                                                                                                                                                                                                                                       |
| Decimal64(P, S)                                                                                         | 0x1A<uint8_precision><uint8_scale>                                                                                                                                                                                                                                                                                                                                       |
| Decimal128(P, S)                                                                                        | 0x1B<uint8_precision><uint8_scale>                                                                                                                                                                                                                                                                                                                                       |
| Decimal256(P, S)                                                                                        | 0x1C<uint8_precision><uint8_scale>                                                                                                                                                                                                                                                                                                                                       |
| UUID                                                                                                    | 0x1D                                                                                                                                                                                                                                                                                                                                                                     |
| Array(T)                                                                                                | 0x1E<nested_type_encoding>                                                                                                                                                                                                                                                                                                                                               |
| Tuple(T1, ..., TN)                                                                                      | 0x1F<var_uint_number_of_elements><nested_type_encoding_1>...<nested_type_encoding_N>                                                                                                                                                                                                                                                                                     |
| Tuple(name1 T1, ..., nameN TN)                                                                          | 0x20<var_uint_number_of_elements><var_uint_name_size_1><name_data_1><nested_type_encoding_1>...<var_uint_name_size_N><name_data_N><nested_type_encoding_N>                                                                                                                                                                                                               |
| Set                                                                                                     | 0x21                                                                                                                                                                                                                                                                                                                                                                     |
| Interval                                                                                                | 0x22<interval_kind>                                                                                                                                                                                                                                                                                                                                                      |
| Nullable(T)                                                                                             | 0x23<nested_type_encoding>                                                                                                                                                                                                                                                                                                                                               |
| Function                                                                                                | 0x24<var_uint_number_of_arguments><argument_type_encoding_1>...<argument_type_encoding_N><return_type_encoding>                                                                                                                                                                                                                                                          |
| AggregateFunction(function_name(param_1, ..., param_N), arg_T1, ..., arg_TN)                            | 0x25<var_uint_version><var_uint_function_name_size><function_name_data><var_uint_number_of_parameters><param_1>...<param_N><var_uint_number_of_arguments><argument_type_encoding_1>...<argument_type_encoding_N>                                                                                                                                                         |
| LowCardinality(T)                                                                                       | 0x26<nested_type_encoding>                                                                                                                                                                                                                                                                                                                                               |
| Map(K, V)                                                                                               | 0x27<key_type_encoding><value_type_encoding>                                                                                                                                                                                                                                                                                                                             |
| IPv4                                                                                                    | 0x28                                                                                                                                                                                                                                                                                                                                                                     |
| IPv6                                                                                                    | 0x29                                                                                                                                                                                                                                                                                                                                                                     |
| Variant(T1, ..., TN)                                                                                    | 0x2A<var_uint_number_of_variants><variant_type_encoding_1>...<variant_type_encoding_N>                                                                                                                                                                                                                                                                                   |
| Dynamic(max_types=N)                                                                                    | 0x2B<uint8_max_types>                                                                                                                                                                                                                                                                                                                                                    |
| Custom type (Ring, Polygon, etc)                                                                        | 0x2C<var_uint_type_name_size><type_name_data>                                                                                                                                                                                                                                                                                                                            |
| Bool                                                                                                    | 0x2D                                                                                                                                                                                                                                                                                                                                                                     |
| SimpleAggregateFunction(function_name(param_1, ..., param_N), arg_T1, ..., arg_TN)                      | 0x2E<var_uint_function_name_size><function_name_data><var_uint_number_of_parameters><param_1>...<param_N><var_uint_number_of_arguments><argument_type_encoding_1>...<argument_type_encoding_N>                                                                                                                                                                           |
| Nested(name1 T1, ..., nameN TN)                                                                         | 0x2F<var_uint_number_of_elements><var_uint_name_size_1><name_data_1><nested_type_encoding_1>...<var_uint_name_size_N><name_data_N><nested_type_encoding_N>                                                                                                                                                                                                               |
| JSON(max_dynamic_paths=N, max_dynamic_types=M, path Type, SKIP skip_path, SKIP REGEXP skip_path_regexp) | 0x30<uint8_serialization_version><var_int_max_dynamic_paths><uint8_max_dynamic_types><var_uint_number_of_typed_paths><var_uint_path_name_size_1><path_name_data_1><encoded_type_1>...<var_uint_number_of_skip_paths><var_uint_skip_path_size_1><skip_path_data_1>...<var_uint_number_of_skip_path_regexps><var_uint_skip_path_regexp_size_1><skip_path_data_regexp_1>... |
| BFloat16                                                                                                | 0x31                                                                                                                                                                                                                                                                                                                                                                     |
| Time                                                                                                    | 0x32                                                                                                                                                                                                                                                                                                                                                                     |
| Time64(P)                                                                                               | 0x34<uint8_precision>                                                                                                                                                                                                                                                                                                                                                    |
| QBit(T, N)                                                                                              | 0x36<element_type_encoding><var_uint_dimension>                                                                                                                                                                                                                                                                                                                          |
| QBit(T, N, stride)                                                                                      | 0x37<element_type_encoding><var_uint_dimension><var_uint_stride>                                                                                                                                                                                                                                                                                                         |
| UInt512, Int512, Decimal512(P, S)                                                                       | 0x2C<var_uint_descriptor_size><SentioWide_v1 descriptor> (see SentioWideTypeEncoding.h)                                                                                                                                                                                                                                                                                  |
|---------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|

For type QBit the 0x37 encoding is used only when stride differs from the dimension N. When stride == N (including when the third argument is written explicitly, as in QBit(Float32, 4, 4)) the type is canonicalized to the two-argument QBit(T, N) form and encoded as 0x36 above, so its binary encoding stays byte-identical to a non-strided QBit.
This fork refuses strided QBit (see DataTypeQBit): decoding 0x37 for the OfficialCanonical origin throws SUPPORT_IS_DISABLED.

The 512-bit types UInt512, Int512 and Decimal512(P, S) of this fork are written as Custom (0x2C) with a private, versioned descriptor
(SentioWide_v1_UInt512, SentioWide_v1_Int512, SentioWide_v1_Decimal512(P,S), see SentioWideTypeEncoding.h), which every origin decodes.
Their legacy indexes (SentioLegacyBinaryTypeIndex: 0x37 UInt512, 0x38 Int512, 0x39<uint8_precision><uint8_scale> Decimal512, 0x3A
strided QBit) are decoded only for the SentioLegacyV0 origin; the Unknown origin refuses 0x37, 0x38 and 0x39 as ambiguous before
reading their payload.

Interval kind binary encoding:
|---------------|-----------------|
| Interval kind | Binary encoding |
|---------------|-----------------|
| Nanosecond    | 0x00            |
| Microsecond   | 0x01            |
| Millisecond   | 0x02            |
| Second        | 0x03            |
| Minute        | 0x04            |
| Hour          | 0x05            |
| Day           | 0x06            |
| Week          | 0x07            |
| Month         | 0x08            |
| Quarter       | 0x09            |
| Year          | 0x0A            |
|---------------|-----------------|

Aggregate function parameter binary encoding (binary encoding of a Field, see src/Common/FieldBinaryEncoding.h):
|------------------------|------------------------------------------------------------------------------------------------------------------------------|
| Parameter type         | Binary encoding                                                                                                              |
|------------------------|------------------------------------------------------------------------------------------------------------------------------|
| Null                   | 0x00                                                                                                                         |
| UInt64                 | 0x01<var_uint_value>                                                                                                         |
| Int64                  | 0x02<var_int_value>                                                                                                          |
| UInt128                | 0x03<uint128_little_endian_value>                                                                                            |
| Int128                 | 0x04<int128_little_endian_value>                                                                                             |
| UInt256                | 0x05<uint256_little_endian_value>                                                                                            |
| Int256                 | 0x06<int256_little_endian_value>                                                                                             |
| Float64                | 0x07<float64_little_endian_value>                                                                                            |
| Decimal32              | 0x08<var_uint_scale><int32_little_endian_value>                                                                              |
| Decimal64              | 0x09<var_uint_scale><int64_little_endian_value>                                                                              |
| Decimal128             | 0x0A<var_uint_scale><int128_little_endian_value>                                                                             |
| Decimal256             | 0x0B<var_uint_scale><int256_little_endian_value>                                                                             |
| String                 | 0x0C<var_uint_size><data>                                                                                                    |
| Array                  | 0x0D<var_uint_size><value_encoding_1>...<value_encoding_N>                                                                   |
| Tuple                  | 0x0E<var_uint_size><value_encoding_1>...<value_encoding_N>                                                                   |
| Map                    | 0x0F<var_uint_size><key_encoding_1><value_encoding_1>...<key_endoding_N><value_encoding_N>                                   |
| IPv4                   | 0x10<uint32_little_endian_value>                                                                                             |
| IPv6                   | 0x11<uint128_little_endian_value>                                                                                            |
| UUID                   | 0x12<uuid_value>                                                                                                             |
| Bool                   | 0x13<bool_value>                                                                                                             |
| Object                 | 0x14<var_uint_size><var_uint_key_size_1><key_data_1><value_encoding_1>...<var_uint_key_size_N><key_data_N><value_encoding_N> |
| AggregateFunctionState | 0x15<var_uint_name_size><name_data><var_uint_data_size><data>                                                                |
| Negative infinity      | 0xFE                                                                                                                         |
| Positive infinity      | 0xFF                                                                                                                         |
|------------------------|------------------------------------------------------------------------------------------------------------------------------|
*/

/// Encode a type with the default writer, BinaryTypeWriterMode::SentioWideV1.
String encodeDataType(const DataTypePtr & type);
void encodeDataType(const DataTypePtr & type, WriteBuffer & buf);

/// Special version of type encoding that is used for generic hash calculation.
/// It can skip serializing some data types parameters.
/// It keeps the historical fingerprint of every type: the 512-bit types are written with their legacy indexes (as the fork always hashed
/// them, in every container), never with the SentioWide descriptors, so the hashes derived from it do not change.
void encodeDataTypeForHashCalculation(const DataTypePtr & type, WriteBuffer & buf);

/// Decode a type. max_complexity limits the number of decoded type nodes (0 == unlimited, the default).
/// Callers reading untrusted input (input formats, and query plans deserialized from a client via
/// receiveQueryPlan) pass the effective `input_format_binary_max_type_complexity` to guard against malicious
/// input; callers decoding already-stored server data (columns, shared-data) use the default (unlimited).
/// Without a declared origin the type is decoded for BinaryTypeOrigin::Unknown: the legacy indexes 0x37/0x38/0x39 are refused as
/// ambiguous, never guessed.
DataTypePtr decodeDataType(ReadBuffer & buf, size_t max_complexity = 0);

/// The origin of a binary type encoding decides what the indexes 0x37, 0x38, 0x39 and 0x3A mean: the Decimal512 fork of 26.3 LTS wrote
/// its 512-bit types as 0x37/0x38/0x39, upstream 26.8 writes strided `QBit` as 0x37. The origin comes from a declared input setting or
/// from stored table metadata; it is never guessed from the payload, the server version or whether a decode succeeds.
enum class BinaryTypeOrigin : uint8_t
{
    /// Undeclared: the legacy indexes 0x37/0x38/0x39 are rejected as ambiguous; self-describing encodings (including the `Custom`
    /// SentioWide descriptors, see SentioWideTypeEncoding.h) are decoded.
    Unknown = 0,
    /// Written by the Decimal512 fork with its legacy indexes: 0x37 `UInt512`, 0x38 `Int512`, 0x39 `Decimal512(P, S)`; 0x3A is refused.
    SentioLegacyV0 = 1,
    /// The upstream meaning of every index (0x37 strided `QBit`, subject to the strided `QBit` guard); never the legacy indexes.
    OfficialCanonical = 2,
};

/// Decode a type whose origin the caller declares. There is no default origin on purpose.
DataTypePtr decodeDataType(ReadBuffer & buf, BinaryTypeOrigin origin, size_t max_complexity = 0);

/// How the 512-bit types are written.
enum class BinaryTypeWriterMode : uint8_t
{
    /// `Custom` (0x2C) with the SentioWide v1 descriptors (see SentioWideTypeEncoding.h).
    SentioWideV1 = 0,
    /// The legacy indexes 0x37/0x38/0x39, only for a controlled bridge build that must stay readable by the old fork.
    SentioLegacyBridge = 1,
};

void encodeDataType(const DataTypePtr & type, WriteBuffer & buf, BinaryTypeWriterMode mode);

/// Resolve the effective input_format_binary_max_type_complexity from a context, to pass to decodeDataType.
/// Reads the setting once (not per type node), so it does not reintroduce per-value context contention.
size_t getBinaryTypeDecodingComplexityLimit(const ContextPtr & context);

}
