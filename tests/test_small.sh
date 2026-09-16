#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/common.sh
source "${SCRIPT_DIR}/common/common.sh"

SMALL_TESTS_DIR="${TEST_DIR}/small"
TEST_LOG="${LOGS_DIR}/test-small.log"
: >"${TEST_LOG}"

printf 'Test: small\n'

configure_project "ci-lite"
build_project

RunQuiet "Clang-Tidy" \
    bash "${SMALL_TESTS_DIR}/clang_tidy.sh"

RunQuiet "ShellCheck" \
    bash "${SMALL_TESTS_DIR}/shellcheck.sh"

run_ctest_label "small"

printf 'Test: small passed\n'
