#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${REPO_ROOT}/../.." && pwd)"
TEST_DIR="${REPO_ROOT}/tests/"

BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/cmake-build-debug}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"

configure_project() {
    RunQuiet "Configure" \
        cmake \
        -S "${REPO_ROOT}" \
        -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
}

build_project() {
    RunQuiet "Build" \
        cmake --build "${BUILD_DIR}"
}

run_ctest_label() {
    local label="$1"

    if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
        printf 'test: no CTest tests are registered; skipping label "%s"\n' "${label}"
        return 0
    fi

    ctest \
        --test-dir "${BUILD_DIR}" \
        --output-on-failure \
        --label-regex "^${label}$"
}

RunQuiet() {
    local name="$1"
    shift

    printf '%s' "${name}"
    printf '\n===== %s =====\n' "${name}" >>"${TEST_LOG}"

    if "$@" >> "${TEST_LOG}" 2>&1; then
        printf ' — OK\n'
        return 0
    fi

    printf ' — FAILED\n\n'
    printf 'log: %s\n' "${TEST_LOG}"
    return 1
}