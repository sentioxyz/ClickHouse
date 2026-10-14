#include <DataTypes/SentioWideTypeEncoding.h>

#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

bool isSentioWideType(const IDataType &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "SentioWide type descriptors are not implemented yet");
}

String makeSentioWideDescriptor(const IDataType &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "SentioWide type descriptors are not implemented yet");
}

DataTypePtr parseSentioWideDescriptor(std::string_view)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "SentioWide type descriptors are not implemented yet");
}

}
