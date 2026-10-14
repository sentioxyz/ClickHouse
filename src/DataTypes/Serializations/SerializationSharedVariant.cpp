#include <DataTypes/Serializations/SerializationSharedVariant.h>

#include <Columns/ColumnString.h>
#include <DataTypes/SentioWideRecode.h>
#include <Formats/FormatSettings.h>
#include <Common/SipHash.h>
#include <Common/assert_cast.h>

namespace DB
{

SerializationSharedVariant::SerializationSharedVariant(const SerializationPtr & nested_)
    : SerializationWrapper(nested_)
{
}

UInt128 SerializationSharedVariant::getHash(const SerializationPtr & nested_)
{
    SipHash hash;
    hash.update("SharedVariant");
    hash.update(nested_->getHash());
    return hash.get128();
}

SerializationPtr SerializationSharedVariant::create(const SerializationPtr & nested_)
{
    if (!nested_->supportsPooling())
        return std::shared_ptr<ISerialization>(new SerializationSharedVariant(nested_));
    return ISerialization::pooled(getHash(nested_), [&] { return new SerializationSharedVariant(nested_); });
}

void SerializationSharedVariant::serializeBinaryBulkWithMultipleStreams(
    const IColumn & column,
    size_t offset,
    size_t limit,
    SerializeBinaryBulkSettings & settings,
    SerializeBinaryBulkStatePtr & state) const
{
    const auto mode = settings.format_settings ? settings.format_settings->binary.type_writer_mode : BinaryTypeWriterMode::SentioWideV1;
    if (mode == BinaryTypeWriterMode::SentioWideV1)
    {
        nested_serialization->serializeBinaryBulkWithMultipleStreams(column, offset, limit, settings, state);
        return;
    }

    /// An explicit bridge export: the rows are written with the legacy indexes (in memory they stay canonical).
    const auto & values = assert_cast<const ColumnString &>(column);
    const size_t end = limit == 0 || offset + limit > values.size() ? values.size() : offset + limit;
    auto written = recodeBinaryDynamicValuesForWrite(values, offset, end - offset, mode);
    nested_serialization->serializeBinaryBulkWithMultipleStreams(*written, 0, 0, settings, state);
}

void SerializationSharedVariant::deserializeBinaryBulkWithMultipleStreams(
    IColumn & column,
    size_t limit,
    DeserializeBinaryBulkSettings & settings,
    DeserializeBinaryBulkStatePtr & state,
    SubstreamsCache * cache) const
{
    /// Stored data whose stored origin is canonical was written in the canonical encoding.
    if (isProvenCanonicalStoredData(settings.format_settings))
    {
        nested_serialization->deserializeBinaryBulkWithMultipleStreams(column, limit, settings, state, cache);
        return;
    }

    /// Otherwise the values are opaque bytes of an undeclared, declared or legacy origin: read them without caching them, accept them
    /// (validate, and normalize the legacy encoding) and cache only the accepted values.
    if (insertDataFromSubstreamsCacheIfAny(cache, settings, column))
        return;

    auto raw = column.cloneEmpty();
    nested_serialization->deserializeBinaryBulkWithMultipleStreams(*raw, limit, settings, state, nullptr);
    const size_t prev_size = column.size();
    acceptBinaryDynamicValues(
        assert_cast<const ColumnString &>(*raw),
        0,
        getSettingsForAcceptingBinaryDynamicValues(settings.format_settings),
        assert_cast<ColumnString &>(column));
    addColumnWithNumReadRowsToSubstreamsCache(cache, settings.path, column.getPtr(), column.size() - prev_size);
}

}
