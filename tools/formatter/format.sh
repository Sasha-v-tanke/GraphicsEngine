#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
IGNORE_FILE="${SCRIPT_DIR}/ignore"

CLANG_FORMAT_BIN="${CLANG_FORMAT_BIN:-clang-format}"
REQUIRED_CLANG_FORMAT_VERSION="23.1.0"

usage() {
    cat <<'EOF'
Usage:
  tools/formatter/run.sh

Environment:
  CLANG_FORMAT_BIN  Path or command name for clang-format.
                    Defaults to: clang-format
EOF
}

die() {
    printf 'formatter: error: %s\n' "$*" >&2
    exit 1
}

check_clang_format() {
    command -v "${CLANG_FORMAT_BIN}" >/dev/null 2>&1 \
        || die "clang-format executable not found: ${CLANG_FORMAT_BIN}"
}

load_ignore_patterns() {
    IGNORE_PATTERNS=()

    [[ -f "${IGNORE_FILE}" ]] || return 0

    while IFS= read -r line || [[ -n "${line}" ]]; do
        # Strip leading/trailing whitespace.
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"

        [[ -z "${line}" ]] && continue
        [[ "${line}" == \#* ]] && continue

        # Store repository-relative shell patterns.
        IGNORE_PATTERNS+=("${line}")
    done < "${IGNORE_FILE}"
}

is_ignored() {
    local relative_path="$1"
    local pattern

    for pattern in "${IGNORE_PATTERNS[@]-}"; do
        case "${relative_path}" in
            ${pattern})
                return 0
                ;;
        esac
    done

    return 1
}

collect_files() {
    FILES=()

    while IFS= read -r -d '' file; do
        local relative_path="${file#"${REPO_ROOT}/"}"

        if ! is_ignored "${relative_path}"; then
            FILES+=("${file}")
        fi
    done < <(
        find "${REPO_ROOT}" \
            -type f \
            \( \
                -name '*.h' \
                -o -name '*.hpp' \
                -o -name '*.hh' \
                -o -name '*.c' \
                -o -name '*.cc' \
                -o -name '*.cpp' \
                -o -name '*.cxx' \
            \) \
            -print0
    )
}

format_files() {
    if [[ "${#FILES[@]}" -eq 0 ]]; then
        printf 'formatter: no C/C++ files found\n'
        return 0
    fi

    printf 'formatter: formatting %d C/C++ files with clang-format %s\n' \
        "${#FILES[@]}" \
        "${REQUIRED_CLANG_FORMAT_VERSION}"

    "${CLANG_FORMAT_BIN}" -i --style=file "${FILES[@]}"

    printf 'formatter: done\n'
}

main() {
    if [[ "${#}" -ne 0 ]]; then
        usage
        exit 2
    fi

    check_clang_format
    load_ignore_patterns
    collect_files
    format_files
}

main "$@"
