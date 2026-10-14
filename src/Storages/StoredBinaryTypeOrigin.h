#pragma once

#include <Core/UUID.h>
#include <DataTypes/BinaryTypeOrigin.h>
#include <base/types.h>

#include <optional>
#include <string_view>

namespace DB
{

class IDisk;
class ReadBuffer;
class WriteBuffer;
struct ReadSettings;
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
///    of the data of the part: a part with a record is read with it, any other part with the origin of its table.
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

}
