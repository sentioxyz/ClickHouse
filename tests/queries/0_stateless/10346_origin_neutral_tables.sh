#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database
# The stored binary type origin matters only for stored values that can hold binary type headers (`Dynamic`, `JSON`, types with them
# nested, aggregation states with such arguments). A table without such a column gets no record file (the file layout of upstream:
# plain Decimal512/Int512/UInt512 columns and states of them included), until an ALTER gives it such a column; its parts get no part
# record when detached; parts move between such tables whatever their origins; a part without such data is attached into a registered
# table without a record of its own. Tables and parts with such data keep every check. A detached part whose files changed after its
# record was written (checksums.txt removed, files renamed) is attached and read with the unknown origin instead of being refused.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ERR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.stderr"
REGISTER="MODIFY SENTIO BINARY TYPE ORIGIN 'sentio_legacy_v0'"
BACKUP="Disk('backups', '${CLICKHOUSE_TEST_UNIQUE_NAME}')"
RECORD=sentio_binary_type_origin.txt

reject() {
    local label="$1" code="$2" message="$3"
    shift 3
    local out rc err
    out=$("$@" 2> "$ERR"); rc=$?
    err=$(cat "$ERR")
    if [ "$rc" -ne 0 ] && [[ "$err" == *"$code"* ]] && [[ "$err" == *"$message"* ]]; then
        echo "$label: refused"
    else
        echo "$label: UNEXPECTED rc=$rc"; echo "stdout: $out"; echo "stderr: $err"
    fi
}

data_path() {
    $CLICKHOUSE_CLIENT --query "SELECT arrayElement(data_paths, 1) FROM system.tables WHERE database = currentDatabase() AND name = '$1'"
}

record() {
    local p; p=$(data_path "$1")
    if [ -f "${p}${RECORD}" ]; then echo "$1: record $(grep '^origin: ' "${p}${RECORD}")"; else echo "$1: no record"; fi
}

echo '--- table records'
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE mt_plain (k UInt64, d Decimal512(60), i Int512, u UInt512, s AggregateFunction(uniq, UInt512)) ENGINE = MergeTree ORDER BY k;
    CREATE TABLE log_plain (k UInt64, d Decimal512(60)) ENGINE = Log;
    CREATE TABLE tinylog_plain (k UInt64, d Decimal512(60)) ENGINE = TinyLog;
    CREATE TABLE stripelog_plain (k UInt64, d Decimal512(60)) ENGINE = StripeLog;
    CREATE TABLE mt_dynamic (k UInt64, v Dynamic) ENGINE = MergeTree ORDER BY k;
    CREATE TABLE mt_json (k UInt64, j JSON) ENGINE = MergeTree ORDER BY k;
    CREATE TABLE mt_state (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = AggregatingMergeTree ORDER BY k;
    CREATE TABLE log_state (k UInt64, s AggregateFunction(groupArray, Dynamic)) ENGINE = Log;"
for t in mt_plain log_plain tinylog_plain stripelog_plain mt_dynamic mt_json mt_state log_state; do record "$t"; done
$CLICKHOUSE_CLIENT --query "INSERT INTO log_plain VALUES (1, 1.5); TRUNCATE TABLE log_plain;"
record log_plain

echo '--- ALTER gives a table its first such column'
$CLICKHOUSE_CLIENT --query "CREATE TABLE mt_alter (k UInt64) ENGINE = MergeTree ORDER BY k; INSERT INTO mt_alter VALUES (1)"
record mt_alter
$CLICKHOUSE_CLIENT --query "ALTER TABLE mt_alter ADD COLUMN v Dynamic"
record mt_alter
$CLICKHOUSE_CLIENT --query "INSERT INTO mt_alter VALUES (2, 'x'::String), (3, toUInt512(3)); SELECT k, v, dynamicType(v) FROM mt_alter ORDER BY k"
reject 'registered after the ALTER' BAD_ARGUMENTS 'has the stored binary type origin official_canonical' \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE mt_alter ${REGISTER}"
# A table with a stated origin keeps it, also without such a column.
$CLICKHOUSE_CLIENT --query "ALTER TABLE mt_alter DROP COLUMN v SETTINGS mutations_sync = 2"
record mt_alter

echo '--- parts move between tables without such data, whatever their origins'
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE p_new (k UInt64, d Decimal512(60)) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    CREATE TABLE p_reg (k UInt64, d Decimal512(60)) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    CREATE TABLE p_canon (k UInt64, d Decimal512(60)) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    INSERT INTO p_new VALUES (1, 1.5); INSERT INTO p_reg VALUES (2, 2.5); INSERT INTO p_canon VALUES (3, 3.5);
    ALTER TABLE p_reg ${REGISTER};
    ALTER TABLE p_canon ADD COLUMN v Dynamic; ALTER TABLE p_canon DROP COLUMN v SETTINGS mutations_sync = 2;"
for t in p_new p_reg p_canon; do record "$t"; done
$CLICKHOUSE_CLIENT --query "ALTER TABLE p_new ATTACH PARTITION tuple() FROM p_reg"
$CLICKHOUSE_CLIENT --query "ALTER TABLE p_canon MOVE PARTITION tuple() TO TABLE p_reg"
$CLICKHOUSE_CLIENT --query "ALTER TABLE p_canon REPLACE PARTITION tuple() FROM p_new"
for t in p_new p_reg p_canon; do $CLICKHOUSE_CLIENT --query "SELECT '$t', k, d FROM $t ORDER BY k"; done

echo '--- tables with such data keep the check'
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE s_canon (k UInt64, v Dynamic) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    INSERT INTO s_canon VALUES (1, 'a'::String);"
$CLICKHOUSE_CLIENT --query "BACKUP TABLE s_canon TO ${BACKUP}" > /dev/null
$CLICKHOUSE_CLIENT --query "RESTORE TABLE s_canon AS s_unset FROM ${BACKUP}" > /dev/null
record s_canon; record s_unset
reject 'ATTACH PARTITION FROM a table without an origin' BAD_ARGUMENTS 'Tables have different stored binary type origins' \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE s_canon ATTACH PARTITION tuple() FROM s_unset"

echo '--- detached parts'
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE d_src (k UInt64, d Decimal512(60)) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    CREATE TABLE d_dst (k UInt64, d Decimal512(60)) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    INSERT INTO d_src VALUES (5, 5.5);
    ALTER TABLE d_dst ${REGISTER};
    ALTER TABLE p_canon DETACH PARTITION tuple();
    ALTER TABLE d_src DETACH PARTITION tuple();"
echo "part records detached from a canonical table without such data: $(find "$(data_path p_canon)detached" -name "${RECORD}" | wc -l)"
mv "$(data_path d_src)"detached/all_* "$(data_path d_dst)detached/"
$CLICKHOUSE_CLIENT --query "ALTER TABLE d_dst ATTACH PARTITION tuple(); SELECT 'd_dst', k, d FROM d_dst"
# A part with such data and without a record of its own is still refused by a registered table.
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE ds_dst AS s_unset;
    ALTER TABLE ds_dst ${REGISTER};
    ALTER TABLE s_unset DETACH PARTITION tuple();"
# The restored parts carry the record of their backup; without it (as a part detached by 26.3) nothing proves their origin.
mv "$(data_path s_unset)"detached/all_* "$(data_path ds_dst)detached/"
rm -f "$(data_path ds_dst)"detached/all_*/"${RECORD}"
reject 'a part with such data without a record into a registered table' INCORRECT_DATA 'no stored binary type origin record of its own' \
    $CLICKHOUSE_CLIENT --query "ALTER TABLE ds_dst ATTACH PARTITION tuple()"

echo '--- a detached part whose files changed after its record was written'
$CLICKHOUSE_CLIENT --query "
    CREATE TABLE g (k UInt64, v Dynamic, INDEX mm k TYPE minmax GRANULARITY 1) ENGINE = MergeTree ORDER BY k
        SETTINGS min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0, packed_skip_index_max_bytes = 0, replace_long_file_name_to_hash = 0;
    INSERT INTO g VALUES (1, 'x'::String), (2, toUInt512(3)), (3, toInt512(-7));
    ALTER TABLE g DETACH PARTITION tuple();"
part=$(ls -d "$(data_path g)"detached/all_* | head -1)
echo "record: $(grep '^origin: ' "${part}/${RECORD}")"
[ -f "${part}/skp_idx_mm.idx2" ] && mv "${part}/skp_idx_mm.idx2" "${part}/skp_idx_mm.idx"
rm "${part}/checksums.txt"
# The server warns that it recomputes the checksums and that the record does not apply: expected here.
$CLICKHOUSE_CLIENT --send_logs_level=error --query "ALTER TABLE g ATTACH PARTITION tuple(); SELECT k, v, dynamicType(v) FROM g ORDER BY k"

$CLICKHOUSE_CLIENT --query "
    DROP TABLE mt_plain; DROP TABLE log_plain; DROP TABLE tinylog_plain; DROP TABLE stripelog_plain; DROP TABLE mt_dynamic; DROP TABLE mt_json;
    DROP TABLE mt_state; DROP TABLE log_state; DROP TABLE mt_alter; DROP TABLE p_new; DROP TABLE p_reg; DROP TABLE p_canon; DROP TABLE s_canon;
    DROP TABLE s_unset; DROP TABLE d_src; DROP TABLE d_dst; DROP TABLE ds_dst; DROP TABLE g;"
rm -f "${ERR:?}"
