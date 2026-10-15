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
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDynamic.h>
#include <DataTypes/DataTypeQBit.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromString.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/HashTable/HashSet.h>
#include <Common/MemoryTracker.h>
#include <Common/SipHash.h>
#include <Common/ThreadStatus.h>
#include <Common/assert_cast.h>
#include <Common/scope_guard_safe.h>
#include <Common/tests/gtest_global_register.h>
#include <base/StringViewHash.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <thread>
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
/// its name does not identify. The unordered states (`groupUniqArray`, `-Distinct`, `groupArrayIntersect`) keep the order of the fork's
/// sets: their bridge serializations are those of the fork's sets after the same operations, simulated here with the fork's set types.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int ATTEMPT_TO_READ_AFTER_EOF;
extern const int CANNOT_READ_ALL_DATA;
extern const int INCORRECT_DATA;
extern const int MEMORY_LIMIT_EXCEEDED;
extern const int TOO_LARGE_ARRAY_SIZE;
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
    /// Array(Dynamic) over the fixture without its String. The legacy oracle uses the plain underlying `Variant` at every nested layer:
    /// its raw legacy headers must be hashed as bytes, without asking a canonical `Dynamic` column to interpret them.
    auto fixture = makeDynamicFixture(/*with_header_like_string=*/ false);
    ASSERT_EQ(fixture.canonical->size(), 6u);
    auto offsets = ColumnArray::ColumnOffsets::create();
    offsets->getData().push_back(2);
    offsets->getData().push_back(6);
    auto canonical_array = ColumnArray::create(std::move(fixture.canonical), offsets->clone());
    auto legacy_array = ColumnArray::create(
        assert_cast<const ColumnDynamic &>(*fixture.legacy).getVariantColumn().clone(), std::move(offsets));
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
    legacy_tuple_columns.push_back(assert_cast<const ColumnDynamic &>(*tuple_fixture.legacy).getVariantColumn().clone());
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
/// with the precision of its class, 154) and b = 7 or 2^40, hashed as the legacy shared data column hashes them.
TEST(HashApiLegacyProjection, ObjectSharedData)
{
    /// Keep the original small value and also exercise a value that cannot be narrowed to Int8/16/32.
    for (const Int64 ordinary_value : {Int64(7), Int64(1) << 40})
    {
        auto object_column = ColumnObject::create(UnorderedMapWithMemoryTracking<String, MutableColumnPtr>{}, 0, 0);
        Object object;
        object["a"] = Field(DecimalField<Decimal512>(Decimal512(12345), 2));
        object["b"] = Field(ordinary_value);
        object_column->insert(Field(object));
        ASSERT_EQ(object_column->size(), 1u);

        auto paths = ColumnString::create();
        auto values = ColumnString::create();
        /// FieldToDataType narrows 7 to Int8; 2^40 retains Int64. Both old wire headers are specified independently.
        const String ordinary_legacy = ordinary_value == 7 ? String{"\x07\x07", 2} : int64Value(ordinary_value);
        for (const auto & [path, value] : std::vector<std::pair<String, String>>{{"a", legacyDecimal512(154, 2, 12345)}, {"b", ordinary_legacy}})
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

namespace
{

/// The set of the generic `groupUniqArray` (AggregateFunctionGroupUniqArrayGenericData) with the algorithm of the fork: its keys are the arena
/// forms of the values as the fork had them in memory, hashed by StringViewHash. Insertions, merges (the keys of the other set in its order)
/// and reads (the keys in the order they were written) are those of the function.
using ForkUniqSet = HashSetWithSavedHashWithStackMemory<std::string_view, StringViewHash, 3>;

/// The serialization of the state of the generic `groupUniqArray`: the number of keys, then every key as a string, in the order of the set.
String uniqState(const ForkUniqSet & set)
{
    WriteBufferFromOwnString out;
    writeVarUInt(set.size(), out);
    for (const auto & elem : set)
        writeStringBinary(elem.getValue(), out);
    return out.str();
}

/// The keys of a serialized `groupUniqArray` state, in order.
std::vector<String> uniqStateKeys(const String & state)
{
    ReadBufferFromString in(state);
    size_t size = 0;
    readVarUInt(size, in);
    std::vector<String> keys(size);
    for (auto & key : keys)
        readStringBinary(key, in);
    return keys;
}

/// A column of Dynamic values read from their binary forms, as the column reads them.
MutableColumnPtr dynamicColumn(const std::vector<String> & values)
{
    auto column = ColumnDynamic::create();
    for (const auto & value : values)
    {
        ReadBufferFromString in(value);
        DataTypeDynamic().getDefaultSerialization()->deserializeBinary(*column, in, FormatSettings{});
    }
    return column;
}

/// The same operations on the states of `groupUniqArray(Dynamic)` and on the sets of the fork: A gets the values `first` one by one, B the
/// values `second`, A merges B, and C reads the serialization of A. The canonical values are what the states get, the fork values are what
/// the fork had in memory for the same values. Returns the column with the states A and C and the fork serializations of A and C.
struct UniqStates
{
    MutableColumnPtr column;
    String fork_a;
    String fork_c;
};

UniqStates runUniqStates(
    const std::vector<String> & first_canonical, const std::vector<String> & first_fork,
    const std::vector<String> & second_canonical, const std::vector<String> & second_fork)
{
    const auto function = getFunction("groupUniqArray", {std::make_shared<DataTypeDynamic>()});
    auto column = ColumnAggregateFunction::create(function);
    auto & states = assert_cast<ColumnAggregateFunction &>(*column);
    Arena & arena = states.createOrGetArena();

    auto add_all = [&](AggregateDataPtr place, const std::vector<String> & values)
    {
        auto values_column = dynamicColumn(values);
        const IColumn * arguments[] = {values_column.get()};
        for (size_t row = 0; row != values.size(); ++row)
            function->add(place, arguments, row, &arena);
    };

    states.insertDefault();
    const auto place_a = states.getData().back();
    add_all(place_a, first_canonical);
    auto b = ColumnAggregateFunction::create(function);
    auto & b_states = assert_cast<ColumnAggregateFunction &>(*b);
    b_states.insertDefault();
    add_all(b_states.getData().back(), second_canonical);
    function->merge(place_a, b_states.getData().back(), &arena);

    WriteBufferFromOwnString serialized_a;
    function->serialize(place_a, serialized_a);
    states.insertDefault();
    ReadBufferFromString in(serialized_a.str());
    function->deserialize(states.getData().back(), in, std::nullopt, &arena);

    /// The fork: the arena forms of the fork values, inserted, merged and read in the same order.
    std::vector<String> keys;
    keys.reserve(first_fork.size() + second_fork.size());
    ForkUniqSet set_a;
    for (const auto & value : first_fork)
        set_a.insert(keys.emplace_back(arenaForm(value)));
    ForkUniqSet set_b;
    for (const auto & value : second_fork)
        set_b.insert(keys.emplace_back(arenaForm(value)));
    for (const auto & elem : set_b)
        set_a.insert(elem.getValue());
    const String fork_a = uniqState(set_a);
    const auto fork_a_keys = uniqStateKeys(fork_a);
    ForkUniqSet set_c;
    for (const auto & key : fork_a_keys)
        set_c.insert(key);
    return {std::move(column), fork_a, uniqState(set_c)};
}

void expectStatesHashTheForkBytes(const IColumn & column, const String & fork_a, const String & fork_c, const String & where)
{
    const std::vector<String> fork_states{fork_a, fork_c};
    for (size_t row = 0; row != fork_states.size(); ++row)
    {
        SipHash hash;
        column.updateHashWithValue(row, hash);
        EXPECT_EQ(hash.get64(), sipHashOf(fork_states[row])) << where << " " << row;

        UInt32 weak = 0;
        column.computeHashInto(row, row + 1, &weak, true);
        EXPECT_EQ(weak, ::updateWeakHash32(
            reinterpret_cast<const UInt8 *>(fork_states[row].data()), fork_states[row].size(), WEAK_HASH32_INITIAL_VALUE)) << where << " " << row;
    }
    EXPECT_EQ(fastHash(column), sipHashOf(fork_a + fork_c)) << where;
}

}

/// Several keys without a 512-bit type, one of them an ordinary String whose bytes look like a SentioWide header: the keys are the same
/// bytes in both encodings, so the states are the sets of the fork, and their hashes after adds, a merge and a read are those of the fork.
/// The same state as a value of `Dynamic` (its type is AggregateFunction(groupUniqArray, Dynamic)) keeps its bytes in the hashes of the
/// shared variant, although they hold bytes of the SentioWide namespace.
TEST(HashApiLegacyProjection, UnorderedStatesWithHeaderLikeContent)
{
    const std::vector<String> first{stringValue(HEADER_LIKE_CONTENT), stringValue("a"), int64Value(7), stringValue("bb")};
    const std::vector<String> second{stringValue("ccc"), int64Value(7), stringValue(HEADER_LIKE_CONTENT + "x"), int64Value(9)};
    auto [column, fork_a, fork_c] = runUniqStates(first, first, second, second);
    expectStatesHashTheForkBytes(*column, fork_a, fork_c, "no 512-bit key");

    const auto function = assert_cast<const ColumnAggregateFunction &>(*column).getAggregateFunction();
    const auto state_type = std::make_shared<DataTypeAggregateFunction>(function, DataTypes{std::make_shared<DataTypeDynamic>()}, Array{});
    auto dynamic = ColumnDynamic::create(0);
    insertShared(assert_cast<ColumnDynamic &>(*dynamic), state_type, *column);
    const auto & variant = assert_cast<const ColumnDynamic &>(*dynamic).getVariantColumn();
    EXPECT_EQ(rangeHash(*dynamic, 0, 1), rangeHash(variant, 0, 1));
    EXPECT_EQ(weakHash(*dynamic, 0, 1, true), weakHash(variant, 0, 1, true));
    EXPECT_EQ(fastHash(*dynamic), fastHash(variant));
}

/// The strict historical oracle for keys with 512-bit types: the fork kept their legacy arena forms in its sets, so the order of its sets
/// and of its states follows their hashes, while the states here keep the canonical arena forms. The hashes are compared with those of the
/// fork's sets after the same adds, merge and read. This is the fixture that tells whether the states keep the fork's order; it must not be
/// weakened to accept another one (see STATUS: the decision on the hash domain of unordered states is pending).
TEST(HashApiLegacyProjection, UnorderedStatesWithWideKeysHaveTheForkOrder)
{
    std::vector<String> first_canonical;
    std::vector<String> first_fork;
    for (UInt64 raw = 1; raw <= 6; ++raw)
    {
        const String descriptor = "SentioWide_v1_Decimal512(76,10)";
        first_canonical.push_back(String(1, '\x2C') + varUInt(descriptor.size()) + descriptor + littleEndian(raw, 64));
        first_fork.push_back(legacyDecimal512(76, 10, raw));
    }
    std::vector<String> second_canonical{int64Value(5)};
    std::vector<String> second_fork{int64Value(5)};
    for (UInt64 raw : {3, 7, 8})
    {
        const String descriptor = "SentioWide_v1_UInt512";
        second_canonical.push_back(String(1, '\x2C') + varUInt(descriptor.size()) + descriptor + littleEndian(raw, 64));
        second_fork.push_back(legacyUInt512(raw));
    }
    auto [column, fork_a, fork_c] = runUniqStates(first_canonical, first_fork, second_canonical, second_fork);
    expectStatesHashTheForkBytes(*column, fork_a, fork_c, "512-bit keys");
}

namespace
{

String canonicalUInt512(UInt64 value)
{
    const String descriptor = "SentioWide_v1_UInt512";
    return String(1, '\x2C') + varUInt(descriptor.size()) + descriptor + littleEndian(value, 64);
}

/// The values of the keys of a set, in its order: the index in `values` of the arena form of each key.
std::vector<size_t> orderOf(const ForkUniqSet & set, const std::vector<String> & values)
{
    std::vector<size_t> order;
    for (const auto & elem : set)
        for (size_t i = 0; i != values.size(); ++i)
            if (elem.getValue() == arenaForm(values[i]))
                order.push_back(i);
    return order;
}

}

/// Root's source model of the CRC32C branch of StringViewHash (e6-multikey-crc-order-source-model.root.json): the UInt512 values 1 and 4
/// have legacy arena keys (74 bytes) in the buckets 1 and 3 of 8, and canonical arena keys (96 bytes) in the buckets 3 and 0. Canonical
/// keys placed by their own hashes would be written as 4, 1; the state places them with the hashes of the fork's keys and writes them as the
/// fork does, 1, 4. The bucket expectations are those of the CRC32C branch, which the flags of the final build bind.
TEST(HashApiLegacyProjection, UniqStateCrcWitnessKeepsTheForkOrder)
{
    const std::vector<String> canonical{canonicalUInt512(1), canonicalUInt512(4)};
    const std::vector<String> legacy{legacyUInt512(1), legacyUInt512(4)};
    ASSERT_EQ(arenaForm(legacy[0]).size(), 74u);
    ASSERT_EQ(arenaForm(canonical[0]).size(), 96u);

    std::vector<String> keys;
    keys.reserve(4);
    ForkUniqSet fork_set;
    ForkUniqSet canonical_set;
    for (size_t i = 0; i != 2; ++i)
    {
        fork_set.insert(keys.emplace_back(arenaForm(legacy[i])));
        canonical_set.insert(keys.emplace_back(arenaForm(canonical[i])));
    }
    EXPECT_EQ(orderOf(fork_set, legacy), (std::vector<size_t>{0, 1}));
    EXPECT_EQ(orderOf(canonical_set, canonical), (std::vector<size_t>{1, 0}));

    auto [column, fork_a, fork_c] = runUniqStates(canonical, legacy, {}, {});
    expectStatesHashTheForkBytes(*column, fork_a, fork_c, "UInt512 1 and 4");
}

namespace
{

/// A `Dynamic` value of UInt8, and the canonical `Dynamic` value of a native Decimal512(P, S).
String uint8Value(UInt8 value)
{
    return String{'\x01', static_cast<char>(value)};
}

String canonicalDecimal512(UInt8 precision, UInt8 scale, UInt64 raw)
{
    const String descriptor = "SentioWide_v1_Decimal512(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
    return String(1, '\x2C') + varUInt(descriptor.size()) + descriptor + littleEndian(raw, 64);
}

/// A state as it is stored (canonical), and as the explicit bridge export writes it: the bytes that the fork wrote for the same state.
String storedState(const IAggregateFunction & function, ConstAggregateDataPtr place)
{
    WriteBufferFromOwnString out;
    function.serialize(place, out);
    return out.str();
}

String bridgeState(const IAggregateFunction & function, ConstAggregateDataPtr place)
{
    WriteBufferFromOwnString out;
    function.serializeWithBinaryTypeWriterMode(place, out, std::nullopt, BinaryTypeWriterMode::SentioLegacyBridge);
    return out.str();
}

/// An Array(Dynamic) column whose rows hold `rows`, read from their binary forms.
MutableColumnPtr dynamicArrayRows(const std::vector<std::vector<String>> & rows)
{
    std::vector<String> values;
    auto offsets = ColumnArray::ColumnOffsets::create();
    for (const auto & row : rows)
    {
        values.insert(values.end(), row.begin(), row.end());
        offsets->getData().push_back(values.size());
    }
    return ColumnArray::create(dynamicColumn(values), std::move(offsets));
}

/// The binary forms of the values of the first row of an Array(Dynamic) column, in order.
std::vector<String> firstRowValues(const IColumn & column)
{
    const auto & array = assert_cast<const ColumnArray &>(column);
    const DataTypeDynamic type;
    const auto serialization = type.getDefaultSerialization();
    std::vector<String> values;
    for (size_t i = 0; i != array.getOffsets()[0]; ++i)
    {
        WriteBufferFromOwnString out;
        serialization->serializeBinary(array.getData(), i, out, FormatSettings{});
        values.push_back(out.str());
    }
    return values;
}

/// The arena forms of `Dynamic` values (see arenaForm).
std::vector<String> arenaForms(const std::vector<String> & values)
{
    std::vector<String> keys;
    keys.reserve(values.size());
    for (const auto & value : values)
        keys.push_back(arenaForm(value));
    return keys;
}

/// The indices in `keys` of the keys of a set, in the order of the set.
template <typename Set>
std::vector<size_t> keyOrder(const Set & set, const std::vector<String> & keys)
{
    std::vector<size_t> order;
    for (const auto & elem : set)
        for (size_t i = 0; i != keys.size(); ++i)
            if (elem.getValue() == keys[i])
                order.push_back(i);
    return order;
}

/// The history of the generic `-Distinct` of the fork (AggregateFunctionDistinctGenericData): the arena forms of the values as the fork had
/// them in memory, hashed by StringViewHash. Its serialization is the number of keys, then every key as a string, in the order of the set.
using ForkDistinctSet = HashSetWithSavedHashWithStackMemory<std::string_view, StringViewHash, 4>;

String historyState(const ForkDistinctSet & set)
{
    WriteBufferFromOwnString out;
    writeVarUInt(set.size(), out);
    for (const auto & elem : set)
        writeStringBinary(elem.getValue(), out);
    return out.str();
}

}

/// `groupArrayDistinct(Dynamic)` with 512-bit values (UInt512 1 to 12, the CRC witness 1 and 4 among them), an Int64 and an ordinary String
/// whose bytes look like a SentioWide header: A adds `first` one by one, B adds `second` in one batch with a filter (the path of the -If
/// combinator), E adds all of `second` in one batch without one, A merges B, and C reads A. The histories are the fork's: their bridge
/// serializations are those of the fork's histories after the same operations. A's result has the values of B that are new to A in the
/// order of B's history, in which the fork's merge fed them to the nested function.
TEST(HashApiLegacyProjection, DistinctStatesWithWideKeysHaveTheForkOrder)
{
    std::vector<String> first_canonical{int64Value(5), stringValue(HEADER_LIKE_CONTENT)};
    std::vector<String> first_fork = first_canonical;
    for (UInt64 raw = 1; raw <= 6; ++raw)
    {
        first_canonical.push_back(canonicalDecimal512(76, 10, raw));
        first_fork.push_back(legacyDecimal512(76, 10, raw));
    }
    /// UInt512 1 to 12, then Int64 5 (A has it) and 6. The filter drops UInt512 7.
    std::vector<String> second_canonical;
    std::vector<String> second_fork;
    for (UInt64 value = 1; value <= 12; ++value)
    {
        second_canonical.push_back(canonicalUInt512(value));
        second_fork.push_back(legacyUInt512(value));
    }
    for (UInt64 value : {5, 6})
    {
        second_canonical.push_back(int64Value(value));
        second_fork.push_back(int64Value(value));
    }
    auto flags = ColumnUInt8::create();
    for (size_t i = 0; i != second_canonical.size(); ++i)
        flags->getData().push_back(i != 6);

    /// The fork: the same operations on the arena forms of the fork values.
    const auto first_keys = arenaForms(first_fork);
    const auto second_keys = arenaForms(second_fork);
    const auto second_canonical_keys = arenaForms(second_canonical);
    ForkDistinctSet fork_a;
    for (const auto & key : first_keys)
        fork_a.insert(key);
    const String fork_a_added = historyState(fork_a);
    ForkDistinctSet fork_b;
    ForkDistinctSet fork_e;
    /// B with the canonical keys placed by their own hashes, which the fixture must tell from the fork's B.
    ForkDistinctSet canonical_b;
    for (size_t i = 0; i != second_keys.size(); ++i)
    {
        fork_e.insert(second_keys[i]);
        if (flags->getData()[i])
        {
            fork_b.insert(second_keys[i]);
            canonical_b.insert(second_canonical_keys[i]);
        }
    }
    ASSERT_NE(keyOrder(canonical_b, second_canonical_keys), keyOrder(fork_b, second_keys));
    std::vector<String> expected_result = first_canonical;
    for (const auto & elem : fork_b)
        if (fork_a.insert(elem.getValue()).second)
            for (size_t i = 0; i != second_keys.size(); ++i)
                if (elem.getValue() == second_keys[i])
                    expected_result.push_back(second_canonical[i]);
    const String fork_a_merged = historyState(fork_a);
    const auto fork_a_written = uniqStateKeys(fork_a_merged);
    ForkDistinctSet fork_c;
    for (const auto & key : fork_a_written)
        fork_c.insert(key);

    const auto function = getFunction("groupArrayDistinct", {std::make_shared<DataTypeDynamic>()});
    auto column = ColumnAggregateFunction::create(function);
    auto & states = assert_cast<ColumnAggregateFunction &>(*column);
    Arena & arena = states.createOrGetArena();
    auto first_values = dynamicColumn(first_canonical);
    auto second_values = dynamicColumn(second_canonical);

    states.insertDefault();
    const auto place_a = states.getData().back();
    const IColumn * first_arguments[] = {first_values.get()};
    for (size_t row = 0; row != first_canonical.size(); ++row)
        function->add(place_a, first_arguments, row, &arena);
    EXPECT_EQ(bridgeState(*function, place_a), fork_a_added);

    states.insertDefault();
    const auto place_b = states.getData().back();
    const IColumn * filtered_arguments[] = {second_values.get(), flags.get()};
    function->addBatchSinglePlace(0, second_canonical.size(), place_b, filtered_arguments, &arena, 1);
    EXPECT_EQ(bridgeState(*function, place_b), historyState(fork_b));

    states.insertDefault();
    const auto place_e = states.getData().back();
    const IColumn * second_arguments[] = {second_values.get()};
    function->addBatchSinglePlace(0, second_canonical.size(), place_e, second_arguments, &arena);
    EXPECT_EQ(bridgeState(*function, place_e), historyState(fork_e));

    function->merge(place_a, place_b, &arena);
    EXPECT_EQ(bridgeState(*function, place_a), fork_a_merged);
    auto result = function->getResultType()->createColumn();
    function->insertResultInto(place_a, *result, &arena);
    EXPECT_EQ(firstRowValues(*result), expected_result);

    states.insertDefault();
    const auto place_c = states.getData().back();
    const String stored_a = storedState(*function, place_a);
    ReadBufferFromString in(stored_a);
    function->deserialize(place_c, in, std::nullopt, &arena);
    EXPECT_TRUE(in.eof());
    EXPECT_EQ(bridgeState(*function, place_c), historyState(fork_c));
}

/// `uniqExactDistinct(Dynamic, UInt8)`: the keys of its history are the arena forms of both arguments, one after another. A adds pairs, B
/// adds pairs (A has some of them), A merges B (it looks the keys of B up with their saved hashes), and C reads A. The histories are the
/// fork's, and A counts the pairs of both.
TEST(HashApiLegacyProjection, DistinctOfSeveralArgumentsWithWideKeysHasTheForkOrder)
{
    struct Pairs
    {
        std::vector<String> canonical;
        std::vector<String> fork;
        std::vector<UInt8> bytes;

        void add(String canonical_value, String fork_value, UInt8 byte)
        {
            canonical.push_back(std::move(canonical_value));
            fork.push_back(std::move(fork_value));
            bytes.push_back(byte);
        }

        std::vector<String> keys(const std::vector<String> & values) const
        {
            std::vector<String> result;
            for (size_t i = 0; i != values.size(); ++i)
                result.push_back(arenaForm(values[i]) + String(1, static_cast<char>(bytes[i])));
            return result;
        }
    };
    Pairs first;
    for (UInt64 value = 1; value <= 10; ++value)
        first.add(canonicalUInt512(value), legacyUInt512(value), static_cast<UInt8>(value % 3));
    first.add(int64Value(5), int64Value(5), 1);
    /// UInt512 4 to 13 with the bytes of A (A has the pairs of 4 to 10), and UInt512 1 with another byte than in A.
    Pairs second;
    for (UInt64 value = 4; value <= 13; ++value)
        second.add(canonicalUInt512(value), legacyUInt512(value), static_cast<UInt8>(value % 3));
    second.add(canonicalUInt512(1), legacyUInt512(1), 2);

    const auto first_keys = first.keys(first.fork);
    const auto first_canonical_keys = first.keys(first.canonical);
    const auto second_keys = second.keys(second.fork);
    ForkDistinctSet fork_a;
    ForkDistinctSet canonical_a;
    for (size_t i = 0; i != first_keys.size(); ++i)
    {
        fork_a.insert(first_keys[i]);
        canonical_a.insert(first_canonical_keys[i]);
    }
    ASSERT_NE(keyOrder(canonical_a, first_canonical_keys), keyOrder(fork_a, first_keys));
    const String fork_a_added = historyState(fork_a);
    ForkDistinctSet fork_b;
    for (const auto & key : second_keys)
        fork_b.insert(key);
    for (const auto & elem : fork_b)
        fork_a.insert(elem.getValue());
    const String fork_a_merged = historyState(fork_a);
    const auto fork_a_written = uniqStateKeys(fork_a_merged);
    ForkDistinctSet fork_c;
    for (const auto & key : fork_a_written)
        fork_c.insert(key);

    const auto function = getFunction("uniqExactDistinct", {std::make_shared<DataTypeDynamic>(), std::make_shared<DataTypeUInt8>()});
    auto column = ColumnAggregateFunction::create(function);
    auto & states = assert_cast<ColumnAggregateFunction &>(*column);
    Arena & arena = states.createOrGetArena();
    auto add_all = [&](AggregateDataPtr place, const Pairs & pairs)
    {
        auto values = dynamicColumn(pairs.canonical);
        auto bytes = ColumnUInt8::create();
        for (const auto byte : pairs.bytes)
            bytes->getData().push_back(byte);
        const IColumn * arguments[] = {values.get(), bytes.get()};
        for (size_t row = 0; row != pairs.canonical.size(); ++row)
            function->add(place, arguments, row, &arena);
    };

    states.insertDefault();
    const auto place_a = states.getData().back();
    add_all(place_a, first);
    EXPECT_EQ(bridgeState(*function, place_a), fork_a_added);
    states.insertDefault();
    const auto place_b = states.getData().back();
    add_all(place_b, second);
    EXPECT_EQ(bridgeState(*function, place_b), historyState(fork_b));

    function->merge(place_a, place_b, &arena);
    EXPECT_EQ(bridgeState(*function, place_a), fork_a_merged);
    auto count = function->getResultType()->createColumn();
    function->insertResultInto(place_a, *count, &arena);
    EXPECT_EQ(assert_cast<const ColumnUInt64 &>(*count).getData()[0], fork_a.size());

    states.insertDefault();
    const auto place_c = states.getData().back();
    const String stored_a = storedState(*function, place_a);
    ReadBufferFromString in(stored_a);
    function->deserialize(place_c, in, std::nullopt, &arena);
    EXPECT_TRUE(in.eof());
    EXPECT_EQ(bridgeState(*function, place_c), historyState(fork_c));
}

namespace
{

/// The set of the generic `groupArrayIntersect` of the fork (HashSet<std::string_view>: StringViewHash and the default grower, without saved
/// hashes) and its state: the version, the number of elements, then every element as a string, in the order of the set.
using ForkIntersectSet = HashSet<std::string_view>;

String intersectState(UInt64 version, const ForkIntersectSet & set)
{
    WriteBufferFromOwnString out;
    writeVarUInt(version, out);
    writeVarUInt(set.size(), out);
    for (const auto & elem : set)
        writeStringBinary(elem.getValue(), out);
    return out.str();
}

/// The version and the elements of a state, in the order they were written.
std::pair<UInt64, std::vector<String>> stateElements(const String & state)
{
    ReadBufferFromString in(state);
    UInt64 version = 0;
    size_t size = 0;
    readVarUInt(version, in);
    readVarUInt(size, in);
    std::vector<String> keys(size);
    for (auto & key : keys)
        readStringBinary(key, in);
    return {version, std::move(keys)};
}

/// The state of `version` whose set is filled with `keys` in this order: what an add after the first one (the kept elements in the order of
/// the row), an intersecting merge (the kept elements in the order of the state) or a read without a reservation leaves.
String filledState(UInt64 version, const std::vector<String> & keys)
{
    ForkIntersectSet set;
    for (const auto & key : keys)
        set.insert(key);
    return intersectState(version, set);
}

/// A state of the fork's model with the stored form of each element: `stored_keys[i]` is the stored form of `fork_keys[i]`.
String storedForm(const String & fork_state, const std::vector<String> & fork_keys, const std::vector<String> & stored_keys)
{
    const auto [version, keys] = stateElements(fork_state);
    WriteBufferFromOwnString out;
    writeVarUInt(version, out);
    writeVarUInt(keys.size(), out);
    for (const auto & key : keys)
    {
        const auto it = std::find(fork_keys.begin(), fork_keys.end(), key);
        if (it == fork_keys.end())
        {
            ADD_FAILURE() << "an element of the fork's state is not a fork key";
            return {};
        }
        writeStringBinary(stored_keys[static_cast<size_t>(it - fork_keys.begin())], out);
    }
    return out.str();
}

/// The fork's add of a row to a set of `version` (before the add): the first row inserts its elements, a later row keeps the elements that
/// the set has, in a new set filled in the order of the row.
void forkIntersectAdd(ForkIntersectSet & set, UInt64 version, const std::vector<String> & row)
{
    if (version == 0)
    {
        for (const auto & key : row)
            set.insert(key);
        return;
    }
    if (set.empty())
        return;
    ForkIntersectSet kept;
    for (const auto & key : row)
        if (set.find(key) != nullptr)
            kept.insert(key);
    set = std::move(kept);
}

/// The fork's merge of `rhs` (of `rhs_version`) into `set` of `version` (before the merge): an empty state copies `rhs` in its order, a
/// later one keeps, in its own order, the elements that `rhs` has.
void forkIntersectMerge(ForkIntersectSet & set, UInt64 version, const ForkIntersectSet & rhs, UInt64 rhs_version)
{
    if (rhs_version == 0)
        return;
    if (version == 0)
    {
        for (const auto & elem : rhs)
            set.insert(elem.getValue());
        return;
    }
    if (set.empty())
        return;
    ForkIntersectSet kept;
    for (const auto & elem : set)
        if (rhs.find(elem.getValue()) != nullptr)
            kept.insert(elem.getValue());
    set = std::move(kept);
}

/// The bytes of the elements of a state: what a buffer that holds exactly the state has left after the version and the size.
size_t elementBytes(const String & state)
{
    ReadBufferFromString in(state);
    UInt64 version = 0;
    size_t size = 0;
    readVarUInt(version, in);
    readVarUInt(size, in);
    return in.available();
}

/// The fork's read of `state`, then its write, and the cells of its set. The two baselines of the fork differ before the codec (Root's
/// reading of their Gitblobs): the build a394e92fb5d, in production on 2026-09-24, reserved the declared number of elements
/// (`frozen79_bytes` is nullopt); 26.8 before the codec (frozen79) reserves min(size, bytes / 16), where `bytes` is what its buffer held
/// after the version and the size (the bound of ClickHouse#117536, which the release branch of the fork also has since its backport
/// 6b217cc8 of 2026-09-25). Both then insert the elements in the order they were written.
struct ForkIntersectRead
{
    String state;
    size_t cells = 0;
};

ForkIntersectRead forkIntersectRead(const String & state, std::optional<size_t> frozen79_bytes)
{
    const auto [version, keys] = stateElements(state);
    ForkIntersectSet set;
    set.reserve(frozen79_bytes ? std::min(keys.size(), *frozen79_bytes / sizeof(ForkIntersectSet::cell_type)) : keys.size());
    for (const auto & key : keys)
        set.insert(key);
    return {intersectState(version, set), set.getBufferSizeInCells()};
}

/// A buffer that serves a string in chunks of at most `chunk_size` bytes, as a stream does.
class ChunkedReadBuffer : public ReadBuffer
{
public:
    ChunkedReadBuffer(const String & source_, size_t chunk_size_)
        : ReadBuffer(nullptr, 0)
        , source(source_)
        , chunk_size(chunk_size_)
    {
    }

private:
    bool nextImpl() override
    {
        if (served == source.size())
            return false;
        const size_t size = std::min(chunk_size, source.size() - served);
        BufferBase::set(const_cast<char *>(source.data()) + served, size, 0);
        served += size;
        return true;
    }

    const String & source;
    const size_t chunk_size;
    size_t served = 0;
};

}

/// `groupArrayIntersect(Array(Dynamic))` with 512-bit values (the CRC witness UInt512 1 and 4 among them), Int64 values and an ordinary
/// String whose bytes look like a SentioWide header. Every element takes at least 19 bytes in a state, so that the reads below reserve all
/// their elements under both baselines of the fork. A adds a row of 157 elements (its set grows past 256 cells) and a row that it
/// intersects with (the new set grows too); B adds a row; A merges B (an intersection); D, empty, merges A (a copy); C reads A after its
/// second row. Both forms of their states (the fork's, which the bridge writes, and the stored one) are those of the fork's states after
/// the same operations.
TEST(HashApiLegacyProjection, IntersectStatesWithWideKeysHaveTheForkOrder)
{
    struct Row
    {
        std::vector<String> canonical;
        std::vector<String> fork;

        void add(String canonical_value, String fork_value)
        {
            canonical.push_back(std::move(canonical_value));
            fork.push_back(std::move(fork_value));
        }
    };
    Row first;
    for (UInt64 value = 0; value < 150; ++value)
        first.add(int64Value(value), int64Value(value));
    for (UInt64 value = 1; value <= 4; ++value)
        first.add(canonicalUInt512(value), legacyUInt512(value));
    for (UInt64 raw = 1; raw <= 2; ++raw)
        first.add(canonicalDecimal512(76, 10, raw), legacyDecimal512(76, 10, raw));
    first.add(stringValue(HEADER_LIKE_CONTENT), stringValue(HEADER_LIKE_CONTENT));
    /// The 512-bit values of A in another order, Int64 139 down to 0, the String, and two values that A does not have.
    Row second;
    for (UInt64 value : {4, 3, 2, 1})
        second.add(canonicalUInt512(value), legacyUInt512(value));
    second.add(canonicalDecimal512(76, 10, 2), legacyDecimal512(76, 10, 2));
    for (UInt64 i = 0; i < 140; ++i)
        second.add(int64Value(139 - i), int64Value(139 - i));
    second.add(stringValue(HEADER_LIKE_CONTENT), stringValue(HEADER_LIKE_CONTENT));
    second.add(int64Value(1000), int64Value(1000));
    second.add(canonicalUInt512(9), legacyUInt512(9));
    /// B: UInt512 1, 4 and 9, the Decimal512 2 and the even Int64 0 to 198.
    Row third;
    for (UInt64 value : {1, 4, 9})
        third.add(canonicalUInt512(value), legacyUInt512(value));
    third.add(canonicalDecimal512(76, 10, 2), legacyDecimal512(76, 10, 2));
    for (UInt64 value = 0; value < 200; value += 2)
        third.add(int64Value(value), int64Value(value));

    const auto first_keys = arenaForms(first.fork);
    const auto first_canonical_keys = arenaForms(first.canonical);
    const auto second_keys = arenaForms(second.fork);
    const auto third_keys = arenaForms(third.fork);
    /// Every fork key and its stored form, to compare the stored form of the states.
    std::vector<String> all_fork_keys = first_keys;
    std::vector<String> all_stored_keys = first_canonical_keys;
    for (const auto * row : {&second, &third})
    {
        for (auto & key : arenaForms(row->fork))
            all_fork_keys.push_back(std::move(key));
        for (auto & key : arenaForms(row->canonical))
            all_stored_keys.push_back(std::move(key));
    }

    ForkIntersectSet fork_a;
    forkIntersectAdd(fork_a, 0, first_keys);
    const String fork_a1 = intersectState(1, fork_a);
    ASSERT_EQ(fork_a.getBufferSizeInCells(), 1024u);
    /// A with the canonical keys placed by their own hashes, which the fixture must tell from the fork's A.
    ForkIntersectSet canonical_a;
    forkIntersectAdd(canonical_a, 0, first_canonical_keys);
    ASSERT_NE(keyOrder(canonical_a, first_canonical_keys), keyOrder(fork_a, first_keys));
    forkIntersectAdd(fork_a, 1, second_keys);
    const String fork_a2 = intersectState(2, fork_a);
    ASSERT_EQ(fork_a.size(), 146u);
    ForkIntersectSet fork_b;
    forkIntersectAdd(fork_b, 0, third_keys);
    const String fork_b1 = intersectState(1, fork_b);
    forkIntersectMerge(fork_a, 2, fork_b, 1);
    const String fork_a3 = intersectState(3, fork_a);
    ASSERT_EQ(fork_a.size(), 73u);
    ForkIntersectSet fork_d;
    forkIntersectMerge(fork_d, 0, fork_a, 3);
    const String fork_d1 = intersectState(1, fork_d);
    const auto fork_c = forkIntersectRead(fork_a2, elementBytes(fork_a2));
    ASSERT_EQ(fork_c.state, forkIntersectRead(fork_a2, std::nullopt).state);

    const auto function = getFunction("groupArrayIntersect", {std::make_shared<DataTypeArray>(std::make_shared<DataTypeDynamic>())});
    auto column = ColumnAggregateFunction::create(function);
    auto & states = assert_cast<ColumnAggregateFunction &>(*column);
    Arena & arena = states.createOrGetArena();
    auto rows = dynamicArrayRows({first.canonical, second.canonical, third.canonical});
    const IColumn * arguments[] = {rows.get()};
    auto expect_state = [&](AggregateDataPtr place, const String & fork_state, const String & where)
    {
        EXPECT_EQ(bridgeState(*function, place), fork_state) << where;
        EXPECT_EQ(storedState(*function, place), storedForm(fork_state, all_fork_keys, all_stored_keys)) << where;
    };

    states.insertDefault();
    const auto place_a = states.getData().back();
    function->add(place_a, arguments, 0, &arena);
    expect_state(place_a, fork_a1, "A after its first row");
    function->add(place_a, arguments, 1, &arena);
    expect_state(place_a, fork_a2, "A after its second row");
    const String stored_a2 = storedState(*function, place_a);

    states.insertDefault();
    const auto place_b = states.getData().back();
    function->add(place_b, arguments, 2, &arena);
    expect_state(place_b, fork_b1, "B");

    function->merge(place_a, place_b, &arena);
    expect_state(place_a, fork_a3, "A after merging B");

    states.insertDefault();
    const auto place_d = states.getData().back();
    function->merge(place_d, place_a, &arena);
    expect_state(place_d, fork_d1, "D");

    states.insertDefault();
    const auto place_c = states.getData().back();
    ReadBufferFromString in(stored_a2);
    function->deserialize(place_c, in, std::nullopt, &arena);
    EXPECT_TRUE(in.eof());
    expect_state(place_c, fork_c.state, "C");
}

/// Controls: `groupArrayIntersect(Array(String))`, whose keys are the arena forms of the strings (the size with the terminating zero byte, the
/// characters and the zero byte), also of strings whose bytes look like a SentioWide header, and `groupArrayIntersect(Array(Int64))`, whose
/// set holds the numbers, keep their sets: their states are those of the fork's sets of the same keys.
TEST(HashApiLegacyProjection, IntersectControlsKeepTheirSets)
{
    std::vector<String> strings;
    std::vector<String> string_keys;
    for (size_t i = 0; i != 140; ++i)
    {
        strings.push_back(i % 7 == 0 ? HEADER_LIKE_CONTENT + std::to_string(i) : "s" + std::to_string(i));
        string_keys.push_back(littleEndian(strings.back().size() + 1, 8) + strings.back() + String(1, '\0'));
    }
    const auto string_function = getFunction("groupArrayIntersect", {std::make_shared<DataTypeArray>(std::make_shared<DataTypeString>())});
    auto string_states = ColumnAggregateFunction::create(string_function);
    auto & string_column = assert_cast<ColumnAggregateFunction &>(*string_states);
    string_column.insertDefault();
    auto string_values = ColumnString::create();
    for (const auto & value : strings)
        string_values->insertData(value.data(), value.size());
    auto string_offsets = ColumnArray::ColumnOffsets::create();
    string_offsets->getData().push_back(strings.size());
    auto string_rows = ColumnArray::create(std::move(string_values), std::move(string_offsets));
    const IColumn * string_arguments[] = {string_rows.get()};
    string_function->add(string_column.getData().back(), string_arguments, 0, &string_column.createOrGetArena());
    ForkIntersectSet fork_strings;
    forkIntersectAdd(fork_strings, 0, string_keys);
    EXPECT_EQ(storedState(*string_function, string_column.getData().back()), intersectState(1, fork_strings));
    EXPECT_EQ(bridgeState(*string_function, string_column.getData().back()), intersectState(1, fork_strings));

    const auto int_function = getFunction("groupArrayIntersect", {std::make_shared<DataTypeArray>(std::make_shared<DataTypeInt64>())});
    auto int_states = ColumnAggregateFunction::create(int_function);
    auto & int_column = assert_cast<ColumnAggregateFunction &>(*int_states);
    int_column.insertDefault();
    auto int_values = ColumnInt64::create();
    HashSet<Int64> fork_ints;
    for (Int64 value = 0; value != 300; ++value)
    {
        int_values->getData().push_back(value * 7919);
        fork_ints.insert(value * 7919);
    }
    auto int_offsets = ColumnArray::ColumnOffsets::create();
    int_offsets->getData().push_back(int_values->size());
    auto int_rows = ColumnArray::create(std::move(int_values), std::move(int_offsets));
    const IColumn * int_arguments[] = {int_rows.get()};
    int_function->add(int_column.getData().back(), int_arguments, 0, &int_column.createOrGetArena());
    WriteBufferFromOwnString fork_int_state;
    writeVarUInt(1, fork_int_state);
    writeVarUInt(fork_ints.size(), fork_int_state);
    for (const auto & elem : fork_ints)
        writeIntBinary(elem.getValue(), fork_int_state);
    EXPECT_EQ(storedState(*int_function, int_column.getData().back()), fork_int_state.str());
}

namespace
{

/// Root's reserve witness (e6b-intersect-reserve-witness-review.root.md): `groupArrayIntersect(Array(Dynamic))` with the UInt8 values 0 to
/// 164 and one more value, added as one row: UInt512 1, or an ordinary String whose bytes look like a SentioWide header. A UInt8 element
/// takes 12 bytes in a state. The UInt512 element takes 75 bytes in the fork's form and 97 in the stored one, so the elements take 2055 and
/// 2077 bytes; the String element takes 44 bytes in both, so they take 2024 bytes.
struct IntersectWitness
{
    AggregateFunctionPtr function;
    /// Holds the states and their arena.
    MutableColumnPtr column;
    std::vector<String> stored_values;
    std::vector<String> fork_keys;
    std::vector<String> stored_keys;
    String stored;
    String bridge;
    /// The fork's state after the same add.
    String fork;
};

IntersectWitness makeIntersectWitness(bool with_uint512)
{
    std::vector<String> fork_values;
    IntersectWitness witness;
    for (UInt64 value = 0; value <= 164; ++value)
    {
        witness.stored_values.push_back(uint8Value(static_cast<UInt8>(value)));
        fork_values.push_back(uint8Value(static_cast<UInt8>(value)));
    }
    witness.stored_values.push_back(with_uint512 ? canonicalUInt512(1) : stringValue(HEADER_LIKE_CONTENT));
    fork_values.push_back(with_uint512 ? legacyUInt512(1) : stringValue(HEADER_LIKE_CONTENT));
    witness.fork_keys = arenaForms(fork_values);
    witness.stored_keys = arenaForms(witness.stored_values);

    witness.function = getFunction("groupArrayIntersect", {std::make_shared<DataTypeArray>(std::make_shared<DataTypeDynamic>())});
    witness.column = ColumnAggregateFunction::create(witness.function);
    auto & states = assert_cast<ColumnAggregateFunction &>(*witness.column);
    states.insertDefault();
    const auto place = states.getData().back();
    auto rows = dynamicArrayRows({witness.stored_values});
    const IColumn * arguments[] = {rows.get()};
    witness.function->add(place, arguments, 0, &states.createOrGetArena());
    witness.stored = storedState(*witness.function, place);
    witness.bridge = bridgeState(*witness.function, place);

    ForkIntersectSet fork_set;
    forkIntersectAdd(fork_set, 0, witness.fork_keys);
    witness.fork = intersectState(1, fork_set);
    return witness;
}

/// A new state of the witness's column read from `in` with `decoding`.
AggregateDataPtr readIntoNewState(IntersectWitness & witness, ReadBuffer & in, const BinaryTypeDecoding & decoding)
{
    auto & states = assert_cast<ColumnAggregateFunction &>(*witness.column);
    states.insertDefault();
    const auto place = states.getData().back();
    witness.function->deserializeWithBinaryTypeDecoding(place, in, std::nullopt, &states.createOrGetArena(), decoding);
    return place;
}

/// A new state of the witness's column that got one row with `values` (stored forms of `Dynamic` values).
AggregateDataPtr newState(IntersectWitness & witness, const std::vector<String> & values)
{
    auto & states = assert_cast<ColumnAggregateFunction &>(*witness.column);
    states.insertDefault();
    const auto place = states.getData().back();
    auto rows = dynamicArrayRows({values});
    const IColumn * arguments[] = {rows.get()};
    witness.function->add(place, arguments, 0, &states.createOrGetArena());
    return place;
}

/// The stored forms of the values of the witness in the order of the elements of a fork's state.
std::vector<String> valuesInOrder(const IntersectWitness & witness, const String & fork_state)
{
    std::vector<String> values;
    for (const auto & key : stateElements(fork_state).second)
    {
        const auto it = std::find(witness.fork_keys.begin(), witness.fork_keys.end(), key);
        if (it == witness.fork_keys.end())
        {
            ADD_FAILURE() << "an element of the fork's state is not a key of the witness";
            return {};
        }
        values.push_back(witness.stored_values[static_cast<size_t>(it - witness.fork_keys.begin())]);
    }
    return values;
}

/// Both forms of a state and its result values are those of the fork's state.
void expectWitnessState(IntersectWitness & witness, AggregateDataPtr place, const String & fork_state, const String & where)
{
    EXPECT_EQ(bridgeState(*witness.function, place), fork_state) << where;
    EXPECT_EQ(storedState(*witness.function, place), storedForm(fork_state, witness.fork_keys, witness.stored_keys)) << where;
    auto result = witness.function->getResultType()->createColumn();
    witness.function->insertResultInto(place, *result, &assert_cast<ColumnAggregateFunction &>(*witness.column).createOrGetArena());
    EXPECT_EQ(firstRowValues(*result), valuesInOrder(witness, fork_state)) << where;
}

}

/// The witness with UInt512 1. The baselines of the fork differ on it before the codec: a394e92fb5d reserves the 166 declared elements (512
/// cells); frozen79 reserves 2055 / 16 = 128 elements from a buffer that holds exactly the fork's form (256 cells, which grow to 1024 at the
/// 129th element), another order, and its formula on the 2077 bytes of the stored form would reserve 129 elements (512 cells). A state with
/// an element that holds a 512-bit type reserves all its elements once they are read and valid, so the reads of both forms, from a buffer
/// that holds exactly the state, from one with more bytes after it and from chunks of a stream, all give a394e92fb5d's set, and the same
/// values with the same types and bytes. Then the read state continues (it merges a state with all the values, which keeps its elements in
/// its order in a new set that grows to 1024 cells), and both forms are read again, again as a394e92fb5d did.
TEST(HashApiLegacyProjection, IntersectReadsOfStatesWith512BitElementsReserveAllElements)
{
    auto witness = makeIntersectWitness(true);
    ASSERT_EQ(elementBytes(witness.bridge), 2055u);
    ASSERT_EQ(elementBytes(witness.stored), 2077u);
    ASSERT_EQ(witness.bridge, witness.fork);

    const auto a394e92 = forkIntersectRead(witness.fork, std::nullopt);
    const auto frozen79 = forkIntersectRead(witness.fork, 2055);
    EXPECT_EQ(a394e92.cells, 512u);
    EXPECT_EQ(frozen79.cells, 1024u);
    /// The witness tells the two baselines apart.
    ASSERT_NE(frozen79.state, a394e92.state);
    EXPECT_EQ(forkIntersectRead(witness.fork, 2077).cells, 512u);

    const BinaryTypeDecoding legacy{.origin = BinaryTypeOrigin::SentioLegacyV0};
    const String next(9, '\x01');
    auto expect_reads = [&](const String & bytes, const BinaryTypeDecoding & decoding, const String & where)
    {
        ReadBufferFromString exact(bytes);
        expectWitnessState(witness, readIntoNewState(witness, exact, decoding), a394e92.state, where + ", exact buffer");
        EXPECT_TRUE(exact.eof()) << where;
        const String bytes_then_next = bytes + next;
        ReadBufferFromString with_next(bytes_then_next);
        expectWitnessState(witness, readIntoNewState(witness, with_next, decoding), a394e92.state, where + ", more bytes after it");
        EXPECT_EQ(with_next.available(), next.size()) << where;
        ChunkedReadBuffer chunks(bytes, 100);
        expectWitnessState(witness, readIntoNewState(witness, chunks, decoding), a394e92.state, where + ", chunks");
        EXPECT_TRUE(chunks.eof()) << where;
    };
    expect_reads(witness.bridge, legacy, "the fork's form");
    expect_reads(witness.stored, BinaryTypeDecoding{}, "the stored form");

    /// Continue, then read both forms again.
    ReadBufferFromString stored_in(witness.stored);
    const auto place = readIntoNewState(witness, stored_in, BinaryTypeDecoding{});
    auto & arena = assert_cast<ColumnAggregateFunction &>(*witness.column).createOrGetArena();
    witness.function->merge(place, newState(witness, witness.stored_values), &arena);
    const String continued = filledState(2, stateElements(a394e92.state).second);
    expectWitnessState(witness, place, continued, "continued");
    const auto continued_read = forkIntersectRead(continued, std::nullopt);
    const String continued_bridge = bridgeState(*witness.function, place);
    const String continued_stored = storedState(*witness.function, place);
    ReadBufferFromString bridge_again(continued_bridge);
    expectWitnessState(witness, readIntoNewState(witness, bridge_again, legacy), continued_read.state, "the fork's form read again");
    ReadBufferFromString stored_again(continued_stored);
    expectWitnessState(witness, readIntoNewState(witness, stored_again, BinaryTypeDecoding{}), continued_read.state, "the stored form read again");
}

/// The witness with the String instead of UInt512: no element holds a 512-bit type (the String's bytes of the namespace are content), so the
/// reads reserve as frozen79 does, from the bytes that the buffer held after the version and the size, and keep its dependence on the
/// buffer: 2024 / 16 = 126 elements from a buffer that holds exactly the state (256 cells, which grow to 1024), 129 with 40 more bytes after
/// it (512 cells), 97 / 16 = 6 from chunks of 100 bytes (256 cells, which grow to 1024). a394e92fb5d's 166 elements (512 cells) give another
/// order than the read of exactly the state, so the fixture tells the policies apart.
TEST(HashApiLegacyProjection, IntersectReadsOfStatesWithout512BitElementsReserveAsBefore)
{
    auto witness = makeIntersectWitness(false);
    ASSERT_EQ(witness.stored, witness.bridge);
    ASSERT_EQ(witness.bridge, witness.fork);
    ASSERT_EQ(elementBytes(witness.stored), 2024u);
    const size_t header = witness.stored.size() - 2024;
    const String next(40, '\x01');
    const auto frozen79_exact = forkIntersectRead(witness.fork, 2024);
    const auto frozen79_with_next = forkIntersectRead(witness.fork, 2024 + next.size());
    const auto frozen79_chunks = forkIntersectRead(witness.fork, 100 - header);
    const auto a394e92 = forkIntersectRead(witness.fork, std::nullopt);
    EXPECT_EQ(frozen79_exact.cells, 1024u);
    EXPECT_EQ(frozen79_with_next.cells, 512u);
    EXPECT_EQ(frozen79_chunks.cells, 1024u);
    EXPECT_EQ(a394e92.cells, 512u);
    ASSERT_NE(frozen79_exact.state, a394e92.state);

    auto expect_reads = [&](const BinaryTypeDecoding & decoding, const String & where)
    {
        ReadBufferFromString exact(witness.stored);
        expectWitnessState(witness, readIntoNewState(witness, exact, decoding), frozen79_exact.state, where + ", exact buffer");
        const String stored_then_next = witness.stored + next;
        ReadBufferFromString with_next(stored_then_next);
        expectWitnessState(witness, readIntoNewState(witness, with_next, decoding), frozen79_with_next.state, where + ", more bytes after it");
        EXPECT_EQ(with_next.available(), next.size()) << where;
        ChunkedReadBuffer chunks(witness.stored, 100);
        expectWitnessState(witness, readIntoNewState(witness, chunks, decoding), frozen79_chunks.state, where + ", chunks");
    };
    expect_reads(BinaryTypeDecoding{}, "unknown origin");
    expect_reads(BinaryTypeDecoding{.origin = BinaryTypeOrigin::SentioLegacyV0}, "the fork's origin");
}

/// Histories of states read with different policies, recorded step by step against both baselines of the fork. Each read follows the
/// policy of the elements that the state has at that read. So a state that held a 512-bit element does not have a394e92fb5d's sets in all of
/// its history, and no history matches one baseline at every step. An add or a merge after the first one only keeps the elements that the
/// state has (an intersection, in a new set), so a state without a 512-bit element never gets one; only the first add or merge of an empty
/// state can bring one.
/// - The witness with the String (no 512-bit element) is read from exactly its bytes: frozen79's set (a394e92fb5d's has other cells). A row
///   with UInt512 1 and all of its values in the reverse order is added: the UInt512, which the state does not have, is not kept, and the new
///   set is filled in the order of the row, the same after both baselines. Written and read again, the state still has no 512-bit element:
///   frozen79's set again.
/// - The same state, read, merges a state with UInt512 1 and all of its values: it keeps its own elements in its own order, so it follows
///   frozen79's read. Whether the two baselines still differ after this merge depends only on the collisions of the new set, which is
///   recorded as a property, not asserted.
/// - The witness with UInt512 1 is read with a394e92fb5d's set. An added row of its UInt8 values does not keep the UInt512. Written and read
///   again without a 512-bit element, the state has frozen79's set.
/// - An empty state's first merge copies the witness with UInt512 1, which it then holds: read again, it has a394e92fb5d's set.
TEST(HashApiLegacyProjection, IntersectHistoriesFollowThePolicyOfEachRead)
{
    auto plain = makeIntersectWitness(false);
    auto & plain_arena = assert_cast<ColumnAggregateFunction &>(*plain.column).createOrGetArena();
    const auto read1_frozen79 = forkIntersectRead(plain.fork, elementBytes(plain.fork));
    const auto read1_a394e92 = forkIntersectRead(plain.fork, std::nullopt);
    EXPECT_NE(read1_frozen79.state, read1_a394e92.state);

    /// An add with a 512-bit value after the read.
    ReadBufferFromString in1(plain.stored);
    const auto place = readIntoNewState(plain, in1, BinaryTypeDecoding{});
    expectWitnessState(plain, place, read1_frozen79.state, "read");
    std::vector<String> row{canonicalUInt512(1)};
    row.insert(row.end(), plain.stored_values.rbegin(), plain.stored_values.rend());
    auto rows = dynamicArrayRows({row});
    const IColumn * arguments[] = {rows.get()};
    plain.function->add(place, arguments, 0, &plain_arena);
    const String added = filledState(2, std::vector<String>(plain.fork_keys.rbegin(), plain.fork_keys.rend()));
    expectWitnessState(plain, place, added, "after the add");
    const String added_stored = storedState(*plain.function, place);
    const auto read2_frozen79 = forkIntersectRead(added, elementBytes(added));
    const auto read2_a394e92 = forkIntersectRead(added, std::nullopt);
    EXPECT_NE(read2_frozen79.state, read2_a394e92.state);
    ReadBufferFromString in2(added_stored);
    expectWitnessState(plain, readIntoNewState(plain, in2, BinaryTypeDecoding{}), read2_frozen79.state, "read after the add");

    /// A merge with a state that has a 512-bit value after the read.
    ReadBufferFromString in3(plain.stored);
    const auto merged_place = readIntoNewState(plain, in3, BinaryTypeDecoding{});
    std::vector<String> all_values{canonicalUInt512(1)};
    all_values.insert(all_values.end(), plain.stored_values.begin(), plain.stored_values.end());
    plain.function->merge(merged_place, newState(plain, all_values), &plain_arena);
    const String merged_frozen79 = filledState(2, stateElements(read1_frozen79.state).second);
    const String merged_a394e92 = filledState(2, stateElements(read1_a394e92.state).second);
    RecordProperty("baselines_differ_after_the_merge", merged_frozen79 == merged_a394e92 ? "no" : "yes");
    expectWitnessState(plain, merged_place, merged_frozen79, "after the merge");

    /// A state with a 512-bit element that loses it.
    auto wide = makeIntersectWitness(true);
    auto & wide_arena = assert_cast<ColumnAggregateFunction &>(*wide.column).createOrGetArena();
    ReadBufferFromString in4(wide.stored);
    const auto wide_place = readIntoNewState(wide, in4, BinaryTypeDecoding{});
    expectWitnessState(wide, wide_place, forkIntersectRead(wide.fork, std::nullopt).state, "read with the UInt512");
    std::vector<String> uint8_row(wide.stored_values.rbegin() + 1, wide.stored_values.rend());
    auto uint8_rows = dynamicArrayRows({uint8_row});
    const IColumn * uint8_arguments[] = {uint8_rows.get()};
    wide.function->add(wide_place, uint8_arguments, 0, &wide_arena);
    const String dropped = filledState(2, std::vector<String>(wide.fork_keys.rbegin() + 1, wide.fork_keys.rend()));
    expectWitnessState(wide, wide_place, dropped, "after the add that drops the UInt512");
    const auto read3_frozen79 = forkIntersectRead(dropped, elementBytes(dropped));
    const auto read3_a394e92 = forkIntersectRead(dropped, std::nullopt);
    EXPECT_NE(read3_frozen79.state, read3_a394e92.state);
    const String dropped_stored = storedState(*wide.function, wide_place);
    ReadBufferFromString in5(dropped_stored);
    expectWitnessState(wide, readIntoNewState(wide, in5, BinaryTypeDecoding{}), read3_frozen79.state, "read without the UInt512");

    /// An empty state's first merge.
    ReadBufferFromString in6(wide.stored);
    const auto wide_read = readIntoNewState(wide, in6, BinaryTypeDecoding{});
    auto & wide_states = assert_cast<ColumnAggregateFunction &>(*wide.column);
    wide_states.insertDefault();
    const auto empty_place = wide_states.getData().back();
    wide.function->merge(empty_place, wide_read, &wide_arena);
    const String copied = filledState(1, stateElements(forkIntersectRead(wide.fork, std::nullopt).state).second);
    expectWitnessState(wide, empty_place, copied, "after an empty state's first merge");
    const String copied_stored = storedState(*wide.function, empty_place);
    ReadBufferFromString in7(copied_stored);
    expectWitnessState(
        wide, readIntoNewState(wide, in7, BinaryTypeDecoding{}), forkIntersectRead(copied, std::nullopt).state, "read after the first merge");
}

/// Malformed states of the witness with UInt512 are refused before the set reserves anything: a truncated element, more declared elements
/// than the state has (up to the maximum, 100000000, whose reservation alone would take gigabytes), more than the maximum, and the fork's
/// form read without a declared origin. Nothing of a refused state is added to the column.
TEST(HashApiLegacyProjection, IntersectReadsRefuseMalformedStatesBeforeReserving)
{
    auto witness = makeIntersectWitness(true);
    const String elements = witness.stored.substr(witness.stored.size() - elementBytes(witness.stored));
    const auto type = std::make_shared<DataTypeAggregateFunction>(
        witness.function, DataTypes{std::make_shared<DataTypeArray>(std::make_shared<DataTypeDynamic>())}, Array{});
    auto code_of_read = [&](const String & bytes, const FormatSettings & settings)
    {
        auto column = type->createColumn();
        int code = 0;
        try
        {
            ReadBufferFromString in(bytes);
            type->getDefaultSerialization()->deserializeBinary(*column, in, settings);
        }
        catch (const Exception & e)
        {
            code = e.code();
        }
        EXPECT_EQ(column->size(), code ? 0u : 1u);
        return code;
    };
    EXPECT_EQ(code_of_read(witness.stored, FormatSettings{}), 0);
    EXPECT_EQ(code_of_read(witness.stored.substr(0, witness.stored.size() - 1), FormatSettings{}), ErrorCodes::CANNOT_READ_ALL_DATA);
    EXPECT_EQ(code_of_read(varUInt(1) + varUInt(100'000'000) + elements, FormatSettings{}), ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF);
    EXPECT_EQ(code_of_read(varUInt(1) + varUInt(100'000'001) + elements, FormatSettings{}), ErrorCodes::TOO_LARGE_ARRAY_SIZE);
    EXPECT_EQ(code_of_read(witness.bridge, FormatSettings{}), ErrorCodes::INCORRECT_DATA);
}

/// Elements that have arrived do not bound the reservation: a state of 524288 copies of one UInt8 element and UInt512 1 has 2 distinct
/// elements in about 6 MiB, but its read reserves 524289 elements, 2097152 cells with saved hashes, 48 MiB. The memory limit applies to
/// that reservation: with a limit below the peak of an unlimited read by half the reservation, the read fails with MEMORY_LIMIT_EXCEEDED
/// and adds nothing to the column. The unlimited read gives the fork's set of the two elements. Both reads run on a thread of their own,
/// with its own ThreadStatus, under a new tracker for each read: everything that a read allocates is freed on that thread under the same
/// tracker, and nothing of the trackers of the test's thread is reset or changed.
TEST(HashApiLegacyProjection, IntersectReadReservationIsUnderTheMemoryLimit)
{
    constexpr size_t copies = 524288;
    const auto function = getFunction("groupArrayIntersect", {std::make_shared<DataTypeArray>(std::make_shared<DataTypeDynamic>())});
    const auto type = std::make_shared<DataTypeAggregateFunction>(
        function, DataTypes{std::make_shared<DataTypeArray>(std::make_shared<DataTypeDynamic>())}, Array{});
    const String uint8_key = arenaForm(uint8Value(0));
    WriteBufferFromOwnString stored_out;
    WriteBufferFromOwnString fork_out;
    for (auto * out : {&stored_out, &fork_out})
    {
        writeVarUInt(1, *out);
        writeVarUInt(copies + 1, *out);
        for (size_t i = 0; i != copies; ++i)
            writeStringBinary(uint8_key, *out);
    }
    writeStringBinary(arenaForm(canonicalUInt512(1)), stored_out);
    writeStringBinary(arenaForm(legacyUInt512(1)), fork_out);
    const String stored = stored_out.str();
    const String fork = fork_out.str();
    const auto expected = forkIntersectRead(fork, std::nullopt);

    HashTableGrowerWithPrecalculation<> grower;
    grower.set(copies + 1);
    const size_t reservation = grower.bufSize() * sizeof(HashSetWithSavedHash<std::string_view>::cell_type);
    EXPECT_EQ(reservation, 48u << 20);
    EXPECT_GT(reservation, 6 * elementBytes(stored));

    /// Only plain values leave the thread, so that nothing that it allocated is freed on another one.
    struct Outcome
    {
        bool unexpected_exception = false;
        Int64 unlimited_peak = 0;
        size_t unlimited_rows = 0;
        bool unlimited_state_is_the_forks = false;
        bool limited_read_ran = false;
        int limited_code = 0;
        size_t limited_rows = 0;
    };
    Outcome outcome;
    std::thread reads([&]
    {
        ThreadStatus thread_status;
        auto & thread_tracker = CurrentThread::get().memory_tracker;
        MemoryTracker * const parent = thread_tracker.getParent();
        CurrentThread::get().untracked_memory_limit = 0;
        CurrentThread::flushUntrackedMemory();
        try
        {
            const auto serialization = type->getDefaultSerialization();
            {
                MemoryTracker read_tracker(&total_memory_tracker, VariableContext::Process, /*log_peak_memory_usage_in_destructor=*/false);
                thread_tracker.setParent(&read_tracker);
                SCOPE_EXIT_SAFE({
                    CurrentThread::flushUntrackedMemory();
                    thread_tracker.setParent(parent);
                });
                auto column = type->createColumn();
                ReadBufferFromString in(stored);
                serialization->deserializeBinary(*column, in, FormatSettings{});
                outcome.unlimited_peak = read_tracker.getPeak();
                outcome.unlimited_rows = column->size();
                if (column->size() == 1)
                    outcome.unlimited_state_is_the_forks
                        = bridgeState(*function, assert_cast<const ColumnAggregateFunction &>(*column).getData()[0]) == expected.state;
                column.reset();
            }
            if (outcome.unlimited_peak > static_cast<Int64>(reservation))
            {
                MemoryTracker read_tracker(&total_memory_tracker, VariableContext::Process, /*log_peak_memory_usage_in_destructor=*/false);
                read_tracker.setHardLimit(outcome.unlimited_peak - static_cast<Int64>(reservation / 2));
                thread_tracker.setParent(&read_tracker);
                SCOPE_EXIT_SAFE({
                    CurrentThread::flushUntrackedMemory();
                    thread_tracker.setParent(parent);
                });
                auto column = type->createColumn();
                outcome.limited_read_ran = true;
                try
                {
                    ReadBufferFromString in(stored);
                    serialization->deserializeBinary(*column, in, FormatSettings{});
                }
                catch (const Exception & e)
                {
                    outcome.limited_code = e.code();
                }
                outcome.limited_rows = column->size();
                column.reset();
            }
        }
        catch (...)
        {
            outcome.unexpected_exception = true;
        }
    });
    reads.join();

    ASSERT_FALSE(outcome.unexpected_exception);
    EXPECT_EQ(outcome.unlimited_rows, 1u);
    EXPECT_TRUE(outcome.unlimited_state_is_the_forks);
    ASSERT_GT(outcome.unlimited_peak, static_cast<Int64>(reservation));
    ASSERT_TRUE(outcome.limited_read_ran);
    EXPECT_EQ(outcome.limited_code, ErrorCodes::MEMORY_LIMIT_EXCEEDED);
    EXPECT_EQ(outcome.limited_rows, 0u);
}

/// A shared `QBit` value must hash in the same legacy domain whether the range also contains a 512-bit value or not.
TEST(HashApiLegacyProjection, StridedQBitSharedHashDoesNotDependOnOtherRows)
{
    const auto qbit_type = std::make_shared<DataTypeQBit>(std::make_shared<DataTypeFloat32>(), 16, 8);
    auto qbit_value = qbit_type->createColumn();
    qbit_value->insertDefault();
    auto canonical_column = ColumnDynamic::create(0);
    auto & canonical = assert_cast<ColumnDynamic &>(*canonical_column);
    insertShared(canonical, qbit_type, *qbit_value);
    const String canonical_bytes = String{"\x37\x0d\x10\x08\x10", 5} + String(64, '\0');
    const String legacy_bytes = String{"\x3a\x0d\x10\x08\x10", 5} + String(64, '\0');
    EXPECT_EQ(canonical.getSharedVariant().getDataAt(0), std::string_view(canonical_bytes));

    auto legacy_column = canonical.cloneEmpty();
    auto & legacy = assert_cast<ColumnDynamic &>(*legacy_column);
    appendLegacyShared(legacy, legacy_bytes);
    EXPECT_EQ(rangeHash(canonical, 0, 1), rangeHash(legacy.getVariantColumn(), 0, 1));
    EXPECT_EQ(weakHash(canonical, 0, 1, true), weakHash(legacy.getVariantColumn(), 0, 1, true));
    EXPECT_EQ(fastHash(canonical), fastHash(legacy.getVariantColumn()));

    const auto uint512_type = std::make_shared<DataTypeUInt512>();
    insertShared(canonical, uint512_type, *oneValue<ColumnUInt512>(uint512_type, UInt512(3)));
    appendLegacyShared(legacy, legacyUInt512(3));
    EXPECT_EQ(weakHash(canonical, 0, 1, true)[0], weakHash(canonical, 0, 2, true)[0]);
    EXPECT_EQ(weakHash(canonical, 0, 2, true), weakHash(legacy.getVariantColumn(), 0, 2, true));
    EXPECT_EQ(rangeHash(canonical, 0, 2), rangeHash(legacy.getVariantColumn(), 0, 2));
    EXPECT_EQ(fastHash(canonical), fastHash(legacy.getVariantColumn()));
}
