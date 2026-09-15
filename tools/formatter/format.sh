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
  tools/formatter/format.sh
  tools/formatter/format.sh <file> [file...]

Without arguments:
  Formats all C/C++ files in the repository.

With arguments:
  Formats only the specified C/C++ files.

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

    # shellcheck disable=SC2254
    for pattern in "${IGNORE_PATTERNS[@]-}"; do
        case "${relative_path}" in
            ${pattern})
                return 0
                ;;
        esac
    done

    return 1
}

is_cpp_file() {
    local file="$1"

    case "${file}" in
        *.h|*.hpp|*.hh|*.c|*.cc|*.cpp|*.cxx)
            return 0
            ;;
    esac

    return 1
}

add_file() {
    local file="$1"
    local absolute_path
    local relative_path
    local directory
    local existing_file

    # Deleted/renamed-away files may still be passed by Git.
    [[ -f "${file}" ]] || return 0

    directory="$(cd -- "$(dirname -- "${file}")" && pwd -P)"
    absolute_path="${directory}/$(basename -- "${file}")"

    case "${absolute_path}" in
        "${REPO_ROOT}"/*)
            ;;
        *)
            die "file is outside repository: ${file}"
            ;;
    esac

    relative_path="${absolute_path#"${REPO_ROOT}/"}"

    is_cpp_file "${relative_path}" || return 0
    is_ignored "${relative_path}" && return 0

    # Avoid formatting the same file twice.
    for existing_file in "${FILES[@]-}"; do
        [[ "${existing_file}" == "${absolute_path}" ]] && return 0
    done

    FILES+=("${absolute_path}")
}

collect_all_files() {
    FILES=()

    while IFS= read -r -d '' file; do
        add_file "${file}"
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

collect_requested_files() {
    FILES=()

    local file

    for file in "$@"; do
        if [[ "${file}" == /* ]]; then
            add_file "${file}"
        else
            add_file "${PWD}/${file}"
        fi
    done
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
    if [[ "${#}" -eq 1 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
        usage
        return 0
    fi

    check_clang_format
    load_ignore_patterns

    if [[ "${#}" -eq 0 ]]; then
        collect_all_files
    else
        collect_requested_files "$@"
    fi

    format_files
}

main "$@"