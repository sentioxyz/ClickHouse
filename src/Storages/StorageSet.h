#pragma once

#include <Interpreters/Context_fwd.h>
#include <Storages/SetOrJoinWritersGate.h>
#include <Storages/StorageWithCommonVirtualColumns.h>
#include <Storages/StoredBinaryTypeOrigin.h>
#include <Common/Exception.h>

#include <functional>
#include <mutex>


namespace DB
{

class IDisk;
using DiskPtr = std::shared_ptr<IDisk>;

class Set;
using SetPtr = std::shared_ptr<Set>;


/** Common part of StorageSet and StorageJoin.
  */
class StorageSetOrJoinBase : public StorageWithCommonVirtualColumns
{
    friend class SetOrJoinSink;

public:
    static VirtualColumnsDescription createVirtuals();

    void rename(const String & new_path_to_table_data, const StorageID & new_table_id) override;

    SinkToStoragePtr write(const ASTPtr & query, const StorageMetadataPtr & /*metadata_snapshot*/, ContextPtr context, bool async_insert) override;

    bool storesDataOnDisk() const override { return true; }
    Strings getDataPaths() const override { return {path}; }

    /// The registration of the stored binary type origin of the table (registerStoredBinaryTypeOrigin), and the default for every other
    /// command.
    void checkAlterIsPossible(const AlterCommands & commands, ContextPtr context) const override;
    void alter(const AlterCommands & params, ContextPtr context, AlterLockHolder & alter_lock_holder) override;

    /// A state of the data built aside from the current one: the registration reads the files of the table into it and publishes it in
    /// place of the current state, which it does not touch before.
    class ReplacementState
    {
    public:
        virtual ~ReplacementState() = default;
        virtual void insertBlock(const Block & block) = 0;
        /// After the blocks of one file, as after one insert.
        virtual void finishFile() = 0;
    };

protected:
    /// `is_original_create` and `stored_binary_type_origin_to_stamp`: see StoredBinaryTypeOriginOfTableDirectory::load.
    StorageSetOrJoinBase(
        DiskPtr disk_,
        const String & relative_path_,
        const StorageID & table_id_,
        const ColumnsDescription & columns_,
        const ConstraintsDescription & constraints_,
        const String & comment,
        bool persistent_,
        bool is_original_create,
        std::optional<BinaryTypeOrigin> stored_binary_type_origin_to_stamp);

    DiskPtr disk;
    String path;
    bool persistent;

    std::atomic<UInt64> increment = 0;    /// For the backup file names.

    /// The table record of the stored binary type origin of the data of the table (Storages/StoredBinaryTypeOrigin.h).
    StoredBinaryTypeOriginOfTableDirectory stored_binary_type_origin;

    /// Restore from backup: reads every numbered file, in the order they were written, with the stored binary type origin of the table. A
    /// file that needs a declared origin (BinaryTypeOriginRequiredException: an ambiguous legacy binary type index) leaves the table
    /// blocked: it loads without its data and refuses every use of it (checkOriginNotRequired) until the registration of its origin reads
    /// the files again. Any other error fails the load.
    void restore();

    /// Throws while the table is blocked (see restore): a copy of the error of the load, with a hint. Every use of the values of the table
    /// checks it (inserts, reads, the set or the join it hands out, its size, mutations); its metadata does not.
    void checkOriginNotRequired() const;

    /// The table is no longer blocked: its data was read again with a declared origin, or removed.
    void clearOriginRequired() noexcept;

    /// Whether the table is blocked (see restore).
    bool isOriginRequired() const;

    /// The token of a new writer (SetOrJoinWritersGate), after checkOriginNotRequired. It waits while the registration of the origin runs.
    SetOrJoinWritersGate::TokenPtr enterAsWriter(ContextPtr context);

    /// A sink of the table that holds `token` until it is destroyed.
    SinkToStoragePtr makeSink(SetOrJoinWritersGate::TokenPtr token, const StorageMetadataPtr & metadata_snapshot, ContextPtr context);

    /// After TRUNCATE removed the directory of the table: writes the record of its origin again, so that the table keeps its identity.
    void restoreStoredBinaryTypeOriginAfterTruncate(ContextPtr context);

private:
    void restoreFromFile(const String & file_path, BinaryTypeOrigin origin);

    /// The numbered files with the data of the table, in the order they were written, and the greatest number among them.
    std::vector<std::pair<UInt64, String>> listBackupFiles(UInt64 & max_number) const;

    /// Reads the blocks of one file with the stored binary type origin `origin`.
    void readBackupFile(const String & file_path, BinaryTypeOrigin origin, const std::function<void(const Block &)> & on_block) const;

    /// The registration of the stored binary type origin: keeps the writers out and waits for them (SetOrJoinWritersGate), reads all the
    /// files again, in order, with the registered origin into a new state, persists the record and publishes the new state, without
    /// throwing once the record is written, and clears the error of a blocked table. Any failure before leaves the files, the state, its
    /// error and the numbering as they were. A table without persistence that is not blocked only persists the record: it keeps its state.
    void registerStoredBinaryTypeOrigin(BinaryTypeOrigin origin, ContextPtr context);

    virtual std::unique_ptr<ReplacementState> createReplacementState() const = 0;
    /// Takes the exclusive lock of the state (which may fail before anything changes), calls `persist` (which may fail: nothing changes
    /// either), then publishes `state` in place of the current state without throwing.
    virtual void persistAndPublish(ReplacementState & state, const std::function<void()> & persist, ContextPtr context) = 0;
    /// Replaces the state with an empty one: the state of a blocked table.
    virtual void resetState() = 0;
    /// The lock that the registration holds against other changes of the files of the table (the mutations of Join), taken once the
    /// writers have left.
    virtual std::unique_lock<std::mutex> lockForRegistration() { return {}; }

    /// Insert the block into the state.
    virtual void insertBlock(const Block & block, ContextPtr context) = 0;
    /// Call after all blocks were inserted.
    virtual void finishInsert() = 0;
    virtual size_t getSize(ContextPtr context) const = 0;

    SetOrJoinWritersGate writers_gate;

    mutable std::mutex origin_required_mutex;
    std::unique_ptr<Exception> origin_required_error TSA_GUARDED_BY(origin_required_mutex);
};


/** Lets you save the set for later use on the right side of the IN statement.
  * When inserted into a table, the data will be inserted into the set,
  *  and also written to a file-backup, for recovery after a restart.
  * Reading from the table is not possible directly - it is possible to specify only the right part of the IN statement.
  */
class StorageSet final : public StorageSetOrJoinBase
{
public:
    StorageSet(
        DiskPtr disk_,
        const String & relative_path_,
        const StorageID & table_id_,
        const ColumnsDescription & columns_,
        const ConstraintsDescription & constraints_,
        const String & comment,
        bool persistent_,
        bool is_original_create = false,
        std::optional<BinaryTypeOrigin> stored_binary_type_origin_to_stamp = {});

    String getName() const override { return "Set"; }

    /// Access the insides.
    SetPtr getSet() const;

    void truncate(const ASTPtr &, const StorageMetadataPtr & metadata_snapshot, ContextPtr, TableExclusiveLockHolder &) override;

    std::optional<UInt64> totalRows(ContextPtr query_context) const override;
    std::optional<UInt64> totalBytes(ContextPtr query_context) const override;

private:
    /// Allows to concurrently truncate the set and work (read/fill) the existing set.
    mutable std::mutex mutex;
    SetPtr set TSA_GUARDED_BY(mutex);

    void insertBlock(const Block & block, ContextPtr) override;
    void finishInsert() override;
    size_t getSize(ContextPtr) const override;

    SetPtr createEmptySet() const;
    std::unique_ptr<ReplacementState> createReplacementState() const override;
    void persistAndPublish(ReplacementState & state, const std::function<void()> & persist, ContextPtr context) override;
    void resetState() override;
};

}
