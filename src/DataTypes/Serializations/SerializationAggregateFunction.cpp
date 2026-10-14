#include <AggregateFunctions/IAggregateFunction.h>
#include <Columns/ColumnTuple.h>
#include <Columns/ColumnAggregateFunction.h>
#include <Core/Defines.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/SentioWideRecode.h>
#include <Common/SipHash.h>
#include <DataTypes/Serializations/SerializationAggregateFunction.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatSettings.h>
#include <IO/Operators.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <absl/container/inlined_vector.h>
#include <base/arithmeticOverflow.h>
#include <Common/AlignedBuffer.h>
#include <Common/Arena.h>
#include <Common/assert_cast.h>
#include <Common/typeid_cast.h>

#include <algorithm>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int TOO_LARGE_ARRAY_SIZE;
}


UInt128 SerializationAggregateFunction::getHash(const AggregateFunctionPtr & function_, const String & type_name_, size_t version_)
{
    SipHash hash;
    hash.update("AggregateFunction");
    auto state_type_name = function_->getStateType()->getName();
    hash.update(state_type_name.size());
    hash.update(state_type_name);
    hash.update(type_name_.size());
    hash.update(type_name_);
    hash.update(version_);
    hash.update(static_cast<UInt8>(function_->getStateVariant()));
    return hash.get128();
}

namespace
{

bool argumentsMayHaveBinaryTypeHeaders(const IAggregateFunction & function)
{
    const auto & arguments = function.getArgumentTypes();
    return std::ranges::any_of(arguments, [](const DataTypePtr & argument) { return valuesMayHaveBinaryTypeHeaders(*argument); });
}

/// The decoding of a state read with `settings`: their origin, and the nesting context that the settings carry (of the value the state is
/// nested in) or `own`.
BinaryTypeDecoding getBinaryTypeDecoding(const FormatSettings & settings, BinaryTypeDecodingContext & own)
{
    return BinaryTypeDecoding{
        .origin = settings.binary.type_origin,
        .context = settings.binary.type_decoding_context ? settings.binary.type_decoding_context : &own};
}

}

SerializationAggregateFunction::SerializationAggregateFunction(const AggregateFunctionPtr & function_, String type_name_, size_t version_)
    : function(function_)
    , type_name(std::move(type_name_))
    , version(version_)
    , state_may_have_binary_type_headers(argumentsMayHaveBinaryTypeHeaders(*function_))
{
}

String SerializationAggregateFunction::readStateWithBinaryTypeDecoding(
    std::string_view data, const BinaryTypeDecoding & decoding, std::optional<BinaryTypeWriterMode> write_mode) const
{
    Arena arena;
    AlignedBuffer place_buffer(function->sizeOfData(), function->alignOfData());
    AggregateDataPtr place = place_buffer.data();

    String result;
    function->create(place);
    try
    {
        ReadBufferFromMemory in(data.data(), data.size());
        function->deserializeWithBinaryTypeDecoding(place, in, version, &arena, decoding);
        /// The data is one whole state, as for deserializeFromString.
        const size_t trailing_bytes = in.available();
        if (trailing_bytes != 0)
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "AggregateFunction state for `{}` has {} trailing byte(s) after deserialization",
                function->getName(),
                trailing_bytes);
        if (write_mode)
        {
            WriteBufferFromString out(result);
            function->serializeWithBinaryTypeWriterMode(place, out, version, *write_mode);
            out.finalize();
        }
    }
    catch (...)
    {
        function->destroy(place);
        throw;
    }
    function->destroy(place);
    return result;
}

void SerializationAggregateFunction::serializeBinary(const Field & field, WriteBuffer & ostr, const FormatSettings & settings) const
{
    const AggregateFunctionStateData & state = field.safeGet<AggregateFunctionStateData>();
    if (settings.binary.type_writer_mode == BinaryTypeWriterMode::SentioWideV1 || !state_may_have_binary_type_headers)
    {
        writeBinary(state.data, ostr);
        return;
    }

    /// The state in the field is canonical, as in memory: it is written again for the writer mode.
    writeBinary(
        readStateWithBinaryTypeDecoding(
            state.data, BinaryTypeDecoding{.origin = BinaryTypeOrigin::OfficialCanonical}, settings.binary.type_writer_mode),
        ostr);
}

void SerializationAggregateFunction::deserializeBinary(Field & field, ReadBuffer & istr, const FormatSettings & settings) const
{
    field = AggregateFunctionStateData();
    AggregateFunctionStateData & s = field.safeGet<AggregateFunctionStateData>();
    readBinary(s.data, istr);
    s.name = type_name;

    if (!state_may_have_binary_type_headers)
        return;

    /// The field holds a canonical state, as memory does: the binary type headers in it are validated, and normalized for SentioLegacyV0.
    /// The state is a value whose scope covers the values in it.
    BinaryTypeDecodingContext own_context(settings.binary.max_binary_type_complexity);
    const auto decoding = getBinaryTypeDecoding(settings, own_context);
    BinaryValueNestingScope scope(*decoding.context);
    if (decoding.origin == BinaryTypeOrigin::SentioLegacyV0)
        s.data = readStateWithBinaryTypeDecoding(s.data, decoding, BinaryTypeWriterMode::SentioWideV1);
    else
        readStateWithBinaryTypeDecoding(s.data, decoding, std::nullopt);
}

void SerializationAggregateFunction::serializeBinary(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    function->serializeWithBinaryTypeWriterMode(
        assert_cast<const ColumnAggregateFunction &>(column).getData()[row_num], ostr, version, settings.binary.type_writer_mode);
}

void SerializationAggregateFunction::deserializeBinary(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    ColumnAggregateFunction & column_concrete = assert_cast<ColumnAggregateFunction &>(column);

    Arena & arena = column_concrete.createOrGetArena();
    size_t size_of_state = function->sizeOfData();
    AggregateDataPtr place = arena.alignedAlloc(size_of_state, function->alignOfData());

    /// The state is a value nested in the value whose reader passes its context, or a value of its own. Its scope covers the values in it.
    BinaryTypeDecodingContext own_context(settings.binary.max_binary_type_complexity);
    const auto decoding = getBinaryTypeDecoding(settings, own_context);
    BinaryValueNestingScope scope(*decoding.context);

    function->create(place);
    try
    {
        function->deserializeWithBinaryTypeDecoding(place, istr, version, &arena, decoding);
        /// Inside the guard: `push_back` can throw, and until it succeeds nothing owns `place`.
        column_concrete.getData().push_back(place);
    }
    catch (...)
    {
        function->destroy(place);
        throw;
    }
}

void SerializationAggregateFunction::serializeBinaryBulk(const IColumn & column, WriteBuffer & ostr, size_t offset, size_t limit) const
{
    const ColumnAggregateFunction & real_column = typeid_cast<const ColumnAggregateFunction &>(column);
    const ColumnAggregateFunction::Container & vec = real_column.getData();

    size_t end = vec.size();
    if (limit)
        end = std::min(end, offset + limit);

    function->serializeBatch(vec, offset, end, ostr, version);
}

void SerializationAggregateFunction::deserializeBinaryBulk(IColumn & column, ReadBuffer & istr, size_t limit, double /*avg_value_size_hint*/) const
{
    ColumnAggregateFunction & real_column = typeid_cast<ColumnAggregateFunction &>(column);
    ColumnAggregateFunction::Container & vec = real_column.getData();

    Arena & arena = real_column.createOrGetArena();
    real_column.set(function, version);

    size_t size_of_state = function->sizeOfData();
    size_t align_of_state = function->alignOfData();

    /// Adjust the size of state to make all states aligned in vector.
    size_t total_size_of_state = (size_of_state + align_of_state - 1) / align_of_state * align_of_state;

    /// The number of rows comes from the data, so it must not be turned into an allocation on its
    /// own: allocating all the states at once would multiply it by the size of a state, which comes
    /// from the data as well, and the product both overflows and lets a tiny block ask for an
    /// enormous allocation. The states are allocated in blocks bounded by the size of the array of
    /// pointers that is reserved for them anyway, so a block of rows of an ordinary state still
    /// takes a single allocation, and a state larger than that bound is allocated on its own.
    static constexpr size_t max_bytes_per_block = DEFAULT_INSERT_BLOCK_SIZE * sizeof(AggregateDataPtr);
    const size_t states_per_block = total_size_of_state == 0
        ? DEFAULT_INSERT_BLOCK_SIZE
        : std::max<size_t>(1, max_bytes_per_block / total_size_of_state);

    vec.reserve(vec.size() + std::min<size_t>(limit, DEFAULT_INSERT_BLOCK_SIZE));

    for (size_t remaining = limit; remaining != 0 && !istr.eof();)
    {
        const size_t states_in_block = std::min(remaining, states_per_block);

        size_t size_of_block = 0;
        if (common::mulOverflow(total_size_of_state, states_in_block, size_of_block))
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE,
                "Too large aggregate function states: {} states of {} bytes each", states_in_block, total_size_of_state);

        char * place = arena.alignedAlloc(size_of_block, align_of_state);

        /// Reads less than the whole block when the data ends, and then the loop stops on `eof`.
        function->createAndDeserializeBatch(vec, place, total_size_of_state, states_in_block, istr, version, &arena);

        remaining -= states_in_block;
    }
}

void SerializationAggregateFunction::serializeBinaryBulkWithMultipleStreams(
    const IColumn & column,
    size_t offset,
    size_t limit,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    const auto mode = settings.format_settings ? settings.format_settings->binary.type_writer_mode : BinaryTypeWriterMode::SentioWideV1;
    if (mode == BinaryTypeWriterMode::SentioWideV1 || !state_may_have_binary_type_headers)
    {
        ISerialization::serializeBinaryBulkWithMultipleStreams(column, offset, limit, settings, state);
        return;
    }

    /// An explicit bridge export: every state is written with the legacy indexes (in memory they stay canonical).
    settings.path.push_back(Substream::Regular);
    if (WriteBuffer * stream = settings.getter(settings.path))
    {
        const auto & vec = assert_cast<const ColumnAggregateFunction &>(column).getData();
        size_t end = vec.size();
        if (limit)
            end = std::min(end, offset + limit);
        for (size_t i = offset; i < end; ++i)
            function->serializeWithBinaryTypeWriterMode(vec[i], *stream, version, mode);
    }
    settings.path.pop_back();
}

void SerializationAggregateFunction::deserializeBinaryBulkWithMultipleStreams(
    IColumn & column,
    size_t limit,
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state,
    SubstreamsCache * cache) const
{
    /// Without format settings, or with an origin that needs no normalization and no type complexity limit (stored data, the native
    /// protocol between servers), the states are read as before: deserialize validates their binary type headers as the Unknown origin.
    const auto * format_settings = settings.format_settings;
    if (!state_may_have_binary_type_headers || !format_settings
        || (format_settings->binary.type_origin != BinaryTypeOrigin::SentioLegacyV0 && format_settings->binary.max_binary_type_complexity == 0))
    {
        ISerialization::deserializeBinaryBulkWithMultipleStreams(column, limit, settings, state, cache);
        return;
    }

    settings.path.push_back(Substream::Regular);
    if (insertDataFromSubstreamsCacheIfAny(cache, settings, column))
    {
        /// Data was inserted from substreams cache.
    }
    else if (ReadBuffer * stream = settings.getter(settings.path))
    {
        auto & real_column = assert_cast<ColumnAggregateFunction &>(column);
        const size_t prev_size = real_column.size();
        auto & vec = real_column.getData();
        Arena & arena = real_column.createOrGetArena();
        real_column.set(function, version);

        /// Every state is a value of its own, whose scope covers the values in it; the states of the rows are not added up.
        for (size_t i = 0; i < limit && !stream->eof(); ++i)
        {
            AggregateDataPtr place = arena.alignedAlloc(function->sizeOfData(), function->alignOfData());
            BinaryTypeDecodingContext context(format_settings->binary.max_binary_type_complexity);
            BinaryValueNestingScope scope(context);
            function->create(place);
            try
            {
                function->deserializeWithBinaryTypeDecoding(
                    place, *stream, version, &arena, BinaryTypeDecoding{.origin = format_settings->binary.type_origin, .context = &context});
                vec.push_back(place);
            }
            catch (...)
            {
                function->destroy(place);
                throw;
            }
        }

        addColumnWithNumReadRowsToSubstreamsCache(cache, settings.path, column.getPtr(), column.size() - prev_size);
        if (settings.update_avg_value_size_hint_callback)
            settings.update_avg_value_size_hint_callback(settings.path, column);
    }
    settings.path.pop_back();
}

static String serializeToString(
    const AggregateFunctionPtr & function, const IColumn & column, size_t row_num, size_t version, BinaryTypeWriterMode mode)
{
    WriteBufferFromOwnString buffer;
    function->serializeWithBinaryTypeWriterMode(assert_cast<const ColumnAggregateFunction &>(column).getData()[row_num], buffer, version, mode);
    return buffer.str();
}

static void deserializeFromString(
    const AggregateFunctionPtr & function, IColumn & column, const String & s, size_t version, const FormatSettings & settings)
{
    ColumnAggregateFunction & column_concrete = assert_cast<ColumnAggregateFunction &>(column);

    Arena & arena = column_concrete.createOrGetArena();
    size_t size_of_state = function->sizeOfData();
    AggregateDataPtr place = arena.alignedAlloc(size_of_state, function->alignOfData());

    /// The state is a value of its own, unless the settings carry the nesting context of an enclosing value; its scope covers the values
    /// in it.
    BinaryTypeDecodingContext own_context(settings.binary.max_binary_type_complexity);
    const auto decoding = getBinaryTypeDecoding(settings, own_context);
    BinaryValueNestingScope scope(*decoding.context);

    function->create(place);

    try
    {
        ReadBufferFromString istr(s);
        function->deserializeWithBinaryTypeDecoding(place, istr, version, &arena, decoding);

        const size_t trailing_bytes = istr.available();
        if (trailing_bytes != 0)
        {
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "AggregateFunction state for `{}` has {} trailing byte(s) after deserialization",
                function->getName(),
                trailing_bytes);
        }

        column_concrete.getData().push_back(place);
    }
    catch (...)
    {
        function->destroy(place);
        throw;
    }
}

static void deserializeFromValue(const AggregateFunctionPtr & function, IColumn & column, const String & value_str, const FormatSettings & settings)
{
    ColumnAggregateFunction & column_concrete = assert_cast<ColumnAggregateFunction &>(column);

    Arena & arena = column_concrete.createOrGetArena();
    size_t size_of_state = function->sizeOfData();
    AggregateDataPtr place = arena.alignedAlloc(size_of_state, function->alignOfData());

    function->create(place);

    try
    {
        // Get the argument types for the aggregate function
        const auto & argument_types = function->getArgumentTypes();
        if (argument_types.size() == 1)
        {
            // Single argument - parse the value directly
            auto temp_column = argument_types[0]->createColumn();
            ReadBufferFromString buf(value_str);
            argument_types[0]->getDefaultSerialization()->deserializeTextCSV(*temp_column, buf, settings);
            // Add the value to the aggregate state
            const IColumn * columns[] = {temp_column.get()};
            function->add(place, columns, 0, &arena);
        }
        else
        {
            // Multiple arguments - parse as tuple
            auto arg_types = DataTypeTuple(function->getArgumentTypes());
            auto tmp_column = arg_types.createColumn();
            ReadBufferFromString buf(value_str);
            arg_types.getDefaultSerialization()->deserializeWholeText(*tmp_column, buf, settings);
            ColumnRawPtrs columns_ptrs;
            for (const auto & col : assert_cast<ColumnTuple*>(tmp_column.get())->getColumns())
                columns_ptrs.push_back(col.get());
            function->add(place, columns_ptrs.data(), 0, &arena);
        }

        column_concrete.getData().push_back(place);
    }
    catch (...)
    {
        function->destroy(place);
        throw;
    }
}

static void deserializeFromArray(const AggregateFunctionPtr & function, IColumn & column, const String & array_str, const FormatSettings & settings)
{
    ColumnAggregateFunction & column_concrete = assert_cast<ColumnAggregateFunction &>(column);

    Arena & arena = column_concrete.createOrGetArena();
    size_t size_of_state = function->sizeOfData();
    AggregateDataPtr place = arena.alignedAlloc(size_of_state, function->alignOfData());

    function->create(place);

    try
    {
        // Get the argument types for the aggregate function
        const auto & argument_types = function->getArgumentTypes();
        const auto elem_type = argument_types.size() == 1 ? argument_types[0] : std::make_shared<DataTypeTuple>(argument_types);
        const auto tmp_column = elem_type->createColumn();
        const auto elem_serialization = elem_type->getDefaultSerialization();
        absl::InlinedVector<const IColumn *, 7> columns_ptrs;
        if (argument_types.size() == 1)
            columns_ptrs.push_back(tmp_column.get());
        else
            for (const auto & col : assert_cast<const ColumnTuple*>(tmp_column.get())->getColumns())
                columns_ptrs.push_back(col.get());
        // Parse the array - expect format like [val1,val2,val3] or [(val1a,val1b),(val2a,val2b)]
        ReadBufferFromString buf(array_str);
        assertChar('[', buf);
        bool first = true;
        while (!buf.eof())
        {
            skipWhitespaceIfAny(buf);
            if (*buf.position() == ']')
                break;
            if (!first)
            {
                assertChar(',', buf);
                skipWhitespaceIfAny(buf);
            }
            first = false;
            if (argument_types.size() == 1)
                elem_serialization->deserializeTextCSV(*tmp_column, buf, settings);  // CSV handles both ' and "
            else
                elem_serialization->deserializeTextQuoted(*tmp_column, buf, settings);  // Quoted for tuples
            function->add(place, columns_ptrs.data(), 0, &arena);
            tmp_column->popBack(1);
        }
        assertChar(']', buf);

        column_concrete.getData().push_back(place);
    }
    catch (...)
    {
        function->destroy(place);
        throw;
    }
}

SerializationPtr SerializationAggregateFunction::create(const AggregateFunctionPtr & function_, String type_name_, size_t version_)
{
    return ISerialization::pooled(getHash(function_, type_name_, version_), [&] { return new SerializationAggregateFunction(function_, std::move(type_name_), version_); });
}

void SerializationAggregateFunction::serializeText(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    writeString(serializeToString(function, column, row_num, version, settings.binary.type_writer_mode), ostr);
}


void SerializationAggregateFunction::serializeTextEscaped(
    const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    writeEscapedString(serializeToString(function, column, row_num, version, settings.binary.type_writer_mode), ostr);
}


void SerializationAggregateFunction::deserializeBasedOnInput(IColumn & column, const FormatSettings & settings, const String & s) const
{
    if (settings.aggregate_function_input_format == DB::FormatSettings::AggregateFunctionInputFormat::State)
    {
        deserializeFromString(function, column, s, version, settings);
    }
    else if (settings.aggregate_function_input_format == DB::FormatSettings::AggregateFunctionInputFormat::Value)
    {
        deserializeFromValue(function, column, s, settings);
    }
    else if (settings.aggregate_function_input_format == DB::FormatSettings::AggregateFunctionInputFormat::Array)
    {
        deserializeFromArray(function, column, s, settings);
    }
    else
    {
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "Invalid value for aggregate_function_input_format: '{}'. Expected '{}', '{}', or '{}'",
            settings.aggregate_function_input_format,
            DB::FormatSettings::AggregateFunctionInputFormat::State,
            DB::FormatSettings::AggregateFunctionInputFormat::Value,
            DB::FormatSettings::AggregateFunctionInputFormat::Array);
    }
}
void SerializationAggregateFunction::deserializeTextEscaped(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    String s;
    settings.tsv.crlf_end_of_line_input ? readEscapedStringCRLF(s, istr) : readEscapedString(s, istr);
    deserializeBasedOnInput(column, settings, s);
}


void SerializationAggregateFunction::serializeTextQuoted(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    auto str = serializeToString(function, column, row_num, version, settings.binary.type_writer_mode);
    if (settings.values.escape_quote_with_quote)
        writeQuotedStringPostgreSQL(str, ostr);
    else
        writeQuotedString(str, ostr);
}


void SerializationAggregateFunction::deserializeTextQuoted(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    String s;
    readQuotedStringWithSQLStyle(s, istr);
    deserializeBasedOnInput(column, settings, s);
}


void SerializationAggregateFunction::deserializeWholeText(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    String s;
    readStringUntilEOF(s, istr);
    deserializeBasedOnInput(column, settings, s);
}


void SerializationAggregateFunction::serializeTextJSON(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    writeJSONString(serializeToString(function, column, row_num, version, settings.binary.type_writer_mode), ostr, settings);
}


void SerializationAggregateFunction::deserializeTextJSON(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    String s;
    readJSONString(s, istr, settings.json);
    deserializeBasedOnInput(column, settings, s);
}


void SerializationAggregateFunction::serializeTextXML(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    writeXMLStringForTextElement(serializeToString(function, column, row_num, version, settings.binary.type_writer_mode), ostr);
}


void SerializationAggregateFunction::serializeTextCSV(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings & settings) const
{
    writeCSV(serializeToString(function, column, row_num, version, settings.binary.type_writer_mode), ostr);
}


void SerializationAggregateFunction::deserializeTextCSV(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const
{
    String s;
    readCSV(s, istr, settings.csv);
    deserializeBasedOnInput(column, settings, s);
}

size_t SerializationAggregateFunction::allocatedBytes() const
{
    return sizeof(*this) + type_name.capacity();
}

}
