#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DB
{

inline constexpr std::string_view housekeeper_storage_integrity_path = "/housekeeper/v1/storage_integrity";
inline constexpr std::string_view housekeeper_storage_integrity_statements_path = "/housekeeper/v1/storage_integrity/statements";
inline constexpr std::string_view housekeeper_storage_integrity_blocks_path = "/housekeeper/v1/storage_integrity/blocks";
inline constexpr std::string_view housekeeper_storage_integrity_replay_jobs_path = "/housekeeper/v1/storage_integrity/replay_jobs";
inline constexpr std::string_view housekeeper_storage_integrity_attestations_path = "/housekeeper/v1/storage_integrity/attestations";
inline constexpr std::string_view housekeeper_storage_integrity_replay_failures_path = "/housekeeper/v1/storage_integrity/replay_failures";
inline constexpr std::string_view housekeeper_storage_integrity_unsafe_tasks_path = "/housekeeper/v1/storage_integrity/unsafe_tasks";
inline constexpr std::string_view housekeeper_storage_integrity_unsafe_results_path = "/housekeeper/v1/storage_integrity/unsafe_results";
inline constexpr std::string_view housekeeper_storage_integrity_unsafe_failures_path = "/housekeeper/v1/storage_integrity/unsafe_failures";
inline constexpr std::string_view housekeeper_storage_integrity_finality_path = "/housekeeper/v1/storage_integrity/finality";
inline constexpr std::string_view housekeeper_storage_integrity_rollbacks_path = "/housekeeper/v1/storage_integrity/rollbacks";
inline constexpr std::string_view housekeeper_storage_integrity_promotions_path = "/housekeeper/v1/storage_integrity/promotions";
inline constexpr std::string_view housekeeper_storage_integrity_rollback_tasks_path = "/housekeeper/v1/storage_integrity/rollback_tasks";
inline constexpr std::string_view housekeeper_storage_integrity_replay_quarantine_path = "/housekeeper/v1/storage_integrity/replay_quarantine";
inline constexpr std::string_view housekeeper_storage_integrity_rollback_leases_path = "/housekeeper/v1/storage_integrity/rollback_leases";
inline constexpr std::string_view housekeeper_storage_integrity_rollback_results_path = "/housekeeper/v1/storage_integrity/rollback_results";
inline constexpr std::string_view housekeeper_storage_integrity_rollback_failures_path = "/housekeeper/v1/storage_integrity/rollback_failures";
inline constexpr std::string_view housekeeper_storage_integrity_promotion_leases_path = "/housekeeper/v1/storage_integrity/promotion_leases";
inline constexpr std::string_view housekeeper_storage_integrity_promotion_results_path = "/housekeeper/v1/storage_integrity/promotion_results";
inline constexpr std::string_view housekeeper_storage_integrity_promotion_failures_path = "/housekeeper/v1/storage_integrity/promotion_failures";
inline constexpr std::string_view housekeeper_storage_integrity_safe_audit_tasks_path = "/housekeeper/v1/storage_integrity/safe_audit_tasks";
inline constexpr std::string_view housekeeper_storage_integrity_safe_audit_votes_path = "/housekeeper/v1/storage_integrity/safe_audit_votes";
inline constexpr std::string_view housekeeper_storage_integrity_decisions_path = "/housekeeper/v1/storage_integrity/decisions";

struct HouseKeeperStorageStatement
{
    std::string statement_id;
    std::string table_id;
    std::string unsafe_table;
    std::string safe_table;
    std::string payload_ref;
    std::string payload_hash;
    uint64_t unsafe_buffer_id = 0;
    uint64_t unsafe_buffer_epoch = 0;
    size_t replay_quorum = 2;
    std::vector<std::string> participants;
    std::vector<std::string> partition_ids;
};

struct HouseKeeperStorageAttestation
{
    std::string statement_id;
    std::string worker_id;
    std::string computed_state_root;
    std::string receipt_hash;
    bool match_source_root = false;
    std::string signature;
};

struct HouseKeeperStorageReplicaDigest
{
    std::string replica_id;
    uint64_t row_count = 0;
    std::string rows_hash;
};

struct HouseKeeperStorageUnsafeResult
{
    std::string statement_id;
    std::string participant_id;
    uint64_t row_count = 0;
    std::string rows_hash;
    std::vector<HouseKeeperStorageReplicaDigest> replicas;
};

struct HouseKeeperStorageFinality
{
    std::string statement_id;
    bool finalized = false;
};

struct HouseKeeperStorageRollback
{
    std::string statement_id;
    std::string kind;
    std::string reason;
};

struct HouseKeeperStorageDecision
{
    std::string statement_id;
    bool replay_quorum_met = false;
    bool unsafe_validated = false;
    bool finalized = false;
    bool rollback_requested = false;
    bool promotion_ready = false;
    bool rollback_ready = false;
    std::string replay_result_hash;
    std::map<std::string, size_t> replay_tally;
};

struct HouseKeeperStorageReplayQuarantine
{
    std::string worker_id;
    std::string statement_id;
    std::string majority_hash;
    std::string reported_hash;
    std::string reason = "replay_minority_mismatch";
    std::string status = "active";
};

std::string storageIntegrityReplayJobPath(std::string_view statement_id);
std::string storageIntegrityAttestationsPath(std::string_view statement_id);
std::string storageIntegrityAttestationPath(std::string_view statement_id, std::string_view worker_id);
std::string storageIntegrityUnsafeTaskPath(std::string_view statement_id);
std::string storageIntegrityUnsafeResultPath(std::string_view statement_id);
std::string storageIntegrityUnsafeResultPath(std::string_view statement_id, std::string_view participant_id);
std::string storageIntegrityFinalityPath(std::string_view statement_id);
std::string storageIntegrityRollbackPath(std::string_view statement_id);
std::string storageIntegrityPromotionPath(std::string_view statement_id);
std::string storageIntegrityRollbackTaskPath(std::string_view statement_id);
std::string storageIntegrityReplayQuarantinePath(std::string_view worker_id);
std::string storageIntegrityDecisionPath(std::string_view statement_id);

std::optional<std::string> storageIntegrityStatementIDFromPath(std::string_view path);
std::optional<std::pair<std::string, std::string>> storageIntegrityAttestationPathParts(std::string_view path);
std::optional<std::string> storageIntegrityUnsafeResultIDFromPath(std::string_view path);
std::optional<std::pair<std::string, std::string>> storageIntegrityUnsafeResultPathParts(std::string_view path);
std::optional<std::string> storageIntegrityFinalityIDFromPath(std::string_view path);
std::optional<std::string> storageIntegrityRollbackIDFromPath(std::string_view path);

bool storageIntegrityIsUnderRoot(std::string_view path);
bool storageIntegrityIsControlPath(std::string_view path);
bool storageIntegrityIsManagedLedgerPath(std::string_view path);
bool storageIntegrityIsWorkerLedgerPath(std::string_view path);

std::optional<HouseKeeperStorageStatement> storageIntegrityParseStatement(std::string_view statement_id, std::string_view data);
std::optional<HouseKeeperStorageAttestation> storageIntegrityParseAttestation(
    std::string_view statement_id,
    std::string_view worker_id,
    std::string_view data);
std::optional<HouseKeeperStorageUnsafeResult> storageIntegrityParseUnsafeResult(
    std::string_view statement_id,
    std::string_view participant_id,
    std::string_view data);
std::optional<HouseKeeperStorageFinality> storageIntegrityParseFinality(std::string_view statement_id, std::string_view data);
std::optional<HouseKeeperStorageRollback> storageIntegrityParseRollback(std::string_view statement_id, std::string_view data);

bool storageIntegrityStatementUsesUnsafeBuffer(const HouseKeeperStorageStatement & statement);
std::string storageIntegrityPromotionGroupID(const HouseKeeperStorageStatement & statement, std::string_view partition_id);

bool storageIntegrityValidateUnsafeParticipantResult(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageUnsafeResult & result);
bool storageIntegrityValidateUnsafeResult(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageUnsafeResult & result);

std::string storageIntegritySerializeReplayJob(const HouseKeeperStorageStatement & statement);
std::string storageIntegritySerializeUnsafeTask(const HouseKeeperStorageStatement & statement);
std::optional<HouseKeeperStorageDecision> storageIntegrityParseDecision(std::string_view statement_id, std::string_view data);
std::string storageIntegritySerializeDecision(const HouseKeeperStorageDecision & decision);
std::string storageIntegritySerializePromotion(const HouseKeeperStorageStatement & statement);
std::string storageIntegritySerializePromotion(
    const HouseKeeperStorageStatement & statement,
    std::string_view promotion_id,
    const std::vector<std::string> & statement_ids,
    const std::vector<std::string> & partition_ids);
std::string storageIntegritySerializeRollbackTask(
    const HouseKeeperStorageStatement & statement,
    const HouseKeeperStorageRollback & rollback);
std::string storageIntegritySerializeReplayQuarantine(const HouseKeeperStorageReplayQuarantine & quarantine);

}
