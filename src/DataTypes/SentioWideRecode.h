#pragma once

#include <Columns/IColumn_fwd.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/IDataType_fwd.h>
#include <base/types.h>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace DB
{

class Arena;
class ColumnString;
class WriteBuffer;
struct FormatSettings;

/// Binary type headers inside values. A binary `Dynamic` value - a binary type header followed by the value in the binary serialization
/// of that type - is a value of the `Dynamic` shared variant and of `JSON` shared data, and it is the payload of the arena form of a
/// `Dynamic` value. The bulk serializations copy these as opaque bytes, and aggregation states keep the arena or binary forms of their
/// argument values. The functions below apply the binary type origin (FormatSettings::binary.type_origin) and writer mode
/// (FormatSettings::binary.type_writer_mode) where such values enter or leave memory:
///  - accepting values read with the Unknown origin (or a declared official one) validates every type header, also the headers nested in
///    a value whose type has them (`Dynamic`, `JSON`, containers of them, aggregation states of functions with such arguments): a legacy
///    index of the Decimal512 fork is refused as ambiguous, and the bytes are kept;
///  - accepting values read with the SentioLegacyV0 origin validates them in the same way and writes the 512-bit types again with the
///    SentioWide v1 descriptors;
///  - values written with the SentioLegacyBridge writer mode get the legacy indexes back (an explicit offline export).
/// So memory only holds the canonical encoding, which never uses the indexes 0x37, 0x38, 0x39 (strided `QBit` is disabled), and decodes
/// the same with the Unknown and the OfficialCanonical origin. A header without a 512-bit type keeps its bytes. The payload of a type
/// without nested headers is never parsed: ordinary `String` content is not type metadata. `Nothing` (NULL) has no payload.
/// Every value is decoded in a nesting scope of its own (BinaryTypeDecodingContext): its header and every header nested in its payload are
/// bounded each on its own by the type complexity limit, the depth of nested values by the same limit and by checkStackSize, and the
/// values of a batch, of an array or of a state are siblings that are not added up.

/// The format settings that readers of stored data use for the origin stored with the data. These are static objects, so pointers to them
/// stay valid for deferred deserialization. Nothing else uses these objects: isProvenCanonicalStoredData relies on that.
const FormatSettings & getFormatSettingsForStoredBinaryTypeOrigin(BinaryTypeOrigin origin);

/// True only for the settings of stored data whose stored origin is OfficialCanonical: such data was written in the canonical encoding,
/// so its values are accepted without validation. A format setting that declares the official origin is not such a proof.
bool isProvenCanonicalStoredData(const FormatSettings * settings);

/// Static default settings with the origin, for the values of aggregation states and arena forms (no type complexity limit, as for
/// stored data). They are not a proof of anything.
const FormatSettings & getFormatSettingsForBinaryTypeOrigin(BinaryTypeOrigin origin);

/// The settings to read values of aggregation states and arena forms with: those of the origin, and, if the decoding carries the nesting
/// context of an enclosing value, a copy of them carrying it (constructed in `storage`).
const FormatSettings & getFormatSettingsForBinaryTypeDecoding(const BinaryTypeDecoding & decoding, std::optional<FormatSettings> & storage);

/// Static default settings with the writer mode.
const FormatSettings & getFormatSettingsForBinaryTypeWriterMode(BinaryTypeWriterMode mode);

/// The settings to accept values read with: the given ones, or the stored Unknown origin when there are none.
const FormatSettings & getSettingsForAcceptingBinaryDynamicValues(const FormatSettings * settings);

/// Whether the serialized values of the type (binary, arena and aggregation state forms) can hold binary type headers: the type or a type
/// nested in it has dynamic structure (`Dynamic`, `JSON`), or is an `AggregateFunction` with such an argument type. `hasDynamicStructure`
/// alone is not enough: an aggregation state of `groupArray(Dynamic)` keeps headers, while `AggregateFunction` has no dynamic structure.
bool valuesMayHaveBinaryTypeHeaders(const IDataType & type);

/// The settings to read the payload of a value of `type` with, inside the nesting scope of the value in `context`: if the payload can hold
/// nested values and the settings do not carry this context yet, a copy of them that does (constructed in `storage`); otherwise the
/// settings themselves.
const FormatSettings & withBinaryTypeDecodingContext(
    const FormatSettings & settings, const IDataType & type, BinaryTypeDecodingContext & context, std::optional<FormatSettings> & storage);

/// Validates one binary `Dynamic` value read with `settings`; throws INCORRECT_DATA for an ambiguous or malformed type header, also a
/// nested one, and for trailing bytes.
void validateBinaryDynamicValue(std::string_view value, const FormatSettings & settings);

/// Writes one value read with `settings` in the encoding of `mode` (validating it as validateBinaryDynamicValue does).
void recodeBinaryDynamicValue(std::string_view value, const FormatSettings & settings, BinaryTypeWriterMode mode, WriteBuffer & out);

/// Whether a value in the canonical encoding can differ from its legacy encoding: only if it holds a SentioWide descriptor, also in a
/// nested header (a byte search for the descriptor namespace, only to skip values that cannot change).
bool binaryDynamicValueMayHaveSentioWideType(std::string_view value);

/// Accepts one value read with `settings`: the value itself after validation, or for SentioLegacyV0 its canonical encoding (written into
/// `storage`).
std::string_view acceptBinaryDynamicValue(std::string_view value, const FormatSettings & settings, String & storage);

/// Accepts the values [from, values.size()) read with `settings` and appends them to `to`.
void acceptBinaryDynamicValues(const ColumnString & values, size_t from, const FormatSettings & settings, ColumnString & to);

/// One canonical (in-memory) value written for `mode`: the value itself, or its encoding for the mode (written into `storage`).
std::string_view recodeBinaryDynamicValueForWrite(std::string_view value, BinaryTypeWriterMode mode, String & storage);

/// The values [from, from + count) of a column of canonical values written for `mode` (a new column; a copy for SentioWideV1).
MutableColumnPtr recodeBinaryDynamicValuesForWrite(const ColumnString & values, size_t from, size_t count, BinaryTypeWriterMode mode);

/// The same for the rows [from, shared_data.size()) of a `JSON` shared data column (Array(Tuple(paths String, values String))): their values
/// are accepted in place (validated, and for SentioLegacyV0 replaced by their canonical encoding).
void acceptObjectSharedDataRows(IColumn & shared_data, size_t from, const FormatSettings & settings);

/// A `JSON` shared data column with its values written for `mode`; the same column for SentioWideV1.
ColumnPtr recodeObjectSharedDataForWrite(const ColumnPtr & shared_data, BinaryTypeWriterMode mode);

/// The hash APIs that hash the bytes of binary `Dynamic` values (the range, weak and fast hashes of the columns of `Dynamic` and `JSON`)
/// hash them as the Decimal512 fork had them in memory, with the legacy indexes of its 512-bit types (SentioLegacyBridge): the same data
/// keeps the hash that the fork computed, which insert deduplication persisted in the IDs of the blocks. Only values that hold a SentioWide
/// descriptor are written differently; everything else that these APIs hash (the layout, the order, the sizes) stays as it is.

/// A column of canonical binary `Dynamic` values for a hash that reads only its rows [from, from + count): the column itself if none of them
/// holds a SentioWide descriptor, otherwise a column of the same size whose rows [from, from + count) are written with SentioLegacyBridge
/// and whose other rows, which that hash does not read, are empty. So the cost follows the rows that are hashed, not the whole column.
ColumnPtr getBinaryDynamicValuesForHashing(const ColumnPtr & values, size_t from, size_t count);

/// The same for the rows [from, from + count) of a `JSON` shared data column (Array(Tuple(paths String, values String))): only the values of
/// these rows are written again.
ColumnPtr getObjectSharedDataForHashing(const ColumnPtr & shared_data, size_t from, size_t count);

/// Elements of aggregation states: one value of each of `types` serialized by IColumn::serializeValueIntoArena with the aggregation state
/// settings, one after another (the arena form that `groupArray`, `groupUniqArray`, `groupArrayIntersect`, `topK` and `-Distinct` keep).
/// Accepts elements read with `decoding`: every element is deserialized with its origin, nested in the value the state belongs to (the
/// elements are siblings), which validates all its type headers and refuses trailing bytes; for SentioLegacyV0 every element is replaced
/// by its canonical arena form, allocated in `arena`. Elements of types without binary type headers are not touched.
void acceptAggregationStateElements(
    const DataTypes & types, std::span<std::string_view> elements, const BinaryTypeDecoding & decoding, Arena & arena);

/// The arena forms of canonical (in-memory) elements written for `mode`, in the same order.
std::vector<String> recodeAggregationStateElementsForWrite(
    const DataTypes & types, std::span<const std::string_view> elements, BinaryTypeWriterMode mode);

/// The arena forms of elements read with `decoding` (validated, and refusing trailing bytes) written for `mode`, in the same order.
std::vector<String> recodeAggregationStateElements(
    const DataTypes & types, std::span<const std::string_view> elements, const BinaryTypeDecoding & decoding, BinaryTypeWriterMode mode);

/// The hash that the Decimal512 fork placed a key of a hash table of an aggregation state with (`groupUniqArray` and the like keep the arena
/// forms of values, one of each of `types`, as keys): StringViewHash of the key as the fork had it, with the legacy indexes of its 512-bit
/// types. Such a table keeps the canonical key and places it with this hash, so that its layout, and so the order of its elements, is that
/// of the fork. A key without a SentioWide descriptor is that key itself. Any other key is read once (it is canonical) and written for both
/// writer modes: only if they differ, the key holds a 512-bit type and its SentioLegacyBridge form is the fork's key; otherwise the bytes of
/// the namespace are ordinary content, and the key is hashed as it is.
size_t hashArenaKeyInLegacyDomain(std::string_view key, const DataTypes & types);

}
