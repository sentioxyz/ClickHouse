#pragma once

#include <AggregateFunctions/IAggregateFunction_fwd.h>

#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/Serializations/ISerialization.h>

#include <optional>

namespace DB
{

/// The binary type headers that states can keep (see IAggregateFunction::deserializeWithBinaryTypeDecoding) are read with the origin of the
/// format settings, every state being a value whose nesting scope covers the values in it (BinaryTypeDecodingContext), and written with
/// their writer mode. States of functions whose arguments cannot hold such headers are read and written as before.
class SerializationAggregateFunction final : public ISerialization
{
private:
    AggregateFunctionPtr function;
    String type_name;
    size_t version;
    /// Whether an argument type of the function can hold binary type headers (SentioWideRecode.h).
    bool state_may_have_binary_type_headers;

    SerializationAggregateFunction(const AggregateFunctionPtr & function_, String type_name_, size_t version_);

    /// Reads the state in `data` with `decoding` (validating its binary type headers) and, if `write_mode` is given, returns it written for
    /// that mode.
    String readStateWithBinaryTypeDecoding(
        std::string_view data, const BinaryTypeDecoding & decoding, std::optional<BinaryTypeWriterMode> write_mode) const;

public:
    static constexpr bool is_parametric = true;

    static UInt128 getHash(const AggregateFunctionPtr & function_, const String & type_name_, size_t version_);

    static SerializationPtr create(const AggregateFunctionPtr & function_, String type_name_, size_t version_);

    size_t allocatedBytes() const override;

    /// NOTE These two functions for serializing single values are incompatible with the functions below.
    void serializeBinary(const Field & field, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeBinary(Field & field, ReadBuffer & istr, const FormatSettings &) const override;

    void serializeBinary(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeBinary(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void serializeBinaryBulk(const IColumn & column, WriteBuffer & ostr, size_t offset, size_t limit) const override;
    void deserializeBinaryBulk(IColumn & column, ReadBuffer & istr, size_t limit, double avg_value_size_hint) const override;

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
    void serializeText(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void serializeTextEscaped(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextEscaped(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void serializeTextQuoted(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextQuoted(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void deserializeWholeText(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void deserializeBasedOnInput(IColumn & column, const FormatSettings & settings, const String & s) const;

    void serializeTextJSON(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextJSON(IColumn & column, ReadBuffer & istr, const FormatSettings &) const override;
    void serializeTextXML(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void serializeTextCSV(const IColumn & column, size_t row_num, WriteBuffer & ostr, const FormatSettings &) const override;
    void deserializeTextCSV(IColumn & column, ReadBuffer & istr, const FormatSettings & settings) const override;
};

}
