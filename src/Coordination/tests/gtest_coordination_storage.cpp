#include "config.h"

#if USE_NURAFT
#include <Coordination/tests/gtest_coordination_common.h>

#include <Coordination/KeeperStorage.h>

#include <Common/ZooKeeper/Types.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>

#include <string_view>

namespace
{

std::string housekeeperTestEncodePath(std::string_view path)
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

std::string housekeeperTestVerifiedTableMarkerPath(std::string_view table_path)
{
    return "/housekeeper/v1/verified_tables/" + housekeeperTestEncodePath(table_path);
}

std::string housekeeperTestSourceClaimsTablePath(std::string_view table_path)
{
    return "/housekeeper/v1/source_claims/" + housekeeperTestEncodePath(table_path);
}

std::string housekeeperTestSourceClaimPath(std::string_view table_path, std::string_view part_name)
{
    return housekeeperTestSourceClaimsTablePath(table_path) + "/" + std::string{part_name};
}

std::string housekeeperTestSafeAuditTaskPath(std::string_view audit_id)
{
    return "/housekeeper/v1/safe_audits/tasks/" + std::string{audit_id};
}

std::string housekeeperTestSafeAuditVotesPath(std::string_view audit_id)
{
    return "/housekeeper/v1/safe_audits/votes/" + std::string{audit_id};
}

std::string housekeeperTestSafeAuditVotePath(std::string_view audit_id, std::string_view replica_id)
{
    return housekeeperTestSafeAuditVotesPath(audit_id) + "/" + std::string{replica_id};
}

std::string housekeeperTestSafeAuditDecisionPath(std::string_view audit_id)
{
    return "/housekeeper/v1/safe_audits/decisions/" + std::string{audit_id};
}

std::string housekeeperTestSafeAuditQuarantinePath(std::string_view audit_id, std::string_view replica_id)
{
    return "/housekeeper/v1/safe_audits/quarantine/" + std::string{audit_id} + "/" + std::string{replica_id};
}


std::string housekeeperTestStorageStatementPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/statements/" + std::string{statement_id};
}

std::string housekeeperTestStorageReplayJobPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/replay_jobs/" + std::string{statement_id};
}

std::string housekeeperTestStorageAttestationsPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/attestations/" + std::string{statement_id};
}

std::string housekeeperTestStorageAttestationPath(std::string_view statement_id, std::string_view worker_id)
{
    return housekeeperTestStorageAttestationsPath(statement_id) + "/" + std::string{worker_id};
}

std::string housekeeperTestStorageUnsafeTaskPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/unsafe_tasks/" + std::string{statement_id};
}

std::string housekeeperTestStorageUnsafeResultPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/unsafe_results/" + std::string{statement_id};
}

std::string housekeeperTestStorageUnsafeResultPath(std::string_view statement_id, std::string_view participant_id)
{
    return housekeeperTestStorageUnsafeResultPath(statement_id) + "/" + std::string{participant_id};
}

std::string housekeeperTestStorageFinalityPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/finality/" + std::string{statement_id};
}

std::string housekeeperTestStorageRollbackPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/rollbacks/" + std::string{statement_id};
}

std::string housekeeperTestStoragePromotionPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/promotions/" + std::string{statement_id};
}

std::string housekeeperTestStorageRollbackTaskPath(std::string_view statement_id)
{
    return "/housekeeper/v1/storage_integrity/rollback_tasks/" + std::string{statement_id};
}

std::string housekeeperTestStorageReplayQuarantinePath(std::string_view worker_id)
{
    return "/housekeeper/v1/storage_integrity/replay_quarantine/" + std::string{worker_id};
}

template <typename Storage>
void housekeeperTestAddPathIfMissing(Storage & storage, const std::string & path, const std::string & data = "")
{
    if (storage.container.find(path) == storage.container.end())
        addNode(storage, path, data);
}

template <typename Storage>
void housekeeperTestAddControlPaths(Storage & storage)
{
    housekeeperTestAddPathIfMissing(storage, "/housekeeper");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/verified_tables");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/source_claims");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/safe_audits");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/safe_audits/tasks");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/safe_audits/votes");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/safe_audits/decisions");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/safe_audits/quarantine");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/statements");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/blocks");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/replay_jobs");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/attestations");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/replay_failures");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/unsafe_tasks");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/unsafe_results");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/unsafe_failures");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/finality");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/rollbacks");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/promotions");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/rollback_tasks");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/replay_quarantine");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/rollback_leases");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/rollback_results");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/rollback_failures");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/promotion_leases");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/promotion_results");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/promotion_failures");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/safe_audit_tasks");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/safe_audit_votes");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1/storage_integrity/decisions");
}

template <typename Storage>
void housekeeperTestAddReplicatedTableLog(Storage & storage, const std::string & table_path)
{
    size_t next_slash = 1;
    while (true)
    {
        next_slash = table_path.find('/', next_slash);
        const std::string current_path = next_slash == std::string::npos ? table_path : table_path.substr(0, next_slash);
        housekeeperTestAddPathIfMissing(storage, current_path);

        if (next_slash == std::string::npos)
            break;

        ++next_slash;
    }
    housekeeperTestAddPathIfMissing(storage, table_path + "/log");
}

std::shared_ptr<Coordination::ZooKeeperCreateRequest> housekeeperTestMakeCreateRequest(
    const std::string & path,
    const std::string & data)
{
    auto request = std::make_shared<Coordination::ZooKeeperCreateRequest>();
    request->path = path;
    request->data = data;
    return request;
}

std::shared_ptr<Coordination::ZooKeeperCreateRequest> housekeeperTestMakeLogCreateRequest(
    const std::string & table_path,
    const std::string & log_name,
    const std::string & data)
{
    auto request = std::make_shared<Coordination::ZooKeeperCreateRequest>();
    request->path = table_path + "/log/" + log_name;
    request->data = data;
    return request;
}

std::string housekeeperTestMergeEntry()
{
    return "format version: 4\nsource replica: source_replica\nmerge\nall_1_1_0\nall_2_2_0\ninto\nall_1_2_1\ndeduplicate: 0\n";
}

std::string housekeeperTestGetPartEntry(std::string_view part_name)
{
    return "format version: 4\nsource replica: source_replica\nget\n" + std::string{part_name} + "\n";
}

std::string housekeeperTestSafeAuditTaskData(std::string_view replicas = "replica-a,replica-b,replica-c")
{
    return "network_id=net-1\n"
        "table_id=db.table\n"
        "schema_hash=schema-1\n"
        "snapshot_id=snapshot-1\n"
        "range=partition=20260626\n"
        "replicas=" + std::string{replicas} + "\n";
}

std::string housekeeperTestSafeAuditVoteData(std::string_view batch_hash, std::string_view snapshot_id = "snapshot-1")
{
    return "worker_id=worker-1\n"
        "snapshot_id=" + std::string{snapshot_id} + "\n"
        "range=partition=20260626\n"
        "batch_hash=" + std::string{batch_hash} + "\n"
        "row_count=10\n"
        "vote_hash=vote-hash\n"
        "signature=signature\n";
}

std::string housekeeperTestStorageStatementData()
{
    return "table_id=dual_hg_auth.t\n"
        "unsafe_table=`hg_unsafe`.`dual_hg_auth.t_a`\n"
        "safe_table=`hg_safe`.`dual_hg_auth.t`\n"
        "payload_ref=mockda://dual_hg_auth.t/stmt/hash\n"
        "payload_hash=payload-hash\n"
        "replay_quorum=2\n"
        "participants=hg-1,hg-2,hg-3\n"
        "partition_ids=202606\n";
}

std::string housekeeperTestStorageAttestationData(std::string_view state_root)
{
    return "computed_state_root=" + std::string{state_root} + "\n"
        "receipt_hash=receipt-" + std::string{state_root} + "\n"
        "match_source_root=true\n"
        "signature=signature\n";
}

std::string housekeeperTestStorageUnsafeResultData(std::string_view participant_id, std::string_view rows_hash = "rows-hash")
{
    return "row_count=1\n"
        "rows_hash=" + std::string{rows_hash} + "\n"
        "replica_digests=" + std::string{participant_id} + ":1:" + std::string{rows_hash} + "\n";
}

std::string housekeeperTestStorageFinalityData()
{
    return "kind=mock\nfinalized=true\n";
}

template <typename Storage>
std::string housekeeperTestNodeData(Storage & storage, const std::string & path)
{
    auto node = storage.container.find(path);
    return node == storage.container.end() ? "" : std::string{node->value.getData()};
}

template <typename Storage>
Coordination::Error housekeeperTestProcessWrite(Storage & storage, const Coordination::ZooKeeperRequestPtr & request, int64_t & zxid)
{
    const auto request_zxid = ++zxid;
    storage.preprocessRequest(request, 1, 0, request_zxid);
    auto responses = storage.processRequest(request, 1, request_zxid);
    EXPECT_EQ(responses.size(), 1);
    return responses.empty() ? Coordination::Error::ZRUNTIMEINCONSISTENCY : responses[0].response->error;
}

}

TYPED_TEST(CoordinationTest, TestSystemNodeModify)
{
    using namespace Coordination;
    int64_t zxid{0};

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    // On INIT we abort when a system path is modified
    this->keeper_context->setServerState(KeeperContext::Phase::RUNNING);
    Storage storage{500, "", this->keeper_context};
    const auto assert_create = [&](const std::string_view path, const auto expected_code)
    {
        auto request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = path;
        storage.preprocessRequest(request, 0, 0, zxid);
        auto responses = storage.processRequest(request, 0, zxid);
        ASSERT_FALSE(responses.empty());

        const auto & response = responses[0];
        ASSERT_EQ(response.response->error, expected_code) << "Unexpected error for path " << path;

        ++zxid;
    };

    assert_create("/keeper", Error::ZBADARGUMENTS);
    assert_create("/keeper/with_child", Error::ZBADARGUMENTS);
    assert_create(DB::keeper_api_version_path, Error::ZBADARGUMENTS);

    assert_create("/keeper_map", Error::ZOK);
    assert_create("/keeper1", Error::ZOK);
    assert_create("/keepe", Error::ZOK);
    assert_create("/keeper1/test", Error::ZOK);
}

TYPED_TEST(CoordinationTest, TestCheckNotExistsRequest)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;

    const auto create_path = [&](const auto & path)
    {
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        int new_zxid = ++zxid;
        create_request->path = path;
        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
    };

    const auto check_request = std::make_shared<ZooKeeperCheckRequest>();
    check_request->path = "/test_node";
    check_request->not_exists = true;

    {
        SCOPED_TRACE("CheckNotExists returns ZOK");
        int new_zxid = ++zxid;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZOK) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }

    create_path("/test_node");
    auto node_it = storage.container.find("/test_node");
    ASSERT_NE(node_it, storage.container.end());
    auto node_version = node_it->value.stats.version;

    {
        SCOPED_TRACE("CheckNotExists returns ZNODEEXISTS");
        int new_zxid = ++zxid;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZNODEEXISTS) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }

    {
        SCOPED_TRACE("CheckNotExists returns ZNODEEXISTS for same version");
        int new_zxid = ++zxid;
        check_request->version = node_version;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZNODEEXISTS) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }

    {
        SCOPED_TRACE("CheckNotExists returns ZOK for different version");
        int new_zxid = ++zxid;
        check_request->version = node_version + 1;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZOK) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }
}

TYPED_TEST(CoordinationTest, TestReapplyingDeltas)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    static constexpr int64_t initial_zxid = 100;

    const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
    create_request->path = "/test/data";
    create_request->is_sequential = true;

    const auto process_create = [](Storage & storage, const auto & request, int64_t zxid)
    {
        storage.preprocessRequest(request, 1, 0, zxid);
        auto responses = storage.processRequest(request, 1, zxid);
        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Error::ZOK);
    };

    const auto commit_initial_data = [&](auto & storage)
    {
        int64_t zxid = 1;

        const auto root_create = std::make_shared<ZooKeeperCreateRequest>();
        root_create->path = "/test";
        process_create(storage, root_create, zxid);
        ++zxid;

        for (; zxid <= initial_zxid; ++zxid)
            process_create(storage, create_request, zxid);
    };

    Storage storage1{500, "", this->keeper_context};
    commit_initial_data(storage1);

    for (int64_t zxid = initial_zxid + 1; zxid < initial_zxid + 50; ++zxid)
        storage1.preprocessRequest(create_request, 1, 0, zxid, /*check_acl=*/true, /*digest=*/std::nullopt, /*log_idx=*/zxid);

    /// create identical new storage
    Storage storage2{500, "", this->keeper_context};
    commit_initial_data(storage2);

    storage1.applyUncommittedState(storage2, initial_zxid);

    const auto commit_unprocessed = [&](Storage & storage)
    {
        for (int64_t zxid = initial_zxid + 1; zxid < initial_zxid + 50; ++zxid)
        {
            auto responses = storage.processRequest(create_request, 1, zxid);
            EXPECT_GE(responses.size(), 1);
            EXPECT_EQ(responses[0].response->error, Error::ZOK);
        }
    };

    commit_unprocessed(storage1);
    commit_unprocessed(storage2);

    const auto get_children = [&](Storage & storage)
    {
        const auto list_request = std::make_shared<ZooKeeperListRequest>();
        list_request->path = "/test";
        auto responses = storage.processRequest(list_request, 1, std::nullopt, /*check_acl=*/true, /*is_local=*/true);
        EXPECT_EQ(responses.size(), 1);
        const auto * list_response = dynamic_cast<const ListResponse *>(responses[0].response.get());
        EXPECT_TRUE(list_response);
        return list_response->names;
    };

    auto children1 = get_children(storage1);
    std::unordered_set<std::string> children1_set(children1.begin(), children1.end());

    auto children2 = get_children(storage2);
    std::unordered_set<std::string> children2_set(children2.begin(), children2.end());

    ASSERT_TRUE(children1_set == children2_set);
}

TYPED_TEST(CoordinationTest, TestRemoveRecursiveRequest)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;

    const auto create = [&](const String & path, int create_mode)
    {
        int new_zxid = ++zxid;

        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        create_request->path = path;
        create_request->is_ephemeral = create_mode == zkutil::CreateMode::Ephemeral || create_mode == zkutil::CreateMode::EphemeralSequential;
        create_request->is_sequential = create_mode == zkutil::CreateMode::PersistentSequential || create_mode == zkutil::CreateMode::EphemeralSequential;

        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
    };

    const auto remove = [&](const String & path, int32_t version = -1)
    {
        int new_zxid = ++zxid;

        auto remove_request = std::make_shared<ZooKeeperRemoveRequest>();
        remove_request->path = path;
        remove_request->version = version;

        storage.preprocessRequest(remove_request, 1, 0, new_zxid);
        return storage.processRequest(remove_request, 1, new_zxid);
    };

    const auto remove_recursive = [&](const String & path, uint32_t remove_nodes_limit = 1)
    {
        int new_zxid = ++zxid;

        auto remove_request = std::make_shared<ZooKeeperRemoveRecursiveRequest>();
        remove_request->path = path;
        remove_request->remove_nodes_limit = remove_nodes_limit;

        storage.preprocessRequest(remove_request, 1, 0, new_zxid);
        return storage.processRequest(remove_request, 1, new_zxid);
    };

    const auto exists = [&](const String & path)
    {
        int new_zxid = ++zxid;

        const auto exists_request = std::make_shared<ZooKeeperExistsRequest>();
        exists_request->path = path;

        storage.preprocessRequest(exists_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(exists_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        return responses[0].response->error == Coordination::Error::ZOK;
    };

    {
        SCOPED_TRACE("Single Remove Single Node");
        create("/T1", zkutil::CreateMode::Persistent);

        auto responses = remove("/T1");
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZOK);
        ASSERT_FALSE(exists("/T1"));
    }

    {
        SCOPED_TRACE("Single Remove Tree");
        create("/T2", zkutil::CreateMode::Persistent);
        create("/T2/A", zkutil::CreateMode::Persistent);

        auto responses = remove("/T2");
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZNOTEMPTY);
        ASSERT_TRUE(exists("/T2"));
    }

    {
        SCOPED_TRACE("Recursive Remove Single Node");
        create("/T3", zkutil::CreateMode::Persistent);

        auto responses = remove_recursive("/T3", 100);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZOK);
        ASSERT_FALSE(exists("/T3"));
    }

    {
        SCOPED_TRACE("Recursive Remove Tree Small Limit");
        create("/T5", zkutil::CreateMode::Persistent);
        create("/T5/A", zkutil::CreateMode::Persistent);
        create("/T5/B", zkutil::CreateMode::Persistent);
        create("/T5/A/C", zkutil::CreateMode::Persistent);

        auto responses = remove_recursive("/T5", 2);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZNOTEMPTY);
        ASSERT_TRUE(exists("/T5"));
        ASSERT_TRUE(exists("/T5/A"));
        ASSERT_TRUE(exists("/T5/B"));
        ASSERT_TRUE(exists("/T5/A/C"));
    }

    {
        SCOPED_TRACE("Recursive Remove Tree Big Limit");
        create("/T6", zkutil::CreateMode::Persistent);
        create("/T6/A", zkutil::CreateMode::Persistent);
        create("/T6/B", zkutil::CreateMode::Persistent);
        create("/T6/A/C", zkutil::CreateMode::Persistent);

        auto responses = remove_recursive("/T6", 4);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZOK);
        ASSERT_FALSE(exists("/T6"));
        ASSERT_FALSE(exists("/T6/A"));
        ASSERT_FALSE(exists("/T6/B"));
        ASSERT_FALSE(exists("/T6/A/C"));
    }

    {
        SCOPED_TRACE("Recursive Remove Ephemeral");
        create("/T7", zkutil::CreateMode::Ephemeral);
        ASSERT_EQ(storage.committed_ephemerals.size(), 1);

        auto responses = remove_recursive("/T7", 100);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZOK);
        ASSERT_EQ(storage.committed_ephemerals.size(), 0);
        ASSERT_FALSE(exists("/T7"));
    }

    {
        SCOPED_TRACE("Recursive Remove Tree With Ephemeral");
        create("/T8", zkutil::CreateMode::Persistent);
        create("/T8/A", zkutil::CreateMode::Persistent);
        create("/T8/B", zkutil::CreateMode::Ephemeral);
        create("/T8/A/C", zkutil::CreateMode::Ephemeral);
        ASSERT_EQ(storage.committed_ephemerals.size(), 1);

        auto responses = remove_recursive("/T8", 4);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Coordination::Error::ZOK);
        ASSERT_EQ(storage.committed_ephemerals.size(), 0);
        ASSERT_FALSE(exists("/T8"));
        ASSERT_FALSE(exists("/T8/A"));
        ASSERT_FALSE(exists("/T8/B"));
        ASSERT_FALSE(exists("/T8/A/C"));
    }
}

namespace
{
Coordination::RequestPtr makeRemoveRecursiveRequest(const std::string & path, uint32_t remove_nodes_limit)
{
    auto request = std::make_shared<Coordination::ZooKeeperRemoveRecursiveRequest>();
    request->path = path;
    request->remove_nodes_limit = remove_nodes_limit;
    return request;
}
}

TYPED_TEST(CoordinationTest, TestRemoveRecursiveInMultiRequest)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int zxid = 0;

    auto prepare_create_tree = []()
    {
        return Coordination::Requests{
            zkutil::makeCreateRequest("/A", "A", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/A/B", "B", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/A/C", "C", zkutil::CreateMode::Ephemeral),
            zkutil::makeCreateRequest("/A/B/D", "D", zkutil::CreateMode::Ephemeral),
        };
    };

    const auto exists = [&](const String & path)
    {
        int new_zxid = ++zxid;

        const auto exists_request = std::make_shared<ZooKeeperExistsRequest>();
        exists_request->path = path;

        storage.preprocessRequest(exists_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(exists_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        return responses[0].response->error == Coordination::Error::ZOK;
    };

    const auto is_multi_ok = [&](Coordination::ZooKeeperResponsePtr response)
    {
        const auto & multi_response = dynamic_cast<Coordination::ZooKeeperMultiResponse &>(*response);

        for (const auto & op_response : multi_response.responses)
            if (op_response->error != Coordination::Error::ZOK)
                return false;

        return true;
    };

    {
        SCOPED_TRACE("Remove In Multi Tx");
        int new_zxid = ++zxid;
        auto ops = prepare_create_tree();

        ops.push_back(zkutil::makeRemoveRequest("/A", -1));
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto responses = storage.processRequest(request, 1, new_zxid);
        ops.pop_back();

        ASSERT_EQ(responses.size(), 1);
        ASSERT_FALSE(is_multi_ok(responses[0].response));
    }

    {
        SCOPED_TRACE("Recursive Remove In Multi Tx");
        int new_zxid = ++zxid;
        auto ops = prepare_create_tree();

        ops.push_back(makeRemoveRecursiveRequest("/A", 4));
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto responses = storage.processRequest(request, 1, new_zxid);
        ops.pop_back();

        ASSERT_EQ(responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(responses[0].response));
        ASSERT_FALSE(exists("/A"));
        ASSERT_FALSE(exists("/A/C"));
        ASSERT_FALSE(exists("/A/B"));
        ASSERT_FALSE(exists("/A/B/D"));
    }

    {
        SCOPED_TRACE("Recursive Remove With Regular In Multi Tx");
        int new_zxid = ++zxid;
        auto ops = prepare_create_tree();

        ops.push_back(zkutil::makeRemoveRequest("/A/C", -1));
        ops.push_back(makeRemoveRecursiveRequest("/A", 3));
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto responses = storage.processRequest(request, 1, new_zxid);
        ops.pop_back();
        ops.pop_back();

        ASSERT_EQ(responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(responses[0].response));
        ASSERT_FALSE(exists("/A"));
        ASSERT_FALSE(exists("/A/C"));
        ASSERT_FALSE(exists("/A/B"));
        ASSERT_FALSE(exists("/A/B/D"));
    }

    {
        SCOPED_TRACE("Recursive Remove From Committed and Uncommitted states");
        int create_zxid = ++zxid;
        auto ops = prepare_create_tree();

        /// First create nodes
        const auto create_request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});
        storage.preprocessRequest(create_request, 1, 0, create_zxid);
        auto create_responses = storage.processRequest(create_request, 1, create_zxid);
        ASSERT_EQ(create_responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(create_responses[0].response));
        ASSERT_TRUE(exists("/A"));
        ASSERT_TRUE(exists("/A/C"));
        ASSERT_TRUE(exists("/A/B"));
        ASSERT_TRUE(exists("/A/B/D"));

        /// Remove node A/C as a single remove request.
        /// Remove all other as remove recursive request.
        /// In this case we should list storage to understand the tree topology
        /// but ignore already deleted nodes in uncommitted state.

        int remove_zxid = ++zxid;
        ops = {
            zkutil::makeRemoveRequest("/A/C", -1),
            makeRemoveRecursiveRequest("/A", 3),
        };
        const auto remove_request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        storage.preprocessRequest(remove_request, 1, 0, remove_zxid);
        auto remove_responses = storage.processRequest(remove_request, 1, remove_zxid);

        ASSERT_EQ(remove_responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(remove_responses[0].response));
        ASSERT_FALSE(exists("/A"));
        ASSERT_FALSE(exists("/A/C"));
        ASSERT_FALSE(exists("/A/B"));
        ASSERT_FALSE(exists("/A/B/D"));
    }

    {
        SCOPED_TRACE("Recursive Remove For Subtree With Updated Node");
        int create_zxid = ++zxid;
        auto ops = prepare_create_tree();

        /// First create nodes
        const auto create_request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});
        storage.preprocessRequest(create_request, 1, 0, create_zxid);
        auto create_responses = storage.processRequest(create_request, 1, create_zxid);
        ASSERT_EQ(create_responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(create_responses[0].response));

        /// Small limit
        int remove_zxid = ++zxid;
        ops = {
            zkutil::makeSetRequest("/A/B", "", -1),
            makeRemoveRecursiveRequest("/A", 3),
        };
        auto remove_request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});
        storage.preprocessRequest(remove_request, 1, 0, remove_zxid);
        auto remove_responses = storage.processRequest(remove_request, 1, remove_zxid);

        ASSERT_EQ(remove_responses.size(), 1);
        ASSERT_FALSE(is_multi_ok(remove_responses[0].response));

        /// Big limit
        remove_zxid = ++zxid;
        ops[1] = makeRemoveRecursiveRequest("/A", 4);
        remove_request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});
        storage.preprocessRequest(remove_request, 1, 0, remove_zxid);
        remove_responses = storage.processRequest(remove_request, 1, remove_zxid);

        ASSERT_EQ(remove_responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(remove_responses[0].response));
        ASSERT_FALSE(exists("/A"));
        ASSERT_FALSE(exists("/A/C"));
        ASSERT_FALSE(exists("/A/B"));
        ASSERT_FALSE(exists("/A/B/D"));
    }

    {
        SCOPED_TRACE("[BUG] Recursive Remove Level Sorting");
        int new_zxid = ++zxid;

        Coordination::Requests ops = {
            zkutil::makeCreateRequest("/a", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/a/bbbbbb", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/A", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/A/B", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/A/CCCCCCCCCCCC", "", zkutil::CreateMode::Persistent),
            makeRemoveRecursiveRequest("/A", 3),
        };
        auto remove_request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});
        storage.preprocessRequest(remove_request, 1, 0, new_zxid);
        auto remove_responses = storage.processRequest(remove_request, 1, new_zxid);

        ASSERT_EQ(remove_responses.size(), 1);
        ASSERT_TRUE(is_multi_ok(remove_responses[0].response));
        ASSERT_TRUE(exists("/a"));
        ASSERT_TRUE(exists("/a/bbbbbb"));
        ASSERT_FALSE(exists("/A"));
        ASSERT_FALSE(exists("/A/B"));
        ASSERT_FALSE(exists("/A/CCCCCCCCCCCC"));
    }

}

TYPED_TEST(CoordinationTest, TestRemoveRecursiveWatches)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int zxid = 0;

    const auto create = [&](const String & path, int create_mode)
    {
        int new_zxid = ++zxid;

        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        create_request->path = path;
        create_request->is_ephemeral = create_mode == zkutil::CreateMode::Ephemeral || create_mode == zkutil::CreateMode::EphemeralSequential;
        create_request->is_sequential = create_mode == zkutil::CreateMode::PersistentSequential || create_mode == zkutil::CreateMode::EphemeralSequential;

        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
    };

    const auto add_watch = [&](const String & path)
    {
        int new_zxid = ++zxid;

        const auto exists_request = std::make_shared<ZooKeeperExistsRequest>();
        exists_request->path = path;
        exists_request->has_watch = true;

        storage.preprocessRequest(exists_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(exists_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK);
    };

    const auto add_list_watch = [&](const String & path)
    {
        int new_zxid = ++zxid;

        const auto list_request = std::make_shared<ZooKeeperListRequest>();
        list_request->path = path;
        list_request->has_watch = true;

        storage.preprocessRequest(list_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(list_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK);
    };

    create("/A", zkutil::CreateMode::Persistent);
    create("/A/B", zkutil::CreateMode::Persistent);
    create("/A/C", zkutil::CreateMode::Ephemeral);
    create("/A/B/D", zkutil::CreateMode::Ephemeral);

    add_watch("/A");
    add_watch("/A/B");
    add_watch("/A/C");
    add_watch("/A/B/D");
    add_list_watch("/A");
    add_list_watch("/A/B");
    ASSERT_EQ(storage.watches.size(), 4);
    ASSERT_EQ(storage.list_watches.size(), 2);

    int new_zxid = ++zxid;

    auto remove_request = std::make_shared<ZooKeeperRemoveRecursiveRequest>();
    remove_request->path = "/A";
    remove_request->remove_nodes_limit = 4;

    storage.preprocessRequest(remove_request, 1, 0, new_zxid);
    auto responses = storage.processRequest(remove_request, 1, new_zxid);

    ASSERT_EQ(responses.size(), 7);
    /// request response is last
    ASSERT_EQ(dynamic_cast<Coordination::ZooKeeperWatchResponse *>(responses.back().response.get()), nullptr);

    std::unordered_map<std::string, std::vector<Coordination::Event>> expected_watch_responses
    {
        {"/A/B/D", {Coordination::Event::DELETED}},
        {"/A/B", {Coordination::Event::CHILD, Coordination::Event::DELETED}},
        {"/A/C", {Coordination::Event::DELETED}},
        {"/A", {Coordination::Event::CHILD, Coordination::Event::DELETED}},
    };

    std::unordered_map<std::string, std::vector<Coordination::Event>> actual_watch_responses;
    for (size_t i = 0; i < 6; ++i)
    {
        ASSERT_EQ(responses[i].response->error, Coordination::Error::ZOK);

        const auto & watch_response = dynamic_cast<Coordination::ZooKeeperWatchResponse &>(*responses[i].response);
        actual_watch_responses[watch_response.path].push_back(static_cast<Coordination::Event>(watch_response.type));
    }
    ASSERT_EQ(expected_watch_responses, actual_watch_responses);

    ASSERT_EQ(storage.watches.size(), 0);
    ASSERT_EQ(storage.list_watches.size(), 0);
}

TYPED_TEST(CoordinationTest, TestRemoveRecursiveAcls)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int zxid = 0;

    {
        int new_zxid = ++zxid;
        String user_auth_data = "test_user:test_password";

        const auto auth_request = std::make_shared<ZooKeeperAuthRequest>();
        auth_request->scheme = "digest";
        auth_request->data = user_auth_data;

        storage.preprocessRequest(auth_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(auth_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to add auth to session";
    }

    const auto create = [&](const String & path)
    {
        int new_zxid = ++zxid;

        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        create_request->path = path;
        create_request->acls = {{.permissions = ACL::Create, .scheme = "auth", .id = ""}};

        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
    };

    /// Add nodes with only Create ACL
    create("/A");
    create("/A/B");
    create("/A/C");
    create("/A/B/D");

    {
        int new_zxid = ++zxid;

        auto remove_request = std::make_shared<ZooKeeperRemoveRecursiveRequest>();
        remove_request->path = "/A";
        remove_request->remove_nodes_limit = 4;

        storage.preprocessRequest(remove_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(remove_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZNOAUTH);
    }
}

TYPED_TEST(CoordinationTest, TestListRequestTypes)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;

    static constexpr std::string_view test_path = "/list_request_type/node";

    const auto create_path = [&](const auto & path, bool is_ephemeral, bool is_sequential = true)
    {
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        int new_zxid = ++zxid;
        create_request->path = path;
        create_request->is_sequential = is_sequential;
        create_request->is_ephemeral = is_ephemeral;
        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
        const auto & create_response = dynamic_cast<ZooKeeperCreateResponse &>(*responses[0].response);
        return create_response.path_created;
    };

    create_path(std::string{parentNodePath(test_path)}, false, false);

    static constexpr size_t persistent_num = 5;
    std::unordered_set<std::string> expected_persistent_children;
    for (size_t i = 0; i < persistent_num; ++i)
    {
        auto created_path = create_path(test_path, false);
        expected_persistent_children.insert(std::string{getBaseNodeName(created_path)});
    }
    ASSERT_EQ(expected_persistent_children.size(), persistent_num);

    static constexpr size_t ephemeral_num = 5;
    std::unordered_set<std::string> expected_ephemeral_children;
    for (size_t i = 0; i < ephemeral_num; ++i)
    {
        auto created_path = create_path(test_path, true);
        expected_ephemeral_children.insert(std::string{getBaseNodeName(created_path)});
    }
    ASSERT_EQ(expected_ephemeral_children.size(), ephemeral_num);

    const auto get_children = [&](const auto list_request_type)
    {
        const auto list_request = std::make_shared<ZooKeeperFilteredListRequest>();
        int new_zxid = ++zxid;
        list_request->path = std::string{parentNodePath(test_path)};
        list_request->list_request_type = list_request_type;
        storage.preprocessRequest(list_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(list_request, 1, new_zxid);

        EXPECT_GE(responses.size(), 1);
        const auto & list_response = dynamic_cast<ZooKeeperListResponse &>(*responses[0].response);
        return list_response.names;
    };

    const auto persistent_children = get_children(ListRequestType::PERSISTENT_ONLY);
    EXPECT_EQ(persistent_children.size(), persistent_num);
    for (const auto & child : persistent_children)
    {
        EXPECT_TRUE(expected_persistent_children.contains(child)) << "Missing persistent child " << child;
    }

    const auto ephemeral_children = get_children(ListRequestType::EPHEMERAL_ONLY);
    EXPECT_EQ(ephemeral_children.size(), ephemeral_num);
    for (const auto & child : ephemeral_children)
    {
        EXPECT_TRUE(expected_ephemeral_children.contains(child)) << "Missing ephemeral child " << child;
    }

    const auto all_children = get_children(ListRequestType::ALL);
    EXPECT_EQ(all_children.size(), ephemeral_num + persistent_num);
    for (const auto & child : all_children)
    {
        EXPECT_TRUE(expected_ephemeral_children.contains(child) || expected_persistent_children.contains(child))
            << "Missing child " << child;
    }
}

TYPED_TEST(CoordinationTest, TestGetChildrenWithStatsAndData)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;

    static constexpr std::string_view test_path = "/list_with_stats_and_data";

    const auto create_path = [&](const auto & path, const std::string & data = "")
    {
        int new_zxid = ++zxid;
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        create_request->path = path;
        create_request->data = data;
        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK);
    };

    const auto modify_path = [&](const auto & path, const auto & data)
    {
        int new_zxid = ++zxid;
        const auto set_request = std::make_shared<ZooKeeperSetRequest>();
        set_request->path = path;
        set_request->data = data;
        storage.preprocessRequest(set_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(set_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK);
    };

    const auto get_children_with_options = [&](const auto & path, bool with_stat, bool with_data)
    {
        int new_zxid = ++zxid;
        const auto list_request = std::make_shared<ZooKeeperFilteredListWithStatsAndDataRequest>();
        list_request->path = path;
        list_request->list_request_type = ListRequestType::ALL;
        list_request->with_stat = with_stat;
        list_request->with_data = with_data;
        storage.preprocessRequest(list_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(list_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        const auto & list_response = dynamic_cast<const ListResponse &>(*responses[0].response);
        return list_response;
    };

    create_path(std::string{test_path});

    {
        SCOPED_TRACE("Empty directory - no stats or data");
        const auto response = get_children_with_options(std::string{test_path}, false, false);
        EXPECT_EQ(response.error, Error::ZOK);
        EXPECT_EQ(response.names.size(), 0);
        EXPECT_EQ(response.stats.size(), 0);
        EXPECT_EQ(response.data.size(), 0);
    }

    // Create children
    create_path(std::string{test_path} + "/child1", "data1");
    create_path(std::string{test_path} + "/child2", "data2");
    create_path(std::string{test_path} + "/child3", "data3");

    {
        SCOPED_TRACE("Directory with children - only names");
        const auto response = get_children_with_options(std::string{test_path}, false, false);
        EXPECT_EQ(response.error, Error::ZOK);
        EXPECT_EQ(response.names.size(), 3);
        EXPECT_EQ(response.stats.size(), 0);
        EXPECT_EQ(response.data.size(), 0);

        std::unordered_set<std::string> children_set(response.names.begin(), response.names.end());
        EXPECT_TRUE(children_set.contains("child1"));
        EXPECT_TRUE(children_set.contains("child2"));
        EXPECT_TRUE(children_set.contains("child3"));
    }

    {
        SCOPED_TRACE("Directory with children - with stats only");
        const auto response = get_children_with_options(std::string{test_path}, true, false);
        EXPECT_EQ(response.error, Error::ZOK);
        EXPECT_EQ(response.names.size(), 3);
        EXPECT_EQ(response.stats.size(), 3);
        EXPECT_EQ(response.data.size(), 0);

        std::unordered_map<std::string, Stat> children_map;
        for (size_t i = 0; i < response.names.size(); ++i)
        {
            children_map[response.names[i]] = response.stats[i];
            EXPECT_EQ(response.stats[i].version, 0);  // Initial version
            EXPECT_GT(response.stats[i].mzxid, 0);     // mzxid should be set
            EXPECT_EQ(response.stats[i].dataLength, 5); // "data1", "data2", "data3" are all 5 chars
        }

        EXPECT_TRUE(children_map.contains("child1"));
        EXPECT_TRUE(children_map.contains("child2"));
        EXPECT_TRUE(children_map.contains("child3"));
    }

    {
        SCOPED_TRACE("Directory with children - with data only");
        const auto response = get_children_with_options(std::string{test_path}, false, true);
        EXPECT_EQ(response.error, Error::ZOK);
        EXPECT_EQ(response.names.size(), 3);
        EXPECT_EQ(response.stats.size(), 0);
        EXPECT_EQ(response.data.size(), 3);

        std::unordered_map<std::string, std::string> children_map;
        for (size_t i = 0; i < response.names.size(); ++i)
        {
            children_map[response.names[i]] = response.data[i];
        }

        EXPECT_EQ(children_map["child1"], "data1");
        EXPECT_EQ(children_map["child2"], "data2");
        EXPECT_EQ(children_map["child3"], "data3");
    }

    {
        SCOPED_TRACE("Directory with children - with both stats and data");
        const auto response = get_children_with_options(std::string{test_path}, true, true);
        EXPECT_EQ(response.error, Error::ZOK);
        EXPECT_EQ(response.names.size(), 3);
        EXPECT_EQ(response.stats.size(), 3);
        EXPECT_EQ(response.data.size(), 3);

        for (size_t i = 0; i < response.names.size(); ++i)
        {
            EXPECT_EQ(response.stats[i].version, 0);
            EXPECT_GT(response.stats[i].mzxid, 0);
            // Data should correspond to the child name
            if (response.names[i] == "child1")
                EXPECT_EQ(response.data[i], "data1");
            else if (response.names[i] == "child2")
                EXPECT_EQ(response.data[i], "data2");
            else if (response.names[i] == "child3")
                EXPECT_EQ(response.data[i], "data3");
        }
    }

    // Modify child2 to change its version and mzxid
    const auto response_before = get_children_with_options(std::string{test_path}, true, true);
    std::unordered_map<std::string, Stat> stats_before;
    for (size_t i = 0; i < response_before.names.size(); ++i)
    {
        stats_before[response_before.names[i]] = response_before.stats[i];
    }

    modify_path(std::string{test_path} + "/child2", "modified_data");

    {
        SCOPED_TRACE("Check version and mzxid after modification");
        const auto response = get_children_with_options(std::string{test_path}, true, true);
        EXPECT_EQ(response.error, Error::ZOK);
        EXPECT_EQ(response.names.size(), 3);
        EXPECT_EQ(response.stats.size(), 3);
        EXPECT_EQ(response.data.size(), 3);

        for (size_t i = 0; i < response.names.size(); ++i)
        {
            if (response.names[i] == "child2")
            {
                // Modified child should have incremented version and mzxid
                EXPECT_EQ(response.stats[i].version, stats_before["child2"].version + 1);
                EXPECT_GT(response.stats[i].mzxid, stats_before["child2"].mzxid);
                EXPECT_EQ(response.data[i], "modified_data");
                EXPECT_EQ(response.stats[i].dataLength, 13); // "modified_data" length
            }
            else
            {
                // Other children should remain unchanged
                const auto & name = response.names[i];
                EXPECT_EQ(response.stats[i].version, stats_before[name].version);
                EXPECT_EQ(response.stats[i].mzxid, stats_before[name].mzxid);
            }
        }
    }

    {
        SCOPED_TRACE("Non-existent path");
        int new_zxid = ++zxid;
        const auto list_request = std::make_shared<ZooKeeperFilteredListWithStatsAndDataRequest>();
        list_request->path = "/nonexistent";
        list_request->list_request_type = ListRequestType::ALL;
        list_request->with_stat = true;
        list_request->with_data = false;
        storage.preprocessRequest(list_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(list_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        const auto & list_response = dynamic_cast<const ListResponse &>(*responses[0].response);
        EXPECT_EQ(list_response.error, Error::ZNONODE);
    }
}

TYPED_TEST(CoordinationTest, TestUncommittedStateBasicCrud)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    constexpr std::string_view path = "/test";

    const auto get_committed_data = [&]() -> std::optional<String>
    {
        auto request = std::make_shared<ZooKeeperGetRequest>();
        request->path = path;
        auto responses = storage.processRequest(request, 0, std::nullopt, true, true);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);

        if (get_response.error != Error::ZOK)
            return std::nullopt;

        return get_response.data;
    };

    const auto preprocess_get = [&](int64_t zxid)
    {
        auto get_request = std::make_shared<ZooKeeperGetRequest>();
        get_request->path = path;
        storage.preprocessRequest(get_request, 0, 0, zxid);
        return get_request;
    };

    const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
    create_request->path = path;
    create_request->data = "initial_data";
    storage.preprocessRequest(create_request, 0, 0, 1);
    storage.preprocessRequest(create_request, 0, 0, 2);

    ASSERT_EQ(get_committed_data(), std::nullopt);

    const auto after_create_get = preprocess_get(3);

    ASSERT_EQ(get_committed_data(), std::nullopt);

    const auto set_request = std::make_shared<ZooKeeperSetRequest>();
    set_request->path = path;
    set_request->data = "new_data";
    storage.preprocessRequest(set_request, 0, 0, 4);

    const auto after_set_get = preprocess_get(5);

    ASSERT_EQ(get_committed_data(), std::nullopt);

    const auto remove_request = std::make_shared<ZooKeeperRemoveRequest>();
    remove_request->path = path;
    storage.preprocessRequest(remove_request, 0, 0, 6);
    storage.preprocessRequest(remove_request, 0, 0, 7);

    const auto after_remove_get = preprocess_get(8);

    ASSERT_EQ(get_committed_data(), std::nullopt);

    {
        const auto responses = storage.processRequest(create_request, 0, 1);
        const auto & create_response = getSingleResponse<ZooKeeperCreateResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZOK);
    }

    {
        const auto responses = storage.processRequest(create_request, 0, 2);
        const auto & create_response = getSingleResponse<ZooKeeperCreateResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZNODEEXISTS);
    }

    {
        const auto responses = storage.processRequest(after_create_get, 0, 3);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
        ASSERT_EQ(get_response.error, Error::ZOK);
        ASSERT_EQ(get_response.data, "initial_data");
    }

    ASSERT_EQ(get_committed_data(), "initial_data");

    {
        const auto responses = storage.processRequest(set_request, 0, 4);
        const auto & create_response = getSingleResponse<ZooKeeperSetResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZOK);
    }

    {
        const auto responses = storage.processRequest(after_set_get, 0, 5);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
        ASSERT_EQ(get_response.error, Error::ZOK);
        ASSERT_EQ(get_response.data, "new_data");
    }

    ASSERT_EQ(get_committed_data(), "new_data");

    {
        const auto responses = storage.processRequest(remove_request, 0, 6);
        const auto & create_response = getSingleResponse<ZooKeeperRemoveResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZOK);
    }

    {
        const auto responses = storage.processRequest(remove_request, 0, 7);
        const auto & create_response = getSingleResponse<ZooKeeperRemoveResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZNONODE);
    }

    {
        const auto responses = storage.processRequest(after_remove_get, 0, 8);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
        ASSERT_EQ(get_response.error, Error::ZNONODE);
    }

    ASSERT_EQ(get_committed_data(), std::nullopt);
}

TYPED_TEST(CoordinationTest, TestBlockACL)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int64_t zxid = 1;

    static constexpr std::string_view digest = "clickhouse:test";
    static constexpr std::string_view new_digest = "antonio:test";

    static constexpr int64_t session_id = 42;
    storage.committed_session_and_auth[session_id].push_back(KeeperStorageBase::AuthID{.scheme = "digest", .id = std::string{digest}});
    {
        static constexpr std::string_view path = "/test";

        auto req_zxid = zxid++;
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        create_request->path = path;
        create_request->acls = {Coordination::ACL{.permissions = Coordination::ACL::All, .scheme = "digest", .id = std::string{digest}}};
        storage.preprocessRequest(create_request, session_id, 0, req_zxid);
        auto acls = storage.uncommitted_state.getACLs(path);
        ASSERT_EQ(acls.size(), 1);
        ASSERT_EQ(acls[0].id, digest);
        storage.processRequest(create_request, session_id, req_zxid);
        ASSERT_NE(storage.container.getValue(path).acl_id, 0);

        req_zxid = zxid++;
        const auto set_acl_request = std::make_shared<ZooKeeperSetACLRequest>();
        set_acl_request->path = path;
        set_acl_request->acls = {Coordination::ACL{.permissions = Coordination::ACL::All, .scheme = "digest", .id = std::string{new_digest}}};
        storage.preprocessRequest(set_acl_request, session_id, 0, req_zxid);
        acls = storage.uncommitted_state.getACLs(path);
        ASSERT_EQ(acls.size(), 1);
        ASSERT_EQ(acls[0].id, new_digest);
        storage.processRequest(set_acl_request, session_id, req_zxid);
        ASSERT_NE(storage.container.getValue(path).acl_id, 0);
    }

    {
        static constexpr std::string_view path = "/test_blocked_acl";
        this->keeper_context->setBlockACL(true);

        auto req_zxid = zxid++;
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        create_request->path = path;
        create_request->acls = {Coordination::ACL{.permissions = Coordination::ACL::All, .scheme = "digest", .id = std::string{digest}}};
        storage.preprocessRequest(create_request, session_id, 0, req_zxid);
        auto acls = storage.uncommitted_state.getACLs(path);
        ASSERT_EQ(acls.size(), 0);
        storage.processRequest(create_request, session_id, req_zxid);
        ASSERT_EQ(storage.container.getValue(path).acl_id, 0);

        req_zxid = zxid++;
        const auto set_acl_request = std::make_shared<ZooKeeperSetACLRequest>();
        set_acl_request->path = path;
        set_acl_request->acls = {Coordination::ACL{.permissions = Coordination::ACL::All, .scheme = "digest", .id = std::string{new_digest}}};
        storage.preprocessRequest(set_acl_request, session_id, 0, req_zxid);
        acls = storage.uncommitted_state.getACLs(path);
        ASSERT_EQ(acls.size(), 0);
        storage.processRequest(set_acl_request, session_id, req_zxid);
        ASSERT_EQ(storage.container.getValue(path).acl_id, 0);
    }
}

TYPED_TEST(CoordinationTest, TestMultiWatches)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;
    auto wait_event = std::make_shared<Poco::Event>();
    auto subscription = std::make_shared<Coordination::WatchCallback>([wait_event](const Coordination::WatchResponse &) { wait_event->set(); });

    /// Create nodes before tests
    {
        const Coordination::Requests ops{
            zkutil::makeCreateRequest("/A1", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/B1", "", zkutil::CreateMode::Persistent),
        };
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        int new_zxid = ++zxid;
        storage.preprocessRequest(request, 1, 0, new_zxid);
        storage.processRequest(request, 1, new_zxid);
    }

    {
        SCOPED_TRACE("Multi With Single Regular Watch");

        const Coordination::Requests ops{
            zkutil::makeGetRequest("/A1", subscription),
            zkutil::makeListRequest("/B1"),
        };
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        int new_zxid = ++zxid;
        storage.preprocessRequest(request, 1, 0, new_zxid);
        storage.processRequest(request, 1, new_zxid);

        ASSERT_EQ(storage.watches.size(), 1);
        ASSERT_EQ(storage.list_watches.size(), 0);
    }

    {
        SCOPED_TRACE("Multi With Single List Watch");

        const Coordination::Requests ops{
            zkutil::makeGetRequest("/A1"),
            zkutil::makeListRequest("/B1", Coordination::ListRequestType::ALL, subscription),
        };
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        int new_zxid = ++zxid;
        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto remove_responses = storage.processRequest(request, 1, new_zxid);

        ASSERT_EQ(storage.watches.size(), 1);
        ASSERT_EQ(storage.list_watches.size(), 1);
    }

    {
        SCOPED_TRACE("Multi Watches Deduplication");

        const Coordination::Requests ops{
            zkutil::makeGetRequest("/A1", subscription),
            zkutil::makeListRequest("/B1", Coordination::ListRequestType::ALL, subscription),
        };
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        int new_zxid = ++zxid;
        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto remove_responses = storage.processRequest(request, 1, new_zxid);

        ASSERT_EQ(storage.watches.size(), 1);
        ASSERT_EQ(storage.list_watches.size(), 1);
    }

    {
        SCOPED_TRACE("Multi Watches Partial Deduplication");

        const Coordination::Requests ops{
            zkutil::makeGetRequest("/A1", subscription),
            zkutil::makeListRequest("/B1", Coordination::ListRequestType::ALL, subscription),
            zkutil::makeSimpleListRequest("/A1", subscription),
            zkutil::makeExistsRequest("/C1", subscription),
        };
        const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

        int new_zxid = ++zxid;
        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto remove_responses = storage.processRequest(request, 1, new_zxid);

        ASSERT_EQ(storage.watches.size(), 2);
        ASSERT_EQ(storage.list_watches.size(), 2);
    }
}

TYPED_TEST(CoordinationTest, TestCheckStat)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;
    auto wait_event = std::make_shared<Poco::Event>();
    auto subscription = std::make_shared<Coordination::WatchCallback>([wait_event](const Coordination::WatchResponse &) { wait_event->set(); });

    Coordination::Stat node_stat;

    /// Create nodes before tests
    {
        const Coordination::Requests create_ops{
            zkutil::makeCreateRequest("/A1", "", zkutil::CreateMode::Persistent),
            zkutil::makeSetRequest("/A1", "new-data!", /*version=*/0),
        };
        const auto create_request = std::make_shared<ZooKeeperMultiRequest>(create_ops, ACLs{});

        int create_zxid = ++zxid;
        storage.preprocessRequest(create_request, 1, 0, create_zxid);
        auto create_responses = storage.processRequest(create_request, 1, create_zxid);
        ASSERT_EQ(create_responses.size(), 1);
        ASSERT_EQ(create_responses[0].response->error, Error::ZOK);

        const auto get_request = std::dynamic_pointer_cast<ZooKeeperRequest>(zkutil::makeGetRequest("/A1"));

        int get_zxid = ++zxid;
        storage.preprocessRequest(get_request, 1, 0, get_zxid);
        auto get_responses = storage.processRequest(get_request, 1, get_zxid);
        ASSERT_EQ(get_responses.size(), 1);
        ASSERT_EQ(get_responses[0].response->error, Error::ZOK);
        node_stat = dynamic_cast<const GetResponse *>(get_responses[0].response.get())->stat;
    }

    [[maybe_unused]] const auto run_check_request = [&](OpNum op, const std::string & path, int version, bool not_exists, std::optional<Coordination::Stat> stat_to_check)
    {
        const auto request = std::dynamic_pointer_cast<ZooKeeperRequest>(zkutil::makeCheckRequest(path, version, not_exists, stat_to_check));
        EXPECT_EQ(request->getOpNum(), op);

        int new_zxid = ++zxid;
        storage.preprocessRequest(request, 1, 0, new_zxid);
        auto responses = storage.processRequest(request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        return std::dynamic_pointer_cast<ZooKeeperCheckResponse>(responses[0].response);
    };

    {
        SCOPED_TRACE("Check without extensions");
        auto response = run_check_request(OpNum::Check, "/A1", /*version=*/1, /*not_exists=*/false, /*stat_to_check=*/std::nullopt);
        ASSERT_EQ(response->error, Error::ZOK);
    }

    {
        SCOPED_TRACE("Check with only not_exists");
        auto response = run_check_request(OpNum::CheckNotExists, "/A1", /*version=*/1, /*not_exists=*/true, /*stat_to_check=*/std::nullopt);
        ASSERT_EQ(response->error, Error::ZNODEEXISTS);
    }

    {
        SCOPED_TRACE("Check with only stat");
        Stat stat = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        ASSERT_EQ(run_check_request(OpNum::CheckStat, "/A1", /*version=*/1, /*not_exists=*/false, /*stat_to_check=*/stat)->error, Error::ZOK);

        stat.version = 1;
        ASSERT_EQ(run_check_request(OpNum::CheckStat, "/A1", /*version=*/1, /*not_exists=*/false, /*stat_to_check=*/stat)->error, Error::ZOK);

        stat.mzxid = node_stat.mzxid;
        ASSERT_EQ(run_check_request(OpNum::CheckStat, "/A1", /*version=*/1, /*not_exists=*/false, /*stat_to_check=*/stat)->error, Error::ZOK);

        stat.mzxid = node_stat.mzxid + 1;
        ASSERT_EQ(run_check_request(OpNum::CheckStat, "/A1", /*version=*/1, /*not_exists=*/false, /*stat_to_check=*/stat)->error, Error::ZBADVERSION);

        stat.mzxid = node_stat.mzxid;
        stat.version = 2;
        ASSERT_EQ(run_check_request(OpNum::CheckStat, "/A1", /*version=*/1, /*not_exists=*/false, /*stat_to_check=*/stat)->error, Error::ZBADVERSION);
    }
}

TYPED_TEST(CoordinationTest, TestTryRemove)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};

    int32_t zxid = 0;

    const auto exists = [&](const String & path)
    {
        int new_zxid = ++zxid;

        const auto exists_request = std::make_shared<ZooKeeperExistsRequest>();
        exists_request->path = path;

        storage.preprocessRequest(exists_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(exists_request, 1, new_zxid);

        EXPECT_EQ(responses.size(), 1);
        return responses[0].response->error == Coordination::Error::ZOK;
    };

    {
        SCOPED_TRACE("Remove without extension");

        const Coordination::Requests create_ops{
            zkutil::makeCreateRequest("/s1", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/s1/A", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/s1/A/B", "", zkutil::CreateMode::Persistent),
        };
        const auto create_request = std::make_shared<ZooKeeperMultiRequest>(create_ops, ACLs{});
        int create_zxid = ++zxid;
        storage.preprocessRequest(create_request, 1, 0, create_zxid);
        storage.processRequest(create_request, 1, create_zxid);

        ASSERT_TRUE(exists("/s1/A"));
        ASSERT_TRUE(exists("/s1/A/B"));

        const Coordination::Requests remove_ops{
            zkutil::makeRemoveRequest("/s1/A", -1),
            zkutil::makeRemoveRequest("/s1/A/B", -1),
            zkutil::makeRemoveRequest("/s1/A", -1),
        };
        const auto remove_request = std::make_shared<ZooKeeperMultiRequest>(remove_ops, ACLs{});
        int remove_zxid = ++zxid;
        storage.preprocessRequest(remove_request, 1, 0, remove_zxid);
        auto responses = storage.processRequest(remove_request, 1, remove_zxid);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Error::ZOK);

        auto multi_response = std::dynamic_pointer_cast<ZooKeeperMultiResponse>(responses[0].response);
        ASSERT_EQ(multi_response->responses.size(), 3);
        ASSERT_EQ(multi_response->responses[0]->error, Error::ZNOTEMPTY);
        ASSERT_EQ(multi_response->responses[1]->error, Error::ZRUNTIMEINCONSISTENCY);
        ASSERT_EQ(multi_response->responses[2]->error, Error::ZRUNTIMEINCONSISTENCY);

        ASSERT_TRUE(exists("/s1/A"));
        ASSERT_TRUE(exists("/s1/A/B"));
    }

    {
        SCOPED_TRACE("Remove with extension");

        const Coordination::Requests create_ops{
            zkutil::makeCreateRequest("/s2", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/s2/A", "", zkutil::CreateMode::Persistent),
            zkutil::makeCreateRequest("/s2/A/B", "", zkutil::CreateMode::Persistent),
        };
        const auto create_request = std::make_shared<ZooKeeperMultiRequest>(create_ops, ACLs{});
        int create_zxid = ++zxid;
        storage.preprocessRequest(create_request, 1, 0, create_zxid);
        storage.processRequest(create_request, 1, create_zxid);

        ASSERT_TRUE(exists("/s2/A"));
        ASSERT_TRUE(exists("/s2/A/B"));

        const Coordination::Requests remove_ops{
            zkutil::makeRemoveRequest("/s2/A", -1, /*try_remove=*/true),
            zkutil::makeRemoveRequest("/s2/A/B", -1),
            zkutil::makeRemoveRequest("/s2/A", -1),
        };
        const auto remove_request = std::make_shared<ZooKeeperMultiRequest>(remove_ops, ACLs{});
        int remove_zxid = ++zxid;
        storage.preprocessRequest(remove_request, 1, 0, remove_zxid);
        auto responses = storage.processRequest(remove_request, 1, remove_zxid);
        ASSERT_EQ(responses.size(), 1);
        ASSERT_EQ(responses[0].response->error, Error::ZOK);

        auto multi_response = std::dynamic_pointer_cast<ZooKeeperMultiResponse>(responses[0].response);
        ASSERT_EQ(multi_response->responses.size(), 3);
        ASSERT_EQ(multi_response->responses[0]->error, Error::ZOK);
        ASSERT_EQ(multi_response->responses[1]->error, Error::ZOK);
        ASSERT_EQ(multi_response->responses[2]->error, Error::ZOK);

        ASSERT_FALSE(exists("/s2/A"));
        ASSERT_FALSE(exists("/s2/A/B"));
    }
}

TYPED_TEST(CoordinationTest, TestHouseKeeperAllowsUnverifiedRMTLogCreate)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;

    const std::string table_path = "/clickhouse/tables/unverified";
    housekeeperTestAddReplicatedTableLog(storage, table_path);

    const auto request = housekeeperTestMakeLogCreateRequest(table_path, "log-0000000000", housekeeperTestMergeEntry());
    EXPECT_EQ(housekeeperTestProcessWrite(storage, request, zxid), Error::ZOK);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperRejectsUnsafeMergeForVerifiedTable)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;

    const std::string table_path = "/clickhouse/tables/verified_merge";
    housekeeperTestAddReplicatedTableLog(storage, table_path);
    housekeeperTestAddControlPaths(storage);
    addNode(storage, housekeeperTestVerifiedTableMarkerPath(table_path), table_path);

    const auto request = housekeeperTestMakeLogCreateRequest(table_path, "log-0000000000", housekeeperTestMergeEntry());
    EXPECT_EQ(housekeeperTestProcessWrite(storage, request, zxid), Error::ZBADARGUMENTS);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperRequiresSourceClaimForVerifiedGetPart)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;

    const std::string table_path = "/clickhouse/tables/verified_get_without_claim";
    const std::string part_name = "all_1_1_0";
    housekeeperTestAddReplicatedTableLog(storage, table_path);
    housekeeperTestAddControlPaths(storage);
    addNode(storage, housekeeperTestVerifiedTableMarkerPath(table_path), table_path);

    const auto request = housekeeperTestMakeLogCreateRequest(table_path, "log-0000000000", housekeeperTestGetPartEntry(part_name));
    EXPECT_EQ(housekeeperTestProcessWrite(storage, request, zxid), Error::ZBADARGUMENTS);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperAllowsClaimedVerifiedGetPart)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;

    const std::string table_path = "/clickhouse/tables/verified_get_with_claim";
    const std::string part_name = "all_1_1_0";
    housekeeperTestAddReplicatedTableLog(storage, table_path);
    housekeeperTestAddControlPaths(storage);
    addNode(storage, housekeeperTestVerifiedTableMarkerPath(table_path), table_path);
    addNode(storage, housekeeperTestSourceClaimsTablePath(table_path), "");
    addNode(storage, housekeeperTestSourceClaimPath(table_path, part_name), "payload_hash=test-hash");

    const auto request = housekeeperTestMakeLogCreateRequest(table_path, "log-0000000000", housekeeperTestGetPartEntry(part_name));
    EXPECT_EQ(housekeeperTestProcessWrite(storage, request, zxid), Error::ZOK);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperRejectsUnsafeMergeInMulti)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;

    const std::string table_path = "/clickhouse/tables/verified_multi_merge";
    housekeeperTestAddReplicatedTableLog(storage, table_path);
    housekeeperTestAddControlPaths(storage);
    addNode(storage, housekeeperTestVerifiedTableMarkerPath(table_path), table_path);

    const Coordination::Requests ops{
        housekeeperTestMakeLogCreateRequest(table_path, "log-0000000000", housekeeperTestMergeEntry()),
    };
    const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

    const auto request_zxid = ++zxid;
    storage.preprocessRequest(request, 1, 0, request_zxid);
    auto responses = storage.processRequest(request, 1, request_zxid);
    ASSERT_EQ(responses.size(), 1);

    const auto multi_response = std::dynamic_pointer_cast<ZooKeeperMultiResponse>(responses[0].response);
    ASSERT_TRUE(multi_response);
    ASSERT_EQ(multi_response->responses.size(), 1);
    EXPECT_EQ(multi_response->responses[0]->error, Error::ZBADARGUMENTS);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperSafeAuditCreateTaskPersistsLedger)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string audit_id = "audit-ledger";
    const auto request = housekeeperTestMakeCreateRequest(
        housekeeperTestSafeAuditTaskPath(audit_id),
        housekeeperTestSafeAuditTaskData());

    EXPECT_EQ(housekeeperTestProcessWrite(storage, request, zxid), Error::ZOK);
    EXPECT_NE(storage.container.find(housekeeperTestSafeAuditVotesPath(audit_id)), storage.container.end());
    EXPECT_NE(storage.container.find("/housekeeper/v1/safe_audits/quarantine/" + audit_id), storage.container.end());

    const auto decision_data = housekeeperTestNodeData(storage, housekeeperTestSafeAuditDecisionPath(audit_id));
    EXPECT_NE(decision_data.find("status=pending\n"), std::string::npos);
    EXPECT_NE(decision_data.find("expected_votes=3\n"), std::string::npos);
    EXPECT_NE(decision_data.find("total_votes=0\n"), std::string::npos);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperSafeAuditVotesUpdateDecisionAndQuarantineMinority)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string audit_id = "audit-majority";
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestSafeAuditTaskPath(audit_id), housekeeperTestSafeAuditTaskData()),
            zxid),
        Error::ZOK);

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestSafeAuditVotePath(audit_id, "replica-a"), housekeeperTestSafeAuditVoteData("hash-majority")),
            zxid),
        Error::ZOK);
    EXPECT_NE(housekeeperTestNodeData(storage, housekeeperTestSafeAuditDecisionPath(audit_id)).find("status=pending\n"), std::string::npos);

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestSafeAuditVotePath(audit_id, "replica-b"), housekeeperTestSafeAuditVoteData("hash-majority")),
            zxid),
        Error::ZOK);
    auto decision_data = housekeeperTestNodeData(storage, housekeeperTestSafeAuditDecisionPath(audit_id));
    EXPECT_NE(decision_data.find("status=majority\n"), std::string::npos);
    EXPECT_NE(decision_data.find("majority_hash=hash-majority\n"), std::string::npos);
    EXPECT_NE(decision_data.find("majority_count=2\n"), std::string::npos);
    EXPECT_EQ(storage.container.find(housekeeperTestSafeAuditQuarantinePath(audit_id, "replica-c")), storage.container.end());

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestSafeAuditVotePath(audit_id, "replica-c"), housekeeperTestSafeAuditVoteData("hash-minority")),
            zxid),
        Error::ZOK);
    decision_data = housekeeperTestNodeData(storage, housekeeperTestSafeAuditDecisionPath(audit_id));
    EXPECT_NE(decision_data.find("status=majority\n"), std::string::npos);
    EXPECT_NE(decision_data.find("minority_replicas=replica-c\n"), std::string::npos);

    const auto quarantine_data = housekeeperTestNodeData(storage, housekeeperTestSafeAuditQuarantinePath(audit_id, "replica-c"));
    EXPECT_NE(quarantine_data.find("reason=safe_audit_minority\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("majority_hash=hash-majority\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("replica_id=replica-c\n"), std::string::npos);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperSafeAuditRejectsMismatchedVote)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string audit_id = "audit-mismatch";
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestSafeAuditTaskPath(audit_id), housekeeperTestSafeAuditTaskData()),
            zxid),
        Error::ZOK);

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(
                housekeeperTestSafeAuditVotePath(audit_id, "replica-a"),
                housekeeperTestSafeAuditVoteData("hash-a", "snapshot-other")),
            zxid),
        Error::ZBADARGUMENTS);
    EXPECT_EQ(storage.container.find(housekeeperTestSafeAuditVotePath(audit_id, "replica-a")), storage.container.end());
}

TYPED_TEST(CoordinationTest, TestHouseKeeperSafeAuditRejectsRPCInMulti)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const Coordination::Requests ops{
        housekeeperTestMakeCreateRequest(housekeeperTestSafeAuditTaskPath("audit-in-multi"), housekeeperTestSafeAuditTaskData()),
    };
    const auto request = std::make_shared<ZooKeeperMultiRequest>(ops, ACLs{});

    const auto request_zxid = ++zxid;
    storage.preprocessRequest(request, 1, 0, request_zxid);
    auto responses = storage.processRequest(request, 1, request_zxid);
    ASSERT_EQ(responses.size(), 1);

    const auto multi_response = std::dynamic_pointer_cast<ZooKeeperMultiResponse>(responses[0].response);
    ASSERT_TRUE(multi_response);
    ASSERT_EQ(multi_response->responses.size(), 1);
    EXPECT_EQ(multi_response->responses[0]->error, Error::ZBADARGUMENTS);
}


TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityStatementCreatesTaskLedger)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string statement_id = "stmt-ledger";
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestStorageStatementPath(statement_id), housekeeperTestStorageStatementData()),
            zxid),
        Error::ZOK);

    EXPECT_NE(storage.container.find(housekeeperTestStorageReplayJobPath(statement_id)), storage.container.end());
    EXPECT_NE(storage.container.find(housekeeperTestStorageUnsafeTaskPath(statement_id)), storage.container.end());
    EXPECT_NE(storage.container.find(housekeeperTestStorageUnsafeResultPath(statement_id)), storage.container.end());
    EXPECT_NE(storage.container.find(housekeeperTestStorageAttestationsPath(statement_id)), storage.container.end());

    const auto replay_job = housekeeperTestNodeData(storage, housekeeperTestStorageReplayJobPath(statement_id));
    EXPECT_NE(replay_job.find("statement_id=stmt-ledger\n"), std::string::npos);
    EXPECT_NE(replay_job.find("payload_hash=payload-hash\n"), std::string::npos);

    const auto unsafe_task = housekeeperTestNodeData(storage, housekeeperTestStorageUnsafeTaskPath(statement_id));
    EXPECT_NE(unsafe_task.find("participants=hg-1,hg-2,hg-3\n"), std::string::npos);
    EXPECT_EQ(unsafe_task.find("replicas="), std::string::npos);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityRejectsStatementWithoutPayloadRef)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string statement_id = "stmt-no-payload-ref";
    const std::string statement_data = "table_id=dual_hg_auth.t\n"
        "unsafe_table=`hg_unsafe`.`dual_hg_auth.t_a`\n"
        "safe_table=`hg_safe`.`dual_hg_auth.t`\n"
        "payload_hash=payload-hash\n"
        "replay_quorum=2\n"
        "participants=hg-1,hg-2,hg-3\n";

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestStorageStatementPath(statement_id), statement_data),
            zxid),
        Error::ZBADARGUMENTS);
    EXPECT_EQ(storage.container.find(housekeeperTestStorageReplayJobPath(statement_id)), storage.container.end());
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityCreatesPromotionAfterReplayUnsafeAndFinality)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string statement_id = "stmt-promote";
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageStatementPath(statement_id), housekeeperTestStorageStatementData()), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageFinalityPath(statement_id), housekeeperTestStorageFinalityData()), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-1"), housekeeperTestStorageUnsafeResultData("hg-1")), zxid), Error::ZOK);
    EXPECT_EQ(storage.container.find(housekeeperTestStoragePromotionPath(statement_id)), storage.container.end());
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-2"), housekeeperTestStorageUnsafeResultData("hg-2")), zxid), Error::ZOK);
    EXPECT_EQ(storage.container.find(housekeeperTestStoragePromotionPath(statement_id)), storage.container.end());
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-3"), housekeeperTestStorageUnsafeResultData("hg-3")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-1"), housekeeperTestStorageAttestationData("state-a")), zxid), Error::ZOK);
    EXPECT_EQ(storage.container.find(housekeeperTestStoragePromotionPath(statement_id)), storage.container.end());

    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-2"), housekeeperTestStorageAttestationData("state-a")), zxid), Error::ZOK);

    const auto decision_data = housekeeperTestNodeData(storage, "/housekeeper/v1/storage_integrity/decisions/" + statement_id);
    EXPECT_NE(decision_data.find("replay_quorum_met=true\n"), std::string::npos);
    EXPECT_NE(decision_data.find("unsafe_validated=true\n"), std::string::npos);
    EXPECT_NE(decision_data.find("finalized=true\n"), std::string::npos);
    EXPECT_NE(decision_data.find("promotion_ready=true\n"), std::string::npos);

    const auto promotion_data = housekeeperTestNodeData(storage, housekeeperTestStoragePromotionPath(statement_id));
    EXPECT_NE(promotion_data.find("promotion_id=promotion-stmt-promote\n"), std::string::npos);
    EXPECT_NE(promotion_data.find("unsafe_table=`hg_unsafe`.`dual_hg_auth.t_a`\n"), std::string::npos);
    EXPECT_NE(promotion_data.find("safe_table=`hg_safe`.`dual_hg_auth.t`\n"), std::string::npos);
    EXPECT_NE(promotion_data.find("partition_ids=202606\n"), std::string::npos);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityQuarantinesReplayMinorityWorker)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string statement_id = "stmt-replay-minority";
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageStatementPath(statement_id), housekeeperTestStorageStatementData()), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-1"), housekeeperTestStorageAttestationData("state-a")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-2"), housekeeperTestStorageAttestationData("state-a")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-3"), housekeeperTestStorageAttestationData("state-b")), zxid), Error::ZOK);

    const auto quarantine_data = housekeeperTestNodeData(storage, housekeeperTestStorageReplayQuarantinePath("hg-3"));
    EXPECT_NE(quarantine_data.find("worker_id=hg-3\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("reason=replay_minority_mismatch\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("statement_id=stmt-replay-minority\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("majority_hash=state-a\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("reported_hash=state-b\n"), std::string::npos);
    EXPECT_NE(quarantine_data.find("status=active\n"), std::string::npos);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityRejectsIncompleteUnsafeValidation)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string statement_id = "stmt-unsafe-fail";
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageStatementPath(statement_id), housekeeperTestStorageStatementData()), zxid), Error::ZOK);

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(
                housekeeperTestStorageUnsafeResultPath(statement_id),
                "row_count=1\nrows_hash=rows-hash\nreplica_digests=r1:1:rows-hash,r2:1:rows-hash\n"),
            zxid),
        Error::ZBADARGUMENTS);
    EXPECT_NE(storage.container.find(housekeeperTestStorageUnsafeResultPath(statement_id)), storage.container.end());
    EXPECT_EQ(storage.container.find(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-1")), storage.container.end());

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(
                housekeeperTestStorageUnsafeResultPath(statement_id, "hg-1"),
                housekeeperTestStorageUnsafeResultData("hg-2")),
            zxid),
        Error::ZBADARGUMENTS);
    EXPECT_EQ(storage.container.find(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-1")), storage.container.end());
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityRollbackBlocksPromotion)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    const std::string statement_id = "stmt-rollback";
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageStatementPath(statement_id), housekeeperTestStorageStatementData()), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-1"), housekeeperTestStorageAttestationData("state-a")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageAttestationPath(statement_id, "hg-2"), housekeeperTestStorageAttestationData("state-a")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-1"), housekeeperTestStorageUnsafeResultData("hg-1")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-2"), housekeeperTestStorageUnsafeResultData("hg-2")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageUnsafeResultPath(statement_id, "hg-3"), housekeeperTestStorageUnsafeResultData("hg-3")), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageRollbackPath(statement_id), "kind=mock\nreason=dispute\n"), zxid), Error::ZOK);
    EXPECT_EQ(housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(housekeeperTestStorageFinalityPath(statement_id), housekeeperTestStorageFinalityData()), zxid), Error::ZOK);

    EXPECT_EQ(storage.container.find(housekeeperTestStoragePromotionPath(statement_id)), storage.container.end());
    const auto rollback_data = housekeeperTestNodeData(storage, housekeeperTestStorageRollbackTaskPath(statement_id));
    EXPECT_NE(rollback_data.find("rollback_id=rollback-stmt-rollback\n"), std::string::npos);
    EXPECT_NE(rollback_data.find("reason=dispute\n"), std::string::npos);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityAllowsHouseGateWorkerLedgerRoots)
{
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddPathIfMissing(storage, "/housekeeper");
    housekeeperTestAddPathIfMissing(storage, "/housekeeper/v1");

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity", ""),
            zxid),
        Error::ZOK);

    const char * root_paths[] = {
        "/housekeeper/v1/storage_integrity/statements",
        "/housekeeper/v1/storage_integrity/blocks",
        "/housekeeper/v1/storage_integrity/replay_jobs",
        "/housekeeper/v1/storage_integrity/attestations",
        "/housekeeper/v1/storage_integrity/replay_failures",
        "/housekeeper/v1/storage_integrity/unsafe_tasks",
        "/housekeeper/v1/storage_integrity/unsafe_results",
        "/housekeeper/v1/storage_integrity/unsafe_failures",
        "/housekeeper/v1/storage_integrity/finality",
        "/housekeeper/v1/storage_integrity/rollbacks",
        "/housekeeper/v1/storage_integrity/promotions",
        "/housekeeper/v1/storage_integrity/rollback_tasks",
        "/housekeeper/v1/storage_integrity/replay_quarantine",
        "/housekeeper/v1/storage_integrity/rollback_leases",
        "/housekeeper/v1/storage_integrity/rollback_results",
        "/housekeeper/v1/storage_integrity/rollback_failures",
        "/housekeeper/v1/storage_integrity/promotion_leases",
        "/housekeeper/v1/storage_integrity/promotion_results",
        "/housekeeper/v1/storage_integrity/promotion_failures",
        "/housekeeper/v1/storage_integrity/safe_audit_tasks",
        "/housekeeper/v1/storage_integrity/safe_audit_votes",
        "/housekeeper/v1/storage_integrity/decisions",
    };

    for (const auto * root_path : root_paths)
    {
        EXPECT_EQ(
            housekeeperTestProcessWrite(storage, housekeeperTestMakeCreateRequest(root_path, ""), zxid),
            Error::ZOK)
            << root_path;
    }
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityAllowsHouseGateWorkerLedgerWrites)
{
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/replay_failures/00000000000000000001", ""),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/replay_failures/00000000000000000001/hg-1", "{\"error\":\"boom\"}"),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/unsafe_failures/stmt-worker", "{\"error\":\"boom\"}"),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/promotion_leases/stmt-worker", "{\"worker_id\":\"hg-1\"}"),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/promotion_results/stmt-worker", "{\"promotion_id\":\"promotion-stmt-worker\"}"),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/rollback_leases/stmt-worker", "{\"worker_id\":\"hg-1\"}"),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/rollback_results/stmt-worker", "{\"rollback_id\":\"rollback-stmt-worker\"}"),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/safe_audit_tasks/audit-stmt-worker", ""),
            zxid),
        Error::ZOK);
    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest("/housekeeper/v1/storage_integrity/safe_audit_tasks/audit-stmt-worker/r1", "{\"audit_id\":\"audit-stmt-worker\"}"),
            zxid),
        Error::ZOK);
}

TYPED_TEST(CoordinationTest, TestHouseKeeperStorageIntegrityRejectsDirectManagedLedgerWrites)
{
    using namespace DB;
    using namespace Coordination;

    using Storage = typename TestFixture::Storage;

    ChangelogDirTest rocks("./rocksdb");
    this->setRocksDBDirectory("./rocksdb");

    Storage storage{500, "", this->keeper_context};
    int64_t zxid = 0;
    housekeeperTestAddControlPaths(storage);

    EXPECT_EQ(
        housekeeperTestProcessWrite(
            storage,
            housekeeperTestMakeCreateRequest(housekeeperTestStoragePromotionPath("stmt-direct"), "promotion_id=promotion-stmt-direct\n"),
            zxid),
        Error::ZBADARGUMENTS);
}

#endif
