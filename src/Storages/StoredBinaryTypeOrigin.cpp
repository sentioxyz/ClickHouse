#include <Storages/StoredBinaryTypeOrigin.h>

#include <Disks/IDisk.h>
#include <Interpreters/StorageID.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <fmt/format.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int TABLE_ALREADY_EXISTS;
    extern const int TABLE_IS_READ_ONLY;
}

namespace
{

/// A record is four short lines. Reading stops after this many bytes, so an untrusted file (attached or restored) is never accumulated
/// beyond it; a longer input is refused as a whole.
constexpr size_t MAX_RECORD_SIZE = 4096;
/// The format version is at most this many decimal digits, without a leading zero; it is compared exactly, never wrapped.
constexpr size_t MAX_VERSION_DIGITS = 10;
constexpr size_t UUID_TEXT_SIZE = 36;
constexpr size_t PART_CHECKSUM_SIZE = 32;

constexpr std::string_view VERSION_PREFIX = "sentio binary type origin record version: ";
constexpr std::string_view KIND_PREFIX = "kind: ";
constexpr std::string_view ORIGIN_PREFIX = "origin: ";
constexpr std::string_view TABLE_UUID_PREFIX = "table uuid: ";
constexpr std::string_view PART_CHECKSUM_PREFIX = "part checksum: ";

bool isPartChecksum(std::string_view checksum)
{
    if (checksum.size() != PART_CHECKSUM_SIZE)
        return false;
    for (char c : checksum)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

std::string_view getRecordKindName(StoredBinaryTypeOriginRecordKind kind)
{
    switch (kind)
    {
        case StoredBinaryTypeOriginRecordKind::Table:
            return "table";
        case StoredBinaryTypeOriginRecordKind::Part:
            return "part";
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected stored binary type origin record kind {}", static_cast<UInt64>(kind));
}

[[noreturn]] void throwMalformedRecord(std::string_view what)
{
    throw Exception(ErrorCodes::CORRUPTED_DATA, "Malformed stored binary type origin record: {}", what);
}

/// Takes the next line of `text`, which must end with a newline and start with `prefix`, and returns what follows the prefix.
std::string_view takeRecordLine(std::string_view & text, std::string_view prefix)
{
    const size_t end = text.find('\n');
    if (end == std::string_view::npos)
        throwMalformedRecord(fmt::format("expected a line '{}...' ending with a newline", prefix));
    const std::string_view line = text.substr(0, end);
    text.remove_prefix(end + 1);
    if (!line.starts_with(prefix))
        throwMalformedRecord(fmt::format("expected a line '{}...'", prefix));
    return line.substr(prefix.size());
}

void checkRecordVersion(std::string_view token)
{
    if (token.empty() || token.size() > MAX_VERSION_DIGITS)
        throwMalformedRecord("the format version is not 1 to 10 decimal digits");
    if (token.size() > 1 && token.front() == '0')
        throwMalformedRecord("the format version has a leading zero");
    UInt64 version = 0;
    for (char c : token)
    {
        if (c < '0' || c > '9')
            throwMalformedRecord("the format version is not decimal digits");
        /// At most 10 digits: the value stays far below the range of UInt64.
        version = version * 10 + static_cast<UInt64>(c - '0');
    }
    if (version != StoredBinaryTypeOriginRecord::FORMAT_VERSION)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Unknown format version {} of a stored binary type origin record", token);
}

UUID parseCanonicalUUID(std::string_view token)
{
    if (token.size() != UUID_TEXT_SIZE)
        throwMalformedRecord("the table UUID is not 36 characters");
    ReadBufferFromMemory in(token);
    UUID uuid;
    if (!tryReadUUIDText(uuid, in) || !in.eof())
        throwMalformedRecord("the table UUID is not a UUID");
    WriteBufferFromOwnString canonical;
    writeUUIDText(uuid, canonical);
    if (canonical.str() != token)
        throwMalformedRecord("the table UUID is not in the canonical form");
    return uuid;
}

}

std::string_view getBinaryTypeOriginName(BinaryTypeOrigin origin)
{
    switch (origin)
    {
        case BinaryTypeOrigin::Unknown:
            return "unknown";
        case BinaryTypeOrigin::SentioLegacyV0:
            return "sentio_legacy_v0";
        case BinaryTypeOrigin::OfficialCanonical:
            return "official_canonical";
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected binary type origin {}", static_cast<UInt64>(origin));
}

BinaryTypeOrigin parseBinaryTypeOriginName(std::string_view name, int error_code)
{
    for (auto origin : {BinaryTypeOrigin::Unknown, BinaryTypeOrigin::SentioLegacyV0, BinaryTypeOrigin::OfficialCanonical})
    {
        if (name == getBinaryTypeOriginName(origin))
            return origin;
    }
    throw Exception(error_code, "Unknown binary type origin '{}': expected 'unknown', 'sentio_legacy_v0' or 'official_canonical'", name);
}

StoredBinaryTypeOriginRecord StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin origin, const UUID & table_uuid)
{
    if (table_uuid == UUIDHelpers::Nil)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "A stored binary type origin record of a table must bind the UUID of the table, but it is nil");
    StoredBinaryTypeOriginRecord record;
    record.kind = StoredBinaryTypeOriginRecordKind::Table;
    record.origin = origin;
    record.table_uuid = table_uuid;
    return record;
}

StoredBinaryTypeOriginRecord StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin origin, const String & part_checksum)
{
    if (!isPartChecksum(part_checksum))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "A stored binary type origin record of a part must bind the total checksum of the part, got '{}'", part_checksum);
    StoredBinaryTypeOriginRecord record;
    record.kind = StoredBinaryTypeOriginRecordKind::Part;
    record.origin = origin;
    record.part_checksum = part_checksum;
    return record;
}

void StoredBinaryTypeOriginRecord::write(WriteBuffer & out) const
{
    writeString(VERSION_PREFIX, out);
    writeText(FORMAT_VERSION, out);
    writeChar('\n', out);
    writeString(KIND_PREFIX, out);
    writeString(getRecordKindName(kind), out);
    writeChar('\n', out);
    writeString(ORIGIN_PREFIX, out);
    writeString(getBinaryTypeOriginName(origin), out);
    writeChar('\n', out);
    if (kind == StoredBinaryTypeOriginRecordKind::Table)
    {
        writeString(TABLE_UUID_PREFIX, out);
        writeUUIDText(table_uuid, out);
    }
    else
    {
        writeString(PART_CHECKSUM_PREFIX, out);
        writeString(part_checksum, out);
    }
    writeChar('\n', out);
}

String StoredBinaryTypeOriginRecord::toString() const
{
    WriteBufferFromOwnString out;
    write(out);
    return out.str();
}

StoredBinaryTypeOriginRecord StoredBinaryTypeOriginRecord::read(ReadBuffer & in)
{
    /// Read at most one byte more than a record can have: the extra byte shows that the input is longer, and is refused, so a prefix of a
    /// longer input is never taken as the record.
    String text(MAX_RECORD_SIZE + 1, '\0');
    const size_t size = in.read(text.data(), text.size());
    if (size > MAX_RECORD_SIZE)
        throwMalformedRecord(fmt::format("longer than {} bytes", MAX_RECORD_SIZE));
    text.resize(size);
    return parse(text);
}

StoredBinaryTypeOriginRecord StoredBinaryTypeOriginRecord::parse(std::string_view text)
{
    if (text.size() > MAX_RECORD_SIZE)
        throwMalformedRecord(fmt::format("longer than {} bytes", MAX_RECORD_SIZE));

    StoredBinaryTypeOriginRecord record;
    checkRecordVersion(takeRecordLine(text, VERSION_PREFIX));

    const std::string_view kind_name = takeRecordLine(text, KIND_PREFIX);
    if (kind_name == getRecordKindName(StoredBinaryTypeOriginRecordKind::Table))
        record.kind = StoredBinaryTypeOriginRecordKind::Table;
    else if (kind_name == getRecordKindName(StoredBinaryTypeOriginRecordKind::Part))
        record.kind = StoredBinaryTypeOriginRecordKind::Part;
    else
        throw Exception(ErrorCodes::CORRUPTED_DATA, "Unknown kind '{}' of a stored binary type origin record", kind_name);

    record.origin = parseBinaryTypeOriginName(takeRecordLine(text, ORIGIN_PREFIX), ErrorCodes::CORRUPTED_DATA);

    if (record.kind == StoredBinaryTypeOriginRecordKind::Table)
    {
        record.table_uuid = parseCanonicalUUID(takeRecordLine(text, TABLE_UUID_PREFIX));
        if (record.table_uuid == UUIDHelpers::Nil)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "A stored binary type origin record of a table binds a nil UUID");
    }
    else
    {
        const std::string_view checksum = takeRecordLine(text, PART_CHECKSUM_PREFIX);
        if (!isPartChecksum(checksum))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "A stored binary type origin record of a part binds an invalid checksum '{}'", checksum);
        record.part_checksum = String(checksum);
    }

    if (!text.empty())
        throwMalformedRecord("trailing bytes after the record");
    return record;
}

std::string_view describeStoredBinaryTypeOriginRecordMatch(StoredBinaryTypeOriginRecordMatch match)
{
    switch (match)
    {
        case StoredBinaryTypeOriginRecordMatch::Applies:
            return "applies";
        case StoredBinaryTypeOriginRecordMatch::WrongKind:
            return "a record of the other kind";
        case StoredBinaryTypeOriginRecordMatch::TableWithoutUUID:
            return "a table record found with a table without a UUID";
        case StoredBinaryTypeOriginRecordMatch::OtherTable:
            return "the record of another table";
        case StoredBinaryTypeOriginRecordMatch::OtherPart:
            return "the record of another part";
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected stored binary type origin record match {}", static_cast<UInt64>(match));
}

StoredBinaryTypeOriginRecordMatch matchTableRecord(const StoredBinaryTypeOriginRecord & record, const UUID & table_uuid)
{
    if (record.kind != StoredBinaryTypeOriginRecordKind::Table)
        return StoredBinaryTypeOriginRecordMatch::WrongKind;
    if (table_uuid == UUIDHelpers::Nil)
        return StoredBinaryTypeOriginRecordMatch::TableWithoutUUID;
    if (record.table_uuid != table_uuid)
        return StoredBinaryTypeOriginRecordMatch::OtherTable;
    return StoredBinaryTypeOriginRecordMatch::Applies;
}

StoredBinaryTypeOriginRecordMatch matchPartRecord(const StoredBinaryTypeOriginRecord & record, const String & part_checksum)
{
    if (record.kind != StoredBinaryTypeOriginRecordKind::Part)
        return StoredBinaryTypeOriginRecordMatch::WrongKind;
    if (record.part_checksum != part_checksum)
        return StoredBinaryTypeOriginRecordMatch::OtherPart;
    return StoredBinaryTypeOriginRecordMatch::Applies;
}

StoredBinaryTypeOriginReconciliation decideStoredBinaryTypeOriginReconciliation(
    std::optional<BinaryTypeOrigin> local, bool local_record_ignored, std::optional<BinaryTypeOrigin> replicated)
{
    if (local_record_ignored)
        return StoredBinaryTypeOriginReconciliation::KeepIgnoredRecord;
    if (local == replicated)
        return StoredBinaryTypeOriginReconciliation::Keep;
    if (!local && replicated == BinaryTypeOrigin::SentioLegacyV0)
        return StoredBinaryTypeOriginReconciliation::ApplyRegistration;
    return StoredBinaryTypeOriginReconciliation::Conflict;
}

std::string_view describeStatedBinaryTypeOrigin(std::optional<BinaryTypeOrigin> origin)
{
    if (!origin)
        return "none";
    return getBinaryTypeOriginName(*origin);
}

std::optional<StoredBinaryTypeOriginRecord> tryReadStoredBinaryTypeOriginRecord(const IDisk & disk, const String & path, const ReadSettings & settings)
{
    auto in = disk.readFileIfExists(path, settings);
    if (!in)
        return {};
    return StoredBinaryTypeOriginRecord::read(*in);
}

void writeStoredBinaryTypeOriginRecord(
    IDisk & disk, const String & path, const StoredBinaryTypeOriginRecord & record, const WriteSettings & settings, bool sync)
{
    const String tmp_path = path + ".tmp";
    {
        auto out = disk.writeFile(tmp_path, 4096, WriteMode::Rewrite, settings);
        record.write(*out);
        out->finalize();
        if (sync)
            out->sync();
    }
    disk.replaceFile(tmp_path, path);
}

std::optional<BinaryTypeOrigin> StoredBinaryTypeOriginOfTableDirectory::load(
    const IDisk & disk,
    const String & table_path,
    const StorageID & table_id,
    bool is_original_create,
    std::optional<BinaryTypeOrigin> origin_to_stamp,
    const ReadSettings & read_settings)
{
    if (is_original_create)
    {
        /// On the disk of the table, which may not be the one that InterpreterCreateQuery checks for an existing directory.
        if (disk.existsFileOrDirectory(table_path))
            throw Exception(ErrorCodes::TABLE_ALREADY_EXISTS,
                            "Directory {} for the data of table {} already exists on disk {}, probably left by an unclean DROP TABLE or by "
                            "manual intervention. A CREATE TABLE does not take over existing content: remove the directory by hand, or use "
                            "ATTACH TABLE", table_path, table_id.getNameForLogs(), disk.getName());

        if (table_id.uuid == UUIDHelpers::Nil)
            return {};
        return origin_to_stamp;
    }

    if (origin_to_stamp)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Table {} gets a stored binary type origin to stamp without an original CREATE",
                        table_id.getNameForLogs());

    const String record_path = fs::path(table_path) / STORED_BINARY_TYPE_ORIGIN_FILE_NAME;
    const auto record = tryReadStoredBinaryTypeOriginRecord(disk, record_path, read_settings);
    record_exists = record.has_value();
    if (!record)
        return {};

    const auto match = matchTableRecord(*record, table_id.uuid);
    if (match == StoredBinaryTypeOriginRecordMatch::Applies)
    {
        origin = record->origin;
    }
    else
    {
        LOG_WARNING(getLogger("StoredBinaryTypeOrigin"), "Ignoring the stored binary type origin record of table {}: it is {}. The table is Unknown",
                    table_id.getNameForLogs(), describeStoredBinaryTypeOriginRecordMatch(match));
        record_ignored = true;
    }
    return {};
}

void StoredBinaryTypeOriginOfTableDirectory::stampNewTable(
    IDisk & disk, const String & table_path, const StorageID & table_id, BinaryTypeOrigin origin_to_stamp, const WriteSettings & write_settings,
    bool sync)
{
    if (disk.isReadOnly() || disk.isWriteOnce())
    {
        LOG_INFO(getLogger("StoredBinaryTypeOrigin"), "Table {} has no writable disk for its stored binary type origin record; it stays unset",
                 table_id.getNameForLogs());
        return;
    }

    const String record_path = fs::path(table_path) / STORED_BINARY_TYPE_ORIGIN_FILE_NAME;
    writeStoredBinaryTypeOriginRecord(disk, record_path, StoredBinaryTypeOriginRecord::forTable(origin_to_stamp, table_id.uuid), write_settings, sync);
    origin = origin_to_stamp;
    record_exists = true;
    record_ignored = false;
}

void StoredBinaryTypeOriginOfTableDirectory::checkCanBeRegistered(BinaryTypeOrigin origin_to_register, const StorageID & table_id) const
{
    if (origin_to_register != BinaryTypeOrigin::SentioLegacyV0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Only sentio_legacy_v0 can be registered as the stored binary type origin of a table, not {}",
                        getBinaryTypeOriginName(origin_to_register));
    if (table_id.uuid == UUIDHelpers::Nil)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "Table {} has no UUID (its database has the Ordinary engine): it cannot have a stored binary type origin record, so its "
                        "origin cannot be registered", table_id.getNameForLogs());
    if (!isUnset())
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "Table {} has the stored binary type origin {}: only the origin of a table without a stored binary type origin record "
                        "can be registered", table_id.getNameForLogs(), describe());
}

void StoredBinaryTypeOriginOfTableDirectory::registerOrigin(
    IDisk & disk, const String & table_path, const StorageID & table_id, BinaryTypeOrigin origin_to_register,
    const WriteSettings & write_settings, bool sync)
{
    checkCanBeRegistered(origin_to_register, table_id);
    if (disk.isReadOnly() || disk.isWriteOnce())
        throw Exception(ErrorCodes::TABLE_IS_READ_ONLY, "Table {} has no writable disk for its stored binary type origin record",
                        table_id.getNameForLogs());

    const String record_path = fs::path(table_path) / STORED_BINARY_TYPE_ORIGIN_FILE_NAME;
    writeStoredBinaryTypeOriginRecord(disk, record_path, StoredBinaryTypeOriginRecord::forTable(origin_to_register, table_id.uuid), write_settings, sync);
    origin = origin_to_register;
    record_exists = true;
    record_ignored = false;
    LOG_INFO(getLogger("StoredBinaryTypeOrigin"), "Registered the stored binary type origin {} of table {}",
             getBinaryTypeOriginName(origin_to_register), table_id.getNameForLogs());
}

void StoredBinaryTypeOriginOfTableDirectory::restoreAfterTruncate(
    IDisk & disk, const String & table_path, const StorageID & table_id, const WriteSettings & write_settings, bool sync)
{
    if (!record_exists.load() || record_ignored.load())
    {
        origin = BinaryTypeOrigin::Unknown;
        record_exists = false;
        record_ignored = false;
        return;
    }

    const String record_path = fs::path(table_path) / STORED_BINARY_TYPE_ORIGIN_FILE_NAME;
    writeStoredBinaryTypeOriginRecord(disk, record_path, StoredBinaryTypeOriginRecord::forTable(origin.load(), table_id.uuid), write_settings, sync);
}

String StoredBinaryTypeOriginOfTableDirectory::describe() const
{
    if (record_ignored.load())
        return "unknown (with a record that does not apply to the table)";
    if (!record_exists.load())
        return "unknown (without a record)";
    return String(getBinaryTypeOriginName(origin.load()));
}

}
