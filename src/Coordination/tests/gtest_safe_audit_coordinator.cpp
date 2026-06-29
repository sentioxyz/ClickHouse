#include <gtest/gtest.h>

#include <Coordination/SafeAuditCoordinator.h>

#include <utility>

using namespace DB;

namespace
{

SafeAuditTask makeSafeAuditTask(std::vector<std::string> replicas = {"replica-a", "replica-b", "replica-c"})
{
    return SafeAuditTask{
        .audit_id = "audit-1",
        .network_id = "net-1",
        .table_id = "db.table",
        .schema_hash = "schema-hash",
        .snapshot_id = "snapshot-1",
        .range = "partition=20260626",
        .replicas = std::move(replicas),
    };
}

SafeAuditVote makeSafeAuditVote(
    std::string replica_id,
    std::string batch_hash,
    uint64_t row_count = 10,
    std::string audit_id = "audit-1")
{
    return SafeAuditVote{
        .audit_id = std::move(audit_id),
        .worker_id = "worker-" + replica_id,
        .replica_id = std::move(replica_id),
        .snapshot_id = "snapshot-1",
        .range = "partition=20260626",
        .batch_hash = std::move(batch_hash),
        .row_count = row_count,
        .vote_hash = "vote-hash",
        .signature = "signature",
    };
}

}

TEST(SafeAuditCoordinator, CreatesPendingAudit)
{
    SafeAuditCoordinator coordinator;

    EXPECT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::Ok);
    EXPECT_TRUE(coordinator.hasAudit("audit-1"));
    EXPECT_EQ(coordinator.auditCount(), 1);

    const auto task = coordinator.getTask("audit-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->replicas, (std::vector<std::string>{"replica-a", "replica-b", "replica-c"}));

    const auto decision = coordinator.getDecision("audit-1");
    ASSERT_TRUE(decision.has_value());
    EXPECT_EQ(decision->status, SafeAuditDecisionStatus::Pending);
    EXPECT_EQ(decision->expected_votes, 3);
    EXPECT_EQ(decision->total_votes, 0);
}

TEST(SafeAuditCoordinator, RejectsInvalidAuditDefinitions)
{
    SafeAuditCoordinator coordinator;

    auto task = makeSafeAuditTask();
    task.audit_id.clear();
    EXPECT_EQ(coordinator.createAudit(task), SafeAuditError::AuditIDRequired);

    task = makeSafeAuditTask({});
    EXPECT_EQ(coordinator.createAudit(task), SafeAuditError::ReplicasRequired);

    task = makeSafeAuditTask({"replica-a", "replica-a"});
    EXPECT_EQ(coordinator.createAudit(task), SafeAuditError::DuplicateReplica);

    EXPECT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::Ok);
    EXPECT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::AuditAlreadyExists);
}

TEST(SafeAuditCoordinator, DecidesMajorityAndTracksMinorityReplicas)
{
    SafeAuditCoordinator coordinator;
    ASSERT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::Ok);

    auto result = coordinator.submitVote(makeSafeAuditVote("replica-a", "hash-majority"));
    ASSERT_TRUE(result.accepted()) << safeAuditErrorName(result.error);
    EXPECT_EQ(result.decision.status, SafeAuditDecisionStatus::Pending);
    EXPECT_EQ(result.decision.total_votes, 1);

    result = coordinator.submitVote(makeSafeAuditVote("replica-b", "hash-majority"));
    ASSERT_TRUE(result.accepted()) << safeAuditErrorName(result.error);
    EXPECT_EQ(result.decision.status, SafeAuditDecisionStatus::Majority);
    EXPECT_EQ(result.decision.majority_hash, "hash-majority");
    EXPECT_EQ(result.decision.majority_count, 2);
    EXPECT_EQ(result.decision.total_votes, 2);
    EXPECT_TRUE(result.decision.minority_replicas.empty());

    result = coordinator.submitVote(makeSafeAuditVote("replica-c", "hash-minority"));
    ASSERT_TRUE(result.accepted()) << safeAuditErrorName(result.error);
    EXPECT_EQ(result.decision.status, SafeAuditDecisionStatus::Majority);
    EXPECT_EQ(result.decision.majority_hash, "hash-majority");
    EXPECT_EQ(result.decision.majority_count, 2);
    EXPECT_EQ(result.decision.total_votes, 3);
    EXPECT_EQ(result.decision.minority_replicas, (std::vector<std::string>{"replica-c"}));

    const auto stored = coordinator.getDecision("audit-1");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->minority_replicas, (std::vector<std::string>{"replica-c"}));
}

TEST(SafeAuditCoordinator, MarksDisputeWhenNoMajorityIsPossible)
{
    SafeAuditCoordinator coordinator;
    ASSERT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::Ok);

    EXPECT_TRUE(coordinator.submitVote(makeSafeAuditVote("replica-a", "hash-a")).accepted());
    EXPECT_TRUE(coordinator.submitVote(makeSafeAuditVote("replica-b", "hash-b")).accepted());
    const auto result = coordinator.submitVote(makeSafeAuditVote("replica-c", "hash-c"));

    ASSERT_TRUE(result.accepted()) << safeAuditErrorName(result.error);
    EXPECT_EQ(result.decision.status, SafeAuditDecisionStatus::Dispute);
    EXPECT_TRUE(result.decision.majority_hash.empty());
    EXPECT_EQ(result.decision.majority_count, 0);
    EXPECT_EQ(result.decision.total_votes, 3);
    EXPECT_TRUE(result.decision.minority_replicas.empty());
}

TEST(SafeAuditCoordinator, RequiresStrictMajorityForEvenReplicaSets)
{
    SafeAuditCoordinator coordinator;
    ASSERT_EQ(coordinator.createAudit(makeSafeAuditTask({"replica-a", "replica-b", "replica-c", "replica-d"})), SafeAuditError::Ok);

    EXPECT_TRUE(coordinator.submitVote(makeSafeAuditVote("replica-a", "hash-a")).accepted());
    EXPECT_TRUE(coordinator.submitVote(makeSafeAuditVote("replica-b", "hash-a")).accepted());
    auto result = coordinator.submitVote(makeSafeAuditVote("replica-c", "hash-b"));
    ASSERT_TRUE(result.accepted()) << safeAuditErrorName(result.error);
    EXPECT_EQ(result.decision.status, SafeAuditDecisionStatus::Pending);
    EXPECT_EQ(result.decision.majority_count, 0);

    result = coordinator.submitVote(makeSafeAuditVote("replica-d", "hash-b"));
    ASSERT_TRUE(result.accepted()) << safeAuditErrorName(result.error);
    EXPECT_EQ(result.decision.status, SafeAuditDecisionStatus::Dispute);
}

TEST(SafeAuditCoordinator, RejectsUnexpectedDuplicateAndMismatchedVotes)
{
    SafeAuditCoordinator coordinator;
    ASSERT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::Ok);

    EXPECT_EQ(coordinator.submitVote(makeSafeAuditVote("replica-x", "hash-a")).error, SafeAuditError::ReplicaNotExpected);

    auto vote = makeSafeAuditVote("replica-a", "hash-a");
    vote.snapshot_id = "snapshot-other";
    EXPECT_EQ(coordinator.submitVote(vote).error, SafeAuditError::SnapshotMismatch);

    vote = makeSafeAuditVote("replica-a", "hash-a");
    vote.range = "partition=other";
    EXPECT_EQ(coordinator.submitVote(vote).error, SafeAuditError::RangeMismatch);

    EXPECT_TRUE(coordinator.submitVote(makeSafeAuditVote("replica-a", "hash-a")).accepted());
    EXPECT_EQ(coordinator.submitVote(makeSafeAuditVote("replica-a", "hash-a")).error, SafeAuditError::DuplicateVote);
}

TEST(SafeAuditCoordinator, RejectsVotesMissingAuthenticationFields)
{
    SafeAuditCoordinator coordinator;
    ASSERT_EQ(coordinator.createAudit(makeSafeAuditTask()), SafeAuditError::Ok);

    auto vote = makeSafeAuditVote("replica-a", "hash-a");
    vote.worker_id.clear();
    EXPECT_EQ(coordinator.submitVote(vote).error, SafeAuditError::WorkerIDRequired);

    vote = makeSafeAuditVote("replica-a", "hash-a");
    vote.vote_hash.clear();
    EXPECT_EQ(coordinator.submitVote(vote).error, SafeAuditError::VoteHashRequired);

    vote = makeSafeAuditVote("replica-a", "hash-a");
    vote.signature.clear();
    EXPECT_EQ(coordinator.submitVote(vote).error, SafeAuditError::SignatureRequired);
}
