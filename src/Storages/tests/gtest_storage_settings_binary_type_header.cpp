#include "config.h"

#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <Common/SettingsChanges.h>
#include <Core/Field.h>
#include <Databases/DataLake/DatabaseDataLakeSettings.h>
#include <Storages/FileLog/FileLogSettings.h>
#include <Storages/ObjectStorage/DataLakes/DataLakeStorageSettings.h>
#include <Storages/ObjectStorage/StorageObjectStorageSettings.h>
#include <Storages/ObjectStorageQueue/ObjectStorageQueueSettings.h>
#include <Storages/SetSettings.h>

#if USE_HIVE
#include <Storages/Hive/HiveSettings.h>
#endif
#if USE_RDKAFKA
#include <Storages/Kafka/KafkaSettings.h>
#endif
#if USE_NATSIO
#include <Storages/NATS/NATSSettings.h>
#endif
#if USE_AMQPCPP
#include <Storages/RabbitMQ/RabbitMQSettings.h>
#endif

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

using namespace DB;

/// Each storage settings class that embeds LIST_OF_ALL_FORMAT_SETTINGS declares its traits with its own list of supported setting types,
/// so every type of a format setting has to be in each of those lists. The binary type header settings `input_format_binary_type_origin`
/// and `output_format_binary_type_writer_mode` were missing from the lists of the nine classes below, and the first full build of the
/// custom codec branch failed in KafkaSettings.cpp. `DatabaseDataLakeSettings` embeds them one level further down (through
/// `LIST_OF_DATA_LAKE_STORAGE_SETTINGS`) and its own type list missed them too; the next build failed in DatabaseDataLakeSettings.cpp.
/// A missing type does not compile; these tests check that each class knows both settings and that the classes with a value interface
/// take the values the enums define and refuse others.
namespace
{

const String origin = "input_format_binary_type_origin";
const String writer_mode = "output_format_binary_type_writer_mode";

SettingsChanges binaryTypeHeaderChanges(const String & origin_value, const String & writer_mode_value)
{
    SettingsChanges changes;
    changes.emplace_back(origin, Field(origin_value));
    changes.emplace_back(writer_mode, Field(writer_mode_value));
    return changes;
}

}

TEST(StorageSettingsBinaryTypeHeader, EveryFormatSettingsConsumerKnowsBoth)
{
    for (const auto & name : {origin, writer_mode})
    {
        EXPECT_TRUE(FileLogSettings::hasBuiltin(name)) << name;
        EXPECT_TRUE(SetSettings::hasBuiltin(name)) << name;
        EXPECT_TRUE(StorageObjectStorageSettings::hasBuiltin(name)) << name;
        EXPECT_TRUE(DataLakeStorageSettings::hasBuiltin(name)) << name;
        EXPECT_TRUE(ObjectStorageQueueSettings::hasBuiltin(name)) << name;
#if USE_HIVE
        EXPECT_TRUE(HiveSettings::hasBuiltin(name)) << name;
#endif
#if USE_RDKAFKA
        EXPECT_TRUE(KafkaSettings::hasBuiltin(name)) << name;
#endif
#if USE_NATSIO
        EXPECT_TRUE(NATSSettings::hasBuiltin(name)) << name;
#endif
#if USE_AMQPCPP
        EXPECT_TRUE(RabbitMQSettings::hasBuiltin(name)) << name;
#endif
    }
}

TEST(StorageSettingsBinaryTypeHeader, ValuesRoundTrip)
{
    const auto changes = binaryTypeHeaderChanges("sentio_legacy_v0", "sentio_legacy_bridge");
    {
        StorageObjectStorageSettings settings;
        settings.loadFromSettingsChanges(changes);
        EXPECT_EQ(settings.get(origin).safeGet<String>(), "sentio_legacy_v0");
        EXPECT_EQ(settings.get(writer_mode).safeGet<String>(), "sentio_legacy_bridge");
    }
    {
        DataLakeStorageSettings settings;
        settings.loadFromSettingsChanges(changes);
        EXPECT_EQ(settings.get(origin).safeGet<String>(), "sentio_legacy_v0");
        EXPECT_EQ(settings.get(writer_mode).safeGet<String>(), "sentio_legacy_bridge");
    }
    {
        ObjectStorageQueueSettings settings;
        settings.applyChanges(changes);
        EXPECT_EQ(settings.get(origin).safeGet<String>(), "sentio_legacy_v0");
        EXPECT_EQ(settings.get(writer_mode).safeGet<String>(), "sentio_legacy_bridge");
    }
}

TEST(StorageSettingsBinaryTypeHeader, UnknownValuesAreRefused)
{
    StorageObjectStorageSettings object_storage;
    EXPECT_THROW(object_storage.loadFromSettingsChanges(binaryTypeHeaderChanges("bogus", "sentio_wide_v1")), Exception);
    DataLakeStorageSettings data_lake;
    EXPECT_THROW(data_lake.loadFromSettingsChanges(binaryTypeHeaderChanges("unknown", "bogus")), Exception);
    ObjectStorageQueueSettings queue;
    EXPECT_THROW(queue.applyChanges(binaryTypeHeaderChanges("official_canonical", "bogus")), Exception);
}

TEST(StorageSettingsBinaryTypeHeader, DatabaseDataLakeSettingsTakesBoth)
{
    /// `DatabaseDataLakeSettings` has no `get` or `hasBuiltin`: its interface is `applyChanges` and `allChanged`. Every value of both enums.
    for (const auto * origin_value : {"unknown", "sentio_legacy_v0", "official_canonical"})
    {
        for (const auto * writer_mode_value : {"sentio_wide_v1", "sentio_legacy_bridge"})
        {
            DatabaseDataLakeSettings settings;
            settings.applyChanges(binaryTypeHeaderChanges(origin_value, writer_mode_value));
            size_t found = 0;
            for (const auto & change : settings.allChanged())
            {
                if (change.name == origin)
                {
                    EXPECT_EQ(change.value.safeGet<String>(), origin_value);
                    ++found;
                }
                else if (change.name == writer_mode)
                {
                    EXPECT_EQ(change.value.safeGet<String>(), writer_mode_value);
                    ++found;
                }
            }
            EXPECT_EQ(found, 2u) << origin_value << ", " << writer_mode_value;
        }
    }
}

TEST(StorageSettingsBinaryTypeHeader, DatabaseDataLakeSettingsRefusesUnknownValues)
{
    /// An unknown origin and an unknown writer mode are refused separately, each next to a valid value of the other setting. The refusal is
    /// the enum's own (`BAD_ARGUMENTS` naming the enum and the value), not `UNKNOWN_SETTING` for a setting the class does not know.
    const auto refusal = [](const SettingsChanges & changes)
    {
        DatabaseDataLakeSettings settings;
        try
        {
            settings.applyChanges(changes);
        }
        catch (const Exception & e)
        {
            return std::make_pair(e.code(), e.message());
        }
        return std::make_pair(0, String{});
    };
    const auto [origin_code, origin_message] = refusal(binaryTypeHeaderChanges("bogus", "sentio_wide_v1"));
    EXPECT_EQ(origin_code, ErrorCodes::BAD_ARGUMENTS) << origin_message;
    EXPECT_TRUE(origin_message.contains("Unexpected value of BinaryTypeOrigin: 'bogus'")) << origin_message;
    const auto [writer_mode_code, writer_mode_message] = refusal(binaryTypeHeaderChanges("official_canonical", "bogus"));
    EXPECT_EQ(writer_mode_code, ErrorCodes::BAD_ARGUMENTS) << writer_mode_message;
    EXPECT_TRUE(writer_mode_message.contains("Unexpected value of BinaryTypeWriterMode: 'bogus'")) << writer_mode_message;
}
