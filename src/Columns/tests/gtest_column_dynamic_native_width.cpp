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
#include <Common/typeid_cast.h>

#include <fmt/format.h>

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

/// The bytes of a Dynamic column in the native format with a V2 structure whose regular variants are `variants` (binary type headers,
/// written here as the Decimal512 fork's native writer wrote them) and whose data is `source` in the layout of `source_type` (written by
/// the unchanged Variant serialization).
String nativeV2Dynamic(const std::vector<String> & variant_headers, const ColumnVariant & source, const DataTypeVariant & source_type)
{
    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(UInt64(2), out);
    writeVarUInt(variant_headers.size(), out);
    for (const auto & header : variant_headers)
        out.write(header.data(), header.size());

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

MutableColumnPtr readNativeDynamic(const String & bytes, size_t rows)
{
    ReadBufferFromString in(bytes);
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::DeserializeBinaryBulkSettings settings;
    settings.getter = [&in](const ISerialization::SubstreamPath &) -> ReadBuffer * { return &in; };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
    ISerialization::DeserializeBinaryBulkStatePtr state;
    auto serialization = DataTypeDynamic().getDefaultSerialization();
    auto column = DataTypeDynamic().createColumn();
    serialization->deserializeBinaryBulkStatePrefix(settings, state, nullptr);
    serialization->deserializeBinaryBulkWithMultipleStreams(*column, rows, settings, state, nullptr);
    EXPECT_TRUE(in.eof());
    return column;
}

String writeNativeDynamic(const ColumnDynamic & column)
{
    WriteBufferFromOwnString out;
    const auto format_settings = nativeBinaryTypesSettings();
    ISerialization::SerializeBinaryBulkSettings settings;
    settings.getter = [&out](const ISerialization::SubstreamPath &) -> WriteBuffer * { return &out; };
    settings.position_independent_encoding = false;
    settings.native_format = true;
    settings.format_settings = &format_settings;
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
/// before the column holds them; the other variants stay regular. This is the layout the fork's native writer produced with binary types
/// (V2); the V3 MergeTree structure reads the same way.
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
