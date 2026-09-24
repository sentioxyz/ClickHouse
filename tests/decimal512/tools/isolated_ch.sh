#!/usr/bin/env bash
# isolated_ch.sh - isolated, NON-PRODUCTION ClickHouse test instances and test runs on this host.
#
# PRODUCTION BOUNDARY: this script never talks to production. Instances listen on loopback only (127.0.0.1-3), have
# no Keeper/raft port, no remote hosts except themselves, and keep data under $CH_ISO_ROOT. Every test
# run first proves isolation (all ports owned by our PID, loopback only, expected buildId, binary copied
# under $CH_ISO_ROOT) and refuses otherwise (fail closed).
#
# Usage:
#   isolated_ch.sh start  <name> <binary> <tree>     copy binary (by build-id) and start instance <name>
#   isolated_ch.sh stop   <name>                     stop our instance (refuses foreign PIDs)
#   isolated_ch.sh status <name>
#   isolated_ch.sh test   <name> <tree> <label> <expected-build-id-prefix> [clickhouse-test args] <test-regex...>
#                         exits with clickhouse-test's exact status (or cd's status if <tree> cannot be entered)
#   isolated_ch.sh local  <binary> <sql-file>        clickhouse local, fresh temp dir, stdin=/dev/null
#
# Env: CH_ISO_ROOT (default ${CLAUDE_JOB_DIR:-$HOME/.cache}/ch-isolated), CH_ISO_LOGS (default $CH_ISO_ROOT/logs)
# Port bases per instance name: a=39000 b=49000 c=59000 (tcp=base, http=base-876, mysql=+4, pg=+5, interserver=+9).
# Lessons baked in (2026-09-24): clickhouse local inherits the caller's stdin and can block on a socket ->
# always </dev/null; 26.8 MemoryWorker reads the session cgroup and a concurrent build makes the server
# refuse queries -> memory_worker_use_cgroup=0 and memory_worker_dynamic_hard_limit=0 in the test config;
# `^name$` selects nothing in clickhouse-test (it matches file names) -> use `^name\.`; clickhouse-test randomly
# injects parallel-replica settings that need a `parallel_replicas` cluster -> it is defined (pointing at itself).
set -u
ROOT=${CH_ISO_ROOT:-${CLAUDE_JOB_DIR:-$HOME/.cache}/ch-isolated}
LOGS=${CH_ISO_LOGS:-$ROOT/logs}
mkdir -p "$ROOT/bin" "$LOGS"

ports() {
  case $1 in a) BASE=39000;; b) BASE=49000;; c) BASE=59000;; *) echo "instance name must be a, b or c" >&2; exit 2;; esac
  HTTP=$((BASE - 876)); MYSQL=$((BASE + 4)); PG=$((BASE + 5)); IS=$((BASE + 9)); D=$ROOT/inst-$1
}

prove_isolated() {  # $1 name, $2 expected build id prefix
  ports "$1"
  local pid; pid=$(cat "$D/server.pid" 2>/dev/null) || { echo "REFUSE: no pid file"; return 90; }
  local exe; exe=$(readlink "/proc/$pid/exe") || { echo "REFUSE: instance not running"; return 90; }
  case "$exe" in "$ROOT"/bin/*) ;; *) echo "REFUSE: server exe $exe is not under $ROOT/bin"; return 89;; esac
  for p in $BASE $HTTP $MYSQL $PG $IS; do
    ss -ltnp | awk -v a="127.0.0.1:$p" '$4==a{print $6}' | grep -q "pid=$pid," || { echo "REFUSE: 127.0.0.1:$p not owned by pid $pid"; return 90; }
  done
  if ss -ltnp | grep "pid=$pid," | awk '{print $4}' | grep -vqE '^127\.0\.0\.[123]:'; then echo "REFUSE: non-loopback listener"; return 91; fi
  local bid; bid=$(timeout 10 "${exe% (deleted)}" client --host 127.0.0.1 --port "$BASE" --query "SELECT buildId()" < /dev/null 2>/dev/null)
  [[ "${bid,,}" == ${2,,}* ]] || { echo "REFUSE: buildId '$bid' != expected '$2' (stale or wrong binary)"; return 92; }
  echo "$exe"
}

# Run <tree>/tests/clickhouse-test with <exe>, append all output to <log>, and return the runner's exact exit
# status; if <tree> cannot be entered, return cd's status. It does not check isolation: `test` calls it only after
# prove_isolated, and tests/selftest.sh calls it with a mock runner (offline, no server).
run_clickhouse_test() {  # <log> <tree> <exe> <tmp-dir> [clickhouse-test args...]
  local log=$1 tree=$2 exe=$3 tmp=$4 rc
  shift 4
  {
    echo "# cmd: tests/clickhouse-test $*"
    (
      cd "$tree" || { rc=$?; echo "# ERROR: cannot cd to $tree"; exit "$rc"; }
      exec python3 tests/clickhouse-test -b "$exe" --tmp "$tmp" "$@" < /dev/null
    )
    rc=$?
    echo "# exit_code=$rc"
  } >> "$log" 2>&1
  return "$rc"
}

# Print the log path, its last line and the result counts, then exit with <rc>.
finish_test_run() {  # <log> <rc>
  echo "$1"; tail -n 1 "$1"
  echo "OK=$(grep -cE '\[ OK \]' "$1") FAIL=$(grep -cE '\[ FAIL \]' "$1") SKIPPED=$(grep -cE '\[ SKIPPED \]' "$1")"
  exit "$2"
}

# Sourced (tests/selftest.sh): define the functions only, never run the dispatcher.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then return 0; fi

case ${1:-} in
start)
  NAME=$2; BIN=$3; TREE=$4; ports "$NAME"
  for p in $BASE $HTTP $MYSQL $PG $IS; do
    ss -ltn | awk '{print $4}' | grep -qE ":$p\$" && { echo "REFUSE: port $p busy"; exit 3; }
  done
  BID=$(readelf -n "$BIN" | awk '/Build ID/{print substr($3,1,12)}')
  COPY=$ROOT/bin/clickhouse-$BID
  [ -x "$COPY" ] || cp "$BIN" "$COPY"
  mkdir -p "$D"/{data,tmp,log,conf,access}
  cat > "$D/conf/config.xml" <<XML
<clickhouse>
    <logger><level>information</level><log>$D/log/server.log</log><errorlog>$D/log/server.err.log</errorlog><size>200M</size><count>3</count></logger>
    <listen_host>127.0.0.1</listen_host><listen_host>127.0.0.2</listen_host><listen_host>127.0.0.3</listen_host>
    <tcp_port>$BASE</tcp_port><http_port>$HTTP</http_port><mysql_port>$MYSQL</mysql_port><postgresql_port>$PG</postgresql_port>
    <interserver_http_port>$IS</interserver_http_port><interserver_http_host>127.0.0.1</interserver_http_host>
    <path>$D/data/</path><tmp_path>$D/tmp/</tmp_path>
    <user_files_path>$TREE/tests/queries/0_stateless/</user_files_path>
    <format_schema_path>$TREE/tests/queries/0_stateless/format_schemas/</format_schema_path>
    <user_directories><users_xml><path>$D/conf/users.xml</path></users_xml><local_directory><path>$D/access/</path></local_directory></user_directories>
    <default_profile>default</default_profile><default_database>default</default_database>
    <max_server_memory_usage_to_ram_ratio>0.25</max_server_memory_usage_to_ram_ratio>
    <memory_worker_use_cgroup>0</memory_worker_use_cgroup><memory_worker_dynamic_hard_limit>0</memory_worker_dynamic_hard_limit>
    <custom_settings_prefixes>custom_</custom_settings_prefixes>
    <database_atomic_delay_before_drop_table_sec>0</database_atomic_delay_before_drop_table_sec>
    <remote_servers>
        <test_shard_localhost><shard><replica><host>127.0.0.1</host><port>$BASE</port></replica></shard></test_shard_localhost>
        <test_cluster_two_shards_localhost><shard><replica><host>127.0.0.1</host><port>$BASE</port></replica></shard><shard><replica><host>127.0.0.1</host><port>$BASE</port></replica></shard></test_cluster_two_shards_localhost>
        <!-- clickhouse-test randomly enables parallel replicas with cluster_for_parallel_replicas=parallel_replicas -->
        <parallel_replicas><shard><replica><host>127.0.0.1</host><port>$BASE</port></replica></shard></parallel_replicas>
        <!-- loopback aliases of this same instance, for tests that address 127.0.0.2/127.0.0.3 or need several replicas -->
        <test_cluster_two_shards><shard><replica><host>127.0.0.1</host><port>$BASE</port></replica></shard><shard><replica><host>127.0.0.2</host><port>$BASE</port></replica></shard></test_cluster_two_shards>
        <test_cluster_one_shard_three_replicas_localhost><shard><internal_replication>false</internal_replication><replica><host>127.0.0.1</host><port>$BASE</port></replica><replica><host>127.0.0.2</host><port>$BASE</port></replica><replica><host>127.0.0.3</host><port>$BASE</port></replica></shard></test_cluster_one_shard_three_replicas_localhost>
    </remote_servers>
    <query_log><database>system</database><table>query_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></query_log>
    <text_log><database>system</database><table>text_log</table><level>trace</level><flush_interval_milliseconds>1000</flush_interval_milliseconds></text_log>
    <crash_log><database>system</database><table>crash_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></crash_log>
</clickhouse>
XML
  cat > "$D/conf/users.xml" <<'XML'
<clickhouse>
    <profiles><default><max_memory_usage>8000000000</max_memory_usage></default></profiles>
    <users><default><password></password><networks><ip>127.0.0.1</ip><ip>::1</ip></networks><profile>default</profile><quota>default</quota>
        <access_management>1</access_management><named_collection_control>1</named_collection_control></default></users>
    <quotas><default></default></quotas>
</clickhouse>
XML
  printf '<config><host>127.0.0.1</host><port>%s</port></config>\n' "$BASE" > "$D/conf/client.xml"
  # `exec` so that no shell stays behind holding the caller's stdout/stderr (a pipe would never see EOF)
  (cd "$D" && exec nohup "$COPY" server --config-file="$D/conf/config.xml" --pid-file="$D/server.pid" > "$D/log/stdout.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
  ready=0
  for _ in $(seq 1 90); do timeout 10 "$COPY" client --host 127.0.0.1 --port "$BASE" --query "SELECT 1" < /dev/null > /dev/null 2>&1 && { ready=1; break; }; sleep 1; done
  [ "$ready" = 1 ] || { echo "REFUSE: instance $NAME did not become ready (see $D/log/)"; exit 94; }
  echo "started $NAME pid=$(cat "$D/server.pid") build_id=$(timeout 10 "$COPY" client --host 127.0.0.1 --port "$BASE" --query 'SELECT buildId()' < /dev/null) ports=$BASE,$HTTP,$MYSQL,$PG,$IS"
  ;;
stop)
  ports "$2"; pid=$(cat "$D/server.pid" 2>/dev/null) || { echo "not running"; exit 0; }
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || { echo "not running (stale pid file)"; rm -f "$D/server.pid"; exit 0; }
  case "$exe" in "$ROOT"/bin/*) kill "$pid"; for _ in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done; echo "stopped $2";;
                 *) echo "REFUSE: pid $pid ($exe) is not ours"; exit 4;; esac
  ;;
status)
  ports "$2"; pid=$(cat "$D/server.pid" 2>/dev/null); echo "$2 pid=${pid:-none} exe=$(readlink "/proc/${pid:-0}/exe" 2>/dev/null)"
  [ -n "${pid:-}" ] && ss -ltnp | grep "pid=$pid," | awk '{print "  listen", $4}'
  ;;
test)
  NAME=$2; TREE=$3; LABEL=$4; EXPECT=$5; shift 5; ports "$NAME"
  EXE=$(prove_isolated "$NAME" "$EXPECT") || { echo "$EXE"; exit 90; }
  # refuse tests that hard-code default ports of other local servers
  for sel in "$@"; do
    case "$sel" in -*) continue;; esac
    name=${sel#^}; name=${name%\\.}
    for f in "$TREE"/tests/queries/0_stateless/$name.sql "$TREE"/tests/queries/0_stateless/$name.sh; do
      [ -f "$f" ] && grep -qE '(:|port[ =]*)(9000|9004|9005|9009|8123|9181|2181)\b' "$f" && { echo "REFUSE: $f hard-codes a default port"; exit 93; }
    done
  done
  export CLICKHOUSE_HOST=127.0.0.1 CLICKHOUSE_PORT_TCP=$BASE CLICKHOUSE_PORT_HTTP=$HTTP CLICKHOUSE_PORT_HTTP_PROTO=http
  export CLICKHOUSE_PORT_MYSQL=$MYSQL CLICKHOUSE_PORT_POSTGRESQL=$PG CLICKHOUSE_PORT_INTERSERVER=$IS
  export CLICKHOUSE_CONFIG=$D/conf/config.xml CLICKHOUSE_CONFIG_CLIENT=$D/conf/client.xml CLICKHOUSE_BINARY=$EXE
  export CLICKHOUSE_USER_FILES=$TREE/tests/queries/0_stateless CLICKHOUSE_TMP=$ROOT/tests-tmp-$NAME
  mkdir -p "$CLICKHOUSE_TMP"
  TS=$(date -u +%Y%m%dT%H%M%SZ); LOG=$LOGS/test_${LABEL}_${TS}.log
  echo "# instance=$NAME label=$LABEL ts=$TS exe=$EXE tree=$TREE" > "$LOG"
  run_clickhouse_test "$LOG" "$TREE" "$EXE" "$CLICKHOUSE_TMP" "$@"
  finish_test_run "$LOG" "$?"
  ;;
local)
  BIN=$(readlink -f "$2"); SQL=$(readlink -f "$3"); W=$(mktemp -d "$ROOT/local.XXXXXX")
  (cd "$W" && "$BIN" local --multiquery --queries-file "$SQL" < /dev/null); rc=$?
  rm -rf "$W"; exit $rc
  ;;
*)
  sed -n '2,20p' "$0"; exit 2
  ;;
esac
