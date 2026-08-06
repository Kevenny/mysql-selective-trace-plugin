#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh
#
# Builds the MySQL 8.0 server (minimal build needed to compile plugins
# against it) and then compiles only the plugin under src/ (+ the shared
# core/ sources it pulls in).
#
# Usage:
#   ./scripts/build.sh              # full build (first time, slower)
#   ./scripts/build.sh --plugin     # recompile the plugin only (incremental)
#   ./scripts/build.sh --package    # produce a test package (mysqld + plugin)
#   ./scripts/build.sh --fetch      # clone the MySQL source tree only
# ---------------------------------------------------------------------------
set -euo pipefail

MYSQL_VERSION="${MYSQL_VERSION:-8.0.40}"
MYSQL_SRC_DIR="${MYSQL_SRC_DIR:-/opt/mysql-src}"
BUILD_DIR="${BUILD_DIR:-/opt/mysql-build}"
BOOST_DIR="${BOOST_DIR:-/opt/boost}"
PLUGIN_LINK_NAME="selective_trace"
WORKSPACE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE="${1:-full}"

fetch_source() {
    if [ -d "${MYSQL_SRC_DIR}/.git" ]; then
        echo ">> MySQL source already present at ${MYSQL_SRC_DIR}"
        return
    fi
    echo ">> Cloning MySQL ${MYSQL_VERSION} (shallow, tag mysql-${MYSQL_VERSION})"
    git clone --branch "mysql-${MYSQL_VERSION}" --depth 1 \
        https://github.com/mysql/mysql-server.git "${MYSQL_SRC_DIR}"
}

link_plugin_source() {
    echo ">> Linking plugin source (src/) into the MySQL plugin tree"
    local target="${MYSQL_SRC_DIR}/plugin/${PLUGIN_LINK_NAME}"
    if [ ! -L "${target}" ]; then
        rm -rf "${target}"
        ln -s "${WORKSPACE_DIR}/src" "${target}"
        echo "   Link created: ${target} -> ${WORKSPACE_DIR}/src"
    else
        echo "   Link already exists: ${target}"
    fi
}

configure_cmake() {
    echo ">> Configuring build with CMake (Ninja + ccache)"
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"
    # DOWNLOAD_BOOST=1 fetches the exact Boost version MySQL 8.0 needs into
    # WITH_BOOST if it is not already cached there from a previous run.
    cmake "${MYSQL_SRC_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DDOWNLOAD_BOOST=1 \
        -DWITH_BOOST="${BOOST_DIR}" \
        -DWITH_UNIT_TESTS=OFF \
        -DWITH_ROUTER=OFF \
        -DCMAKE_CXX_STANDARD=17
}

build_full() {
    fetch_source
    link_plugin_source
    configure_cmake
    echo ">> Building MySQL + plugins (this can take 30 to 90+ minutes)"
    cd "${BUILD_DIR}"
    # BUILD_JOBS caps parallelism; memory-constrained runners can OOM-kill
    # the compiler linking several large C++ objects at once. Defaults to
    # all cores locally.
    local jobs="${BUILD_JOBS:-$(nproc)}"
    echo "   using -j${jobs}"
    ninja -j"${jobs}"
    echo ">> Full build finished."
}

build_plugin_only() {
    link_plugin_source
    if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
        echo "!! Build not configured yet. Run first: ./scripts/build.sh full"
        exit 1
    fi
    echo ">> Recompiling only the plugin ${PLUGIN_LINK_NAME}"
    cd "${BUILD_DIR}"
    cmake . >/dev/null   # re-scan in case of new .cc files
    ninja "${PLUGIN_LINK_NAME}"
    echo ">> Plugin recompiled."
}

package_for_test() {
    echo ">> Packaging the compiled plugin for the test container"
    local out_dir="${PLUGIN_OUTPUT_DIR:-${WORKSPACE_DIR}/build/plugin_output}"
    mkdir -p "${out_dir}"

    local so_path
    so_path=$(find "${BUILD_DIR}" -name "${PLUGIN_LINK_NAME}.so" | head -n1)

    if [ -z "${so_path}" ]; then
        echo "!! Plugin .so not found. Run the build first."
        exit 1
    fi

    cp "${so_path}" "${out_dir}/${PLUGIN_LINK_NAME}.so"
    echo ">> Plugin copied to: ${out_dir}/${PLUGIN_LINK_NAME}.so"
    echo ">> Start a MySQL 8.0 container with that file mounted into its"
    echo "   plugin_dir and run: INSTALL PLUGIN selective_trace SONAME"
    echo "   '${PLUGIN_LINK_NAME}.so';"
}

case "${MODE}" in
    full|"")
        build_full
        ;;
    --plugin)
        build_plugin_only
        ;;
    --package)
        package_for_test
        ;;
    --fetch)
        fetch_source
        ;;
    *)
        echo "Usage: $0 [full|--plugin|--package|--fetch]"
        exit 1
        ;;
esac
