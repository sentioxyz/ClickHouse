#include <gtest/gtest.h>

#include <Core/Block.h>
#include <Core/NamesAndTypes.h>
#include <Core/ProtocolDefines.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/DataTypesDecimal.h>
#include <DataTypes/DataTypesNumber.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Interpreters/Context.h>
#include <Interpreters/SetSerialization.h>
#include <Processors/QueryPlan/QueryPlanSerializationSettings.h>
#include <Processors/QueryPlan/Serialization.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/prepareReadingFromFormat.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>

#include <optional>

/// ReadFromFormatInfo::serialize writes every list of a format plan with the names of its types (and the types in storage of subcolumns),
/// and deserialize creates the types from these names. A decimal constructed with a precision of another decimal class, or with a scale
/// greater than its precision, has the name of another type or of no type: the plan is refused before anything is written. Types created
/// from their names keep their class and width through the round trip. The expected classes are the ones that the factory gives a name
/// (DataTypeFactory, createDecimal), not results of the code under test.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int INCORRECT_DATA;
}

namespace
{

/// Where a format plan holds the tested type: every list that serialize writes, and a subcolumn whose visible type is ordinary while its
/// type in storage holds the tested type.
enum class List
{
    SourceHeader,
    FormatHeader,
    ColumnsDescription,
    RequestedColumns,
    RequestedSubcolumnTypeInStorage,
    RequestedVirtualColumns,
    HivePartitionColumns,
};

constexpr List ALL_LISTS[] = {
    List::SourceHeader,
    List::FormatHeader,
    List::ColumnsDescription,
    List::RequestedColumns,
    List::RequestedSubcolumnTypeInStorage,
    List::RequestedVirtualColumns,
    List::HivePartitionColumns,
};

/// A format plan with `tested` in the list `where` and `ordinary` everywhere else.
ReadFromFormatInfo makeInfo(const DataTypePtr & ordinary, const DataTypePtr & tested, std::optional<List> where)
{
    auto pick = [&](List list) { return where == list ? tested : ordinary; };

    ReadFromFormatInfo info;
    info.source_header = Block({ColumnWithTypeAndName(pick(List::SourceHeader)->createColumn(), pick(List::SourceHeader), "x")});
    info.format_header = Block({ColumnWithTypeAndName(pick(List::FormatHeader)->createColumn(), pick(List::FormatHeader), "x")});
    info.columns_description = ColumnsDescription(NamesAndTypesList{{"x", pick(List::ColumnsDescription)}});
    info.requested_columns = NamesAndTypesList{{"x", pick(List::RequestedColumns)}};
    auto string_type = std::make_shared<DataTypeString>();
    auto tuple_type = std::make_shared<DataTypeTuple>(DataTypes{pick(List::RequestedSubcolumnTypeInStorage), string_type}, Names{"a", "b"});
    info.requested_columns.emplace_back("t", "b", tuple_type, string_type);
    info.requested_virtual_columns = NamesAndTypesList{{"_v", pick(List::RequestedVirtualColumns)}};
    info.hive_partition_columns_to_read_from_file_path = NamesAndTypesList{{"h", pick(List::HivePartitionColumns)}};
    return info;
}

void serializeInto(const ReadFromFormatInfo & info, WriteBuffer & out)
{
    SerializedSetsRegistry registry;
    IQueryPlanStep::Serialization ctx{out, registry};
    ctx.version = DBMS_QUERY_PLAN_SERIALIZATION_VERSION;
    info.serialize(ctx);
}

ReadFromFormatInfo deserialize(const String & bytes)
{
    ReadBufferFromString in(bytes);
    DeserializedSetsRegistry registry;
    QueryPlanSerializationSettings settings;
    SharedHeaders input_headers;
    SharedHeader output_header = std::make_shared<const Block>();
    ContextPtr context = getContext().context;
    IQueryPlanStep::Deserialization ctx{
        in, registry, {}, context, input_headers, output_header, settings, 0, DBMS_QUERY_PLAN_SERIALIZATION_VERSION, false};
    return ReadFromFormatInfo::deserialize(ctx);
}

void expectSameTypes(const NamesAndTypesList & expected, const NamesAndTypesList & actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    auto actual_it = actual.begin();
    for (const auto & column : expected)
    {
        EXPECT_EQ(column.name, actual_it->name);
        EXPECT_EQ(column.type->getTypeId(), actual_it->type->getTypeId()) << column.type->getName();
        EXPECT_TRUE(column.type->equals(*actual_it->type)) << column.type->getName() << " vs " << actual_it->type->getName();
        EXPECT_EQ(column.getTypeInStorage()->getTypeId(), actual_it->getTypeInStorage()->getTypeId());
        EXPECT_TRUE(column.getTypeInStorage()->equals(*actual_it->getTypeInStorage()))
            << column.getTypeInStorage()->getName() << " vs " << actual_it->getTypeInStorage()->getName();
        ++actual_it;
    }
}

DataTypePtr nativeDecimal512(UInt32 precision, UInt32 scale)
{
    return std::make_shared<DataTypeDecimal<Decimal512>>(precision, scale);
}

}

/// The loss that the refusal prevents: the name of a native Decimal512(50, 2) creates a Decimal256, and the name of a native decimal with
/// a scale greater than its precision creates nothing.
TEST(ReadFromFormatInfoTypeNames, NamesOfUnnameableTypes)
{
    const auto & factory = DataTypeFactory::instance();
    EXPECT_EQ(nativeDecimal512(50, 2)->getName(), "Decimal(50, 2)");
    EXPECT_EQ(factory.get(nativeDecimal512(50, 2)->getName())->getTypeId(), TypeIndex::Decimal256);
    EXPECT_EQ(factory.get(std::make_shared<DataTypeDecimal<Decimal64>>(5, 2)->getName())->getTypeId(), TypeIndex::Decimal32);
    EXPECT_ANY_THROW(factory.get(nativeDecimal512(80, 90)->getName()));
}

/// Types created from their names, Decimal256(50, 2) and Decimal512(80, 2) among them, keep their class through every list.
TEST(ReadFromFormatInfoTypeNames, RoundTripOfTypesCreatedFromNames)
{
    const auto & factory = DataTypeFactory::instance();
    const auto decimal256 = factory.get("Decimal(50, 2)");
    const auto decimal512 = factory.get("Decimal(80, 2)");
    ASSERT_EQ(decimal256->getTypeId(), TypeIndex::Decimal256);
    ASSERT_EQ(decimal512->getTypeId(), TypeIndex::Decimal512);

    for (auto where : ALL_LISTS)
    {
        const auto info = makeInfo(decimal256, decimal512, where);
        WriteBufferFromOwnString out;
        serializeInto(info, out);
        const auto restored = deserialize(out.str());

        expectSameTypes(info.source_header.getNamesAndTypesList(), restored.source_header.getNamesAndTypesList());
        expectSameTypes(info.format_header.getNamesAndTypesList(), restored.format_header.getNamesAndTypesList());
        expectSameTypes(info.columns_description.getAll(), restored.columns_description.getAll());
        expectSameTypes(info.requested_columns, restored.requested_columns);
        expectSameTypes(info.requested_virtual_columns, restored.requested_virtual_columns);
        expectSameTypes(info.hive_partition_columns_to_read_from_file_path, restored.hive_partition_columns_to_read_from_file_path);
    }
}

/// A native low-precision Decimal512, the same inside an Array, a native decimal with a scale greater than its precision and a native
/// Decimal64 with a precision of Decimal32 are refused in every list, also as the type in storage of a subcolumn, before a byte is written.
TEST(ReadFromFormatInfoTypeNames, RefusesUnnameableTypesInEveryList)
{
    const auto ordinary = std::make_shared<DataTypeInt32>();
    const DataTypes unnameable = {
        nativeDecimal512(50, 2),
        std::make_shared<DataTypeArray>(nativeDecimal512(50, 2)),
        nativeDecimal512(80, 90),
        std::make_shared<DataTypeDecimal<Decimal64>>(5, 2),
    };

    for (const auto & type : unnameable)
    {
        for (auto where : ALL_LISTS)
        {
            const auto info = makeInfo(ordinary, type, where);
            WriteBufferFromOwnString out;
            try
            {
                serializeInto(info, out);
                ADD_FAILURE() << "serialized " << type->getName() << " in list " << static_cast<int>(where);
            }
            catch (const Exception & e)
            {
                EXPECT_EQ(e.code(), ErrorCodes::INCORRECT_DATA) << e.message();
                EXPECT_NE(e.message().find("cannot be written by its name in a serialized plan of reading from a format"), std::string::npos)
                    << e.message();
            }
            EXPECT_TRUE(out.str().empty()) << type->getName() << " in list " << static_cast<int>(where);
        }
    }
}
