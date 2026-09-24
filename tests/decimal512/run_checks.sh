#!/usr/bin/env bash
# Decimal512/Int512 checks of the Sentio fork, in three tiers:
#   quick    per commit:   midpoint/avg2, operations and key matrices (clickhouse local, no server)
#   full     periodic:     quick + vector-form matrix + 256-bit dispatch scan + the fork's stateless tests
#   release  before an image is built or deployed: full + regression proof against the previous build + on-disk and
#            aggregate-state compatibility in both directions + image identity; Keeper/replication and performance
#            have no automated check yet and are declared not run, so a release is BLOCKED until they exist.
#
# PRODUCTION BOUNDARY: this script never talks to production. It runs `clickhouse local` (no listening ports) and,
# for the stateless tests, one loopback-only server started and proven isolated by tools/isolated_ch.sh (fail
# closed). It never pulls or pushes images, never uses credentials and never deploys anything.
#
#   tests/decimal512/run_checks.sh --tier quick|full|release --binary <clickhouse> --out <new or empty dir>
#       [--source-sha <commit the binary was built from>]  default: the binary's embedded GIT_HASH (a hint only)
#       [--source-clean]                   the operator asserts the binary was built from a clean checkout of that
#                                          commit (release requires it; the embedded GIT_HASH must also match)
#       [--buggy-binary <clickhouse>]      release: the previous build; the targeted checks must FAIL on it
#       [--old-binary <clickhouse>]        release: the build to be compatible with (default: --buggy-binary)
#       [--image <ref>]                    release: the LOCAL image built from --binary (never pulled); its image id
#                                          and the sha256 of /usr/bin/clickhouse inside it are recorded
#       [--protocol-evidence <native_matrix.tsv>]  release: output of tools/compat/native_compat.sh (not run here)
#       [--instance a|b|c]                 isolated server slot for the stateless tests (default c: ports 59000..)
#
# Exit status is the gate's (tools/check_gate.py): 0 PASS, 3 PASS WITH OPEN KNOWN DEFECTS (not releasable),
# 1 FAIL or BLOCKED, 2 usage error. Skipped, empty or unproven checks never pass. Evidence: <out>/evidence.json.
set -u
export GIT_NO_LAZY_FETCH=1 GIT_OPTIONAL_LOCKS=0 GIT_TERMINAL_PROMPT=0
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
T=$HERE/tools

usage() { sed -n '2,24p' "$0" >&2; exit 2; }
TIER= BIN= OUT= SRC= BUGGY= OLD= IMAGE= PROTO= CLEAN=null INST=c
while [ $# -gt 0 ]; do
  case $1 in
    --tier) TIER=$2; shift 2;;
    --binary) BIN=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --source-sha) SRC=$2; shift 2;;
    --source-clean) CLEAN=false; shift;;
    --buggy-binary) BUGGY=$2; shift 2;;
    --old-binary) OLD=$2; shift 2;;
    --image) IMAGE=$2; shift 2;;
    --protocol-evidence) PROTO=$2; shift 2;;
    --instance) INST=$2; shift 2;;
    *) usage;;
  esac
done
case $TIER in quick|full|release) ;; *) usage;; esac
case $INST in a|b|c) ;; *) usage;; esac
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "ERROR: --binary must be an executable clickhouse binary" >&2; exit 2; }
[ -n "$OUT" ] || usage
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: --out $OUT is not empty (evidence is never mixed)" >&2; exit 2; fi
# the only server this script talks to is its own loopback instance; refuse an environment pointing elsewhere
case ${CLICKHOUSE_HOST:-127.0.0.1} in 127.0.0.1|localhost) ;; *) echo "REFUSE: CLICKHOUSE_HOST=$CLICKHOUSE_HOST" >&2; exit 2;; esac
mkdir -p "$OUT"/{gen,results,logs}
OUT=$(cd "$OUT" && pwd)
BIN=$(readlink -f "$BIN")
[ -z "$BUGGY" ] || BUGGY=$(readlink -f "$BUGGY")
[ -z "$OLD" ] && OLD=$BUGGY
[ -z "$OLD" ] || OLD=$(readlink -f "$OLD")
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
log "tier=$TIER binary=$BIN sha256=$BSHA build-id=$BBID version=$BVER git_hash=$BGIT source=$SRC tests=$(git -C "$TREE" rev-parse HEAD)"

run_matrix() {  # <matrix name> <binary> <label> -> results/<name>.<label>.{result.jsonl,summary.json}
  local m=$1 b=$2 l=$3
  python3 "$T/matrix/run_matrix.py" --binary "$b" "$OUT/gen/$m.sql" "$OUT/gen/$m.oracle.jsonl" \
    "$OUT/results/$m.$l.result.jsonl" > "$OUT/results/$m.$l.summary.json" 2> "$OUT/results/$m.$l.err"
  log "matrix $m on $l: rc=$? $(cat "$OUT/results/$m.$l.summary.json" 2>/dev/null | cut -c1-200)"
}
gen() {
  (cd "$T/matrix" &&
   python3 gen_midpoint_matrix.py "$OUT/gen/midpoint.sql" "$OUT/gen/midpoint.oracle.jsonl" &&
   python3 gen_midpoint_vector.py "$OUT/gen/midpoint_vector.sql" "$OUT/gen/midpoint_vector.oracle.jsonl" &&
   python3 gen_decimal512_ops_matrix.py "$OUT/gen/ops.sql" "$OUT/gen/ops.oracle.jsonl" &&
   python3 gen_composite_keys.py "$OUT/gen/keys.sql" "$OUT/gen/keys.oracle.jsonl") > "$OUT/logs/gen.log" 2>&1 \
    || { log "ERROR: matrix generation failed (see logs/gen.log)"; exit 1; }
}

SUITES=()   # JSON objects, joined into evidence.json
NOT_RUN=()
suite() { SUITES+=("$1"); }
matrix_suite() {  # <suite name> <matrix> <label>
  suite "{\"name\": \"$1\", \"kind\": \"matrix\", \"result\": \"results/$2.$3.result.jsonl\", \"summary\": \"results/$2.$3.summary.json\"}"
}

gen
for m in midpoint ops keys; do run_matrix $m "$BIN" tested; done
matrix_suite midpoint-matrix midpoint tested
matrix_suite ops-matrix ops tested
matrix_suite keys-matrix keys tested

if [ "$TIER" != quick ]; then
  run_matrix midpoint_vector "$BIN" tested
  matrix_suite midpoint-vector-matrix midpoint_vector tested

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
  if bash "$T/isolated_ch.sh" start "$INST" "$BIN" "$TREE" > "$OUT/logs/server_start.log" 2>&1; then
    bash "$T/isolated_ch.sh" test "$INST" "$TREE" fork-stateless "${BBID:0:12}" --no-random-settings --no-random-merge-tree-settings \
      --no-zookeeper --no-shard --no-stateful -j 4 "${SEL[@]}" > "$OUT/logs/stateless_driver.log" 2>&1
    log "fork stateless tests: rc=$? (${#SEL[@]} selected, ${#EXCLUDED[@]} excluded)"
  else
    log "ERROR: isolated server did not start (see logs/server_start.log)"
  fi
  bash "$T/isolated_ch.sh" stop "$INST" >> "$OUT/logs/server_start.log" 2>&1
  rm -f "$OUT"/iso/bin/clickhouse-*   # the server's private copy of the binary (GBs); logs stay as evidence
  # files the tests created or rewrote inside the source tree (reported, never deleted by this script)
  find "$TREE/tests" -newer "$OUT/logs/.stateless_start" -type f 2>/dev/null | sed "s#^$TREE/##" | sort > "$OUT/logs/tree_side_effects.txt"
  log "stateless run wrote $(wc -l < "$OUT/logs/tree_side_effects.txt") file(s) inside the tree (logs/tree_side_effects.txt)"
  TLOG=$(ls -t "$OUT"/logs/test_fork-stateless_*.log 2>/dev/null | head -1)
  sel_json=$(IFS=,; echo "${SELECTED[*]}"); exc_json=$(IFS=,; echo "${EXCLUDED[*]}")
  suite "{\"name\": \"fork-stateless\", \"kind\": \"clickhouse-test\", \"log\": \"${TLOG#$OUT/}\", \"build_id\": \"$BBID\", \"selected\": [$sel_json], \"excluded\": [$exc_json]}"
fi

IMG_JSON=
if [ "$TIER" = release ]; then
  if [ -n "$BUGGY" ]; then
    read -r OSHA OBID _ <<< "$(ident "$BUGGY")"
    for m in midpoint keys; do run_matrix $m "$BUGGY" previous; done
    python3 "$T/regression_proof.py" --buggy previous="$OUT/results/keys.previous.result.jsonl:$OUT/results/keys.previous.summary.json" \
      --fixed tested="$OUT/results/keys.tested.result.jsonl:$OUT/results/keys.tested.summary.json" --target keys-512 \
      --control keys-control --control single-key-control --buggy-binary "$BUGGY" --fixed-binary "$BIN" \
      --json "$OUT/results/proof.keys.json" > "$OUT/results/proof.keys.txt" 2>&1
    log "regression proof keys: rc=$?"
    python3 "$T/regression_proof.py" --buggy previous="$OUT/results/midpoint.previous.result.jsonl:$OUT/results/midpoint.previous.summary.json" \
      --fixed tested="$OUT/results/midpoint.tested.result.jsonl:$OUT/results/midpoint.tested.summary.json" --target midpoint-dec512 \
      --control midpoint-reject --buggy-binary "$BUGGY" --fixed-binary "$BIN" \
      --json "$OUT/results/proof.midpoint.json" > "$OUT/results/proof.midpoint.txt" 2>&1
    log "regression proof midpoint: rc=$?"
    suite '{"name": "regression-proof:keys", "kind": "proof", "result": "results/proof.keys.json"}'
    suite '{"name": "regression-proof:midpoint", "kind": "proof", "result": "results/proof.midpoint.json"}'
  else
    NOT_RUN+=('{"name": "regression-proof", "reason": "no --buggy-binary given"}')
  fi
  if [ -n "$OLD" ]; then
    bash "$T/compat/run_compat2.sh" "$OUT/compat/old_writes" old="$OLD" tested="$BIN" > "$OUT/logs/compat_old_writes.log" 2>&1
    bash "$T/compat/run_compat2.sh" "$OUT/compat/tested_writes" tested="$BIN" old="$OLD" > "$OUT/logs/compat_tested_writes.log" 2>&1
    rm -rf "$OUT"/compat/*/data.*
    suite '{"name": "compat-disk", "kind": "compat", "results": ["compat/old_writes/summary.old.txt", "compat/tested_writes/summary.tested.txt"]}'
  else
    NOT_RUN+=('{"name": "compat-disk", "reason": "no --old-binary/--buggy-binary given"}')
  fi
  if [ -n "$PROTO" ]; then
    cp "$PROTO" "$OUT/results/native_matrix.tsv"
    suite '{"name": "compat-protocol", "kind": "protocol", "result": "results/native_matrix.tsv"}'
  else
    NOT_RUN+=('{"name": "compat-protocol", "reason": "run tools/compat/native_compat.sh on isolated servers and pass --protocol-evidence"}')
  fi
  if [ -n "$IMAGE" ]; then
    # local inspection only: the image id is the identity before a push; after the push, deploy the repo digest
    # whose config is this image id (the deployment approval checks that, this script never pushes)
    IID=$(docker image inspect --format '{{.Id}}' "$IMAGE" 2>/dev/null)
    RDIG=$(docker image inspect --format '{{range .RepoDigests}}{{.}} {{end}}' "$IMAGE" 2>/dev/null | xargs)
    ISHA=$(timeout 300 docker run --rm --pull never --network none --entrypoint sha256sum "$IMAGE" /usr/bin/clickhouse 2>/dev/null | cut -d' ' -f1)
    IMG_JSON=", \"image\": {\"ref\": \"$IMAGE\", \"id\": \"$IID\", \"repo_digests\": \"$RDIG\", \"binary_sha256\": \"$ISHA\"}"
    log "image $IMAGE: id=${IID:-unknown} repo digests=${RDIG:-none} binary sha256=${ISHA:-unknown}"
  fi
  NOT_RUN+=('{"name": "keeper-replication", "reason": "no automated isolated Keeper/ReplicatedMergeTree mixed-version check exists yet"}')
  NOT_RUN+=('{"name": "performance", "reason": "no benchmark against the previous build exists yet"}')
fi

DIRTY=$CLEAN   # null unless --source-clean: this script cannot see the tree the binary was built from
suites_json=$(IFS=,; echo "${SUITES[*]}"); not_run_json=$(IFS=,; echo "${NOT_RUN[*]}")
cat > "$OUT/evidence.json" <<JSON
{"schema": 1, "tier": "$TIER", "created_at": "$(date -u +%FT%TZ)",
 "source": {"sha": "$SRC", "dirty": $DIRTY, "binary_git_hash": "$BGIT"},
 "tests": {"repo": "$TREE", "sha": "$(git -C "$TREE" rev-parse HEAD)"},
 "binary": {"path": "$BIN", "sha256": "$BSHA", "build_id": "$BBID", "version": "$BVER"}$IMG_JSON,
 "suites": [$suites_json],
 "not_run": [$not_run_json]}
JSON
python3 -m json.tool "$OUT/evidence.json" > /dev/null || { log "ERROR: evidence.json is not valid JSON"; exit 1; }
python3 "$T/check_gate.py" --manifest "$OUT/evidence.json" --known-defects "$HERE/known_defects.json" --tier "$TIER" \
  --json "$OUT/gate.json" | tee "$OUT/gate.txt"
rc=${PIPESTATUS[0]}
log "gate $TIER: exit $rc"
exit $rc
