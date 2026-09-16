#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

readonly RESULT_DIR="${SCRIPT_DIR}/result"
readonly LATEST="${RESULT_DIR}/latest.json"
readonly BASELINE="${RESULT_DIR}/baseline.json"

"${SCRIPT_DIR}/run.sh"

if [[ ! -f "${LATEST}" ]]; then
    echo "Benchmark result not found: ${LATEST}"
    exit 1
fi

cp "${LATEST}" "${BASELINE}"

printf 'Baseline saved: %s\n' "${BASELINE}"
