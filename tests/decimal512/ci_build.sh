#!/usr/bin/env bash
# ci_build.sh - build the clickhouse binary of the checked-out commit the way the Decimal512 release candidates are built,
# for the self-hosted binary job of .github/workflows/decimal512_checks.yml (tests/decimal512/run_checks.sh needs a
# build of the very commit it checks). Release, clang-22, no compiler cache, no tests or examples, heavy debug symbols
# omitted - the CMakeCache of the CAND9 build (build_release, 2026-09-25). A reused build directory is reconfigured, so
# the embedded GIT_HASH follows HEAD; the result is refused unless its GIT_HASH equals HEAD. Prints the binary path.
#
# PRODUCTION BOUNDARY: builds locally from the checkout; no network beyond what the build system itself needs for
# already-checked-out submodules (none is fetched here), no registry, no deployment.
#
#   tests/decimal512/ci_build.sh <build-dir> [--jobs N] [--dry-run]
#   --dry-run  check the prerequisites and print the configure/build commands, build nothing (exit 0 when ready)
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,/^set -euo/p' "$0" | sed '$d' >&2; exit 2; }
BUILD=$1; shift
JOBS=12 DRY=0
while [ $# -gt 0 ]; do
  case $1 in --jobs) JOBS=$2; shift 2;; --dry-run) DRY=1; shift;; *) echo "unknown argument $1" >&2; exit 2;; esac
done
export GIT_NO_LAZY_FETCH=1 GIT_OPTIONAL_LOCKS=0
SRC=$(git rev-parse --show-toplevel)
HEAD=$(git -C "$SRC" rev-parse HEAD)
missing=()
for t in cmake ninja clang-22 clang++-22 ld.lld-22; do command -v "$t" > /dev/null || missing+=("$t"); done
[ -f "$SRC/contrib/sysroot/linux-x86_64/x86_64-linux-gnu/libc/usr/include/stdio.h" ] || missing+=("submodule contrib/sysroot (checkout with submodules)")
if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]; then missing+=("a clean tree (tracked files are modified)"); fi
CONFIGURE=(cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-22
           -DCMAKE_CXX_COMPILER=clang++-22 -DCOMPILER_CACHE=disabled -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
           -DOMIT_HEAVY_DEBUG_SYMBOLS=ON -DPARALLEL_COMPILE_JOBS="$JOBS" -DPARALLEL_LINK_JOBS=2)
BUILD_CMD=(ninja -C "$BUILD" -k 0 -j"$JOBS" clickhouse)
if [ "$DRY" = 1 ]; then
  echo "HEAD $HEAD"
  echo "configure: ${CONFIGURE[*]}"
  echo "build:     ${BUILD_CMD[*]}"
  if [ ${#missing[@]} -gt 0 ]; then printf 'NOT READY: %s\n' "${missing[@]}"; exit 1; fi
  echo "READY"; exit 0
fi
[ ${#missing[@]} -eq 0 ] || { printf 'REFUSE: %s\n' "${missing[@]}" >&2; exit 2; }
"${CONFIGURE[@]}" >&2
"${BUILD_CMD[@]}" >&2
BIN=$BUILD/programs/clickhouse
GH=$(cd /tmp && "$BIN" local --query "SELECT value FROM system.build_options WHERE name = 'GIT_HASH'" < /dev/null)
[ "$GH" = "$HEAD" ] || { echo "REFUSE: the binary's GIT_HASH $GH is not HEAD $HEAD (stale build directory)" >&2; exit 1; }
echo "$BIN"
