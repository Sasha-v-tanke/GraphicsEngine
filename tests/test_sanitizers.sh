#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "${SCRIPT_DIR}/result"

TEST_LOG="${SCRIPT_DIR}/result/sanitizers.log"
: >"${TEST_LOG}"

# shellcheck source=common/common.sh
source "${SCRIPT_DIR}/common/common.sh"

printf 'Test: sanitizers\n'

configure_project_with \
    -DGRAPHICS_ENGINE_ENABLE_ASAN=ON \
    -DGRAPHICS_ENGINE_ENABLE_UBSAN=ON
build_project

run_ctest_label "small"

printf 'Test: sanitizers passed\n'