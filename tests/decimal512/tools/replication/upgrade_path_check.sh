#!/usr/bin/env bash
# upgrade_path_check.sh - isolated acceptance test of the upgrade PATH for KD-D512-MIXED-GROUPBY-NULLABLE: version groups
# switched together with the application's addresses, a server-side fence (remote_url_allow_hosts per group) that makes
# any cross-group fan-out fail closed, fault injection and a rollback. It does not fix or waive the defect: a query that
# spans both builds stays wrong (mixed-groupby keeps failing); this checks that the procedure never lets one happen.
#
# Production model (k8s-sea, 2026-09-24): every ClickHouse cluster is 1 shard x 2 replicas; the application picks a cluster
# per tier/org/chain and connects to its address; cross-cluster reads (timeseries SQL, reserved MVs, the MV refresher)
# are remote('<address of another cluster>', db, table) executed by the node the application is connected to. Today the
# addresses are cluster Services that spread connections over both replicas (emulated by rr_proxy.py).
# Test topology (all on 127.0.0.1, one Keeper): cluster X = x0, x1 and cluster Y = y0, y1 (ReplicatedMergeTree app.events,
# the key shapes of groupby_oracle.py); version group g0 = {x0, y0} (replica 0), g1 = {x1, y1} (replica 1).
# Application configurations (each app process uses one of them as a whole: entry of X and remote() target in Y):
#   svc  entry = cluster Service of X, target = cluster Service of Y (today)
#   g0   entry = x0, target = y0            g1   entry = x1, target = y1
# Procedure under test and checks per phase (7 key shapes x single-/two-level; fan-out, local, and write (CTAS) paths):
#   P0 all old, svc                         P1 all old, g0 (pin)          P2 fence on (config reload, no restart)
#   P3 upgrade g1 (x1, y1 restarted on the candidate; replicas compared)
#   P4 rolling switch: g0 and g1 workloads interleaved (both configs live at once); F1 stop y1 -> g1 fan-out must fail,
#      never be served by y0; F2 misrouted configs -> rejected by the fence; F3 Keeper restart -> replication resumes
#   R1 rollback: app back to g0, g1 downgraded to the baseline; replicas compared; R2 g1 upgraded again
#   N1 negative control (fence off on x1, target = cluster Service of Y): mixed-build queries MUST be detected
#   P5 app on g1, upgrade g0 (x0, y0); P6 all new, fence off, svc -> must equal the oracle
# Span detector: after each phase every server's query_log is read (SYSTEM FLUSH LOGS on these test servers); a query
# (initial_query_id) whose parts ran on servers of two builds is a span. Acceptance (exit 0): no span outside N1, spans
# found in N1; every candidate-only result equals the oracle; every fenced or failed-over query fails with no result;
# replicas identical after every restart; the fence rejects every disallowed target. Old-only results are recorded as
# the baseline's own behaviour (it groups these keys wrongly: KD-KEYSNULLMAP-512), not judged. 1 otherwise, 2 usage/isolation.
#
# PRODUCTION BOUNDARY: talks only to the processes it starts (127.0.0.1, ports 38300-38470, refused when busy; every
# server proven to run its own copy and to listen on loopback only). SYSTEM, CREATE, INSERT and config changes go only
# to these test servers. It validates a procedure; it changes no deployment and approves no rollout.
#
#   upgrade_path_check.sh <out-dir> --keeper <clickhouse-keeper> --baseline <old clickhouse> --candidate <new clickhouse>
set -u
export GIT_NO_LAZY_FETCH=1 GIT_OPTIONAL_LOCKS=0
HERE=$(cd "$(dirname "$0")" && pwd)
ORACLE_DIR=${GROUPBY_ORACLE_DIR:-$HERE}
LOCK=${GROUPBY_LOCK:-$ORACLE_DIR/groupby_cases.lock.json}
PROXY=${RR_PROXY:-$HERE/rr_proxy.py}
usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
[ $# -ge 1 ] || usage
OUT=$1; shift
KEEPER= BASE= CAND=
while [ $# -gt 0 ]; do
  case $1 in --keeper) KEEPER=$2; shift 2;; --baseline) BASE=$2; shift 2;; --candidate) CAND=$2; shift 2;; *) usage;; esac
done
for b in "$KEEPER" "$BASE" "$CAND"; do [ -n "$b" ] && [ -x "$b" ] || { echo "ERROR: --keeper, --baseline and --candidate must be executables" >&2; exit 2; }; done
[ -f "$PROXY" ] && [ -f "$LOCK" ] || { echo "ERROR: rr_proxy.py or the case lock is missing" >&2; exit 2; }
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: $OUT is not empty" >&2; exit 2; fi
mkdir -p "$OUT"/{iso/bin,logs,results,dumps}
OUT=$(cd "$OUT" && pwd); ISO=$OUT/iso
python3 "$ORACLE_DIR/groupby_oracle.py" check "$LOCK" > "$OUT/logs/lock_check.txt" 2>&1 || { echo "ERROR: lock does not match the oracle" >&2; exit 2; }

NODES="x0 x1 y0 y1"
KP=38381; RAFT=38434; SVC_X=38450; SVC_Y=38460
declare -A TCP=([x0]=38300 [x1]=38310 [y0]=38320 [y1]=38330) CLUSTER=([x0]=X [x1]=X [y0]=Y [y1]=Y) GROUP=([x0]=g0 [y0]=g0 [x1]=g1 [y1]=g1)
declare -A HTTP ISP
for n in $NODES; do HTTP[$n]=$((TCP[$n] + 1)); ISP[$n]=$((TCP[$n] + 2)); done
for p in $KP $RAFT $SVC_X $SVC_Y $(for n in $NODES; do echo ${TCP[$n]} ${HTTP[$n]} ${ISP[$n]}; done); do
  ss -ltn | awk '{print $4}' | grep -qE ":$p\$" && { echo "REFUSE: port $p is busy" >&2; exit 2; }
done
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$OUT/logs/run.log" >&2; }
printf 'phase\tcheck\tconfig\tshape\tvariant\toutcome\tdetail\tresult_sha256\n' > "$OUT/results/checks.tsv"
rec() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" "$6" "$(echo "${7:-}" | tr '\t\n' '  ' | cut -c1-240)" "${8:--}" >> "$OUT/results/checks.tsv"; }

place() {  # <role> <binary>: hard link (same file system) or copy under iso/bin
  local src; src=$(readlink -f "$2"); local bid; bid=$(readelf -n "$src" 2>/dev/null | awk '/Build ID/{print substr($3,1,12)}')
  local dst=$ISO/bin/$1-${bid:-nobuildid}; ln "$src" "$dst" 2>/dev/null || cp "$src" "$dst"; echo "$dst"
}
KBIN=$(place keeper "$KEEPER"); BBIN=$(place baseline "$BASE"); CBIN=$(place candidate "$CAND")
BID_B=$(readelf -n "$BBIN" | awk '/Build ID/{print $3}'); BID_C=$(readelf -n "$CBIN" | awk '/Build ID/{print $3}')
[ "$BID_B" != "$BID_C" ] || { echo "ERROR: baseline and candidate are the same build" >&2; exit 2; }
printf 'role\tpath\tsha256\tbuild_id\n' > "$OUT/identities.tsv"
for rb in "KEEPER $KBIN $KEEPER" "BASELINE $BBIN $BASE" "CANDIDATE $CBIN $CAND"; do set -- $rb
  printf '%s\t%s\t%s\t%s\n' "$1" "$(readlink -f "$3")" "$(sha256sum "$2" | cut -d' ' -f1)" "$(readelf -n "$2" | awk '/Build ID/{print $3}')" >> "$OUT/identities.tsv"
done

declare -A PIDFILE EXE BUILD
stop_one() {
  local pf=${PIDFILE[$1]:-} pid exe
  [ -n "$pf" ] && pid=$(cat "$pf" 2>/dev/null) || return 0
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 0
  case "$exe" in "$ISO"/bin/*|*/python3*) kill "$pid"; for _ in $(seq 1 90); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
                                 kill -0 "$pid" 2>/dev/null && kill -9 "$pid";;
                 *) log "REFUSE to stop pid $pid ($exe): not ours";; esac
}
cleanup() { for n in svc_x svc_y y1 y0 x1 x0 keeper; do stop_one "$n"; done; }
trap cleanup EXIT
refuse() { log "REFUSE: $*"; rec isolation - - - - REFUSED "$*"; exit 2; }
prove() {  # <name> <ports...>
  local n=$1; shift; local pid exe; pid=$(cat "${PIDFILE[$n]}" 2>/dev/null) || refuse "$n: no pid file"
  exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || refuse "$n: not running"
  [ "$exe" = "${EXE[$n]}" ] || refuse "$n: runs $exe, expected ${EXE[$n]}"
  local listen; listen=$(ss -ltnp | awk -v p="pid=$pid," 'index($0, p) {print $4}' | sort -u)
  for p in "$@"; do echo "$listen" | grep -qx "127.0.0.1:$p" || refuse "$n: 127.0.0.1:$p is not listened by pid $pid"; done
  local other; other=$(echo "$listen" | grep -v '^127\.0\.0\.1:' || true); [ -z "$other" ] || refuse "$n: non-loopback listener(s): $other"
}
start_keeper() {
  local D=$ISO/keeper; mkdir -p "$D"/coordination/{log,snapshots}
  [ -f "$D/keeper.xml" ] || cat > "$D/keeper.xml" <<XML
<clickhouse>
    <logger><level>information</level><log>$D/keeper.log</log><errorlog>$D/keeper.err.log</errorlog></logger>
    <listen_host>127.0.0.1</listen_host><interserver_listen_host>127.0.0.1</interserver_listen_host><path>$D/</path>
    <keeper_server><tcp_port>$KP</tcp_port><server_id>1</server_id>
        <log_storage_path>$D/coordination/log</log_storage_path><snapshot_storage_path>$D/coordination/snapshots</snapshot_storage_path>
        <coordination_settings><operation_timeout_ms>10000</operation_timeout_ms><session_timeout_ms>30000</session_timeout_ms><raft_logs_level>warning</raft_logs_level></coordination_settings>
        <raft_configuration><server><id>1</id><hostname>127.0.0.1</hostname><port>$RAFT</port></server></raft_configuration>
    </keeper_server>
</clickhouse>
XML
  PIDFILE[keeper]=$D/keeper.pid; EXE[keeper]=$KBIN
  (cd "$D" && exec nohup "$KBIN" --config-file="$D/keeper.xml" --pid-file="$D/keeper.pid" > "$D/stdout.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
  for _ in $(seq 1 60); do
    [ "$(timeout 5 bash -c "exec 3<>/dev/tcp/127.0.0.1/$KP; printf ruok >&3; timeout 2 cat <&3" 2>/dev/null)" = imok ] && { prove keeper $KP $RAFT; return 0; }
    sleep 1
  done
  refuse "keeper did not answer ruok"
}
start_server() {  # <node> <binary copy>
  local n=$1 bin=$2 D=$ISO/$1; mkdir -p "$D"/{data,tmp,log,user_files,config.d}
  cat > "$D/config.xml" <<XML
<clickhouse>
    <logger><level>information</level><log>$D/log/server.log</log><errorlog>$D/log/server.err.log</errorlog><size>200M</size><count>2</count></logger>
    <listen_host>127.0.0.1</listen_host><interserver_listen_host>127.0.0.1</interserver_listen_host>
    <tcp_port>${TCP[$n]}</tcp_port><http_port>${HTTP[$n]}</http_port>
    <interserver_http_port>${ISP[$n]}</interserver_http_port><interserver_http_host>127.0.0.1</interserver_http_host>
    <path>$D/data/</path><tmp_path>$D/tmp/</tmp_path><user_files_path>$D/user_files/</user_files_path>
    <user_directories><users_xml><path>$D/users.xml</path></users_xml></user_directories>
    <default_profile>default</default_profile><default_database>default</default_database>
    <max_server_memory_usage_to_ram_ratio>0.1</max_server_memory_usage_to_ram_ratio>
    <memory_worker_use_cgroup>0</memory_worker_use_cgroup><memory_worker_dynamic_hard_limit>0</memory_worker_dynamic_hard_limit>
    <background_schedule_pool_size>16</background_schedule_pool_size>
    <zookeeper><node><host>127.0.0.1</host><port>$KP</port></node><session_timeout_ms>30000</session_timeout_ms></zookeeper>
    <macros><cluster>${CLUSTER[$n]}</cluster><shard>1</shard><replica>$n</replica></macros>
    <remote_servers>
        <all_nodes>$(for m in $NODES; do echo "<shard><replica><host>127.0.0.1</host><port>${TCP[$m]}</port></replica></shard>"; done)</all_nodes>
    </remote_servers>
    <query_log><database>system</database><table>query_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></query_log>
</clickhouse>
XML
  [ -f "$D/users.xml" ] || cat > "$D/users.xml" <<'XML'
<clickhouse>
    <profiles><default><max_memory_usage>3000000000</max_memory_usage><log_queries>1</log_queries></default></profiles>
    <users><default><password></password><networks><ip>127.0.0.1</ip></networks><profile>default</profile><quota>default</quota></default></users>
    <quotas><default></default></quotas>
</clickhouse>
XML
  PIDFILE[$n]=$D/server.pid; EXE[$n]=$bin
  (cd "$D" && exec nohup "$bin" server --config-file="$D/config.xml" --pid-file="$D/server.pid" > "$D/log/stdout.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
  for _ in $(seq 1 120); do timeout 10 "$CBIN" client --host 127.0.0.1 --port "${TCP[$n]}" --query "SELECT 1" < /dev/null > /dev/null 2>&1 && break; sleep 1; done
  prove "$n" "${TCP[$n]}" "${HTTP[$n]}" "${ISP[$n]}"
  BUILD[$n]=$(q "$n" "SELECT lower(buildId())")
  local want_bid=$BID_B; [ "$bin" = "$CBIN" ] && want_bid=$BID_C
  [[ ${BUILD[$n]} == ${want_bid:0:12}* ]] || refuse "$n reports build ${BUILD[$n]}, expected ${want_bid:0:12}"
  log "started $n ($(basename "$bin")) build ${BUILD[$n]:0:12}"
}
start_proxy() {  # <name> <port> <backend ports...>
  local n=$1 p=$2; shift 2
  PIDFILE[$n]=$ISO/$n.pid; EXE[$n]=$(readlink -f "$(command -v python3)")
  (exec nohup python3 "$PROXY" "$p" "$ISO/$n.pid" "$OUT/logs/$n.connections.tsv" "$@" > "$OUT/logs/$n.log" 2>&1 < /dev/null) > /dev/null 2>&1 &
  for _ in $(seq 1 30); do ss -ltn | awk '{print $4}' | grep -qx "127.0.0.1:$p" && return 0; sleep 0.5; done
  refuse "proxy $n did not listen on $p"
}
q() {  # <node> <query> [extra client args]: the candidate client formats every result
  local n=$1 sql=$2; shift 2
  timeout 330 "$CBIN" client --host 127.0.0.1 --port "${TCP[$n]}" --receive_timeout 300 "$@" --query "$sql" < /dev/null
}
qp() {  # <port> <query>
  timeout 330 "$CBIN" client --host 127.0.0.1 --port "$1" --receive_timeout 300 --query "$2" < /dev/null
}

# the fence: a node may call remote() only on hosts of its own version group (reloaded without a restart)
fence_on() {
  local n=$1 D=$ISO/$1 hosts=""
  for m in $NODES; do [ "${GROUP[$m]}" = "${GROUP[$n]}" ] && hosts+="<host>127.0.0.1:${TCP[$m]}</host><host>localhost:${TCP[$m]}</host>"; done
  echo "<clickhouse><remote_url_allow_hosts>$hosts</remote_url_allow_hosts></clickhouse>" > "$D/config.d/zz_upgrade_fence.xml"
}
fence_off() { rm -f "$ISO/$1/config.d/zz_upgrade_fence.xml"; }
fence_wait() {  # <node> <on|off>: until the reloaded config shows the expected behaviour for a cross-group target
  local n=$1 want=$2 other=""
  for m in $NODES; do [ "${GROUP[$m]}" != "${GROUP[$n]}" ] && other=${TCP[$m]} && break; done
  for _ in $(seq 1 60); do
    local err; err=$(q "$n" "SELECT count() FROM remote('127.0.0.1:$other', system, one)" 2>&1 >/dev/null)
    if [ "$want" = on ]; then echo "$err" | grep -q UNACCEPTABLE_URL && return 0; else [ -z "$err" ] && return 0; fi
    sleep 1
  done
  return 1
}

declare -A EXPECT
while IFS=$'\t' read -r shape sha; do EXPECT[$shape]=$sha; done < <(python3 -c '
import json, sys
seen = {}
for c in json.load(open(sys.argv[1]))["cases"]:
    seen[c["shape"]] = c["expected_sha256"]
for s, h in seen.items(): print(f"{s}\t{h}")' "$LOCK")
declare -A KEYS=([nullable_u256_single]="k_nu256" [nullable_i64_x4]="k_ni64_1, k_ni64_2, k_ni64_3, k_ni64_4" [u256_u128_48b]="k_u256, k_u128"
                 [int512_single]="k_i512" [decimal512_single]="k_d512" [u64_nullable_d76]="k_u64, k_nd76" [u256_u64_40b]="k_u256, k_u64")
declare -A VARS=([single]="" [two_level]=", group_by_two_level_threshold = 1, group_by_two_level_threshold_bytes = 1")
# n1 (negative control only): entry x1, target = the cluster Service of Y (reaches both builds)
declare -A CFG_ENTRY=([svc]=$SVC_X [g0]=${TCP[x0]} [g1]=${TCP[x1]} [n1]=${TCP[x1]}) CFG_TARGET=([svc]=$SVC_Y [g0]=${TCP[y0]} [g1]=${TCP[y1]} [n1]=$SVC_Y)
SCHEMA="k_nu256 Nullable(UInt256), k_ni64_1 Nullable(Int64), k_ni64_2 Nullable(Int64), k_ni64_3 Nullable(Int64), k_ni64_4 Nullable(Int64),
        k_u256 UInt256, k_u128 UInt128, k_i512 Int512, k_d512 Decimal(154, 60), k_nd76 Nullable(Decimal(76, 30)), k_u64 UInt64, v UInt64"
WSEQ=0
# judged: "oracle" -> must equal the oracle; "record" -> the baseline's own result, recorded; "fail" -> must fail, no result
workload() {  # <phase> <config> <judged> [shape filter]
  local ph=$1 cfg=$2 judge=$3 only=${4:-}
  local entry=${CFG_ENTRY[$cfg]} target=${CFG_TARGET[$cfg]} shape var keys out err sha outcome
  for shape in "${!KEYS[@]}"; do
    [ -n "$only" ] && [ "$shape" != "$only" ] && continue
    keys=${KEYS[$shape]}
    for var in single two_level; do
      local tag="$ph|$cfg|$shape|$var"
      for path in fanout local write; do
        # a failed-over fan-out must fail; the local path of the entry node legitimately keeps working
        [ "$judge" = fail ] && [ $path = local ] && continue
        case $path in
          fanout) sqlq="SELECT $keys, count() AS c, sum(v) AS s FROM remote('127.0.0.1:$target', app, events) GROUP BY $keys ORDER BY $keys, c, s";;
          local)  sqlq="SELECT $keys, count() AS c, sum(v) AS s FROM app.events GROUP BY $keys ORDER BY $keys, c, s";;
          write)  WSEQ=$((WSEQ + 1)); wt="app.w_${ph}_${cfg}_${WSEQ}"
                  sqlq="CREATE TABLE $wt ENGINE = MergeTree ORDER BY tuple() AS SELECT $keys, count() AS c, sum(v) AS s FROM remote('127.0.0.1:$target', app, events) GROUP BY $keys";;
        esac
        err=$(mktemp)
        if [ $path = write ]; then  # create and read back over ONE connection: a cluster Service may route the next one elsewhere
          out=$(qp "$entry" "$sqlq SETTINGS log_comment = '$tag|$path'${VARS[$var]}; SELECT * FROM $wt ORDER BY ALL" 2> "$err")
        else
          out=$(qp "$entry" "$sqlq SETTINGS log_comment = '$tag|$path'${VARS[$var]}" 2> "$err")
        fi
        local rc=$?
        sha=$(printf '%s\n' "$out" | sha256sum | cut -c1-64)
        [ -z "$out" ] && sha=empty
        if [ "$judge" = fail ]; then
          if [ $rc != 0 ] && [ -z "$out" ]; then outcome=FAILED_CLOSED; else outcome=VIOLATION; fi
        elif [ $rc != 0 ]; then outcome=ERROR
        elif [ "$sha" = "${EXPECT[$shape]}" ]; then outcome=ORACLE
        elif [ "$judge" = record ]; then outcome=BASELINE_OWN
        else outcome=WRONG; fi
        [ $path = local ] && [ "$cfg" = svc ] && outcome="$outcome(svc)"
        rec "$ph" "$path" "$cfg" "$shape" "$var" "$outcome" "$(head -c 200 "$err")" "$sha"
        rm -f "$err"
      done
    done
  done
}
spans() {  # <phase label> <expect>: none = multi-node queries seen and none spans builds; none0 = none spans (zero multi-node
           # queries allowed, e.g. every fan-out failed); some = at least one spans builds (negative control)
  local ph=$1 want=$2 f=$OUT/results/querylog_$1.tsv
  : > "$f"
  for n in $NODES; do
    q "$n" "SYSTEM FLUSH LOGS" > /dev/null 2>&1 || continue
    q "$n" "SELECT '$n', lower(buildId()), initial_query_id, query_id, is_initial_query, type, exception_code, log_comment
            FROM system.query_log WHERE log_comment LIKE '$ph|%' AND type != 'QueryStart' FORMAT TSV" >> "$f" 2>/dev/null
  done
  python3 - "$f" "$OUT/results/spans_$ph.tsv" "$want" "$ph" <<'EOF' >> "$OUT/results/spans_summary.tsv"
import collections, sys
rows = [l.rstrip("\n").split("\t") for l in open(sys.argv[1]) if l.strip()]
by = collections.defaultdict(set); nodes = collections.defaultdict(set); tag = {}
for node, build, iq, qid, initial, typ, code, comment in rows:
    by[iq].add(build[:12]); nodes[iq].add(node); tag[iq] = comment
mixed = [iq for iq, b in by.items() if len(b) > 1]
with open(sys.argv[2], "w") as out:
    out.write("initial_query_id\tbuilds\tnodes\tlog_comment\n")
    for iq in sorted(by):
        out.write(f"{iq}\t{','.join(sorted(by[iq]))}\t{','.join(sorted(nodes[iq]))}\t{tag[iq]}\n")
multi = sum(1 for iq in by if len(nodes[iq]) > 1)
want = sys.argv[3]
ok = {"none": not mixed and multi > 0, "none0": not mixed, "some": bool(mixed)}[want]
print(f"{sys.argv[4]}\t{len(by)}\t{multi}\t{len(mixed)}\t{want}\t{'OK' if ok else 'FAIL'}")
EOF
  local last; last=$(tail -1 "$OUT/results/spans_summary.tsv")
  log "spans $ph: $(echo "$last" | awk -F'\t' '{print $2" queries, "$3" multi-node, "$4" spanning builds (want "$5") -> "$6}')"
}
compare_replicas() {  # <phase> <a> <b>: both replicas hold the same rows in app.events and app.ingest
  local ph=$1 a=$2 b=$3 ha hb t
  for t in events ingest; do
    q "$a" "SYSTEM SYNC REPLICA app.$t" > /dev/null 2>&1; q "$b" "SYSTEM SYNC REPLICA app.$t" > /dev/null 2>&1
    ha=$(q "$a" "SELECT * FROM app.$t ORDER BY ALL FORMAT TSV" | tee "$OUT/dumps/$ph.$a.tsv" | sha256sum | cut -c1-64)
    hb=$(q "$b" "SELECT * FROM app.$t ORDER BY ALL FORMAT TSV" | tee "$OUT/dumps/$ph.$b.tsv" | sha256sum | cut -c1-64)
    local rows; rows=$(wc -l < "$OUT/dumps/$ph.$a.tsv")
    if [ "$ha" = "$hb" ] && [ "$rows" -gt 0 ]; then rec "$ph" "replicas:$t" "$a=$b" - - SAME "$rows rows ${ha:0:16}"
    else rec "$ph" "replicas:$t" "$a=$b" - - DIFF "$a ${ha:0:16} $b ${hb:0:16}"; fi
    rm -f "$OUT/dumps/$ph.$a.tsv" "$OUT/dumps/$ph.$b.tsv"
  done
}
insert_more() {  # <node> <offset> [table]: more rows through one replica (replication carries them to the other group)
  q "$1" "INSERT INTO app.${3:-ingest} SELECT
      if(number % 4 = 0, NULL, toUInt256(number % 50) * toUInt256('340282366920938463463374607431768211456')),
      if(number % 5 = 0, NULL, number % 7), if(number % 6 = 0, NULL, number % 3), number % 2, if(number % 9 = 0, NULL, number % 5),
      toUInt256(number % 40), toUInt128(number % 3), toInt512(toInt64(number % 30) - 15), toDecimal512(toString(number % 25) || '.5', 60),
      if(number % 3 = 0, NULL, toDecimal256(toString(number % 11), 30)), number % 13, number
    FROM numbers($2, 100000)"
}
fence_reject() {  # <phase> <node> <target port> <label>: a disallowed remote() target must be refused with no result
  local out err rc; err=$(mktemp)
  out=$(q "$2" "SELECT count() FROM remote('127.0.0.1:$3', app, events) SETTINGS log_comment = '$1|fence|$4'" 2> "$err"); rc=$?
  if [ $rc != 0 ] && [ -z "$out" ] && grep -q UNACCEPTABLE_URL "$err"; then rec "$1" fence "$2->$3" "$4" - REJECTED "$(grep -o 'Code: [0-9]*[^.]*' "$err" | head -1)"
  else rec "$1" fence "$2->$3" "$4" - VIOLATION "rc=$rc out=$out $(head -c 160 "$err")"; fi
  rm -f "$err"
}
restart_on() {  # <node> <binary copy>
  stop_one "$1"; start_server "$1" "$2"
}

#### run
log "keeper $KBIN, baseline ${BID_B:0:12}, candidate ${BID_C:0:12}"
start_keeper
for n in $NODES; do start_server "$n" "$BBIN"; done
start_proxy svc_x $SVC_X ${TCP[x0]} ${TCP[x1]}
start_proxy svc_y $SVC_Y ${TCP[y0]} ${TCP[y1]}
for n in $NODES; do
  # app.events: exactly the oracle's data set (never written after the start); app.ingest: replication traffic of the window
  q "$n" "CREATE DATABASE app" && q "$n" "CREATE TABLE app.events ($SCHEMA) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{cluster}/events', '{replica}') ORDER BY tuple()" \
    && q "$n" "CREATE TABLE app.ingest ($SCHEMA) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{cluster}/ingest', '{replica}') ORDER BY tuple()" || refuse "create on $n"
done
insert_more x0 0 events && insert_more x0 100000 events && insert_more y0 0 events && insert_more y0 100000 events || refuse "initial insert"
insert_more x0 0 && insert_more y0 0 || refuse "initial ingest"
compare_replicas P0 x0 x1; compare_replicas P0 y0 y1

log "P0: all old, application on cluster Services"
workload P0 svc record; spans P0 none
log "P1: pin the application to group g0"
workload P1 g0 record; spans P1 none
log "P2: fence on every node (config reload, no restart)"
for n in $NODES; do fence_on "$n"; done
for n in $NODES; do fence_wait "$n" on || rec P2 fence "$n" - - NOT_APPLIED "fence not active after 60 s"; done
workload P2 g0 record
fence_reject P2 x0 $SVC_Y cluster_service; fence_reject P2 x0 ${TCP[y1]} other_group
spans P2 none
log "P3: upgrade g1 (x1, y1) while g0 serves"
restart_on x1 "$CBIN"; restart_on y1 "$CBIN"
insert_more x0 200000; compare_replicas P3 x0 x1; compare_replicas P3 y0 y1
workload P3 g0 record; spans P3 none
log "P4: rolling switch, both application configurations live"
workload P4 g1 oracle & W1=$!
workload P4b g0 record
wait $W1
fence_reject P4 x1 ${TCP[y0]} other_group; fence_reject P4 x0 ${TCP[y1]} other_group; fence_reject P4 x1 $SVC_Y cluster_service
# a merge done by the candidate that the baseline replica must take over (part fetch or its own merge), then compare
q x1 "OPTIMIZE TABLE app.ingest FINAL" && rec P4 merge_on_candidate x1 - - OK "" || rec P4 merge_on_candidate x1 - - ERROR ""
compare_replicas P4 x0 x1
spans P4 none; spans P4b none
log "F1: y1 down -> g1 fan-out must fail closed (never served by y0)"
stop_one y1
workload F1 g1 fail nullable_u256_single
start_server y1 "$CBIN"; fence_on y1; fence_wait y1 on || rec F1 fence y1 - - NOT_APPLIED ""
compare_replicas F1 y0 y1
spans F1 none0
log "F2: misrouted application configuration -> fence"
fence_reject F2 x1 ${TCP[y0]} g1_entry_g0_target; fence_reject F2 x0 $SVC_Y svc_target
log "F3: Keeper restart during the window"
stop_one keeper; insert_more x1 300000 > "$OUT/logs/f3_insert.err" 2>&1 && rec F3 insert_without_keeper x1 - - UNEXPECTED_OK "" || rec F3 insert_without_keeper x1 - - FAILED_AS_EXPECTED "$(head -c 120 "$OUT/logs/f3_insert.err")"
start_keeper
f3=ERROR; for _ in $(seq 1 30); do insert_more x1 300000 2> "$OUT/logs/f3_after.err" && { f3=OK; break; }; sleep 2; done
rec F3 insert_after_keeper x1 - - $f3 "$(head -c 120 "$OUT/logs/f3_after.err")"
compare_replicas F3 x0 x1
log "R1: rollback - application back to g0, g1 downgraded to the baseline"
workload R1a g0 record
restart_on x1 "$BBIN"; restart_on y1 "$BBIN"; fence_on x1; fence_on y1
compare_replicas R1 x0 x1; compare_replicas R1 y0 y1
workload R1 g0 record; spans R1a none; spans R1 none
log "R2: upgrade g1 again"
restart_on x1 "$CBIN"; restart_on y1 "$CBIN"; fence_on x1; fence_on y1
compare_replicas R2 x0 x1; compare_replicas R2 y0 y1
log "N1: negative control - fence off on x1, target = cluster Service of Y (spans both builds)"
fence_off x1; fence_wait x1 off || rec N1 fence x1 - - STILL_ON ""
for i in 1 2 3; do workload N1 n1 record nullable_u256_single; done
spans N1 some
fence_on x1; fence_wait x1 on || rec N1 fence x1 - - NOT_RESTORED ""
log "P5: application on g1; upgrade g0 (x0, y0)"
workload P5 g1 oracle
restart_on x0 "$CBIN"; restart_on y0 "$CBIN"
compare_replicas P5 x0 x1; compare_replicas P5 y0 y1
spans P5 none
log "P6: all new, fence removed, application back on cluster Services"
for n in $NODES; do fence_off "$n"; done
for n in $NODES; do fence_wait "$n" off || rec P6 fence "$n" - - STILL_ON ""; done
workload P6 svc oracle; spans P6 none

#### verdict
python3 - "$OUT" <<'EOF'
import collections, json, sys
out = sys.argv[1]
rows = [l.rstrip("\n").split("\t") for l in open(f"{out}/results/checks.tsv")][1:]
spans = [l.rstrip("\n").split("\t") for l in open(f"{out}/results/spans_summary.tsv") if l.strip()]
bad = []
for ph, check, cfg, shape, var, outcome, detail, _sha in rows:
    if outcome in ("WRONG", "VIOLATION", "DIFF", "ERROR", "NOT_APPLIED", "STILL_ON", "NOT_RESTORED", "UNEXPECTED_OK", "REFUSED"):
        # an error of a baseline-judged workload is still a failure of the procedure (it must keep serving)
        bad.append(f"{ph} {check} {cfg} {shape} {var}: {outcome} {detail[:80]}")
for s in spans:
    if s[5] != "OK":
        bad.append(f"spans {s[0]}: {s[3]} spanning builds (want {s[4]})")
count = collections.Counter(r[5].split("(")[0] for r in rows)
summary = {"verdict": "PASS" if not bad else "FAIL", "problems": bad, "outcomes": count,
           "spans": [{"phase": s[0], "queries": int(s[1]), "multi_node": int(s[2]), "spanning_builds": int(s[3]), "want": s[4], "result": s[5]} for s in spans]}
json.dump(summary, open(f"{out}/results/summary.json", "w"), indent=1)
print(json.dumps({"verdict": summary["verdict"], "outcomes": count, "problems": bad[:10]}, indent=1))
sys.exit(0 if not bad else 1)
EOF
