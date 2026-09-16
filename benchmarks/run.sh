#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

readonly PRESET="benchmark"
readonly BUILD_DIR="${REPO_DIR}/cmake-build-${PRESET}"
readonly BENCHMARK="${BUILD_DIR}/GraphicsEngineBenchmarks"

readonly RESULT_DIR="${SCRIPT_DIR}/result"
readonly RESULT="${RESULT_DIR}/latest.json"

readonly CMAKE_CACHE="${BUILD_DIR}/CMakeCache.txt"

ReadCMakeCache() {
    local key="$1"

    sed -n \
        "s/^${key}:[^=]*=//p" \
        "${CMAKE_CACHE}" \
        | head -n 1
}

CollectMetaData() {
    gitCommit="$(
        git -C "${REPO_DIR}" rev-parse HEAD
    )"

    gitBranch="$(
        git -C "${REPO_DIR}" rev-parse --abbrev-ref HEAD
    )"

    if [[ -n "$(git -C "${REPO_DIR}" status --porcelain)" ]]; then
        gitDirty="true"
    else
        gitDirty="false"
    fi

    buildType="$(
        ReadCMakeCache "CMAKE_BUILD_TYPE"
    )"

    compiler="$(
        ReadCMakeCache "CMAKE_CXX_COMPILER"
    )"

    compilerVersionOutput="$(
        "${compiler}" --version
    )"

    compilerVersion="${compilerVersionOutput%%$'\n'*}"

    os="$(
        uname -s
    )"

    osRelease="$(
        uname -r
    )"

    architecture="$(
        uname -m
    )"

    contextArgs=(
        "--benchmark_context=git_commit=${gitCommit}"
        "--benchmark_context=git_branch=${gitBranch}"
        "--benchmark_context=git_dirty=${gitDirty}"
        "--benchmark_context=build_type=${buildType}"
        "--benchmark_context=compiler=${compilerVersion}"
        "--benchmark_context=os=${os}"
        "--benchmark_context=os_release=${osRelease}"
        "--benchmark_context=architecture=${architecture}"
    )
}

cd "${REPO_DIR}"

cmake \
    --preset "${PRESET}" \
    -S "${REPO_DIR}"

cmake \
    --build \
    --preset "${PRESET}"

CollectMetaData

mkdir -p "${RESULT_DIR}"

BENCHMARK_OUT="${RESULT}" \
BENCHMARK_OUT_FORMAT="json" \
exec "${BENCHMARK}" \
    "${contextArgs[@]}" \
    "$@"
