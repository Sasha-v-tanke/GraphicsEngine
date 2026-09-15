#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

SHELLCHECK_BIN="${SHELLCHECK_BIN:-shellcheck}"

if ! command -v "${SHELLCHECK_BIN}" >/dev/null 2>&1; then
    printf 'shellcheck: executable not found: %s\n' "${SHELLCHECK_BIN}" >&2
    exit 1
fi

FILES=()

while IFS= read -r -d '' file; do
    FILES+=("${REPO_ROOT}/${file}")
done < <(
    git \
        -C "${REPO_ROOT}" \
        ls-files \
        -z \
        -- \
        '*.sh'
)

if [[ "${#FILES[@]}" -eq 0 ]]; then
    printf 'shellcheck: no shell files found\n'
    exit 0
fi

"${SHELLCHECK_BIN}" \
    -x \
    -P SCRIPTDIR \
    "${FILES[@]}"