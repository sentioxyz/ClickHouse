#include <gtest/gtest.h>

#include <DataTypes/BinaryTypeOrigin.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <Storages/StoredBinaryTypeOrigin.h>
#include <Common/Exception.h>

#include <string>
#include <tuple>
#include <utility>

/// The record of the stored binary type origin of a table and of a part: its exact text, its two kinds, and whom it applies to. A table
/// record binds exactly one non-nil table UUID, a part record exactly one part checksum; a nil UUID is never a wildcard, a record of the
/// other kind never applies, and a table without a UUID has no table record. The expected texts are written out here, not produced by the
/// code under test.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int LOGICAL_ERROR;
extern const int BAD_ARGUMENTS;
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

const String TABLE_UUID_TEXT = "1f8b9e2c-4d3a-4c6b-9e7f-0a1b2c3d4e5f";
const String OTHER_UUID_TEXT = "1f8b9e2c-4d3a-4c6b-9e7f-0a1b2c3d4e60";
const String PART_CHECKSUM = "0123456789ABCDEF0123456789ABCDEF";
const String OTHER_PART_CHECKSUM = "0123456789ABCDEF0123456789ABCDEE";

template <typename F>
std::pair<int, String> errorOf(F && f)
{
    try
    {
        f();
    }
    catch (const Exception & e)
    {
        return {e.code(), e.message()};
    }
    return {0, "no exception"};
}

}

TEST(StoredBinaryTypeOrigin, OriginNames)
{
    EXPECT_EQ(getBinaryTypeOriginName(BinaryTypeOrigin::Unknown), "unknown");
    EXPECT_EQ(getBinaryTypeOriginName(BinaryTypeOrigin::SentioLegacyV0), "sentio_legacy_v0");
    EXPECT_EQ(getBinaryTypeOriginName(BinaryTypeOrigin::OfficialCanonical), "official_canonical");
    for (auto origin : {BinaryTypeOrigin::Unknown, BinaryTypeOrigin::SentioLegacyV0, BinaryTypeOrigin::OfficialCanonical})
        EXPECT_EQ(parseBinaryTypeOriginName(getBinaryTypeOriginName(origin), ErrorCodes::BAD_ARGUMENTS), origin);

    EXPECT_EQ(errorOf([] { parseBinaryTypeOriginName("legacy", ErrorCodes::BAD_ARGUMENTS); }).first, ErrorCodes::BAD_ARGUMENTS);
    EXPECT_EQ(errorOf([] { parseBinaryTypeOriginName("Unknown", ErrorCodes::CORRUPTED_DATA); }).first, ErrorCodes::CORRUPTED_DATA);
}

TEST(StoredBinaryTypeOrigin, TableRecordText)
{
    const auto record = StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, uuidOf(TABLE_UUID_TEXT));
    const String text = "sentio binary type origin record version: 1\n"
                        "kind: table\n"
                        "origin: official_canonical\n"
                        "table uuid: " + TABLE_UUID_TEXT + "\n";
    EXPECT_EQ(record.toString(), text);

    const auto parsed = StoredBinaryTypeOriginRecord::parse(text);
    EXPECT_EQ(parsed.kind, StoredBinaryTypeOriginRecordKind::Table);
    EXPECT_EQ(parsed.origin, BinaryTypeOrigin::OfficialCanonical);
    EXPECT_EQ(parsed.table_uuid, uuidOf(TABLE_UUID_TEXT));
    EXPECT_TRUE(parsed.part_checksum.empty());
}

TEST(StoredBinaryTypeOrigin, PartRecordText)
{
    const auto record = StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin::SentioLegacyV0, PART_CHECKSUM);
    const String text = "sentio binary type origin record version: 1\n"
                        "kind: part\n"
                        "origin: sentio_legacy_v0\n"
                        "part checksum: " + PART_CHECKSUM + "\n";
    EXPECT_EQ(record.toString(), text);

    const auto parsed = StoredBinaryTypeOriginRecord::parse(text);
    EXPECT_EQ(parsed.kind, StoredBinaryTypeOriginRecordKind::Part);
    EXPECT_EQ(parsed.origin, BinaryTypeOrigin::SentioLegacyV0);
    EXPECT_EQ(parsed.table_uuid, UUIDHelpers::Nil);
    EXPECT_EQ(parsed.part_checksum, PART_CHECKSUM);
}

TEST(StoredBinaryTypeOrigin, RecordsCannotBeMadeWithoutTheirIdentity)
{
    EXPECT_EQ(errorOf([] { StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, UUIDHelpers::Nil); }).first,
              ErrorCodes::LOGICAL_ERROR);
    for (const String & checksum : {String(), String("0123456789abcdef0123456789abcdef"), String("0123456789ABCDEF"), PART_CHECKSUM + "0"})
        EXPECT_EQ(errorOf([&] { StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin::SentioLegacyV0, checksum); }).first,
                  ErrorCodes::LOGICAL_ERROR) << checksum;
}

TEST(StoredBinaryTypeOrigin, InvalidRecordsAreRefused)
{
    const String head = "sentio binary type origin record version: 1\n";
    const String table_tail = "kind: table\norigin: official_canonical\ntable uuid: " + TABLE_UUID_TEXT + "\n";
    const String cases[] = {
        /// a table record with a nil UUID, with an uppercase UUID, with a UUID without dashes
        head + "kind: table\norigin: official_canonical\ntable uuid: 00000000-0000-0000-0000-000000000000\n",
        head + "kind: table\norigin: official_canonical\ntable uuid: 1F8B9E2C-4D3A-4C6B-9E7F-0A1B2C3D4E5F\n",
        head + "kind: table\norigin: official_canonical\ntable uuid: 1f8b9e2c4d3a4c6b9e7f0a1b2c3d4e5f\n",
        /// an unknown kind and an unknown origin
        head + "kind: database\norigin: official_canonical\ntable uuid: " + TABLE_UUID_TEXT + "\n",
        head + "kind: table\norigin: legacy\ntable uuid: " + TABLE_UUID_TEXT + "\n",
        /// unknown and malformed format versions: 2, 0, a version that wraps to 1 in 32 bits (4294967297 = 2^32 + 1), one that wraps in
        /// 64 bits (18446744073709551617 = 2^64 + 1), 11 digits, a leading zero, a sign, an empty one
        "sentio binary type origin record version: 2\n" + table_tail,
        "sentio binary type origin record version: 0\n" + table_tail,
        "sentio binary type origin record version: 4294967297\n" + table_tail,
        "sentio binary type origin record version: 18446744073709551617\n" + table_tail,
        "sentio binary type origin record version: 10000000001\n" + table_tail,
        "sentio binary type origin record version: 01\n" + table_tail,
        "sentio binary type origin record version: +1\n" + table_tail,
        "sentio binary type origin record version: \n" + table_tail,
        /// a part record with a lowercase, a short and an empty checksum
        head + "kind: part\norigin: sentio_legacy_v0\npart checksum: 0123456789abcdef0123456789abcdef\n",
        head + "kind: part\norigin: sentio_legacy_v0\npart checksum: 0123456789ABCDEF\n",
        head + "kind: part\norigin: sentio_legacy_v0\npart checksum: \n",
        /// the identity line of the other kind
        head + "kind: part\norigin: sentio_legacy_v0\ntable uuid: " + TABLE_UUID_TEXT + "\n",
        head + "kind: table\norigin: official_canonical\npart checksum: " + PART_CHECKSUM + "\n",
        /// trailing bytes, a trailing empty line, a missing identity line, truncated in the middle and before the last newline, empty
        head + table_tail + "x",
        head + table_tail + "\n",
        head + "kind: table\norigin: official_canonical\n",
        head + "kind: tab",
        (head + table_tail).substr(0, (head + table_tail).size() - 1),
        "",
        /// an origin line without a newline, short and long
        head + "kind: table\norigin: official_canonical",
        head + "kind: table\norigin: " + String(5000, 'a'),
    };
    for (const auto & text : cases)
    {
        EXPECT_EQ(errorOf([&] { StoredBinaryTypeOriginRecord::parse(text); }).first, ErrorCodes::CORRUPTED_DATA) << text.substr(0, 200);
        ReadBufferFromString in(text);
        EXPECT_EQ(errorOf([&] { StoredBinaryTypeOriginRecord::read(in); }).first, ErrorCodes::CORRUPTED_DATA) << text.substr(0, 200);
    }
}

TEST(StoredBinaryTypeOrigin, VersionIsComparedExactlyWithoutWrapping)
{
    const String table_tail = "kind: table\norigin: official_canonical\ntable uuid: " + TABLE_UUID_TEXT + "\n";
    const auto [code, message] = errorOf([&] { StoredBinaryTypeOriginRecord::parse("sentio binary type origin record version: 4294967297\n" + table_tail); });
    EXPECT_EQ(code, ErrorCodes::CORRUPTED_DATA);
    EXPECT_NE(message.find("Unknown format version 4294967297"), String::npos) << message;
    EXPECT_EQ(StoredBinaryTypeOriginRecord::parse("sentio binary type origin record version: 1\n" + table_tail).origin, BinaryTypeOrigin::OfficialCanonical);
}

TEST(StoredBinaryTypeOrigin, RecordIsBoundedTo4096Bytes)
{
    const String valid = StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, uuidOf(TABLE_UUID_TEXT)).toString();
    ASSERT_LT(valid.size(), 4096u);

    /// Exactly 4096 bytes are read and parsed: the input is refused for its trailing bytes, not for its size.
    const String at_bound = valid + String(4096 - valid.size(), 'x');
    ASSERT_EQ(at_bound.size(), 4096u);
    for (const auto & [code, message] : {errorOf([&] { StoredBinaryTypeOriginRecord::parse(at_bound); }),
                                         errorOf([&] { ReadBufferFromString in(at_bound); StoredBinaryTypeOriginRecord::read(in); })})
    {
        EXPECT_EQ(code, ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(message.find("trailing bytes after the record"), String::npos) << message;
    }

    /// One byte more is refused for its size, by both entry points, before any line is parsed.
    const String over_bound = at_bound + "x";
    for (const auto & [code, message] : {errorOf([&] { StoredBinaryTypeOriginRecord::parse(over_bound); }),
                                         errorOf([&] { ReadBufferFromString in(over_bound); StoredBinaryTypeOriginRecord::read(in); })})
    {
        EXPECT_EQ(code, ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(message.find("longer than 4096 bytes"), String::npos) << message;
    }

    /// A valid record followed by more input than the bound is not taken: reading stops at 4097 bytes and refuses the whole input.
    const String long_input = valid + String(100000, 'x');
    ReadBufferFromString in(long_input);
    const auto [code, message] = errorOf([&] { StoredBinaryTypeOriginRecord::read(in); });
    EXPECT_EQ(code, ErrorCodes::CORRUPTED_DATA);
    EXPECT_NE(message.find("longer than 4096 bytes"), String::npos) << message;
    EXPECT_EQ(in.count(), 4097u);
}

TEST(StoredBinaryTypeOrigin, ReadTakesTheWholeInput)
{
    const auto record = StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin::SentioLegacyV0, PART_CHECKSUM);
    /// ReadBufferFromString borrows the string: it must outlive every read.
    const String text = record.toString();
    ReadBufferFromString in(text);
    const auto parsed = StoredBinaryTypeOriginRecord::read(in);
    EXPECT_EQ(parsed.kind, StoredBinaryTypeOriginRecordKind::Part);
    EXPECT_EQ(parsed.part_checksum, PART_CHECKSUM);
    EXPECT_TRUE(in.eof());
}

TEST(StoredBinaryTypeOrigin, TableRecordAppliesOnlyToItsOwnTable)
{
    const UUID table_uuid = uuidOf(TABLE_UUID_TEXT);
    const auto table_record = StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::SentioLegacyV0, table_uuid);
    const auto part_record = StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin::SentioLegacyV0, PART_CHECKSUM);

    /// the same UUID
    EXPECT_EQ(matchTableRecord(table_record, table_uuid), StoredBinaryTypeOriginRecordMatch::Applies);
    /// another UUID
    EXPECT_EQ(matchTableRecord(table_record, uuidOf(OTHER_UUID_TEXT)), StoredBinaryTypeOriginRecordMatch::OtherTable);
    /// a table without a UUID: a nil UUID is not a wildcard
    EXPECT_EQ(matchTableRecord(table_record, UUIDHelpers::Nil), StoredBinaryTypeOriginRecordMatch::TableWithoutUUID);
    /// a part record at the table path never applies to a table, whatever the UUID
    EXPECT_EQ(matchTableRecord(part_record, table_uuid), StoredBinaryTypeOriginRecordMatch::WrongKind);
    EXPECT_EQ(matchTableRecord(part_record, UUIDHelpers::Nil), StoredBinaryTypeOriginRecordMatch::WrongKind);
}

TEST(StoredBinaryTypeOrigin, PartRecordAppliesOnlyToItsOwnPart)
{
    const auto part_record = StoredBinaryTypeOriginRecord::forPart(BinaryTypeOrigin::OfficialCanonical, PART_CHECKSUM);
    const auto table_record = StoredBinaryTypeOriginRecord::forTable(BinaryTypeOrigin::OfficialCanonical, uuidOf(TABLE_UUID_TEXT));

    EXPECT_EQ(matchPartRecord(part_record, PART_CHECKSUM), StoredBinaryTypeOriginRecordMatch::Applies);
    EXPECT_EQ(matchPartRecord(part_record, OTHER_PART_CHECKSUM), StoredBinaryTypeOriginRecordMatch::OtherPart);
    EXPECT_EQ(matchPartRecord(part_record, String()), StoredBinaryTypeOriginRecordMatch::OtherPart);
    /// a table record in a part directory never applies to a part
    EXPECT_EQ(matchPartRecord(table_record, PART_CHECKSUM), StoredBinaryTypeOriginRecordMatch::WrongKind);
}

TEST(StoredBinaryTypeOrigin, ReplicatedReconciliationAppliesOnlyTheRegistration)
{
    using enum StoredBinaryTypeOriginReconciliation;
    using Origin = std::optional<BinaryTypeOrigin>;
    const Origin N = std::nullopt;
    const Origin U = BinaryTypeOrigin::Unknown;
    const Origin L = BinaryTypeOrigin::SentioLegacyV0;
    const Origin C = BinaryTypeOrigin::OfficialCanonical;

    /// {local, local record ignored, replicated} -> decision, every combination written out. N is no origin (unset), U a stated Unknown.
    const std::tuple<Origin, bool, Origin, StoredBinaryTypeOriginReconciliation> cases[] = {
        /// no record and fieldless metadata of an old table: nothing changes
        {N, false, N, Keep},
        /// the registration, also recovered after a restart between its ZooKeeper part and its local part
        {N, false, L, ApplyRegistration},
        /// an existing replica without an origin never becomes canonical or a stated Unknown implicitly
        {N, false, C, Conflict},
        {N, false, U, Conflict},
        /// a stated Unknown is never relabelled by the registration, nor reset by fieldless metadata
        {U, false, N, Conflict},
        {U, false, U, Keep},
        {U, false, L, Conflict},
        {U, false, C, Conflict},
        /// an existing Legacy record is never reset by fieldless metadata, nor relabelled
        {L, false, N, Conflict},
        {L, false, U, Conflict},
        {L, false, L, Keep},
        {L, false, C, Conflict},
        /// an existing canonical record is never reset, nor relabelled Legacy
        {C, false, N, Conflict},
        {C, false, U, Conflict},
        {C, false, L, Conflict},
        {C, false, C, Keep},
        /// a record of another table or of the other kind is never replaced implicitly, whatever ZooKeeper holds
        {U, true, N, KeepIgnoredRecord},
        {U, true, U, KeepIgnoredRecord},
        {U, true, L, KeepIgnoredRecord},
        {U, true, C, KeepIgnoredRecord},
    };
    for (const auto & [local, ignored, replicated, expected] : cases)
        EXPECT_EQ(decideStoredBinaryTypeOriginReconciliation(local, ignored, replicated), expected)
            << describeStatedBinaryTypeOrigin(local) << " " << ignored << " " << describeStatedBinaryTypeOrigin(replicated);
}
