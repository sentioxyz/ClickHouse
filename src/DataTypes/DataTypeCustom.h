#pragma once

#include <memory>
#include <cstddef>
#include <Core/Types_fwd.h>
#include <DataTypes/Serializations/ISerialization.h>

namespace DB
{

class ReadBuffer;
class WriteBuffer;
struct FormatSettings;
class IColumn;

/** Allow to customize an existing data type and set a different name and/or text serialization/deserialization methods.
 * See use in IPv4 and IPv6 data types, and also in SimpleAggregateFunction.
  */
class IDataTypeCustomName
{
public:
    virtual ~IDataTypeCustomName() = default;

    virtual String getName() const = 0;
};

using DataTypeCustomNamePtr = std::unique_ptr<const IDataTypeCustomName>;

/** Describe a data type customization
 */
struct DataTypeCustomDesc
{
    /// Wraps the serialization that the type itself chooses for the settings of each request.
    using SerializationWrapper = SerializationPtr (*)(const SerializationPtr & nested);

    DataTypeCustomNamePtr name;
    SerializationPtr serialization;
    /// Instead of a fixed serialization: the type keeps choosing its serialization for the settings (for example the version of the
    /// `String` serialization), and this function wraps it.
    SerializationWrapper serialization_wrapper = nullptr;

    explicit DataTypeCustomDesc(
        DataTypeCustomNamePtr name_,
        SerializationPtr serialization_ = nullptr,
        SerializationWrapper serialization_wrapper_ = nullptr)
    : name(std::move(name_))
    , serialization(std::move(serialization_))
    , serialization_wrapper(serialization_wrapper_) {}
};

using DataTypeCustomDescPtr = std::unique_ptr<DataTypeCustomDesc>;

/** A simple implementation of IDataTypeCustomName
 */
class DataTypeCustomFixedName : public IDataTypeCustomName
{
private:
    String name;
public:
    explicit DataTypeCustomFixedName(String name_) : name(name_) {}
    String getName() const override { return name; }
};

}
