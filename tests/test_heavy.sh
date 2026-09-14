#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common/common.sh"

printf 'Test: heavy\n'

configure_project
build_project
run_ctest_label "heavy"

printf 'Test: heavy passed\n'
