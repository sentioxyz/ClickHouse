#include <DataTypes/SentioWideTypeEncoding.h>

#include <Core/DecimalFunctions.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>

#include <fmt/format.h>

#include <optional>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

namespace
{

constexpr std::string_view UINT512_NAME = "UInt512";
constexpr std::string_view INT512_NAME = "Int512";
constexpr std::string_view DECIMAL512_PREFIX = "Decimal512(";

/// A decimal number of at most three digits, without sign, spaces or leading zeros ("0" itself is allowed).
std::optional<UInt32> parseSmallNumber(std::string_view text)
{
    if (text.empty() || text.size() > 3 || (text.size() > 1 && text.front() == '0'))
        return {};
    UInt32 value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9')
            return {};
        value = value * 10 + static_cast<UInt32>(c - '0');
    }
    return value;
}

}

bool isSentioWideType(const IDataType & type)
{
    if (type.hasCustomName())
        return false;
    const auto type_id = type.getTypeId();
    return type_id == TypeIndex::UInt512 || type_id == TypeIndex::Int512 || type_id == TypeIndex::Decimal512;
}

String makeSentioWideDescriptor(const IDataType & type)
{
    if (isSentioWideType(type))
    {
        switch (type.getTypeId())
        {
            case TypeIndex::UInt512:
                return fmt::format("{}{}", SENTIO_WIDE_V1_PREFIX, UINT512_NAME);
            case TypeIndex::Int512:
                return fmt::format("{}{}", SENTIO_WIDE_V1_PREFIX, INT512_NAME);
            case TypeIndex::Decimal512:
            {
                const auto & decimal_type = assert_cast<const DataTypeDecimal<Decimal512> &>(type);
                return fmt::format(
                    "{}{}{},{})", SENTIO_WIDE_V1_PREFIX, DECIMAL512_PREFIX, decimal_type.getPrecision(), decimal_type.getScale());
            }
            default:
                break;
        }
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Type {} has no SentioWide type descriptor", type.getName());
}

DataTypePtr parseSentioWideDescriptor(std::string_view descriptor)
{
    if (!descriptor.starts_with(SENTIO_WIDE_PREFIX))
        throw Exception(ErrorCodes::INCORRECT_DATA, "Not a SentioWide type descriptor: '{}'", descriptor);
    if (descriptor.size() > SENTIO_WIDE_DESCRIPTOR_MAX_SIZE)
        throw Exception(
            ErrorCodes::INCORRECT_DATA,
            "SentioWide type descriptor is too long: {} bytes, the maximum is {}",
            descriptor.size(),
            SENTIO_WIDE_DESCRIPTOR_MAX_SIZE);
    if (!descriptor.starts_with(SENTIO_WIDE_V1_PREFIX))
        throw Exception(
            ErrorCodes::INCORRECT_DATA,
            "Unsupported SentioWide type descriptor version: '{}' (supported: {})",
            descriptor,
            SENTIO_WIDE_V1_PREFIX);

    const std::string_view body = descriptor.substr(SENTIO_WIDE_V1_PREFIX.size());
    if (body == UINT512_NAME)
        return std::make_shared<DataTypeUInt512>();
    if (body == INT512_NAME)
        return std::make_shared<DataTypeInt512>();
    if (body.starts_with(DECIMAL512_PREFIX) && body.ends_with(')'))
    {
        const std::string_view arguments = body.substr(DECIMAL512_PREFIX.size(), body.size() - DECIMAL512_PREFIX.size() - 1);
        const size_t comma = arguments.find(',');
        if (comma != std::string_view::npos)
        {
            /// P and S are each limited by the native DataTypeDecimalBase constructor, S independently of P.
            const auto precision = parseSmallNumber(arguments.substr(0, comma));
            const auto scale = parseSmallNumber(arguments.substr(comma + 1));
            constexpr size_t max_decimal512_precision = DecimalUtils::max_precision<Decimal512>;
            if (precision && scale && *precision >= 1 && *precision <= max_decimal512_precision && *scale <= max_decimal512_precision)
                return std::make_shared<DataTypeDecimal<Decimal512>>(*precision, *scale);
        }
    }
    throw Exception(ErrorCodes::INCORRECT_DATA, "Malformed SentioWide type descriptor: '{}'", descriptor);
}

}
