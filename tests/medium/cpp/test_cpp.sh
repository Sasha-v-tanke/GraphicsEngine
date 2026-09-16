#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=../../common/common.sh
source "${SCRIPT_DIR}/../../common/common.sh"

TEST_LOG="${LOGS_DIR}/test-medium-cpp.log"
: >"${TEST_LOG}"

printf 'Test: medium/cpp\n'

configure_project "ci-vulkan-glfw"
build_project

run_ctest_label "cpp"

printf 'Test: medium/cpp passed\n'
