#include <gtest/gtest.h>

#include <Columns/ColumnString.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/BinaryValueNestingLimitException.h>
#include <DataTypes/SentioWideRecode.h>
#include <Formats/FormatSettings.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

#include <fmt/format.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

/// Binary `Dynamic` values (a binary type header and the value) where they enter memory: their binary type headers are validated with the
/// origin (Unknown refuses the legacy indexes of the Decimal512 fork), normalized for SentioLegacyV0, written again with the legacy
/// indexes for SentioLegacyBridge; the payload of a type without nested headers is never parsed; every header has its own type complexity
/// limit and only the depth of nested values is bounded, so siblings and rows are not added up. The bytes are taken from the binary type
/// encoding specification (upstream indexes) and from Root's fixture draft (SentioWide v1 descriptors), not from the code under test.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int INCORRECT_DATA;
}

namespace
{

constexpr char NOTHING = 0x00;
constexpr char INT64 = 0x0A;
constexpr char STRING = 0x15;
constexpr char ARRAY = 0x1E;
constexpr char UNNAMED_TUPLE = 0x1F;
constexpr char DYNAMIC = 0x2B;
constexpr char CUSTOM = 0x2C;
constexpr char LEGACY_DECIMAL512 = 0x39;

String varUInt(UInt64 value)
{
    WriteBufferFromOwnString out;
    writeVarUInt(value, out);
    return out.str();
}

/// `value` as `width` little-endian bytes.
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

/// The SentioWide v1 header of `Decimal512(p, s)`: `Custom`, the size of the descriptor and the descriptor.
String canonicalDecimal512Header(UInt32 p, UInt32 s)
{
    const String descriptor = fmt::format("SentioWide_v1_Decimal512({},{})", p, s);
    return String(1, CUSTOM) + varUInt(descriptor.size()) + descriptor;
}

/// The legacy header of `Decimal512(p, s)` written by the Decimal512 fork: 0x39, the precision and the scale.
String legacyDecimal512Header(UInt32 p, UInt32 s)
{
    return String{LEGACY_DECIMAL512, static_cast<char>(p), static_cast<char>(s)};
}

/// `Dynamic` with max_dynamic_types 32.
String dynamicHeader()
{
    return String{DYNAMIC, 0x20};
}

String int64Value(UInt64 value)
{
    return String(1, INT64) + littleEndian(value, 8);
}

/// A binary Dynamic value of type Array(Dynamic) holding `elements` (binary Dynamic values).
String arrayOfDynamic(const std::vector<String> & elements)
{
    String out = String(1, ARRAY) + dynamicHeader() + varUInt(elements.size());
    for (const auto & element : elements)
        out += element;
    return out;
}

/// A chain of `depth` nested binary Dynamic values: Array(Dynamic) values holding one element each, the innermost an Int64.
String nestedChain(size_t depth)
{
    String value = int64Value(1);
    for (size_t i = 1; i < depth; ++i)
        value = arrayOfDynamic({value});
    return value;
}

FormatSettings settingsWith(BinaryTypeOrigin origin, size_t max_complexity = 0)
{
    FormatSettings settings;
    settings.binary.type_origin = origin;
    settings.binary.max_binary_type_complexity = max_complexity;
    return settings;
}

String accept(const String & value, const FormatSettings & settings)
{
    String storage;
    return String(acceptBinaryDynamicValue(value, settings, storage));
}

template <typename F>
std::pair<int, String> errorOf(F && f)
{
    try
    {
        f();
    }
    catch (const Exception & e)
    {
        return {e.code(), e.message()};
    }
    return {0, "no exception"};
}

constexpr BinaryTypeOrigin ALL_ORIGINS[] = {BinaryTypeOrigin::Unknown, BinaryTypeOrigin::SentioLegacyV0, BinaryTypeOrigin::OfficialCanonical};

}

TEST(SentioWideRecode, LegacyValueIsNormalizedAndUnknownRefusesIt)
{
    const String payload = littleEndian(1, 64);
    const String legacy = legacyDecimal512Header(76, 10) + payload;
    const String canonical = canonicalDecimal512Header(76, 10) + payload;

    EXPECT_EQ(hexOf(accept(legacy, settingsWith(BinaryTypeOrigin::SentioLegacyV0))), hexOf(canonical));

    auto [code, message] = errorOf([&] { accept(legacy, settingsWith(BinaryTypeOrigin::Unknown)); });
    EXPECT_EQ(code, ErrorCodes::INCORRECT_DATA) << message;
    EXPECT_NE(message.find("ambiguous"), String::npos) << message;

    /// The canonical value is accepted as it is with every origin.
    for (auto origin : ALL_ORIGINS)
        EXPECT_EQ(hexOf(accept(canonical, settingsWith(origin))), hexOf(canonical)) << UInt32(origin);
}

TEST(SentioWideRecode, BridgeWriterRestoresLegacyIndexes)
{
    const String payload = littleEndian(1, 64);
    const String legacy = legacyDecimal512Header(76, 10) + payload;
    const String canonical = canonicalDecimal512Header(76, 10) + payload;

    String storage;
    EXPECT_EQ(hexOf(recodeBinaryDynamicValueForWrite(canonical, BinaryTypeWriterMode::SentioLegacyBridge, storage)), hexOf(legacy));
    EXPECT_EQ(hexOf(recodeBinaryDynamicValueForWrite(canonical, BinaryTypeWriterMode::SentioWideV1, storage)), hexOf(canonical));

    /// A value without a 512-bit type is written as it is.
    const String int64 = int64Value(5);
    EXPECT_EQ(hexOf(recodeBinaryDynamicValueForWrite(int64, BinaryTypeWriterMode::SentioLegacyBridge, storage)), hexOf(int64));
}

TEST(SentioWideRecode, NestedValuesAreNormalizedOrRefused)
{
    const String payload = littleEndian(1, 64);
    const String legacy = arrayOfDynamic({legacyDecimal512Header(76, 10) + payload, int64Value(5)});
    const String canonical = arrayOfDynamic({canonicalDecimal512Header(76, 10) + payload, int64Value(5)});

    EXPECT_EQ(hexOf(accept(legacy, settingsWith(BinaryTypeOrigin::SentioLegacyV0))), hexOf(canonical));
    EXPECT_EQ(errorOf([&] { accept(legacy, settingsWith(BinaryTypeOrigin::Unknown)); }).first, ErrorCodes::INCORRECT_DATA);
    EXPECT_EQ(hexOf(accept(canonical, settingsWith(BinaryTypeOrigin::Unknown))), hexOf(canonical));

    String storage;
    EXPECT_EQ(hexOf(recodeBinaryDynamicValueForWrite(canonical, BinaryTypeWriterMode::SentioLegacyBridge, storage)), hexOf(legacy));
}

TEST(SentioWideRecode, NothingHasNoPayload)
{
    for (auto origin : ALL_ORIGINS)
    {
        EXPECT_EQ(hexOf(accept(String(1, NOTHING), settingsWith(origin))), "00");
        auto [code, message] = errorOf([&] { accept(String{NOTHING, 0x01}, settingsWith(origin)); });
        EXPECT_EQ(code, ErrorCodes::INCORRECT_DATA) << message;
    }
}

TEST(SentioWideRecode, OrdinaryStringContentIsOpaque)
{
    /// A String whose content looks like a legacy header and a descriptor is never parsed as type metadata.
    const String content = String("SentioWide_v1_Decimal512(76,10)") + legacyDecimal512Header(76, 10);
    const String value = String(1, STRING) + varUInt(content.size()) + content;
    for (auto origin : ALL_ORIGINS)
        EXPECT_EQ(hexOf(accept(value, settingsWith(origin))), hexOf(value)) << UInt32(origin);

    String storage;
    EXPECT_TRUE(binaryDynamicValueMayHaveSentioWideType(value));
    EXPECT_EQ(hexOf(recodeBinaryDynamicValueForWrite(value, BinaryTypeWriterMode::SentioLegacyBridge, storage)), hexOf(value));
}

TEST(SentioWideRecode, NestingDepthIsBoundedAtTheLimit)
{
    constexpr size_t limit = 5;
    EXPECT_NO_THROW(accept(nestedChain(limit), settingsWith(BinaryTypeOrigin::Unknown, limit)));
    auto [code, message] = errorOf([&] { accept(nestedChain(limit + 1), settingsWith(BinaryTypeOrigin::Unknown, limit)); });
    EXPECT_EQ(code, ErrorCodes::INCORRECT_DATA) << message;
    EXPECT_NE(message.find("nested too deeply"), String::npos) << message;

    /// Without a limit (stored data, aggregation states) only the stack bounds the depth.
    EXPECT_NO_THROW(accept(nestedChain(50), settingsWith(BinaryTypeOrigin::Unknown, 0)));
    EXPECT_NO_THROW(accept(nestedChain(50), getFormatSettingsForStoredBinaryTypeOrigin(BinaryTypeOrigin::Unknown)));
}

TEST(SentioWideRecode, SiblingsAndRowsAreNotAddedUp)
{
    /// 2000 ordinary siblings with the limit 1000: every header is one node and the depth is 2.
    std::vector<String> elements;
    for (size_t i = 0; i < 2000; ++i)
        elements.push_back(int64Value(i));
    EXPECT_NO_THROW(accept(arrayOfDynamic(elements), settingsWith(BinaryTypeOrigin::Unknown, 1000)));

    /// Every row of a batch is a value of its own: two rows at the depth limit are both accepted.
    constexpr size_t limit = 4;
    auto rows = ColumnString::create();
    for (size_t i = 0; i < 2; ++i)
    {
        const String row = nestedChain(limit);
        rows->insertData(row.data(), row.size());
    }
    auto accepted = ColumnString::create();
    EXPECT_NO_THROW(acceptBinaryDynamicValues(*rows, 0, settingsWith(BinaryTypeOrigin::Unknown, limit), *accepted));
    EXPECT_EQ(accepted->size(), 2U);
}

TEST(SentioWideRecode, EveryHeaderKeepsItsOwnComplexityLimit)
{
    /// Tuple(998 Int64, Dynamic): 1000 nodes, at the limit; the nested Dynamic value holds one Int64, and the sizes of the headers are not
    /// added up.
    String header = String(1, UNNAMED_TUPLE) + varUInt(999);
    String payload;
    for (size_t i = 0; i < 998; ++i)
    {
        header += INT64;
        payload += littleEndian(i, 8);
    }
    header += dynamicHeader();
    payload += int64Value(7);
    EXPECT_NO_THROW(accept(header + payload, settingsWith(BinaryTypeOrigin::Unknown, 1000)));

    /// Tuple(1000 Int64): 1001 nodes, above the limit.
    String too_complex = String(1, UNNAMED_TUPLE) + varUInt(1000);
    String too_complex_payload;
    for (size_t i = 0; i < 1000; ++i)
    {
        too_complex += INT64;
        too_complex_payload += littleEndian(i, 8);
    }
    auto [code, message] = errorOf([&] { accept(too_complex + too_complex_payload, settingsWith(BinaryTypeOrigin::Unknown, 1000)); });
    EXPECT_EQ(code, ErrorCodes::INCORRECT_DATA) << message;
    EXPECT_NE(message.find("complexity"), String::npos) << message;
}

TEST(SentioWideRecode, NestingScopeIsRestoredAfterAnException)
{
    constexpr size_t limit = 3;
    BinaryTypeDecodingContext context(limit);
    FormatSettings settings = settingsWith(BinaryTypeOrigin::Unknown, limit);
    settings.binary.type_decoding_context = &context;

    EXPECT_EQ(errorOf([&] { accept(nestedChain(limit + 1), settings); }).first, ErrorCodes::INCORRECT_DATA);
    EXPECT_EQ(context.depth, 0U);
    EXPECT_NO_THROW(accept(nestedChain(limit), settings));
    EXPECT_EQ(context.depth, 0U);

    /// A refused legacy header deep inside a value restores the scope as well.
    const String legacy = legacyDecimal512Header(76, 10) + littleEndian(1, 64);
    EXPECT_EQ(errorOf([&] { accept(arrayOfDynamic({legacy}), settings); }).first, ErrorCodes::INCORRECT_DATA);
    EXPECT_EQ(context.depth, 0U);
}

TEST(SentioWideRecode, NestingRefusalPreservesItsTypeAcrossCloneAndRethrow)
{
    try
    {
        accept(nestedChain(6), settingsWith(BinaryTypeOrigin::Unknown, 5));
        FAIL() << "The value-nesting budget must reject the sixth level";
    }
    catch (const BinaryValueNestingLimitException & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA);
        Exception plain(ErrorCodes::INCORRECT_DATA, "{}", e.message());
        EXPECT_STREQ(e.name(), plain.name());
        EXPECT_STREQ(static_cast<const Poco::Exception &>(e).className(), static_cast<const Poco::Exception &>(plain).className());
        std::unique_ptr<Exception> copy(e.clone());
        ASSERT_NE(dynamic_cast<BinaryValueNestingLimitException *>(copy.get()), nullptr);
        EXPECT_EQ(copy->code(), e.code());
        EXPECT_EQ(copy->message(), e.message());
        EXPECT_THROW(copy->rethrow(), BinaryValueNestingLimitException);
    }
    catch (const Exception & e)
    {
        FAIL() << "A nesting refusal lost its concrete boundary exception: " << e.message();
    }
}

TEST(SentioWideRecode, DescriptorAndOriginErrorsAreNotValueNestingRefusals)
{
    const String array_header = String(1, ARRAY) + INT64 + varUInt(0);
    const String legacy = legacyDecimal512Header(76, 10) + littleEndian(1, 64);
    for (const auto & value : {array_header, legacy})
    {
        try
        {
            accept(value, settingsWith(BinaryTypeOrigin::Unknown, 1));
            FAIL() << "The descriptor or origin must be rejected";
        }
        catch (const BinaryValueNestingLimitException &)
        {
            FAIL() << "RowBinary must not turn a type-header/origin refusal into a value-nesting error";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA);
        }
    }
}
