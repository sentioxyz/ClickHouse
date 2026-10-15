#include <gtest/gtest.h>

#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <Core/Block.h>
#include <Core/Joins.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Disks/DiskLocal.h>
#include <Disks/tests/gtest_disk.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <Interpreters/Context.h>
#include <Interpreters/Set.h>
#include <Interpreters/StorageID.h>
#include <Processors/Executors/PullingPipelineExecutor.h>
#include <Processors/Executors/PushingPipelineExecutor.h>
#include <QueryPipeline/Pipe.h>
#include <QueryPipeline/QueryPipeline.h>
#include <QueryPipeline/SizeLimits.h>
#include <Storages/AlterCommands.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/ConstraintsDescription.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageJoin.h>
#include <Storages/StorageSet.h>
#include <Storages/StoredBinaryTypeOrigin.h>
#include <Common/Exception.h>
#include <Common/assert_cast.h>
#include <Common/tests/gtest_global_context.h>
#include <base/scope_guard.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

/// The registration of the stored binary type origin of a Set or Join table against the writers of the table and the readers of its state.
/// An insert holds the gate of the writers from the creation of its sink to its destruction: a registration that cannot wait for it changes
/// nothing, and one that waits reads the file that the insert published, or none of the rows of a cancelled insert, which published nothing
/// (its number stays unused). A registration that cannot take the state from a reader in time writes no record and keeps the state and the
/// numbering of the files. While a registration of a Join table persists its record, it holds the lock of the state for write, and the
/// return type of joinGet, which is read under the lock of the state, waits for it: a disk of the test stops the registration exactly there.
/// The tables load as on ATTACH from directories without a record, so that their origin can be registered; their data has no binary type
/// headers, so every origin reads it the same. Every case is decided by the state of the tables, not by timing: a short timeout expires only
/// when no other thread can release what it waits for, and a thread that waits is joined.

using namespace DB;

namespace DB::Setting
{
extern const SettingsSeconds lock_acquire_timeout;
}

namespace DB::ErrorCodes
{
extern const int DEADLOCK_AVOIDED;
extern const int NO_SUCH_COLUMN_IN_TABLE;
extern const int TYPE_MISMATCH;
}

namespace
{

constexpr double SHORT_TIMEOUT_SECONDS = 0.001;
constexpr double LONG_TIMEOUT_SECONDS = 60;

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

ContextMutablePtr makeContext(const String & query_id, double lock_acquire_timeout_seconds)
{
    auto context = Context::createCopy(getContext().context);
    context->makeQueryContext();
    context->setQueryKindInitial();
    context->setCurrentQueryId(query_id);
    context->setSetting("lock_acquire_timeout", Field(lock_acquire_timeout_seconds));
    return context;
}

StoragePtr createJoin(const DiskPtr & disk, const String & path, const UUID & uuid)
{
    const NamesAndTypesList columns{{"k", std::make_shared<DataTypeUInt64>()}, {"v", std::make_shared<DataTypeString>()}};
    auto table = std::make_shared<StorageJoin>(
        disk, path, StorageID("test", "join", uuid), Names{"k"}, /*use_nulls=*/ false, SizeLimits{}, JoinKind::Left, JoinStrictness::Any,
        ColumnsDescription(columns), ConstraintsDescription{}, String{}, /*overwrite=*/ false, /*persistent=*/ true);
    table->startup();
    return table;
}

StoragePtr createSet(const DiskPtr & disk, const String & path, const UUID & uuid)
{
    const NamesAndTypesList columns{{"k", std::make_shared<DataTypeUInt64>()}};
    auto table = std::make_shared<StorageSet>(
        disk, path, StorageID("test", "set", uuid), ColumnsDescription(columns), ConstraintsDescription{}, String{}, /*persistent=*/ true);
    table->startup();
    return table;
}

Block joinBlock(UInt64 key, const String & value)
{
    auto keys = ColumnUInt64::create();
    keys->insertValue(key);
    auto values = ColumnString::create();
    values->insert(value);
    return Block({ColumnWithTypeAndName(std::move(keys), std::make_shared<DataTypeUInt64>(), "k"),
                  ColumnWithTypeAndName(std::move(values), std::make_shared<DataTypeString>(), "v")});
}

Block setBlock(UInt64 key)
{
    auto keys = ColumnUInt64::create();
    keys->insertValue(key);
    return Block({ColumnWithTypeAndName(std::move(keys), std::make_shared<DataTypeUInt64>(), "k")});
}

/// An insert into a table that has consumed one block and holds its sink until it finishes or is cancelled.
class Insert
{
public:
    Insert(const StoragePtr & table, const ContextPtr & context, const Block & block)
        : pipeline(table->write({}, table->getInMemoryMetadataPtr(context, false), context, /*async_insert=*/ false))
        , executor(std::make_unique<PushingPipelineExecutor>(pipeline))
    {
        executor->start();
        executor->push(block);
    }

    ~Insert()
    {
        if (executor)
            executor->cancel();
    }

    /// Publishes the file of the insert, then destroys the sink.
    void finish()
    {
        executor->finish();
        release();
    }

    /// Cancels the insert, which publishes nothing, then destroys the sink.
    void cancel()
    {
        executor->cancel();
        release();
    }

private:
    void release()
    {
        executor.reset();
        pipeline.reset();
    }

    QueryPipeline pipeline;
    std::unique_ptr<PushingPipelineExecutor> executor;
};

void insertFinished(const StoragePtr & table, const ContextPtr & context, const Block & block)
{
    Insert insert(table, context, block);
    insert.finish();
}

void registerOrigin(const StoragePtr & table, const ContextPtr & context)
{
    AlterCommand command;
    command.type = AlterCommand::MODIFY_SENTIO_BINARY_TYPE_ORIGIN;
    command.binary_type_origin = BinaryTypeOrigin::SentioLegacyV0;
    AlterCommands commands;
    commands.push_back(command);

    auto alter_lock = table->lockForAlter(context->getSettingsRef()[Setting::lock_acquire_timeout]);
    table->checkAlterIsPossible(commands, context);
    table->alter(commands, context, alter_lock);
}

/// A read of the keys of a Join table: the pipe holds the lock of the state for read until it is destroyed.
Pipe readJoinKeys(const StoragePtr & table, const ContextPtr & context)
{
    auto snapshot = table->getStorageSnapshot(table->getInMemoryMetadataPtr(context, false), context);
    SelectQueryInfo query_info;
    return table->read(Names{"k"}, snapshot, query_info, context, QueryProcessingStage::FetchColumns, 8192, 1);
}

std::vector<UInt64> joinKeys(const StoragePtr & table, const ContextPtr & context)
{
    QueryPipeline pipeline(readJoinKeys(table, context));
    PullingPipelineExecutor executor(pipeline);
    std::vector<UInt64> keys;
    Block block;
    while (executor.pull(block))
    {
        const auto & column = assert_cast<const ColumnUInt64 &>(*block.getByName("k").column);
        keys.insert(keys.end(), column.getData().begin(), column.getData().end());
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

/// A local disk of the test that stops the replacement of the record of a stored binary type origin (the last step of writing it) until
/// the test releases it. The registration of the origin of a Join table writes its record while it holds the lock of the state for write.
class RecordLatchDisk final : public DiskLocal
{
public:
    RecordLatchDisk(const String & name_, const String & path_) : DiskLocal(name_, path_) {}

    void replaceFile(const String & from_path, const String & to_path) override
    {
        if (to_path.ends_with(STORED_BINARY_TYPE_ORIGIN_FILE_NAME))
        {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [this] { return released; });
        }
        DiskLocal::replaceFile(from_path, to_path);
    }

    /// Waits until a replacement of a record waits for the release, or the registration has ended without one; returns whether it waits.
    bool waitUntilEnteredOrEnded()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return entered || ended; });
        return entered;
    }

    /// The registration has ended, so that nothing waits for it to enter.
    void end()
    {
        std::lock_guard lock(mutex);
        ended = true;
        changed.notify_all();
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool ended = false;
    bool released = false;
};

class SetOrJoinOriginRegistrationTest : public testing::Test
{
protected:
    void SetUp() override { disk = createDisk("set_or_join_origin_registration"); }
    void TearDown() override { destroyDisk(disk); }

    /// The numbered files of the table in `path`, which hold its data.
    std::vector<String> dataFiles(const String & path) const
    {
        std::vector<String> names;
        disk->listFiles(path, names);
        std::vector<String> data_files;
        for (const auto & name : names)
            if (name.ends_with(".bin"))
                data_files.push_back(name);
        std::sort(data_files.begin(), data_files.end());
        return data_files;
    }

    /// The text of the record of the table in `path`, or nothing.
    std::optional<String> record(const String & path) const
    {
        const String record_path = path + STORED_BINARY_TYPE_ORIGIN_FILE_NAME;
        if (!disk->existsFile(record_path))
            return {};
        auto in = disk->readFile(record_path, ReadSettings{});
        String text;
        readStringUntilEOF(text, *in);
        return text;
    }

    DiskPtr disk;
};

std::optional<String> legacyRecord(const UUID & uuid)
{
    return StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::SentioLegacyV0, uuid).toString();
}

}

/// A registration that cannot wait for an insert in its sink changes nothing; one that waits proceeds once the insert has published its
/// file and destroyed its sink, and reads that file, whichever comes first: its wait or the end of the insert.
TEST_F(SetOrJoinOriginRegistrationTest, JoinRegistrationWaitsForAnInsertAndReadsItsFile)
{
    const String path = "join/";
    const UUID uuid = UUIDHelpers::generateV4();
    auto table = createJoin(disk, path, uuid);
    const auto writer = makeContext("writer", LONG_TIMEOUT_SECONDS);
    const auto reader = makeContext("reader", LONG_TIMEOUT_SECONDS);
    insertFinished(table, writer, joinBlock(1, "a"));

    Insert insert(table, writer, joinBlock(2, "b"));
    EXPECT_EQ(codeOf([&] { registerOrigin(table, makeContext("short registration", SHORT_TIMEOUT_SECONDS)); }), ErrorCodes::DEADLOCK_AVOIDED);
    EXPECT_FALSE(record(path).has_value());
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin"}));
    /// The rows of the insert are in the state while it consumes, as before.
    EXPECT_EQ(joinKeys(table, reader), (std::vector<UInt64>{1, 2}));

    std::optional<int> code;
    std::thread registration([&] { code = codeOf([&] { registerOrigin(table, makeContext("registration", LONG_TIMEOUT_SECONDS)); }); });
    insert.finish();
    registration.join();
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 0);
    EXPECT_EQ(record(path), legacyRecord(uuid));
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin", "2.bin"}));
    EXPECT_EQ(joinKeys(table, reader), (std::vector<UInt64>{1, 2}));
}

/// A cancelled insert publishes nothing: the state that the registration reads from the files has none of its rows, as after a restart.
TEST_F(SetOrJoinOriginRegistrationTest, JoinRegistrationAfterACancelledInsert)
{
    const String path = "join/";
    const UUID uuid = UUIDHelpers::generateV4();
    auto table = createJoin(disk, path, uuid);
    const auto writer = makeContext("writer", LONG_TIMEOUT_SECONDS);
    const auto reader = makeContext("reader", LONG_TIMEOUT_SECONDS);
    insertFinished(table, writer, joinBlock(1, "a"));

    Insert insert(table, writer, joinBlock(2, "b"));
    std::optional<int> code;
    std::thread registration([&] { code = codeOf([&] { registerOrigin(table, makeContext("registration", LONG_TIMEOUT_SECONDS)); }); });
    insert.cancel();
    registration.join();
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 0);
    EXPECT_EQ(record(path), legacyRecord(uuid));
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin"}));
    EXPECT_EQ(joinKeys(table, reader), (std::vector<UInt64>{1}));
}

/// A registration that cannot take the state from a reader in time fails before it writes the record: the table keeps its state, its files
/// and their numbering, and stays without a record, so that a later registration succeeds.
TEST_F(SetOrJoinOriginRegistrationTest, JoinRegistrationThatCannotPublishChangesNothing)
{
    const String path = "join/";
    const UUID uuid = UUIDHelpers::generateV4();
    auto table = createJoin(disk, path, uuid);
    const auto writer = makeContext("writer", LONG_TIMEOUT_SECONDS);
    const auto reader = makeContext("reader", LONG_TIMEOUT_SECONDS);
    insertFinished(table, writer, joinBlock(1, "a"));

    {
        auto reading = readJoinKeys(table, reader);
        EXPECT_EQ(codeOf([&] { registerOrigin(table, makeContext("short registration", SHORT_TIMEOUT_SECONDS)); }), ErrorCodes::DEADLOCK_AVOIDED);
        EXPECT_FALSE(record(path).has_value());
    }
    EXPECT_EQ(joinKeys(table, reader), (std::vector<UInt64>{1}));

    /// The next insert takes the next number.
    insertFinished(table, writer, joinBlock(2, "b"));
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin", "2.bin"}));

    registerOrigin(table, makeContext("registration", LONG_TIMEOUT_SECONDS));
    EXPECT_EQ(record(path), legacyRecord(uuid));
    EXPECT_EQ(joinKeys(table, reader), (std::vector<UInt64>{1, 2}));
}

/// The registration stops while it writes the record, holding the lock of the state for write: the return type of joinGet, which takes
/// that lock for read, cannot be read before the timeout of its query. Without that lock it would read the join that the registration is
/// about to replace. Once the registration ends, the type and the ordinary errors of the type check are as before.
TEST_F(SetOrJoinOriginRegistrationTest, JoinGetReturnTypeWaitsForThePublication)
{
    const String path = "join/";
    const UUID uuid = UUIDHelpers::generateV4();
    auto latch_disk = std::make_shared<RecordLatchDisk>("record_latch", disk->getPath());
    auto table = createJoin(latch_disk, path, uuid);
    insertFinished(table, makeContext("writer", LONG_TIMEOUT_SECONDS), joinBlock(1, "a"));
    const auto & join = assert_cast<const StorageJoin &>(*table);
    const DataTypes key_types{std::make_shared<DataTypeUInt64>()};
    const auto string_type = std::make_shared<DataTypeString>();

    std::optional<int> registration_code;
    bool entered = false;
    int type_code_while_registering = 0;
    {
        std::thread registration([&]
        {
            registration_code = codeOf([&] { registerOrigin(table, makeContext("registration", LONG_TIMEOUT_SECONDS)); });
            latch_disk->end();
        });
        /// However this block ends, the registration is released and joined.
        SCOPE_EXIT({
            latch_disk->release();
            registration.join();
        });

        entered = latch_disk->waitUntilEnteredOrEnded();
        if (entered)
            type_code_while_registering = codeOf([&]
            {
                join.joinGetCheckAndGetReturnType(key_types, "v", /*or_null=*/ false, makeContext("reader", SHORT_TIMEOUT_SECONDS));
            });
    }
    ASSERT_TRUE(entered);
    EXPECT_EQ(type_code_while_registering, ErrorCodes::DEADLOCK_AVOIDED);
    ASSERT_TRUE(registration_code.has_value());
    EXPECT_EQ(*registration_code, 0);
    EXPECT_EQ(record(path), legacyRecord(uuid));

    const auto reader = makeContext("reader", LONG_TIMEOUT_SECONDS);
    EXPECT_TRUE(join.joinGetCheckAndGetReturnType(key_types, "v", /*or_null=*/ false, reader)->equals(*string_type));
    EXPECT_TRUE(join.joinGetCheckAndGetReturnType(key_types, "v", /*or_null=*/ true, reader)->equals(DataTypeNullable(string_type)));
    EXPECT_EQ(codeOf([&] { join.joinGetCheckAndGetReturnType({string_type}, "v", false, reader); }), ErrorCodes::TYPE_MISMATCH);
    EXPECT_EQ(codeOf([&] { join.joinGetCheckAndGetReturnType(key_types, "w", false, reader); }), ErrorCodes::NO_SUCH_COLUMN_IN_TABLE);
}

/// A stress check without a handshake: a reader asks the return type of joinGet of eight tables while they are registered one after
/// another. It may run before, during or after the publications, so it proves only that every answer is the type of the column; the test
/// above is the deterministic witness of the lock.
TEST_F(SetOrJoinOriginRegistrationTest, JoinGetReturnTypeStressWithRegistrations)
{
    constexpr size_t tables_count = 8;
    const auto writer = makeContext("writer", LONG_TIMEOUT_SECONDS);
    std::vector<StoragePtr> tables;
    for (size_t i = 0; i < tables_count; ++i)
    {
        tables.push_back(createJoin(disk, "join" + std::to_string(i) + "/", UUIDHelpers::generateV4()));
        insertFinished(tables.back(), writer, joinBlock(1, "a"));
    }

    const DataTypes key_types{std::make_shared<DataTypeUInt64>()};
    const auto string_type = std::make_shared<DataTypeString>();
    const auto nullable_string_type = std::make_shared<DataTypeNullable>(string_type);

    std::atomic<bool> registered = false;
    std::atomic<size_t> wrong_types = 0;
    std::thread reader([&]
    {
        const auto context = makeContext("reader", LONG_TIMEOUT_SECONDS);
        do
        {
            for (const auto & table : tables)
            {
                const auto & join = assert_cast<const StorageJoin &>(*table);
                if (!join.joinGetCheckAndGetReturnType(key_types, "v", /*or_null=*/ false, context)->equals(*string_type))
                    ++wrong_types;
                if (!join.joinGetCheckAndGetReturnType(key_types, "v", /*or_null=*/ true, context)->equals(*nullable_string_type))
                    ++wrong_types;
            }
        } while (!registered.load());
    });
    for (const auto & table : tables)
        registerOrigin(table, makeContext("registration", LONG_TIMEOUT_SECONDS));
    registered = true;
    reader.join();
    EXPECT_EQ(wrong_types.load(), 0u);
}

/// The same for a Set table. An insert that is cancelled after it took the number 2 publishes no 2.bin; the next insert takes 3. The
/// registration reads the files 1.bin and 3.bin, not the key of the cancelled insert, and the numbering goes on after the greatest number.
TEST_F(SetOrJoinOriginRegistrationTest, SetRegistrationWithFinishedAndCancelledInserts)
{
    const String path = "set/";
    const UUID uuid = UUIDHelpers::generateV4();
    auto table = createSet(disk, path, uuid);
    const auto writer = makeContext("writer", LONG_TIMEOUT_SECONDS);
    const auto reader = makeContext("reader", LONG_TIMEOUT_SECONDS);
    insertFinished(table, writer, setBlock(1));

    {
        Insert insert(table, writer, setBlock(2));
        EXPECT_EQ(codeOf([&] { registerOrigin(table, makeContext("short registration", SHORT_TIMEOUT_SECONDS)); }), ErrorCodes::DEADLOCK_AVOIDED);
        EXPECT_FALSE(record(path).has_value());
        insert.cancel();
    }
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin"}));
    /// The key that the cancelled insert consumed stays in the state until the state is read again, as before.
    EXPECT_EQ(table->totalRows(reader), std::optional<UInt64>(2));

    Insert insert(table, writer, setBlock(3));
    std::optional<int> code;
    std::thread registration([&] { code = codeOf([&] { registerOrigin(table, makeContext("registration", LONG_TIMEOUT_SECONDS)); }); });
    insert.finish();
    registration.join();
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 0);
    EXPECT_EQ(record(path), legacyRecord(uuid));
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin", "3.bin"}));
    EXPECT_EQ(table->totalRows(reader), std::optional<UInt64>(2));

    const auto set = assert_cast<const StorageSet &>(*table).getSet();
    auto probe = ColumnUInt64::create();
    probe->insertValue(1);
    probe->insertValue(2);
    probe->insertValue(3);
    const auto found = set->execute({ColumnWithTypeAndName(std::move(probe), std::make_shared<DataTypeUInt64>(), "k")}, /*negative=*/ false);
    const auto & found_data = assert_cast<const ColumnUInt8 &>(*found).getData();
    ASSERT_EQ(found_data.size(), 3u);
    EXPECT_EQ(found_data[0], 1);
    EXPECT_EQ(found_data[1], 0);
    EXPECT_EQ(found_data[2], 1);

    insertFinished(table, writer, setBlock(4));
    EXPECT_EQ(dataFiles(path), (std::vector<String>{"1.bin", "3.bin", "4.bin"}));
    EXPECT_EQ(table->totalRows(reader), std::optional<UInt64>(3));
}
