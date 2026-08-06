# Dockerfile — build environment for the selective_trace MySQL 8.0+ plugin
#
# Produces a selective_trace.so linked against the glibc of Oracle Linux 8,
# loadable into an official MySQL 8.0.x server running on OL8/RHEL8/Rocky8/
# Alma8 and their 9-series successors (glibc is forward compatible: an EL8
# binary runs on EL9, not the other way round). Mirrors the sibling MariaDB
# project's docker/Dockerfile.ol8 — see t:\mariadb-selective-log-plugin.
FROM oraclelinux:8

ENV MYSQL_VERSION=8.0.40
ENV MYSQL_SRC_DIR=/opt/mysql-src
ENV BUILD_DIR=/opt/mysql-build

# ---------------------------------------------------------------------------
# Build dependencies. MySQL 8.0 requires C++17 (server code itself moved to
# C++17 in the 8.0 series) — the native OL8 gcc (8.5) is too old; use
# gcc-toolset-12, same choice as the MariaDB build. MySQL's CMake can
# download Boost itself (-DDOWNLOAD_BOOST=1); we still install the system
# boost-devel as a fallback / to speed up repeated builds via the bundled
# source cache.
# ---------------------------------------------------------------------------
RUN dnf -y install oracle-epel-release-el8 dnf-plugins-core && \
    dnf config-manager --set-enabled ol8_codeready_builder && \
    dnf -y install \
        gcc-toolset-12-gcc \
        gcc-toolset-12-gcc-c++ \
        gcc-toolset-12-binutils \
        cmake \
        ninja-build \
        ccache \
        git \
        bison \
        flex \
        wget \
        ncurses-devel \
        readline-devel \
        openssl-devel \
        zlib-devel \
        libaio-devel \
        pam-devel \
        libtirpc-devel \
        libxml2-devel \
        libedit-devel \
        numactl-devel \
        rpcgen \
        which \
        diffutils \
        procps-ng \
        perl-interpreter \
        perl-Memoize \
        perl-Time-HiRes \
        perl-JSON-PP \
        perl-Data-Dumper \
        perl-Getopt-Long \
        perl-Digest-MD5 \
        perl-Env \
    && dnf clean all

# gcc-toolset-12 ahead of PATH (no need for `scl enable`)
ENV PATH="/opt/rh/gcc-toolset-12/root/usr/bin:${PATH}"

RUN useradd -u 1000 -ms /bin/bash devuser \
    && mkdir -p ${MYSQL_SRC_DIR} ${BUILD_DIR} /opt/ccache /opt/boost \
    && chown -R devuser:devuser ${MYSQL_SRC_DIR} ${BUILD_DIR} /opt/ccache \
                /opt/boost

WORKDIR /workspace
USER devuser

ENV CCACHE_DIR=/opt/ccache
ENV BOOST_DIR=/opt/boost

CMD ["/bin/bash"]
