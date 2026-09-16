#!/usr/bin/env python3

import json
import os
import sys
from pathlib import Path


RESET = "\033[0m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"

USE_COLOR = sys.stdout.isatty() and "NO_COLOR" not in os.environ

LABEL_WIDTH = 20
VALUE_WIDTH = 42


def color(text: str, code: str) -> str:
    if not USE_COLOR:
        return text

    return f"{code}{text}{RESET}"


def load_result(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def print_header(title: str) -> None:
    print()
    print(title)
    print(
        f"{'':<{LABEL_WIDTH}}"
        f"{'Baseline':<{VALUE_WIDTH}}"
        f"{'Latest':<{VALUE_WIDTH}}"
    )

    print("─" * (LABEL_WIDTH + VALUE_WIDTH * 2))


def print_setup(baseline: dict, latest: dict) -> None:
    baseline_context = baseline.get("context", {})
    latest_context = latest.get("context", {})

    fields = (
        ("Date", "date"),
        ("Commit", "git_commit"),
        ("Branch", "git_branch"),
        ("Dirty", "git_dirty"),
        ("Host", "host_name"),
        ("OS", "os"),
        ("OS release", "os_release"),
        ("Architecture", "architecture"),
        ("Build", "build_type"),
        ("Compiler", "compiler"),
    )

    print_header("Setup")

    for label, key in fields:
        baseline_value = str(baseline_context.get(key, "-"))
        latest_value = str(latest_context.get(key, "-"))

        baseline_text = f"{baseline_value:<{VALUE_WIDTH}}"
        latest_text = f"{latest_value:<{VALUE_WIDTH}}"

        if baseline_value != latest_value:
            baseline_text = color(baseline_text, YELLOW)
            latest_text = color(latest_text, YELLOW)

        print(
            f"{label:<{LABEL_WIDTH}}"
            f"{baseline_text}"
            f"{latest_text}"
        )


def collect_benchmarks(result: dict) -> dict:
    benchmarks = {}

    for benchmark in result.get("benchmarks", []):
        name = benchmark.get("name")

        if name is None:
            continue

        benchmarks[name] = benchmark

    return benchmarks


def format_time(value: float, unit: str) -> str:
    return f"{value:.3f} {unit}"


def print_results(baseline: dict, latest: dict) -> int:
    baseline_benchmarks = collect_benchmarks(baseline)
    latest_benchmarks = collect_benchmarks(latest)

    common_names = sorted(
        baseline_benchmarks.keys()
        & latest_benchmarks.keys()
    )

    if not common_names:
        print("No common benchmarks found.", file=sys.stderr)
        return 1

    print_header("Results")

    for name in common_names:
        baseline_benchmark = baseline_benchmarks[name]
        latest_benchmark = latest_benchmarks[name]

        baseline_time = baseline_benchmark["cpu_time"]
        latest_time = latest_benchmark["cpu_time"]

        baseline_unit = baseline_benchmark["time_unit"]
        latest_unit = latest_benchmark["time_unit"]

        baseline_text = format_time(
            baseline_time,
            baseline_unit,
        )

        if baseline_unit != latest_unit:
            latest_text = (
                f"{format_time(latest_time, latest_unit)} "
                "(different unit)"
            )

            latest_text = color(
                f"{latest_text:<{VALUE_WIDTH}}",
                YELLOW,
            )
        else:
            if baseline_time == 0:
                latest_text = format_time(
                    latest_time,
                    latest_unit,
                )
            else:
                change = (
                        (latest_time - baseline_time)
                        / baseline_time
                        * 100.0
                )

                latest_text = (
                    f"{format_time(latest_time, latest_unit)} "
                    f"{change:+.2f}%"
                )

                if change < 0:
                    latest_text = color(
                        f"{latest_text:<{VALUE_WIDTH}}",
                        GREEN,
                    )
                elif change > 0:
                    latest_text = color(
                        f"{latest_text:<{VALUE_WIDTH}}",
                        RED,
                    )
                else:
                    latest_text = (
                        f"{latest_text:<{VALUE_WIDTH}}"
                    )

        baseline_text = (
            f"{baseline_text:<{VALUE_WIDTH}}"
        )

        print(
            f"{name:<{LABEL_WIDTH}}"
            f"{baseline_text}"
            f"{latest_text}"
        )

    return 0


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    result_dir = script_dir / "result"

    baseline_path = result_dir / "baseline.json"
    latest_path = result_dir / "latest.json"

    if not baseline_path.is_file():
        print(
            f"Baseline not found: {baseline_path}",
            file=sys.stderr,
        )
        return 1

    if not latest_path.is_file():
        print(
            f"Latest result not found: {latest_path}",
            file=sys.stderr,
        )
        return 1

    baseline = load_result(baseline_path)
    latest = load_result(latest_path)

    print_setup(
        baseline,
        latest,
    )

    result = print_results(
        baseline,
        latest,
    )

    print()

    return result


if __name__ == "__main__":
    sys.exit(main())
