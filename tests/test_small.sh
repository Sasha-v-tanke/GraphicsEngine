#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -d "${SCRIPT_DIR}/result" ]]; then
    mkdir "${SCRIPT_DIR}/result/"
fi
TEST_LOG="${SCRIPT_DIR}/result/small.log"
: >"${TEST_LOG}"

# shellcheck source=common/common.sh
source "${SCRIPT_DIR}/common/common.sh"

printf 'Test: small\n'

configure_project
build_project

RunQuiet "Clang-Tidy" \
  bash "${SCRIPT_DIR}/small/clang_tidy.sh"

RunQuiet "ShellCheck" \
  bash "${SCRIPT_DIR}/small/shellcheck.sh"

run_ctest_label "small"

printf 'Test: small passed\n'
