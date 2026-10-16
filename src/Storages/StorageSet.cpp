#include <DataTypes/BinaryTypeOriginRequiredException.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/Serializations/SerializationNumber.h>
#include <DataTypes/Serializations/SerializationString.h>
#include <Storages/SetSettings.h>
#include <Storages/StorageSet.h>
#include <Storages/StorageFactory.h>
#include <Compression/CompressedReadBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <Compression/CompressedWriteBuffer.h>
#include <Formats/NativeWriter.h>
#include <Formats/NativeReader.h>
#include <QueryPipeline/ProfileInfo.h>
#include <Disks/IDisk.h>
#include <Common/CurrentThread.h>
#include <Common/assert_cast.h>
#include <Common/formatReadable.h>
#include <Common/StringUtils.h>
#include <Interpreters/Context.h>
#include <IO/ReadBufferFromFileBase.h>
#include <Common/logger_useful.h>
#include <Interpreters/Set.h>
#include <Processors/Sinks/SinkToStorage.h>
#include <Parsers/ASTCreateQuery.h>
#include <Core/Settings.h>
#include <IO/ReadSettings.h>
#include <IO/WriteSettings.h>
#include <Storages/AlterCommands.h>
#include <base/scope_guard.h>
#include <filesystem>
#include <optional>
#include <typeinfo>

namespace fs = std::filesystem;


namespace DB
{

namespace Setting
{
    extern const SettingsBool fsync_metadata;
    extern const SettingsSeconds lock_acquire_timeout;
}

namespace SetSetting
{
    extern const SetSettingsString disk;
    extern const SetSettingsBool persistent;
}

namespace ErrorCodes
{
    extern const int INCORRECT_FILE_NAME;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

std::chrono::milliseconds getLockAcquireTimeout(const ContextPtr & context)
{
    return std::chrono::milliseconds(context->getSettingsRef()[Setting::lock_acquire_timeout].totalMilliseconds());
}

/// `SetOrJoinSink` writes revision-0 `Native` files: type names and default serializations. Standard `UInt64` and `String` payloads
/// have no binary type headers, so their meaning does not depend on an origin. Preserve their original file/object layout instead of
/// adding a record (and, on S3, another object). This is deliberately a closed list: containers, aggregate states, dynamic types, 512-bit
/// types and every future or customized serialization retain the record. Never use a dynamic-structure negative as proof here.
bool needsAutomaticStoredBinaryTypeOriginRecord(const ColumnsDescription & columns)
{
    for (const auto & column : columns.getAll())
    {
        const auto & type = *column.type;
        if (type.hasCustomName())
            return true;
        const auto serialization = type.getDefaultSerialization();
        if (type.getTypeId() == TypeIndex::UInt64 && typeid(*serialization) == typeid(SerializationNumber<UInt64>))
            continue;
        if (type.getTypeId() == TypeIndex::String && typeid(*serialization) == typeid(SerializationString))
            continue;
        return true;
    }
    return false;
}

}

class SetOrJoinSink final : public SinkToStorage, WithContext
{
public:
    SetOrJoinSink(
        SetOrJoinWritersGate::TokenPtr writer_token_,
        ContextPtr ctx, StorageSetOrJoinBase & table_, const StorageMetadataPtr & metadata_snapshot_,
        const String & backup_path_, const String & backup_tmp_path_,
        const String & backup_file_name_, bool persistent_);
    ~SetOrJoinSink() override;

    String getName() const override { return "SetOrJoinSink"; }
    void consume(Chunk & chunk) override;
    void onFinish() override;

private:
    void cancelBuffers() noexcept;

    /// Destroyed last: the sink leaves the gate of the writers only once it has published or cancelled everything.
    SetOrJoinWritersGate::TokenPtr writer_token;
    StorageSetOrJoinBase & table;
    StorageMetadataPtr metadata_snapshot;
    String backup_path;
    String backup_tmp_path;
    String backup_file_name;
    std::unique_ptr<WriteBufferFromFileBase> backup_buf;
    std::optional<CompressedWriteBuffer> compressed_backup_buf;
    std::optional<NativeWriter> backup_stream;
    bool persistent;
};


SetOrJoinSink::SetOrJoinSink(
    SetOrJoinWritersGate::TokenPtr writer_token_,
    ContextPtr ctx,
    StorageSetOrJoinBase & table_,
    const StorageMetadataPtr & metadata_snapshot_,
    const String & backup_path_,
    const String & backup_tmp_path_,
    const String & backup_file_name_,
    bool persistent_)
    : SinkToStorage(std::make_shared<const Block>(metadata_snapshot_->getSampleBlock()))
    , WithContext(ctx)
    , writer_token(std::move(writer_token_))
    , table(table_)
    , metadata_snapshot(metadata_snapshot_)
    , backup_path(backup_path_)
    , backup_tmp_path(backup_tmp_path_)
    , backup_file_name(backup_file_name_)
    , persistent(persistent_)
{
}

SetOrJoinSink::~SetOrJoinSink()
{
    if (isCancelled())
        cancelBuffers();
}

void SetOrJoinSink::cancelBuffers() noexcept
{
    if (compressed_backup_buf)
        compressed_backup_buf->cancel();
    if (backup_buf)
        backup_buf->cancel();
}


void SetOrJoinSink::consume(Chunk & chunk)
{
    Block block = getHeader().cloneWithColumns(chunk.getColumns());

    table.insertBlock(block, getContext());
    if (persistent)
    {
        if (!backup_buf)
        {
            backup_buf = table.disk->writeFile(fs::path(backup_tmp_path) / backup_file_name);
            compressed_backup_buf.emplace(*backup_buf);
            backup_stream.emplace(*compressed_backup_buf, 0, std::make_shared<const Block>(metadata_snapshot->getSampleBlock()));
        }
        backup_stream->write(block);
    }
}

void SetOrJoinSink::onFinish()
{
    table.finishInsert();
    if (backup_buf)
    {
        backup_stream->flush();
        compressed_backup_buf->finalize();
        backup_buf->finalize();

        table.disk->replaceFile(fs::path(backup_tmp_path) / backup_file_name, fs::path(backup_path) / backup_file_name);
    }
}


SinkToStoragePtr StorageSetOrJoinBase::write(const ASTPtr & /*query*/, const StorageMetadataPtr & metadata_snapshot, ContextPtr context, bool /*async_insert*/)
{
    return makeSink(enterAsWriter(context), metadata_snapshot, context);
}

SetOrJoinWritersGate::TokenPtr StorageSetOrJoinBase::enterAsWriter(ContextPtr context)
{
    checkOriginNotRequired();
    return writers_gate.enter(getLockAcquireTimeout(context), getStorageID().getNameForLogs());
}

SinkToStoragePtr StorageSetOrJoinBase::makeSink(
    SetOrJoinWritersGate::TokenPtr token, const StorageMetadataPtr & metadata_snapshot, ContextPtr context)
{
    UInt64 id = ++increment;
    return std::make_shared<SetOrJoinSink>(
        std::move(token), context, *this, metadata_snapshot, path, fs::path(path) / "tmp/", toString(id) + ".bin", persistent);
}


StorageSetOrJoinBase::StorageSetOrJoinBase(
    DiskPtr disk_,
    const String & relative_path_,
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & comment,
    bool persistent_,
    bool is_original_create,
    std::optional<BinaryTypeOrigin> stored_binary_type_origin_to_stamp)
    : StorageWithCommonVirtualColumns(table_id_), disk(disk_), persistent(persistent_)
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_);
    storage_metadata.setConstraints(constraints_);
    storage_metadata.setComment(comment);
    storage_metadata.setVirtuals(createVirtuals());
    setInMemoryMetadata(storage_metadata);

    if (relative_path_.empty())
        throw Exception(ErrorCodes::INCORRECT_FILE_NAME, "Join and Set storages require data path");

    path = relative_path_;

    /// Before the directory is created or changed: an original CREATE requires that it does not exist yet, and creates it here.
    const auto origin_to_stamp = stored_binary_type_origin.load(
        *disk, path, table_id_, is_original_create, stored_binary_type_origin_to_stamp, getReadSettings());
    if (is_original_create)
    {
        disk->createDirectories(fs::path(path) / "tmp/");
        if (origin_to_stamp && needsAutomaticStoredBinaryTypeOriginRecord(columns_))
        {
            const auto query_context = CurrentThread::tryGetQueryContext();
            stored_binary_type_origin.stampNewTable(
                *disk, path, table_id_, *origin_to_stamp, getWriteSettings(),
                query_context && query_context->getSettingsRef()[Setting::fsync_metadata]);
        }
    }
}

void StorageSetOrJoinBase::checkOriginNotRequired() const
{
    std::unique_ptr<Exception> error;
    {
        std::lock_guard lock(origin_required_mutex);
        if (!origin_required_error)
            return;
        /// A copy for each use, so that no two queries add to the same exception.
        error.reset(origin_required_error->clone());
    }
    error->addMessage(
        "Table {} was loaded without its data, which needs a declared stored binary type origin: register it with ALTER TABLE ... MODIFY "
        "SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'",
        getStorageID().getNameForLogs());
    error->rethrow();
}

void StorageSetOrJoinBase::clearOriginRequired() noexcept
{
    std::lock_guard lock(origin_required_mutex);
    origin_required_error.reset();
}

bool StorageSetOrJoinBase::isOriginRequired() const
{
    std::lock_guard lock(origin_required_mutex);
    return origin_required_error != nullptr;
}

void StorageSetOrJoinBase::checkAlterIsPossible(const AlterCommands & commands, ContextPtr local_context) const
{
    /// The registration of the stored binary type origin changes nothing else, and AlterCommands::validate allows it only alone.
    if (commands.isStoredBinaryTypeOriginRegistration())
    {
        stored_binary_type_origin.checkCanBeRegistered(commands.front().binary_type_origin, getStorageID());
        return;
    }
    IStorage::checkAlterIsPossible(commands, local_context);
}

void StorageSetOrJoinBase::alter(const AlterCommands & params, ContextPtr local_context, AlterLockHolder & alter_lock_holder)
{
    if (params.isStoredBinaryTypeOriginRegistration())
    {
        registerStoredBinaryTypeOrigin(params.front().binary_type_origin, local_context);
        return;
    }
    IStorage::alter(params, local_context, alter_lock_holder);
}

void StorageSetOrJoinBase::registerStoredBinaryTypeOrigin(BinaryTypeOrigin origin, ContextPtr context)
{
    const auto table_id = getStorageID();
    stored_binary_type_origin.checkCanBeRegistered(origin, table_id);

    const bool sync = context->getSettingsRef()[Setting::fsync_metadata];
    auto persist = [&] { stored_binary_type_origin.registerOrigin(*disk, path, table_id, origin, getWriteSettings(), sync); };

    /// A table without persistence keeps the rows inserted into it by this server, which are in no file, and its writers write no file: its
    /// state stays as it is. The files it loaded were read with the Unknown origin of a table without a record, so they hold none of the
    /// indexes whose meaning depends on the origin (0x37 to 0x3A), and read the same with the registered one. A blocked table is not such a
    /// table, whatever its persistence: it loaded without the data of its files (which remain from when it had persistence, or were
    /// written by another server) and refuses inserts, so its files are all its data, which it reads again here as a persistent table does.
    /// After the load, only the registration and TRUNCATE, which hold the lock of the alter of the table exclusively against each other,
    /// change whether a table is blocked.
    if (!persistent && !isOriginRequired())
    {
        persist();
        return;
    }

    /// No writer inserts into the state or publishes a file while the files are read again, and none started before publishes after the
    /// new state. No lock of the table is held while the writers leave: they may need one to finish.
    writers_gate.close(getLockAcquireTimeout(context), table_id.getNameForLogs());
    SCOPE_EXIT({ writers_gate.open(); });
    auto registration_lock = lockForRegistration();

    UInt64 max_number = 0;
    const auto files = listBackupFiles(max_number);
    auto replacement = createReplacementState();
    for (const auto & [number, file_path] : files)
    {
        readBackupFile(file_path, origin, [&](const Block & block) { replacement->insertBlock(block); });
        replacement->finishFile();
    }

    persistAndPublish(*replacement, persist, context);
    clearOriginRequired();
    if (increment.load() < max_number)
        increment = max_number;
    LOG_INFO(getLogger("StorageSetOrJoinBase"), "Read the {} files of table {} again with the registered stored binary type origin {}",
             files.size(), table_id.getNameForLogs(), getBinaryTypeOriginName(origin));
}

void StorageSetOrJoinBase::restoreStoredBinaryTypeOriginAfterTruncate(ContextPtr context)
{
    stored_binary_type_origin.restoreAfterTruncate(
        *disk, path, getStorageID(), getWriteSettings(), context && context->getSettingsRef()[Setting::fsync_metadata]);
}

VirtualColumnsDescription StorageSetOrJoinBase::createVirtuals()
{
    VirtualColumnsDescription desc;
    desc.addEphemeral("_table", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "", VirtualsMaterializationPlace::Plan);
    desc.addEphemeral("_database", std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>()), "", VirtualsMaterializationPlace::Plan);
    return desc;
}


StorageSet::StorageSet(
    DiskPtr disk_,
    const String & relative_path_,
    const StorageID & table_id_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & comment,
    bool persistent_,
    bool is_original_create,
    std::optional<BinaryTypeOrigin> stored_binary_type_origin_to_stamp)
    : StorageSetOrJoinBase{disk_, relative_path_, table_id_, columns_, constraints_, comment, persistent_, is_original_create,
                           stored_binary_type_origin_to_stamp}
    , set(std::make_shared<Set>(SizeLimits(), 0, true))
{
    auto metadata_snapshot = getInMemoryMetadataPtr(CurrentThread::tryGetQueryContext(), false);
    Block header = metadata_snapshot->getSampleBlock();
    set->setHeader(header.getColumnsWithTypeAndName());

    restore();
}


SetPtr StorageSet::getSet() const
{
    checkOriginNotRequired();
    std::lock_guard lock(mutex);
    return set;
}

SetPtr StorageSet::createEmptySet() const
{
    auto empty_set = std::make_shared<Set>(SizeLimits(), 0, true);
    auto metadata_snapshot = getInMemoryMetadataPtr(CurrentThread::tryGetQueryContext(), false);
    empty_set->setHeader(metadata_snapshot->getSampleBlock().getColumnsWithTypeAndName());
    return empty_set;
}

namespace
{

class SetReplacementState final : public StorageSetOrJoinBase::ReplacementState
{
public:
    explicit SetReplacementState(SetPtr set_) : set(std::move(set_)) {}

    void insertBlock(const Block & block) override { set->insertFromBlock(block.getColumnsWithTypeAndName()); }
    void finishFile() override { set->finishInsert(); }

    SetPtr set;
};

}

std::unique_ptr<StorageSetOrJoinBase::ReplacementState> StorageSet::createReplacementState() const
{
    return std::make_unique<SetReplacementState>(createEmptySet());
}

void StorageSet::persistAndPublish(ReplacementState & state, const std::function<void()> & persist, ContextPtr)
{
    auto & replacement = assert_cast<SetReplacementState &>(state);
    std::lock_guard lock(mutex);
    persist();
    set = std::move(replacement.set);
}

void StorageSet::resetState()
{
    auto empty_set = createEmptySet();
    std::lock_guard lock(mutex);
    set = std::move(empty_set);
}


void StorageSet::insertBlock(const Block & block, ContextPtr)
{
    SetPtr current_set;
    {
        std::lock_guard lock(mutex);
        current_set = set;
    }
    current_set->insertFromBlock(block.getColumnsWithTypeAndName());
}

void StorageSet::finishInsert()
{
    SetPtr current_set;
    {
        std::lock_guard lock(mutex);
        current_set = set;
    }
    current_set->finishInsert();
}

size_t StorageSet::getSize(ContextPtr) const
{
    SetPtr current_set;
    {
        std::lock_guard lock(mutex);
        current_set = set;
    }
    return current_set->getTotalRowCount();
}

std::optional<UInt64> StorageSet::totalRows(ContextPtr) const
{
    checkOriginNotRequired();
    SetPtr current_set;
    {
        std::lock_guard lock(mutex);
        current_set = set;
    }
    return current_set->getTotalRowCount();
}

std::optional<UInt64> StorageSet::totalBytes(ContextPtr) const
{
    checkOriginNotRequired();
    SetPtr current_set;
    {
        std::lock_guard lock(mutex);
        current_set = set;
    }
    return current_set->getTotalByteCount();
}

void StorageSet::truncate(const ASTPtr &, const StorageMetadataPtr & metadata_snapshot, ContextPtr context, TableExclusiveLockHolder &)
{
    if (disk->existsDirectory(path))
        disk->removeRecursive(path);
    else
        LOG_INFO(getLogger("StorageSet"), "Path {} is already removed from disk {}", path, disk->getName());

    disk->createDirectories(path);
    disk->createDirectories(fs::path(path) / "tmp/");
    restoreStoredBinaryTypeOriginAfterTruncate(context);

    Block header = metadata_snapshot->getSampleBlock();

    increment = 0;

    auto new_set = std::make_shared<Set>(SizeLimits(), 0, true);
    new_set->setHeader(header.getColumnsWithTypeAndName());
    {
        std::lock_guard lock(mutex);
        set = new_set;
    }
    /// The data that needed a declared origin is removed.
    clearOriginRequired();
}


std::vector<std::pair<UInt64, String>> StorageSetOrJoinBase::listBackupFiles(UInt64 & max_number) const
{
    static const char * file_suffix = ".bin";
    static const auto file_suffix_size = strlen(".bin");

    std::vector<std::pair<UInt64, String>> backup_files;
    for (auto dir_it{disk->iterateDirectory(path)}; dir_it->isValid(); dir_it->next())
    {
        const auto & name = dir_it->name();
        const auto & file_path = dir_it->path();

        if (disk->existsFile(file_path)
            && endsWith(name, file_suffix)
            && disk->getFileSize(file_path) > 0)
        {
            /// Calculate the maximum number of available files with a backup to add the following files with large numbers.
            UInt64 file_num = parse<UInt64>(name.substr(0, name.size() - file_suffix_size));
            max_number = std::max(max_number, file_num);
            backup_files.emplace_back(file_num, file_path);
        }
    }

    /// In the same order as blocks were written.
    /// It may be important for storage Join, user expect to get the first row (unless `join_any_take_last_row` setting is set)
    /// but after restart we may have different order of blocks in memory.
    std::sort(backup_files.begin(), backup_files.end());
    return backup_files;
}

void StorageSetOrJoinBase::readBackupFile(
    const String & file_path, BinaryTypeOrigin origin, const std::function<void(const Block &)> & on_block) const
{
    auto backup_buf = disk->readFile(file_path, getReadSettings());
    CompressedReadBuffer compressed_backup_buf(*backup_buf);
    NativeReader backup_stream(compressed_backup_buf, 0);
    backup_stream.setStoredBinaryTypeOrigin(origin);

    for (Block block = backup_stream.read(); !block.empty(); block = backup_stream.read())
        on_block(block);
}

void StorageSetOrJoinBase::restore()
{
    /// Every numbered file is data that a finished insert published: a missing tmp directory does not prove that there is none.
    disk->createDirectories(fs::path(path) / "tmp");

    UInt64 max_number = 0;
    const auto backup_files = listBackupFiles(max_number);
    if (increment.load() < max_number)
        increment = max_number;

    const auto origin = stored_binary_type_origin.get();
    try
    {
        for (const auto & [number, file_path] : backup_files)
            restoreFromFile(file_path, origin);
    }
    catch (const BinaryTypeOriginRequiredException & e)
    {
        /// Only an ambiguous legacy binary type index, which a declared producer origin reads: the table is blocked until the registration
        /// of its origin, without the part of its data read so far. Any other error fails the load, as before.
        resetState();
        LOG_ERROR(getLogger("StorageSetOrJoinBase"), "Table {} needs the registration of its stored binary type origin to read its data: {}",
                  getStorageID().getNameForLogs(), e.message());
        std::lock_guard lock(origin_required_mutex);
        origin_required_error.reset(e.clone());
    }
}


void StorageSetOrJoinBase::restoreFromFile(const String & file_path, BinaryTypeOrigin origin)
{
    ContextPtr ctx = nullptr;
    ProfileInfo info;
    readBackupFile(file_path, origin, [&](const Block & block)
    {
        info.update(block.rows(), block.bytes());
        insertBlock(block, ctx);
    });

    finishInsert();

    /// TODO Add speed, compressed bytes, data volume in memory, compression ratio ... Generalize all statistics logging in project.
    LOG_INFO(getLogger("StorageSetOrJoinBase"), "Loaded from backup file {}. {} rows, {}. State has {} unique rows.",
        file_path, info.rows, ReadableSize(info.bytes), getSize(ctx));
}


void StorageSetOrJoinBase::rename(const String & new_path_to_table_data, const StorageID & new_table_id)
{
    /// Rename directory with data.
    disk->replaceFile(path, new_path_to_table_data);

    path = new_path_to_table_data;
    renameInMemory(new_table_id);
}


void registerStorageSet(StorageFactory & factory);
void registerStorageSet(StorageFactory & factory)
{
    factory.registerStorage("Set", [](const StorageFactory::Arguments & args)
    {
        if (!args.engine_args.empty())
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Engine {} doesn't support any arguments ({} given)",
                args.engine_name, args.engine_args.size());

        bool has_settings = args.storage_def->settings;
        SetSettings set_settings;
        if (has_settings)
            set_settings.loadFromQuery(*args.storage_def);

        DiskPtr disk = args.getContext()->getDisk(set_settings[SetSetting::disk]);
        return std::make_shared<StorageSet>(
            disk, args.relative_data_path, args.table_id, args.columns, args.constraints, args.comment, set_settings[SetSetting::persistent],
            args.is_original_create, args.stored_binary_type_origin_to_stamp);
    }, StorageFactory::StorageFeatures{ .supports_settings = true, .has_builtin_setting_fn = SetSettings::hasBuiltin, },
    Documentation{
        .description = R"DOCS_MD(
:::note
In ClickHouse Cloud, if your service was created with a version earlier than 25.4, you will need to set the compatibility to at least 25.4 using  `SET compatibility=25.4`.
:::

A data set that is always in RAM. It is intended for use on the right side of the `IN` operator (see the section "IN operators").

You can use `INSERT` to insert data in the table. New elements will be added to the data set, while duplicates will be ignored.
But you can't perform `SELECT` from the table. The only way to retrieve data is by using it in the right half of the `IN` operator.

Data is always located in RAM. For `INSERT`, the blocks of inserted data are also written to the directory of tables on the disk. When starting the server, this data is loaded to RAM. In other words, after restarting, the data remains in place.

For a rough server restart, the block of data on the disk might be lost or damaged. In the latter case, you may need to manually delete the file with damaged data.

### Limitations and settings {#join-limitations-and-settings}

When creating a table, the following settings are applied:

#### Persistent {#persistent}

Disables persistency for the Set and [Join](/reference/engines/table-engines/special/join) table engines.

Reduces the I/O overhead. Suitable for scenarios that pursue performance and do not require persistence.

Possible values:

- 1 — Enabled.
- 0 — Disabled.

Default value: `1`.
)DOCS_MD",
        .syntax = "ENGINE = Set",
        .related = {"Join"}});
}


}
