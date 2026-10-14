#include <gtest/gtest.h>

#include <DataTypes/BinaryTypeOrigin.h>
#include <Storages/MergeTree/ReplicatedMergeTreeTableMetadata.h>
#include <Common/Exception.h>

#include <string>
#include <utility>

/// The stored binary type origin in the ZooKeeper metadata of a replicated table: written last and only while it is not Unknown, so the
/// metadata of every table that never had one is byte for byte as before; read when present, Unknown when absent. Its name starts with a
/// letter no optional field before it starts with, so the checks of the optional fields never advance into it, also in the legacy layout
/// without the merge parameters. The metadata texts are written out here, not produced by the code under test.

using namespace DB;

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

namespace
{

/// The required lines and a partition key, as a table with custom partitioning writes them.
const String HEAD = "metadata format version: 1\n"
                    "date column: \n"
                    "sampling expression: \n"
                    "index granularity: 8192\n"
                    "mode: 0\n"
                    "sign column: \n"
                    "primary key: a\n"
                    "data format version: 1\n"
                    "partition key: \n";
const String MERGE_PARAMETERS = "merge parameters format version: 2\n";

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

TEST(ReplicatedMetadataBinaryTypeOrigin, AbsentIsUnknownAndNotWritten)
{
    const auto metadata = ReplicatedMergeTreeTableMetadata::parseRaw(HEAD + MERGE_PARAMETERS);
    EXPECT_EQ(metadata.binary_type_origin, BinaryTypeOrigin::Unknown);
    /// The metadata of a table without an origin is written exactly as before.
    EXPECT_EQ(metadata.toString(), HEAD + MERGE_PARAMETERS);
}

TEST(ReplicatedMetadataBinaryTypeOrigin, WrittenLastAndReadBack)
{
    for (auto [origin, name] : {std::pair{BinaryTypeOrigin::SentioLegacyV0, "sentio_legacy_v0"}, std::pair{BinaryTypeOrigin::OfficialCanonical, "official_canonical"}})
    {
        auto metadata = ReplicatedMergeTreeTableMetadata::parseRaw(HEAD + MERGE_PARAMETERS);
        metadata.binary_type_origin = origin;
        const String text = HEAD + MERGE_PARAMETERS + "binary type origin: " + name + "\n";
        EXPECT_EQ(metadata.toString(), text);
        EXPECT_EQ(ReplicatedMergeTreeTableMetadata::parseRaw(text).binary_type_origin, origin);
    }
}

TEST(ReplicatedMetadataBinaryTypeOrigin, ReadAfterEveryOptionalField)
{
    /// Each optional field may be absent; the field must be found after any subset of them, also without the merge parameters (the legacy
    /// layout) and after "sorting key", whose first letter differs from its own.
    const String optional_fields = "sorting key: a, b\nttl: \nindices: \nprojections: \ngranularity bytes: 10485760\nconstraints: \n";
    const String cases[] = {
        HEAD + "binary type origin: sentio_legacy_v0\n",
        HEAD + optional_fields + "binary type origin: sentio_legacy_v0\n",
        HEAD + MERGE_PARAMETERS + "version column: v\ngraphite hash: 0\nbinary type origin: sentio_legacy_v0\n",
        HEAD + "constraints: \n" + MERGE_PARAMETERS + "binary type origin: sentio_legacy_v0\n",
    };
    for (const auto & text : cases)
    {
        const auto metadata = ReplicatedMergeTreeTableMetadata::parseRaw(text);
        EXPECT_EQ(metadata.binary_type_origin, BinaryTypeOrigin::SentioLegacyV0) << text;
        EXPECT_EQ(metadata.primary_key, "a") << text;
    }
    EXPECT_EQ(ReplicatedMergeTreeTableMetadata::parseRaw(HEAD + optional_fields + "binary type origin: sentio_legacy_v0\n").sorting_key, "a, b");
}

TEST(ReplicatedMetadataBinaryTypeOrigin, UnknownValueIsRefused)
{
    EXPECT_EQ(errorOf([] { ReplicatedMergeTreeTableMetadata::parseRaw(HEAD + MERGE_PARAMETERS + "binary type origin: legacy\n"); }).first,
              ErrorCodes::CORRUPTED_DATA);
}
