#include <gtest/gtest.h>

#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnDynamic.h>
#include <Columns/ColumnVariant.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDynamic.h>
#include <DataTypes/DataTypeVariant.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/TypeNameIdentity.h>
#include <DataTypes/Serializations/SerializationDynamicHelpers.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadBufferFromString.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Arena.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <Common/typeid_cast.h>

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

/// A native `Decimal512(76, 10)` (64-byte values) has the name `Decimal(76, 10)` of the 32-byte `Decimal256(76, 10)`. `Dynamic` must keep
/// the two apart: the native one is never a regular variant (those are found by name) and stays in the shared variant with its SentioWide
/// descriptor, the other one is a regular variant while there is capacity. The input bytes come from the binary type encoding
/// specification (0x1C for `Decimal256`) and from Root's fixture draft (the SentioWide v1 descriptor), not from the code under test.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

namespace
{

String varUInt(UInt64 value)
{
    WriteBufferFromOwnString out;
    writeVarUInt(value, out);
    return out.str();
}

String littleEndian(UInt64 value, size_t width)
{
    String out(width, '\0');
    for (size_t i = 0; i < 8 && i < width; ++i)
        out[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    return out;
}

String hexOf(std::string_view bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    String out;
    for (unsigned char c : bytes)
    {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

/// The binary Dynamic value of a native `Decimal512(76, 10)` with the raw value 1: the SentioWide v1 header and 64 bytes.
String nativeDecimal512Value()
{
    const String descriptor = "SentioWide_v1_Decimal512(76,10)";
    return String(1, 0x2C) + varUInt(descriptor.size()) + descriptor + littleEndian(1, 64);
}

/// The binary Dynamic value of a `Decimal256(76, 10)` with the raw value 2: 0x1C, the precision, the scale and 32 bytes.
String decimal256Value()
{
    return String{0x1C, 76, 10} + littleEndian(2, 32);
}

DataTypePtr nativeDecimal512()
{
    return std::make_shared<DataTypeDecimal<Decimal512>>(76, 10);
}

DataTypePtr decimal256()
{
    return std::make_shared<DataTypeDecimal<Decimal256>>(76, 10);
}

void insertBinary(ColumnDynamic & column, const String & value)
{
    ReadBufferFromString in(value);
    DataTypeDynamic(column.getMaxDynamicTypes()).getDefaultSerialization()->deserializeBinary(column, in, FormatSettings{});
    ASSERT_TRUE(in.eof());
}

String serializeBinary(const ColumnDynamic & column, size_t row)
{
    WriteBufferFromOwnString out;
    DataTypeDynamic(column.getMaxDynamicTypes()).getDefaultSerialization()->serializeBinary(column, row, out, FormatSettings{});
    return out.str();
}

bool isInSharedVariant(const ColumnDynamic & column, size_t row)
{
    return column.getVariantColumn().globalDiscriminatorAt(row) == column.getSharedVariantDiscriminator();
}

void expectClassAt(const ColumnDynamic & column, size_t row, TypeIndex expected, std::string_view where)
{
    auto type = column.getTypeAt(row);
    EXPECT_EQ(type->getTypeId(), expected) << where;
    EXPECT_EQ(type->getName(), "Decimal(76, 10)") << where;
    EXPECT_EQ(type->getSizeOfValueInMemory(), expected == TypeIndex::Decimal512 ? 64U : 32U) << where;
}

}

TEST(ColumnDynamicNativeWidth, TypeNameIdentity)
{
    EXPECT_FALSE(isTypeIdentifiedByName(*nativeDecimal512()));
    EXPECT_TRUE(isTypeIdentifiedByName(*decimal256()));
    EXPECT_TRUE(isTypeIdentifiedByName(DataTypeDecimal<Decimal512>(154, 10)));
    EXPECT_TRUE(isTypeIdentifiedByName(DataTypeDecimal<Decimal512>(77, 76)));
    /// A scale greater than the precision names no type.
    EXPECT_FALSE(isTypeIdentifiedByName(DataTypeDecimal<Decimal512>(10, 20)));
    EXPECT_FALSE(isTypeIdentifiedByName(DataTypeDecimal<Decimal256>(20, 2)));
    EXPECT_FALSE(isTypeIdentifiedByName(DataTypeArray(nativeDecimal512())));
    EXPECT_TRUE(isTypeIdentifiedByName(DataTypeArray(decimal256())));
    EXPECT_TRUE(isTypeIdentifiedByName(DataTypeInt64()));

    EXPECT_NE(getTypeIdentityKey(nativeDecimal512()), getTypeIdentityKey(decimal256()));
    EXPECT_EQ(getTypeIdentityKey(decimal256()), "Decimal(76, 10)");
}

/// A name that no other type has: decimals below `Decimal512` share their names with native decimals of wider classes.
TEST(ColumnDynamicNativeWidth, TypeNameUnambiguity)
{
    EXPECT_TRUE(isTypeNameUnambiguous(DataTypeInt64()));
    EXPECT_TRUE(isTypeNameUnambiguous(DataTypeArray(std::make_shared<DataTypeInt64>())));
    EXPECT_TRUE(isTypeNameUnambiguous(DataTypeDecimal<Decimal512>(100, 2)));
    EXPECT_FALSE(isTypeNameUnambiguous(*decimal256()));
    EXPECT_FALSE(isTypeNameUnambiguous(DataTypeDecimal<Decimal64>(18, 2)));
    EXPECT_FALSE(isTypeNameUnambiguous(DataTypeArray(decimal256())));
    EXPECT_FALSE(isTypeNameUnambiguous(*nativeDecimal512()));
}

TEST(ColumnDynamicNativeWidth, MixedValuesKeepTheirClass)
{
    for (size_t max_types : {0UL, 32UL})
    {
        for (bool native_first : {true, false})
        {
            const String where = fmt::format("max_types {} native_first {}", max_types, native_first);
            const String first = native_first ? nativeDecimal512Value() : decimal256Value();
            const String second = native_first ? decimal256Value() : nativeDecimal512Value();
            const size_t native_row = native_first ? 0 : 1;
            const size_t twin_row = 1 - native_row;

            auto column = ColumnDynamic::create(max_types);
            insertBinary(*column, first);
            insertBinary(*column, second);
            ASSERT_EQ(column->size(), 2U) << where;

            expectClassAt(*column, native_row, TypeIndex::Decimal512, where);
            expectClassAt(*column, twin_row, TypeIndex::Decimal256, where);
            EXPECT_TRUE(isInSharedVariant(*column, native_row)) << where;
            EXPECT_EQ(isInSharedVariant(*column, twin_row), max_types == 0) << where;

            /// The exact descriptor and the whole 64-byte (and 32-byte) payload are written back.
            EXPECT_EQ(hexOf(serializeBinary(*column, 0)), hexOf(first)) << where;
            EXPECT_EQ(hexOf(serializeBinary(*column, 1)), hexOf(second)) << where;

            /// Different types under one name: ordered consistently, never equal.
            int order = column->compareAt(0, 1, *column, 1);
            EXPECT_NE(order, 0) << where;
            EXPECT_EQ(column->compareAt(1, 0, *column, 1), -order) << where;

            /// The arena form keeps the class too.
            auto copy = ColumnDynamic::create(max_types);
            Arena arena;
            const auto settings = IColumn::SerializationSettings::createForAggregationState();
            for (size_t row = 0; row < 2; ++row)
            {
                const char * begin = nullptr;
                auto value = column->serializeValueIntoArena(row, arena, begin, &settings);
                ReadBufferFromMemory in(value.data(), value.size());
                copy->deserializeAndInsertFromArena(in, &settings);
                EXPECT_TRUE(in.eof()) << where;
            }
            expectClassAt(*copy, native_row, TypeIndex::Decimal512, where + " arena");
            expectClassAt(*copy, twin_row, TypeIndex::Decimal256, where + " arena");
            EXPECT_EQ(hexOf(serializeBinary(*copy, native_row)), hexOf(nativeDecimal512Value())) << where;
        }
    }
}

TEST(ColumnDynamicNativeWidth, VariantRefusesNameTwinsBeforeDeduplication)
{
    EXPECT_THROW(
        {
            try
            {
                DataTypeVariant variant({decimal256(), nativeDecimal512()});
            }
            catch (const Exception & e)
            {
                EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
                throw;
            }
        },
        Exception);

    /// Identical types are still squashed.
    EXPECT_EQ(DataTypeVariant({decimal256(), decimal256()}).getVariants().size(), 1U);
}

TEST(ColumnDynamicNativeWidth, UnflattenPlacesEveryTypeOnItsOwn)
{
    for (size_t max_types : {0UL, 1UL, 32UL})
    {
        for (bool native_first : {true, false})
        {
            const String where = fmt::format("max_types {} native_first {}", max_types, native_first);

            auto native_column = ColumnDecimal<Decimal512>::create(0, 10);
            native_column->insertValue(Decimal512(1));
            auto int64_column = ColumnInt64::create();
            int64_column->insertValue(7);

            FlattenedDynamicColumn flattened;
            if (native_first)
            {
                flattened.types = {nativeDecimal512(), std::make_shared<DataTypeInt64>()};
                flattened.columns = {std::move(native_column), std::move(int64_column)};
            }
            else
            {
                flattened.types = {std::make_shared<DataTypeInt64>(), nativeDecimal512()};
                flattened.columns = {std::move(int64_column), std::move(native_column)};
            }
            auto indexes = ColumnUInt8::create();
            indexes->insertValue(0);
            indexes->insertValue(1);
            flattened.indexes_column = std::move(indexes);
            flattened.indexes_type = std::make_shared<DataTypeUInt8>();

            auto column = ColumnDynamic::create(max_types);
            unflattenDynamicColumn(std::move(flattened), *column);
            ASSERT_EQ(column->size(), 2U) << where;

            const size_t native_row = native_first ? 0 : 1;
            const size_t int64_row = 1 - native_row;
            EXPECT_TRUE(isInSharedVariant(*column, native_row)) << where;
            /// The native type cannot be a regular variant, but it does not take regular placement from the type after it.
            EXPECT_EQ(isInSharedVariant(*column, int64_row), max_types == 0) << where;
            EXPECT_EQ(column->getTypeAt(native_row)->getTypeId(), TypeIndex::Decimal512) << where;
            EXPECT_EQ(column->getTypeAt(int64_row)->getTypeId(), TypeIndex::Int64) << where;
            EXPECT_EQ(hexOf(serializeBinary(*column, native_row)), hexOf(nativeDecimal512Value())) << where;
        }
    }
}

namespace
{

/// Native-format bulk settings with one stream, as NativeWriter and NativeReader use them, with the types in binary form.
FormatSettings nativeBinaryTypesSettings()
{
    FormatSettings settings;
    settings.native.encode_types_in_binary_format = true;
    settings.native.decode_types_in_binary_format = true;
    return settings;
}

/// The bytes of a Dynamic column in the native format with a V2 structure whose regular variants have the binary type headers
/// `variant_headers` (in the layout of the native writer), followed by `statistics` if given, and whose data is `source` in the layout of
/// `source_type` (written by the unchanged Variant serialization).
String nativeV2Dynamic(
    const std::vector<String> & variant_headers,
    const ColumnVariant & source,
    const DataTypeVariant & source_type,
    const String & statistics = {})
{
    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(UInt64(2), out);
    writeVarUInt(variant_headers.size(), out);
    for (const auto & header : variant_headers)
        out.write(header.data(), header.size());
    out.write(statistics.data(), statistics.size());

    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::SerializeBinaryBulkSettings settings;
    settings.getter = [&out](const ISerialization::SubstreamPath &) -> WriteBuffer * { return &out; };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
    ISerialization::SerializeBinaryBulkStatePtr state;
    auto serialization = source_type.getDefaultSerialization();
    serialization->serializeBinaryBulkStatePrefix(source, settings, state);
    serialization->serializeBinaryBulkWithMultipleStreams(source, 0, 0, settings, state);
    serialization->serializeBinaryBulkStateSuffix(settings, state);
    return out.str();
}

MutableColumnPtr readNativeDynamic(const String & bytes, size_t rows, bool read_statistics = false)
{
    ReadBufferFromString in(bytes);
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::DeserializeBinaryBulkSettings settings;
    settings.getter = [&in](const ISerialization::SubstreamPath &) -> ReadBuffer * { return &in; };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
    settings.object_and_dynamic_read_statistics = read_statistics;
    ISerialization::DeserializeBinaryBulkStatePtr state;
    auto serialization = DataTypeDynamic().getDefaultSerialization();
    auto column = DataTypeDynamic().createColumn();
    serialization->deserializeBinaryBulkStatePrefix(settings, state, nullptr);
    serialization->deserializeBinaryBulkWithMultipleStreams(*column, rows, settings, state, nullptr);
    EXPECT_TRUE(in.eof());
    return column;
}

using StatisticsMode = ISerialization::SerializeBinaryBulkSettings::StatisticsMode;

String writeNativeDynamic(const ColumnDynamic & column, StatisticsMode statistics_mode = StatisticsMode::NONE)
{
    WriteBufferFromOwnString out;
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::SerializeBinaryBulkSettings settings;
    settings.getter = [&out](const ISerialization::SubstreamPath &) -> WriteBuffer * { return &out; };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
    settings.write_statistics = statistics_mode;
    ISerialization::SerializeBinaryBulkStatePtr state;
    auto serialization = DataTypeDynamic().getDefaultSerialization();
    serialization->serializeBinaryBulkStatePrefix(column, settings, state);
    serialization->serializeBinaryBulkWithMultipleStreams(column, 0, 0, settings, state);
    serialization->serializeBinaryBulkStateSuffix(settings, state);
    return out.str();
}

}

/// A Dynamic structure written with binary type headers can have a single regular variant of a type that its name does not identify: the
/// descriptor identifies it exactly, so the data is read in its layout, and its values move into the shared variant (with the descriptor)
/// before the column holds them; the other variants stay regular. The V3 MergeTree structure reads the same way. The fixture has the
/// canonical `Custom` descriptor, so this is the layout migration of canonical data, not data written by the Decimal512 fork (its headers
/// have 0x39): the declared legacy origin, the refusal without it and the real writer modes of the fork are in the final compatibility
/// harness.
TEST(ColumnDynamicNativeWidth, OldBinaryStructureWithNativeRegularVariant)
{
    const std::pair<UInt32, UInt32> native_states[] = {{76, 10}, {10, 20}};
    for (const auto & [precision, scale] : native_states)
    {
        const String where = fmt::format("Decimal512({}, {})", precision, scale);
        const auto native = std::make_shared<DataTypeDecimal<Decimal512>>(precision, scale);
        const String descriptor = fmt::format("SentioWide_v1_Decimal512({},{})", precision, scale);
        const String native_header = String(1, 0x2C) + varUInt(descriptor.size()) + descriptor;

        /// The source layout: Variant(native, Int64, SharedVariant) with rows [native -5, Int64 7, NULL].
        const auto source_type = std::make_shared<DataTypeVariant>(DataTypes{native, std::make_shared<DataTypeInt64>(), ColumnDynamic::getSharedVariantDataType()});
        auto source = source_type->createColumn();
        auto & source_variant = assert_cast<ColumnVariant &>(*source);
        auto native_values = ColumnDecimal<Decimal512>::create(0, scale);
        native_values->insertValue(Decimal512(-5));
        auto int64_values = ColumnInt64::create();
        int64_values->insertValue(7);
        source_variant.insertIntoVariantFrom(*source_type->tryGetVariantDiscriminator(native->getName()), *native_values, 0);
        source_variant.insertIntoVariantFrom(*source_type->tryGetVariantDiscriminator("Int64"), *int64_values, 0);
        source_variant.insertDefault();

        const String bytes = nativeV2Dynamic({native_header, String(1, 0x0A)}, source_variant, *source_type);
        auto read = readNativeDynamic(bytes, 3);
        auto & column = assert_cast<ColumnDynamic &>(*read);
        ASSERT_EQ(column.size(), 3U) << where;

        /// The native value: in the shared variant, with its class, descriptor and 64-byte payload (-5, two's complement).
        EXPECT_TRUE(isInSharedVariant(column, 0)) << where;
        const auto native_type = column.getTypeAt(0);
        ASSERT_EQ(native_type->getTypeId(), TypeIndex::Decimal512) << where;
        const auto & native_decimal = assert_cast<const DataTypeDecimal<Decimal512> &>(*native_type);
        EXPECT_EQ(native_decimal.getPrecision(), precision) << where;
        EXPECT_EQ(native_decimal.getScale(), scale) << where;
        EXPECT_EQ(hexOf(serializeBinary(column, 0)), hexOf(native_header + String(1, char(0xFB)) + String(63, char(0xFF)))) << where;

        /// The other variant stays regular, and NULL stays NULL.
        EXPECT_FALSE(isInSharedVariant(column, 1)) << where;
        EXPECT_EQ(column.getTypeAt(1)->getTypeId(), TypeIndex::Int64) << where;
        EXPECT_TRUE(column.isNullAt(2)) << where;

        /// Written again (canonically: the native value in the shared variant) and read back, the same.
        auto reread = readNativeDynamic(writeNativeDynamic(column), 3);
        auto & column_again = assert_cast<ColumnDynamic &>(*reread);
        EXPECT_TRUE(isInSharedVariant(column_again, 0)) << where;
        EXPECT_EQ(hexOf(serializeBinary(column_again, 0)), hexOf(serializeBinary(column, 0))) << where;
        EXPECT_EQ(column_again.getTypeAt(1)->getTypeId(), TypeIndex::Int64) << where;
        EXPECT_TRUE(column_again.isNullAt(2)) << where;
    }
}

/// Two different types with one name are not representable as variants: the structure is refused before anything is read.
TEST(ColumnDynamicNativeWidth, OldBinaryStructureWithNameTwinsIsRefused)
{
    const String native_descriptor = "SentioWide_v1_Decimal512(76,10)";
    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(UInt64(2), out);
    writeVarUInt(2, out);
    out.write(char(0x2C));
    writeVarUInt(native_descriptor.size(), out);
    out.write(native_descriptor.data(), native_descriptor.size());
    out.write(String{0x1C, 76, 10}.data(), 3);

    ReadBufferFromString in(out.str());
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::DeserializeBinaryBulkSettings settings;
    settings.getter = [&in](const ISerialization::SubstreamPath &) -> ReadBuffer * { return &in; };
    settings.native_format = true;
    settings.format_settings = &format_settings;
    ISerialization::DeserializeBinaryBulkStatePtr state;
    try
    {
        DataTypeDynamic().getDefaultSerialization()->deserializeBinaryBulkStatePrefix(settings, state, nullptr);
        ADD_FAILURE() << "the structure was accepted";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS) << e.message();
    }
}

namespace
{

/// The binary Dynamic values of a native `Decimal512(precision, scale)` (SentioWide v1 header, 64 bytes), of a `Decimal256(76, 10)` (0x1C,
/// 32 bytes), of an `Int64` (0x0A) and of a `String` (0x15).
String nativeValue(UInt32 precision, UInt32 scale, UInt64 raw)
{
    const String descriptor = fmt::format("SentioWide_v1_Decimal512({},{})", precision, scale);
    return String(1, 0x2C) + varUInt(descriptor.size()) + descriptor + littleEndian(raw, 64);
}

String decimal256ValueOf(UInt64 raw)
{
    return String{0x1C, 76, 10} + littleEndian(raw, 32);
}

String int64Value(UInt64 value)
{
    return String(1, 0x0A) + littleEndian(value, 8);
}

String stringValue(const String & value)
{
    return String(1, 0x15) + varUInt(value.size()) + value;
}

std::vector<String> sortedVariantNames(const ColumnDynamic & column)
{
    auto names = column.getVariantInfo().variant_names;
    std::sort(names.begin(), names.end());
    return names;
}

/// A row: its class, its place (the shared variant or a regular variant) and its exact binary value.
void expectRow(const ColumnDynamic & column, size_t row, const String & value, TypeIndex type, bool shared, std::string_view where)
{
    EXPECT_EQ(column.getTypeAt(row)->getTypeId(), type) << where << ", row " << row;
    EXPECT_EQ(isInSharedVariant(column, row), shared) << where << ", row " << row;
    EXPECT_EQ(hexOf(serializeBinary(column, row)), hexOf(value)) << where << ", row " << row;
}

/// A native value: its class, precision, scale and value size.
void expectNative(const ColumnDynamic & column, size_t row, UInt32 precision, UInt32 scale, std::string_view where)
{
    const auto type = column.getTypeAt(row);
    ASSERT_EQ(type->getTypeId(), TypeIndex::Decimal512) << where << ", row " << row;
    const auto & decimal = assert_cast<const DataTypeDecimal<Decimal512> &>(*type);
    EXPECT_EQ(decimal.getPrecision(), precision) << where << ", row " << row;
    EXPECT_EQ(decimal.getScale(), scale) << where << ", row " << row;
    EXPECT_EQ(type->getSizeOfValueInMemory(), 64U) << where << ", row " << row;
}

/// The statistics of a V2 structure: the size of every variant of `type` in its order (the shared variant included), then the number of
/// named types of the shared variant, with the name and the size of each.
String structureStatistics(
    const DataTypeVariant & type,
    const std::map<String, UInt64> & variant_sizes,
    const std::vector<std::pair<String, UInt64>> & shared_sizes)
{
    String out;
    for (const auto & variant : type.getVariants())
        out += varUInt(variant_sizes.at(variant->getName()));
    out += varUInt(shared_sizes.size());
    for (const auto & [name, size] : shared_sizes)
        out += varUInt(name.size()) + name + varUInt(size);
    return out;
}

bool isStructureStream(const ISerialization::SubstreamPath & path)
{
    return !path.empty() && path.back().type == ISerialization::Substream::DynamicStructure;
}

/// A Dynamic column written as the native format writes it, but with the structure (and the statistics of `statistics_mode`) in a stream
/// of its own, as MergeTree has it, and read back from the two streams with the statistics.
std::pair<String, String> writeDynamicStreams(const ColumnDynamic & column, StatisticsMode statistics_mode)
{
    WriteBufferFromOwnString structure;
    WriteBufferFromOwnString data;
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::SerializeBinaryBulkSettings settings;
    settings.getter = [&](const ISerialization::SubstreamPath & path) -> WriteBuffer *
    {
        if (isStructureStream(path))
            return &structure;
        return &data;
    };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
    settings.write_statistics = statistics_mode;
    ISerialization::SerializeBinaryBulkStatePtr state;
    auto serialization = DataTypeDynamic().getDefaultSerialization();
    serialization->serializeBinaryBulkStatePrefix(column, settings, state);
    serialization->serializeBinaryBulkWithMultipleStreams(column, 0, 0, settings, state);
    serialization->serializeBinaryBulkStateSuffix(settings, state);
    return {structure.str(), data.str()};
}

MutableColumnPtr readDynamicStreams(const String & structure, const String & data, size_t rows)
{
    ReadBufferFromString structure_in(structure);
    ReadBufferFromString data_in(data);
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::DeserializeBinaryBulkSettings settings;
    settings.getter = [&](const ISerialization::SubstreamPath & path) -> ReadBuffer *
    {
        if (isStructureStream(path))
            return &structure_in;
        return &data_in;
    };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
    settings.object_and_dynamic_read_statistics = true;
    ISerialization::DeserializeBinaryBulkStatePtr state;
    auto serialization = DataTypeDynamic().getDefaultSerialization();
    auto column = DataTypeDynamic().createColumn();
    serialization->deserializeBinaryBulkStatePrefix(settings, state, nullptr);
    serialization->deserializeBinaryBulkWithMultipleStreams(*column, rows, settings, state, nullptr);
    EXPECT_TRUE(structure_in.eof());
    EXPECT_TRUE(data_in.eof());
    return column;
}

}

/// The statistics name a type of the shared variant only if its name identifies it, both when they are calculated for a column in memory
/// and when they are recalculated while the column is written (into the suffix of the structure stream, as MergeTree writes them). The size
/// of the shared variant counts every value; the real `Decimal256(76, 10)` is named, the native decimals are not.
TEST(ColumnDynamicNativeWidth, StatisticsNameOnlyIdentifiedTypes)
{
    /// One regular variant (Int64); the native decimals, a real Decimal256 and a String in the shared variant.
    const std::vector<String> values{int64Value(7), nativeValue(10, 20, 1), nativeValue(76, 10, 2), decimal256ValueOf(3), stringValue("s")};
    auto column = ColumnDynamic::create(1);
    for (const auto & value : values)
        insertBinary(*column, value);

    auto expect_statistics = [](const ColumnDynamic::Statistics & statistics, std::string_view where)
    {
        EXPECT_EQ(statistics.variants_statistics.size(), 2U) << where;
        EXPECT_EQ(statistics.variants_statistics.at("Int64"), 1U) << where;
        EXPECT_EQ(statistics.variants_statistics.at(ColumnDynamic::getSharedVariantTypeName()), 4U) << where;
        EXPECT_EQ(statistics.shared_variants_statistics.size(), 2U) << where;
        EXPECT_EQ(statistics.shared_variants_statistics.at("Decimal(76, 10)"), 1U) << where;
        EXPECT_EQ(statistics.shared_variants_statistics.at("String"), 1U) << where;
    };
    expect_statistics(*column->getOrCalculateStatistics(), "calculated");

    const auto [structure, data] = writeDynamicStreams(*column, StatisticsMode::SUFFIX);
    auto read = readDynamicStreams(structure, data, values.size());
    const auto & column_read = assert_cast<const ColumnDynamic &>(*read);
    ASSERT_TRUE(column_read.getStatistics());
    expect_statistics(*column_read.getStatistics(), "suffix");
    ASSERT_EQ(column_read.size(), values.size());
    for (size_t row = 0; row < values.size(); ++row)
        EXPECT_EQ(hexOf(serializeBinary(column_read, row)), hexOf(values[row])) << row;
}

/// The statistics of the shared variant are hints by name, and they can come from older data. A merge takes a type from them only if no
/// other type has its name: a name of no type (`Decimal(10, 20)`, a scale greater than the precision) and a name that a native decimal has
/// (`Decimal(76, 10)`) neither fail the merge nor add a `Decimal256` variant. Every value keeps its class, the native decimals stay in the
/// shared variant, an ordinary type (String) is still promoted from the statistics, and a real Decimal256 still goes to its regular
/// variant.
TEST(ColumnDynamicNativeWidth, MergeStructureIgnoresUntrustedStatisticsNames)
{
    /// A: the native (10, 20) and (76, 10) values in the shared variant, an Int64 and a real Decimal256(76, 10) as regular variants.
    const std::vector<String> a_values{nativeValue(10, 20, 1), nativeValue(76, 10, 2), int64Value(7), decimal256ValueOf(3)};
    auto a = ColumnDynamic::create(32);
    for (const auto & value : a_values)
        insertBinary(*a, value);
    /// Its calculated statistics count every value of the shared variant but do not name the native decimals.
    const auto a_statistics = a->getOrCalculateStatistics();
    EXPECT_EQ(a_statistics->variants_statistics.at(ColumnDynamic::getSharedVariantTypeName()), 2U);
    EXPECT_EQ(a_statistics->variants_statistics.at("Int64"), 1U);
    EXPECT_EQ(a_statistics->variants_statistics.at("Decimal(76, 10)"), 1U);
    EXPECT_TRUE(a_statistics->shared_variants_statistics.empty());

    /// B (one regular variant, Int64): a String, the native decimals and a real Decimal256 in the shared variant, with statistics of
    /// older data that name the native decimals.
    const std::vector<String> b_values{
        int64Value(8), stringValue("s"), nativeValue(10, 20, 4), nativeValue(76, 10, 5), decimal256ValueOf(6)};
    auto b = ColumnDynamic::create(1);
    for (const auto & value : b_values)
        insertBinary(*b, value);
    auto b_statistics = std::make_shared<ColumnDynamic::Statistics>();
    b_statistics->variants_statistics["Int64"] = 1;
    b_statistics->variants_statistics[ColumnDynamic::getSharedVariantTypeName()] = 4;
    b_statistics->shared_variants_statistics["String"] = 25;
    b_statistics->shared_variants_statistics["Decimal(10, 20)"] = 50;
    b_statistics->shared_variants_statistics["Decimal(76, 10)"] = 40;
    b->setStatistics(b_statistics);

    /// B alone: String is promoted, no decimal variant is chosen.
    {
        auto merged = ColumnDynamic::create(32);
        merged->chooseDynamicStructureForMerge({b->getPtr()}, std::nullopt);
        EXPECT_EQ(sortedVariantNames(*merged), (std::vector<String>{"Int64", "SharedVariant", "String"}));
        merged->insertRangeFrom(*b, 0, b->size());
        ASSERT_EQ(merged->size(), b_values.size());
        expectRow(*merged, 0, b_values[0], TypeIndex::Int64, false, "B");
        expectRow(*merged, 1, b_values[1], TypeIndex::String, false, "B");
        expectRow(*merged, 2, b_values[2], TypeIndex::Decimal512, true, "B");
        expectRow(*merged, 3, b_values[3], TypeIndex::Decimal512, true, "B");
        expectRow(*merged, 4, b_values[4], TypeIndex::Decimal256, true, "B");
        expectNative(*merged, 2, 10, 20, "B");
        expectNative(*merged, 3, 76, 10, "B");
    }

    /// A and B: the Decimal256 variant comes from A, and the Decimal256 of B goes there too.
    auto merged = ColumnDynamic::create(32);
    merged->chooseDynamicStructureForMerge({a->getPtr(), b->getPtr()}, std::nullopt);
    EXPECT_EQ(sortedVariantNames(*merged), (std::vector<String>{"Decimal(76, 10)", "Int64", "SharedVariant", "String"}));
    merged->insertRangeFrom(*a, 0, a->size());
    merged->insertRangeFrom(*b, 0, b->size());
    ASSERT_EQ(merged->size(), a_values.size() + b_values.size());
    expectRow(*merged, 0, a_values[0], TypeIndex::Decimal512, true, "A and B");
    expectRow(*merged, 1, a_values[1], TypeIndex::Decimal512, true, "A and B");
    expectRow(*merged, 2, a_values[2], TypeIndex::Int64, false, "A and B");
    expectRow(*merged, 3, a_values[3], TypeIndex::Decimal256, false, "A and B");
    expectRow(*merged, 4, b_values[0], TypeIndex::Int64, false, "A and B");
    expectRow(*merged, 5, b_values[1], TypeIndex::String, false, "A and B");
    expectRow(*merged, 6, b_values[2], TypeIndex::Decimal512, true, "A and B");
    expectRow(*merged, 7, b_values[3], TypeIndex::Decimal512, true, "A and B");
    expectRow(*merged, 8, b_values[4], TypeIndex::Decimal256, false, "A and B");
    expectNative(*merged, 0, 10, 20, "A and B");
    expectNative(*merged, 1, 76, 10, "A and B");
    expectNative(*merged, 6, 10, 20, "A and B");
    expectNative(*merged, 7, 76, 10, "A and B");

    /// The statistics of the merged data: the size of a name that other types can have is not added to the regular variant of that name.
    merged->takeOrCalculateStatisticsFrom({a->getPtr(), b->getPtr()});
    const auto & statistics = merged->getStatistics();
    ASSERT_TRUE(statistics);
    EXPECT_EQ(statistics->variants_statistics.at("Decimal(76, 10)"), 1U);
    EXPECT_EQ(statistics->variants_statistics.at("Int64"), 2U);
    EXPECT_EQ(statistics->variants_statistics.at("String"), 25U);
    EXPECT_EQ(statistics->variants_statistics.at(ColumnDynamic::getSharedVariantTypeName()), 6U);
    EXPECT_EQ(statistics->shared_variants_statistics.at("Decimal(76, 10)"), 40U);
    EXPECT_EQ(statistics->shared_variants_statistics.at("Decimal(10, 20)"), 50U);
}

/// The statistics of a binary structure describe the whole data in its own layout. When a single native regular variant moves into the
/// shared variant (see OldBinaryStructureWithNativeRegularVariant), the column gets the statistics of the published layout: the size of
/// that variant (of the whole data, not of this block) is added to the shared variant, and its name is neither a variant nor a type of the
/// shared variant. Written again with the statistics in the prefix and read back, they stay the same, and a merge chooses its structure
/// from them.
TEST(ColumnDynamicNativeWidth, MigratedNativeVariantStatisticsMoveToTheSharedVariant)
{
    const auto native = nativeDecimal512();
    const String descriptor = "SentioWide_v1_Decimal512(76,10)";
    const String native_header = String(1, 0x2C) + varUInt(descriptor.size()) + descriptor;

    /// The source layout: Variant(native, Int64, SharedVariant) with rows [native -5, Int64 7, NULL, native 3].
    const auto source_type = std::make_shared<DataTypeVariant>(
        DataTypes{native, std::make_shared<DataTypeInt64>(), ColumnDynamic::getSharedVariantDataType()});
    auto source = source_type->createColumn();
    auto & source_variant = assert_cast<ColumnVariant &>(*source);
    auto native_values = ColumnDecimal<Decimal512>::create(0, 10);
    native_values->insertValue(Decimal512(-5));
    native_values->insertValue(Decimal512(3));
    auto int64_values = ColumnInt64::create();
    int64_values->insertValue(7);
    const auto native_discriminator = *source_type->tryGetVariantDiscriminator(native->getName());
    source_variant.insertIntoVariantFrom(native_discriminator, *native_values, 0);
    source_variant.insertIntoVariantFrom(*source_type->tryGetVariantDiscriminator("Int64"), *int64_values, 0);
    source_variant.insertDefault();
    source_variant.insertIntoVariantFrom(native_discriminator, *native_values, 1);

    /// The statistics of the whole data: 20 native values, 10 Int64 values and 5 values in the shared variant, which older statistics name
    /// as 5 Strings and 7 `Decimal(76, 10)` values.
    const String statistics = structureStatistics(
        *source_type,
        {{native->getName(), 20}, {"Int64", 10}, {ColumnDynamic::getSharedVariantTypeName(), 5}},
        {{"String", 5}, {"Decimal(76, 10)", 7}});
    const String bytes = nativeV2Dynamic({native_header, String(1, 0x0A)}, source_variant, *source_type, statistics);

    const String native_minus_five = native_header + String(1, char(0xFB)) + String(63, char(0xFF));
    const String native_three = native_header + littleEndian(3, 64);
    auto expect_rows = [&](const ColumnDynamic & column, std::string_view where)
    {
        ASSERT_EQ(column.size(), 4U) << where;
        expectRow(column, 0, native_minus_five, TypeIndex::Decimal512, true, where);
        expectRow(column, 1, int64Value(7), TypeIndex::Int64, false, where);
        EXPECT_TRUE(column.isNullAt(2)) << where;
        expectRow(column, 3, native_three, TypeIndex::Decimal512, true, where);
        expectNative(column, 0, 76, 10, where);
        expectNative(column, 3, 76, 10, where);
    };
    auto expect_statistics = [](const ColumnDynamic & column, std::string_view where)
    {
        const auto & column_statistics = column.getStatistics();
        ASSERT_TRUE(column_statistics) << where;
        EXPECT_EQ(column_statistics->variants_statistics.size(), 2U) << where;
        EXPECT_EQ(column_statistics->variants_statistics.at("Int64"), 10U) << where;
        EXPECT_EQ(column_statistics->variants_statistics.at(ColumnDynamic::getSharedVariantTypeName()), 25U) << where;
        EXPECT_EQ(column_statistics->shared_variants_statistics.size(), 2U) << where;
        EXPECT_EQ(column_statistics->shared_variants_statistics.at("String"), 5U) << where;
        EXPECT_EQ(column_statistics->shared_variants_statistics.at("Decimal(76, 10)"), 7U) << where;
    };

    auto read = readNativeDynamic(bytes, 4, true);
    const auto & column = assert_cast<const ColumnDynamic &>(*read);
    expect_rows(column, "read");
    expect_statistics(column, "read");

    auto reread = readNativeDynamic(writeNativeDynamic(column, StatisticsMode::PREFIX), 4, true);
    const auto & column_again = assert_cast<const ColumnDynamic &>(*reread);
    expect_rows(column_again, "re-read");
    expect_statistics(column_again, "re-read");

    /// A merge: String is promoted from the statistics, `Decimal(76, 10)` is not, and every value keeps its class and place.
    auto merged = ColumnDynamic::create(32);
    merged->chooseDynamicStructureForMerge({column_again.getPtr()}, std::nullopt);
    EXPECT_EQ(sortedVariantNames(*merged), (std::vector<String>{"Int64", "SharedVariant", "String"}));
    merged->insertRangeFrom(column_again, 0, column_again.size());
    expect_rows(*merged, "merged");
    merged->takeOrCalculateStatisticsFrom({column_again.getPtr()});
    const auto & merged_statistics = merged->getStatistics();
    ASSERT_TRUE(merged_statistics);
    EXPECT_EQ(merged_statistics->variants_statistics.at("Int64"), 10U);
    EXPECT_EQ(merged_statistics->variants_statistics.at(ColumnDynamic::getSharedVariantTypeName()), 25U);
    EXPECT_EQ(merged_statistics->variants_statistics.at("String"), 5U);
    EXPECT_EQ(merged_statistics->shared_variants_statistics.at("Decimal(76, 10)"), 7U);
}
