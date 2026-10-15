#include <gtest/gtest.h>

#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Block.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypesBinaryEncoding.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <Formats/FormatFactory.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>
#include <Processors/Executors/StreamingFormatExecutor.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>

#include <utility>
#include <vector>

/// RowBinaryWithNamesAndTypes with the types in binary format, read by one reader for several inputs, as the executor of a stream does (the
/// entries of an asynchronous insert): every input has its own header, whose types the reader decodes again. A column that the table does
/// not have is skipped with the type of its own header (a native Decimal512(50, 2) has 64-byte values), not with a type of an earlier
/// header. A type of the header that is named like the type of its column but is another type (the native Decimal512(50, 2) is named like
/// Decimal256(50, 2), whose values have 32 bytes) is refused before any value is read. The payloads are built with the encoder and the
/// serializations of the types, so they are what a writer of the format produces.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int INCORRECT_DATA;
}

namespace
{

using HeaderColumns = std::vector<std::pair<String, DataTypePtr>>;

/// The header: the number of columns, their names, their types in the binary type encoding.
void writeHeader(const HeaderColumns & columns, WriteBuffer & out)
{
    writeVarUInt(columns.size(), out);
    for (const auto & [name, type] : columns)
        writeStringBinary(name, out);
    for (const auto & [name, type] : columns)
        encodeDataType(type, out);
}

void writeValue(const DataTypePtr & type, const Field & value, WriteBuffer & out)
{
    type->getDefaultSerialization()->serializeBinary(value, out, FormatSettings{});
}

DataTypePtr nativeDecimal512(UInt32 precision, UInt32 scale)
{
    return std::make_shared<DataTypeDecimal<Decimal512>>(precision, scale);
}

}

TEST(BinaryRowInputHeaderTypes, ReusedReaderTakesTheTypesOfEachHeader)
{
    tryRegisterFormats();
    const auto context = getContext().context;

    const auto decimal256 = DataTypeFactory::instance().get("Decimal(50, 2)");
    ASSERT_EQ(decimal256->getTypeId(), TypeIndex::Decimal256);
    const auto native512 = nativeDecimal512(50, 2);
    ASSERT_EQ(native512->getName(), decimal256->getName());
    const auto int32 = std::make_shared<DataTypeInt32>();

    const Block sample({
        ColumnWithTypeAndName(decimal256->createColumn(), decimal256, "x"),
        ColumnWithTypeAndName(int32->createColumn(), int32, "y"),
    });

    auto settings = getFormatSettings(context);
    settings.binary.decode_types_in_binary_format = true;
    settings.skip_unknown_fields = true;

    ReadBufferFromString no_input("");
    auto format = FormatFactory::instance().getInput("RowBinaryWithNamesAndTypes", no_input, sample, context, 8192, settings);
    StreamingFormatExecutor executor(sample, format);

    /// The types of the table: x = 123.45, y = 7.
    WriteBufferFromOwnString first;
    writeHeader({{"x", decimal256}, {"y", int32}}, first);
    writeValue(decimal256, DecimalField<Decimal256>(Decimal256(12345), 2), first);
    writeValue(int32, Int32(7), first);
    ReadBufferFromString first_input(first.str());
    EXPECT_EQ(executor.execute(first_input), 1u);

    /// A column z that the table does not have, a native Decimal512(50, 2) whose 64-byte value is skipped, then y = 8 and x = 1.5. A reader
    /// that kept the types of the first header would skip z as the 32-byte Decimal256 of its first column.
    WriteBufferFromOwnString second;
    writeHeader({{"z", native512}, {"y", int32}, {"x", decimal256}}, second);
    writeValue(native512, DecimalField<Decimal512>(Decimal512(99999), 2), second);
    writeValue(int32, Int32(8), second);
    writeValue(decimal256, DecimalField<Decimal256>(Decimal256(150), 2), second);
    ReadBufferFromString second_input(second.str());
    EXPECT_EQ(executor.execute(second_input), 1u);

    auto columns = executor.getResultColumns();
    ASSERT_EQ(columns.size(), 2u);
    const auto & x = assert_cast<const ColumnDecimal<Decimal256> &>(*columns[0]).getData();
    const auto & y = assert_cast<const ColumnInt32 &>(*columns[1]).getData();
    ASSERT_EQ(x.size(), 2u);
    ASSERT_EQ(y.size(), 2u);
    EXPECT_EQ(x[0], Decimal256(12345));
    EXPECT_EQ(x[1], Decimal256(150));
    EXPECT_EQ(y[0], 7);
    EXPECT_EQ(y[1], 8);

    /// x as the native Decimal512(50, 2): the name of the type of the column, another type.
    WriteBufferFromOwnString third;
    writeHeader({{"x", native512}, {"y", int32}}, third);
    writeValue(native512, DecimalField<Decimal512>(Decimal512(12345), 2), third);
    writeValue(int32, Int32(9), third);
    ReadBufferFromString third_input(third.str());
    try
    {
        executor.execute(third_input);
        ADD_FAILURE() << "read x of the native Decimal512(50, 2) as Decimal256(50, 2)";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA) << e.message();
        EXPECT_NE(e.message().find("is named Decimal(50, 2), as the type of the column, but it is another type"), std::string::npos)
            << e.message();
    }
}
