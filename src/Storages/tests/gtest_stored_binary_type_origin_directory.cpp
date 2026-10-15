#include <gtest/gtest.h>

#include <DataTypes/BinaryTypeOrigin.h>
#include <DataTypes/DataTypesNumber.h>
#include <Disks/tests/gtest_disk.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteSettings.h>
#include <Interpreters/Context.h>
#include <Interpreters/StorageID.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/ConstraintsDescription.h>
#include <Storages/StorageLog.h>
#include <Storages/StorageStripeLog.h>
#include <Storages/StoredBinaryTypeOrigin.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>

#include <functional>
#include <memory>

/// The table record of a table that keeps its data in one directory of one disk (Log, TinyLog, StripeLog). An original CREATE requires
/// that the directory does not exist on the disk of the table, with or without a UUID, and leaves what is there byte for byte when it
/// refuses; a fresh directory of a table with a UUID gets the record. ATTACH loads the record and never writes one. The registration states
/// the origin of a table without any record only: an explicit Unknown, a record of another table or of the other kind, and a table without
/// a UUID are refused. The expected record texts come from StoredBinaryTypeOriginRecord, whose exact text gtest_stored_binary_type_origin
/// pins.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int LOGICAL_ERROR;
extern const int TABLE_ALREADY_EXISTS;
}

namespace
{

UUID uuidOf(const String & text)
{
    ReadBufferFromString in(text);
    UUID uuid;
    readUUIDText(uuid, in);
    return uuid;
}

const UUID TABLE_UUID = uuidOf("2a7c9e1f-5b3d-4e6a-8f0b-1c2d3e4f5a6b");
const UUID OTHER_UUID = uuidOf("2a7c9e1f-5b3d-4e6a-8f0b-1c2d3e4f5a6c");
const String RECORD_FILE = String("t/") + STORED_BINARY_TYPE_ORIGIN_FILE_NAME;

String readFileText(const DiskPtr & disk, const String & path)
{
    auto in = disk->readFile(path, ReadSettings{});
    String text;
    readStringUntilEOF(text, *in);
    return text;
}

void writeFileText(const DiskPtr & disk, const String & path, const String & text)
{
    auto out = disk->writeFile(path);
    writeString(text, *out);
    out->finalize();
}

int codeOf(const std::function<void()> & action)
{
    try
    {
        action();
    }
    catch (const Exception & e)
    {
        return e.code();
    }
    return 0;
}

class StoredBinaryTypeOriginDirectoryTest : public testing::Test
{
public:
    void SetUp() override { disk = createDisk(); }
    void TearDown() override { destroyDisk(disk); }

protected:
    DiskPtr disk;
};

}

TEST_F(StoredBinaryTypeOriginDirectoryTest, OriginalCreateStampsFreshDirectory)
{
    const StorageID table_id("db", "t", TABLE_UUID);
    {
        StoredBinaryTypeOriginOfTableDirectory origin;
        const auto to_stamp = origin.load(*disk, "t/", table_id, true, BinaryTypeOrigin::OfficialCanonical, ReadSettings{});
        ASSERT_TRUE(to_stamp.has_value());
        EXPECT_EQ(*to_stamp, BinaryTypeOrigin::OfficialCanonical);
        disk->createDirectories("t/");
        origin.stampNewTable(*disk, "t/", table_id, *to_stamp, WriteSettings{}, false);
        EXPECT_EQ(origin.get(), BinaryTypeOrigin::OfficialCanonical);
        EXPECT_FALSE(origin.isUnset());
    }
    EXPECT_EQ(readFileText(disk, RECORD_FILE), StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, TABLE_UUID).toString());

    /// ATTACH loads it, and a stamped table cannot be registered.
    StoredBinaryTypeOriginOfTableDirectory attached;
    EXPECT_FALSE(attached.load(*disk, "t/", table_id, false, std::nullopt, ReadSettings{}).has_value());
    EXPECT_EQ(attached.get(), BinaryTypeOrigin::OfficialCanonical);
    EXPECT_EQ(codeOf([&] { attached.checkCanBeRegistered(BinaryTypeOrigin::SentioLegacyV0, table_id); }), ErrorCodes::BAD_ARGUMENTS);
}

TEST_F(StoredBinaryTypeOriginDirectoryTest, OriginalCreateRefusesExistingDirectoryAndKeepsIt)
{
    const String data = "resident data";
    const String matching_record = StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, TABLE_UUID).toString();

    struct Case
    {
        const char * name;
        UUID uuid;
        bool with_data;
        bool with_record;
    };
    const Case cases[] = {
        {"empty directory", TABLE_UUID, false, false},
        {"data", TABLE_UUID, true, false},
        {"matching record and data", TABLE_UUID, true, true},
        {"matching record alone", TABLE_UUID, false, true},
        {"no UUID, data", UUIDHelpers::Nil, true, false},
        {"no UUID, empty directory", UUIDHelpers::Nil, false, false},
    };

    for (const auto & test_case : cases)
    {
        disk->createDirectories("t/");
        if (test_case.with_data)
            writeFileText(disk, "t/data.bin", data);
        if (test_case.with_record)
            writeFileText(disk, RECORD_FILE, matching_record);

        StoredBinaryTypeOriginOfTableDirectory origin;
        const StorageID table_id("db", "t", test_case.uuid);
        EXPECT_EQ(
            codeOf([&] { origin.load(*disk, "t/", table_id, true, BinaryTypeOrigin::OfficialCanonical, ReadSettings{}); }),
            ErrorCodes::TABLE_ALREADY_EXISTS)
            << test_case.name;

        /// Nothing was written or removed.
        EXPECT_TRUE(disk->existsDirectory("t/")) << test_case.name;
        if (test_case.with_data)
            EXPECT_EQ(readFileText(disk, "t/data.bin"), data) << test_case.name;
        if (test_case.with_record)
            EXPECT_EQ(readFileText(disk, RECORD_FILE), matching_record) << test_case.name;
        else
            EXPECT_FALSE(disk->existsFile(RECORD_FILE)) << test_case.name;

        disk->removeRecursive("t/");
    }
}

/// An original CREATE without an origin to stamp (a source table without an origin) is still an original CREATE: it requires a fresh
/// directory, and the new table stays unset. An origin to stamp without an original CREATE is a logical error.
TEST_F(StoredBinaryTypeOriginDirectoryTest, OriginalCreateWithoutOriginToStamp)
{
    const StorageID table_id("db", "t", TABLE_UUID);

    disk->createDirectories("t/");
    writeFileText(disk, "t/resident.bin", "resident");
    StoredBinaryTypeOriginOfTableDirectory refused;
    EXPECT_EQ(codeOf([&] { refused.load(*disk, "t/", table_id, true, std::nullopt, ReadSettings{}); }), ErrorCodes::TABLE_ALREADY_EXISTS);
    EXPECT_EQ(readFileText(disk, "t/resident.bin"), "resident");
    disk->removeRecursive("t/");

    StoredBinaryTypeOriginOfTableDirectory origin;
    EXPECT_FALSE(origin.load(*disk, "t/", table_id, true, std::nullopt, ReadSettings{}).has_value());
    EXPECT_TRUE(origin.isUnset());
    origin.checkCanBeRegistered(BinaryTypeOrigin::SentioLegacyV0, table_id);

    StoredBinaryTypeOriginOfTableDirectory misused;
    EXPECT_EQ(codeOf([&] { misused.load(*disk, "t/", table_id, false, BinaryTypeOrigin::OfficialCanonical, ReadSettings{}); }),
              ErrorCodes::LOGICAL_ERROR);
}

TEST_F(StoredBinaryTypeOriginDirectoryTest, OriginalCreateWithoutUuidHasNoRecord)
{
    const StorageID table_id("db", "t", UUIDHelpers::Nil);
    StoredBinaryTypeOriginOfTableDirectory origin;
    EXPECT_FALSE(origin.load(*disk, "t/", table_id, true, BinaryTypeOrigin::OfficialCanonical, ReadSettings{}).has_value());
    EXPECT_TRUE(origin.isUnset());
    EXPECT_EQ(codeOf([&] { origin.checkCanBeRegistered(BinaryTypeOrigin::SentioLegacyV0, table_id); }), ErrorCodes::BAD_ARGUMENTS);
}

TEST_F(StoredBinaryTypeOriginDirectoryTest, AttachLoadsEveryKindOfRecord)
{
    const StorageID table_id("db", "t", TABLE_UUID);
    disk->createDirectories("t/");

    /// No record: unset, and the registration states SentioLegacyV0, once.
    {
        StoredBinaryTypeOriginOfTableDirectory origin;
        origin.load(*disk, "t/", table_id, false, std::nullopt, ReadSettings{});
        EXPECT_TRUE(origin.isUnset());
        EXPECT_EQ(origin.get(), BinaryTypeOrigin::Unknown);
        EXPECT_EQ(codeOf([&] { origin.checkCanBeRegistered(BinaryTypeOrigin::OfficialCanonical, table_id); }), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_EQ(codeOf([&] { origin.checkCanBeRegistered(BinaryTypeOrigin::Unknown, table_id); }), ErrorCodes::BAD_ARGUMENTS);
        origin.registerOrigin(*disk, "t/", table_id, BinaryTypeOrigin::SentioLegacyV0, WriteSettings{}, false);
        EXPECT_EQ(origin.get(), BinaryTypeOrigin::SentioLegacyV0);
        EXPECT_EQ(codeOf([&] { origin.registerOrigin(*disk, "t/", table_id, BinaryTypeOrigin::SentioLegacyV0, WriteSettings{}, false); }),
                  ErrorCodes::BAD_ARGUMENTS);
    }
    EXPECT_EQ(readFileText(disk, RECORD_FILE), StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::SentioLegacyV0, TABLE_UUID).toString());
    {
        StoredBinaryTypeOriginOfTableDirectory origin;
        origin.load(*disk, "t/", table_id, false, std::nullopt, ReadSettings{});
        EXPECT_EQ(origin.get(), BinaryTypeOrigin::SentioLegacyV0);
        EXPECT_FALSE(origin.isUnset());
    }

    /// An explicit Unknown record, a record of another table, a part record: Unknown, not unset, not registered, and kept as it is.
    const String other_records[] = {
        StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::Unknown, TABLE_UUID).toString(),
        StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::SentioLegacyV0, OTHER_UUID).toString(),
        StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin::SentioLegacyV0, "0123456789ABCDEF0123456789ABCDEF").toString(),
    };
    for (const auto & record : other_records)
    {
        writeFileText(disk, RECORD_FILE, record);
        StoredBinaryTypeOriginOfTableDirectory origin;
        origin.load(*disk, "t/", table_id, false, std::nullopt, ReadSettings{});
        EXPECT_EQ(origin.get(), BinaryTypeOrigin::Unknown) << record;
        EXPECT_FALSE(origin.isUnset()) << record;
        EXPECT_EQ(codeOf([&] { origin.checkCanBeRegistered(BinaryTypeOrigin::SentioLegacyV0, table_id); }), ErrorCodes::BAD_ARGUMENTS)
            << record;
        EXPECT_EQ(readFileText(disk, RECORD_FILE), record);
    }

    /// A table without a UUID ignores a record and cannot be registered.
    {
        writeFileText(disk, RECORD_FILE, StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::SentioLegacyV0, TABLE_UUID).toString());
        const StorageID table_without_uuid("db", "t", UUIDHelpers::Nil);
        StoredBinaryTypeOriginOfTableDirectory origin;
        origin.load(*disk, "t/", table_without_uuid, false, std::nullopt, ReadSettings{});
        EXPECT_EQ(origin.get(), BinaryTypeOrigin::Unknown);
        EXPECT_EQ(codeOf([&] { origin.checkCanBeRegistered(BinaryTypeOrigin::SentioLegacyV0, table_without_uuid); }), ErrorCodes::BAD_ARGUMENTS);
    }

    /// A malformed record is an error, not an absent one.
    writeFileText(disk, RECORD_FILE, "sentio binary type origin record version: 1\nkind: table\norigin: something\n");
    StoredBinaryTypeOriginOfTableDirectory malformed;
    EXPECT_EQ(codeOf([&] { malformed.load(*disk, "t/", table_id, false, std::nullopt, ReadSettings{}); }), ErrorCodes::CORRUPTED_DATA);
}

/// The storages check the directory before they create anything, also on a disk that is not the default one of the server.
TEST_F(StoredBinaryTypeOriginDirectoryTest, StoragesRefuseOriginalCreateOverExistingDirectory)
{
    NamesAndTypesList columns{{"a", std::make_shared<DataTypeUInt64>()}};

    auto create_log = [&](const String & path, const UUID & uuid)
    {
        return std::make_shared<StorageLog>(
            "Log", disk, path, StorageID("db", "log", uuid), ColumnsDescription{columns}, ConstraintsDescription{}, String{},
            LoadingStrictnessLevel::CREATE, getContext().context, true, BinaryTypeOrigin::OfficialCanonical);
    };
    auto create_stripe_log = [&](const String & path, const UUID & uuid)
    {
        return std::make_shared<StorageStripeLog>(
            disk, path, StorageID("db", "stripe_log", uuid), ColumnsDescription{columns}, ConstraintsDescription{}, String{},
            LoadingStrictnessLevel::CREATE, getContext().context, true, BinaryTypeOrigin::OfficialCanonical);
    };

    /// A fresh directory is created and stamped.
    create_log("log/", TABLE_UUID)->flushAndShutdown();
    EXPECT_EQ(readFileText(disk, String("log/") + STORED_BINARY_TYPE_ORIGIN_FILE_NAME),
              StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, TABLE_UUID).toString());
    create_stripe_log("stripe_log/", OTHER_UUID)->flushAndShutdown();
    EXPECT_EQ(readFileText(disk, String("stripe_log/") + STORED_BINARY_TYPE_ORIGIN_FILE_NAME),
              StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, OTHER_UUID).toString());

    /// An existing directory is refused, with or without a UUID, and stays as it was. The resident file is one that the constructors do not
    /// read (each of them parses an existing sizes.json in its FileChecker before the check).
    for (const auto & uuid : {TABLE_UUID, UUIDHelpers::Nil})
    {
        disk->createDirectories("old/");
        writeFileText(disk, "old/resident.bin", "resident");
        EXPECT_EQ(codeOf([&] { create_log("old/", uuid); }), ErrorCodes::TABLE_ALREADY_EXISTS);
        EXPECT_EQ(codeOf([&] { create_stripe_log("old/", uuid); }), ErrorCodes::TABLE_ALREADY_EXISTS);
        EXPECT_EQ(readFileText(disk, "old/resident.bin"), "resident");
        EXPECT_FALSE(disk->existsFile(String("old/") + STORED_BINARY_TYPE_ORIGIN_FILE_NAME));
        EXPECT_FALSE(disk->existsFile("old/sizes.json"));
        disk->removeRecursive("old/");
    }
}
