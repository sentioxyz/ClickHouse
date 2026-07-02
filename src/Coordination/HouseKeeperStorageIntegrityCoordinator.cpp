#include <Coordination/HouseKeeperStorageIntegrityCoordinator.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace DB
{
namespace
{

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string childPath(std::string_view parent, std::string_view child)
{
    return std::string{parent} + "/" + std::string{child};
}

std::optional<std::string> directChildID(std::string_view path, std::string_view parent)
{
    if (!startsWith(path, parent) || path.size() <= parent.size() || path[parent.size()] != '/')
        return std::nullopt;

    const auto rest = path.substr(parent.size() + 1);
    if (rest.empty() || rest.find('/') != std::string_view::npos)
        return std::nullopt;

    return std::string{rest};
}

bool isDescendantOf(std::string_view path, std::string_view parent)
{
    return startsWith(path, parent) && path.size() > parent.size() && path[parent.size()] == '/';
}

std::string trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
        value.remove_prefix(1);

    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);

    return std::string{value};
}

std::map<std::string, std::string> parseKeyValueLines(std::string_view data)
{
    std::map<std::string, std::string> result;
    size_t offset = 0;
    while (offset <= data.size())
    {
        const auto end = data.find('\n', offset);
        const auto raw_line = end == std::string_view::npos ? data.substr(offset) : data.substr(offset, end - offset);
        const auto line = trim(raw_line);
        if (!line.empty())
        {
            const auto equals_pos = line.find('=');
            if (equals_pos == std::string::npos)
                return {};

            auto key = trim(std::string_view{line}.substr(0, equals_pos));
            auto value = trim(std::string_view{line}.substr(equals_pos + 1));
            if (key.empty())
                return {};
            result.emplace(std::move(key), std::move(value));
        }

        if (end == std::string_view::npos)
            break;

        offset = end + 1;
    }

    return result;
}

std::vector<std::string> splitCSV(std::string_view value)
{
    std::vector<std::string> result;
    size_t offset = 0;
    while (offset <= value.size())
    {
        const auto end = value.find(',', offset);
        auto item = trim(end == std::string_view::npos ? value.substr(offset) : value.substr(offset, end - offset));
        if (item.empty())
            return {};
        result.push_back(std::move(item));

        if (end == std::string_view::npos)
            break;

        offset = end + 1;
    }

    return result;
}

std::string joinCSV(const std::vector<std::string> & values)
{
    std::string result;
    for (const auto & value : values)
    {
        if (!result.empty())
            result += ",";
        result += value;
    }
    return result;
}

std::string pathSafeID(std::string_view value)
{
    std::string result;
    for (const auto ch : value)
    {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) || ch == '.' || ch == '_' || ch == '-')
            result += ch;
        else
            result += '_';
    }
    return result;
}

std::optional<uint64_t> parseUInt64(std::string_view value)
{
    uint64_t parsed = 0;
    const auto * begin = value.data();
    const auto * end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::nullopt;
    return parsed;
}

std::optional<size_t> parseSize(std::string_view value)
{
    const auto parsed = parseUInt64(value);
    if (!parsed || *parsed == 0)
        return std::nullopt;
    return static_cast<size_t>(*parsed);
}

std::optional<bool> parseBool(std::string_view value)
{
    if (value == "true" || value == "1")
        return true;
    if (value == "false" || value == "0")
        return false;
    return std::nullopt;
}

std::optional<HouseKeeperStorageReplicaDigest> parseReplicaDigest(std::string_view value)
{
    const auto first = value.find(':');
    if (first == std::string_view::npos || first == 0 || first + 1 >= value.size())
        return std::nullopt;
    const auto second = value.find(':', first + 1);
    if (second == std::string_view::npos || second + 1 >= value.size())
        return std::nullopt;

    const auto row_count = parseUInt64(value.substr(first + 1, second - first - 1));
    if (!row_count)
        return std::nullopt;

    HouseKeeperStorageReplicaDigest digest;
    digest.replica_id = std::string{value.substr(0, first)};
    digest.row_count = *row_count;
    digest.rows_hash = std::string{value.substr(second + 1)};
    if (digest.replica_id.empty() || digest.rows_hash.empty())
        return std::nullopt;
    return digest;
}

std::vector<HouseKeeperStorageReplicaDigest> parseReplicaDigests(std::string_view value)
{
    std::vector<HouseKeeperStorageReplicaDigest> result;
    for (const auto & item : splitCSV(value))
    {
        auto digest = parseReplicaDigest(item);
        if (!digest)
            return {};
        result.push_back(std::move(*digest));
    }
    return result;
}

std::optional<HouseKeeperStorageByteSidePart> parseByteSidePart(std::string_view value)
{
    const auto first = value.find(':');
    if (first == std::string_view::npos || first == 0 || first + 1 >= value.size())
        return std::nullopt;
    const auto second = value.find(':', first + 1);
    if (second == std::string_view::npos || second + 1 >= value.size())
        return std::nullopt;
    const auto third = value.find(':', second + 1);
    if (third == std::string_view::npos || third + 1 >= value.size())
        return std::nullopt;

    const auto row_count = parseUInt64(value.substr(second + 1, third - second - 1));
    if (!row_count)
        return std::nullopt;

    HouseKeeperStorageByteSidePart part;
    part.partition_id = std::string{value.substr(0, first)};
    part.part_name = std::string{value.substr(first + 1, second - first - 1)};
    part.row_count = *row_count;
    part.part_row_lthash = std::string{value.substr(third + 1)};
    if (part.partition_id.empty() || part.part_name.empty() || part.part_row_lthash.empty())
        return std::nullopt;
    return part;
}

std::vector<HouseKeeperStorageByteSidePart> parseByteSideParts(std::string_view value)
{
    std::vector<HouseKeeperStorageByteSidePart> result;
    if (value.empty())
        return result;
    for (const auto & item : splitCSV(value))
    {
        auto part = parseByteSidePart(item);
        if (!part)
            return {};
        result.push_back(std::move(*part));
    }
    return result;
}

void sortByteSideParts(std::vector<HouseKeeperStorageByteSidePart> & parts)
{
    std::sort(parts.begin(), parts.end(), [](const auto & lhs, const auto & rhs)
    {
        if (lhs.partition_id != rhs.partition_id)
            return lhs.partition_id < rhs.partition_id;
        return lhs.part_name < rhs.part_name;
    });
}

std::string serializeByteSideParts(std::vector<HouseKeeperStorageByteSidePart> parts)
{
    if (parts.empty())
        return {};
    sortByteSideParts(parts);
    std::string result;
    for (const auto & part : parts)
    {
        if (!result.empty())
            result += ",";
        result += part.partition_id;
        result += ":";
        result += part.part_name;
        result += ":";
        result += std::to_string(part.row_count);
        result += ":";
        result += part.part_row_lthash;
    }
    return result;
}

bool byteSidePartsEqual(std::vector<HouseKeeperStorageByteSidePart> lhs, std::vector<HouseKeeperStorageByteSidePart> rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    sortByteSideParts(lhs);
    sortByteSideParts(rhs);
    for (size_t i = 0; i < lhs.size(); ++i)
    {
        if (lhs[i].partition_id != rhs[i].partition_id
            || lhs[i].part_name != rhs[i].part_name
            || lhs[i].row_count != rhs[i].row_count
            || lhs[i].part_row_lthash != rhs[i].part_row_lthash)
            return false;
    }
    return true;
}

std::map<std::string, size_t> parseReplayTally(std::string_view value)
{
    std::map<std::string, size_t> result;
    if (value.empty())
        return result;
    for (const auto & item : splitCSV(value))
    {
        const auto colon = item.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= item.size())
            return {};
        const auto count = parseSize(std::string_view{item}.substr(colon + 1));
        if (!count)
            return {};
        result.emplace(item.substr(0, colon), *count);
    }
    return result;
}

std::string serializeReplayTally(const std::map<std::string, size_t> & tally)
{
    std::string result;
    for (const auto & [hash, count] : tally)
    {
        if (!result.empty())
            result += ",";
        result += hash;
        result += ":";
        result += std::to_string(count);
    }
    return result;
}

bool isDirectManagedChild(std::string_view path, std::string_view parent)
{
    return directChildID(path, parent).has_value();
}

}

std::string storageIntegrityReplayJobPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_replay_jobs_path, statement_id);
}

std::string storageIntegrityAttestationsPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_attestations_path, statement_id);
}

std::string storageIntegrityAttestationPath(std::string_view statement_id, std::string_view worker_id)
{
    return childPath(storageIntegrityAttestationsPath(statement_id), worker_id);
}

std::string storageIntegrityUnsafeTaskPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_unsafe_tasks_path, statement_id);
}

std::string storageIntegrityUnsafeResultPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_unsafe_results_path, statement_id);
}

std::string storageIntegrityUnsafeResultPath(std::string_view statement_id, std::string_view participant_id)
{
    return childPath(storageIntegrityUnsafeResultPath(statement_id), participant_id);
}

std::string storageIntegrityByteSideScanTaskPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_byte_side_scan_tasks_path, statement_id);
}

std::string storageIntegrityByteSideScansPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_byte_side_scans_path, statement_id);
}

std::string storageIntegrityByteSideScanPath(std::string_view statement_id, std::string_view worker_id)
{
    return childPath(storageIntegrityByteSideScansPath(statement_id), worker_id);
}

std::string storageIntegrityByteSideScanFailurePath(std::string_view statement_id, std::string_view worker_id)
{
    return childPath(childPath(housekeeper_storage_integrity_byte_side_scan_failures_path, statement_id), worker_id);
}

std::string storageIntegrityFinalityPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_finality_path, statement_id);
}

std::string storageIntegrityRollbackPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_rollbacks_path, statement_id);
}

std::string storageIntegrityPromotionPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_promotions_path, statement_id);
}

std::string storageIntegrityRollbackTaskPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_rollback_tasks_path, statement_id);
}

std::string storageIntegrityReplayQuarantinePath(std::string_view worker_id)
{
    return childPath(housekeeper_storage_integrity_replay_quarantine_path, worker_id);
}

std::string storageIntegrityDecisionPath(std::string_view statement_id)
{
    return childPath(housekeeper_storage_integrity_decisions_path, statement_id);
}

std::optional<std::string> storageIntegrityStatementIDFromPath(std::string_view path)
{
    return directChildID(path, housekeeper_storage_integrity_statements_path);
}

std::optional<std::pair<std::string, std::string>> storageIntegrityAttestationPathParts(std::string_view path)
{
    if (!startsWith(path, housekeeper_storage_integrity_attestations_path)
        || path.size() <= housekeeper_storage_integrity_attestations_path.size()
        || path[housekeeper_storage_integrity_attestations_path.size()] != '/')
        return std::nullopt;

    const auto rest = path.substr(housekeeper_storage_integrity_attestations_path.size() + 1);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= rest.size())
        return std::nullopt;

    const auto statement_id = rest.substr(0, slash);
    const auto worker_id = rest.substr(slash + 1);
    if (worker_id.find('/') != std::string_view::npos)
        return std::nullopt;

    return std::pair{std::string{statement_id}, std::string{worker_id}};
}

std::optional<std::string> storageIntegrityUnsafeResultIDFromPath(std::string_view path)
{
    return directChildID(path, housekeeper_storage_integrity_unsafe_results_path);
}

std::optional<std::pair<std::string, std::string>> storageIntegrityUnsafeResultPathParts(std::string_view path)
{
    if (!startsWith(path, housekeeper_storage_integrity_unsafe_results_path)
        || path.size() <= housekeeper_storage_integrity_unsafe_results_path.size()
        || path[housekeeper_storage_integrity_unsafe_results_path.size()] != '/')
        return std::nullopt;

    const auto rest = path.substr(housekeeper_storage_integrity_unsafe_results_path.size() + 1);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= rest.size())
        return std::nullopt;

    const auto statement_id = rest.substr(0, slash);
    const auto participant_id = rest.substr(slash + 1);
    if (participant_id.find('/') != std::string_view::npos)
        return std::nullopt;

    return std::pair{std::string{statement_id}, std::string{participant_id}};
}

std::optional<std::string> storageIntegrityByteSideScanIDFromPath(std::string_view path)
{
    return directChildID(path, housekeeper_storage_integrity_byte_side_scans_path);
}

std::optional<std::pair<std::string, std::string>> storageIntegrityByteSideScanPathParts(std::string_view path)
{
    if (!startsWith(path, housekeeper_storage_integrity_byte_side_scans_path)
        || path.size() <= housekeeper_storage_integrity_byte_side_scans_path.size()
        || path[housekeeper_storage_integrity_byte_side_scans_path.size()] != '/')
        return std::nullopt;

    const auto rest = path.substr(housekeeper_storage_integrity_byte_side_scans_path.size() + 1);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= rest.size())
        return std::nullopt;

    const auto statement_id = rest.substr(0, slash);
    const auto worker_id = rest.substr(slash + 1);
    if (worker_id.find('/') != std::string_view::npos)
        return std::nullopt;

    return std::pair{std::string{statement_id}, std::string{worker_id}};
}

std::optional<std::string> storageIntegrityFinalityIDFromPath(std::string_view path)
{
    return directChildID(path, housekeeper_storage_integrity_finality_path);
}

std::optional<std::string> storageIntegrityRollbackIDFromPath(std::string_view path)
{
    return directChildID(path, housekeeper_storage_integrity_rollbacks_path);
}

bool storageIntegrityIsUnderRoot(std::string_view path)
{
    return path == housekeeper_storage_integrity_path
        || (startsWith(path, housekeeper_storage_integrity_path)
            && path.size() > housekeeper_storage_integrity_path.size()
            && path[housekeeper_storage_integrity_path.size()] == '/');
}

bool storageIntegrityIsControlPath(std::string_view path)
{
    return path == housekeeper_storage_integrity_path
        || path == housekeeper_storage_integrity_statements_path
        || path == housekeeper_storage_integrity_blocks_path
        || path == housekeeper_storage_integrity_replay_jobs_path
        || path == housekeeper_storage_integrity_attestations_path
        || path == housekeeper_storage_integrity_replay_failures_path
        || path == housekeeper_storage_integrity_unsafe_tasks_path
        || path == housekeeper_storage_integrity_unsafe_results_path
        || path == housekeeper_storage_integrity_unsafe_failures_path
        || path == housekeeper_storage_integrity_byte_side_scan_tasks_path
        || path == housekeeper_storage_integrity_byte_side_scans_path
        || path == housekeeper_storage_integrity_byte_side_scan_failures_path
        || path == housekeeper_storage_integrity_mutations_path
        || path == housekeeper_storage_integrity_mutation_tasks_path
        || path == housekeeper_storage_integrity_mutation_leases_path
        || path == housekeeper_storage_integrity_mutation_claims_path
        || path == housekeeper_storage_integrity_mutation_failures_path
        || path == housekeeper_storage_integrity_finality_path
        || path == housekeeper_storage_integrity_rollbacks_path
        || path == housekeeper_storage_integrity_promotions_path
        || path == housekeeper_storage_integrity_rollback_tasks_path
        || path == housekeeper_storage_integrity_replay_quarantine_path
        || path == housekeeper_storage_integrity_rollback_leases_path
        || path == housekeeper_storage_integrity_rollback_results_path
        || path == housekeeper_storage_integrity_rollback_failures_path
        || path == housekeeper_storage_integrity_promotion_leases_path
        || path == housekeeper_storage_integrity_promotion_results_path
        || path == housekeeper_storage_integrity_promotion_failures_path
        || path == housekeeper_storage_integrity_safe_audit_tasks_path
        || path == housekeeper_storage_integrity_safe_audit_votes_path
        || path == housekeeper_storage_integrity_decisions_path;
}

bool storageIntegrityIsManagedLedgerPath(std::string_view path)
{
    if (storageIntegrityIsControlPath(path))
        return false;

    if (isDirectManagedChild(path, housekeeper_storage_integrity_replay_jobs_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_attestations_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_unsafe_tasks_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_unsafe_results_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_byte_side_scan_tasks_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_byte_side_scans_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_promotions_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_rollback_tasks_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_replay_quarantine_path)
        || isDirectManagedChild(path, housekeeper_storage_integrity_decisions_path))
        return true;

    return startsWith(path, housekeeper_storage_integrity_replay_jobs_path)
        || startsWith(path, housekeeper_storage_integrity_unsafe_tasks_path)
        || startsWith(path, housekeeper_storage_integrity_byte_side_scan_tasks_path)
        || startsWith(path, housekeeper_storage_integrity_promotions_path)
        || startsWith(path, housekeeper_storage_integrity_rollback_tasks_path)
        || startsWith(path, housekeeper_storage_integrity_replay_quarantine_path)
        || startsWith(path, housekeeper_storage_integrity_decisions_path);
}

bool storageIntegrityIsWorkerLedgerPath(std::string_view path)
{
    if (storageIntegrityIsControlPath(path))
        return false;

    return isDescendantOf(path, housekeeper_storage_integrity_blocks_path)
        || isDescendantOf(path, housekeeper_storage_integrity_replay_failures_path)
        || isDescendantOf(path, housekeeper_storage_integrity_unsafe_failures_path)
        || isDescendantOf(path, housekeeper_storage_integrity_byte_side_scans_path)
        || isDescendantOf(path, housekeeper_storage_integrity_byte_side_scan_failures_path)
        || isDescendantOf(path, housekeeper_storage_integrity_rollback_leases_path)
        || isDescendantOf(path, housekeeper_storage_integrity_rollback_results_path)
        || isDescendantOf(path, housekeeper_storage_integrity_rollback_failures_path)
        || isDescendantOf(path, housekeeper_storage_integrity_promotion_leases_path)
        || isDescendantOf(path, housekeeper_storage_integrity_promotion_results_path)
        || isDescendantOf(path, housekeeper_storage_integrity_promotion_failures_path)
        || isDescendantOf(path, housekeeper_storage_integrity_safe_audit_tasks_path)
        || isDescendantOf(path, housekeeper_storage_integrity_safe_audit_votes_path);
}

std::optional<HouseKeeperStorageStatement> storageIntegrityParseStatement(std::string_view statement_id, std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    HouseKeeperStorageStatement statement;
    statement.statement_id = std::string{statement_id};
    statement.table_id = fields["table_id"];
    statement.unsafe_table = fields["unsafe_table"];
    statement.safe_table = fields["safe_table"];
    statement.payload_ref = fields["payload_ref"];
    statement.payload_hash = fields["payload_hash"];
    statement.participants = splitCSV(fields["participants"]);
    statement.partition_ids = splitCSV(fields["partition_ids"]);
    statement.candidate_parts = parseByteSideParts(fields["candidate_parts"]);
    if (!fields["candidate_parts"].empty() && statement.candidate_parts.empty())
        return std::nullopt;
    if (statement.participants.empty())
        statement.participants = splitCSV(fields["unsafe_replicas"]);

    if (auto replay_quorum = parseSize(fields["replay_quorum"]))
        statement.replay_quorum = *replay_quorum;
    statement.byte_side_quorum = statement.replay_quorum;
    if (auto byte_side_quorum = parseSize(fields["byte_side_quorum"]))
        statement.byte_side_quorum = *byte_side_quorum;
    if (auto byte_side_required = parseBool(fields["byte_side_required"]))
        statement.byte_side_required = *byte_side_required;

    if (!fields["unsafe_buffer_id"].empty())
    {
        const auto unsafe_buffer_id = parseUInt64(fields["unsafe_buffer_id"]);
        if (!unsafe_buffer_id)
            return std::nullopt;
        statement.unsafe_buffer_id = *unsafe_buffer_id;
    }
    if (!fields["unsafe_buffer_epoch"].empty())
    {
        const auto unsafe_buffer_epoch = parseUInt64(fields["unsafe_buffer_epoch"]);
        if (!unsafe_buffer_epoch)
            return std::nullopt;
        statement.unsafe_buffer_epoch = *unsafe_buffer_epoch;
    }

    if (statement.statement_id.empty()
        || statement.table_id.empty()
        || statement.unsafe_table.empty()
        || statement.safe_table.empty()
        || statement.payload_ref.empty()
        || statement.payload_hash.empty()
        || statement.replay_quorum == 0
        || statement.byte_side_quorum == 0
        || statement.participants.empty())
        return std::nullopt;

    std::set<std::string> seen;
    for (const auto & participant_id : statement.participants)
    {
        if (participant_id.empty() || !seen.insert(participant_id).second)
            return std::nullopt;
    }

    return statement;
}

bool storageIntegrityStatementUsesUnsafeBuffer(const HouseKeeperStorageStatement & statement)
{
    return statement.unsafe_buffer_epoch > 0;
}

std::string storageIntegrityPromotionGroupID(const HouseKeeperStorageStatement & statement, std::string_view partition_id)
{
    return "group-" + pathSafeID(statement.table_id)
        + "-b" + std::to_string(statement.unsafe_buffer_id)
        + "-e" + std::to_string(statement.unsafe_buffer_epoch)
        + "-p" + pathSafeID(partition_id);
}

std::optional<HouseKeeperStorageAttestation> storageIntegrityParseAttestation(
    std::string_view statement_id,
    std::string_view worker_id,
    std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    HouseKeeperStorageAttestation attestation;
    attestation.statement_id = std::string{statement_id};
    attestation.worker_id = std::string{worker_id};
    attestation.computed_state_root = fields["computed_state_root"];
    attestation.receipt_hash = fields["receipt_hash"];
    attestation.signature = fields["signature"];
    if (const auto match = parseBool(fields["match_source_root"]))
        attestation.match_source_root = *match;

    if (attestation.statement_id.empty()
        || attestation.worker_id.empty()
        || (attestation.computed_state_root.empty() && attestation.receipt_hash.empty())
        || !attestation.match_source_root
        || attestation.signature.empty())
        return std::nullopt;

    return attestation;
}

std::optional<HouseKeeperStorageUnsafeResult> storageIntegrityParseUnsafeResult(
    std::string_view statement_id,
    std::string_view participant_id,
    std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    auto row_count = parseUInt64(fields["row_count"]);
    auto replica_digests = parseReplicaDigests(fields["replica_digests"]);
    if (!row_count || fields["rows_hash"].empty() || replica_digests.size() != 1 || participant_id.empty())
        return std::nullopt;
    if (replica_digests.front().replica_id != participant_id)
        return std::nullopt;

    HouseKeeperStorageUnsafeResult result;
    result.statement_id = std::string{statement_id};
    result.participant_id = std::string{participant_id};
    result.row_count = *row_count;
    result.rows_hash = fields["rows_hash"];
    result.replicas = std::move(replica_digests);
    return result;
}

std::optional<HouseKeeperStorageByteSideScan> storageIntegrityParseByteSideScan(
    std::string_view statement_id,
    std::string_view worker_id,
    std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    auto parts = parseByteSideParts(fields["parts"]);
    if (!fields["parts"].empty() && parts.empty())
        return std::nullopt;

    HouseKeeperStorageByteSideScan scan;
    scan.statement_id = std::string{statement_id};
    scan.worker_id = std::string{worker_id};
    scan.scan_id = fields["scan_id"];
    scan.table_id = fields["table_id"];
    scan.unsafe_table = fields["unsafe_table"];
    scan.part_set_hash = fields["part_set_hash"];
    scan.parts = std::move(parts);

    if (!fields["statement_id"].empty() && fields["statement_id"] != scan.statement_id)
        return std::nullopt;
    if (!fields["worker_id"].empty() && fields["worker_id"] != scan.worker_id)
        return std::nullopt;
    if (scan.statement_id.empty()
        || scan.worker_id.empty()
        || scan.scan_id.empty()
        || scan.table_id.empty()
        || scan.unsafe_table.empty()
        || scan.part_set_hash.empty()
        || scan.parts.empty())
        return std::nullopt;

    return scan;
}

std::optional<HouseKeeperStorageFinality> storageIntegrityParseFinality(std::string_view statement_id, std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;
    const auto finalized = parseBool(fields["finalized"]);
    if (!finalized || !*finalized)
        return std::nullopt;
    return HouseKeeperStorageFinality{.statement_id = std::string{statement_id}, .finalized = true};
}

std::optional<HouseKeeperStorageRollback> storageIntegrityParseRollback(std::string_view statement_id, std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty() || fields["reason"].empty())
        return std::nullopt;
    return HouseKeeperStorageRollback{
        .statement_id = std::string{statement_id},
        .kind = fields["kind"],
        .reason = fields["reason"],
    };
}

bool storageIntegrityValidateUnsafeParticipantResult(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageUnsafeResult & result)
{
    if (result.statement_id != statement.statement_id || result.participant_id.empty() || result.rows_hash.empty() || result.replicas.size() != 1)
        return false;

    const auto participant_matches = std::find(statement.participants.begin(), statement.participants.end(), result.participant_id);
    if (participant_matches == statement.participants.end())
        return false;

    const auto & replica = result.replicas.front();
    return replica.replica_id == result.participant_id
        && replica.row_count == result.row_count
        && replica.rows_hash == result.rows_hash;
}

bool storageIntegrityValidateUnsafeResult(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageUnsafeResult & result)
{
    if (result.statement_id != statement.statement_id || result.rows_hash.empty())
        return false;

    std::set<std::string> expected(statement.participants.begin(), statement.participants.end());
    std::set<std::string> seen;
    for (const auto & replica : result.replicas)
    {
        if (!expected.contains(replica.replica_id) || !seen.insert(replica.replica_id).second)
            return false;
        if (replica.row_count != result.row_count || replica.rows_hash != result.rows_hash)
            return false;
    }

    return seen == expected;
}

bool storageIntegrityValidateByteSideScan(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageByteSideScan & scan)
{
    if (scan.statement_id != statement.statement_id
        || scan.table_id != statement.table_id
        || scan.unsafe_table != statement.unsafe_table
        || scan.worker_id.empty()
        || scan.scan_id.empty()
        || scan.part_set_hash.empty()
        || scan.parts.empty())
        return false;

    const auto participant_matches = std::find(statement.participants.begin(), statement.participants.end(), scan.worker_id);
    if (participant_matches == statement.participants.end())
        return false;

    if (!statement.candidate_parts.empty() && !byteSidePartsEqual(statement.candidate_parts, scan.parts))
        return false;

    return true;
}

std::string storageIntegritySerializeReplayJob(const HouseKeeperStorageStatement & statement)
{
    return "statement_id=" + statement.statement_id + "\n"
        "table_id=" + statement.table_id + "\n"
        "payload_ref=" + statement.payload_ref + "\n"
        "payload_hash=" + statement.payload_hash + "\n";
}

std::string storageIntegritySerializeUnsafeTask(const HouseKeeperStorageStatement & statement)
{
    auto data = "statement_id=" + statement.statement_id + "\n"
        "table_id=" + statement.table_id + "\n"
        "unsafe_table=" + statement.unsafe_table + "\n"
        "participants=" + joinCSV(statement.participants) + "\n";
    if (storageIntegrityStatementUsesUnsafeBuffer(statement))
        data += "unsafe_buffer_id=" + std::to_string(statement.unsafe_buffer_id) + "\n"
            "unsafe_buffer_epoch=" + std::to_string(statement.unsafe_buffer_epoch) + "\n";
    return data;
}

std::string storageIntegritySerializeByteSideScanTask(const HouseKeeperStorageStatement & statement)
{
    auto data = "scan_id=byteside-" + statement.statement_id + "\n"
        "statement_id=" + statement.statement_id + "\n"
        "table_id=" + statement.table_id + "\n"
        "unsafe_table=" + statement.unsafe_table + "\n"
        "partition_ids=" + joinCSV(statement.partition_ids) + "\n"
        "candidate_parts=" + serializeByteSideParts(statement.candidate_parts) + "\n";
    if (storageIntegrityStatementUsesUnsafeBuffer(statement))
        data += "unsafe_buffer_id=" + std::to_string(statement.unsafe_buffer_id) + "\n"
            "unsafe_buffer_epoch=" + std::to_string(statement.unsafe_buffer_epoch) + "\n";
    return data;
}


std::optional<HouseKeeperStorageDecision> storageIntegrityParseDecision(std::string_view statement_id, std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    HouseKeeperStorageDecision decision;
    decision.statement_id = std::string{statement_id};
    if (!fields["statement_id"].empty() && fields["statement_id"] != decision.statement_id)
        return std::nullopt;

    if (const auto value = parseBool(fields["replay_quorum_met"]))
        decision.replay_quorum_met = *value;
    if (const auto value = parseBool(fields["unsafe_validated"]))
        decision.unsafe_validated = *value;
    if (const auto value = parseBool(fields["byte_side_validated"]))
        decision.byte_side_validated = *value;
    if (const auto value = parseBool(fields["finalized"]))
        decision.finalized = *value;
    if (const auto value = parseBool(fields["rollback_requested"]))
        decision.rollback_requested = *value;
    if (const auto value = parseBool(fields["promotion_ready"]))
        decision.promotion_ready = *value;
    if (const auto value = parseBool(fields["rollback_ready"]))
        decision.rollback_ready = *value;
    decision.replay_result_hash = fields["replay_result_hash"];
    decision.byte_side_result_hash = fields["byte_side_result_hash"];
    decision.replay_tally = parseReplayTally(fields["replay_tally"]);
    decision.byte_side_tally = parseReplayTally(fields["byte_side_tally"]);
    return decision;
}

std::string storageIntegritySerializeDecision(const HouseKeeperStorageDecision & decision)
{
    return "statement_id=" + decision.statement_id + "\n"
        "replay_quorum_met=" + std::string{decision.replay_quorum_met ? "true" : "false"} + "\n"
        "unsafe_validated=" + std::string{decision.unsafe_validated ? "true" : "false"} + "\n"
        "byte_side_validated=" + std::string{decision.byte_side_validated ? "true" : "false"} + "\n"
        "finalized=" + std::string{decision.finalized ? "true" : "false"} + "\n"
        "rollback_requested=" + std::string{decision.rollback_requested ? "true" : "false"} + "\n"
        "promotion_ready=" + std::string{decision.promotion_ready ? "true" : "false"} + "\n"
        "rollback_ready=" + std::string{decision.rollback_ready ? "true" : "false"} + "\n"
        "replay_result_hash=" + decision.replay_result_hash + "\n"
        "byte_side_result_hash=" + decision.byte_side_result_hash + "\n"
        "replay_tally=" + serializeReplayTally(decision.replay_tally) + "\n"
        "byte_side_tally=" + serializeReplayTally(decision.byte_side_tally) + "\n";
}

std::string storageIntegritySerializePromotion(const HouseKeeperStorageStatement & statement)
{
    return storageIntegritySerializePromotion(statement, statement.statement_id, {statement.statement_id}, statement.partition_ids);
}

std::string storageIntegritySerializePromotion(
    const HouseKeeperStorageStatement & statement,
    std::string_view promotion_id,
    const std::vector<std::string> & statement_ids,
    const std::vector<std::string> & partition_ids)
{
    auto data = "promotion_id=promotion-" + std::string{promotion_id} + "\n"
        "lease_id=lease-" + std::string{promotion_id} + "\n"
        "unsafe_table=" + statement.unsafe_table + "\n"
        "safe_table=" + statement.safe_table + "\n"
        "partition_ids=" + joinCSV(partition_ids) + "\n";
    if (storageIntegrityStatementUsesUnsafeBuffer(statement))
        data += "unsafe_buffer_id=" + std::to_string(statement.unsafe_buffer_id) + "\n"
            "unsafe_buffer_epoch=" + std::to_string(statement.unsafe_buffer_epoch) + "\n"
            "statement_ids=" + joinCSV(statement_ids) + "\n";
    return data;
}

std::string storageIntegritySerializeRollbackTask(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageRollback & rollback)
{
    return "rollback_id=rollback-" + statement.statement_id + "\n"
        "lease_id=rollback-lease-" + statement.statement_id + "\n"
        "reason=" + rollback.reason + "\n"
        "unsafe_table=" + statement.unsafe_table + "\n";
}

std::string storageIntegritySerializeReplayQuarantine(const HouseKeeperStorageReplayQuarantine & quarantine)
{
    return "worker_id=" + quarantine.worker_id + "\n"
        "reason=" + quarantine.reason + "\n"
        "statement_id=" + quarantine.statement_id + "\n"
        "majority_hash=" + quarantine.majority_hash + "\n"
        "reported_hash=" + quarantine.reported_hash + "\n"
        "status=" + quarantine.status + "\n";
}

}
