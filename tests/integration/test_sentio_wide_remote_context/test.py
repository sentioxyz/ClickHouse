import pytest

from helpers.cluster import ClickHouseCluster


cluster = ClickHouseCluster(__file__)
initiator = cluster.add_instance("initiator")
peer = cluster.add_instance("peer")


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        peer.query("CREATE TABLE wide_remote_source (id UInt32, v Dynamic(max_types=1)) ENGINE=MergeTree ORDER BY id")
        peer.query(
            "INSERT INTO wide_remote_source SELECT number, "
            "multiIf(number % 3 = 0, toInt512(-toInt64(number))::Dynamic, "
            "number % 3 = 1, toUInt512(number)::Dynamic, "
            "toDecimal512(toString(number) || '.25', 2)::Dynamic) FROM numbers(12)"
        )
        yield cluster
    finally:
        cluster.shutdown()


def test_remote_dynamic_origin_is_query_scoped(started_cluster):
    query = (
        f"SELECT id, v FROM remote('{peer.ip_address}:9000', default, wide_remote_source) "
        "ORDER BY id FORMAT TSV"
    )
    expected = "".join(
        f"{i}\t{-i if i % 3 == 0 else i if i % 3 == 1 else str(i) + '.25'}\n"
        for i in range(12)
    )
    bridge = {"output_format_binary_type_writer_mode": "sentio_legacy_bridge"}
    legacy = {**bridge, "input_format_binary_type_origin": "sentio_legacy_v0"}
    unknown = {**bridge, "input_format_binary_type_origin": "unknown"}
    assert initiator.http_query(query, params=legacy) == expected
    error = initiator.http_query_and_get_error(query, params=unknown)
    assert "INCORRECT_DATA" in error
    assert "ambiguous without a declared origin" in error
    assert initiator.http_query(query, params=legacy) == expected
    canonical = {
        "input_format_binary_type_origin": "unknown",
        "output_format_binary_type_writer_mode": "sentio_wide_v1",
    }
    assert initiator.http_query(query, params=canonical) == expected
