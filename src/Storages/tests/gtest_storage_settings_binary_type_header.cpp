#include "config.h"

#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <Common/SettingsChanges.h>
#include <Core/Field.h>
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

using namespace DB;

/// Each storage settings class that embeds LIST_OF_ALL_FORMAT_SETTINGS declares its traits with its own list of supported setting types,
/// so every type of a format setting has to be in each of those lists. The binary type header settings `input_format_binary_type_origin`
/// and `output_format_binary_type_writer_mode` were missing from the lists of the nine classes below, and the first full build of the
/// custom codec branch failed in KafkaSettings.cpp. A missing type does not compile; these tests check that each class knows both settings
/// and that the classes with a value interface take the values the enums define and refuse others.
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
