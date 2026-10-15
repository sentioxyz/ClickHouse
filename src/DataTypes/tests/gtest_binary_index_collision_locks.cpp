#include <gtest/gtest.h>

#include <Common/FieldBinaryEncoding.h>
#include <Core/Field.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeFunction.h>
#include <DataTypes/DataTypeSet.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_register.h>

#include <string>

/// Locks of the index assignments that the Decimal512 fork relies on, from the binary encoding specification
/// (docs/reference/data-types/data-types-binary-encoding.mdx), not from the code under test. A merge of a newer upstream version that
/// assigns any of the indexes below, or changes a documented one, fails here and needs a review of the collision before anything else:
/// - the binary type indexes of the documented types, and the indexes that upstream leaves unassigned: 0x33 and 0x35 (reserved), and
///   0x38 to 0x3A, which the fork used for its 512-bit types (0x37 is upstream's strided QBit). Decoded as the official encoding, an
///   unassigned index is an unknown type code;
/// - the fork's 512-bit types are written as Custom (0x2C) types whose names are descriptors with the reserved prefix `SentioWide_`, which
///   no registered type name has;
/// - the Field binary encoding (aggregate function parameters and other Field values), a separate format: the fork assigns 0x16, 0x17 and
///   0x18 to Decimal512, UInt512 and Int512 after upstream's last index 0x15. The indexes from 0x19 to 0xFD tested below are unknown;
///   0xFE and 0xFF are assigned (the negative and the positive infinity).

using namespace DB;

namespace DB::ErrorCodes
{
extern const int INCORRECT_DATA;
extern const int ATTEMPT_TO_READ_AFTER_EOF;
}

namespace
{

struct DocumentedIndex
{
    const char * type_name;
    UInt8 index;
};

/// The first byte of the binary encoding of each type, from the table of the specification.
constexpr DocumentedIndex DOCUMENTED_INDEXES[] = {
    {"Nothing", 0x00},
    {"UInt8", 0x01},
    {"UInt16", 0x02},
    {"UInt32", 0x03},
    {"UInt64", 0x04},
    {"UInt128", 0x05},
    {"UInt256", 0x06},
    {"Int8", 0x07},
    {"Int16", 0x08},
    {"Int32", 0x09},
    {"Int64", 0x0A},
    {"Int128", 0x0B},
    {"Int256", 0x0C},
    {"Float32", 0x0D},
    {"Float64", 0x0E},
    {"Date", 0x0F},
    {"Date32", 0x10},
    {"DateTime", 0x11},
    {"DateTime('UTC')", 0x12},
    {"DateTime64(3)", 0x13},
    {"DateTime64(3, 'UTC')", 0x14},
    {"String", 0x15},
    {"FixedString(4)", 0x16},
    {"Enum8('a' = 1)", 0x17},
    {"Enum16('a' = 1)", 0x18},
    {"Decimal(9, 2)", 0x19},
    {"Decimal(18, 2)", 0x1A},
    {"Decimal(38, 2)", 0x1B},
    {"Decimal(76, 2)", 0x1C},
    {"UUID", 0x1D},
    {"Array(UInt8)", 0x1E},
    {"Tuple(UInt8, String)", 0x1F},
    {"Tuple(a UInt8, b String)", 0x20},
    {"IntervalSecond", 0x22},
    {"Nullable(UInt8)", 0x23},
    {"AggregateFunction(sum, UInt64)", 0x25},
    {"LowCardinality(String)", 0x26},
    {"Map(String, UInt8)", 0x27},
    {"IPv4", 0x28},
    {"IPv6", 0x29},
    {"Variant(String, UInt8)", 0x2A},
    {"Dynamic", 0x2B},
    {"Ring", 0x2C},
    {"Bool", 0x2D},
    {"SimpleAggregateFunction(sum, UInt64)", 0x2E},
    {"Nested(a UInt8)", 0x2F},
    {"JSON", 0x30},
    {"BFloat16", 0x31},
    {"Time", 0x32},
    {"Time64(3)", 0x34},
    {"QBit(Float32, 8)", 0x36},
    /// The 512-bit types of the fork: Custom.
    {"UInt512", 0x2C},
    {"Int512", 0x2C},
    {"Decimal(80, 2)", 0x2C},
};

/// Indexes that upstream leaves unassigned, as of the version this fork is based on.
constexpr UInt8 UNASSIGNED_INDEXES[] = {0x33, 0x35, 0x38, 0x39, 0x3A, 0x3B, 0x40, 0x7F, 0xFE, 0xFF};

String encodedType(const String & type_name)
{
    return encodeDataType(DataTypeFactory::instance().get(type_name));
}

int codeOfDecoding(const String & bytes, BinaryTypeOrigin origin, String * message = nullptr)
{
    try
    {
        ReadBufferFromString in(bytes);
        decodeDataType(in, origin);
    }
    catch (const Exception & e)
    {
        if (message)
            *message = e.message();
        return e.code();
    }
    return 0;
}

String encodedField(const Field & field)
{
    WriteBufferFromOwnString out;
    encodeField(field, out);
    return out.str();
}

}

TEST(BinaryIndexCollisionLocks, DocumentedTypeIndexes)
{
    tryRegisterAggregateFunctions();
    for (const auto & [type_name, index] : DOCUMENTED_INDEXES)
    {
        const String encoded = encodedType(type_name);
        ASSERT_FALSE(encoded.empty()) << type_name;
        EXPECT_EQ(static_cast<UInt8>(encoded[0]), index) << type_name;
    }

    /// `Set` and `Function` are not created from names: the constructors of the types, as the encoding test builds them.
    EXPECT_EQ(static_cast<UInt8>(encodeDataType(std::make_shared<DataTypeSet>())[0]), 0x21);
    EXPECT_EQ(
        static_cast<UInt8>(
            encodeDataType(std::make_shared<DataTypeFunction>(DataTypes{std::make_shared<DataTypeInt8>()}, std::make_shared<DataTypeUInt8>()))[0]),
        0x24);

    /// The names of the 512-bit types are descriptors of version 1.
    for (const auto * type_name : {"UInt512", "Int512", "Decimal(80, 2)"})
        EXPECT_NE(encodedType(type_name).find("SentioWide_v1_"), std::string::npos) << type_name;
}

TEST(BinaryIndexCollisionLocks, UnassignedTypeIndexesStayUnknown)
{
    for (const auto index : UNASSIGNED_INDEXES)
    {
        String message;
        EXPECT_EQ(codeOfDecoding(String(1, static_cast<char>(index)), BinaryTypeOrigin::OfficialCanonical, &message), ErrorCodes::INCORRECT_DATA)
            << static_cast<int>(index);
        EXPECT_NE(message.find("Unknown type code"), std::string::npos) << static_cast<int>(index) << ": " << message;
    }

    /// `0x37` is the official strided `QBit` header; an absent payload is truncated input, not a legacy integer.
    EXPECT_EQ(codeOfDecoding(String(1, '\x37'), BinaryTypeOrigin::OfficialCanonical), ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF);
}

/// A Custom name with the prefix `SentioWide_` is always read as a descriptor, so a registered type with such a name could not be encoded.
/// Names that only start with other letters of it are not reserved.
TEST(BinaryIndexCollisionLocks, NoRegisteredTypeUsesTheReservedPrefix)
{
    for (const auto & name : DataTypeFactory::instance().getAllRegisteredNames())
        EXPECT_FALSE(name.starts_with("SentioWide_")) << name;
}

TEST(BinaryIndexCollisionLocks, FieldIndexes)
{
    /// Upstream's last Field index is 0x15 (AggregateFunctionState); the fork's 512-bit Fields follow it.
    EXPECT_EQ(static_cast<UInt8>(encodedField(Field(DecimalField<Decimal256>(Decimal256(5), 2)))[0]), 0x0B);
    EXPECT_EQ(static_cast<UInt8>(encodedField(Field(String("a")))[0]), 0x0C);
    const Field decimal512(DecimalField<Decimal512>(Decimal512(12345), 2));
    const Field uint512(UInt512(7));
    const Field int512(Int512(-7));
    EXPECT_EQ(static_cast<UInt8>(encodedField(decimal512)[0]), 0x16);
    EXPECT_EQ(static_cast<UInt8>(encodedField(uint512)[0]), 0x17);
    EXPECT_EQ(static_cast<UInt8>(encodedField(int512)[0]), 0x18);
    for (const auto & field : {decimal512, uint512, int512})
    {
        ReadBufferFromOwnString in(encodedField(field));
        EXPECT_EQ(decodeField(in), field);
        EXPECT_TRUE(in.eof());
    }

    for (const UInt8 index : {UInt8(0x19), UInt8(0x1A), UInt8(0x20), UInt8(0x7F), UInt8(0xFD)})
    {
        ReadBufferFromOwnString in(String(1, static_cast<char>(index)));
        try
        {
            decodeField(in);
            ADD_FAILURE() << "decoded the unassigned Field index " << static_cast<int>(index);
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA) << e.message();
            EXPECT_NE(e.message().find("Unknown Field type"), std::string::npos) << e.message();
        }
    }
}
