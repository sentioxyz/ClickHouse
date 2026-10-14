#pragma once

#include <DataTypes/IDataType_fwd.h>
#include <base/types.h>

namespace DB
{

class IDataType;

/// Whether the name of the type identifies it, so that structures that hold or look types up by name (the regular variants of `Dynamic`,
/// caches of serializations by type name, subcolumns named by a type) can do so for it. Every type created from its name is identified by
/// it. A decimal constructed natively with a precision that selects another class is not: a native `Decimal512(76, 10)` (64-byte values)
/// is named `Decimal(76, 10)`, the name of the 32-byte `Decimal256(76, 10)`, and a native decimal with a scale greater than its precision
/// has the name of no type at all. Neither is a type that has such a decimal inside (in containers, `Variant`, `JSON` typed paths,
/// `AggregateFunction` arguments). Such a type keeps its public name, but it is held and looked up by the type itself (in `Dynamic`: in
/// the shared variant, with its binary type descriptor) and is never resolved from its name.
bool isTypeIdentifiedByName(const IDataType & type);

/// Whether no other type has the name of the type: the name identifies it, and no type that its name does not identify has the same name.
/// A decimal of a class below `Decimal512` fails the second condition, as does a type that has one inside: a native decimal of a wider
/// class can have the same precision and scale (a native `Decimal512(76, 10)` is named like `Decimal256(76, 10)`). A hint that has only the
/// name of a type, such as the statistics of the shared variant of `Dynamic` (which can come from data written by another version), tells
/// the type created from the name only if the name is unambiguous.
bool isTypeNameUnambiguous(const IDataType & type);

/// A key that tells types apart where they are grouped or cached by name: the name of a type that it identifies, otherwise the binary
/// encoding of the type after a zero byte (which never equals a type name).
String getTypeIdentityKey(const DataTypePtr & type);

}
