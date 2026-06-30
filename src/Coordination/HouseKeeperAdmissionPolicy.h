#pragma once

#include <Coordination/SafeAuditCoordinator.h>
#include <Coordination/HouseKeeperStorageIntegrityCoordinator.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    enum class Operation
    {
        Create,
        Set,
    };

    std::string path;
    std::string data;
    Operation operation = Operation::Create;
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
            .operation = HouseKeeperWriteCandidate::Operation::Create,
            .failed_pos = failed_pos,
        });
        return;
    }

    if (const auto * set_request = dynamic_cast<const Coordination::ZooKeeperSetRequest *>(&request))
    {
        candidates.push_back(HouseKeeperWriteCandidate{
            .path = set_request->path,
            .data = set_request->data,
            .operation = HouseKeeperWriteCandidate::Operation::Set,
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
std::optional<std::string> houseKeeperGetNodeData(Storage & storage, const std::string & path)
{
    auto node = storage.uncommitted_state.getNode(path);
    if (!node)
        return std::nullopt;

    return std::string{node->getData()};
}

template <typename Storage>
SafeAuditSubmitResult houseKeeperEvaluateSafeAuditVote(Storage & storage, const SafeAuditVote & candidate_vote)
{
    const auto task_data = houseKeeperGetNodeData(storage, std::string{housekeeper_safe_audit_tasks_path} + "/" + candidate_vote.audit_id);
    if (!task_data)
        return SafeAuditSubmitResult{.error = SafeAuditError::AuditNotFound, .decision = {}};

    const auto task = safeAuditParseTask(candidate_vote.audit_id, *task_data);
    if (!task)
        return SafeAuditSubmitResult{.error = SafeAuditError::AuditNotFound, .decision = {}};

    SafeAuditCoordinator coordinator;
    if (coordinator.createAudit(*task) != SafeAuditError::Ok)
        return SafeAuditSubmitResult{.error = SafeAuditError::AuditNotFound, .decision = {}};

    for (const auto & replica_id : task->replicas)
    {
        const auto vote_data = houseKeeperGetNodeData(storage, safeAuditVotesPath(task->audit_id) + "/" + replica_id);
        if (!vote_data)
            continue;

        const auto existing_vote = safeAuditParseVote(task->audit_id, replica_id, *vote_data);
        if (!existing_vote)
            return SafeAuditSubmitResult{.error = SafeAuditError::VoteHashRequired, .decision = {}};

        const auto result = coordinator.submitVote(*existing_vote);
        if (!result.accepted())
            return result;
    }

    return coordinator.submitVote(candidate_vote);
}

template <typename Storage>
std::optional<HouseKeeperStorageStatement> houseKeeperStorageIntegrityGetStatement(Storage & storage, const std::string & statement_id)
{
    const auto statement_data = houseKeeperGetNodeData(
        storage,
        std::string{housekeeper_storage_integrity_statements_path} + "/" + statement_id);
    if (!statement_data)
        return std::nullopt;
    return storageIntegrityParseStatement(statement_id, *statement_data);
}

template <typename Storage>
bool houseKeeperStorageIntegrityWorkerQuarantined(Storage & storage, const std::string & worker_id)
{
    if (worker_id.empty())
        return false;
    const auto data = houseKeeperGetNodeData(storage, storageIntegrityReplayQuarantinePath(worker_id));
    if (!data)
        return false;

    return data->find("status=inactive\n") == std::string::npos;
}

template <typename Storage>
std::vector<HouseKeeperStorageAttestation> houseKeeperStorageIntegrityGetAttestations(
    Storage & storage,
    const std::string & statement_id,
    std::optional<HouseKeeperStorageAttestation> candidate_attestation = std::nullopt)
{
    std::vector<HouseKeeperStorageAttestation> attestations;
    const auto parent_path = storageIntegrityAttestationsPath(statement_id);

    if constexpr (Storage::use_rocksdb)
    {
        for (const auto & [path, node] : storage.container.getChildren(parent_path, true, true))
        {
            const auto parts = storageIntegrityAttestationPathParts(path);
            if (!parts)
                continue;
            const auto attestation = storageIntegrityParseAttestation(parts->first, parts->second, node.getData());
            if (attestation)
                attestations.push_back(*attestation);
        }
    }
    else
    {
        if (auto node = storage.uncommitted_state.getNode(parent_path))
        {
            for (const auto & child : node->getChildren())
            {
                const auto path = storageIntegrityAttestationPath(statement_id, child);
                const auto data = houseKeeperGetNodeData(storage, path);
                if (!data)
                    continue;
                const auto attestation = storageIntegrityParseAttestation(statement_id, child, *data);
                if (attestation)
                    attestations.push_back(*attestation);
            }
        }
    }

    if (candidate_attestation)
    {
        const auto exists = std::ranges::any_of(attestations, [&](const auto & attestation)
        {
            return attestation.worker_id == candidate_attestation->worker_id;
        });
        if (!exists)
            attestations.push_back(std::move(*candidate_attestation));
    }

    return attestations;
}

inline bool houseKeeperStorageIntegrityHasReplayQuorum(
    const HouseKeeperStorageStatement & statement,
    const std::vector<HouseKeeperStorageAttestation> & attestations,
    std::string * quorum_hash = nullptr)
{
    std::map<std::string, size_t> by_result;
    for (const auto & attestation : attestations)
    {
        if (!attestation.match_source_root)
            continue;
        auto key = attestation.computed_state_root.empty() ? attestation.receipt_hash : attestation.computed_state_root;
        if (key.empty())
            continue;
        ++by_result[key];
    }

    for (const auto & [result_hash, count] : by_result)
    {
        if (count >= statement.replay_quorum)
        {
            if (quorum_hash)
                *quorum_hash = result_hash;
            return true;
        }
    }
    return false;
}

template <typename Storage>
std::optional<HouseKeeperStorageUnsafeResult> houseKeeperStorageIntegrityGetUnsafeResult(
    Storage & storage,
    const HouseKeeperStorageStatement & statement,
    std::optional<HouseKeeperStorageUnsafeResult> candidate_result = std::nullopt)
{
    std::map<std::string, HouseKeeperStorageUnsafeResult> by_participant;
    if (candidate_result)
        by_participant.emplace(candidate_result->participant_id, *candidate_result);

    for (const auto & participant_id : statement.participants)
    {
        if (by_participant.contains(participant_id))
            continue;
        const auto data = houseKeeperGetNodeData(storage, storageIntegrityUnsafeResultPath(statement.statement_id, participant_id));
        if (!data)
            continue;
        const auto result = storageIntegrityParseUnsafeResult(statement.statement_id, participant_id, *data);
        if (result)
            by_participant.emplace(participant_id, *result);
    }

    if (by_participant.size() != statement.participants.size() || by_participant.empty())
        return std::nullopt;

    HouseKeeperStorageUnsafeResult aggregate;
    aggregate.statement_id = statement.statement_id;
    for (const auto & participant_id : statement.participants)
    {
        const auto it = by_participant.find(participant_id);
        if (it == by_participant.end() || !storageIntegrityValidateUnsafeParticipantResult(statement, it->second))
            return std::nullopt;

        const auto & result = it->second;
        if (aggregate.rows_hash.empty())
        {
            aggregate.row_count = result.row_count;
            aggregate.rows_hash = result.rows_hash;
        }
        else if (aggregate.row_count != result.row_count || aggregate.rows_hash != result.rows_hash)
        {
            return std::nullopt;
        }

        aggregate.replicas.push_back(result.replicas.front());
    }

    return aggregate;
}

template <typename Storage>
bool houseKeeperStorageIntegrityHasFinality(
    Storage & storage,
    const std::string & statement_id,
    std::optional<HouseKeeperStorageFinality> candidate_finality = std::nullopt)
{
    if (candidate_finality)
        return candidate_finality->finalized;
    const auto data = houseKeeperGetNodeData(storage, storageIntegrityFinalityPath(statement_id));
    if (!data)
        return false;
    const auto finality = storageIntegrityParseFinality(statement_id, *data);
    return finality && finality->finalized;
}

template <typename Storage>
std::optional<HouseKeeperStorageRollback> houseKeeperStorageIntegrityGetRollback(
    Storage & storage,
    const std::string & statement_id,
    std::optional<HouseKeeperStorageRollback> candidate_rollback = std::nullopt)
{
    if (candidate_rollback)
        return candidate_rollback;
    const auto data = houseKeeperGetNodeData(storage, storageIntegrityRollbackPath(statement_id));
    if (!data)
        return std::nullopt;
    return storageIntegrityParseRollback(statement_id, *data);
}


template <typename Storage>
HouseKeeperStorageDecision houseKeeperStorageIntegrityGetDecision(Storage & storage, const std::string & statement_id)
{
    if (const auto data = houseKeeperGetNodeData(storage, storageIntegrityDecisionPath(statement_id)))
    {
        if (const auto decision = storageIntegrityParseDecision(statement_id, *data))
            return *decision;
    }
    HouseKeeperStorageDecision decision;
    decision.statement_id = statement_id;
    return decision;
}

inline void houseKeeperStorageIntegrityApplyReplayTally(
    HouseKeeperStorageDecision & decision,
    const HouseKeeperStorageStatement & statement,
    const std::vector<HouseKeeperStorageAttestation> & attestations)
{
    for (const auto & attestation : attestations)
    {
        if (!attestation.match_source_root)
            continue;
        auto key = attestation.computed_state_root.empty() ? attestation.receipt_hash : attestation.computed_state_root;
        if (key.empty())
            continue;
        ++decision.replay_tally[key];
    }

    decision.replay_quorum_met = false;
    decision.replay_result_hash.clear();
    for (const auto & [result_hash, count] : decision.replay_tally)
    {
        if (count >= statement.replay_quorum)
        {
            decision.replay_quorum_met = true;
            decision.replay_result_hash = result_hash;
            return;
        }
    }
}

template <typename Storage>
HouseKeeperStorageDecision houseKeeperStorageIntegrityEvaluate(
    Storage & storage,
    const HouseKeeperStorageStatement & statement,
    std::optional<HouseKeeperStorageAttestation> candidate_attestation = std::nullopt,
    std::optional<HouseKeeperStorageUnsafeResult> candidate_unsafe_result = std::nullopt,
    std::optional<HouseKeeperStorageFinality> candidate_finality = std::nullopt,
    std::optional<HouseKeeperStorageRollback> candidate_rollback = std::nullopt)
{
    auto decision = houseKeeperStorageIntegrityGetDecision(storage, statement.statement_id);

    std::vector<HouseKeeperStorageAttestation> replay_votes;
    if (candidate_attestation)
        replay_votes.push_back(std::move(*candidate_attestation));
    else if (decision.replay_tally.empty())
        replay_votes = houseKeeperStorageIntegrityGetAttestations(storage, statement.statement_id);
    if (!replay_votes.empty() || decision.replay_tally.empty())
        houseKeeperStorageIntegrityApplyReplayTally(decision, statement, replay_votes);

    const auto unsafe_result = houseKeeperStorageIntegrityGetUnsafeResult(storage, statement, std::move(candidate_unsafe_result));
    decision.unsafe_validated = unsafe_result && storageIntegrityValidateUnsafeResult(statement, *unsafe_result);
    decision.finalized = houseKeeperStorageIntegrityHasFinality(storage, statement.statement_id, std::move(candidate_finality));
    decision.rollback_requested = houseKeeperStorageIntegrityGetRollback(storage, statement.statement_id, std::move(candidate_rollback)).has_value();
    decision.rollback_ready = decision.rollback_requested;
    decision.promotion_ready = !decision.rollback_requested
        && decision.replay_quorum_met
        && decision.unsafe_validated
        && decision.finalized;
    return decision;
}

template <typename Storage>
std::optional<HouseKeeperAdmissionRejection> checkHouseKeeperStorageIntegrityAdmission(
    const Coordination::ZooKeeperRequest & request,
    Storage & storage,
    const std::vector<HouseKeeperWriteCandidate> & candidates)
{
    const bool is_multi = request.getOpNum() == Coordination::OpNum::Multi || request.getOpNum() == Coordination::OpNum::MultiRead;
    for (const auto & candidate : candidates)
    {
        if (!storageIntegrityIsUnderRoot(candidate.path))
            continue;

        if (storageIntegrityIsControlPath(candidate.path))
            continue;

        if (is_multi)
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

        if (storageIntegrityIsManagedLedgerPath(candidate.path))
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

        if (const auto statement_id = storageIntegrityStatementIDFromPath(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (!storageIntegrityParseStatement(*statement_id, candidate.data))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (houseKeeperNodeExists(storage, storageIntegrityReplayJobPath(*statement_id))
                || houseKeeperNodeExists(storage, storageIntegrityAttestationsPath(*statement_id))
                || houseKeeperNodeExists(storage, storageIntegrityUnsafeTaskPath(*statement_id))
                || houseKeeperNodeExists(storage, storageIntegrityUnsafeResultPath(*statement_id))
                || houseKeeperNodeExists(storage, storageIntegrityDecisionPath(*statement_id)))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            continue;
        }

        if (const auto attestation_path = storageIntegrityAttestationPathParts(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            const auto statement = houseKeeperStorageIntegrityGetStatement(storage, attestation_path->first);
            if (!statement)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (houseKeeperStorageIntegrityWorkerQuarantined(storage, attestation_path->second))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (!storageIntegrityParseAttestation(attestation_path->first, attestation_path->second, candidate.data))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            continue;
        }

        if (const auto result_path = storageIntegrityUnsafeResultPathParts(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            const auto statement = houseKeeperStorageIntegrityGetStatement(storage, result_path->first);
            const auto result = storageIntegrityParseUnsafeResult(result_path->first, result_path->second, candidate.data);
            if (!statement || !result || !storageIntegrityValidateUnsafeParticipantResult(*statement, *result))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            continue;
        }

        if (storageIntegrityIsWorkerLedgerPath(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create
                && candidate.operation != HouseKeeperWriteCandidate::Operation::Set)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            continue;
        }

        if (const auto statement_id = storageIntegrityFinalityIDFromPath(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (!houseKeeperStorageIntegrityGetStatement(storage, *statement_id)
                || !storageIntegrityParseFinality(*statement_id, candidate.data))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            continue;
        }

        if (const auto statement_id = storageIntegrityRollbackIDFromPath(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (!houseKeeperStorageIntegrityGetStatement(storage, *statement_id)
                || !storageIntegrityParseRollback(*statement_id, candidate.data))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            continue;
        }

        return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
    }

    return std::nullopt;
}

template <typename Storage>
std::optional<HouseKeeperAdmissionRejection> checkHouseKeeperSafeAuditAdmission(
    const Coordination::ZooKeeperRequest & request,
    Storage & storage,
    const std::vector<HouseKeeperWriteCandidate> & candidates)
{
    const bool is_multi = request.getOpNum() == Coordination::OpNum::Multi || request.getOpNum() == Coordination::OpNum::MultiRead;
    for (const auto & candidate : candidates)
    {
        if (!safeAuditIsUnderRoot(candidate.path))
            continue;

        if (safeAuditIsControlPath(candidate.path))
            continue;

        if (is_multi)
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

        if (safeAuditIsManagedLedgerPath(candidate.path))
            return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

        if (const auto audit_id = safeAuditTaskIDFromPath(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

            if (!safeAuditParseTask(*audit_id, candidate.data))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

            if (houseKeeperNodeExists(storage, safeAuditVotesPath(*audit_id))
                || houseKeeperNodeExists(storage, safeAuditDecisionPath(*audit_id))
                || houseKeeperNodeExists(storage, safeAuditQuarantineAuditPath(*audit_id)))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

            continue;
        }

        if (const auto vote_path = safeAuditVotePathParts(candidate.path))
        {
            if (candidate.operation != HouseKeeperWriteCandidate::Operation::Create)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
            if (houseKeeperStorageIntegrityWorkerQuarantined(storage, vote_path->second))
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

            const auto vote = safeAuditParseVote(vote_path->first, vote_path->second, candidate.data);
            if (!vote)
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

            const auto result = houseKeeperEvaluateSafeAuditVote(storage, *vote);
            if (!result.accepted())
                return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);

            continue;
        }

        return houseKeeperReject(candidate, Coordination::Error::ZBADARGUMENTS);
    }

    return std::nullopt;
}

template <typename Storage>
std::optional<HouseKeeperAdmissionRejection> checkHouseKeeperAdmission(
    const Coordination::ZooKeeperRequest & request,
    Storage & storage)
{
    std::vector<HouseKeeperWriteCandidate> candidates;
    houseKeeperCollectWriteCandidates(request, candidates);

    if (auto storage_integrity_rejection = checkHouseKeeperStorageIntegrityAdmission(request, storage, candidates))
        return storage_integrity_rejection;

    if (auto safe_audit_rejection = checkHouseKeeperSafeAuditAdmission(request, storage, candidates))
        return safe_audit_rejection;

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
