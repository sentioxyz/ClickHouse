#include <DataTypes/TypeNameIdentity.h>

#include <Core/DecimalFunctions.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/IDataType.h>
#include <Common/assert_cast.h>

namespace DB
{

namespace
{

/// The factory (createDecimal) selects the class of a decimal by its precision and refuses a scale greater than the precision.
template <typename T>
bool isDecimalIdentifiedByName(const IDataType & type, size_t min_precision)
{
    const auto & decimal = assert_cast<const DataTypeDecimal<T> &>(type);
    const size_t precision = decimal.getPrecision();
    return min_precision <= precision && precision <= DecimalUtils::max_precision<T> && decimal.getScale() <= precision;
}

bool isTypeItselfIdentifiedByName(const IDataType & type)
{
    switch (type.getTypeId())
    {
        case TypeIndex::Decimal32:
            return isDecimalIdentifiedByName<Decimal32>(type, 1);
        case TypeIndex::Decimal64:
            return isDecimalIdentifiedByName<Decimal64>(type, DecimalUtils::max_precision<Decimal32> + 1);
        case TypeIndex::Decimal128:
            return isDecimalIdentifiedByName<Decimal128>(type, DecimalUtils::max_precision<Decimal64> + 1);
        case TypeIndex::Decimal256:
            return isDecimalIdentifiedByName<Decimal256>(type, DecimalUtils::max_precision<Decimal128> + 1);
        case TypeIndex::Decimal512:
            return isDecimalIdentifiedByName<Decimal512>(type, DecimalUtils::max_precision<Decimal256> + 1);
        case TypeIndex::AggregateFunction:
        {
            /// The name of an aggregation state type has the names of its argument types.
            const auto & aggregate_function = assert_cast<const DataTypeAggregateFunction &>(type);
            for (const auto & argument : aggregate_function.getArgumentsDataTypes())
            {
                if (!isTypeIdentifiedByName(*argument))
                    return false;
            }
            return true;
        }
        default:
            return true;
    }
}

}

bool isTypeIdentifiedByName(const IDataType & type)
{
    if (!isTypeItselfIdentifiedByName(type))
        return false;
    /// forEachChild visits all nested types, not only the direct children.
    bool result = true;
    type.forEachChild([&result](const IDataType & child) { result = result && isTypeItselfIdentifiedByName(child); });
    return result;
}

String getTypeIdentityKey(const DataTypePtr & type)
{
    if (isTypeIdentifiedByName(*type))
        return type->getName();
    /// A binary encoding starts with a type index byte, so with the leading zero byte it never equals a type name.
    return '\0' + encodeDataType(type);
}

}
