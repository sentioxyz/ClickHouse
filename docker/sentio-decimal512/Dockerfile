# syntax=docker/dockerfile:1
# Release image of the Sentio ClickHouse fork (26.3-lts-decimal512). Build it with build.sh next to this file, never
# by hand: build.sh reads the identity of the binary (sha256, GNU build-id, the GIT_HASH and version it reports),
# refuses a binary that was not built from the checked-out commit, and passes that identity as the labels below, so a
# label cannot name another commit than the binary inside the image.
#
# Layout: the same as the image deployed on 2026-09-16 (clickhouse-server:26.3-lts-decimal512-release-2026-09-16):
#   * the same ubuntu:22.04 base (pinned by digest; its only layer is the base layer of that image),
#   * the same packages at the same versions, installed from the Ubuntu snapshot archive at the time of that build,
#   * /usr/bin/clickhouse and the clickhouse-* tool links,
#   * docker/server/entrypoint.sh, programs/server/{config,users}.xml and docker/server/docker_related_config.xml from
#     the source tree, plus the four config.d files that `clickhouse install` writes (copied in config.d/ here),
#   * an empty /etc/clickhouse-client, the same users, directories, environment, ports and volume.
ARG BASE_IMAGE=ubuntu:22.04@sha256:b8b6ee6aa931ecd9d0d952abc34dc0e5f7c6a30c6bb71b079fe399fde0329c02
FROM ${BASE_IMAGE}
ARG BASE_IMAGE
ARG DEBIAN_FRONTEND=noninteractive
ARG APT_SNAPSHOT=20260916T145500Z
# snapshot.ubuntu.com redirects to HTTPS, and the base image has no CA certificates before this step installs them:
# TLS peer verification is off for this step only. apt still verifies the archive's GPG signatures (InRelease), which
# is what protects the default http://archive.ubuntu.com sources of the base image as well.

RUN groupadd -r -g 101 clickhouse \
 && useradd -r -u 101 -g clickhouse -s /bin/false -d /var/lib/clickhouse clickhouse \
 && cp /etc/apt/sources.list /etc/apt/sources.list.orig \
 && printf 'deb http://snapshot.ubuntu.com/ubuntu/%s %s main restricted universe multiverse\n' \
      "$APT_SNAPSHOT" jammy "$APT_SNAPSHOT" jammy-updates "$APT_SNAPSHOT" jammy-security > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false -o Acquire::https::Verify-Peer=false update \
 && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates locales tzdata wget \
 && locale-gen en_US.UTF-8 \
 && mv /etc/apt/sources.list.orig /etc/apt/sources.list \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

COPY --chown=clickhouse:clickhouse --chmod=0555 clickhouse /usr/bin/clickhouse
COPY --chown=root:root --chmod=0775 entrypoint.sh /entrypoint.sh
COPY --chown=clickhouse:clickhouse clickhouse-server /etc/clickhouse-server
COPY --chown=clickhouse:clickhouse clickhouse-client /etc/clickhouse-client

RUN for tool in benchmark chdig client compressor disks extract-from-config \
        format git-import keeper keeper-converter local obfuscator server; do \
        ln -sf /usr/bin/clickhouse /usr/bin/clickhouse-$tool; \
    done \
 && mkdir -p /var/lib/clickhouse /var/log/clickhouse-server \
 && chown clickhouse:clickhouse /var/lib/clickhouse /var/log/clickhouse-server \
 && chmod 0777 /var/lib/clickhouse /var/log/clickhouse-server

ENV LANG=en_US.UTF-8 TZ=UTC CLICKHOUSE_CONFIG=/etc/clickhouse-server/config.xml
EXPOSE 9000 8123 9009
VOLUME /var/lib/clickhouse
ENTRYPOINT ["/entrypoint.sh"]

ARG SOURCE_COMMIT=unset
ARG SOURCE_BRANCH=unset
ARG BINARY_SHA256=unset
ARG BINARY_BUILD_ID=unset
ARG BINARY_GIT_HASH=unset
ARG BINARY_VERSION=unset
ARG BUILD_TYPE=unset
LABEL org.opencontainers.image.title="clickhouse-server 26.3-lts-decimal512" \
      org.opencontainers.image.description="Sentio ClickHouse fork, built from ${SOURCE_COMMIT} (labels written by docker/sentio-decimal512/build.sh from the binary)" \
      org.opencontainers.image.revision="${SOURCE_COMMIT}" \
      org.opencontainers.image.version="${BINARY_VERSION}" \
      org.opencontainers.image.base.name="${BASE_IMAGE}" \
      sentio.git.commit="${SOURCE_COMMIT}" \
      sentio.git.branch="${SOURCE_BRANCH}" \
      sentio.build.type="${BUILD_TYPE}" \
      sentio.binary.sha256="${BINARY_SHA256}" \
      sentio.binary.build_id="${BINARY_BUILD_ID}" \
      sentio.binary.git_hash="${BINARY_GIT_HASH}" \
      sentio.apt.snapshot="${APT_SNAPSHOT}"
