#!/usr/bin/env python3
"""Discover GraphicsEngine benchmark binaries and their registered cases.

This module deliberately does not configure, build, or execute benchmarks.  It
only reads approved CMake presets and asks an already built Google Benchmark
binary for its supported ``--benchmark_list_tests`` output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from pathlib import Path
import platform
import re
import subprocess
import sys
from typing import Iterable


SCHEMA_VERSION = 1
BENCHMARK_TARGET = "GraphicsEngineBenchmarks"


@dataclass(frozen=True)
class Diagnostic:
    code: str
    message: str


@dataclass(frozen=True)
class CatalogStatus:
    state: str
    diagnostic: Diagnostic | None = None


def _utc_timestamp(path: Path) -> str:
    return datetime.fromtimestamp(path.stat().st_mtime, UTC).isoformat().replace("+00:00", "Z")


def _file_fingerprint(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def _platform() -> dict:
    system = platform.system()
    machine = platform.machine()
    return {"system": system, "machine": machine, "supported": system in {"Darwin", "Linux", "Windows"} and bool(machine)}


def _load_preset_document(repo: Path) -> dict:
    path = repo / "CMakePresets.json"
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error

    return document


def _load_presets(repo: Path) -> dict[str, dict]:
    document = _load_preset_document(repo)
    return {
        preset["name"]: preset
        for preset in document.get("buildPresets", [])
        if isinstance(preset, dict) and isinstance(preset.get("name"), str)
    }


def discover_profiles(repo: Path) -> list[dict]:
    """Return only explicit build presets; never manufacture a build directory."""
    return [
        {"buildPreset": name, "configurePreset": preset.get("configurePreset")}
        for name, preset in sorted(_load_presets(repo).items())
        if isinstance(preset.get("configurePreset"), str)
    ]


def _configure_preset(repo: Path, name: str, resolving: frozenset[str] = frozenset()) -> dict | None:
    presets = {
        preset["name"]: preset
        for preset in _load_preset_document(repo).get("configurePresets", [])
        if isinstance(preset, dict) and isinstance(preset.get("name"), str)
    }
    preset = presets.get(name)
    if preset is None or name in resolving:
        return None
    inherited = preset.get("inherits", [])
    if isinstance(inherited, str):
        inherited = [inherited]
    resolved: dict = {"cacheVariables": {}}
    for parent in inherited:
        if not isinstance(parent, str):
            continue
        inherited_preset = _configure_preset(repo, parent, resolving | {name})
        if inherited_preset is not None:
            resolved.update(inherited_preset)
            resolved["cacheVariables"].update(inherited_preset.get("cacheVariables", {}))
    cache_variables = preset.get("cacheVariables", {})
    if not isinstance(cache_variables, dict):
        cache_variables = {}
    resolved.update({key: value for key, value in preset.items() if key != "cacheVariables"})
    resolved["cacheVariables"] = dict(resolved.get("cacheVariables", {}))
    resolved["cacheVariables"].update(cache_variables)
    return resolved


def _profile(repo: Path, build_preset: str) -> tuple[dict | None, CatalogStatus | None]:
    try:
        preset = _load_presets(repo).get(build_preset)
    except ValueError as error:
        return None, CatalogStatus("error", Diagnostic("presets_unreadable", str(error)))
    if preset is None:
        return None, CatalogStatus(
            "unsupported", Diagnostic("unknown_build_preset", f"Build preset '{build_preset}' is not allowed."),
        )
    configure_preset = preset.get("configurePreset")
    if not isinstance(configure_preset, str):
        return None, CatalogStatus(
            "unsupported", Diagnostic("missing_configure_preset", f"Build preset '{build_preset}' has no configure preset."),
        )
    configure = _configure_preset(repo, configure_preset)
    if configure is None:
        return None, CatalogStatus("unsupported", Diagnostic("configure_preset_unresolvable", configure_preset))
    enabled = configure.get("cacheVariables", {}).get("GRAPHICS_ENGINE_BUILD_BENCHMARKS")
    if isinstance(enabled, dict):
        enabled = enabled.get("value")
    if str(enabled).upper() not in {"ON", "TRUE", "1", "YES"}:
        return None, CatalogStatus(
            "unsupported",
            Diagnostic("benchmarks_disabled", f"Build preset '{build_preset}' does not enable GRAPHICS_ENGINE_BUILD_BENCHMARKS."),
        )
    return {"buildPreset": build_preset, "configurePreset": configure_preset}, None


def _binary_path(repo: Path, configure_preset: str) -> Path | None:
    preset = _configure_preset(repo, configure_preset)
    if preset is None or not isinstance(preset.get("binaryDir"), str):
        return None
    binary_dir = preset["binaryDir"].replace("${sourceDir}", str(repo)).replace("${presetName}", configure_preset)
    directory = Path(binary_dir)
    if not directory.is_absolute():
        directory = repo / directory
    suffix = ".exe" if os.name == "nt" else ""
    return directory / f"{BENCHMARK_TARGET}{suffix}"


def _case(name: str) -> dict:
    group, separator, parameters = name.partition("/")
    return {
        "id": "case:" + name,
        "name": name,
        "group": group,
        "parameters": parameters if separator else None,
        "availability": "ready",
    }


def _list_cases(binary: Path, timeout_seconds: float, benchmark_filter: str | None = None) -> tuple[list[str] | None, CatalogStatus | None]:
    command = [str(binary), "--benchmark_list_tests"]
    if benchmark_filter is not None:
        command.append("--benchmark_filter=" + benchmark_filter)
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired:
        return None, CatalogStatus("error", Diagnostic("list_tests_timeout", "Listing benchmark cases timed out."))
    except OSError as error:
        return None, CatalogStatus("error", Diagnostic("list_tests_start_failed", str(error)))
    if completed.returncode != 0:
        stderr = completed.stderr.strip()
        return None, CatalogStatus(
            "error",
            Diagnostic("list_tests_failed", stderr or f"benchmark exited with {completed.returncode}"),
        )
    return [line.strip() for line in completed.stdout.splitlines() if line.strip()], None


def _benchmark_inputs(repo: Path) -> Iterable[Path]:
    yield repo / "CMakeLists.txt"
    yield repo / "CMakePresets.json"
    yield repo / "cmake" / "utils" / "benchmarks.cmake"
    benchmarks = repo / "benchmarks"
    if benchmarks.is_dir():
        yield from (path for path in benchmarks.rglob("*") if path.is_file() and path.suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".cmake"})


def _stale_input(repo: Path, binary: Path) -> Path | None:
    binary_time = binary.stat().st_mtime
    inputs = [binary.parent / "CMakeCache.txt", *_benchmark_inputs(repo)]
    return next((path for path in inputs if path.is_file() and path.stat().st_mtime > binary_time), None)


def build_catalog(repo: Path, build_preset: str, timeout_seconds: float = 10.0) -> dict:
    """Create a versioned immutable snapshot of a selected build profile."""
    repo = repo.resolve()
    profile, status = _profile(repo, build_preset)
    generated_at = datetime.now(UTC).isoformat().replace("+00:00", "Z")
    host_platform = _platform()
    base = {
        "schemaVersion": SCHEMA_VERSION,
        "generatedAt": generated_at,
        "platform": host_platform,
        "profile": profile or {"buildPreset": build_preset},
        "status": asdict(status) if status else None,
        "binary": None,
        "cases": [],
    }
    if status:
        return _version(base)
    if not host_platform["supported"]:
        base["status"] = asdict(CatalogStatus("unsupported", Diagnostic("unsupported_platform", host_platform["system"])))
        return _version(base)

    binary = _binary_path(repo, profile["configurePreset"])
    if binary is None:
        base["status"] = asdict(CatalogStatus("unsupported", Diagnostic("configure_preset_unresolvable", profile["configurePreset"])))
        return _version(base)
    if not binary.is_file():
        base["status"] = asdict(CatalogStatus("not_built", Diagnostic("benchmark_binary_missing", str(binary))))
        return _version(base)

    stale_input = _stale_input(repo, binary)
    if stale_input is not None:
        base["status"] = asdict(CatalogStatus("stale", Diagnostic("benchmark_input_newer_than_binary", str(stale_input))))
        return _version(base)

    names, status = _list_cases(binary, timeout_seconds)
    if status:
        base["status"] = asdict(status)
        return _version(base)

    base["binary"] = {"path": str(binary), "fingerprint": _file_fingerprint(binary), "modifiedAt": _utc_timestamp(binary)}
    base["cases"] = [_case(name) for name in names]
    base["status"] = asdict(CatalogStatus("ready"))
    return _version(base)


def _version(catalog: dict) -> dict:
    material_catalog = dict(catalog)
    material_catalog.pop("generatedAt", None)
    material = json.dumps(material_catalog, sort_keys=True, separators=(",", ":")).encode("utf-8")
    catalog["catalogVersion"] = "sha256:" + hashlib.sha256(material).hexdigest()
    return catalog


def exact_filter(case_names: Iterable[str]) -> str:
    """Build a POSIX ERE accepted by Google Benchmark for exact full-name matches."""
    names = list(case_names)
    if not names or len(names) != len(set(names)) or any(not name for name in names):
        raise ValueError("selection must contain one or more distinct non-empty benchmark names")
    expression = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    compiled = re.compile(expression)
    if any(not compiled.fullmatch(name) for name in names):
        raise ValueError("selection cannot be represented as an exact filter")
    return expression


def validate_selection(catalog: dict, names: Iterable[str]) -> str:
    selected = list(names)
    if catalog["status"]["state"] != "ready":
        raise ValueError("benchmark selection requires a ready catalog")
    known_names = {case["name"] for case in catalog["cases"]}
    unknown_names = sorted(set(selected) - known_names)
    if unknown_names:
        raise ValueError("unknown benchmark case: " + ", ".join(unknown_names))
    return exact_filter(selected)


def verify_exact_selection(catalog: dict, names: Iterable[str], timeout_seconds: float) -> str:
    selected = list(names)
    expression = validate_selection(catalog, selected)
    listed, status = _list_cases(Path(catalog["binary"]["path"]), timeout_seconds, expression)
    if status is not None:
        raise ValueError(status.diagnostic.message)
    if len(listed) != len(selected) or set(listed) != set(selected):
        raise ValueError("Google Benchmark filter did not resolve to exactly the selected cases")
    return expression


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--build-preset", default="benchmark")
    parser.add_argument("--timeout-seconds", type=float, default=10.0)
    parser.add_argument("--select", action="append", default=[], metavar="CASE")
    parser.add_argument("--list-profiles", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    try:
        output = {"profiles": discover_profiles(arguments.repo.resolve())} if arguments.list_profiles else build_catalog(arguments.repo, arguments.build_preset, arguments.timeout_seconds)
        if arguments.select:
            if arguments.list_profiles:
                raise ValueError("--select cannot be used with --list-profiles")
            output["exactFilter"] = verify_exact_selection(output, arguments.select, arguments.timeout_seconds)
    except (OSError, ValueError) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        return 2
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
