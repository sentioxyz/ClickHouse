#pragma once

#include <Core/Settings.h>
#include <Formats/FormatSettings.h>

#include <optional>

namespace DB
{
namespace Setting
{
    extern const SettingsBinaryTypeOrigin input_format_binary_type_origin;
    extern const SettingsBinaryTypeWriterMode output_format_binary_type_writer_mode;
}

/// Resolve each Native exchange from explicit connection defaults, never from a previous query.
inline std::optional<FormatSettings> getNativeQueryFormatSettings(
    const std::optional<FormatSettings> & connection_settings, const Settings * query_settings)
{
    auto result = connection_settings;
    if (query_settings)
    {
        if (!result)
            result.emplace();
        result->binary.type_origin = (*query_settings)[Setting::input_format_binary_type_origin];
        result->binary.type_writer_mode = (*query_settings)[Setting::output_format_binary_type_writer_mode];
    }
    return result;
}
}
