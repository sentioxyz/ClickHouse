#!/usr/bin/env bash
# mixed_replication.sh - ReplicatedMergeTree across two ClickHouse builds, coordinated by the production Keeper build.
# One Keeper and two replicas run on 127.0.0.1. Replica r2 is upgraded in place from the baseline to the candidate
# while r1 stays on the baseline (mixed versions), and is then rolled back to the baseline. Keeper is never upgraded.
# Each step compares both replicas row by row (sha256 of an ordered dump, formatted by one client binary):
#   part fetches baseline -> candidate and candidate -> baseline, mutations initiated on either build, a merge executed
#   by the candidate whose result the baseline downloads (r1 has always_fetch_merged_part), the reads right after the
#   upgrade and after the rollback, and replication after the rollback. Tables use Wide and Compact parts and hold
#   Decimal512, Nullable(Decimal512), Int512, UInt512, Dynamic (512-bit variants) and sum states of Decimal512.
#   One informational step (not required) records what happens to a mutation that only the candidate can execute
#   (Int512 arithmetic) while the other replica still runs the baseline.
#
# PRODUCTION BOUNDARY: talks only to the three processes it starts. They listen on 127.0.0.1 only (ports 38100-38299,
# refused when busy; the raft port is bound through interserver_listen_host), keep their data under <out>/iso and run
# links or copies of the binaries under <out>/iso/bin. After every start the script proves that the process runs that
# copy and that every listening socket of the process is on 127.0.0.1; otherwise it stops everything it started and
# exits 2 (fail closed). No pulls, no credentials, nothing outside <out>. SYSTEM SYNC REPLICA, ALTER, OPTIMIZE and
# KILL MUTATION go only to these loopback test servers.
#
#   mixed_replication.sh <out-dir> --keeper <clickhouse-keeper> --baseline <old clickhouse> --candidate <new clickhouse>
#
# Output in <out-dir>: identities.tsv (role, path, sha256, build_id, version), steps.tsv (step, actor, rc, verdict,
# detail), dumps/ and logs/. Exit 0 when every required step has rc 0 and verdict SAME or OK, 1 otherwise, 2 on a
# usage error or refused isolation.
set -u
export GIT_NO_LAZY_FETCH=1 GIT_OPTIONAL_LOCKS=0

usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
[ $# -ge 1 ] || usage
OUT=$1; shift
KEEPER= BASE= CAND=
while [ $# -gt 0 ]; do
  case $1 in
    --keeper) KEEPER=$2; shift 2;;
    --baseline) BASE=$2; shift 2;;
    --candidate) CAND=$2; shift 2;;
    *) usage;;
  esac
done
for b in "$KEEPER" "$BASE" "$CAND"; do [ -n "$b" ] && [ -x "$b" ] || { echo "ERROR: --keeper, --baseline and --candidate must be executables" >&2; exit 2; }; done
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: $OUT is not empty" >&2; exit 2; fi
mkdir -p "$OUT"/{iso/bin,logs,dumps}
OUT=$(cd "$OUT" && pwd); ISO=$OUT/iso
KP=38181; RAFT=38234
declare -A TCP=([r1]=38100 [r2]=38200) HTTP=([r1]=38123 [r2]=38223) ISP=([r1]=38109 [r2]=38209)
for p in $KP $RAFT ${TCP[r1]} ${HTTP[r1]} ${ISP[r1]} ${TCP[r2]} ${HTTP[r2]} ${ISP[r2]}; do
  ss -ltn | awk '{print $4}' | grep -qE ":$p\$" && { echo "REFUSE: port $p is busy (another server owns it)" >&2; exit 2; }
done
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$OUT/logs/run.log" >&2; }
printf 'step\tactor\trc\tverdict\tdetail\n' > "$OUT/steps.tsv"
step() { printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$(echo "${5:-}" | tr '\t\n' '  ' | cut -c1-300)" >> "$OUT/steps.tsv"; log "step $1 ($2): rc=$3 $4 ${5:-}"; }

# binaries: a hard link (same file system) or a copy under iso/bin, identified before anything runs
place() {  # <role> <binary> -> path of the placed binary
  local src; src=$(readlink -f "$2")
  local bid; bid=$(readelf -n "$src" 2>/dev/null | awk '/Build ID/{print substr($3,1,12)}')
  local dst=$ISO/bin/$1-${bid:-nobuildid}
  ln "$src" "$dst" 2>/dev/null || cp "$src" "$dst"
  echo "$dst"
}
KBIN=$(place keeper "$KEEPER"); BBIN=$(place baseline "$BASE"); CBIN=$(place candidate "$CAND")
printf 'role\tpath\tsha256\tbuild_id\tversion\n' > "$OUT/identities.tsv"
for rb in "KEEPER $KBIN $KEEPER" "BASELINE $BBIN $BASE" "CANDIDATE $CBIN $CAND"; do
  set -- $rb
  sha=$(sha256sum "$2" | cut -d' ' -f1); bid=$(readelf -n "$2" 2>/dev/null | awk '/Build ID/{print $3}')
  if [ "$1" = KEEPER ]; then ver=$("$2" --version 2>/dev/null | head -1)
  else ver=$(w=$(mktemp -d "$ISO/ident.XXXXXX"); cd "$w" && timeout 60 "$2" local --query "SELECT version()" < /dev/null 2>/dev/null; rm -rf "$w"); fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$(readlink -f "$3")" "$sha" "$bid" "$ver" >> "$OUT/identities.tsv"
done
cat "$OUT/identities.tsv" >&2

declare -A PIDFILE EXE
STARTED=()
stop_one() {  # <name>: stop a process this script started (its exe must be under iso/bin)
  local pf=${PIDFILE[$1]:-} pid exe
  [ -n "$pf" ] && pid=$(cat "$pf" 2>/dev/null) || return 0
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 0
  case "$exe" in "$ISO"/bin/*) kill "$pid"; for _ in $(seq 1 90); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
                              kill -0 "$pid" 2>/dev/null && kill -9 "$pid";;
                 *) log "REFUSE to stop pid $pid ($exe): not ours";; esac
}
cleanup() { for n in r2 r1 keeper; do stop_one "$n"; done; }
trap cleanup EXIT
refuse() { log "REFUSE: $*"; step isolation - 2 REFUSED "$*"; exit 2; }

prove() {  # <name> <ports...>: the process runs our copy and listens on 127.0.0.1 only, on exactly the expected ports
  local n=$1; shift
  local pid exe; pid=$(cat "${PIDFILE[$n]}" 2>/dev/null) || refuse "$n: no pid file"
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || refuse "$n: not running"
  [ "$exe" = "${EXE[$n]}" ] || refuse "$n: runs $exe, expected ${EXE[$n]}"
  local listen; listen=$(ss -ltnp | awk -v p="pid=$pid," 'index($0, p) {print $4}' | sort -u)
  for p in "$@"; do echo "$listen" | grep -qx "127.0.0.1:$p" || refuse "$n: 127.0.0.1:$p is not listened by pid $pid"; done
  local other; other=$(echo "$listen" | grep -v '^127\.0\.0\.1:' || true)
  [ -z "$other" ] || refuse "$n: non-loopback listener(s): $other"
  local extra; extra=$(echo "$listen" | sed 's/^127\.0\.0\.1://' | grep -vxF -f <(printf '%s\n' "$@") || true)
  [ -z "$extra" ] || refuse "$n: unexpected port(s): $extra"
  log "proved $n: pid $pid exe $exe listens only on $(echo $listen)"
}

start_keeper() {
  local D=$ISO/keeper; mkdir -p "$D"/coordination/{log,snapshots}
  cat > "$D/keeper.xml" <<XML
<clickhouse>
    <logger><level>information</level><log>$D/keeper.log</log><errorlog>$D/keeper.err.log</errorlog></logger>
    <listen_host>127.0.0.1</listen_host>
    <interserver_listen_host>127.0.0.1</interserver_listen_host>
    <path>$D/</path>
    <keeper_server>
        <tcp_port>$KP</tcp_port>
        <server_id>1</server_id>
        <log_storage_path>$D/coordination/log</log_storage_path>
        <snapshot_storage_path>$D/coordination/snapshots</snapshot_storage_path>
        <coordination_settings><operation_timeout_ms>10000</operation_timeout_ms><session_timeout_ms>30000</session_timeout_ms><raft_logs_level>warning</raft_logs_level></coordination_settings>
        <raft_configuration><server><id>1</id><hostname>127.0.0.1</hostname><port>$RAFT</port></server></raft_configuration>
    </keeper_server>
</clickhouse>
XML
  PIDFILE[keeper]=$D/keeper.pid; EXE[keeper]=$KBIN
  (cd "$D" && exec nohup "$KBIN" --config-file="$D/keeper.xml" --pid-file="$D/keeper.pid" > "$D/stdout.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
  local ok=0
  for _ in $(seq 1 60); do
    [ "$(timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/$KP; printf ruok >&3; timeout 2 cat <&3" 2>/dev/null)" = imok ] && { ok=1; break; }
    sleep 1
  done
  [ $ok = 1 ] || refuse "keeper did not answer ruok (see $D)"
  prove keeper $KP $RAFT
  local srvr; srvr=$(timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/$KP; printf srvr >&3; timeout 2 cat <&3" 2>/dev/null | head -1)
  step keeper.start KEEPER 0 OK "$srvr"
}

start_server() {  # <r1|r2> <binary copy> <role label> <step name>
  local n=$1 bin=$2 role=$3 sname=$4 D=$ISO/$1
  mkdir -p "$D"/{data,tmp,log,user_files,format_schemas}
  local merge_tree=""
  # r1 never merges by itself: it downloads the parts that r2 merges (with r2 on the candidate: candidate-written merges)
  [ "$n" = r1 ] && merge_tree="<merge_tree><always_fetch_merged_part>1</always_fetch_merged_part></merge_tree>"
  cat > "$D/config.xml" <<XML
<clickhouse>
    <logger><level>information</level><log>$D/log/server.log</log><errorlog>$D/log/server.err.log</errorlog><size>200M</size><count>2</count></logger>
    <listen_host>127.0.0.1</listen_host>
    <interserver_listen_host>127.0.0.1</interserver_listen_host>
    <tcp_port>${TCP[$n]}</tcp_port><http_port>${HTTP[$n]}</http_port>
    <interserver_http_port>${ISP[$n]}</interserver_http_port><interserver_http_host>127.0.0.1</interserver_http_host>
    <path>$D/data/</path><tmp_path>$D/tmp/</tmp_path><user_files_path>$D/user_files/</user_files_path>
    <format_schema_path>$D/format_schemas/</format_schema_path>
    <user_directories><users_xml><path>$D/users.xml</path></users_xml></user_directories>
    <default_profile>default</default_profile><default_database>default</default_database>
    <max_server_memory_usage_to_ram_ratio>0.15</max_server_memory_usage_to_ram_ratio>
    <memory_worker_use_cgroup>0</memory_worker_use_cgroup><memory_worker_dynamic_hard_limit>0</memory_worker_dynamic_hard_limit>
    <zookeeper><node><host>127.0.0.1</host><port>$KP</port></node><session_timeout_ms>30000</session_timeout_ms></zookeeper>
    <macros><shard>1</shard><replica>$n</replica></macros>
    <part_log><database>system</database><table>part_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></part_log>
    $merge_tree
</clickhouse>
XML
  cat > "$D/users.xml" <<'XML'
<clickhouse>
    <profiles><default><max_memory_usage>4000000000</max_memory_usage></default></profiles>
    <users><default><password></password><networks><ip>127.0.0.1</ip></networks><profile>default</profile><quota>default</quota></default></users>
    <quotas><default></default></quotas>
</clickhouse>
XML
  PIDFILE[$n]=$D/server.pid; EXE[$n]=$bin
  (cd "$D" && exec nohup "$bin" server --config-file="$D/config.xml" --pid-file="$D/server.pid" > "$D/log/stdout.$role.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
  local ok=0
  for _ in $(seq 1 120); do timeout 10 "$BBIN" client --host 127.0.0.1 --port "${TCP[$n]}" --query "SELECT 1" < /dev/null > /dev/null 2>&1 && { ok=1; break; }; sleep 1; done
  [ $ok = 1 ] || refuse "$n ($role) did not become ready (see $D/log)"
  prove "$n" "${TCP[$n]}" "${HTTP[$n]}" "${ISP[$n]}"
  local v; v=$(q "$n" "SELECT version() || ' ' || buildId()")
  step "$sname" "$role" 0 OK "$v"
}

q() {  # <r1|r2> <query>: one client binary (the baseline) formats every result, so dumps compare data only
  timeout 330 "$BBIN" client --host 127.0.0.1 --port "${TCP[$1]}" --receive_timeout 300 --send_timeout 300 --query "$2" < /dev/null
}
TABLES="t_wide t_compact"
dump() {  # <replica> <label>: sha256 of every table, ordered, as TSV
  local f=$OUT/dumps/$2.$1.tsv
  : > "$f"
  for t in $TABLES; do
    echo "## $t" >> "$f"
    q "$1" "SELECT id, d, dn, i, u, v, dynamicType(v), finalizeAggregation(s) FROM repl.$t ORDER BY id FORMAT TSV" >> "$f" || return 1
  done
  sha256sum "$f" | cut -d' ' -f1
}
compare() {  # <step> <actor>: both replicas hold exactly the same rows
  local a b rc=0 rows
  a=$(dump r1 "$1") || rc=1
  b=$(dump r2 "$1") || rc=1
  rows=$(grep -vc '^##' "$OUT/dumps/$1.r1.tsv" 2>/dev/null)
  if [ $rc != 0 ]; then step "$1" "$2" 1 ERROR "a dump failed"; return; fi
  if [ "$a" = "$b" ] && [ "${rows:-0}" -gt 0 ]; then step "$1" "$2" 0 SAME "$rows rows, sha256 ${a:0:16}"
  else diff "$OUT/dumps/$1.r1.tsv" "$OUT/dumps/$1.r2.tsv" > "$OUT/dumps/$1.diff" 2>&1; step "$1" "$2" 0 DIFF "r1 ${a:0:16} r2 ${b:0:16} rows $rows (dumps/$1.diff)"; fi
}
sync_replica() {  # <replica>
  local rc=0
  for t in $TABLES; do q "$1" "SYSTEM SYNC REPLICA repl.$t" > /dev/null 2>> "$OUT/logs/sync.err" || rc=1; done
  return $rc
}
wait_mutations() {  # <replica>...: every mutation done (bounded)
  for r in "$@"; do
    for _ in $(seq 1 150); do
      [ "$(q "$r" "SELECT count() FROM system.mutations WHERE database = 'repl' AND table IN ('t_wide', 't_compact') AND NOT is_done")" = 0 ] && continue 2
      sleep 2
    done
    return 1
  done
}
insert_batch() {  # <replica> <offset> <rows>
  local rc=0
  for t in $TABLES; do
    q "$1" "INSERT INTO repl.$t SELECT number + $2 AS id,
        toDecimal512(concat(toString(number + $2), '.', leftPad(toString((number * 7919) % 1000000000000000000), 18, '0')), 18) AS d,
        if(number % 5 = 0, NULL, toDecimal512(concat(if(number % 2 = 0, '-', ''), '0.', leftPad(toString(number * 104729), 60, '0')), 60)) AS dn,
        toInt512(concat(if(number % 2 = 0, '-', ''), toString(number + $2), repeat('7', number % 100))) AS i,
        toUInt512(concat(toString(number + $2), repeat('3', number % 120))) AS u,
        multiIf(number % 3 = 0, CAST(toInt512(concat('-', toString(number + $2))) AS Dynamic), number % 3 = 1, CAST(d AS Dynamic), CAST(toUInt512(number) AS Dynamic)) AS v,
        initializeAggregation('sumState', d) AS s
      FROM numbers($3)" || rc=1
  done
  return $rc
}
EXTREMES=$(python3 - <<'EOF'
imax, imin, umax = 2**511 - 1, -2**511, 2**512 - 1
# d is one unit (10^18 scaled) inside the Int512 range: a mutation evaluates d + 0.5 on every row of a part, whatever
# its WHERE says, so d = max would make it overflow on both builds; i, u, v and dn keep the exact extremes
dmax, dmin = imax - 10**18, imin + 10**18
def fmt(v, s):
    neg = v < 0; t = str(abs(v)).rjust(s + 1, "0"); return ("-" if neg else "") + t[:-s] + "." + t[-s:]
print(f"(1000000000, CAST('{fmt(dmax, 18)}' AS Decimal(154, 18)), CAST('{fmt(imin, 60)}' AS Decimal(154, 60)), toInt512('{imin}'), toUInt512('{umax}'), CAST(toInt512('{imax}') AS Dynamic), initializeAggregation('sumState', CAST('{fmt(dmax, 18)}' AS Decimal(154, 18)))),"
      f"(1000000001, CAST('{fmt(dmin, 18)}' AS Decimal(154, 18)), NULL, toInt512('{imax}'), toUInt512('0'), CAST(toUInt512('{umax}') AS Dynamic), initializeAggregation('sumState', CAST('{fmt(dmin, 18)}' AS Decimal(154, 18))))")
EOF
)

#### the run
start_keeper
start_server r1 "$BBIN" baseline r1.start.baseline
start_server r2 "$BBIN" baseline r2.start.baseline
rc=0
for n in r1 r2; do
  q "$n" "CREATE DATABASE IF NOT EXISTS repl" || rc=1
  for t in $TABLES; do
    wide=""; [ "$t" = t_wide ] && wide="SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0"
    q "$n" "CREATE TABLE repl.$t (id UInt64, d Decimal(154, 18), dn Nullable(Decimal(154, 60)), i Int512, u UInt512, v Dynamic, s AggregateFunction(sum, Decimal(154, 18)))
            ENGINE = ReplicatedMergeTree('/clickhouse/tables/repl/$t', '{replica}') ORDER BY id $wide" || rc=1
  done
  q "$n" "CREATE TABLE repl.t_risk (id UInt64, i Int512) ENGINE = ReplicatedMergeTree('/clickhouse/tables/repl/t_risk', '{replica}') ORDER BY id" || rc=1
done
step create both $rc "$([ $rc = 0 ] && echo OK || echo ERROR)" "databases and tables on r1 and r2"

insert_batch r1 0 3000; irc=$?
for t in $TABLES; do q r1 "INSERT INTO repl.$t VALUES $EXTREMES" || irc=1; done
sync_replica r2 || irc=1
[ $irc = 0 ] || step insert.baseline_to_baseline baseline 1 ERROR "insert or sync failed"
compare fetch.baseline_to_baseline baseline

stop_one r2; step r2.stop.for_upgrade baseline 0 OK "rolling upgrade: r2 stops on the baseline"
start_server r2 "$CBIN" candidate r2.start.candidate
sync_replica r2; compare read.after_upgrade candidate

insert_batch r1 10000 2000; x=$?; sync_replica r2 || x=1
[ $x = 0 ] || step fetch.baseline_to_candidate.insert baseline 1 ERROR "insert on r1 or sync on r2 failed"
compare fetch.baseline_to_candidate baseline

insert_batch r2 20000 2000; x=$?; sync_replica r1 || x=1
[ $x = 0 ] || step fetch.candidate_to_baseline.insert candidate 1 ERROR "insert on r2 or sync on r1 failed"
compare fetch.candidate_to_baseline candidate

x=0
for t in $TABLES; do q r1 "ALTER TABLE repl.$t UPDATE dn = NULL, d = d + CAST('0.5' AS Decimal(154, 18)) WHERE id % 7 = 0" || x=1; done
wait_mutations r1 r2 || x=1; sync_replica r1; sync_replica r2
[ $x = 0 ] || step mutation.baseline_initiated.run baseline 1 ERROR "mutation failed or did not finish on both replicas"
compare mutation.baseline_initiated baseline

x=0
for t in $TABLES; do q r2 "ALTER TABLE repl.$t DELETE WHERE id % 11 = 0" || x=1; q r2 "ALTER TABLE repl.$t UPDATE u = toUInt512('123') WHERE id % 13 = 0" || x=1; done
wait_mutations r1 r2 || x=1; sync_replica r1; sync_replica r2
[ $x = 0 ] || step mutation.candidate_initiated.run candidate 1 ERROR "mutation failed or did not finish on both replicas"
compare mutation.candidate_initiated candidate

x=0
for t in $TABLES; do q r2 "OPTIMIZE TABLE repl.$t FINAL" || x=1; done
sync_replica r1 || x=1; sync_replica r2 || x=1
parts=$(for n in r1 r2; do echo -n "$n:$(q "$n" "SELECT groupArray(concat(table, '=', toString(c))) FROM (SELECT table, count() AS c FROM system.parts WHERE database = 'repl' AND table IN ('t_wide', 't_compact') AND active GROUP BY table ORDER BY table)") "; done)
[ $x = 0 ] || step merge.candidate.run candidate 1 ERROR "optimize or sync failed"
compare merge.candidate_fetched_by_baseline candidate
# the baseline replica must have downloaded parts that the candidate merged (level > 0 in the part name), not merged itself
q r1 "SYSTEM FLUSH LOGS" > /dev/null 2>&1
fetched=$(q r1 "SELECT count() FROM system.part_log WHERE database = 'repl' AND event_type = 'DownloadPart' AND toUInt32OrZero(splitByChar('_', part_name)[4]) > 0 AND event_time >= now() - INTERVAL 10 MINUTE" 2>&1)
# (a MergeParts record with an error is r1 waiting for r2's result: "No active replica has part ..." (234), not a merge)
merged_by_r1=$(q r1 "SELECT count() FROM system.part_log WHERE database = 'repl' AND event_type = 'MergeParts' AND error = 0" 2>&1)
if [ "${fetched:-0}" -gt 0 ] 2>/dev/null && [ "$merged_by_r1" = 0 ]; then step merge.downloaded_by_baseline baseline 0 OK "r1 downloaded $fetched merged part(s), merged none itself; active parts $parts"
else step merge.downloaded_by_baseline baseline 1 NOT_FETCHED "r1 downloads of merged parts: $fetched, merges by r1: $merged_by_r1; active parts $parts"; fi

# informational: a mutation that only the candidate can execute, on its own table, while r1 runs the baseline
q r1 "INSERT INTO repl.t_risk SELECT number, toInt512(number) FROM numbers(10)" > /dev/null 2>&1; sync_replica r2 > /dev/null 2>&1
q r2 "SYSTEM SYNC REPLICA repl.t_risk" > /dev/null 2>&1
rk=$(q r2 "ALTER TABLE repl.t_risk UPDATE i = i + toInt512(1) WHERE id = 1" 2>&1); rrc=$?; rk=${rk:0:200}
sleep 20
r1state=$(q r1 "SELECT concat('is_done=', toString(is_done), ' fail=', latest_fail_reason) FROM system.mutations WHERE table = 't_risk' ORDER BY create_time DESC LIMIT 1" 2>&1 | head -c 300)
r2state=$(q r2 "SELECT concat('is_done=', toString(is_done)) FROM system.mutations WHERE table = 't_risk' ORDER BY create_time DESC LIMIT 1" 2>&1 | head -c 100)
step info.candidate_only_mutation candidate "$rrc" INFO "alter: ${rk:-accepted}; r1(baseline): $r1state; r2(candidate): $r2state"
q r2 "KILL MUTATION WHERE database = 'repl' AND table = 't_risk'" > /dev/null 2>&1

stop_one r2; step r2.stop.for_rollback candidate 0 OK "rollback: r2 stops on the candidate"
start_server r2 "$BBIN" baseline r2.rollback.baseline
sync_replica r2; compare read.after_rollback baseline

insert_batch r2 30000 1000; x=$?; insert_batch r1 40000 1000 || x=1; sync_replica r1 || x=1; sync_replica r2 || x=1
[ $x = 0 ] || step replication.after_rollback.insert baseline 1 ERROR "insert or sync failed after the rollback"
compare replication.after_rollback baseline

cleanup; trap - EXIT
rm -f "$ISO"/bin/*
for n in r1 r2; do grep -c '<Error>' "$ISO/$n/log/server.err.log" 2>/dev/null | sed "s/^/$n server.err.log <Error> lines: /" >> "$OUT/logs/run.log"; done
REQUIRED="keeper.start r1.start.baseline r2.start.baseline create fetch.baseline_to_baseline r2.start.candidate read.after_upgrade fetch.baseline_to_candidate fetch.candidate_to_baseline mutation.baseline_initiated mutation.candidate_initiated merge.candidate_fetched_by_baseline merge.downloaded_by_baseline r2.rollback.baseline read.after_rollback replication.after_rollback"
fail=0
for s in $REQUIRED; do
  awk -F'\t' -v s="$s" '$1 == s && $3 == 0 && ($4 == "SAME" || $4 == "OK") {f = 1} END {exit !f}' "$OUT/steps.tsv" || { log "required step $s missing or not OK"; fail=1; }
done
awk -F'\t' 'NR > 1 && ($4 == "ERROR" || $4 == "DIFF" || $4 == "REFUSED" || $4 == "NOT_FETCHED") {f = 1} END {exit !f}' "$OUT/steps.tsv" && fail=1
log "mixed replication: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
