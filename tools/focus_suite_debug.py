#!/usr/bin/env python3

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import re
import subprocess
import sys
from collections import Counter
from dataclasses import asdict
from datetime import datetime
from pathlib import Path
from typing import Any


FAIL_BUCKET_RE = re.compile(
    r"^(1xs|16xs|1xv|16xv) ([^:]+): Got ([0-9A-Fa-f]+) vs ([0-9A-Fa-f]+): FAIL$"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build the focused GBA suite runner workflow, save logs, parse them, "
            "and print the timer-failure buckets we keep checking by hand."
        )
    )
    parser.add_argument(
        "--suite",
        dest="display_suites",
        action="append",
        type=int,
        default=[],
        help="Printed suite number (1-based, as shown in runner output). Can be repeated.",
    )
    parser.add_argument(
        "--runner-suite",
        dest="runner_suites",
        action="append",
        type=int,
        default=[],
        help="Raw zero-based --suite index accepted by gba_suite_runner. Can be repeated.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip `cmake --build` and only run the selected suites.",
    )
    parser.add_argument(
        "--phase",
        type=str,
        help="Convenience alias for `--tag phaseN`.",
    )
    parser.add_argument(
        "--tag",
        type=str,
        help="Log/report tag. Defaults to a timestamp if omitted.",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="Build directory passed to CMake.",
    )
    parser.add_argument(
        "--runner",
        type=Path,
        default=Path("build/gba_suite_runner"),
        help="Suite runner executable path.",
    )
    parser.add_argument(
        "--parser",
        type=Path,
        default=Path("tools/parse_gba_suite_logs.py"),
        help="Parser module used for summaries and JSON export.",
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=Path("tools/logs"),
        help="Directory where logs, parsed JSON, and reports are written.",
    )
    parser.add_argument(
        "--tail",
        type=int,
        default=20,
        help="How many trailing log lines to include in the console/report summary.",
    )
    parser.add_argument(
        "--show-examples",
        type=int,
        default=3,
        help="How many parsed failure examples to include per suite summary.",
    )
    parser.add_argument(
        "--compare-latest",
        action="store_true",
        help="Compare each new log against the most recent earlier log for the same suite.",
    )
    return parser.parse_args()


def load_parser_module(parser_path: Path) -> Any:
    spec = importlib.util.spec_from_file_location("parse_gba_suite_logs", parser_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load parser module from {parser_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def format_command(args: list[str]) -> str:
    return " ".join(args)


def run_command(args: list[str], cwd: Path, stdout_path: Path | None = None) -> int:
    if stdout_path is None:
        completed = subprocess.run(args, cwd=cwd, check=False)
        return completed.returncode

    with stdout_path.open("w", encoding="utf-8") as output:
        completed = subprocess.run(
            args,
            cwd=cwd,
            check=False,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
    return completed.returncode


def unique_runner_indices(display_suites: list[int], runner_suites: list[int]) -> list[int]:
    indices: list[int] = []
    seen: set[int] = set()

    for display_suite in display_suites:
        if display_suite <= 0:
            raise ValueError(f"Displayed suite numbers must be >= 1, got {display_suite}")
        runner_index = display_suite - 1
        if runner_index not in seen:
            seen.add(runner_index)
            indices.append(runner_index)

    for runner_suite in runner_suites:
        if runner_suite < 0:
            raise ValueError(f"Runner suite indices must be >= 0, got {runner_suite}")
        if runner_suite not in seen:
            seen.add(runner_suite)
            indices.append(runner_suite)

    if not indices:
        return [3, 4]
    return indices


def capture_summary(parser_module: Any, record: Any, show_examples: int) -> str:
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        parser_module.print_run_summary(record, show_examples)
    return buffer.getvalue().rstrip()


def capture_compare(parser_module: Any, records: list[Any]) -> str:
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        parser_module.print_compare(records)
    return buffer.getvalue().rstrip()


def latest_previous_log(log_dir: Path, suite_number: int, current_log: Path) -> Path | None:
    pattern = f"suite{suite_number:02d}_*.log"
    candidates = [path for path in log_dir.glob(pattern) if path != current_log]
    if not candidates:
        return None
    return max(candidates, key=lambda path: path.stat().st_mtime)


def tail_lines(path: Path, count: int) -> list[str]:
    if count <= 0:
        return []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    return lines[-count:]


def analyze_fail_buckets(path: Path) -> tuple[Counter[tuple[str, str], int], Counter[tuple[str, int], int]]:
    by_preface_and_kind: Counter[tuple[str, str], int] = Counter()
    by_kind_and_delta: Counter[tuple[str, int], int] = Counter()

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = FAIL_BUCKET_RE.match(line.strip())
        if not match:
            continue
        kind, preface, got_hex, expected_hex = match.groups()
        got = int(got_hex, 16)
        expected = int(expected_hex, 16)
        by_preface_and_kind[(preface, kind)] += 1
        by_kind_and_delta[(kind, got - expected)] += 1

    return by_preface_and_kind, by_kind_and_delta


def fail_count(path: Path) -> int:
    return sum(1 for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if "FAIL" in line)


def suite_log_path(log_dir: Path, runner_index: int, tag: str) -> Path:
    suite_number = runner_index + 1
    return log_dir / f"suite{suite_number:02d}_{tag}.log"


def suite_json_path(log_path: Path) -> Path:
    return log_path.with_suffix(".parsed.json")


def report_path(log_dir: Path, tag: str) -> Path:
    return log_dir / f"focus_{tag}_report.txt"


def summary_json_path(log_dir: Path, tag: str) -> Path:
    return log_dir / f"focus_{tag}_summary.json"


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    build_dir = (repo_root / args.build_dir).resolve()
    runner_path = (repo_root / args.runner).resolve()
    parser_path = (repo_root / args.parser).resolve()
    log_dir = (repo_root / args.log_dir).resolve()
    log_dir.mkdir(parents=True, exist_ok=True)

    tag = args.tag or (f"phase{args.phase}" if args.phase else datetime.now().strftime("%Y%m%d_%H%M%S"))
    runner_indices = unique_runner_indices(args.display_suites, args.runner_suites)
    parser_module = load_parser_module(parser_path)

    report_lines: list[str] = []
    summary_payload: dict[str, Any] = {
        "tag": tag,
        "repo_root": str(repo_root),
        "runner": str(runner_path),
        "build_dir": str(build_dir),
        "logs": [],
    }

    print(f"Tag: {tag}")
    print(f"Suites: {', '.join(str(index + 1) for index in runner_indices)}")

    if not args.skip_build:
        build_cmd = ["cmake", "--build", str(build_dir), "-j4"]
        print(f"$ {format_command(build_cmd)}")
        build_rc = run_command(build_cmd, repo_root)
        report_lines.append(f"$ {format_command(build_cmd)}")
        report_lines.append(f"build_exit_code={build_rc}")
        report_lines.append("")
        if build_rc != 0:
            report_lines.append("Build failed; suite runs were skipped.")
            report_path(log_dir, tag).write_text("\n".join(report_lines).rstrip() + "\n", encoding="utf-8")
            print(f"Build failed. Report saved to {report_path(log_dir, tag)}")
            return build_rc

    for runner_index in runner_indices:
        suite_number = runner_index + 1
        log_path = suite_log_path(log_dir, runner_index, tag)
        cmd = [str(runner_path), "--suite", str(runner_index)]
        print(f"$ {format_command(cmd)} > {log_path}")
        rc = run_command(cmd, repo_root, stdout_path=log_path)

        parsed_record = parser_module.parse_log(log_path)
        suite_json = suite_json_path(log_path)
        suite_json.write_text(json.dumps(asdict(parsed_record), indent=2), encoding="utf-8")

        current_tail = tail_lines(log_path, args.tail)
        current_fail_count = fail_count(log_path)
        preface_counts, delta_counts = analyze_fail_buckets(log_path)
        summary_text = capture_summary(parser_module, parsed_record, args.show_examples)

        report_lines.append(f"=== Suite {suite_number:02d} ===")
        report_lines.append(f"command: {format_command(cmd)}")
        report_lines.append(f"log: {log_path}")
        report_lines.append(f"parsed_json: {suite_json}")
        report_lines.append(f"exit_code: {rc}")
        report_lines.append(f"fail_lines: {current_fail_count}")
        report_lines.append(summary_text)

        print()
        print(f"Suite {suite_number:02d}")
        print(f"  log: {log_path}")
        print(f"  parsed json: {suite_json}")
        print(f"  exit code: {rc}")
        print(f"  FAIL lines: {current_fail_count}")

        if current_tail:
            report_lines.append("tail:")
            print("  tail:")
            for line in current_tail:
                report_lines.append(f"  {line}")
                print(f"    {line}")

        if preface_counts:
            report_lines.append("fail buckets by preface:")
            print("  top fail buckets by preface:")
            for (preface, kind), count in preface_counts.most_common(12):
                line = f"  {kind} {preface}: {count}"
                report_lines.append(line)
                print(f"    {kind} {preface}: {count}")

        if delta_counts:
            report_lines.append("fail deltas:")
            print("  top fail deltas:")
            for (kind, delta), count in delta_counts.most_common(12):
                line = f"  {kind} delta {delta:+d}: {count}"
                report_lines.append(line)
                print(f"    {kind} delta {delta:+d}: {count}")

        compare_text = ""
        compare_path = None
        if args.compare_latest:
            compare_path = latest_previous_log(log_dir, suite_number, log_path)
            if compare_path is not None:
                previous_record = parser_module.parse_log(compare_path)
                compare_text = capture_compare(parser_module, [previous_record, parsed_record])
                report_lines.append(f"compare_latest: {compare_path}")
                report_lines.append(compare_text)
                print(f"  compare latest: {compare_path}")
                for line in compare_text.splitlines():
                    print(f"    {line}")
            else:
                report_lines.append("compare_latest: unavailable")
                print("  compare latest: unavailable")

        report_lines.append("")
        summary_payload["logs"].append(
            {
                "suite_number": suite_number,
                "runner_index": runner_index,
                "log_path": str(log_path),
                "parsed_json_path": str(suite_json),
                "exit_code": rc,
                "fail_lines": current_fail_count,
                "tail": current_tail,
                "preface_counts": [
                    {"preface": preface, "kind": kind, "count": count}
                    for (preface, kind), count in preface_counts.most_common()
                ],
                "delta_counts": [
                    {"kind": kind, "delta": delta, "count": count}
                    for (kind, delta), count in delta_counts.most_common()
                ],
                "compare_path": str(compare_path) if compare_path else None,
                "summary": asdict(parsed_record),
            }
        )

        if rc != 0:
            print(f"Suite {suite_number:02d} exited with code {rc}; stopping.")
            break

    report_file = report_path(log_dir, tag)
    report_file.write_text("\n".join(report_lines).rstrip() + "\n", encoding="utf-8")

    summary_file = summary_json_path(log_dir, tag)
    summary_file.write_text(json.dumps(summary_payload, indent=2), encoding="utf-8")

    print()
    print(f"Report: {report_file}")
    print(f"Summary JSON: {summary_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
