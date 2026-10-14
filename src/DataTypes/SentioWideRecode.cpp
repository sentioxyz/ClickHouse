#include <DataTypes/SentioWideRecode.h>

#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnTuple.h>
#include <Columns/IColumn.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/SentioWideTypeEncoding.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Arena.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <Common/checkStackSize.h>
#include <Common/typeid_cast.h>

#include <algorithm>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

namespace
{

FormatSettings makeFormatSettings(BinaryTypeOrigin origin, BinaryTypeWriterMode mode)
{
    FormatSettings settings;
    settings.binary.type_origin = origin;
    settings.binary.type_writer_mode = mode;
    return settings;
}

/// The stored-data settings; their addresses identify stored data (isProvenCanonicalStoredData).
const FormatSettings & storedUnknownSettings()
{
    static const FormatSettings settings = makeFormatSettings(BinaryTypeOrigin::Unknown, BinaryTypeWriterMode::SentioWideV1);
    return settings;
}

const FormatSettings & storedLegacySettings()
{
    static const FormatSettings settings = makeFormatSettings(BinaryTypeOrigin::SentioLegacyV0, BinaryTypeWriterMode::SentioWideV1);
    return settings;
}

const FormatSettings & storedCanonicalSettings()
{
    static const FormatSettings settings = makeFormatSettings(BinaryTypeOrigin::OfficialCanonical, BinaryTypeWriterMode::SentioWideV1);
    return settings;
}

/// The settings to read and write the nested values of one value with: the caller's origin and limits, the nesting context of the value,
/// the given writer mode, and `JSON` never read or written as a string (these values are always in the binary form).
FormatSettings makeNestedSettings(const FormatSettings & settings, BinaryTypeWriterMode mode, BinaryTypeDecodingContext & context)
{
    FormatSettings result = settings;
    result.binary.read_json_as_string = false;
    result.binary.write_json_as_string = false;
    result.binary.type_writer_mode = mode;
    result.binary.type_decoding_context = &context;
    return result;
}

/// Whether the binary header of the type depends on the writer mode: it has a 512-bit type somewhere (also as an aggregate function
/// argument or a `JSON` typed path), which only the encoder knows completely.
bool headerDependsOnWriterMode(const DataTypePtr & type)
{
    WriteBufferFromOwnString canonical;
    encodeDataType(type, canonical, BinaryTypeWriterMode::SentioWideV1);
    WriteBufferFromOwnString legacy;
    encodeDataType(type, legacy, BinaryTypeWriterMode::SentioLegacyBridge);
    return canonical.stringView() != legacy.stringView();
}

/// Reads one value with `settings` (this validates every header, also the nested ones); if `out` is given, writes it for `mode`.
void readValue(std::string_view value, const FormatSettings & settings, BinaryTypeWriterMode mode, WriteBuffer * out)
{
    checkStackSize();

    /// The value is nested in the value whose reader passes its context, or is a value of its own. Its scope covers the header and the
    /// whole payload.
    BinaryTypeDecodingContext own_context(settings.binary.max_binary_type_complexity);
    BinaryTypeDecodingContext & context = settings.binary.type_decoding_context ? *settings.binary.type_decoding_context : own_context;
    BinaryValueNestingScope scope(context);

    ReadBufferFromMemory in(value.data(), value.size());
    auto type = decodeDataType(in, settings.binary.type_origin, context.max_complexity);
    const size_t header_size = in.count();

    if (out)
    {
        if (headerDependsOnWriterMode(type))
            encodeDataType(type, *out, mode);
        else
            out->write(value.data(), header_size);
    }

    if (isNothing(type))
    {
        if (!in.eof())
            throw Exception(ErrorCodes::INCORRECT_DATA, "A binary Dynamic value of type Nothing has {} bytes of payload", in.available());
        return;
    }

    if (!valuesMayHaveBinaryTypeHeaders(*type))
    {
        /// The payload is opaque here: ordinary String content is not type metadata.
        if (out)
            out->write(value.data() + header_size, value.size() - header_size);
        return;
    }

    /// The value has type headers of its own (`Dynamic`, `JSON`, containers of them, aggregation states that keep them): reading it with
    /// the origin, nested in this value, validates them, writing it again uses the writer mode.
    const FormatSettings nested_settings = makeNestedSettings(settings, mode, context);
    auto serialization = type->getDefaultSerialization();
    auto column = type->createColumn();
    serialization->deserializeBinary(*column, in, nested_settings);
    if (!in.eof())
        throw Exception(
            ErrorCodes::INCORRECT_DATA, "Unexpected {} bytes after a binary Dynamic value of type {}", in.available(), type->getName());
    if (out)
        serialization->serializeBinary(*column, 0, *out, nested_settings);
}

bool typeItselfMayHaveBinaryTypeHeaders(const IDataType & type)
{
    if (type.hasDynamicStructure())
        return true;
    if (const auto * aggregate_function = typeid_cast<const DataTypeAggregateFunction *>(&type))
    {
        const auto arguments = aggregate_function->getArgumentsDataTypes();
        return std::ranges::any_of(arguments, [](const DataTypePtr & argument) { return valuesMayHaveBinaryTypeHeaders(*argument); });
    }
    return false;
}

}

const FormatSettings & getFormatSettingsForStoredBinaryTypeOrigin(BinaryTypeOrigin origin)
{
    switch (origin)
    {
        case BinaryTypeOrigin::Unknown:
            return storedUnknownSettings();
        case BinaryTypeOrigin::SentioLegacyV0:
            return storedLegacySettings();
        case BinaryTypeOrigin::OfficialCanonical:
            return storedCanonicalSettings();
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected binary type origin {}", static_cast<UInt64>(origin));
}

bool isProvenCanonicalStoredData(const FormatSettings * settings)
{
    return settings == &storedCanonicalSettings();
}

const FormatSettings & getFormatSettingsForBinaryTypeOrigin(BinaryTypeOrigin origin)
{
    static const FormatSettings unknown = makeFormatSettings(BinaryTypeOrigin::Unknown, BinaryTypeWriterMode::SentioWideV1);
    static const FormatSettings legacy = makeFormatSettings(BinaryTypeOrigin::SentioLegacyV0, BinaryTypeWriterMode::SentioWideV1);
    static const FormatSettings canonical = makeFormatSettings(BinaryTypeOrigin::OfficialCanonical, BinaryTypeWriterMode::SentioWideV1);
    switch (origin)
    {
        case BinaryTypeOrigin::Unknown:
            return unknown;
        case BinaryTypeOrigin::SentioLegacyV0:
            return legacy;
        case BinaryTypeOrigin::OfficialCanonical:
            return canonical;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected binary type origin {}", static_cast<UInt64>(origin));
}

const FormatSettings & getFormatSettingsForBinaryTypeDecoding(const BinaryTypeDecoding & decoding, std::optional<FormatSettings> & storage)
{
    const auto & settings = getFormatSettingsForBinaryTypeOrigin(decoding.origin);
    if (!decoding.context)
        return settings;
    storage.emplace(settings);
    storage->binary.type_decoding_context = decoding.context;
    return *storage;
}

const FormatSettings & getFormatSettingsForBinaryTypeWriterMode(BinaryTypeWriterMode mode)
{
    static const FormatSettings canonical = makeFormatSettings(BinaryTypeOrigin::Unknown, BinaryTypeWriterMode::SentioWideV1);
    static const FormatSettings bridge = makeFormatSettings(BinaryTypeOrigin::Unknown, BinaryTypeWriterMode::SentioLegacyBridge);
    switch (mode)
    {
        case BinaryTypeWriterMode::SentioWideV1:
            return canonical;
        case BinaryTypeWriterMode::SentioLegacyBridge:
            return bridge;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected binary type writer mode {}", static_cast<UInt64>(mode));
}

const FormatSettings & getSettingsForAcceptingBinaryDynamicValues(const FormatSettings * settings)
{
    return settings ? *settings : storedUnknownSettings();
}

bool valuesMayHaveBinaryTypeHeaders(const IDataType & type)
{
    if (typeItselfMayHaveBinaryTypeHeaders(type))
        return true;
    /// forEachChild visits all nested types, not only the direct children.
    bool result = false;
    type.forEachChild([&result](const IDataType & child) { result = result || typeItselfMayHaveBinaryTypeHeaders(child); });
    return result;
}

const FormatSettings & withBinaryTypeDecodingContext(
    const FormatSettings & settings, const IDataType & type, BinaryTypeDecodingContext & context, std::optional<FormatSettings> & storage)
{
    if (settings.binary.type_decoding_context == &context || !valuesMayHaveBinaryTypeHeaders(type))
        return settings;
    if (!storage || storage->binary.type_decoding_context != &context)
    {
        storage.emplace(settings);
        storage->binary.type_decoding_context = &context;
    }
    return *storage;
}

void validateBinaryDynamicValue(std::string_view value, const FormatSettings & settings)
{
    readValue(value, settings, BinaryTypeWriterMode::SentioWideV1, nullptr);
}

void recodeBinaryDynamicValue(std::string_view value, const FormatSettings & settings, BinaryTypeWriterMode mode, WriteBuffer & out)
{
    readValue(value, settings, mode, &out);
}

bool binaryDynamicValueMayHaveSentioWideType(std::string_view value)
{
    return value.find(SENTIO_WIDE_PREFIX) != std::string_view::npos;
}

std::string_view acceptBinaryDynamicValue(std::string_view value, const FormatSettings & settings, String & storage)
{
    if (settings.binary.type_origin != BinaryTypeOrigin::SentioLegacyV0)
    {
        validateBinaryDynamicValue(value, settings);
        return value;
    }

    WriteBufferFromString out(storage);
    recodeBinaryDynamicValue(value, settings, BinaryTypeWriterMode::SentioWideV1, out);
    out.finalize();
    return storage;
}

void acceptBinaryDynamicValues(const ColumnString & values, size_t from, const FormatSettings & settings, ColumnString & to)
{
    to.reserve(to.size() + values.size() - from);
    String storage;
    for (size_t i = from; i < values.size(); ++i)
    {
        auto accepted = acceptBinaryDynamicValue(values.getDataAt(i), settings, storage);
        to.insertData(accepted.data(), accepted.size());
    }
}

std::string_view recodeBinaryDynamicValueForWrite(std::string_view value, BinaryTypeWriterMode mode, String & storage)
{
    if (mode == BinaryTypeWriterMode::SentioWideV1 || !binaryDynamicValueMayHaveSentioWideType(value))
        return value;

    /// Values in memory are canonical, so they are read with the official origin.
    WriteBufferFromString out(storage);
    recodeBinaryDynamicValue(value, storedCanonicalSettings(), mode, out);
    out.finalize();
    return storage;
}

MutableColumnPtr recodeBinaryDynamicValuesForWrite(const ColumnString & values, size_t from, size_t count, BinaryTypeWriterMode mode)
{
    auto result = ColumnString::create();
    result->reserve(count);
    String storage;
    for (size_t i = from; i < from + count; ++i)
    {
        auto written = recodeBinaryDynamicValueForWrite(values.getDataAt(i), mode, storage);
        result->insertData(written.data(), written.size());
    }
    return result;
}

void acceptObjectSharedDataRows(IColumn & shared_data, size_t from, const FormatSettings & settings)
{
    if (from >= shared_data.size())
        return;

    auto & shared_data_array = assert_cast<ColumnArray &>(shared_data);
    auto & values = assert_cast<ColumnString &>(assert_cast<ColumnTuple &>(shared_data_array.getData()).getColumn(1));
    const size_t first_value = shared_data_array.getOffsets()[static_cast<ssize_t>(from) - 1];

    if (settings.binary.type_origin != BinaryTypeOrigin::SentioLegacyV0)
    {
        for (size_t i = first_value; i < values.size(); ++i)
            validateBinaryDynamicValue(values.getDataAt(i), settings);
        return;
    }

    /// Paths keep their order and count; only the values are replaced by their canonical encoding.
    auto canonical = ColumnString::create();
    acceptBinaryDynamicValues(values, first_value, settings, *canonical);
    values.popBack(values.size() - first_value);
    values.insertRangeFrom(*canonical, 0, canonical->size());
}

ColumnPtr recodeObjectSharedDataForWrite(const ColumnPtr & shared_data, BinaryTypeWriterMode mode)
{
    if (mode == BinaryTypeWriterMode::SentioWideV1)
        return shared_data;

    const auto & shared_data_array = assert_cast<const ColumnArray &>(*shared_data);
    const auto & shared_data_tuple = assert_cast<const ColumnTuple &>(shared_data_array.getData());
    const auto & values = assert_cast<const ColumnString &>(shared_data_tuple.getColumn(1));
    ColumnPtr written = recodeBinaryDynamicValuesForWrite(values, 0, values.size(), mode);
    return ColumnArray::create(ColumnTuple::create(Columns{shared_data_tuple.getColumnPtr(0), written}), shared_data_array.getOffsetsPtr());
}

void acceptAggregationStateElements(
    const DataTypes & types, std::span<std::string_view> elements, const BinaryTypeDecoding & decoding, Arena & arena)
{
    if (elements.empty() || std::ranges::none_of(types, [](const DataTypePtr & type) { return valuesMayHaveBinaryTypeHeaders(*type); }))
        return;

    MutableColumns columns;
    columns.reserve(types.size());
    for (const auto & type : types)
        columns.push_back(type->createColumn());

    auto read_settings = IColumn::SerializationSettings::createForAggregationState();
    read_settings.binary_type_origin = decoding.origin;
    read_settings.binary_type_decoding_context = decoding.context;
    for (const auto element : elements)
    {
        ReadBufferFromMemory in(element.data(), element.size());
        for (auto & column : columns)
            column->deserializeAndInsertFromArena(in, &read_settings);
        if (!in.eof())
            throw Exception(ErrorCodes::INCORRECT_DATA, "Extra bytes ({}) found after deserializing aggregation state", in.available());
    }

    if (decoding.origin != BinaryTypeOrigin::SentioLegacyV0)
        return;

    /// The columns hold the values in memory, so serializing them again writes the canonical arena form.
    const auto write_settings = IColumn::SerializationSettings::createForAggregationState();
    for (size_t i = 0; i < elements.size(); ++i)
    {
        const char * begin = nullptr;
        std::string_view element;
        for (const auto & column : columns)
        {
            auto part = column->serializeValueIntoArena(i, arena, begin, &write_settings);
            element = std::string_view(part.data() - element.size(), element.size() + part.size());
        }
        elements[i] = element;
    }
}

std::vector<String> recodeAggregationStateElementsForWrite(
    const DataTypes & types, std::span<const std::string_view> elements, BinaryTypeWriterMode mode)
{
    if (mode == BinaryTypeWriterMode::SentioWideV1
        || std::ranges::none_of(types, [](const DataTypePtr & type) { return valuesMayHaveBinaryTypeHeaders(*type); }))
        return std::vector<String>(elements.begin(), elements.end());

    /// Elements in memory are canonical, so they are read with the official origin.
    return recodeAggregationStateElements(types, elements, BinaryTypeDecoding{.origin = BinaryTypeOrigin::OfficialCanonical}, mode);
}

std::vector<String> recodeAggregationStateElements(
    const DataTypes & types, std::span<const std::string_view> elements, const BinaryTypeDecoding & decoding, BinaryTypeWriterMode mode)
{
    MutableColumns columns;
    columns.reserve(types.size());
    for (const auto & type : types)
        columns.push_back(type->createColumn());

    auto read_settings = IColumn::SerializationSettings::createForAggregationState();
    read_settings.binary_type_origin = decoding.origin;
    read_settings.binary_type_decoding_context = decoding.context;
    for (const auto element : elements)
    {
        ReadBufferFromMemory in(element.data(), element.size());
        for (auto & column : columns)
            column->deserializeAndInsertFromArena(in, &read_settings);
        if (!in.eof())
            throw Exception(ErrorCodes::INCORRECT_DATA, "Extra bytes ({}) found after deserializing aggregation state", in.available());
    }

    auto write_settings = IColumn::SerializationSettings::createForAggregationState();
    write_settings.binary_type_writer_mode = mode;
    Arena arena;
    std::vector<String> result;
    result.reserve(elements.size());
    for (size_t i = 0; i < elements.size(); ++i)
    {
        const char * begin = nullptr;
        std::string_view element;
        for (const auto & column : columns)
        {
            auto part = column->serializeValueIntoArena(i, arena, begin, &write_settings);
            element = std::string_view(part.data() - element.size(), element.size() + part.size());
        }
        result.emplace_back(element);
    }
    return result;
}

}
