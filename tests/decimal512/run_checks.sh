#!/usr/bin/env bash
# Decimal512/Int512 checks of the Sentio fork, in four tiers:
#   quick    per commit:   midpoint/avg2, operations and key matrices (clickhouse local, no server)
#   full     periodic:     quick + vector-form matrix + fixed-seed random differential matrix (3000 cases) + 256-bit
#                          dispatch scan + the fork's stateless tests
#   nightly  scheduled:    full + a second fixed-seed random differential matrix (9000 cases)
#   release  before an image is built or deployed: nightly + regression proofs against the previous build + on-disk and
#            aggregate-state compatibility in both directions + mixed-version Keeper/ReplicatedMergeTree replication
#            with rollback + a synthetic performance comparison with the previous build + image identity.
#
# PRODUCTION BOUNDARY: this script never talks to production. It runs `clickhouse local` (no listening ports) and,
# for the stateless tests, one loopback-only server started and proven isolated by tools/isolated_ch.sh (fail
# closed). It never pulls or pushes images, never uses credentials and never deploys anything.
#
#   tests/decimal512/run_checks.sh --tier quick|full|nightly|release --binary <clickhouse> --out <new or empty dir>
#       [--source-sha <commit the binary was built from>]  default: the binary's embedded GIT_HASH (a hint only)
#       [--source-clean]                   the operator asserts the binary was built from a clean checkout of that
#                                          commit (release requires it; the embedded GIT_HASH must also match)
#       [--buggy-binary <clickhouse>]      release: the baseline, i.e. the previous production build: the targeted
#                                          checks must FAIL on it (regression proof) and on-disk/aggregate-state data
#                                          must be readable both ways between it and --binary (compatibility)
#       [--image <ref>]                    release: the LOCAL image built from --binary (never pulled); the raw outputs
#                                          of `docker image inspect` and of sha256sum inside a --network none container
#                                          are kept as the (attested) image identity
#       [--protocol-evidence <native_matrix.tsv>]  release: output of tools/compat/native_compat.sh OLD=<baseline>
#                                          NEW=<binary> (not run here); its identities.tsv must sit next to it
#       [--keeper-binary <clickhouse-keeper>]  release: the Keeper build production runs (never upgraded); used by
#                                          tools/replication/mixed_replication.sh (loopback only)
#       [--keeper-expected-sha256 <hex>]   release: sha256 of the production Keeper binary, with --keeper-basis <text>
#                                          saying where it comes from (an operator attestation, compared by the gate)
#       [--perf-rounds <n>]                release: rounds of tools/perf/perf_compare.py (default 7, at least 5)
#       [--instance a|b|c]                 isolated server slot for the stateless tests (default c: ports 59000..)
#       [--workers N]                      engine processes per matrix run (run_matrix.py --workers; default
#                                          $DECIMAL512_MATRIX_WORKERS or 1). Results do not depend on N. Each engine process
#                                          starts ~520 threads: the cgroup's pids.max must be >= N * 640 + 1024 (checked).
#       [--matrix-cache DIR]               reuse a matrix result (tools/matrix/matrix_cache.py) only when the engine, the SQL,
#                                          the oracle, run_matrix.py, python and the time zone are byte-identical; the
#                                          result is then recorded as REUSED historical evidence (results/*.reuse.json,
#                                          checked by the gate), never as a new run. A cache entry that fails verification
#                                          stops the run. Default $DECIMAL512_MATRIX_CACHE or none.
#
# Checks tree vs binary: the checks come from this tree's HEAD. When HEAD differs from --source-sha (the binary's
# commit), the diff must touch only tests/, docs/ and .github/ (nothing compiled into the binary); it is recorded in
# evidence.json and re-checked by the gate. Anything else stops the run: rebuild the binary first.
# CPU (optional, DECIMAL512_SCOPE_CPU_SPLIT=1 inside a ch-validation-*.scope): while the stateless server container runs
# (CH_ISO_DOCKER_CPUS), this scope's CPUQuota is lowered to DECIMAL512_STATELESS_SCOPE_CPU (400%) and restored to
# DECIMAL512_SCOPE_CPU (800%) afterwards, so the task never uses more than 8 cores in total.
#
# Host name of the stateless server (full/nightly/release): CH_ISO_HOST_ISOLATION=container (the default here) runs it
# in a local docker container whose host name is "localhost", and clickhouse-test sees the same name, so the runner's
# substitution of its host name by "localhost" cannot rewrite words of test outputs (on a host called "build" it turned
# "a build that" into "a localhost that" and failed 04881). CH_ISO_HOST_ISOLATION=real keeps the server on the host;
# a test output that contains the host name as a word then fails. See tools/isolated_ch.sh.
# Exit status is the gate's (tools/check_gate.py): 0 PASS, 3 PASS WITH OPEN KNOWN DEFECTS (not releasable),
# 1 FAIL or BLOCKED, 2 usage error. Skipped, empty or unproven checks never pass. Evidence: <out>/evidence.json.
set -u
export GIT_NO_LAZY_FETCH=1 GIT_OPTIONAL_LOCKS=0 GIT_TERMINAL_PROMPT=0
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
T=$HERE/tools

usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
TIER= BIN= OUT= SRC= BUGGY= IMAGE= PROTO= CLEAN=null INST=c KEEPER= KEEPER_SHA= KEEPER_BASIS= PERF_ROUNDS=7
WORKERS=${DECIMAL512_MATRIX_WORKERS:-1} CACHE=${DECIMAL512_MATRIX_CACHE:-}
while [ $# -gt 0 ]; do
  case $1 in
    --tier) TIER=$2; shift 2;;
    --binary) BIN=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --source-sha) SRC=$2; shift 2;;
    --source-clean) CLEAN=false; shift;;
    --buggy-binary) BUGGY=$2; shift 2;;
    --image) IMAGE=$2; shift 2;;
    --protocol-evidence) PROTO=$2; shift 2;;
    --instance) INST=$2; shift 2;;
    --keeper-binary) KEEPER=$2; shift 2;;
    --keeper-expected-sha256) KEEPER_SHA=$2; shift 2;;
    --keeper-basis) KEEPER_BASIS=$2; shift 2;;
    --perf-rounds) PERF_ROUNDS=$2; shift 2;;
    --workers) WORKERS=$2; shift 2;;
    --matrix-cache) CACHE=$2; shift 2;;
    *) usage;;
  esac
done
case $TIER in quick|full|nightly|release) ;; *) usage;; esac
case $INST in a|b|c) ;; *) usage;; esac
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "ERROR: --binary must be an executable clickhouse binary" >&2; exit 2; }
[ -n "$OUT" ] || usage
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: --out $OUT is not empty (evidence is never mixed)" >&2; exit 2; fi
# the only server this script talks to is its own loopback instance; refuse an environment pointing elsewhere
case ${CLICKHOUSE_HOST:-127.0.0.1} in 127.0.0.1|localhost) ;; *) echo "REFUSE: CLICKHOUSE_HOST=$CLICKHOUSE_HOST" >&2; exit 2;; esac
case $WORKERS in ''|*[!0-9]*|0) echo "ERROR: --workers must be a positive integer" >&2; exit 2;; esac
# every clickhouse local starts ~520 threads (16-core host): with too small a pids limit one of the parallel engine
# processes cannot create a thread and hangs (2026-09-25: TasksMax=4096 with 8 workers). Refuse up front instead.
PIDS_MAX=$(cat "/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)/pids.max" 2>/dev/null)
if [ -n "$PIDS_MAX" ] && [ "$PIDS_MAX" != max ] && [ "$PIDS_MAX" -lt $((WORKERS * 640 + 1024)) ]; then
  echo "ERROR: pids.max $PIDS_MAX of this cgroup is below $((WORKERS * 640 + 1024)) for $WORKERS matrix worker(s) (~520 threads per engine process): raise TasksMax or lower --workers" >&2
  exit 2
fi
RUN_STARTED_AT=$(date -u +%FT%TZ)
mkdir -p "$OUT"/{gen,results,logs}
OUT=$(cd "$OUT" && pwd)
BIN=$(readlink -f "$BIN")
[ -z "$BUGGY" ] || BUGGY=$(readlink -f "$BUGGY")
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$OUT/logs/run_checks.log" >&2; }

ident() {  # <binary> -> "sha256 build-id version git-hash" (clickhouse local, private cwd, no ports)
  local b=$1 w sha bid info
  sha=$(sha256sum "$b" | cut -d' ' -f1)
  bid=$(readelf -n "$b" 2>/dev/null | awk '/Build ID/{print $3}')
  w=$(mktemp -d "$OUT/gen/ident.XXXXXX")
  info=$(cd "$w" && timeout 60 "$b" local --query "SELECT version(), (SELECT value FROM system.build_options WHERE name = 'GIT_HASH')" < /dev/null 2>/dev/null | tr '\t' ' ')
  rm -rf "$w"
  echo "$sha ${bid:-none} ${info:-unknown unknown}"
}
read -r BSHA BBID BVER BGIT <<< "$(ident "$BIN")"
[ -n "$SRC" ] || SRC=$BGIT
TESTS_SHA=$(git -C "$TREE" rev-parse HEAD)
log "tier=$TIER binary=$BIN sha256=$BSHA build-id=$BBID version=$BVER git_hash=$BGIT source=$SRC tests=$TESTS_SHA workers=$WORKERS cache=${CACHE:-none}"
# checks from a newer commit than the binary: allowed only for a diff that cannot change the binary
HARNESS_JSON=
if [ "$TESTS_SHA" != "$SRC" ]; then
  if ! diff_files=$(git -C "$TREE" diff --name-only "$SRC" "$TESTS_SHA" 2>/dev/null); then
    log "ERROR: cannot diff the binary's commit $SRC against the checks' tree $TESTS_SHA"; exit 1
  fi
  bad=$(echo "$diff_files" | grep -v -E '^(tests/|docs/|\.github/)' | grep -v '^$' || true)
  if [ -n "$bad" ]; then
    log "ERROR: the checks' tree $TESTS_SHA differs from the binary's commit $SRC in paths that can change the binary (rebuild first): $(echo $bad | cut -c1-300)"
    exit 1
  fi
  HARNESS_JSON=", \"harness_diff\": {\"from\": \"$SRC\", \"to\": \"$TESTS_SHA\", \"files\": $(echo "$diff_files" | python3 -c 'import json,sys; print(json.dumps([l for l in sys.stdin.read().splitlines() if l]))')}"
  log "checks tree $TESTS_SHA differs from the binary's commit $SRC only in tests/, docs/, .github/ ($(echo "$diff_files" | grep -c .) files)"
fi

run_matrix() {  # <matrix name> <binary> <label> -> results/<name>.<label>.{result.jsonl,summary.json[,reuse.json]}
  local m=$1 b=$2 l=$3 rc t0
  local cargs=(--cache "$CACHE" --matrix "$m" --engine "$b" --sql "$OUT/gen/$m.sql" --oracle "$OUT/gen/$m.oracle.jsonl"
               --runner "$T/matrix/run_matrix.py" --result "$OUT/results/$m.$l.result.jsonl" --summary "$OUT/results/$m.$l.summary.json")
  if [ -n "$CACHE" ]; then
    python3 "$T/matrix/matrix_cache.py" fetch "${cargs[@]}" --reuse "$OUT/results/$m.$l.reuse.json" > "$OUT/results/$m.$l.cache.json" 2>&1
    rc=$?
    if [ $rc = 0 ]; then
      log "matrix $m on $l: REUSED historical evidence, not re-run: $(cut -c1-220 "$OUT/results/$m.$l.cache.json")"
      return
    elif [ $rc = 3 ]; then
      log "ERROR: matrix $m on $l: the cache entry for this key failed verification: $(cut -c1-300 "$OUT/results/$m.$l.cache.json")"
      exit 1
    fi
  fi
  t0=$(date +%s)
  python3 "$T/matrix/run_matrix.py" --binary "$b" --workers "$WORKERS" "$OUT/gen/$m.sql" "$OUT/gen/$m.oracle.jsonl" \
    "$OUT/results/$m.$l.result.jsonl" > "$OUT/results/$m.$l.summary.json" 2> "$OUT/results/$m.$l.err"
  rc=$?
  log "matrix $m on $l: rc=$rc ($(( $(date +%s) - t0 )) s, $WORKERS worker(s)) $(cut -c1-200 "$OUT/results/$m.$l.summary.json" 2>/dev/null)"
  if [ -n "$CACHE" ]; then
    python3 "$T/matrix/matrix_cache.py" store "${cargs[@]}" \
      --origin "{\"out\": \"$OUT\", \"tests_sha\": \"$TESTS_SHA\", \"source_sha\": \"$SRC\", \"run_started_at\": \"$RUN_STARTED_AT\"}" \
      >> "$OUT/results/$m.$l.cache.json" 2>&1
  fi
}
reuse_ref() {  # <matrix> <label> -> the JSON member naming the reuse record, when that run was reused
  [ -f "$OUT/results/$1.$2.reuse.json" ] && echo ", \"reuse\": \"results/$1.$2.reuse.json\""
}
gen() {
  (cd "$T/matrix" &&
   python3 gen_midpoint_matrix.py "$OUT/gen/midpoint.sql" "$OUT/gen/midpoint.oracle.jsonl" &&
   python3 gen_midpoint_vector.py "$OUT/gen/midpoint_vector.sql" "$OUT/gen/midpoint_vector.oracle.jsonl" &&
   python3 gen_decimal512_ops_matrix.py "$OUT/gen/ops.sql" "$OUT/gen/ops.oracle.jsonl" &&
   python3 gen_composite_keys.py "$OUT/gen/keys.sql" "$OUT/gen/keys.oracle.jsonl" &&
   python3 gen_random_decimal512.py "$OUT/gen/random.sql" "$OUT/gen/random.oracle.jsonl" &&
   python3 gen_random_decimal512.py "$OUT/gen/random_nightly.sql" "$OUT/gen/random_nightly.oracle.jsonl" --seed 20260926 --cases 9000) \
    > "$OUT/logs/gen.log" 2>&1 \
    || { log "ERROR: matrix generation failed (see logs/gen.log)"; exit 1; }
}

SUITES=()   # JSON objects, joined into evidence.json
NOT_RUN=()
suite() { SUITES+=("$1"); }
matrix_suite() {  # <suite name> <matrix> <label>: the gate checks the inputs against tools/matrix/cases.lock.json
  suite "{\"name\": \"$1\", \"kind\": \"matrix\", \"matrix\": \"$2\", \"sql\": \"gen/$2.sql\", \"oracle\": \"gen/$2.oracle.jsonl\", \"result\": \"results/$2.$3.result.jsonl\", \"summary\": \"results/$2.$3.summary.json\"$(reuse_ref "$2" "$3")}"
}
proof_suite() {  # <matrix> <targets json> <controls json>: recomputed by the gate from both result files
  suite "{\"name\": \"regression-proof:$1\", \"kind\": \"proof\", \"matrix\": \"$1\", \"targets\": $2, \"controls\": $3, \"sql\": \"gen/$1.sql\", \"oracle\": \"gen/$1.oracle.jsonl\", \"buggy\": {\"result\": \"results/$1.previous.result.jsonl\", \"summary\": \"results/$1.previous.summary.json\"$(reuse_ref "$1" previous)}, \"fixed\": {\"result\": \"results/$1.tested.result.jsonl\", \"summary\": \"results/$1.tested.summary.json\"$(reuse_ref "$1" tested)}, \"report\": \"results/proof.$1.json\"}"
}

gen
for m in midpoint ops keys; do run_matrix $m "$BIN" tested; done
matrix_suite midpoint-matrix midpoint tested
matrix_suite ops-matrix ops tested
matrix_suite keys-matrix keys tested

if [ "$TIER" != quick ]; then
  run_matrix midpoint_vector "$BIN" tested
  matrix_suite midpoint-vector-matrix midpoint_vector tested
  run_matrix random "$BIN" tested
  matrix_suite random-matrix random tested
  if [ "$TIER" = nightly ] || [ "$TIER" = release ]; then
    run_matrix random_nightly "$BIN" tested
    matrix_suite random-nightly-matrix random_nightly tested
  fi

  if git -C "$TREE" cat-file -e "$SRC^{commit}" 2>/dev/null; then
    python3 "$T/scan_wide_dispatch.py" --repo "$TREE" --rev "$SRC" --baseline "$HERE/dispatch_scan_baseline.json" --json \
      > "$OUT/results/dispatch_scan.json" 2> "$OUT/results/dispatch_scan.err"
    log "dispatch scan of $SRC: rc=$?"
    suite '{"name": "dispatch-scan", "kind": "scan", "result": "results/dispatch_scan.json"}'
  else
    log "dispatch scan: source $SRC is not a local commit"
    NOT_RUN+=("{\"name\": \"dispatch-scan\", \"reason\": \"source $SRC is not available locally\"}")
  fi

  # fork stateless tests on one isolated loopback server (instance $INST; a/b/c = ports 39000/49000/59000..)
  export CH_ISO_ROOT=$OUT/iso CH_ISO_LOGS=$OUT/logs
  SEL=() SELECTED=() EXCLUDED=()
  while IFS= read -r line; do
    name=${line%%#*}; name=$(echo "$name" | tr -d '[:space:]'); [ -n "$name" ] || continue
    if [[ $line == *"# exclude:"* ]]; then
      kd=$(echo "${line#*# exclude:}" | awk -F: '{gsub(/ /, "", $1); print $1}')
      EXCLUDED+=("{\"test\": \"$name\", \"reason\": \"$kd\"}")
    else
      SEL+=("^$name\\."); SELECTED+=("\"$name\"")
    fi
  done < "$HERE/stateless_tests.txt"
  touch "$OUT/logs/.stateless_start"   # every file of the tree written after this marker is a side effect of the run
  # CH_ISO_KEEPER=1: the server has a private loopback Keeper, so tests that need ZooKeeper (Replicated tables,
  # generateSerialID, ...) run as in upstream CI instead of failing on "no Zookeeper configuration"
  HOST_ISOLATION=${CH_ISO_HOST_ISOLATION:-container}
  log "stateless server host name isolation: $HOST_ISOLATION"
  # total CPU of the task stays within 8 cores: the container gets CH_ISO_DOCKER_CPUS, this scope gives up as much
  SCOPE_UNIT=
  if [ "${DECIMAL512_SCOPE_CPU_SPLIT:-0}" = 1 ]; then
    SCOPE_UNIT=$(sed -n 's|^0::.*/\(ch-validation-[A-Za-z0-9_-]*\.scope\)$|\1|p' /proc/self/cgroup)
    if [ -n "$SCOPE_UNIT" ] && systemctl --user set-property --runtime "$SCOPE_UNIT" CPUQuota="${DECIMAL512_STATELESS_SCOPE_CPU:-400%}"; then
      log "cpu: $SCOPE_UNIT CPUQuota ${DECIMAL512_STATELESS_SCOPE_CPU:-400%} while the container (${CH_ISO_DOCKER_CPUS:-all} cpus) runs"
    else
      log "cpu: no ch-validation scope found or quota not changed (SCOPE_UNIT='$SCOPE_UNIT')"; SCOPE_UNIT=
    fi
  fi
  if CH_ISO_KEEPER=1 CH_ISO_HOST_ISOLATION=$HOST_ISOLATION bash "$T/isolated_ch.sh" start "$INST" "$BIN" "$TREE" > "$OUT/logs/server_start.log" 2>&1; then
    bash "$T/isolated_ch.sh" test "$INST" "$TREE" fork-stateless "${BBID:0:12}" --no-random-settings --no-random-merge-tree-settings \
      --no-stateful -j 4 "${SEL[@]}" > "$OUT/logs/stateless_driver.log" 2>&1
    log "fork stateless tests: rc=$? (${#SEL[@]} selected, ${#EXCLUDED[@]} excluded)"
  else
    log "ERROR: isolated server did not start (see logs/server_start.log)"
  fi
  bash "$T/isolated_ch.sh" stop "$INST" >> "$OUT/logs/server_start.log" 2>&1
  if [ -n "$SCOPE_UNIT" ]; then
    systemctl --user set-property --runtime "$SCOPE_UNIT" CPUQuota="${DECIMAL512_SCOPE_CPU:-800%}" && log "cpu: $SCOPE_UNIT CPUQuota restored to ${DECIMAL512_SCOPE_CPU:-800%}"
  fi
  rm -f "$OUT"/iso/bin/clickhouse-*   # the server's private copy of the binary (GBs); logs stay as evidence
  # files the tests created or rewrote inside the source tree (reported, never deleted by this script)
  find "$TREE/tests" -newer "$OUT/logs/.stateless_start" -type f 2>/dev/null | sed "s#^$TREE/##" | sort > "$OUT/logs/tree_side_effects.txt"
  log "stateless run wrote $(wc -l < "$OUT/logs/tree_side_effects.txt") file(s) inside the tree (logs/tree_side_effects.txt)"
  TLOG=$(ls -t "$OUT"/logs/test_fork-stateless_*.log 2>/dev/null | head -1)
  sel_json=$(IFS=,; echo "${SELECTED[*]}"); exc_json=$(IFS=,; echo "${EXCLUDED[*]}")
  suite "{\"name\": \"fork-stateless\", \"kind\": \"clickhouse-test\", \"log\": \"${TLOG#$OUT/}\", \"build_id\": \"$BBID\", \"selected\": [$sel_json], \"excluded\": [$exc_json], \"selection\": \"$HERE/stateless_tests.txt\", \"selection_sha256\": \"$(sha256sum "$HERE/stateless_tests.txt" | cut -d' ' -f1)\"}"
fi

IMG_JSON= BASE_JSON=
if [ -n "$BUGGY" ]; then
  read -r OSHA OBID OVER OGIT <<< "$(ident "$BUGGY")"
  BASE_JSON=", \"baseline\": {\"path\": \"$BUGGY\", \"sha256\": \"$OSHA\", \"build_id\": \"$OBID\", \"version\": \"$OVER\", \"git_hash\": \"$OGIT\", \"role\": \"previous production build\"}"
fi
if [ "$TIER" = release ]; then
  if [ -n "$BUGGY" ]; then
    for m in midpoint ops keys; do run_matrix $m "$BUGGY" previous; done
    python3 "$T/regression_proof.py" --buggy previous="$OUT/results/keys.previous.result.jsonl:$OUT/results/keys.previous.summary.json" \
      --fixed tested="$OUT/results/keys.tested.result.jsonl:$OUT/results/keys.tested.summary.json" --target keys-512 --target single-key-512 \
      --control keys-control --control single-key-control --buggy-binary "$BUGGY" --fixed-binary "$BIN" \
      --json "$OUT/results/proof.keys.json" > "$OUT/results/proof.keys.txt" 2>&1
    log "regression proof keys: rc=$?"
    python3 "$T/regression_proof.py" --buggy previous="$OUT/results/midpoint.previous.result.jsonl:$OUT/results/midpoint.previous.summary.json" \
      --fixed tested="$OUT/results/midpoint.tested.result.jsonl:$OUT/results/midpoint.tested.summary.json" --target midpoint-dec512 \
      --control midpoint-reject --buggy-binary "$BUGGY" --fixed-binary "$BIN" \
      --json "$OUT/results/proof.midpoint.json" > "$OUT/results/proof.midpoint.txt" 2>&1
    log "regression proof midpoint: rc=$?"
    OPS_T="boundary parse-boundary int512-arith int512-supertype midpoint-int512 scale-overflow convert-wide scale-154"
    OPS_C="int-wrap-control int-supertype-control midpoint-int-control scale-overflow-control convert-wide-control"
    python3 "$T/regression_proof.py" --buggy previous="$OUT/results/ops.previous.result.jsonl:$OUT/results/ops.previous.summary.json" \
      --fixed tested="$OUT/results/ops.tested.result.jsonl:$OUT/results/ops.tested.summary.json" \
      $(for c in $OPS_T; do printf -- '--target %s ' "$c"; done) $(for c in $OPS_C; do printf -- '--control %s ' "$c"; done) \
      --buggy-binary "$BUGGY" --fixed-binary "$BIN" --json "$OUT/results/proof.ops.json" > "$OUT/results/proof.ops.txt" 2>&1
    log "regression proof ops: rc=$?"
    jlist() { printf '['; local sep=; for c in "$@"; do printf '%s"%s"' "$sep" "$c"; sep=', '; done; printf ']'; }
    proof_suite keys '["keys-512", "single-key-512"]' '["keys-control", "single-key-control"]'
    proof_suite midpoint '["midpoint-dec512"]' '["midpoint-reject"]'
    proof_suite ops "$(jlist $OPS_T)" "$(jlist $OPS_C)"
  else
    NOT_RUN+=('{"name": "regression-proof", "reason": "no --buggy-binary given"}')
  fi
  if [ -n "$BUGGY" ]; then
    # engine labels are bound to binaries by the engine= lines run_compat2.sh writes (sha256 measured before the run)
    bash "$T/compat/run_compat2.sh" "$OUT/compat/previous_writes" previous="$BUGGY" tested="$BIN" > "$OUT/logs/compat_previous_writes.log" 2>&1
    bash "$T/compat/run_compat2.sh" "$OUT/compat/tested_writes" tested="$BIN" previous="$BUGGY" > "$OUT/logs/compat_tested_writes.log" 2>&1
    rm -rf "$OUT"/compat/*/data.*
    suite '{"name": "compat-disk", "kind": "compat", "results": ["compat/previous_writes/summary.previous.txt", "compat/tested_writes/summary.tested.txt"]}'
  else
    NOT_RUN+=('{"name": "compat-disk", "reason": "no --buggy-binary (baseline) given"}')
  fi
  if [ -n "$PROTO" ]; then
    cp "$PROTO" "$OUT/results/native_matrix.tsv"
    cp "$(dirname "$PROTO")/identities.tsv" "$OUT/results/native_identities.tsv" 2>/dev/null || log "protocol evidence has no identities.tsv next to it"
    suite '{"name": "compat-protocol", "kind": "protocol", "result": "results/native_matrix.tsv", "identities": "results/native_identities.tsv"}'
  else
    NOT_RUN+=('{"name": "compat-protocol", "reason": "run tools/compat/native_compat.sh on isolated servers and pass --protocol-evidence"}')
  fi
  if [ -n "$IMAGE" ]; then
    # local inspection only: the image id is the identity before a push; after the push, deploy the repo digest
    # whose config is this image id (the deployment approval checks that, this script never pushes)
    # raw outputs are kept: the gate cross-checks the manifest against them (still an attestation, not proof)
    docker image inspect "$IMAGE" > "$OUT/results/image_inspect.json" 2> "$OUT/logs/image_inspect.err"
    timeout 300 docker run --rm --pull never --network none --entrypoint sha256sum "$IMAGE" /usr/bin/clickhouse \
      > "$OUT/results/image_binary_sha256.txt" 2> "$OUT/logs/image_sha256.err"
    IID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["Id"])' "$OUT/results/image_inspect.json" 2>/dev/null)
    RDIG=$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))[0].get("RepoDigests") or []))' "$OUT/results/image_inspect.json" 2>/dev/null)
    ISHA=$(cut -d' ' -f1 "$OUT/results/image_binary_sha256.txt" 2>/dev/null)
    IMG_JSON=", \"image\": {\"ref\": \"$IMAGE\", \"id\": \"$IID\", \"repo_digests\": \"$RDIG\", \"binary_sha256\": \"$ISHA\", \"inspect\": \"results/image_inspect.json\", \"binary_sha256_output\": \"results/image_binary_sha256.txt\"}"
    log "image $IMAGE: id=${IID:-unknown} repo digests=${RDIG:-none} binary sha256=${ISHA:-unknown}"
  fi
  if [ -n "$BUGGY" ] && [ -n "$KEEPER" ]; then
    # Keeper (the production build, never upgraded) + two replicas on 127.0.0.1: baseline -> candidate -> rollback
    bash "$T/replication/mixed_replication.sh" "$OUT/replication" --keeper "$KEEPER" --baseline "$BUGGY" --candidate "$BIN" \
      > "$OUT/logs/replication.log" 2>&1
    log "mixed-version replication: rc=$?"
    rm -rf "$OUT"/replication/iso/r1/data "$OUT"/replication/iso/r2/data "$OUT"/replication/iso/keeper/coordination
    KX=; [ -n "$KEEPER_SHA" ] && KX=", \"keeper_expected_sha256\": \"$KEEPER_SHA\", \"keeper_basis\": \"${KEEPER_BASIS:-operator}\""
    suite "{\"name\": \"keeper-replication\", \"kind\": \"replication\", \"steps\": \"replication/steps.tsv\", \"identities\": \"replication/identities.tsv\"$KX}"
  else
    NOT_RUN+=('{"name": "keeper-replication", "reason": "needs --buggy-binary (baseline) and --keeper-binary (the production Keeper build)"}')
  fi
  if [ -n "$BUGGY" ]; then
    # distributed GROUP BY over shards of the baseline and the candidate (rolling upgrade, both coordinator directions,
    # single- and two-level aggregation): four isolated instances, expectations from tools/replication/groupby_oracle.py
    bash "$T/replication/mixed_distributed_groupby.sh" "$OUT/mixed_groupby" --baseline "$BUGGY" --candidate "$BIN" --tree "$TREE" \
      > "$OUT/logs/mixed_groupby.log" 2>&1
    log "mixed-version distributed GROUP BY: rc=$?"
    suite '{"name": "mixed-groupby", "kind": "groupby", "cases": "mixed_groupby/results/cases.tsv", "results_dir": "mixed_groupby/results", "identities": "mixed_groupby/identities.tsv", "lock": "mixed_groupby/groupby_cases.lock.json"}'
  else
    NOT_RUN+=('{"name": "mixed-groupby", "reason": "needs --buggy-binary (baseline)"}')
  fi
  if [ -n "$BUGGY" ]; then
    python3 "$T/perf/perf_compare.py" --baseline "$BUGGY" --candidate "$BIN" --out "$OUT/perf" --rounds "$PERF_ROUNDS" \
      > "$OUT/logs/perf.log" 2>&1
    log "performance comparison: rc=$? $(head -1 "$OUT/logs/perf.log" 2>/dev/null | cut -c1-200)"
    suite '{"name": "performance", "kind": "perf", "summary": "perf/perf_summary.json", "raw": "perf/perf_raw.tsv"}'
  else
    NOT_RUN+=('{"name": "performance", "reason": "no --buggy-binary (baseline) to compare with"}')
  fi
fi

DIRTY=$CLEAN   # null unless --source-clean: this script cannot see the tree the binary was built from
ATTEST=null; [ "$CLEAN" = false ] && ATTEST='"operator (run_checks.sh --source-clean)"'
suites_json=$(IFS=,; echo "${SUITES[*]}"); not_run_json=$(IFS=,; echo "${NOT_RUN[*]}")
cat > "$OUT/evidence.json" <<JSON
{"schema": 2, "tier": "$TIER", "created_at": "$(date -u +%FT%TZ)", "run_started_at": "$RUN_STARTED_AT",
 "scheduling": {"matrix_workers": $WORKERS, "matrix_cache": "${CACHE:-}"},
 "source": {"sha": "$SRC", "dirty": $DIRTY, "dirty_attested_by": $ATTEST, "binary_git_hash": "$BGIT",
            "binary_git_hash_source": "runner: clickhouse local, system.build_options GIT_HASH"},
 "tests": {"repo": "$TREE", "sha": "$TESTS_SHA"}$HARNESS_JSON,
 "binary": {"path": "$BIN", "sha256": "$BSHA", "build_id": "$BBID", "version": "$BVER"}$BASE_JSON$IMG_JSON,
 "suites": [$suites_json],
 "not_run": [$not_run_json]}
JSON
python3 -m json.tool "$OUT/evidence.json" > /dev/null || { log "ERROR: evidence.json is not valid JSON"; exit 1; }
python3 "$T/check_gate.py" --manifest "$OUT/evidence.json" --known-defects "$HERE/known_defects.json" \
  --case-lock "$T/matrix/cases.lock.json" --tier "$TIER" --json "$OUT/gate.json" | tee "$OUT/gate.txt"
rc=${PIPESTATUS[0]}
log "gate $TIER: exit $rc"
exit $rc
