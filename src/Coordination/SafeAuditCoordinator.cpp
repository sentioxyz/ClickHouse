#include <Coordination/SafeAuditCoordinator.h>

#include <algorithm>
#include <charconv>
#include <sstream>
#include <utility>

namespace DB
{

std::string_view safeAuditDecisionStatusName(SafeAuditDecisionStatus status)
{
    switch (status)
    {
        case SafeAuditDecisionStatus::Pending:
            return "pending";
        case SafeAuditDecisionStatus::Majority:
            return "majority";
        case SafeAuditDecisionStatus::Dispute:
            return "dispute";
    }

    return "unknown";
}

std::string_view safeAuditErrorName(SafeAuditError error)
{
    switch (error)
    {
        case SafeAuditError::Ok:
            return "ok";
        case SafeAuditError::AuditAlreadyExists:
            return "audit_already_exists";
        case SafeAuditError::AuditNotFound:
            return "audit_not_found";
        case SafeAuditError::AuditIDRequired:
            return "audit_id_required";
        case SafeAuditError::TableIDRequired:
            return "table_id_required";
        case SafeAuditError::SnapshotIDRequired:
            return "snapshot_id_required";
        case SafeAuditError::RangeRequired:
            return "range_required";
        case SafeAuditError::ReplicasRequired:
            return "replicas_required";
        case SafeAuditError::ReplicaIDRequired:
            return "replica_id_required";
        case SafeAuditError::DuplicateReplica:
            return "duplicate_replica";
        case SafeAuditError::WorkerIDRequired:
            return "worker_id_required";
        case SafeAuditError::BatchHashRequired:
            return "batch_hash_required";
        case SafeAuditError::VoteHashRequired:
            return "vote_hash_required";
        case SafeAuditError::SignatureRequired:
            return "signature_required";
        case SafeAuditError::ReplicaNotExpected:
            return "replica_not_expected";
        case SafeAuditError::DuplicateVote:
            return "duplicate_vote";
        case SafeAuditError::SnapshotMismatch:
            return "snapshot_mismatch";
        case SafeAuditError::RangeMismatch:
            return "range_mismatch";
    }

    return "unknown";
}

namespace
{

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
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

std::string childPath(std::string_view parent, std::string_view child)
{
    return std::string{parent} + "/" + std::string{child};
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
            if (key.empty() || value.empty())
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

SafeAuditError validateTask(const SafeAuditTask & task)
{
    if (task.audit_id.empty())
        return SafeAuditError::AuditIDRequired;
    if (task.table_id.empty())
        return SafeAuditError::TableIDRequired;
    if (task.snapshot_id.empty())
        return SafeAuditError::SnapshotIDRequired;
    if (task.range.empty())
        return SafeAuditError::RangeRequired;
    if (task.replicas.empty())
        return SafeAuditError::ReplicasRequired;

    std::set<std::string> seen;
    for (const auto & replica_id : task.replicas)
    {
        if (replica_id.empty())
            return SafeAuditError::ReplicaIDRequired;
        if (!seen.insert(replica_id).second)
            return SafeAuditError::DuplicateReplica;
    }

    return SafeAuditError::Ok;
}

SafeAuditError validateVote(const SafeAuditVote & vote)
{
    if (vote.audit_id.empty())
        return SafeAuditError::AuditIDRequired;
    if (vote.replica_id.empty())
        return SafeAuditError::ReplicaIDRequired;
    if (vote.worker_id.empty())
        return SafeAuditError::WorkerIDRequired;
    if (vote.snapshot_id.empty())
        return SafeAuditError::SnapshotIDRequired;
    if (vote.range.empty())
        return SafeAuditError::RangeRequired;
    if (vote.batch_hash.empty())
        return SafeAuditError::BatchHashRequired;
    if (vote.vote_hash.empty())
        return SafeAuditError::VoteHashRequired;
    if (vote.signature.empty())
        return SafeAuditError::SignatureRequired;
    return SafeAuditError::Ok;
}

SafeAuditDecision makeEmptyDecision(std::string audit_id)
{
    SafeAuditDecision decision;
    decision.audit_id = std::move(audit_id);
    return decision;
}

}

std::string safeAuditVotesPath(std::string_view audit_id)
{
    return childPath(housekeeper_safe_audit_votes_path, audit_id);
}

std::string safeAuditDecisionPath(std::string_view audit_id)
{
    return childPath(housekeeper_safe_audit_decisions_path, audit_id);
}

std::string safeAuditQuarantineAuditPath(std::string_view audit_id)
{
    return childPath(housekeeper_safe_audit_quarantine_path, audit_id);
}

std::string safeAuditQuarantineReplicaPath(std::string_view audit_id, std::string_view replica_id)
{
    return childPath(safeAuditQuarantineAuditPath(audit_id), replica_id);
}

std::optional<std::string> safeAuditTaskIDFromPath(std::string_view path)
{
    return directChildID(path, housekeeper_safe_audit_tasks_path);
}

std::optional<std::pair<std::string, std::string>> safeAuditVotePathParts(std::string_view path)
{
    if (!startsWith(path, housekeeper_safe_audit_votes_path)
        || path.size() <= housekeeper_safe_audit_votes_path.size()
        || path[housekeeper_safe_audit_votes_path.size()] != '/')
        return std::nullopt;

    const auto rest = path.substr(housekeeper_safe_audit_votes_path.size() + 1);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= rest.size())
        return std::nullopt;

    const auto audit_id = rest.substr(0, slash);
    const auto replica_id = rest.substr(slash + 1);
    if (replica_id.find('/') != std::string_view::npos)
        return std::nullopt;

    return std::pair{std::string{audit_id}, std::string{replica_id}};
}

bool safeAuditIsUnderRoot(std::string_view path)
{
    return path == housekeeper_safe_audits_path
        || (startsWith(path, housekeeper_safe_audits_path)
            && path.size() > housekeeper_safe_audits_path.size()
            && path[housekeeper_safe_audits_path.size()] == '/');
}

bool safeAuditIsControlPath(std::string_view path)
{
    return path == housekeeper_safe_audits_path
        || path == housekeeper_safe_audit_tasks_path
        || path == housekeeper_safe_audit_votes_path
        || path == housekeeper_safe_audit_decisions_path
        || path == housekeeper_safe_audit_quarantine_path;
}

bool safeAuditIsManagedLedgerPath(std::string_view path)
{
    if (safeAuditIsControlPath(path))
        return false;

    if (directChildID(path, housekeeper_safe_audit_decisions_path))
        return true;

    if (directChildID(path, housekeeper_safe_audit_votes_path))
        return true;

    if (directChildID(path, housekeeper_safe_audit_quarantine_path))
        return true;

    return startsWith(path, housekeeper_safe_audit_decisions_path)
        || startsWith(path, housekeeper_safe_audit_quarantine_path);
}

std::optional<SafeAuditTask> safeAuditParseTask(std::string_view audit_id, std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    SafeAuditTask task;
    task.audit_id = std::string{audit_id};
    task.network_id = fields["network_id"];
    task.table_id = fields["table_id"];
    task.schema_hash = fields["schema_hash"];
    task.snapshot_id = fields["snapshot_id"];
    task.range = fields["range"];
    task.replicas = splitCSV(fields["replicas"]);

    if (validateTask(task) != SafeAuditError::Ok)
        return std::nullopt;

    return task;
}

std::optional<SafeAuditVote> safeAuditParseVote(std::string_view audit_id, std::string_view replica_id, std::string_view data)
{
    auto fields = parseKeyValueLines(data);
    if (fields.empty())
        return std::nullopt;

    auto row_count = parseUInt64(fields["row_count"]);
    if (!row_count)
        return std::nullopt;

    SafeAuditVote vote;
    vote.audit_id = std::string{audit_id};
    vote.worker_id = fields["worker_id"];
    vote.replica_id = std::string{replica_id};
    vote.snapshot_id = fields["snapshot_id"];
    vote.range = fields["range"];
    vote.batch_hash = fields["batch_hash"];
    vote.row_count = *row_count;
    vote.vote_hash = fields["vote_hash"];
    vote.signature = fields["signature"];

    if (validateVote(vote) != SafeAuditError::Ok)
        return std::nullopt;

    return vote;
}

std::string safeAuditSerializeDecision(const SafeAuditDecision & decision)
{
    std::ostringstream out;
    out << "audit_id=" << decision.audit_id << "\n"
        << "status=" << safeAuditDecisionStatusName(decision.status) << "\n"
        << "majority_hash=" << decision.majority_hash << "\n"
        << "majority_count=" << decision.majority_count << "\n"
        << "total_votes=" << decision.total_votes << "\n"
        << "expected_votes=" << decision.expected_votes << "\n"
        << "minority_replicas=" << joinCSV(decision.minority_replicas) << "\n";
    return out.str();
}

std::string safeAuditSerializeQuarantineAction(const SafeAuditDecision & decision, const SafeAuditVote & vote)
{
    std::ostringstream out;
    out << "audit_id=" << decision.audit_id << "\n"
        << "replica_id=" << vote.replica_id << "\n"
        << "reason=safe_audit_minority\n"
        << "majority_hash=" << decision.majority_hash << "\n"
        << "replica_batch_hash=" << vote.batch_hash << "\n"
        << "vote_hash=" << vote.vote_hash << "\n";
    return out.str();
}

bool SafeAuditSubmitResult::accepted() const
{
    return error == SafeAuditError::Ok;
}

SafeAuditError SafeAuditCoordinator::createAudit(SafeAuditTask task)
{
    if (const auto error = validateTask(task); error != SafeAuditError::Ok)
        return error;

    std::sort(task.replicas.begin(), task.replicas.end());

    AuditState state;
    state.task = std::move(task);
    state.expected_replicas = std::set<std::string>(state.task.replicas.begin(), state.task.replicas.end());
    state.decision = makeEmptyDecision(state.task.audit_id);
    state.decision.expected_votes = state.expected_replicas.size();

    const auto audit_id = state.task.audit_id;
    const auto [_, inserted] = audits.emplace(audit_id, std::move(state));
    if (!inserted)
        return SafeAuditError::AuditAlreadyExists;

    return SafeAuditError::Ok;
}

bool SafeAuditCoordinator::hasAudit(std::string_view audit_id) const
{
    return audits.find(std::string{audit_id}) != audits.end();
}

size_t SafeAuditCoordinator::auditCount() const
{
    return audits.size();
}

std::optional<SafeAuditTask> SafeAuditCoordinator::getTask(std::string_view audit_id) const
{
    const auto it = audits.find(std::string{audit_id});
    if (it == audits.end())
        return std::nullopt;
    return it->second.task;
}

std::optional<SafeAuditDecision> SafeAuditCoordinator::getDecision(std::string_view audit_id) const
{
    const auto it = audits.find(std::string{audit_id});
    if (it == audits.end())
        return std::nullopt;
    return it->second.decision;
}

SafeAuditSubmitResult SafeAuditCoordinator::submitVote(SafeAuditVote vote)
{
    if (const auto error = validateVote(vote); error != SafeAuditError::Ok)
        return SafeAuditSubmitResult{.error = error, .decision = {}};

    auto it = audits.find(vote.audit_id);
    if (it == audits.end())
        return SafeAuditSubmitResult{.error = SafeAuditError::AuditNotFound, .decision = {}};

    auto & state = it->second;
    if (!state.expected_replicas.contains(vote.replica_id))
        return SafeAuditSubmitResult{.error = SafeAuditError::ReplicaNotExpected, .decision = state.decision};
    if (state.votes.contains(vote.replica_id))
        return SafeAuditSubmitResult{.error = SafeAuditError::DuplicateVote, .decision = state.decision};
    if (vote.snapshot_id != state.task.snapshot_id)
        return SafeAuditSubmitResult{.error = SafeAuditError::SnapshotMismatch, .decision = state.decision};
    if (vote.range != state.task.range)
        return SafeAuditSubmitResult{.error = SafeAuditError::RangeMismatch, .decision = state.decision};

    state.votes.emplace(vote.replica_id, std::move(vote));
    state.decision = evaluate(state);
    return SafeAuditSubmitResult{.error = SafeAuditError::Ok, .decision = state.decision};
}

SafeAuditDecision SafeAuditCoordinator::evaluate(const AuditState & state) const
{
    SafeAuditDecision decision;
    decision.audit_id = state.task.audit_id;
    decision.status = SafeAuditDecisionStatus::Pending;
    decision.total_votes = state.votes.size();
    decision.expected_votes = state.expected_replicas.size();

    if (state.expected_replicas.empty())
        return decision;

    const auto threshold = state.expected_replicas.size() / 2 + 1;
    std::map<std::string, size_t> count_by_hash;
    for (const auto & [_, vote] : state.votes)
        ++count_by_hash[vote.batch_hash];

    for (const auto & [hash, count] : count_by_hash)
    {
        if (count >= threshold && count > decision.majority_count)
        {
            decision.status = SafeAuditDecisionStatus::Majority;
            decision.majority_hash = hash;
            decision.majority_count = count;
        }
    }

    if (decision.status == SafeAuditDecisionStatus::Majority)
    {
        for (const auto & [replica_id, vote] : state.votes)
        {
            if (vote.batch_hash != decision.majority_hash)
                decision.minority_replicas.push_back(replica_id);
        }
        return decision;
    }

    if (state.votes.size() >= state.expected_replicas.size())
        decision.status = SafeAuditDecisionStatus::Dispute;

    return decision;
}

}
