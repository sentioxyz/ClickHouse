#include <gtest/gtest.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnDynamic.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Field.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDynamic.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesNumber.h>
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
#include <base/StringViewHash.h>

#include <fmt/format.h>

#include <algorithm>
#include <vector>

/// Aggregation states that keep `Dynamic` values (their arena or binary forms) keep their binary type headers. The plain deserialize
/// validates them as the Unknown origin while the state is read; an explicit SentioLegacyV0 origin normalizes them to the canonical
/// encoding, SentioLegacyBridge writes the legacy indexes again; the values in a state are nested in it, so the depth limit counts the
/// state; `topK` keeps its keys in the legacy projection, the hash domain of its alpha map. The bytes come from the binary type encoding
/// specification and from Root's fixture draft (the SentioWide v1 descriptor), not from the code under test. The `topK` states are laid
/// out as SpaceSaving::write writes them; they are codec fixtures, not states written by the Decimal512 fork, whose states are continued in
/// the final compatibility harness.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int ATTEMPT_TO_READ_AFTER_EOF;
extern const int BAD_ARGUMENTS;
extern const int CANNOT_READ_ALL_DATA;
extern const int INCORRECT_DATA;
extern const int SIZES_OF_ARRAYS_DONT_MATCH;
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

/// The binary Dynamic value of `Decimal512(76, 10)` with the raw value `raw`: the legacy header 0x39 P S, or the SentioWide v1 header.
String legacyDecimal(UInt64 raw)
{
    return String{0x39, 76, 10} + littleEndian(raw, 64);
}

String canonicalDecimal(UInt64 raw)
{
    const String descriptor = "SentioWide_v1_Decimal512(76,10)";
    return String(1, 0x2C) + varUInt(descriptor.size()) + descriptor + littleEndian(raw, 64);
}

/// The same with the raw value 1.
String legacyValue()
{
    return legacyDecimal(1);
}

String canonicalValue()
{
    return canonicalDecimal(1);
}

/// The binary Dynamic value of an `Int64`, the same in both encodings.
String int64Value(UInt64 value)
{
    return String(1, 0x0A) + littleEndian(value, 8);
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

/// A counter of a `topK` fixture: the raw value of its `Decimal512(76, 10)` key, its count and its error.
struct TopKCounter
{
    UInt64 raw;
    UInt64 count;
    UInt64 error;
};

/// The counters of a `topK` set as SpaceSaving::write writes them: their number, then the key (a string: the arena form of the value, with
/// the legacy or the canonical header), the count and the error of every counter.
String topKCounters(const std::vector<TopKCounter> & counters, bool legacy)
{
    String out = varUInt(counters.size());
    for (const auto & counter : counters)
    {
        const String key = arenaForm(legacy ? legacyDecimal(counter.raw) : canonicalDecimal(counter.raw));
        out += varUInt(key.size()) + key + varUInt(counter.count) + varUInt(counter.error);
    }
    return out;
}

/// The alpha map that follows the counters: its size and its values.
String topKAlpha(const std::vector<UInt64> & alpha)
{
    String out = varUInt(alpha.size());
    for (const auto value : alpha)
        out += varUInt(value);
    return out;
}

String topKState(const std::vector<TopKCounter> & counters, const std::vector<UInt64> & alpha, bool legacy)
{
    return topKCounters(counters, legacy) + topKAlpha(alpha);
}

/// `topK(1)` keeps 3 counters (the threshold times the load factor 3); its alpha map has 32 buckets (the power of two above 6 per counter).
constexpr size_t TOP_K_CAPACITY = 3;
constexpr size_t TOP_K_ALPHA_SIZE = 32;

/// A full `topK(1)` set: three counters D1, D2, D3 (raw values 1, 2, 3) in the order of SpaceSaving::write (by count minus error), and an
/// alpha map of the expected size whose buckets are all different and nonzero, so that a key that reads or updates another bucket changes
/// the state. With the legacy keys it is laid out as the Decimal512 fork lays out its sets.
const std::vector<TopKCounter> TOP_K_FIXTURE{{1, 500, 100}, {2, 400, 100}, {3, 300, 100}};

std::vector<UInt64> topKFixtureAlpha()
{
    std::vector<UInt64> alpha(TOP_K_ALPHA_SIZE);
    for (size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = 2 * i + 1;
    return alpha;
}

/// The bucket of the alpha map for a value: the hash of the set (SpaceSaving<std::string_view, StringViewHash>) of its arena form.
size_t topKBucket(const String & value)
{
    return StringViewHash{}(arenaForm(value)) & (TOP_K_ALPHA_SIZE - 1);
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
        const String value = canonicalValue();
        ReadBufferFromString in(value);
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

/// A full legacy `topK` set with a nonzero alpha map continues in the hash domain of that map, whether it is read from the legacy state
/// with the legacy origin or from the equivalent canonical state: the same stream (unit increments for `topK`, weights for `topKWeighted`)
/// and the same merge give the same keys, counts, errors and alpha map. The expected states follow from the rules of the set (SpaceSaving):
/// - a known key adds the weight to its count; a new key in a full set starts from the alpha value of its bucket: count = alpha + weight,
///   error = alpha;
/// - the set holds up to max(64, 2 * capacity) counters; at 64 it keeps the `capacity` greatest by count minus error and adds the count
///   minus error of every dropped counter to the alpha value of its bucket;
/// - merging a full set adds its greatest alpha value m2 to the count and the error of every counter, then the counters of that set: to a
///   known key its count and error minus m2, as a new key its count and error plus the greatest alpha value of this set; the alpha maps of
///   the same size are added up;
/// - the state has the `capacity` greatest counters by count minus error (then by count) and the whole alpha map.
TEST(AggregateStateBinaryTypeOrigin, TopKContinuesALegacyStateInItsHashDomain)
{
    const auto alpha = topKFixtureAlpha();
    const String legacy_state = topKState(TOP_K_FIXTURE, alpha, true);
    const String canonical_state = topKState(TOP_K_FIXTURE, alpha, false);

    /// X and Y are new keys, D2 is the counter of the fixture that the set drops. The legacy and the canonical forms of each fall into
    /// different buckets, so a set that read or updated the bucket of the canonical form would end with another state.
    const UInt64 x = 7;
    const UInt64 y = 8;
    const UInt64 d2 = 2;
    for (const UInt64 raw : {x, y, d2})
        ASSERT_NE(topKBucket(legacyDecimal(raw)), topKBucket(canonicalDecimal(raw))) << raw;

    /// The stream: X with the weight 1000, D3 with 250, the Int64 values 1..60 with 1 (the set reaches 64 counters and keeps X, D3 and D1),
    /// then Y with 2000 and D1 with 1.
    auto values = ColumnDynamic::create();
    auto weights = ColumnUInt64::create();
    auto append = [&](const String & value, UInt64 weight)
    {
        ReadBufferFromString in(value);
        DataTypeDynamic().getDefaultSerialization()->deserializeBinary(*values, in, FormatSettings{});
        weights->insertValue(weight);
    };
    append(canonicalDecimal(x), 1000);
    append(canonicalDecimal(3), 250);
    for (UInt64 i = 1; i <= 60; ++i)
        append(int64Value(i), 1);
    append(canonicalDecimal(y), 2000);
    append(canonicalDecimal(1), 1);
    const IColumn * columns[] = {values.get(), weights.get()};

    /// After the stream: Y, X and D3 have the greatest counts minus errors (2000, 1000, 450; D1 has 401). The alpha map got the dropped
    /// D2 (400 - 100) and the Int64 values (1 each); X started from the fixture map, Y from that one.
    auto continued_alpha = alpha;
    continued_alpha[topKBucket(legacyDecimal(d2))] += 400 - 100;
    for (UInt64 i = 1; i <= 60; ++i)
        continued_alpha[topKBucket(int64Value(i))] += 1;
    const UInt64 alpha_x = alpha[topKBucket(legacyDecimal(x))];
    const UInt64 alpha_y = continued_alpha[topKBucket(legacyDecimal(y))];
    const std::vector<TopKCounter> continued{{y, alpha_y + 2000, alpha_y}, {x, alpha_x + 1000, alpha_x}, {3, 300 + 250, 100}};

    /// Merged with the fixture set: Y and X get m2, D1 and D3 get the counts and errors of the fixture (D1: 1001 - 200 = 801, D3: 650),
    /// and D2 comes after them. The alpha maps are added up.
    const UInt64 m2 = *std::ranges::max_element(alpha);
    auto merged_alpha = continued_alpha;
    for (size_t i = 0; i < merged_alpha.size(); ++i)
        merged_alpha[i] += alpha[i];
    const std::vector<TopKCounter> merged{
        {y, alpha_y + 2000 + m2, alpha_y + m2}, {x, alpha_x + 1000 + m2, alpha_x + m2}, {1, 501 + 500, 100 + 100}};

    for (const bool weighted : {false, true})
    {
        auto function = weighted
            ? getFunction("topKWeighted", {dynamicType(), std::make_shared<DataTypeUInt64>()}, Array{Field(UInt64(1))})
            : getFunction("topK", {dynamicType()}, Array{Field(UInt64(1))});

        /// The plain deserialize refuses the legacy state as ambiguous.
        {
            State state(function);
            EXPECT_EQ(errorCodeOf([&] { state.deserialize(legacy_state); }), ErrorCodes::INCORRECT_DATA) << function->getName();
        }

        for (const bool from_legacy : {true, false})
        {
            const String where = fmt::format("{} from the {} state", function->getName(), from_legacy ? "legacy" : "canonical");
            auto read = [&](State & state)
            {
                if (from_legacy)
                    state.deserialize(legacy_state, &LEGACY);
                else
                    state.deserialize(canonical_state);
            };

            /// Both states are read into the same set.
            State state(function);
            read(state);
            EXPECT_EQ(hexOf(state.serialize()), hexOf(canonical_state)) << where;
            EXPECT_EQ(hexOf(state.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(legacy_state)) << where;

            for (size_t row = 0; row < values->size(); ++row)
            {
                if (weighted)
                {
                    function->add(state.place(), columns, row, &state.arena);
                }
                else
                {
                    for (UInt64 i = 0; i < weights->getElement(row); ++i)
                        function->add(state.place(), columns, row, &state.arena);
                }
            }
            EXPECT_EQ(hexOf(state.serialize()), hexOf(topKState(continued, continued_alpha, false))) << where;
            EXPECT_EQ(hexOf(state.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(topKState(continued, continued_alpha, true)))
                << where;

            State fixture(function);
            read(fixture);
            function->merge(state.place(), fixture.place(), &state.arena);
            EXPECT_EQ(hexOf(state.serialize()), hexOf(topKState(merged, merged_alpha, false))) << where;
            EXPECT_EQ(hexOf(state.serialize(BinaryTypeWriterMode::SentioLegacyBridge)), hexOf(topKState(merged, merged_alpha, true)))
                << where;

            /// The result is read from the projection: Y, a Decimal512 value.
            auto result = function->getResultType()->createColumn();
            function->insertResultInto(state.place(), *result, &state.arena);
            const auto & items = assert_cast<const ColumnDynamic &>(assert_cast<const ColumnArray &>(*result).getData());
            ASSERT_EQ(items.size(), 1U) << where;
            EXPECT_EQ(items.getTypeAt(0)->getTypeId(), TypeIndex::Decimal512) << where;
        }
    }
}

/// The boundaries of a read `topK` set, with small fixtures in both encodings: a set without counters skips its alpha map (it has no
/// errors) and stays uninitialized; more counters than the capacity are refused before any key is read; an alpha map smaller than expected
/// is refused, a larger one (written by old versions) is drained and the set starts with zeros; truncated data is refused; the alpha map is
/// streamed, so a huge declared size ends with the data instead of being allocated.
TEST(AggregateStateBinaryTypeOrigin, TopKStateBoundaries)
{
    auto function = getFunction("topK", {dynamicType()}, Array{Field(UInt64(1))});
    const auto alpha = topKFixtureAlpha();
    const String empty_set = varUInt(0) + varUInt(0);
    const String huge_alpha = varUInt(UInt64(1) << 62) + varUInt(1) + varUInt(2) + varUInt(3);

    for (const bool from_legacy : {true, false})
    {
        const String where = from_legacy ? "legacy" : "canonical";
        auto read = [&](const String & bytes)
        {
            State state(function);
            state.deserialize(bytes, from_legacy ? &LEGACY : nullptr);
            return state.serialize();
        };
        const String counters = topKCounters(TOP_K_FIXTURE, from_legacy);

        EXPECT_EQ(hexOf(read(varUInt(0) + topKAlpha(alpha))), hexOf(empty_set)) << where;
        EXPECT_EQ(hexOf(read(empty_set)), hexOf(empty_set)) << where;
        EXPECT_EQ(errorCodeOf([&] { read(varUInt(TOP_K_CAPACITY + 1)); }), ErrorCodes::TOO_LARGE_ARRAY_SIZE) << where;

        EXPECT_EQ(hexOf(read(counters + topKAlpha(alpha))), hexOf(topKState(TOP_K_FIXTURE, alpha, false))) << where;
        const std::vector<UInt64> smaller(alpha.begin(), alpha.begin() + TOP_K_ALPHA_SIZE / 2);
        EXPECT_EQ(errorCodeOf([&] { read(counters + topKAlpha(smaller)); }), ErrorCodes::SIZES_OF_ARRAYS_DONT_MATCH) << where;
        std::vector<UInt64> oversized(2 * TOP_K_ALPHA_SIZE);
        for (size_t i = 0; i < oversized.size(); ++i)
            oversized[i] = i + 1;
        const std::vector<UInt64> zeros(TOP_K_ALPHA_SIZE);
        EXPECT_EQ(hexOf(read(counters + topKAlpha(oversized))), hexOf(topKState(TOP_K_FIXTURE, zeros, false))) << where;

        const String full = counters + topKAlpha(alpha);
        EXPECT_EQ(errorCodeOf([&] { read(full.substr(0, full.size() - 1)); }), ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF) << where;
        const String key = arenaForm(from_legacy ? legacyDecimal(1) : canonicalDecimal(1));
        const String truncated_key = varUInt(1) + varUInt(key.size()) + key.substr(0, 10);
        EXPECT_EQ(errorCodeOf([&] { read(truncated_key); }), ErrorCodes::CANNOT_READ_ALL_DATA) << where;

        EXPECT_EQ(errorCodeOf([&] { read(counters + huge_alpha); }), ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF) << where;
        EXPECT_EQ(errorCodeOf([&] { read(varUInt(0) + huge_alpha); }), ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF) << where;
    }
}
