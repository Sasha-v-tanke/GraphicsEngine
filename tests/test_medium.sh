#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/common.sh
source "${SCRIPT_DIR}/common/common.sh"

# MEDIUM_TESTS_DIR="${TEST_DIR}/medium"
TEST_LOG="${LOGS_DIR}/test-medium.log"
: >"${TEST_LOG}"

printf 'Test: medium\n'

configure_project "ci-vulkan-glfw"
build_project

run_ctest_label "medium"

printf 'Test: medium passed\n'
