#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
TEST_LOG="${TEST_DIR}/result/small.log"
: >"${TEST_LOG}"

# shellcheck source=../../common/common.sh
source "${TEST_DIR}/common/common.sh"

printf 'Test: small/cpp\n'

configure_project
build_project
run_ctest_label "cpp"

printf 'Test: small/cpp passed\n'