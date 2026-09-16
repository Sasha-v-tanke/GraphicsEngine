#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${REPO_ROOT}/../.." && pwd)"
TEST_DIR="${REPO_ROOT}/tests"
LOGS_DIR="${TEST_DIR}/result"

mkdir -p "${LOGS_DIR}"

CMAKE_PRESET="${CMAKE_PRESET:-}"
BUILD_DIR="${BUILD_DIR:-}"

run_in_repo() {
    (
        cd "${REPO_ROOT}"
        "$@"
    )
}

configure_project() {
    local preset="$1"

    CMAKE_PRESET="${preset}"
    BUILD_DIR="${REPO_ROOT}/cmake-build-${preset}"

    export CMAKE_PRESET
    export BUILD_DIR

    RunQuiet "Configure" \
        run_in_repo \
        cmake \
        --preset "${CMAKE_PRESET}"
}

build_project() {
    if [[ -z "${CMAKE_PRESET}" ]]; then
        printf 'build: no CMake preset configured\n' >&2
        return 1
    fi

    RunQuiet "Build" \
        run_in_repo \
        cmake \
        --build \
        --preset "${CMAKE_PRESET}"
}

run_ctest_label() {
    local label="$1"
    local log_file="${TEST_LOG:-}"

    if [[ -z "${BUILD_DIR}" ]]; then
        printf 'test: project is not configured\n' >&2
        return 1
    fi

    if [[ -z "${log_file}" ]]; then
        log_file="${LOGS_DIR}/ctest-${label}.log"
    fi

    if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
        printf 'test: no CTest tests are registered; skipping label "%s"\n' "${label}"
        return 0
    fi

    local start_time="${SECONDS}"
    local started_at
    started_at="$(date '+%Y-%m-%d %H:%M:%S %z')"

    printf 'CTest %s' "${label}"
    {
        printf '\n===== CTest %s =====\n' "${label}"
        printf 'Started: %s\n' "${started_at}"
    } >>"${log_file}"

    if ctest \
        --test-dir "${BUILD_DIR}" \
        --output-on-failure \
        --label-regex "^${label}$" \
        >>"${log_file}" 2>&1; then

        local elapsed=$((SECONDS - start_time))
        local finished_at
        finished_at="$(date '+%Y-%m-%d %H:%M:%S %z')"

        {
            printf 'Finished: %s\n' "${finished_at}"
            printf 'Duration: %ss\n' "${elapsed}"
            printf 'Result: OK\n'
        } >>"${log_file}"

        printf ' — OK (%ss)\n' "${elapsed}"
        return 0
    fi

    local elapsed=$((SECONDS - start_time))
    local finished_at
    finished_at="$(date '+%Y-%m-%d %H:%M:%S %z')"

    {
        printf 'Finished: %s\n' "${finished_at}"
        printf 'Duration: %ss\n' "${elapsed}"
        printf 'Result: FAILED\n'
    } >>"${log_file}"

    printf ' — FAILED (%ss)\n\n' "${elapsed}"
    printf 'log: %s\n' "${log_file}"
    return 1
}

RunQuiet() {
    local name="$1"
    shift

    local start_time="${SECONDS}"
    local started_at
    started_at="$(date '+%Y-%m-%d %H:%M:%S %z')"

    printf '%s' "${name}"
    {
        printf '\n===== %s =====\n' "${name}"
        printf 'Started: %s\n' "${started_at}"
    } >>"${TEST_LOG}"

    if "$@" >> "${TEST_LOG}" 2>&1; then
        local elapsed=$((SECONDS - start_time))
        local finished_at
        finished_at="$(date '+%Y-%m-%d %H:%M:%S %z')"

         {
            printf 'Finished: %s\n' "${finished_at}"
            printf 'Duration: %ss\n' "${elapsed}"
            printf 'Result: OK\n'
        } >>"${TEST_LOG}"

        printf ' — OK (%ss)\n' "${elapsed}"
        return 0
    fi

    local elapsed=$((SECONDS - start_time))
    local finished_at
    finished_at="$(date '+%Y-%m-%d %H:%M:%S %z')"

    {
        printf 'Finished: %s\n' "${finished_at}"
        printf 'Duration: %ss\n' "${elapsed}"
        printf 'Result: FAILED\n'
    } >>"${TEST_LOG}"

    printf ' — FAILED (%ss)\n\n' "${elapsed}"
    printf 'log: %s\n' "${TEST_LOG}"
    return 1
}
