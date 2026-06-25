#pragma once

#include <Common/ZooKeeper/ZooKeeperCommon.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DB
{

inline constexpr std::string_view housekeeper_root_path = "/housekeeper/v1";
inline constexpr std::string_view housekeeper_verified_tables_path = "/housekeeper/v1/verified_tables";
inline constexpr std::string_view housekeeper_source_claims_path = "/housekeeper/v1/source_claims";

struct HouseKeeperAdmissionRejection
{
    static constexpr size_t not_in_multi = std::numeric_limits<size_t>::max();

    Coordination::Error error = Coordination::Error::ZOK;
    size_t failed_pos = not_in_multi;
};

enum class HouseKeeperRMTLogEntryType
{
    Unknown,
    GetPart,
    AttachPart,
    MergeParts,
};

struct HouseKeeperRMTLogEntry
{
    HouseKeeperRMTLogEntryType type = HouseKeeperRMTLogEntryType::Unknown;
    std::string new_part_name;
    std::vector<std::string> source_parts;
};

struct HouseKeeperWriteCandidate
{
    std::string path;
    std::string data;
    size_t failed_pos = HouseKeeperAdmissionRejection::not_in_multi;
};

inline std::string houseKeeperEncodePathForKey(std::string_view path)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(path.size() * 2);
    for (unsigned char c : path)
    {
        encoded.push_back(hex[c >> 4]);
        encoded.push_back(hex[c & 0x0f]);
    }
    return encoded;
}

inline std::string houseKeeperVerifiedTableMarkerPath(std::string_view table_path)
{
    return std::string{housekeeper_verified_tables_path} + "/" + houseKeeperEncodePathForKey(table_path);
}

inline std::string houseKeeperSourceClaimTablePath(std::string_view table_path)
{
    return std::string{housekeeper_source_claims_path} + "/" + houseKeeperEncodePathForKey(table_path);
}

inline std::string houseKeeperSourceClaimPath(std::string_view table_path, std::string_view part_name)
{
    return houseKeeperSourceClaimTablePath(table_path) + "/" + std::string{part_name};
}

inline std::string_view houseKeeperTrimLogLine(std::string_view line)
{
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);

    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.remove_suffix(1);

    return line;
}

inline std::vector<std::string_view> houseKeeperSplitLogLines(std::string_view data)
{
    std::vector<std::string_view> lines;
    size_t offset = 0;
    while (offset <= data.size())
    {
        const auto end = data.find('\n', offset);
        auto line = end == std::string_view::npos ? data.substr(offset) : data.substr(offset, end - offset);
        line = houseKeeperTrimLogLine(line);
        if (!line.empty())
            lines.push_back(line);

        if (end == std::string_view::npos)
            break;

        offset = end + 1;
    }
    return lines;
}

inline std::optional<HouseKeeperRMTLogEntry> houseKeeperParseRMTLogEntry(std::string_view data)
{
    const auto lines = houseKeeperSplitLogLines(data);

    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i] == "get" || lines[i] == "attach")
        {
            if (i + 1 >= lines.size())
                return std::nullopt;

            return HouseKeeperRMTLogEntry{
                .type = lines[i] == "get" ? HouseKeeperRMTLogEntryType::GetPart : HouseKeeperRMTLogEntryType::AttachPart,
                .new_part_name = std::string{lines[i + 1]},
                .source_parts = {},
            };
        }

        if (lines[i] == "merge")
        {
            HouseKeeperRMTLogEntry entry{
                .type = HouseKeeperRMTLogEntryType::MergeParts,
                .new_part_name = {},
                .source_parts = {},
            };
            for (size_t j = i + 1; j < lines.size(); ++j)
            {
                if (lines[j] == "into")
                {
                    if (j + 1 >= lines.size())
                        return std::nullopt;

                    entry.new_part_name = std::string{lines[j + 1]};
                    return entry;
                }

                entry.source_parts.emplace_back(lines[j]);
            }
            return std::nullopt;
        }
    }

    return HouseKeeperRMTLogEntry{};
}

inline std::optional<std::string> houseKeeperExtractTablePathFromLogPath(std::string_view path)
{
    const auto log_pos = path.rfind("/log/");
    if (log_pos == std::string_view::npos || log_pos == 0)
        return std::nullopt;

    const auto log_entry_name = path.substr(log_pos + std::string_view{"/log/"}.size());
    if (log_entry_name.empty())
        return std::nullopt;

    return std::string{path.substr(0, log_pos)};
}

inline void houseKeeperCollectWriteCandidates(
    const Coordination::ZooKeeperRequest & request,
    std::vector<HouseKeeperWriteCandidate> & candidates,
    size_t failed_pos = HouseKeeperAdmissionRejection::not_in_multi)
{
    if (const auto * create_request = dynamic_cast<const Coordination::ZooKeeperCreateRequest *>(&request))
    {
        candidates.push_back(HouseKeeperWriteCandidate{
            .path = create_request->path,
            .data = create_request->data,
            .failed_pos = failed_pos,
        });
        return;
    }

    if (const auto * set_request = dynamic_cast<const Coordination::ZooKeeperSetRequest *>(&request))
    {
        candidates.push_back(HouseKeeperWriteCandidate{
            .path = set_request->path,
            .data = set_request->data,
            .failed_pos = failed_pos,
        });
        return;
    }

    if (const auto * multi_request = dynamic_cast<const Coordination::ZooKeeperMultiRequest *>(&request))
    {
        for (size_t i = 0; i < multi_request->requests.size(); ++i)
            houseKeeperCollectWriteCandidates(*multi_request->requests[i], candidates, failed_pos == HouseKeeperAdmissionRejection::not_in_multi ? i : failed_pos);
    }
}

template <typename Storage>
bool houseKeeperNodeExists(Storage & storage, const std::string & path)
{
    return storage.uncommitted_state.getNode(path) != nullptr;
}

inline HouseKeeperAdmissionRejection houseKeeperReject(const HouseKeeperWriteCandidate & candidate, Coordination::Error error)
{
    return HouseKeeperAdmissionRejection{ .error = error, .failed_pos = candidate.failed_pos };
}

template <typename Storage>
std::optional<HouseKeeperAdmissionRejection> checkHouseKeeperAdmission(
    const Coordination::ZooKeeperRequest & request,
    Storage & storage)
{
    std::vector<HouseKeeperWriteCandidate> candidates;
    houseKeeperCollectWriteCandidates(request, candidates);

    for (const auto & candidate : candidates)
    {
        const auto table_path = houseKeeperExtractTablePathFromLogPath(candidate.path);
        if (!table_path)
            continue;

        if (!houseKeeperNodeExists(storage, houseKeeperVerifiedTableMarkerPath(*table_path)))
            continue;

        const auto entry = houseKeeperParseRMTLogEntry(candidate.data);
        if (!entry || entry->type == HouseKeeperRMTLogEntryType::Unknown || entry->new_part_name.empty())
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

        if (entry->type == HouseKeeperRMTLogEntryType::MergeParts)
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

        if ((entry->type == HouseKeeperRMTLogEntryType::GetPart || entry->type == HouseKeeperRMTLogEntryType::AttachPart)
            && !houseKeeperNodeExists(storage, houseKeeperSourceClaimPath(*table_path, entry->new_part_name)))
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
    }

    return std::nullopt;
}

}
