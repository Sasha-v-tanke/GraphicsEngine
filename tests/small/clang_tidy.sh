#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../common/common.sh
source "${SCRIPT_DIR}/../common/common.sh"

CLANG_TIDY_BIN="${CLANG_TIDY_BIN:-clang-tidy}"


die() {
    printf 'clang-tidy: error: %s\n' "$*" >&2
    exit 1
}

check_clang_tidy() {
    command -v "${CLANG_TIDY_BIN}" >/dev/null 2>&1 \
        || die "executable not found: ${CLANG_TIDY_BIN}"
}

collect_translation_units() {
    TRANSLATION_UNITS=()

    while IFS= read -r file; do
        case "${file}" in
            "${REPO_ROOT}"/external/* \
            | "${REPO_ROOT}"/third_party/* \
            | "${REPO_ROOT}"/vendor/* \
            | "${REPO_ROOT}"/generated/*)
                continue
                ;;
        esac

        TRANSLATION_UNITS+=("${file}")
    done < <(
        python3 - "${BUILD_DIR}/compile_commands.json" <<'PY'
import json
import os
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    commands = json.load(stream)

files = set()

for command in commands:
    path = command["file"]

    if not os.path.isabs(path):
        path = os.path.join(command["directory"], path)

    files.add(os.path.realpath(path))

for path in sorted(files):
    print(path)
PY
    )
}

run_tidy() {
    [[ -f "${BUILD_DIR}/compile_commands.json" ]] \
        || die "compile_commands.json not found in ${BUILD_DIR}"

    collect_translation_units

    if [[ "${#TRANSLATION_UNITS[@]}" -eq 0 ]]; then
        printf 'clang-tidy: no C/C++ translation units found\n'
        return 0
    fi

    local extra_args=()

    if [[ "$(uname -s)" == "Darwin" ]]; then
        command -v xcrun >/dev/null 2>&1 \
            || die "xcrun is required on macOS"

        local sdk_path
        sdk_path="$(xcrun --show-sdk-path)"

        extra_args+=(
            "--extra-arg-before=-isysroot"
            "--extra-arg-before=${sdk_path}"
        )
    fi

    printf 'clang-tidy: checking %d translation units\n' "${#TRANSLATION_UNITS[@]}"

    local file
    for file in "${TRANSLATION_UNITS[@]}"; do
        printf 'clang-tidy: %s\n' "${file#"${REPO_ROOT}/"}"

        "${CLANG_TIDY_BIN}" \
            -p "${BUILD_DIR}" \
            --config-file="${REPO_ROOT}/.clang-tidy" \
            --header-filter="${REPO_ROOT}/.*" \
            --exclude-header-filter="${REPO_ROOT}/(.*/)?(external|third_party|vendor|generated)/.*" \
            "${extra_args[@]}" \
            "${file}"
    done
}

main() {
    check_clang_tidy
    run_tidy
}

main "$@"
