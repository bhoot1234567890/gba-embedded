#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


SUITE_RE = re.compile(r"^--- Suite (\d+): (.+) ---$")
FRAMES_RE = re.compile(
    r"^Frames:\s+(\d+)(?:\s+PC=0x([0-9A-Fa-f]+)\s+CPSR=0x([0-9A-Fa-f]+)\s+halted=(\d+)\s+cycle=(\d+))?$"
)
BOOT_RE = re.compile(
    r"^After (\d+) frames: PC=0x([0-9A-Fa-f]+)\s+CPSR=0x([0-9A-Fa-f]+)\s+halted=(\d+)\s+cycle=(\d+)$"
)
ROM_RE = re.compile(r"^ROM:\s+(.*?)(?:\s+\((\d+) bytes\))?$")
BIOS_RE = re.compile(r"^BIOS:\s+(.*?)(?:\s+\((\d+) bytes\))?$")
END_RE = re.compile(r"^END:\s*(\d+)\s*/\s*(\d+)$")
PASS_RE = re.compile(r"^PASS:\s*(.+)$")
FAIL_RE = re.compile(r"^FAIL:\s*(.+)$")
RESULT_RE = re.compile(r"^(.*?):(?:\s+Got .*?)?\s*:\s*(PASS|FAIL)$")
SUMMARY_TOTAL_RE = re.compile(r"^Total:\s*(\d+)\s*/\s*(\d+)\s+passed$")


@dataclass
class ResultCounts:
    passed: int | None = None
    total: int | None = None
    source: str | None = None


@dataclass
class SuiteRecord:
    index: int
    name: str
    frames: int | None = None
    pc: str | None = None
    cpsr: str | None = None
    halted: bool | None = None
    cycle: int | None = None
    mgba_begin: str | None = None
    mgba_end_passed: int | None = None
    mgba_end_total: int | None = None
    mgba_pass_count: int = 0
    mgba_fail_count: int = 0
    sram_pass_count: int = 0
    sram_fail_count: int = 0
    mgba_fail_examples: list[str] = field(default_factory=list)
    sram_fail_examples: list[str] = field(default_factory=list)
    sram_lines: list[str] = field(default_factory=list)
    mgba_lines: list[str] = field(default_factory=list)
    normalized: ResultCounts = field(default_factory=ResultCounts)


@dataclass
class RunRecord:
    path: str
    rom: str | None = None
    bios: str | None = None
    boot_frames: int | None = None
    boot_pc: str | None = None
    boot_cpsr: str | None = None
    boot_halted: bool | None = None
    boot_cycle: int | None = None
    summary_lines: list[str] = field(default_factory=list)
    suites: list[SuiteRecord] = field(default_factory=list)
    total_passed: int | None = None
    total_tests: int | None = None
    completed: bool = False


def strip_debug_prefix(line: str) -> str | None:
    if line.startswith("mGBA log: "):
        return line[len("mGBA log: ") :]
    if line.startswith("[DBG] "):
        return line[len("[DBG] ") :]
    return None


def maybe_take_example(target: list[str], text: str, limit: int = 5) -> None:
    if text and len(target) < limit:
        target.append(text)


def parse_mgba_lines(lines: Iterable[str], suite: SuiteRecord) -> None:
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("BEGIN: "):
            suite.mgba_begin = stripped[len("BEGIN: ") :]
            continue

        end_match = END_RE.match(stripped)
        if end_match:
            suite.mgba_end_passed = int(end_match.group(1))
            suite.mgba_end_total = int(end_match.group(2))
            continue

        pass_match = PASS_RE.match(stripped)
        if pass_match:
            suite.mgba_pass_count += 1
            continue

        fail_match = FAIL_RE.match(stripped)
        if fail_match:
            suite.mgba_fail_count += 1
            maybe_take_example(suite.mgba_fail_examples, fail_match.group(1))
            continue


def parse_sram_line(line: str, suite: SuiteRecord) -> None:
    stripped = line.strip()
    result_match = RESULT_RE.match(stripped)
    if not result_match:
        return

    label = result_match.group(1).strip()
    outcome = result_match.group(2)
    if outcome == "PASS":
        suite.sram_pass_count += 1
    else:
        suite.sram_fail_count += 1
        maybe_take_example(suite.sram_fail_examples, label)


def finalize_suite(suite: SuiteRecord) -> None:
    if suite.mgba_end_total is not None:
        suite.normalized = ResultCounts(
            passed=suite.mgba_end_passed,
            total=suite.mgba_end_total,
            source="mgba_end",
        )
        return

    mgba_total = suite.mgba_pass_count + suite.mgba_fail_count
    if mgba_total:
        suite.normalized = ResultCounts(
            passed=suite.mgba_pass_count,
            total=mgba_total,
            source="mgba",
        )
        return

    suite.normalized = ResultCounts()


def parse_log(path: Path) -> RunRecord:
    record = RunRecord(path=str(path))
    pending_mgba_lines: list[str] = []
    current_suite: SuiteRecord | None = None
    in_sram_block = False
    in_summary = False

    lines = path.read_text(errors="replace").splitlines()
    for raw_line in lines:
        line = raw_line.rstrip("\n")

        if in_sram_block and (
            line.startswith("mGBA log: ")
            or line.startswith("[DBG] ")
            or line.startswith("--- Suite ")
            or line.startswith("=== Summary ===")
        ):
            in_sram_block = False

        if in_summary:
            if line:
                record.summary_lines.append(line)
                total_match = SUMMARY_TOTAL_RE.match(line)
                if total_match:
                    record.total_passed = int(total_match.group(1))
                    record.total_tests = int(total_match.group(2))
                if line == "Done.":
                    record.completed = True
            continue

        rom_match = ROM_RE.match(line)
        if rom_match:
            record.rom = rom_match.group(1)
            continue

        bios_match = BIOS_RE.match(line)
        if bios_match:
            record.bios = bios_match.group(1)
            continue

        boot_match = BOOT_RE.match(line)
        if boot_match:
            record.boot_frames = int(boot_match.group(1))
            record.boot_pc = f"0x{boot_match.group(2).upper()}"
            record.boot_cpsr = f"0x{boot_match.group(3).upper()}"
            record.boot_halted = bool(int(boot_match.group(4)))
            record.boot_cycle = int(boot_match.group(5))
            continue

        if line == "=== Summary ===":
            in_summary = True
            continue

        suite_match = SUITE_RE.match(line)
        if suite_match:
            current_suite = SuiteRecord(index=int(suite_match.group(1)), name=suite_match.group(2))
            current_suite.mgba_lines = pending_mgba_lines
            parse_mgba_lines(pending_mgba_lines, current_suite)
            pending_mgba_lines = []
            record.suites.append(current_suite)
            continue

        frames_match = FRAMES_RE.match(line)
        if frames_match and current_suite is not None:
            current_suite.frames = int(frames_match.group(1))
            if frames_match.group(2):
                current_suite.pc = f"0x{frames_match.group(2).upper()}"
                current_suite.cpsr = f"0x{frames_match.group(3).upper()}"
                current_suite.halted = bool(int(frames_match.group(4)))
                current_suite.cycle = int(frames_match.group(5))
            continue

        if line == "[sram]":
            in_sram_block = True
            continue

        if in_sram_block and current_suite is not None:
            current_suite.sram_lines.append(line)
            parse_sram_line(line, current_suite)
            continue

        mgba_line = strip_debug_prefix(line)
        if mgba_line is not None:
            pending_mgba_lines.append(mgba_line)
            continue

    for suite in record.suites:
        finalize_suite(suite)

    if record.summary_lines and record.total_tests is not None:
        record.completed = True

    if record.total_passed is None:
        known_suites = [suite for suite in record.suites if suite.normalized.total is not None]
        if known_suites and len(known_suites) == len(record.suites):
            record.total_passed = sum(suite.normalized.passed or 0 for suite in known_suites)
            record.total_tests = sum(suite.normalized.total or 0 for suite in known_suites)

    return record


def print_run_summary(record: RunRecord, show_examples: int) -> None:
    print(f"Run: {record.path}")
    if record.rom:
        print(f"  ROM: {record.rom}")
    if record.bios:
        print(f"  BIOS: {record.bios}")
    if record.boot_frames is not None:
        print(
            f"  Boot: frames={record.boot_frames} pc={record.boot_pc} "
            f"cpsr={record.boot_cpsr} halted={int(bool(record.boot_halted))} cycle={record.boot_cycle}"
        )
    print(f"  Suites parsed: {len(record.suites)}")
    if record.total_tests is not None:
        print(f"  Overall: {record.total_passed}/{record.total_tests} passed")
    elif record.summary_lines:
        print(f"  Summary: {' | '.join(record.summary_lines)}")
    else:
        print("  Summary: unavailable")

    for suite in record.suites:
        norm = suite.normalized
        result_text = "unknown"
        if norm.total is not None:
            result_text = f"{norm.passed}/{norm.total} passed via {norm.source}"
        elif suite.sram_fail_count or suite.sram_pass_count:
            result_text = (
                f"no normalized total; sram_fail_lines={suite.sram_fail_count}, "
                f"sram_pass_lines={suite.sram_pass_count}"
            )

        frame_text = f"frames={suite.frames}" if suite.frames is not None else "frames=?"
        state_parts = [frame_text]
        if suite.pc:
            state_parts.append(f"pc={suite.pc}")
        if suite.halted is not None:
            state_parts.append(f"halted={int(suite.halted)}")
        if suite.cycle is not None:
            state_parts.append(f"cycle={suite.cycle}")

        print(f"  Suite {suite.index:02d} {suite.name}: {result_text} ({', '.join(state_parts)})")

        examples: list[str] = []
        if suite.sram_fail_examples:
            examples.extend(f"sram:{text}" for text in suite.sram_fail_examples[:show_examples])
        if len(examples) < show_examples and suite.mgba_fail_examples:
            remaining = show_examples - len(examples)
            examples.extend(f"mgba:{text}" for text in suite.mgba_fail_examples[:remaining])
        if examples:
            print(f"    Examples: {' | '.join(examples)}")


def print_compare(records: list[RunRecord]) -> None:
    if len(records) < 2:
        return

    by_name = [Path(record.path).stem for record in records]
    all_suite_numbers = sorted({suite.index for record in records for suite in record.suites})

    print("Comparison:")
    for suite_index in all_suite_numbers:
        print(f"  Suite {suite_index:02d}")
        for label, record in zip(by_name, records):
            suite = next((item for item in record.suites if item.index == suite_index), None)
            if suite is None:
                print(f"    {label}: missing")
                continue

            norm = suite.normalized
            if norm.total is None:
                if suite.sram_fail_count or suite.sram_pass_count:
                    result = (
                        f"no normalized total; "
                        f"sram_fail_lines={suite.sram_fail_count} sram_pass_lines={suite.sram_pass_count}"
                    )
                else:
                    result = "unknown"
            else:
                fails = (norm.total - (norm.passed or 0))
                result = f"{norm.passed}/{norm.total} passed, {fails} failed ({norm.source})"

            extras = []
            if suite.frames is not None:
                extras.append(f"frames={suite.frames}")
            if suite.pc:
                extras.append(f"pc={suite.pc}")
            if suite.halted is not None:
                extras.append(f"halted={int(suite.halted)}")
            print(f"    {label}: {suite.name} -> {result}" + (f"; {' '.join(extras)}" if extras else ""))


def main() -> int:
    parser = argparse.ArgumentParser(description="Parse and compare gba-suite runner logs.")
    parser.add_argument("logs", nargs="+", type=Path, help="Suite runner log files to parse")
    parser.add_argument("--json", action="store_true", help="Emit parsed records as JSON")
    parser.add_argument("--compare", action="store_true", help="Print a suite-by-suite comparison")
    parser.add_argument(
        "--show-examples",
        type=int,
        default=3,
        help="How many failure examples to print per suite in the text summary",
    )
    args = parser.parse_args()

    records = [parse_log(path) for path in args.logs]

    if args.json:
        print(json.dumps([asdict(record) for record in records], indent=2))
        return 0

    for index, record in enumerate(records):
        if index:
            print()
        print_run_summary(record, max(args.show_examples, 0))

    if args.compare:
        print()
        print_compare(records)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
