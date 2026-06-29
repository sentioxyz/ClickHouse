#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DB
{

inline constexpr std::string_view housekeeper_safe_audits_path = "/housekeeper/v1/safe_audits";
inline constexpr std::string_view housekeeper_safe_audit_tasks_path = "/housekeeper/v1/safe_audits/tasks";
inline constexpr std::string_view housekeeper_safe_audit_votes_path = "/housekeeper/v1/safe_audits/votes";
inline constexpr std::string_view housekeeper_safe_audit_decisions_path = "/housekeeper/v1/safe_audits/decisions";
inline constexpr std::string_view housekeeper_safe_audit_quarantine_path = "/housekeeper/v1/safe_audits/quarantine";

enum class SafeAuditDecisionStatus
{
    Pending,
    Majority,
    Dispute,
};

std::string_view safeAuditDecisionStatusName(SafeAuditDecisionStatus status);

enum class SafeAuditError
{
    Ok,
    AuditAlreadyExists,
    AuditNotFound,
    AuditIDRequired,
    TableIDRequired,
    SnapshotIDRequired,
    RangeRequired,
    ReplicasRequired,
    ReplicaIDRequired,
    DuplicateReplica,
    WorkerIDRequired,
    BatchHashRequired,
    VoteHashRequired,
    SignatureRequired,
    ReplicaNotExpected,
    DuplicateVote,
    SnapshotMismatch,
    RangeMismatch,
};

std::string_view safeAuditErrorName(SafeAuditError error);

struct SafeAuditTask
{
    std::string audit_id;
    std::string network_id;
    std::string table_id;
    std::string schema_hash;
    std::string snapshot_id;
    std::string range;
    std::vector<std::string> replicas;
};

struct SafeAuditVote
{
    std::string audit_id;
    std::string worker_id;
    std::string replica_id;
    std::string snapshot_id;
    std::string range;
    std::string batch_hash;
    uint64_t row_count = 0;
    std::string vote_hash;
    std::string signature;
};

struct SafeAuditDecision
{
    std::string audit_id;
    SafeAuditDecisionStatus status = SafeAuditDecisionStatus::Pending;
    std::string majority_hash;
    size_t majority_count = 0;
    size_t total_votes = 0;
    size_t expected_votes = 0;
    std::vector<std::string> minority_replicas;
};

struct SafeAuditSubmitResult
{
    SafeAuditError error = SafeAuditError::Ok;
    SafeAuditDecision decision;

    bool accepted() const;
};

/// Keeper-side deterministic state machine for post-promotion safe-table audits.
///
/// HouseGate SafeAuditWorker instances read ClickHouse replicas and compute
/// batch_hash values. The coordinator only persists audit metadata, validates
/// vote scope, and decides whether a strict majority of expected replicas agree
/// on the same batch_hash.
class SafeAuditCoordinator
{
public:
    SafeAuditError createAudit(SafeAuditTask task);
    SafeAuditSubmitResult submitVote(SafeAuditVote vote);

    bool hasAudit(std::string_view audit_id) const;
    size_t auditCount() const;

    std::optional<SafeAuditTask> getTask(std::string_view audit_id) const;
    std::optional<SafeAuditDecision> getDecision(std::string_view audit_id) const;

private:
    struct AuditState
    {
        SafeAuditTask task;
        std::set<std::string> expected_replicas;
        std::map<std::string, SafeAuditVote> votes;
        SafeAuditDecision decision;
    };

    SafeAuditDecision evaluate(const AuditState & state) const;

    std::map<std::string, AuditState> audits;
};

std::string safeAuditVotesPath(std::string_view audit_id);
std::string safeAuditDecisionPath(std::string_view audit_id);
std::string safeAuditQuarantineAuditPath(std::string_view audit_id);
std::string safeAuditQuarantineReplicaPath(std::string_view audit_id, std::string_view replica_id);

std::optional<std::string> safeAuditTaskIDFromPath(std::string_view path);
std::optional<std::pair<std::string, std::string>> safeAuditVotePathParts(std::string_view path);
bool safeAuditIsManagedLedgerPath(std::string_view path);
bool safeAuditIsControlPath(std::string_view path);
bool safeAuditIsUnderRoot(std::string_view path);

std::optional<SafeAuditTask> safeAuditParseTask(std::string_view audit_id, std::string_view data);
std::optional<SafeAuditVote> safeAuditParseVote(std::string_view audit_id, std::string_view replica_id, std::string_view data);

std::string safeAuditSerializeDecision(const SafeAuditDecision & decision);
std::string safeAuditSerializeQuarantineAction(const SafeAuditDecision & decision, const SafeAuditVote & vote);

}
