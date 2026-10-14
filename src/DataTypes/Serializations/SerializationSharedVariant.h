#pragma once

#include <DataTypes/Serializations/SerializationWrapper.h>

namespace DB
{

/// The serialization of the `Dynamic` shared variant: a `String` column of binary `Dynamic` values (a binary type header and the value).
/// It applies the binary type origin and writer mode of the settings (FormatSettings::binary, see SentioWideRecode.h) where these values
/// enter or leave memory in bulk. Values read with any origin except proven canonical stored data are validated (Unknown, also a declared
/// official origin) or normalized to the canonical encoding (SentioLegacyV0) before they are inserted or cached, so that every reader of
/// the stream (the column and its subcolumns) gets accepted values; values are written with the legacy indexes for the SentioLegacyBridge
/// writer mode. The origin and the mode come from the settings of each call, never from this pooled object. The streams and every other
/// method are the nested `String` serialization's.
class SerializationSharedVariant final : public SerializationWrapper
{
private:
    explicit SerializationSharedVariant(const SerializationPtr & nested_);

public:
    static UInt128 getHash(const SerializationPtr & nested_);
    static SerializationPtr create(const SerializationPtr & nested_);

    void serializeBinaryBulkWithMultipleStreams(
        const IColumn & column,
        size_t offset,
        size_t limit,
        SerializeBinaryBulkSettings & settings,
        SerializeBinaryBulkStatePtr & state) const override;

    void deserializeBinaryBulkWithMultipleStreams(
        IColumn & column,
        size_t limit,
        DeserializeBinaryBulkSettings & settings,
        DeserializeBinaryBulkStatePtr & state,
        SubstreamsCache * cache) const override;
};

}
