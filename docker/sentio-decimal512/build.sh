#!/usr/bin/env bash
# build.sh - build the release image of the Sentio fork LOCALLY from a binary built from the checked-out commit, with
# identity labels written from that binary, and record the chain source commit -> binary -> image.
#
# PRODUCTION BOUNDARY: builds and inspects a local image only. It never pushes, never logs in to a registry and never
# deploys. The image name is a local-only repository (sentio-local/...), not a registry path. The containers it runs
# for verification use --network none and --pull never. The package step of the build downloads Ubuntu packages from
# the public snapshot archive (snapshot.ubuntu.com); nothing else is fetched.
#
#   docker/sentio-decimal512/build.sh --binary <clickhouse> --out <new or empty evidence dir> [--tag <local name:tag>]
#       [--allow-dirty]    build from a tree with local modifications (the labels then say dirty=true; never a release)
#
# Refuses (exit 1): a tree with modified tracked files (unless --allow-dirty), a binary whose embedded GIT_HASH is not
# HEAD (a stale build directory) or that does not contain the HEAD commit id, an image whose /usr/bin/clickhouse is
# not the given binary. Usage errors exit 2.
# Evidence in --out: identity.json (source, binary, image, labels), image_inspect.json, image_binary_sha256.txt,
# image_version.txt, layout_parity.txt (config files and package versions compared with layout.expected: the image
# deployed on 2026-09-16), build.log.
set -u
export GIT_NO_LAZY_FETCH=1 GIT_OPTIONAL_LOCKS=0
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../.." && pwd)
usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d' >&2; exit 2; }
BIN= OUT= TAG= DIRTY_OK=0
while [ $# -gt 0 ]; do
  case $1 in
    --binary) BIN=$2; shift 2;;
    --out) OUT=$2; shift 2;;
    --tag) TAG=$2; shift 2;;
    --allow-dirty) DIRTY_OK=1; shift;;
    *) usage;;
  esac
done
[ -n "$BIN" ] && [ -x "$BIN" ] && [ -n "$OUT" ] || usage
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ]; then echo "ERROR: --out $OUT is not empty" >&2; exit 2; fi
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); BIN=$(readlink -f "$BIN")
log() { echo "[$(date -u +%FT%TZ)] $*" | tee -a "$OUT/build.log" >&2; }
die() { log "REFUSE: $*"; exit 1; }

# 1. the source: HEAD, and whether any tracked file differs from it. A path that `git status` lists only because of a
#    .gitattributes eol rule (its raw bytes hash to the committed blob) is not a modification.
HEAD=$(git -C "$TREE" rev-parse HEAD) || die "not a git checkout: $TREE"
BRANCH=$(git -C "$TREE" rev-parse --abbrev-ref HEAD)
real_dirty=()
while IFS= read -r line; do
  p=${line:3}
  committed=$(git -C "$TREE" rev-parse -q --verify "HEAD:$p" 2>/dev/null) || { real_dirty+=("$p"); continue; }
  [ -f "$TREE/$p" ] && [ "$(git -C "$TREE" hash-object --no-filters -- "$p")" = "$committed" ] && continue
  real_dirty+=("$p")
done < <(git -C "$TREE" status --porcelain --untracked-files=no)
DIRTY=false
if [ ${#real_dirty[@]} -gt 0 ]; then
  DIRTY=true
  printf '  modified: %s\n' "${real_dirty[@]}" | head -20 | tee -a "$OUT/build.log" >&2
  [ $DIRTY_OK = 1 ] || die "${#real_dirty[@]} tracked file(s) differ from HEAD $HEAD (commit them, or --allow-dirty for a non-release image)"
fi
log "source $HEAD ($BRANCH) dirty=$DIRTY"

# 2. the binary: sha256, build-id, and what it reports about itself (clickhouse local, private cwd, no ports)
BSHA=$(sha256sum "$BIN" | cut -d' ' -f1)
BBID=$(readelf -n "$BIN" 2>/dev/null | awk '/Build ID/{print $3}')
W=$(mktemp -d "$OUT/ident.XXXXXX")
read -r BVER BGIT BTYPE < <(cd "$W" && timeout 120 "$BIN" local --query "SELECT version(), (SELECT value FROM system.build_options WHERE name = 'GIT_HASH'), (SELECT value FROM system.build_options WHERE name = 'BUILD_TYPE')" < /dev/null 2>/dev/null | tr '\t' ' ')
rm -rf "$W"
log "binary $BIN sha256=$BSHA build-id=$BBID version=$BVER GIT_HASH=$BGIT BUILD_TYPE=$BTYPE"
[ -n "$BBID" ] || die "the binary has no GNU build-id"
[ "$BGIT" = "$HEAD" ] || die "the binary reports GIT_HASH $BGIT, not HEAD $HEAD (stale build directory: reconfigure and rebuild)"
grep -qaF "$HEAD" "$BIN" || die "the commit id $HEAD does not occur in the binary"

# 3. the build context: the binary, the entrypoint and configs from the tree, the config.d files kept here
SHORT=${HEAD:0:11}
[ -n "$TAG" ] || TAG=sentio-local/clickhouse-server:26.3-lts-decimal512-$SHORT
case $TAG in sentio-local/*) ;; *) die "the tag must be a local-only name under sentio-local/ (got $TAG): this script never builds a pushable registry path";; esac
CTX=$(mktemp -d "${TMPDIR:-/tmp}/sentio-ctx.XXXXXX")
trap 'rm -rf "$CTX"' EXIT
ln "$BIN" "$CTX/clickhouse" 2>/dev/null || cp "$BIN" "$CTX/clickhouse"
cp "$TREE/docker/server/entrypoint.sh" "$CTX/entrypoint.sh"
mkdir -p "$CTX/clickhouse-server/config.d" "$CTX/clickhouse-client"
cp "$TREE/programs/server/config.xml" "$TREE/programs/server/users.xml" "$CTX/clickhouse-server/"
cp "$TREE/docker/server/docker_related_config.xml" "$HERE"/config.d/*.xml "$CTX/clickhouse-server/config.d/"
cp "$HERE/Dockerfile" "$CTX/Dockerfile"

# 4. build (the base is pinned by digest and must already be local; only the apt step uses the network)
docker image inspect "$(sed -n 's/^ARG BASE_IMAGE=//p' "$HERE/Dockerfile")" > /dev/null 2>&1 || die "the pinned base image is not in the local cache"
DOCKER_BUILDKIT=1 docker build --pull=false -t "$TAG" \
  --build-arg SOURCE_COMMIT="$HEAD" --build-arg SOURCE_BRANCH="$BRANCH" --build-arg BINARY_SHA256="$BSHA" \
  --build-arg BINARY_BUILD_ID="$BBID" --build-arg BINARY_GIT_HASH="$BGIT" --build-arg BINARY_VERSION="$BVER" \
  --build-arg BUILD_TYPE="$BTYPE" --label "sentio.source.dirty=$DIRTY" "$CTX" >> "$OUT/build.log" 2>&1 || die "docker build failed (see build.log)"

# 5. verify and record: image id, the binary inside, labels, layout parity with the deployed image
docker image inspect "$TAG" > "$OUT/image_inspect.json"
IID=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["Id"])' "$OUT/image_inspect.json")
timeout 300 docker run --rm --pull never --network none --entrypoint sha256sum "$TAG" /usr/bin/clickhouse > "$OUT/image_binary_sha256.txt" 2>> "$OUT/build.log"
ISHA=$(cut -d' ' -f1 "$OUT/image_binary_sha256.txt")
timeout 300 docker run --rm --pull never --network none --entrypoint /usr/bin/clickhouse "$TAG" local \
  --query "SELECT version(), buildId(), (SELECT value FROM system.build_options WHERE name = 'GIT_HASH')" > "$OUT/image_version.txt" 2>> "$OUT/build.log"
[ "$ISHA" = "$BSHA" ] || die "the image's /usr/bin/clickhouse has sha256 $ISHA, not the binary $BSHA"
timeout 300 docker run --rm --pull never --network none --entrypoint /bin/sh "$TAG" -c \
  'cd /etc/clickhouse-server && find . -type f | sort | xargs sha256sum; dpkg-query -W -f="pkg \${Package}=\${Version}\n" ca-certificates locales tzdata wget openssl libssl3' \
  > "$OUT/layout_actual.txt" 2>> "$OUT/build.log"
if diff <(grep -v '^#' "$HERE/layout.expected") "$OUT/layout_actual.txt" > "$OUT/layout_diff.txt"; then
  echo "layout: identical to layout.expected (config files and package versions of the image deployed on 2026-09-16)" > "$OUT/layout_parity.txt"
else
  { echo "layout: differs from layout.expected (see layout_diff.txt):"; cat "$OUT/layout_diff.txt"; } > "$OUT/layout_parity.txt"
fi
python3 - "$OUT" "$HEAD" "$BRANCH" "$DIRTY" "$BIN" "$BSHA" "$BBID" "$BVER" "$BGIT" "$BTYPE" "$TAG" "$IID" "$ISHA" <<'EOF'
import json, sys
out, head, branch, dirty, binp, bsha, bbid, bver, bgit, btype, tag, iid, isha = sys.argv[1:14]
labels = json.load(open(f"{out}/image_inspect.json"))[0]["Config"]["Labels"]
ok = labels.get("sentio.git.commit") == head and labels.get("sentio.binary.sha256") == bsha and isha == bsha
json.dump({"source": {"commit": head, "branch": branch, "dirty": dirty == "true", "basis": "git rev-parse HEAD; tracked files compared with HEAD by raw hash"},
           "binary": {"path": binp, "sha256": bsha, "build_id": bbid, "version": bver, "reported_git_hash": bgit, "build_type": btype},
           "image": {"tag": tag, "id": iid, "binary_sha256_inside": isha, "labels": labels, "pushed": False},
           "chain_consistent": ok}, open(f"{out}/identity.json", "w"), indent=1)
print(("OK" if ok else "INCONSISTENT") + f": source {head} -> binary {bsha[:16]} (build-id {bbid[:12]}) -> image {iid[:19]} [{tag}]")
EOF
log "image $TAG id=$IID binary inside sha256=$ISHA; $(cat "$OUT/layout_parity.txt" | head -1)"
