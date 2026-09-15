#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -d "${SCRIPT_DIR}/result" ]]; then
    mkdir "${SCRIPT_DIR}/result/"
fi
TEST_LOG="${SCRIPT_DIR}/result/medium.log"
: >"${TEST_LOG}"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common/common.sh"

printf 'Test: medium\n'

configure_project
build_project
run_ctest_label "medium"

printf 'Test: medium passed\n'
