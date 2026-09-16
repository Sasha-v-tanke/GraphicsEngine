#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/common.sh
source "${SCRIPT_DIR}/common/common.sh"

# HEAVY_TESTS_DIR="${TEST_DIR}/heavy"
TEST_LOG="${LOGS_DIR}/test-heavy.log"
: >"${TEST_LOG}"

printf 'Test: heavy\n'

configure_project "ci-vulkan-glfw"
build_project

run_ctest_label "heavy"

printf 'Test: heavy passed\n'
