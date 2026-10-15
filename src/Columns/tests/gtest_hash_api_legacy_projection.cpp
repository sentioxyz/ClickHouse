#include <gtest/gtest.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnAggregateFunction.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnDynamic.h>
#include <Columns/ColumnObject.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnVariant.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Field.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDynamic.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromString.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/SipHash.h>
#include <Common/assert_cast.h>
#include <Common/tests/gtest_global_register.h>

#include <functional>
#include <vector>

/// The hash APIs that hash the bytes of binary `Dynamic` values hash them as the Decimal512 fork had them in memory, with the legacy indexes
/// 0x37, 0x38 and 0x39 of its 512-bit types: the range hash (the IDs of the blocks of insert deduplication), the weak hash and the fast
/// hash of `Dynamic` (its shared variant, also nested in an array and in the values of the shared variant), of `JSON` (its shared data) and
/// of aggregation states. The oracle is the same API over the bytes that the fork had: the legacy values are written here from the binary
/// type encoding specification (the index, then for Decimal512 the precision and the scale as UInt8, then the little-endian value), not by
/// the code under test, and are hashed by the plain columns (`Variant`, `Array(Tuple(String, String))`) that hash them as they are. The
/// canonical values are inserted through the columns. A control hashes the canonical bytes as they are, which differs, so the projection is
/// what makes the hashes equal. An ordinary `String` whose bytes look like a SentioWide descriptor is content, not a type, and stays as it
/// is. States without binary type headers hash as before. A SimpleAggregateFunction type, which is created from names, refuses a type that
/// its name does not identify.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int INCORRECT_DATA;
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

/// -`value` in two's complement, `width` bytes, little-endian.
String negativeLittleEndian(UInt64 value, size_t width)
{
    String out(width, '\xff');
    const UInt64 twos = ~value + 1;
    for (size_t i = 0; i < 8 && i < width; ++i)
        out[i] = static_cast<char>((twos >> (8 * i)) & 0xFF);
    return out;
}

/// The legacy binary `Dynamic` values of the fork.
String legacyUInt512(UInt64 value) { return String(1, '\x37') + littleEndian(value, 64); }
String legacyInt512Negative(UInt64 value) { return String(1, '\x38') + negativeLittleEndian(value, 64); }
String legacyDecimal512(UInt8 precision, UInt8 scale, UInt64 raw)
{
    return String{'\x39', static_cast<char>(precision), static_cast<char>(scale)} + littleEndian(raw, 64);
}
String int64Value(UInt64 value) { return String(1, '\x0A') + littleEndian(value, 8); }
String stringValue(const String & content) { return String(1, '\x15') + varUInt(content.size()) + content; }

/// Content of an ordinary String that looks like the SentioWide v1 header of a native Decimal512(50, 2).
const String HEADER_LIKE_CONTENT = String(1, '\x2C') + varUInt(30) + "SentioWide_v1_Decimal512(50,2)";

DataTypePtr nativeDecimal512(UInt32 precision, UInt32 scale)
{
    return std::make_shared<DataTypeDecimal<Decimal512>>(precision, scale);
}

/// Inserts row 0 of `source`, a column of `type`, into the shared variant of `column`, as the column itself encodes it.
void insertShared(ColumnDynamic & column, const DataTypePtr & type, const IColumn & source)
{
    column.insertValueIntoSharedVariant(source, type, type->getName(), 0);
}

template <typename Column, typename Value>
MutableColumnPtr oneValue(const DataTypePtr & type, Value value)
{
    auto column = type->createColumn();
    assert_cast<Column &>(*column).getData().push_back(value);
    return column;
}

/// Appends raw bytes as a value of the shared variant of `column`, as the fork had them in memory.
void appendLegacyShared(ColumnDynamic & column, const String & bytes)
{
    auto & variant = column.getVariantColumn();
    auto & shared = column.getSharedVariant();
    variant.getLocalDiscriminators().push_back(variant.localDiscriminatorByGlobal(column.getSharedVariantDiscriminator()));
    variant.getOffsets().push_back(shared.size());
    shared.insertData(bytes.data(), bytes.size());
}

UInt64 rangeHash(const IColumn & column, size_t begin, size_t end)
{
    SipHash hash;
    column.updateHashWithValueRange(begin, end, hash);
    return hash.get64();
}

UInt64 fastHash(const IColumn & column)
{
    SipHash hash;
    column.updateHashFast(hash);
    return hash.get64();
}

/// The weak hash of the rows [begin, end), from the initial value or combined into nonuniform seeds.
std::vector<UInt32> weakHash(const IColumn & column, size_t begin, size_t end, bool initial)
{
    std::vector<UInt32> hashes(end - begin);
    for (size_t i = 0; i < hashes.size(); ++i)
        hashes[i] = static_cast<UInt32>(0x9E3779B9u * (i + 1) + 17 * i);
    column.computeHashInto(begin, end, hashes.data(), initial);
    return hashes;
}

/// The values of the fixture: canonical (inserted through the column) and legacy (written from the specification), NULL included, and
/// last an ordinary String whose bytes look like a SentioWide header.
struct DynamicFixture
{
    MutableColumnPtr canonical;
    MutableColumnPtr legacy;
};

DynamicFixture makeDynamicFixture(bool with_header_like_string = true)
{
    auto canonical_column = ColumnDynamic::create(0);
    auto & canonical = assert_cast<ColumnDynamic &>(*canonical_column);
    auto legacy_column = canonical.cloneEmpty();
    auto & legacy = assert_cast<ColumnDynamic &>(*legacy_column);

    const auto uint512 = std::make_shared<DataTypeUInt512>();
    insertShared(canonical, uint512, *oneValue<ColumnUInt512>(uint512, UInt512(3)));
    appendLegacyShared(legacy, legacyUInt512(3));

    canonical.insertDefault();
    legacy.insertDefault();

    const auto int512 = std::make_shared<DataTypeInt512>();
    insertShared(canonical, int512, *oneValue<ColumnInt512>(int512, Int512(-2)));
    appendLegacyShared(legacy, legacyInt512Negative(2));

    const auto native = nativeDecimal512(50, 2);
    insertShared(canonical, native, *oneValue<ColumnDecimal<Decimal512>>(native, Decimal512(12345)));
    appendLegacyShared(legacy, legacyDecimal512(50, 2, 12345));

    const auto wide = nativeDecimal512(80, 2);
    insertShared(canonical, wide, *oneValue<ColumnDecimal<Decimal512>>(wide, Decimal512(777)));
    appendLegacyShared(legacy, legacyDecimal512(80, 2, 777));

    const auto int64 = std::make_shared<DataTypeInt64>();
    insertShared(canonical, int64, *oneValue<ColumnInt64>(int64, Int64(7)));
    appendLegacyShared(legacy, int64Value(7));

    if (!with_header_like_string)
        return {std::move(canonical_column), std::move(legacy_column)};

    const auto string = std::make_shared<DataTypeString>();
    auto string_column = string->createColumn();
    string_column->insertData(HEADER_LIKE_CONTENT.data(), HEADER_LIKE_CONTENT.size());
    insertShared(canonical, string, *string_column);
    appendLegacyShared(legacy, stringValue(HEADER_LIKE_CONTENT));

    return {std::move(canonical_column), std::move(legacy_column)};
}

}

/// The shared variant of `Dynamic`: the range hash over the full column, a range with a nonzero begin and an empty range, the weak hash in
/// both modes and the fast hash equal those of the legacy bytes, hashed as the plain `Variant` column hashes them.
TEST(HashApiLegacyProjection, DynamicSharedVariant)
{
    auto fixture = makeDynamicFixture();
    const auto & canonical = assert_cast<const ColumnDynamic &>(*fixture.canonical);
    const auto & legacy_variant = assert_cast<const ColumnDynamic &>(*fixture.legacy).getVariantColumn();
    ASSERT_EQ(canonical.size(), 7u);
    ASSERT_EQ(legacy_variant.size(), 7u);

    /// The values of ordinary types are the same bytes in both; the 512-bit ones differ.
    const auto & canonical_shared = canonical.getSharedVariant();
    const auto & legacy_shared = assert_cast<const ColumnDynamic &>(*fixture.legacy).getSharedVariant();
    ASSERT_EQ(canonical_shared.size(), 6u);
    EXPECT_NE(canonical_shared.getDataAt(0), legacy_shared.getDataAt(0));
    EXPECT_EQ(canonical_shared.getDataAt(4), legacy_shared.getDataAt(4));
    EXPECT_EQ(canonical_shared.getDataAt(5), legacy_shared.getDataAt(5));

    for (const auto & [begin, end] : std::vector<std::pair<size_t, size_t>>{{0, 7}, {2, 5}, {3, 3}, {1, 7}})
    {
        EXPECT_EQ(rangeHash(canonical, begin, end), rangeHash(legacy_variant, begin, end)) << begin << ", " << end;
        EXPECT_EQ(weakHash(canonical, begin, end, true), weakHash(legacy_variant, begin, end, true)) << begin << ", " << end;
        EXPECT_EQ(weakHash(canonical, begin, end, false), weakHash(legacy_variant, begin, end, false)) << begin << ", " << end;
    }
    EXPECT_EQ(fastHash(canonical), fastHash(legacy_variant));

    /// The canonical bytes hashed as they are differ: the projection is what makes the hashes equal.
    EXPECT_NE(rangeHash(canonical.getVariantColumn(), 0, 7), rangeHash(legacy_variant, 0, 7));
    EXPECT_NE(fastHash(canonical.getVariantColumn()), fastHash(legacy_variant));
}

/// `Dynamic` nested in an array and in a tuple, and a value of the shared variant whose payload holds `Dynamic` values (`Array(Dynamic)`),
/// whose nested headers are written with the legacy indexes too. (`JSON` values inside `Dynamic` are left to the fixtures of the frozen
/// builds in the final validation.)
TEST(HashApiLegacyProjection, DynamicNested)
{
    /// Array(Dynamic) over the fixture without its String: the legacy column then holds no bytes of the SentioWide namespace, so that it
    /// hashes its values as they are.
    auto fixture = makeDynamicFixture(/*with_header_like_string=*/ false);
    ASSERT_EQ(fixture.canonical->size(), 6u);
    auto offsets = ColumnArray::ColumnOffsets::create();
    offsets->getData().push_back(2);
    offsets->getData().push_back(6);
    auto canonical_array = ColumnArray::create(std::move(fixture.canonical), offsets->clone());
    auto legacy_array = ColumnArray::create(std::move(fixture.legacy), std::move(offsets));
    EXPECT_EQ(rangeHash(*canonical_array, 0, 2), rangeHash(*legacy_array, 0, 2));
    EXPECT_EQ(rangeHash(*canonical_array, 1, 2), rangeHash(*legacy_array, 1, 2));
    EXPECT_EQ(weakHash(*canonical_array, 0, 2, true), weakHash(*legacy_array, 0, 2, true));
    EXPECT_EQ(weakHash(*canonical_array, 0, 2, false), weakHash(*legacy_array, 0, 2, false));
    EXPECT_EQ(fastHash(*canonical_array), fastHash(*legacy_array));

    /// Tuple(Dynamic, Int64) over the same values.
    auto tuple_fixture = makeDynamicFixture(/*with_header_like_string=*/ false);
    auto numbers = ColumnInt64::create();
    for (Int64 i = 0; i != 6; ++i)
        numbers->getData().push_back(i * 3);
    MutableColumns canonical_tuple_columns;
    canonical_tuple_columns.push_back(std::move(tuple_fixture.canonical));
    canonical_tuple_columns.push_back(numbers->clone());
    MutableColumns legacy_tuple_columns;
    legacy_tuple_columns.push_back(std::move(tuple_fixture.legacy));
    legacy_tuple_columns.push_back(std::move(numbers));
    auto canonical_tuple = ColumnTuple::create(std::move(canonical_tuple_columns));
    auto legacy_tuple = ColumnTuple::create(std::move(legacy_tuple_columns));
    for (const auto & [begin, end] : std::vector<std::pair<size_t, size_t>>{{0, 6}, {2, 5}})
    {
        EXPECT_EQ(rangeHash(*canonical_tuple, begin, end), rangeHash(*legacy_tuple, begin, end)) << begin << ", " << end;
        EXPECT_EQ(weakHash(*canonical_tuple, begin, end, true), weakHash(*legacy_tuple, begin, end, true)) << begin << ", " << end;
        EXPECT_EQ(weakHash(*canonical_tuple, begin, end, false), weakHash(*legacy_tuple, begin, end, false)) << begin << ", " << end;
    }
    EXPECT_EQ(fastHash(*canonical_tuple), fastHash(*legacy_tuple));

    /// A value of type Array(Dynamic) holding the native Decimal512(50, 2) 123.45, in the shared variant.
    auto inner = ColumnDynamic::create(0);
    const auto native = nativeDecimal512(50, 2);
    insertShared(assert_cast<ColumnDynamic &>(*inner), native, *oneValue<ColumnDecimal<Decimal512>>(native, Decimal512(12345)));
    auto inner_offsets = ColumnArray::ColumnOffsets::create();
    inner_offsets->getData().push_back(1);
    auto array_value = ColumnArray::create(std::move(inner), std::move(inner_offsets));
    const auto dynamic_type = std::make_shared<DataTypeDynamic>();
    const auto array_type = std::make_shared<DataTypeArray>(dynamic_type);

    auto canonical_column = ColumnDynamic::create(0);
    insertShared(assert_cast<ColumnDynamic &>(*canonical_column), array_type, *array_value);
    auto legacy_column = canonical_column->cloneEmpty();
    /// Array (0x1E) of Dynamic (0x2B, max_types 32), one element: the legacy value of the native Decimal512(50, 2).
    appendLegacyShared(
        assert_cast<ColumnDynamic &>(*legacy_column), String{'\x1E', '\x2B', '\x20'} + varUInt(1) + legacyDecimal512(50, 2, 12345));
    const auto & legacy_variant = assert_cast<const ColumnDynamic &>(*legacy_column).getVariantColumn();
    EXPECT_EQ(rangeHash(*canonical_column, 0, 1), rangeHash(legacy_variant, 0, 1));
    EXPECT_EQ(weakHash(*canonical_column, 0, 1, true), weakHash(legacy_variant, 0, 1, true));
    EXPECT_EQ(fastHash(*canonical_column), fastHash(legacy_variant));
}

/// The shared data of `JSON` (no typed or dynamic paths, so that it is all that the object hashes): the values a = 123.45 (a Decimal512
/// with the precision of its class, 154) and b = 7, hashed as the legacy shared data column hashes them.
TEST(HashApiLegacyProjection, ObjectSharedData)
{
    auto object_column = ColumnObject::create(UnorderedMapWithMemoryTracking<String, MutableColumnPtr>{}, 0, 0);
    Object object;
    object["a"] = Field(DecimalField<Decimal512>(Decimal512(12345), 2));
    object["b"] = Field(Int64(7));
    object_column->insert(Field(object));
    ASSERT_EQ(object_column->size(), 1u);

    auto paths = ColumnString::create();
    auto values = ColumnString::create();
    for (const auto & [path, value] : std::vector<std::pair<String, String>>{{"a", legacyDecimal512(154, 2, 12345)}, {"b", int64Value(7)}})
    {
        paths->insertData(path.data(), path.size());
        values->insertData(value.data(), value.size());
    }
    auto shared_data_offsets = ColumnArray::ColumnOffsets::create();
    shared_data_offsets->getData().push_back(2);
    auto legacy_shared_data = ColumnArray::create(ColumnTuple::create(Columns{std::move(paths), std::move(values)}), std::move(shared_data_offsets));

    EXPECT_EQ(rangeHash(*object_column, 0, 1), rangeHash(*legacy_shared_data, 0, 1));
    EXPECT_EQ(rangeHash(*object_column, 1, 1), rangeHash(*legacy_shared_data, 1, 1));
    EXPECT_EQ(weakHash(*object_column, 0, 1, true), weakHash(*legacy_shared_data, 0, 1, true));
    EXPECT_EQ(fastHash(*object_column), fastHash(*legacy_shared_data));

    /// A combining weak hash combines the finalized hash of the object into the seed (ColumnObject::computeHashInto).
    auto expected = weakHash(*legacy_shared_data, 0, 1, true);
    auto combined = weakHash(*object_column, 0, 1, false);
    const UInt32 seed = 0x9E3779B9u;
    EXPECT_EQ(combined[0], combineWeakHash32(expected[0], seed));
}

namespace
{

AggregateFunctionPtr getFunction(const String & name, const DataTypes & arguments)
{
    tryRegisterAggregateFunctions();
    AggregateFunctionProperties properties;
    return AggregateFunctionFactory::instance().get(name, NullsAction::EMPTY, arguments, {}, properties);
}

/// The arena form of a non-NULL Dynamic value (the NULL flag, the size in 8 bytes, the binary value), and the states that keep one.
String arenaForm(const String & value)
{
    return String(1, '\0') + littleEndian(value.size(), 8) + value;
}

String oneElementState(const String & value)
{
    const String element = arenaForm(value);
    return varUInt(1) + varUInt(element.size()) + element;
}

/// A column of states of `function`, each read from its legacy bytes with the declared origin of the fork, so that memory holds them
/// canonically.
MutableColumnPtr statesReadFromLegacyBytes(const AggregateFunctionPtr & function, const std::vector<String> & legacy_states)
{
    auto column = ColumnAggregateFunction::create(function);
    auto & states = assert_cast<ColumnAggregateFunction &>(*column);
    for (const auto & bytes : legacy_states)
    {
        /// A created, empty state, as deserialize requires.
        states.insertDefault();
        ReadBufferFromString in(bytes);
        auto place = states.getData().back();
        function->deserializeWithBinaryTypeDecoding(
            place, in, std::nullopt, &states.createOrGetArena(), BinaryTypeDecoding{.origin = BinaryTypeOrigin::SentioLegacyV0});
        EXPECT_TRUE(in.eof());
    }
    return column;
}

UInt64 sipHashOf(const String & bytes)
{
    SipHash hash;
    hash.update(bytes.data(), bytes.size());
    return hash.get64();
}

}

/// States that keep `Dynamic` values (`groupArray`, `groupUniqArray`) hash the bytes that the fork wrote for them: the value hash and the
/// weak hash of each state, and the fast hash of all of them one after another. A state without binary type headers hashes as before.
TEST(HashApiLegacyProjection, AggregationStates)
{
    const auto dynamic_type = std::make_shared<DataTypeDynamic>();
    for (const auto * name : {"groupArray", "groupUniqArray"})
    {
        const auto function = getFunction(name, {dynamic_type});
        const std::vector<String> legacy_states{
            oneElementState(legacyDecimal512(76, 10, 1)), oneElementState(legacyUInt512(5))};
        auto column = statesReadFromLegacyBytes(function, legacy_states);

        for (size_t row = 0; row != legacy_states.size(); ++row)
        {
            SipHash hash;
            column->updateHashWithValue(row, hash);
            EXPECT_EQ(hash.get64(), sipHashOf(legacy_states[row])) << name << " " << row;

            UInt32 weak = 0;
            column->computeHashInto(row, row + 1, &weak, true);
            EXPECT_EQ(weak, ::updateWeakHash32(
                reinterpret_cast<const UInt8 *>(legacy_states[row].data()), legacy_states[row].size(), WEAK_HASH32_INITIAL_VALUE)) << name;
        }
        EXPECT_EQ(fastHash(*column), sipHashOf(legacy_states[0] + legacy_states[1])) << name;
    }

    /// sum(UInt64): no binary type headers; its hashes are those of serialize and serializeBatch.
    const auto sum = getFunction("sum", {std::make_shared<DataTypeUInt64>()});
    auto sums = ColumnAggregateFunction::create(sum);
    auto & sum_states = assert_cast<ColumnAggregateFunction &>(*sums);
    auto values = ColumnUInt64::create();
    values->getData().push_back(41);
    const IColumn * arguments[] = {values.get()};
    for (size_t i = 0; i != 2; ++i)
    {
        sum_states.insertDefault();
        sum->add(sum_states.getData().back(), arguments, 0, nullptr);
    }
    WriteBufferFromOwnString serialized;
    sum->serialize(sum_states.getData()[0], serialized);
    SipHash value_hash;
    sums->updateHashWithValue(0, value_hash);
    EXPECT_EQ(value_hash.get64(), sipHashOf(serialized.str()));
    WriteBufferFromOwnString batch;
    sum->serializeBatch(sum_states.getData(), 0, 2, batch);
    EXPECT_EQ(fastHash(*sums), sipHashOf(batch.str()));
}

/// A SimpleAggregateFunction type is created from names: a decoded one whose argument is a native Decimal512(50, 2), and the -SimpleState
/// of a function whose result is one, are refused; their name-created controls are not.
TEST(HashApiLegacyProjection, SimpleAggregateFunctionRefusesUnidentifiedTypes)
{
    tryRegisterAggregateFunctions();
    /// SimpleAggregateFunction (0x2E), function any, no parameters, one argument: the SentioWide v1 header of a native Decimal512(50, 2).
    const String encoded = String(1, '\x2E') + varUInt(3) + "any" + varUInt(0) + varUInt(1) + String(1, '\x2C') + varUInt(30)
        + "SentioWide_v1_Decimal512(50,2)";
    ReadBufferFromString in(encoded);
    try
    {
        decodeDataType(in);
        ADD_FAILURE() << "decoded SimpleAggregateFunction(any, native Decimal512(50, 2))";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA) << e.message();
    }

    try
    {
        getFunction("anySimpleState", {nativeDecimal512(50, 2)});
        ADD_FAILURE() << "made anySimpleState of a native Decimal512(50, 2)";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA) << e.message();
    }

    EXPECT_EQ(getFunction("anySimpleState", {nativeDecimal512(80, 2)})->getResultType()->getName(), "SimpleAggregateFunction(any, Decimal(80, 2))");
    EXPECT_EQ(
        getFunction("anySimpleState", {std::make_shared<DataTypeDecimal<Decimal256>>(50, 2)})->getResultType()->getName(),
        "SimpleAggregateFunction(any, Decimal(50, 2))");
}
