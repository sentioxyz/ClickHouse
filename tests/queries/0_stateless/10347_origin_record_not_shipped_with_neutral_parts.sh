#!/usr/bin/env bash
# Tags: no-replicated-database, no-ordinary-database
# A part without data whose meaning depends on the stored binary type origin carries no part record anywhere it can reach a server that
# does not know the record (an older or official version refuses it as an unexpected file, as in the upstream integration test
# test_backup_restore_on_cluster/test_different_versions): a backup of it has no record, and a record that it brings along into the table
# (from detached/, a backup or a fetch) is removed when it is attached. A part with such data keeps its record in the backup.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

RECORD=sentio_binary_type_origin.txt
B="${CLICKHOUSE_TEST_UNIQUE_NAME}"

data_path() {
    $CLICKHOUSE_CLIENT --query "SELECT arrayElement(data_paths, 1) FROM system.tables WHERE database = currentDatabase() AND name = '$1'"
}
BACKUPS=$($CLICKHOUSE_CLIENT --query "SELECT path FROM system.disks WHERE name = 'backups'")

$CLICKHOUSE_CLIENT --query "
    CREATE TABLE plain (k UInt64, d Decimal512(60)) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    CREATE TABLE dyn (k UInt64, v Dynamic) ENGINE = MergeTree ORDER BY k SETTINGS min_bytes_for_full_part_storage = 0;
    INSERT INTO plain VALUES (1, 1.5); INSERT INTO plain VALUES (2, 2.5);
    INSERT INTO dyn VALUES (1, 'a'::String);"
$CLICKHOUSE_CLIENT --query "BACKUP TABLE plain TO Disk('backups', '${B}_plain')" > /dev/null
$CLICKHOUSE_CLIENT --query "BACKUP TABLE dyn TO Disk('backups', '${B}_dyn')" > /dev/null
echo "records in the backup of the table without such data: $(find "${BACKUPS}${B}_plain" -name "${RECORD}" | wc -l)"
echo "records in the backup of the Dynamic table: $(find "${BACKUPS}${B}_dyn" -name "${RECORD}" | wc -l)"

$CLICKHOUSE_CLIENT --query "RESTORE TABLE plain AS plain_restored FROM Disk('backups', '${B}_plain')" > /dev/null
echo "records in the restored parts: $(find "$(data_path plain_restored)" -path '*all_*' -name "${RECORD}" | wc -l)"
$CLICKHOUSE_CLIENT --query "SELECT k, d FROM plain_restored ORDER BY k"

# A detached part without such data that brings a record (here one of another part, as an older version or a copy may leave it).
$CLICKHOUSE_CLIENT --query "ALTER TABLE plain DETACH PARTITION tuple()"
for part in "$(data_path plain)"detached/all_*; do
    printf 'sentio binary type origin record version: 1\nkind: part\norigin: sentio_legacy_v0\npart checksum: 0123456789ABCDEF0123456789ABCDEF\n' > "${part}/${RECORD}"
done
$CLICKHOUSE_CLIENT --send_logs_level=error --query "ALTER TABLE plain ATTACH PARTITION tuple()"
echo "records in the attached parts: $(find "$(data_path plain)" -path '*all_*' -name "${RECORD}" | wc -l)"
$CLICKHOUSE_CLIENT --query "SELECT k, d FROM plain ORDER BY k"

$CLICKHOUSE_CLIENT --query "DROP TABLE plain; DROP TABLE plain_restored; DROP TABLE dyn"
