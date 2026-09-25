#!/usr/bin/env bash
# isolated_ch.sh - isolated, NON-PRODUCTION ClickHouse test instances and test runs on this host.
#
# PRODUCTION BOUNDARY: this script never talks to production. Instances listen on loopback only (127.0.0.1-3), have
# no Keeper/raft port unless CH_ISO_KEEPER=1 asks for a private single-node Keeper on loopback ports (below), no
# remote hosts except themselves, and keep data under $CH_ISO_ROOT. Every test
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
# Env: CH_ISO_ROOT (default ${CLAUDE_JOB_DIR:-$HOME/.cache}/ch-isolated), CH_ISO_LOGS (default $CH_ISO_ROOT/logs),
#      CH_ISO_KEEPER=1 at `start`: embedded single-node Keeper on loopback (client port base+81, raft port base+82) and a
#      <zookeeper> section pointing at it, for tests that need ZooKeeper (Replicated*, generateSerialID, ...). Its ports are
#      part of the isolation proof like all others (owned by the server PID, loopback only), so a Keeper that listened
#      on a non-loopback address would make every `test` refuse.
# Port bases per instance name: a=39000 b=49000 c=59000 d=29000 (tcp=base, http=base-876, mysql=+4, pg=+5,
# interserver=+9, keeper=+81, raft=+82).
# The production boundary is unchanged with CH_ISO_KEEPER: the Keeper is a private, empty ensemble of this instance.
# Host name (CH_ISO_HOST_ISOLATION at `start`): clickhouse-test replaces the runner's host name in every test output
# with "localhost" as a plain substring. On a host called "build" that also rewrote words of correct outputs (04881:
# "a build that" -> "a localhost that"); upstream CI does not see this because its containers have random host names.
#   real (default)  server on the host, real host name on both sides (the behaviour before 2026-09-25).
#   container       the server runs in a container with its own UTS namespace and the host name "localhost"
#                   (docker, local image $CH_ISO_DOCKER_IMAGE (default ubuntu:22.04, never pulled), host network and PID
#                   namespace, this user's uid, only $CH_ISO_ROOT and <tree> mounted, memory/pids capped by
#                   $CH_ISO_DOCKER_MEM (12g) / $CH_ISO_DOCKER_PIDS (8192)), and `test` runs clickhouse-test and its
#                   shells with the LD_PRELOAD library of hostname_shim.c, so the runner's host name is "localhost" too
#                   and its substitution changes nothing. (ClickHouse itself ignores LD_PRELOAD: it clears the variable
#                   and re-executes itself, see checkHarmfulEnvironmentVariables, hence the container for the server.)
# `test` refuses when the server's hostName() differs from the runner's host name, in either mode.
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
  case $1 in a) BASE=39000;; b) BASE=49000;; c) BASE=59000;; d) BASE=29000;; *) echo "instance name must be a, b, c or d" >&2; exit 2;; esac
  HTTP=$((BASE - 876)); MYSQL=$((BASE + 4)); PG=$((BASE + 5)); IS=$((BASE + 9)); D=$ROOT/inst-$1
  KEEPER=$((BASE + 81)); RAFT=$((BASE + 82))
  ALL_PORTS="$BASE $HTTP $MYSQL $PG $IS"
  if [ -f "$D/keeper.enabled" ] || [ "${CH_ISO_KEEPER:-0}" = 1 ]; then ALL_PORTS="$ALL_PORTS $KEEPER $RAFT"; fi
}

prove_isolated() {  # $1 name, $2 expected build id prefix
  ports "$1"
  local pid; pid=$(cat "$D/server.pid" 2>/dev/null) || { echo "REFUSE: no pid file"; return 90; }
  local exe; exe=$(readlink "/proc/$pid/exe") || { echo "REFUSE: instance not running"; return 90; }
  case "$exe" in "$ROOT"/bin/*) ;; *) echo "REFUSE: server exe $exe is not under $ROOT/bin"; return 89;; esac
  for p in $ALL_PORTS; do
    ss -ltnp | awk -v a="127.0.0.1:$p" '$4==a{print $6}' | grep -q "pid=$pid," || { echo "REFUSE: 127.0.0.1:$p not owned by pid $pid"; return 90; }
  done
  if ss -ltnp | grep "pid=$pid," | awk '{print $4}' | grep -vqE '^127\.0\.0\.[123]:'; then echo "REFUSE: non-loopback listener"; return 91; fi
  local bid; bid=$(timeout 10 "${exe% (deleted)}" client --host 127.0.0.1 --port "$BASE" --query "SELECT buildId()" < /dev/null 2>/dev/null)
  [[ "${bid,,}" == ${2,,}* ]] || { echo "REFUSE: buildId '$bid' != expected '$2' (stale or wrong binary)"; return 92; }
  echo "$exe"
}

# The LD_PRELOAD library of hostname_shim.c, built once per source version under $ROOT/lib and checked to take
# effect (socket.gethostname() and os.uname() in python3); prints its path, or REFUSE on stderr and returns 95.
hostname_shim_lib() {
  local src lib got
  src=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/hostname_shim.c
  [ -f "$src" ] || { echo "REFUSE: $src missing" >&2; return 95; }
  lib=$ROOT/lib/hostname_shim-$(sha256sum "$src" | cut -c1-12).so
  if [ ! -f "$lib" ]; then
    mkdir -p "$ROOT/lib"
    if ! cc -shared -fPIC -O2 -Wall -Werror -o "$lib.tmp.$$" "$src" -ldl; then
      rm -f "$lib.tmp.$$"; echo "REFUSE: cannot build $lib" >&2; return 95
    fi
    mv "$lib.tmp.$$" "$lib"
  fi
  got=$(LD_PRELOAD=$lib CH_ISO_HOSTNAME=localhost python3 -c 'import os, socket; print(socket.gethostname(), os.uname().nodename)' 2>&1)
  [ "$got" = "localhost localhost" ] || { echo "REFUSE: $lib does not set the host name (got '$got')" >&2; return 95; }
  echo "$lib"
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
  NAME=$2; BIN=$3; TREE=$4; rm -f "$ROOT/inst-$NAME/keeper.enabled"; ports "$NAME"
  for p in $ALL_PORTS; do
    ss -ltn | awk '{print $4}' | grep -qE ":$p\$" && { echo "REFUSE: port $p busy"; exit 3; }
  done
  BID=$(readelf -n "$BIN" | awk '/Build ID/{print substr($3,1,12)}')
  COPY=$ROOT/bin/clickhouse-$BID
  [ -x "$COPY" ] || cp "$BIN" "$COPY"
  mkdir -p "$D"/{data,tmp,log,conf,access}
  KEEPER_XML=""
  if [ "${CH_ISO_KEEPER:-0}" = 1 ]; then
    mkdir -p "$D/coordination"; touch "$D/keeper.enabled"
    # interserver_listen_host: without it the raft listener binds every interface (KeeperServer.cpp), with it only loopback
    KEEPER_XML="<interserver_listen_host>127.0.0.1</interserver_listen_host>
    <keeper_server><tcp_port>$KEEPER</tcp_port><server_id>1</server_id>
        <log_storage_path>$D/coordination/log</log_storage_path><snapshot_storage_path>$D/coordination/snapshots</snapshot_storage_path>
        <coordination_settings><operation_timeout_ms>10000</operation_timeout_ms><session_timeout_ms>30000</session_timeout_ms><raft_logs_level>warning</raft_logs_level></coordination_settings>
        <raft_configuration><server><id>1</id><hostname>127.0.0.1</hostname><port>$RAFT</port></server></raft_configuration></keeper_server>
    <zookeeper><node><host>127.0.0.1</host><port>$KEEPER</port></node></zookeeper>
    <macros><shard>s1</shard><replica>r1</replica></macros>
    <distributed_ddl><path>/clickhouse/task_queue/ddl</path></distributed_ddl>"
  fi
  # a private copy of the tree's format schemas: tests copy their own schemas into CLICKHOUSE_SCHEMA_FILES, which must
  # not be the source tree (nor the host default /var/lib/clickhouse/format_schemas that shell_config.sh falls back to)
  rm -rf "$D/format_schemas" && cp -r "$TREE/tests/queries/0_stateless/format_schemas" "$D/format_schemas"
  cat > "$D/conf/config.xml" <<XML
<clickhouse>
    <logger><level>information</level><log>$D/log/server.log</log><errorlog>$D/log/server.err.log</errorlog><size>200M</size><count>3</count></logger>
    <listen_host>127.0.0.1</listen_host><listen_host>127.0.0.2</listen_host><listen_host>127.0.0.3</listen_host>
    <tcp_port>$BASE</tcp_port><http_port>$HTTP</http_port><mysql_port>$MYSQL</mysql_port><postgresql_port>$PG</postgresql_port>
    <interserver_http_port>$IS</interserver_http_port><interserver_http_host>127.0.0.1</interserver_http_host>
    <path>$D/data/</path><tmp_path>$D/tmp/</tmp_path>
    <user_files_path>$TREE/tests/queries/0_stateless/</user_files_path>
    <format_schema_path>$D/format_schemas/</format_schema_path>
    <user_directories><users_xml><path>$D/conf/users.xml</path></users_xml><local_directory><path>$D/access/</path></local_directory></user_directories>
    <default_profile>default</default_profile><default_database>default</default_database>
    <max_server_memory_usage_to_ram_ratio>0.25</max_server_memory_usage_to_ram_ratio>
    <memory_worker_use_cgroup>0</memory_worker_use_cgroup><memory_worker_dynamic_hard_limit>0</memory_worker_dynamic_hard_limit>
    <custom_settings_prefixes>SQL_,custom_</custom_settings_prefixes>
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
    $KEEPER_XML
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
  rm -f "$D/hostname_shim" "$D/container" "$D/server.pid"
  case ${CH_ISO_HOST_ISOLATION:-real} in
  real)
    # `exec` so that no shell stays behind holding the caller's stdout/stderr (a pipe would never see EOF)
    (cd "$D" && exec nohup "$COPY" server --config-file="$D/conf/config.xml" --pid-file="$D/server.pid" > "$D/log/stdout.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
    ;;
  container)
    SHIM=$(hostname_shim_lib) || exit 95
    IMG=${CH_ISO_DOCKER_IMAGE:-ubuntu:22.04}; CNAME=ch-iso-$NAME-$BASE
    docker image inspect "$IMG" > /dev/null 2>&1 || { echo "REFUSE: docker image $IMG is not available locally (it is never pulled)"; exit 95; }
    if docker container inspect "$CNAME" > /dev/null 2>&1; then echo "REFUSE: container $CNAME already exists"; exit 95; fi
    docker run -d --rm --pull never --name "$CNAME" --hostname localhost --network host --pid host --user "$(id -u):$(id -g)" \
      --memory="${CH_ISO_DOCKER_MEM:-12g}" --memory-swap="${CH_ISO_DOCKER_MEM:-12g}" --pids-limit="${CH_ISO_DOCKER_PIDS:-8192}" \
      --cpus="$(nproc)" -v "$ROOT:$ROOT" -v "$TREE:$TREE" -w "$D" "$IMG" \
      sh -c 'exec "$0" server --config-file="$1" --pid-file="$2" > "$3" 2>&1 < /dev/null' \
      "$COPY" "$D/conf/config.xml" "$D/server.pid" "$D/log/stdout.log" > "$D/log/docker_run.log" 2>&1 \
      || { echo "REFUSE: docker run failed (see $D/log/docker_run.log)"; exit 95; }
    echo "$CNAME" > "$D/container"
    ;;
  *) echo "CH_ISO_HOST_ISOLATION must be real or container" >&2; exit 2;;
  esac
  ready=0
  for _ in $(seq 1 90); do timeout 10 "$COPY" client --host 127.0.0.1 --port "$BASE" --query "SELECT 1" < /dev/null > /dev/null 2>&1 && { ready=1; break; }; sleep 1; done
  if [ "$ready" != 1 ]; then
    echo "REFUSE: instance $NAME did not become ready (see $D/log/)"
    [ -f "$D/container" ] && docker stop -t 30 "$(cat "$D/container")" > /dev/null 2>&1
    exit 94
  fi
  if [ -f "$D/container" ]; then
    # the pid file holds a host PID (host PID namespace): the server, which is the container's main process or, with
    # ClickHouse's watchdog, its direct child
    spid=$(cat "$D/server.pid"); cpid=$(docker container inspect -f '{{.State.Pid}}' "$(cat "$D/container")" 2>/dev/null)
    sppid=$(awk '/^PPid:/{print $2}' "/proc/$spid/status" 2>/dev/null)
    hn=$(timeout 10 "$COPY" client --host 127.0.0.1 --port "$BASE" --query 'SELECT hostName()' < /dev/null)
    if [ -z "$cpid" ] || { [ "$cpid" != "$spid" ] && [ "$cpid" != "$sppid" ]; } || [ "$hn" != localhost ]; then
      echo "REFUSE: container $(cat "$D/container"): main pid '$cpid', server.pid '$spid' (parent '$sppid'), host name '$hn' (expected localhost); stopping it"
      kill "$spid"; docker stop -t 30 "$(cat "$D/container")" > /dev/null 2>&1; exit 95
    fi
    echo "$SHIM" > "$D/hostname_shim"
  fi
  echo "started $NAME pid=$(cat "$D/server.pid") build_id=$(timeout 10 "$COPY" client --host 127.0.0.1 --port "$BASE" --query 'SELECT buildId()' < /dev/null) ports=${ALL_PORTS// /,} host_isolation=${CH_ISO_HOST_ISOLATION:-real}$([ -f "$D/container" ] && echo " container=$(cat "$D/container")")"
  ;;
stop)
  ports "$2"; pid=$(cat "$D/server.pid" 2>/dev/null) || { echo "not running"; exit 0; }
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || { echo "not running (stale pid file)"; rm -f "$D/server.pid"; exit 0; }
  case "$exe" in "$ROOT"/bin/*) kill "$pid"; for _ in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done; echo "stopped $2";;
                 *) echo "REFUSE: pid $pid ($exe) is not ours"; exit 4;; esac
  # container mode: the container (--rm) ends with its main process; stop it by name if it is still there
  if [ -f "$D/container" ]; then
    c=$(cat "$D/container")
    for _ in $(seq 1 30); do docker container inspect "$c" > /dev/null 2>&1 || break; sleep 1; done
    docker container inspect "$c" > /dev/null 2>&1 && { docker stop -t 30 "$c" > /dev/null; echo "stopped container $c"; }
    rm -f "$D/container"
  fi
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
  export CLICKHOUSE_SCHEMA_FILES=$D/format_schemas
  [ -f "$D/keeper.enabled" ] && export CLICKHOUSE_PORT_KEEPER=$KEEPER
  [ -f "$D/hostname_shim" ] && export LD_PRELOAD="$(cat "$D/hostname_shim")" CH_ISO_HOSTNAME=localhost
  # the runner normalizes its own host name in outputs; the server's host names are normalized only if they match
  SRV_HN=$(timeout 10 "${EXE% (deleted)}" client --host 127.0.0.1 --port "$BASE" --query 'SELECT hostName()' < /dev/null 2>/dev/null)
  RUN_HN=$(python3 -c 'import socket; print(socket.gethostname())')
  [ -n "$SRV_HN" ] && [ "$SRV_HN" = "$RUN_HN" ] || { echo "REFUSE: server host name '$SRV_HN' != runner host name '$RUN_HN'"; exit 95; }
  mkdir -p "$CLICKHOUSE_TMP"
  TS=$(date -u +%Y%m%dT%H%M%SZ); LOG=$LOGS/test_${LABEL}_${TS}.log
  echo "# instance=$NAME label=$LABEL ts=$TS exe=$EXE tree=$TREE host_name=$RUN_HN shim=${LD_PRELOAD:-none}" > "$LOG"
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
