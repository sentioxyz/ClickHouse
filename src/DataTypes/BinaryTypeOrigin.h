#pragma once

#include <cstddef>
#include <cstdint>

namespace DB
{

/// The origin of a binary type encoding decides what the indexes 0x37, 0x38, 0x39 and 0x3A mean: the Decimal512 fork of 26.3 LTS wrote
/// its 512-bit types as 0x37/0x38/0x39, upstream 26.8 writes strided `QBit` as 0x37. The origin comes from a declared input setting or
/// from stored table metadata; it is never guessed from the payload, the server version or whether a decode succeeds.
enum class BinaryTypeOrigin : uint8_t
{
    /// Undeclared: the legacy indexes 0x37/0x38/0x39 are rejected as ambiguous; self-describing encodings (including the `Custom`
    /// SentioWide descriptors, see SentioWideTypeEncoding.h) are decoded.
    Unknown = 0,
    /// Written by the Decimal512 fork with its legacy indexes: 0x37 `UInt512`, 0x38 `Int512`, 0x39 `Decimal512(P, S)`; 0x3A is refused.
    SentioLegacyV0 = 1,
    /// The upstream meaning of every index (0x37 strided `QBit`, subject to the strided `QBit` guard); never the legacy indexes.
    OfficialCanonical = 2,
};

/// How the 512-bit types are written.
enum class BinaryTypeWriterMode : uint8_t
{
    /// `Custom` (0x2C) with the SentioWide v1 descriptors (see SentioWideTypeEncoding.h).
    SentioWideV1 = 0,
    /// The legacy indexes 0x37/0x38/0x39, only for an explicit offline export that must stay readable by the old fork.
    SentioLegacyBridge = 1,
};

/// The nesting of the binary values being decoded. A value whose payload holds other values with binary type headers (a `Dynamic` value,
/// a `JSON` value, an aggregation state that keeps such values) is decoded inside a nesting scope (BinaryValueNestingScope) that covers its
/// header and its whole payload, so that the values nested in it are one level deeper. Only the depth of the active scopes is bounded, by
/// the type complexity limit: sibling values (the elements of an array, the paths of a `JSON` value, the elements of an aggregation state,
/// the rows of a batch) enter and leave their scopes one after another and are never added up, and the sizes of the headers of the
/// enclosing values are not added up either. Every binary type header keeps its own bound by the same limit (decodeDataType), and the
/// ordinary byte, row, size, memory and stack limits bound the total work. The context is passed explicitly (FormatSettings::binary.
/// type_decoding_context, IColumn::SerializationSettings, BinaryTypeDecoding), never through global or thread-local state, and is never
/// stored in a serialization, a deserialization state, a format or an aggregation state.
struct BinaryTypeDecodingContext
{
    explicit BinaryTypeDecodingContext(size_t max_complexity_) : max_complexity(max_complexity_) {}

    /// The type complexity limit of every header and the limit of the nesting depth, or 0 for unlimited (stored data, aggregation states
    /// and background operations, as before).
    const size_t max_complexity;
    /// The number of active nesting scopes.
    size_t depth = 0;
};

/// Enters one nesting level of the context for the header and the whole payload of one value, and leaves it when destroyed, also on an
/// exception. Throws INCORRECT_DATA if the depth would exceed the limit.
class BinaryValueNestingScope
{
public:
    explicit BinaryValueNestingScope(BinaryTypeDecodingContext & context_);
    ~BinaryValueNestingScope();

    BinaryValueNestingScope(const BinaryValueNestingScope &) = delete;
    BinaryValueNestingScope & operator=(const BinaryValueNestingScope &) = delete;

private:
    BinaryTypeDecodingContext & context;
};

/// How the binary type headers in a serialized value are read where no FormatSettings are passed (aggregation states, arena forms): their
/// origin, and the nesting context of the value they belong to. A reader that receives a context passes it on to the readers of the nested
/// values; without one, the value is decoded without a type complexity limit, as stored data and aggregation states always were. Passed by
/// reference for one call, never stored.
struct BinaryTypeDecoding
{
    BinaryTypeOrigin origin = BinaryTypeOrigin::Unknown;
    BinaryTypeDecodingContext * context = nullptr;
};

}
