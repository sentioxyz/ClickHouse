#!/usr/bin/env bash
# Native-protocol cross-version checks between isolated loopback instances started by scripts/isolated_ch.sh.
# Never production: every server is started here, proven isolated (exe under $CH_ISO_ROOT/bin, loopback-only
# listeners, expected build id) and stopped at the end.
#
#   native_compat.sh <out-dir> OLD=<binary> NEW=<binary> [UP=<binary>]
#     OLD  the fork build that runs in production today (e.g. the binary of the production image digest)
#     NEW  the port under test
#     UP   optional official upstream binary of the target version (docker create + docker cp from the image)
#   env: CH_ISO_ROOT (isolated root), CH_TREE (any source tree; only used for user_files_path),
#        CH_ISO_SCRIPT (path of isolated_ch.sh; default: the skill's scripts/isolated_ch.sh)
#
# Channels: client<->server SELECT (client decodes Dynamic incl. shared variant) and INSERT Native, server<->server
# remote() SELECT and INSERT INTO FUNCTION remote() in both directions (a canary replica next to old replicas),
# plain typed columns as control; NEW<->UP strided QBit inside Dynamic (the ambiguous binary type index 0x37).
# Output: <out-dir>/native_matrix.tsv. SAME = identical to the writer's own rendering. <out-dir>/identities.tsv binds
# each role to a binary (role, path, sha256, build-id; measured before the servers start).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ISO=${CH_ISO_SCRIPT:-$HERE/../../scripts/isolated_ch.sh}   # vendored copies set CH_ISO_SCRIPT to their isolated_ch.sh
OUT=$1; shift; mkdir -p "$OUT"
export CH_ISO_ROOT=${CH_ISO_ROOT:-${CLAUDE_JOB_DIR:-$HOME/.cache}/ch-isolated}
# every run needs fresh instances: servers started on an earlier run's data dirs failed the setup ("table already
# exists", 0 SAME rows; twice on 2026-09-25), so refuse a root that already has instance data
for s in a b c; do
  [ -e "$CH_ISO_ROOT/inst-$s/data" ] && { echo "REFUSE: $CH_ISO_ROOT/inst-$s already holds data of an earlier run: use a new CH_ISO_ROOT" >&2; exit 2; }
done
TREE=${CH_TREE:-$PWD}
declare -A EXE PORT INST BID
i=0; ROLES=()
for kv in "$@"; do
  r=${kv%%=*}; b=${kv#*=}; case $r in OLD|NEW|UP) ;; *) echo "unknown role $r" >&2; exit 2;; esac
  EXE[$r]=$(readlink -f "$b"); INST[$r]=$(echo a b c | cut -d' ' -f$((i + 1))); i=$((i + 1)); ROLES+=("$r")
  case ${INST[$r]} in a) PORT[$r]=39000;; b) PORT[$r]=49000;; c) PORT[$r]=59000;; esac
  BID[$r]=$(readelf -n "${EXE[$r]}" | awk '/Build ID/{print substr($3,1,12)}')
done
[ -n "${EXE[OLD]:-}" ] && [ -n "${EXE[NEW]:-}" ] || { echo "OLD and NEW are required" >&2; exit 2; }
TSV=$OUT/native_matrix.tsv
printf 'step\tclient\tserver\trc\tresult\n' > "$TSV"
: > "$OUT/identities.tsv"
for r in "${ROLES[@]}"; do
  printf '%s\t%s\t%s\t%s\n' "$r" "${EXE[$r]}" "$(sha256sum "${EXE[$r]}" | cut -d' ' -f1)" "$(readelf -n "${EXE[$r]}" | awk '/Build ID/{print $3}')" >> "$OUT/identities.tsv"
done

cl() { timeout 300 "${EXE[$1]}" client --host 127.0.0.1 --port "${PORT[$2]}" --query "$3" < "${4:-/dev/null}"; }
rec() {
  local res
  if [ "$4" -eq 0 ]; then res=$(tr '\n\t' '| ' < "$5" | head -c 400); else res="ERROR $(grep -m1 -oE 'Code: [0-9]+\. DB::Exception: .{0,220}' "$6")"; fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$res" >> "$TSV"
}
verify_server() {
  local s=$1 pid exe b
  pid=$(cat "$CH_ISO_ROOT/inst-${INST[$s]}/server.pid" 2>/dev/null) || return 1
  exe=$(readlink "/proc/$pid/exe") || return 1
  case "$exe" in "$CH_ISO_ROOT"/bin/*) ;; *) return 1;; esac
  # loopback only; isolated_ch.sh listens on 127.0.0.1-3 (aliases of the same instance for test clusters)
  ss -ltnp | grep "pid=$pid," | awk '{print $4}' | grep -vqE '^127\.0\.0\.[123]:' && return 1
  b=$(cl "$s" "$s" "SELECT lower(buildId())") || return 1
  [[ $b == ${BID[$s]}* ]]
}
# stop the servers, then drop the servers' private binary copies (GBs each; the originals are the given paths, whose
# identities are in identities.tsv); logs and data stay
cleanup() { for s in "${ROLES[@]}"; do "$ISO" stop "${INST[$s]}" >> "$OUT/stop.txt" 2>&1; done; rm -f "$CH_ISO_ROOT"/bin/clickhouse-*; }
trap cleanup EXIT

for s in "${ROLES[@]}"; do
  "$ISO" start "${INST[$s]}" "${EXE[$s]}" "$TREE" > "$OUT/start.$s.txt" 2>&1
  verify_server "$s" || { echo "REFUSE: $s instance not proven isolated"; cat "$OUT/start.$s.txt"; exit 90; }
done
echo "servers proven isolated: $(for s in "${ROLES[@]}"; do printf '%s@127.0.0.1:%s(%s) ' "$s" "${PORT[$s]}" "${BID[$s]}"; done)" | tee "$OUT/isolation.txt"

DYN="SELECT number AS id, multiIf(number % 3 = 0, toInt512(-toInt64(number))::Dynamic, number % 3 = 1, toUInt512(number)::Dynamic, toDecimal512(toString(number) || '.25', 2)::Dynamic) AS v FROM numbers(12)"
for s in OLD NEW; do
  for q in "CREATE DATABASE IF NOT EXISTS compat" \
           "CREATE TABLE compat.t_dyn_shared (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id" \
           "INSERT INTO compat.t_dyn_shared $DYN" \
           "CREATE TABLE compat.t_plain (id UInt32, i Int512, u UInt512, d Decimal512(2)) ENGINE = MergeTree ORDER BY id" \
           "INSERT INTO compat.t_plain SELECT number, toInt512(-toInt64(number)), toUInt512(number), toDecimal512(toString(number) || '.25', 2) FROM numbers(12)" \
           "CREATE TABLE compat.t_in (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id" \
           "CREATE TABLE compat.t_in_remote (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id"; do
    cl "$s" "$s" "$q" || { echo "setup failed on $s: $q"; exit 1; }
  done
done
cl NEW NEW "SELECT id, toString(v), dynamicType(v) FROM compat.t_dyn_shared ORDER BY id FORMAT TSV" > "$OUT/expected.tsv"
same() { diff -q "$1" "$2" > /dev/null && echo SAME || echo DIFF; }

for c in OLD NEW; do for s in OLD NEW; do
  st=sel_dyn.$c.$s; cl "$c" "$s" "SELECT id, v FROM compat.t_dyn_shared ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2> "$OUT/$st.err"; rc=$?
  [ $rc -eq 0 ] && { cut -f1,2 "$OUT/expected.tsv" > "$OUT/expected12.tsv"; st2="$st($(same "$OUT/expected12.tsv" "$OUT/$st.out"))"; } || st2=$st
  rec "$st2" "$c" "$s" $rc "$OUT/$st.out" "$OUT/$st.err"
  st=sel_plain.$c.$s; cl "$c" "$s" "SELECT id, i, u, d FROM compat.t_plain WHERE id IN (0, 1, 11) ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2> "$OUT/$st.err"; rec "$st" "$c" "$s" $? "$OUT/$st.out" "$OUT/$st.err"
done; done

for c in OLD NEW; do
  (cd "$OUT" && "${EXE[$c]}" local --query "$DYN FORMAT Native" < /dev/null > "$OUT/dyn.$c.native" 2> "$OUT/dyn.$c.native.err")
  for s in OLD NEW; do
    cl "$s" "$s" "TRUNCATE TABLE compat.t_in" > /dev/null 2>&1
    st=ins_dyn.$c.$s; st2=$st; cl "$c" "$s" "INSERT INTO compat.t_in FORMAT Native" "$OUT/dyn.$c.native" > /dev/null 2> "$OUT/$st.err"; rc=$?
    if [ $rc -eq 0 ]; then
      cl "$s" "$s" "SELECT id, toString(v), dynamicType(v) FROM compat.t_in ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2>> "$OUT/$st.err"; rc=$?
      [ $rc -eq 0 ] && st2="$st($(same "$OUT/expected.tsv" "$OUT/$st.out"))"
    fi
    rec "$st2" "$c" "$s" $rc "$OUT/$st.out" "$OUT/$st.err"
  done
done

for x in OLD NEW; do for y in OLD NEW; do [ "$x" = "$y" ] && continue
  st=remote_sel.$x.from.$y; st2=$st; cl "$x" "$x" "SELECT id, v FROM remote('127.0.0.1:${PORT[$y]}', compat, t_dyn_shared) ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2> "$OUT/$st.err"; rc=$?
  [ $rc -eq 0 ] && st2="$st($(same "$OUT/expected12.tsv" "$OUT/$st.out"))"
  rec "$st2" "$x" "$y" $rc "$OUT/$st.out" "$OUT/$st.err"
  cl "$y" "$y" "TRUNCATE TABLE compat.t_in_remote" > /dev/null 2>&1
  st=remote_ins.$x.to.$y; st2=$st; cl "$x" "$x" "INSERT INTO FUNCTION remote('127.0.0.1:${PORT[$y]}', compat, t_in_remote) SELECT id, v FROM compat.t_dyn_shared" > /dev/null 2> "$OUT/$st.err"; rc=$?
  if [ $rc -eq 0 ]; then
    cl "$y" "$y" "SELECT id, toString(v), dynamicType(v) FROM compat.t_in_remote ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2>> "$OUT/$st.err"; rc=$?
    [ $rc -eq 0 ] && st2="$st($(same "$OUT/expected.tsv" "$OUT/$st.out"))"
  fi
  rec "$st2" "$x" "$y" $rc "$OUT/$st.out" "$OUT/$st.err"
done; done

if [ -n "${EXE[UP]:-}" ]; then
  QB="SELECT 1 AS id, CAST(range(1, 17)::Array(Float32), 'QBit(Float32, 16, 8)') AS q"
  QD="SELECT number AS id, if(number = 0, 'x'::Dynamic, CAST(range(1, 17)::Array(Float32), 'QBit(Float32, 16, 8)')::Dynamic) AS v FROM numbers(2)"
  for s in NEW UP; do
    for q in "CREATE DATABASE IF NOT EXISTS compat" \
             "CREATE TABLE compat.t_qdyn (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id" \
             "INSERT INTO compat.t_qdyn $QD" \
             "CREATE TABLE compat.t_qin (id UInt32, v Dynamic(max_types = 1)) ENGINE = MergeTree ORDER BY id"; do
      cl "$s" "$s" "$q" || echo "qbit setup failed on $s: $q"
    done
  done
  for c in NEW UP; do for s in NEW UP; do [ "$c" = "$s" ] && continue
    st=qbit_plain.$c.$s; cl "$c" "$s" "$QB FORMAT TSV" > "$OUT/$st.out" 2> "$OUT/$st.err"; rec "$st" "$c" "$s" $? "$OUT/$st.out" "$OUT/$st.err"
    st=qbit_dyn_sel.$c.$s; cl "$c" "$s" "SELECT id, v FROM compat.t_qdyn ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2> "$OUT/$st.err"; rec "$st" "$c" "$s" $? "$OUT/$st.out" "$OUT/$st.err"
    (cd "$OUT" && "${EXE[$c]}" local --query "$QD FORMAT Native" < /dev/null > "$OUT/qdyn.$c.native" 2> /dev/null)
    cl "$s" "$s" "TRUNCATE TABLE compat.t_qin" > /dev/null 2>&1
    st=qbit_dyn_ins.$c.$s; cl "$c" "$s" "INSERT INTO compat.t_qin FORMAT Native" "$OUT/qdyn.$c.native" > /dev/null 2> "$OUT/$st.err"; rc=$?
    [ $rc -eq 0 ] && { cl "$s" "$s" "SELECT id, dynamicType(v), toString(v) FROM compat.t_qin ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2>> "$OUT/$st.err"; rc=$?; }
    rec "$st" "$c" "$s" $rc "$OUT/$st.out" "$OUT/$st.err"
    st=qbit_remote_sel.$s.from.$c; cl "$s" "$s" "SELECT id, v FROM remote('127.0.0.1:${PORT[$c]}', compat, t_qdyn) ORDER BY id FORMAT TSV" > "$OUT/$st.out" 2> "$OUT/$st.err"; rec "$st" "$s" "$c" $? "$OUT/$st.out" "$OUT/$st.err"
  done; done
fi
column -t -s $'\t' "$TSV" | cut -c1-260
