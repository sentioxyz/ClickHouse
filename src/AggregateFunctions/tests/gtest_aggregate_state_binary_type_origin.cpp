#include <gtest/gtest.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnDynamic.h>
#include <Columns/ColumnString.h>
#include <Core/Field.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDynamic.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/SentioWideRecode.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/AlignedBuffer.h>
#include <Common/Arena.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_register.h>

#include <fmt/format.h>

/// Aggregation states that keep `Dynamic` values (their arena or binary forms) keep their binary type headers. The plain deserialize
/// validates them as the Unknown origin while the state is read; an explicit SentioLegacyV0 origin normalizes them to the canonical
/// encoding, SentioLegacyBridge writes the legacy indexes again; the values in a state are nested in it, so the depth limit counts the
/// state; `topK` keeps its keys in the legacy projection, the hash domain of its alpha map. The bytes come from the binary type encoding
/// specification and from Root's fixture draft (the SentioWide v1 descriptor), not from the code under test.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
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

/// The binary Dynamic value of `Decimal512(76, 10)` with the raw value 1: the legacy header 0x39 P S, or the SentioWide v1 header.
String legacyValue()
{
    return String{0x39, 76, 10} + littleEndian(1, 64);
}

String canonicalValue()
{
    const String descriptor = "SentioWide_v1_Decimal512(76,10)";
    return String(1, 0x2C) + varUInt(descriptor.size()) + descriptor + littleEndian(1, 64);
}

/// The arena form of a non-NULL Dynamic value: the NULL flag, the size (8 bytes) and the binary value.
String arenaForm(const String & value)
{
    return String(1, '\0') + littleEndian(value.size(), 8) + value;
}

/// A `groupArray` state with one general node.
String groupArrayState(const String & value)
{
    const String node = arenaForm(value);
    return varUInt(1) + varUInt(node.size()) + node;
}

/// A set state (`groupUniqArray`, `-Distinct` history) with one element.
String setState(const String & value)
{
    const String element = arenaForm(value);
    return varUInt(1) + varUInt(element.size()) + element;
}

AggregateFunctionPtr getFunction(const String & name, const DataTypes & arguments, const Array & parameters = {})
{
    tryRegisterAggregateFunctions();
    AggregateFunctionProperties properties;
    return AggregateFunctionFactory::instance().get(name, NullsAction::EMPTY, arguments, parameters, properties);
}

DataTypePtr dynamicType()
{
    return std::make_shared<DataTypeDynamic>();
}

/// A created state that is destroyed at the end of the scope.
struct State
{
    explicit State(AggregateFunctionPtr function_) : function(std::move(function_)), buffer(function->sizeOfData(), function->alignOfData())
    {
        function->create(place());
    }
    ~State() { function->destroy(place()); }

    AggregateDataPtr place() { return buffer.data(); }

    String serialize(BinaryTypeWriterMode mode = BinaryTypeWriterMode::SentioWideV1)
    {
        WriteBufferFromOwnString out;
        if (mode == BinaryTypeWriterMode::SentioWideV1)
            function->serialize(place(), out);
        else
            function->serializeWithBinaryTypeWriterMode(place(), out, std::nullopt, mode);
        return out.str();
    }

    /// Reads all of `bytes` with the plain deserialize, or with `decoding`.
    void deserialize(const String & bytes, const BinaryTypeDecoding * decoding = nullptr)
    {
        ReadBufferFromString in(bytes);
        if (decoding)
            function->deserializeWithBinaryTypeDecoding(place(), in, std::nullopt, &arena, *decoding);
        else
            function->deserialize(place(), in, std::nullopt, &arena);
        EXPECT_TRUE(in.eof());
    }

    AggregateFunctionPtr function;
    AlignedBuffer buffer;
    Arena arena;
};

template <typename F>
int errorCodeOf(F && f)
{
    try
    {
        f();
    }
    catch (const Exception & e)
    {
        return e.code();
    }
    return 0;
}

const BinaryTypeDecoding LEGACY{.origin = BinaryTypeOrigin::SentioLegacyV0};

/// The keys of a serialized `topK` set (SpaceSaving::write): count, then key, count and error of every counter, then the alpha map.
std::vector<String> topKKeys(const String & state)
{
    ReadBufferFromString in(state);
    size_t count = 0;
    readVarUInt(count, in);
    std::vector<String> keys(count);
    for (auto & key : keys)
    {
        readStringBinary(key, in);
        UInt64 ignored = 0;
        readVarUInt(ignored, in);
        readVarUInt(ignored, in);
    }
    return keys;
}

}

TEST(AggregateStateBinaryTypeOrigin, GroupArrayNodesAreValidatedAndNormalized)
{
    auto function = getFunction("groupArray", {dynamicType()});

    /// The plain deserialize refuses an ambiguous legacy header while the state is read.
    {
        State state(function);
        EXPECT_EQ(errorCodeOf([&] { state.deserialize(groupArrayState(legacyValue())); }), ErrorCodes::INCORRECT_DATA);
    }

    /// The legacy origin normalizes it; the canonical state is written, and the bridge export writes the legacy one again.
    State state(function);
    state.deserialize(groupArrayState(legacyValue()), &LEGACY);
    EXPECT_EQ(hexOf(state.serialize()), hexOf(groupArrayState(canonicalValue())));
    EXPECT_EQ(hexOf(state.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(groupArrayState(legacyValue())));

    auto result = function->getResultType()->createColumn();
    function->insertResultInto(state.place(), *result, &state.arena);
    const auto & elements = assert_cast<const ColumnDynamic &>(assert_cast<const ColumnArray &>(*result).getData());
    ASSERT_EQ(elements.size(), 1U);
    EXPECT_EQ(elements.getTypeAt(0)->getTypeId(), TypeIndex::Decimal512);

    /// The canonical state is read by the plain deserialize as it is.
    State canonical(function);
    canonical.deserialize(groupArrayState(canonicalValue()));
    EXPECT_EQ(hexOf(canonical.serialize()), hexOf(groupArrayState(canonicalValue())));
}

TEST(AggregateStateBinaryTypeOrigin, SetStatesAreValidatedAndNormalized)
{
    for (const auto * name : {"groupUniqArray", "groupArrayDistinct"})
    {
        auto function = getFunction(name, {dynamicType()});
        {
            State state(function);
            EXPECT_EQ(errorCodeOf([&] { state.deserialize(setState(legacyValue())); }), ErrorCodes::INCORRECT_DATA) << name;
        }
        State state(function);
        state.deserialize(setState(legacyValue()), &LEGACY);
        EXPECT_EQ(hexOf(state.serialize()), hexOf(setState(canonicalValue()))) << name;
        EXPECT_EQ(hexOf(state.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(setState(legacyValue()))) << name;
    }
}

TEST(AggregateStateBinaryTypeOrigin, SingleValueStatesAreValidatedAndNormalized)
{
    /// `anyLast(Dynamic)` keeps the binary Dynamic value after a flag.
    auto function = getFunction("anyLast", {dynamicType()});
    const String legacy = String(1, '\1') + legacyValue();
    const String canonical = String(1, '\1') + canonicalValue();
    {
        State state(function);
        EXPECT_EQ(errorCodeOf([&] { state.deserialize(legacy); }), ErrorCodes::INCORRECT_DATA);
    }
    State state(function);
    state.deserialize(legacy, &LEGACY);
    EXPECT_EQ(hexOf(state.serialize()), hexOf(canonical));
    EXPECT_EQ(hexOf(state.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(legacy));
}

TEST(AggregateStateBinaryTypeOrigin, WholeStateHelpersRefuseTrailingBytes)
{
    auto function = getFunction("groupArray", {dynamicType()});
    auto type = std::make_shared<DataTypeAggregateFunction>(function, DataTypes{dynamicType()}, Array{});
    auto serialization = type->getDefaultSerialization();

    auto read_field = [&](const String & state)
    {
        WriteBufferFromOwnString out;
        writeStringBinary(state, out);
        ReadBufferFromString in(out.str());
        Field field;
        serialization->deserializeBinary(field, in, FormatSettings{});
        return field;
    };
    EXPECT_NO_THROW(read_field(groupArrayState(canonicalValue())));
    EXPECT_EQ(errorCodeOf([&] { read_field(groupArrayState(canonicalValue()) + String(1, '\0')); }), ErrorCodes::BAD_ARGUMENTS);
    /// A truncated state.
    const String state = groupArrayState(canonicalValue());
    EXPECT_NE(errorCodeOf([&] { read_field(state.substr(0, state.size() - 1)); }), 0);
}

/// A Dynamic value holding an aggregation state: the state is nested in the value and the values in the state are nested in it, so the
/// depth limit counts them; the elements of the state are siblings and are not added up.
TEST(AggregateStateBinaryTypeOrigin, NestedStateCountsTowardsTheDepthLimit)
{
    auto function = getFunction("groupArray", {dynamicType()});
    auto type = std::make_shared<DataTypeAggregateFunction>(function, DataTypes{dynamicType()}, Array{});
    WriteBufferFromOwnString header;
    encodeDataType(type, header);
    const String value = header.str() + groupArrayState(canonicalValue());

    auto accept = [&](const String & bytes, size_t limit)
    {
        FormatSettings settings;
        settings.binary.max_binary_type_complexity = limit;
        String storage;
        acceptBinaryDynamicValue(bytes, settings, storage);
    };
    EXPECT_EQ(errorCodeOf([&] { accept(value, 2); }), ErrorCodes::INCORRECT_DATA);
    EXPECT_NO_THROW(accept(value, 3));

    /// 2000 elements with the limit 1000.
    String many = varUInt(2000);
    for (size_t i = 0; i < 2000; ++i)
    {
        const String node = arenaForm(String(1, 0x0A) + littleEndian(i, 8));
        many += varUInt(node.size()) + node;
    }
    EXPECT_NO_THROW(accept(header.str() + many, 1000));
}

/// `topK` keeps its keys in the legacy projection, the hash domain of its alpha map: the canonical state has the canonical keys, the
/// bridge export the legacy ones, and reading the canonical state restores exactly the same set.
TEST(AggregateStateBinaryTypeOrigin, TopKKeepsTheHashDomainOfItsKeys)
{
    auto function = getFunction("topK", {dynamicType()}, Array{Field(UInt64(10))});

    auto column = ColumnDynamic::create();
    {
        ReadBufferFromString in(canonicalValue());
        DataTypeDynamic().getDefaultSerialization()->deserializeBinary(*column, in, FormatSettings{});
    }
    const IColumn * columns[] = {column.get()};

    State state(function);
    function->add(state.place(), columns, 0, &state.arena);
    function->add(state.place(), columns, 0, &state.arena);

    const String canonical = state.serialize();
    const String bridge = state.serialize(BinaryTypeWriterMode::SentioLegacyBridge);
    ASSERT_EQ(topKKeys(canonical).size(), 1U);
    EXPECT_EQ(hexOf(topKKeys(canonical)[0]), hexOf(arenaForm(canonicalValue())));
    EXPECT_EQ(hexOf(topKKeys(bridge)[0]), hexOf(arenaForm(legacyValue())));

    /// Read back (plain, and from the legacy bridge bytes): the same set, with the same keys, counts and alpha map.
    State reread(function);
    reread.deserialize(canonical);
    EXPECT_EQ(hexOf(reread.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(bridge));
    State from_legacy(function);
    from_legacy.deserialize(bridge, &LEGACY);
    EXPECT_EQ(hexOf(from_legacy.serialize()), hexOf(canonical));

    auto result = function->getResultType()->createColumn();
    function->insertResultInto(reread.place(), *result, &reread.arena);
    const auto & values = assert_cast<const ColumnDynamic &>(assert_cast<const ColumnArray &>(*result).getData());
    ASSERT_EQ(values.size(), 1U);
    EXPECT_EQ(values.getTypeAt(0)->getTypeId(), TypeIndex::Decimal512);
}

/// With evictions the alpha map of `topK` holds nonzero buckets indexed by the hashes of the keys. A state written canonically and read
/// back continues exactly like the original: the same further values give the same keys, counts, errors and alpha map.
TEST(AggregateStateBinaryTypeOrigin, TopKContinuationAfterARoundTrip)
{
    auto function = getFunction("topK", {dynamicType()}, Array{Field(UInt64(1))});

    /// Native Decimal512(76, 10) values 1..6 (in the shared variant, with the descriptor) and Int64 values, as Dynamic values.
    auto column = ColumnDynamic::create();
    const auto serialization = DataTypeDynamic().getDefaultSerialization();
    const String descriptor = "SentioWide_v1_Decimal512(76,10)";
    for (UInt64 i = 1; i <= 6; ++i)
    {
        const String native = String(1, 0x2C) + varUInt(descriptor.size()) + descriptor + littleEndian(i, 64);
        ReadBufferFromString in(native);
        serialization->deserializeBinary(*column, in, FormatSettings{});
        const String int64 = String(1, 0x0A) + littleEndian(i, 8);
        ReadBufferFromString int64_in(int64);
        serialization->deserializeBinary(*column, int64_in, FormatSettings{});
    }
    const IColumn * columns[] = {column.get()};
    const size_t rows = column->size();

    State original(function);
    for (size_t row = 0; row < rows / 2; ++row)
        function->add(original.place(), columns, row, &original.arena);

    const String canonical = original.serialize();
    State reread(function);
    reread.deserialize(canonical);
    EXPECT_EQ(hexOf(reread.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(original.serialize(BinaryTypeWriterMode::SentioLegacyBridge)));

    /// Continue both with the same stream (the second half, then the whole column again).
    for (State * state : {&original, &reread})
    {
        for (size_t row = rows / 2; row < rows; ++row)
            function->add(state->place(), columns, row, &state->arena);
        for (size_t row = 0; row < rows; ++row)
            function->add(state->place(), columns, row, &state->arena);
    }
    EXPECT_EQ(hexOf(reread.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(original.serialize(BinaryTypeWriterMode::SentioLegacyBridge)));
    EXPECT_EQ(hexOf(reread.serialize()), hexOf(original.serialize()));

    /// Merging them into states that already hold other values gives the same result too.
    State merged_original(function);
    State merged_reread(function);
    for (State * state : {&merged_original, &merged_reread})
    {
        for (size_t row = 0; row < rows; row += 2)
            function->add(state->place(), columns, row, &state->arena);
    }
    function->merge(merged_original.place(), original.place(), &merged_original.arena);
    function->merge(merged_reread.place(), reread.place(), &merged_reread.arena);
    EXPECT_EQ(hexOf(merged_reread.serialize()), hexOf(merged_original.serialize()));
}
