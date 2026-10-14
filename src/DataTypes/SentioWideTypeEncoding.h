#pragma once

#include <DataTypes/IDataType.h>

#include <string_view>

namespace DB
{

/// Binary type descriptors of the fork's 512-bit types (`UInt512`, `Int512`, `Decimal512`).
///
/// New binary type encodings write these types as `Custom` (0x2C) with a descriptor of a private namespace and version:
///     0x2C <var_uint_descriptor_size> <descriptor>
///     descriptor := "SentioWide_v1_UInt512" | "SentioWide_v1_Int512" | "SentioWide_v1_Decimal512(" P "," S ")"
/// P and S are decimal numbers without sign, leading zeros or spaces, 1 <= P <= 154 and 0 <= S <= 154. As in the native
/// `DataTypeDecimalBase` constructor (and the old 0x39 encoding), S is not limited by P: `Decimal512(1,154)` is a valid descriptor.
/// The descriptor always means the 64-byte type, whatever P is: `SentioWide_v1_Decimal512(76,10)` is `Decimal512`, not `Decimal256`.
/// The SQL names of the types (`UInt512`, `Int512`, `Decimal(P, S)`) and their 64-byte value encodings do not change.
///
/// A descriptor that starts with SENTIO_WIDE_PREFIX is parsed only by parseSentioWideDescriptor and never by `DataTypeFactory`;
/// an unknown version, an unknown type, malformed parameters and trailing bytes are rejected. Other `Custom` names keep the upstream
/// path (`DataTypeFactory`), including their length handling.

inline constexpr std::string_view SENTIO_WIDE_PREFIX = "SentioWide_";
inline constexpr std::string_view SENTIO_WIDE_V1_PREFIX = "SentioWide_v1_";

/// Maximum size of a SentioWide descriptor. The decoder reads the size, probes the first SENTIO_WIDE_PREFIX.size() bytes of the name,
/// and rejects a longer SentioWide descriptor before reading or allocating the rest of it.
inline constexpr size_t SENTIO_WIDE_DESCRIPTOR_MAX_SIZE = 256;

/// True for `UInt512`, `Int512` and `Decimal512` types that have no custom name.
bool isSentioWideType(const IDataType & type);

/// The v1 descriptor of a type for which isSentioWideType is true.
String makeSentioWideDescriptor(const IDataType & type);

/// Parse a v1 descriptor (the whole name, starting with SENTIO_WIDE_PREFIX) into the native data type.
/// Throws INCORRECT_DATA for anything that is not exactly one of the three v1 forms.
DataTypePtr parseSentioWideDescriptor(std::string_view descriptor);

}
