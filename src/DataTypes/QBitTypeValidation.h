#pragma once

#include <DataTypes/IDataType.h>

namespace DB
{

/// The constructor and binary type decoder share these element/shape/group checks. No nested type is materialized.
/// Returns the number of FixedString streams so a decoder can charge its budget before constructing the QBit type.
size_t validateQBitAndGetNumStreams(const DataTypePtr & element_type, size_t dimension, size_t stride);

}
