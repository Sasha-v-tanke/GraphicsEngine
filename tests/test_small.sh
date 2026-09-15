#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TEST_LOG="${SCRIPT_DIR}/result/small.log"
: >"${TEST_LOG}"

# shellcheck source=common.sh
source "${SCRIPT_DIR}/common/common.sh"

printf 'Test: small\n'

configure_project
build_project

RunQuiet "Clang-Tidy" \
  bash "${SCRIPT_DIR}/small/clang_tidy.sh"

run_ctest_label "small"

printf 'Test: small passed\n'
