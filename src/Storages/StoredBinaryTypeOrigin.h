#pragma once

#include <Core/UUID.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <base/types.h>

#include <atomic>
#include <optional>
#include <string_view>

namespace DB
{

class IDisk;
class ReadBuffer;
class WriteBuffer;
struct ReadSettings;
struct StorageID;
struct WriteSettings;

/// The stored binary type origin (see BinaryTypeOrigin.h) of a table and of a data part: what the binary type headers in their stored
/// `Dynamic`, `JSON` and aggregation state data mean. It is kept in a small versioned record file of one of two kinds. Each record is
/// bound to the one entity it describes and never applies to another one; an absent record, an invalid one and a record of another
/// entity all mean Unknown, which reads only self-describing encodings and refuses the ambiguous legacy indexes.
///  - A table record lives in the data directory of a table with a UUID (`Atomic` and `Replicated` databases) and binds exactly that
///    non-nil UUID. It is written only by an original `CREATE` on this binary (OfficialCanonical, or the origin of the source table of
///    `CREATE ... AS` and `CLONE AS`), by a trusted restore of a backup that carries the record of the table, by the privileged one-time
///    registration `ALTER TABLE ... MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'` (accepted only from Unknown), and by a `Replicated`
///    table taking the origin of its ZooKeeper metadata. A table without a UUID (an `Ordinary` database) has no table record: it stays
///    Unknown and cannot be registered.
///  - A part record lives in the directory of a data part that left its table as raw files (detached, fetched from another table, restored
///    into a table of another origin), or of a mutation of such a part, and binds the total checksum of that part. It carries the origin
///    of the data of the part: a part with a record is read with it, any other part with the origin of its table. A part that comes into a
///    table as raw files (fetched from another table into detached/, attached from detached/, restored from a backup) is read with its
///    own record or with Unknown from the first read of its load on, never with the origin of the table, which is no proof of its data.
///    A part fetched from another table gets a record of the origin of that table, if its ZooKeeper metadata holds one. A part attached or
///    restored without a record of its own follows a table with an unset origin, is refused by a SentioLegacyV0 table, and gets a record
///    of the Unknown origin in any other table (MergeTreeData::admitStoredBinaryTypeOriginOfRawPart).
/// A part record is never a table record and the reverse: the loader of each kind rejects the other one.
inline constexpr auto STORED_BINARY_TYPE_ORIGIN_FILE_NAME = "sentio_binary_type_origin.txt";

enum class StoredBinaryTypeOriginRecordKind : uint8_t
{
    Table = 0,
    Part = 1,
};

struct StoredBinaryTypeOriginRecord
{
    static constexpr UInt32 FORMAT_VERSION = 1;

    StoredBinaryTypeOriginRecordKind kind = StoredBinaryTypeOriginRecordKind::Table;
    BinaryTypeOrigin origin = BinaryTypeOrigin::Unknown;
    /// A table record: the UUID of its table, never nil.
    UUID table_uuid = UUIDHelpers::Nil;
    /// A part record: the total checksum of its part (MergeTreeDataPartChecksums::getTotalChecksumHex, 32 uppercase hex digits).
    String part_checksum;

    /// Throws LOGICAL_ERROR for a nil UUID.
    static StoredBinaryTypeOriginRecord forTable(BinaryTypeOrigin origin, const UUID & table_uuid);
    /// Throws LOGICAL_ERROR for a checksum that is not 32 uppercase hex digits.
    static StoredBinaryTypeOriginRecord forPart(BinaryTypeOrigin origin, const String & part_checksum);

    void write(WriteBuffer & out) const;
    String toString() const;

    /// Reads exactly one complete record. An unknown format version, kind or origin, a table record with a nil UUID and a part record
    /// without a valid checksum throw CORRUPTED_DATA; anything else that is not exactly one record throws a parsing exception.
    static StoredBinaryTypeOriginRecord read(ReadBuffer & in);
    static StoredBinaryTypeOriginRecord parse(std::string_view text);
};

/// Whether a record applies to the entity it was found with, and if not, why.
enum class StoredBinaryTypeOriginRecordMatch : uint8_t
{
    Applies = 0,
    /// A part record where a table record is expected, or the reverse.
    WrongKind = 1,
    /// A table record found with a table that has no UUID.
    TableWithoutUUID = 2,
    /// The table record of another table: another UUID.
    OtherTable = 3,
    /// The part record of another part: another total checksum.
    OtherPart = 4,
};

std::string_view describeStoredBinaryTypeOriginRecordMatch(StoredBinaryTypeOriginRecordMatch match);

/// Whether `record` is the table record of the table with `table_uuid`: a table record with exactly this UUID, which is never nil.
StoredBinaryTypeOriginRecordMatch matchTableRecord(const StoredBinaryTypeOriginRecord & record, const UUID & table_uuid);

/// Whether `record` is the part record of the part with the total checksum `part_checksum`.
StoredBinaryTypeOriginRecordMatch matchPartRecord(const StoredBinaryTypeOriginRecord & record, const String & part_checksum);

/// What an existing replicated table does when ZooKeeper holds `replicated` as its stored binary type origin, while its local state is
/// `local` (the origin of its local table record; Unknown without one) and `local_record_ignored` (a record at the table path that does not
/// apply to the table). Only the one versioned transition is applied implicitly, never a relabel, a reset or a rebind of another record.
/// A new replica of an existing table, which has no data yet, takes the replicated origin directly instead.
enum class StoredBinaryTypeOriginReconciliation : uint8_t
{
    /// The same origin: nothing to do.
    Keep = 0,
    /// Unknown to SentioLegacyV0: the registration, applied by its ALTER_METADATA entry, or recovered when the replica stopped between
    /// applying the entry in ZooKeeper and locally. It is the only transition there is.
    ApplyRegistration = 1,
    /// Any other difference: the local record and the data stay as they are, and the table is read as Unknown until it is resolved.
    Conflict = 2,
    /// A record at the table path that does not apply to the table is never replaced implicitly: the table stays Unknown.
    KeepIgnoredRecord = 3,
};

StoredBinaryTypeOriginReconciliation decideStoredBinaryTypeOriginReconciliation(
    BinaryTypeOrigin local, bool local_record_ignored, BinaryTypeOrigin replicated);

/// The names of the origins in records, ZooKeeper metadata and statements, which are also the values of input_format_binary_type_origin:
/// `unknown`, `sentio_legacy_v0` and `official_canonical`.
std::string_view getBinaryTypeOriginName(BinaryTypeOrigin origin);

/// The origin with the name; any other name throws an exception with `error_code`.
BinaryTypeOrigin parseBinaryTypeOriginName(std::string_view name, int error_code);

/// The record in the file `path` of `disk`, if the file exists.
std::optional<StoredBinaryTypeOriginRecord> tryReadStoredBinaryTypeOriginRecord(const IDisk & disk, const String & path, const ReadSettings & settings);

/// Writes the record into the file `path` of `disk` atomically: into a temporary file next to it (finalized, and synced if `sync`), which
/// then replaces `path`. A failure leaves the previous file, or no file, in place.
void writeStoredBinaryTypeOriginRecord(
    IDisk & disk, const String & path, const StoredBinaryTypeOriginRecord & record, const WriteSettings & settings, bool sync);

/// The stored binary type origin of a table that keeps its data in one directory of one disk (`Log`, `TinyLog`, `StripeLog`, `Set` and
/// `Join`): its table record in that directory, with the rules of the table record of MergeTree. A table without a record is unset: it is
/// read as Unknown, and the registration states its origin. A record that does not apply to the table (of another table, of the other
/// kind, or found with a table without a UUID) is ignored: the table is read as Unknown, and its origin cannot be registered. A record of
/// the Unknown origin is a statement as well: such a table is not unset, and its origin cannot be registered either.
class StoredBinaryTypeOriginOfTableDirectory
{
public:
    /// Called by the constructor of the storage, in every mode, before it creates or changes anything in its directory.
    ///  - An original CREATE (`is_original_create`, see InterpreterCreateQuery::isOriginalCreate) requires that the directory does not exist
    ///    on `disk`, with or without a UUID, and whether or not it stamps the table: a CREATE never takes over existing content (ATTACH
    ///    loads it), so everything in the directory belongs to this CREATE and a failed one removes only what it created. Returns
    ///    `origin_to_stamp` (InterpreterCreateQuery::getStoredBinaryTypeOriginToStamp) for the storage to stamp the new table with once it
    ///    has created its directory (stampNewTable); nothing for a table without a UUID, which has no record, or without an origin to
    ///    stamp, which stays unset.
    ///  - Every other mode loads the record (a malformed one throws) and returns nothing; it has no origin to stamp.
    std::optional<BinaryTypeOrigin> load(
        const IDisk & disk,
        const String & table_path,
        const StorageID & table_id,
        bool is_original_create,
        std::optional<BinaryTypeOrigin> origin_to_stamp,
        const ReadSettings & read_settings);

    /// Writes the record of the new table with the origin that load returned, after the storage has created its directory. A table on a
    /// read-only or write-once disk is not stamped and stays unset.
    void stampNewTable(
        IDisk & disk, const String & table_path, const StorageID & table_id, BinaryTypeOrigin origin_to_stamp, const WriteSettings & write_settings,
        bool sync);

    /// Throws unless `origin_to_register` can be registered: it is SentioLegacyV0, and the table has a UUID and no record at all.
    void checkCanBeRegistered(BinaryTypeOrigin origin_to_register, const StorageID & table_id) const;

    /// The registration (`ALTER TABLE ... MODIFY SENTIO BINARY TYPE ORIGIN`): checks it again and writes the record atomically. The caller
    /// holds the lock that excludes every reader that takes the origin and every writer of the data of the table.
    void registerOrigin(
        IDisk & disk, const String & table_path, const StorageID & table_id, BinaryTypeOrigin origin_to_register,
        const WriteSettings & write_settings, bool sync);

    BinaryTypeOrigin get() const { return origin.load(); }
    bool isUnset() const { return !record_exists.load(); }

    /// The origin for messages: its name, and why it is Unknown.
    String describe() const;

private:
    std::atomic<BinaryTypeOrigin> origin{BinaryTypeOrigin::Unknown};
    /// A record is in the directory, whether it applies to the table or not.
    std::atomic<bool> record_exists{false};
    std::atomic<bool> record_ignored{false};
};

}
