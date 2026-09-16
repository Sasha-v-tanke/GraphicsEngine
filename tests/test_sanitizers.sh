#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common/common.sh
source "${SCRIPT_DIR}/common/common.sh"

TEST_LOG="${LOGS_DIR}/sanitizers.log"
: >"${TEST_LOG}"

printf 'Test: sanitizers\n'

configure_project "ci-sanitizers"
build_project

run_ctest_label "small"

printf 'Test: sanitizers passed\n'
