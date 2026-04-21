#!/usr/bin/env python3
"""Compare two CPU traces and show mismatch context with decoded instructions."""
from __future__ import annotations

import argparse
import csv
import struct
import sys
from pathlib import Path

MODE_NAMES = {16: "USR", 17: "FIQ", 18: "IRQ", 19: "SVC", 23: "ABT", 27: "UND", 31: "SYS"}


def decode_thumb_quick(hw: int, addr: int) -> str:
    if (hw >> 13) == 1:
        op = (hw >> 11) & 3
        rd = (hw >> 8) & 7
        imm = hw & 0xFF
        names = ["MOV", "CMP", "ADD", "SUB"]
        return f"{names[op]} r{rd}, #0x{imm:02X}"
    if (hw >> 12) == 0xD:
        cond = (hw >> 8) & 0xF
        if cond == 0xF:
            return "SWI"
        offset = hw & 0xFF
        if offset & 0x80:
            offset -= 0x100
        target = addr + 4 + offset * 2
        cn = ["EQ","NE","CS","CC","MI","PL","VS","VC","HI","LS","GE","LT","GT","LE","","NV"][cond]
        return f"B{cn} 0x{target:08X}"
    if (hw >> 11) == 0x1C:
        offset = hw & 0x7FF
        if offset & 0x400:
            offset -= 0x800
        target = addr + 4 + offset * 2
        return f"B 0x{target:08X}"
    if (hw >> 13) == 2 and (hw >> 11) & 3 == 1:
        rs = (hw >> 3) & 7
        rd = hw & 7
        op4 = (hw >> 6) & 0xF
        names = ["AND","EOR","LSL","LSR","ASR","ADC","SBC","ROR","TST","NEG","CMP","CMN","ORR","MUL","BIC","MVN"]
        return f"{names[op4]} r{rd}, r{rs}"
    return f"(0x{hw:04X})"


def decode_arm_quick(instr: int, addr: int) -> str:
    cond = (instr >> 28) & 0xF
    cn = ["EQ","NE","CS","CC","MI","PL","VS","VC","HI","LS","GE","LT","GT","LE","AL","NV"][cond]
    typ = (instr >> 26) & 3
    if typ == 2:
        offset = instr & 0x00FFFFFF
        if offset & 0x800000:
            offset -= 0x1000000
        target = addr + 8 + offset * 4
        link = "L" if (instr >> 24) & 1 else ""
        return f"{cn} B{link} 0x{target:08X}"
    if typ == 0:
        op = (instr >> 21) & 0xF
        S = "S" if (instr >> 20) & 1 else ""
        Rn = (instr >> 16) & 0xF
        Rd = (instr >> 12) & 0xF
        on = ["AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN"][op]
        I = (instr >> 25) & 1
        if I:
            imm = instr & 0xFF
            rot = ((instr >> 8) & 0xF) * 2
            if op == 13:
                return f"{cn} MOV{S} r{Rd}, #0x{imm:02X} rot={rot}"
            if op in (8,9,10,11):
                return f"{cn} {on}{S} r{Rn}, #0x{imm:02X} rot={rot}"
            return f"{cn} {on}{S} r{Rd}, r{Rn}, #0x{imm:02X} rot={rot}"
        rm = instr & 0xF
        sh = (instr >> 5) & 3
        sa = (instr >> 7) & 0x1F
        sn = ["LSL","LSR","ASR","ROR"][sh]
        if (instr >> 4) & 1:
            rs = (instr >> 8) & 0xF
            if op == 13:
                return f"{cn} MOV{S} r{Rd}, r{rm} {sn} r{rs}"
            if op in (8,9,10,11):
                return f"{cn} {on}{S} r{Rn}, r{rm} {sn} r{rs}"
            return f"{cn} {on}{S} r{Rd}, r{Rn}, r{rm} {sn} r{rs}"
        if op == 13:
            return f"{cn} MOV{S} r{Rd}, r{rm} {sn} #{sa}"
        if op in (8,9,10,11):
            return f"{cn} {on}{S} r{Rn}, r{rm} {sn} #{sa}"
        return f"{cn} {on}{S} r{Rd}, r{Rn}, r{rm} {sn} #{sa}"
    return f"(type={typ})"


def format_row(row: dict, fields: list[str] | None = None) -> str:
    if fields is None:
        fields = list(row.keys())
    parts = []
    for f in fields:
        if f == "cycle":
            continue
        v = row[f]
        if f == "cpsr":
            val = int(v)
            mode = val & 0x1F
            mn = MODE_NAMES.get(mode, f"?{mode:02X}")
            nzcv = f"N{(val>>31)&1}Z{(val>>30)&1}C{(val>>29)&1}V{(val>>28)&1}"
            parts.append(f"CPSR={nzcv} {mn}")
        elif f in ("pc", "r15"):
            parts.append(f"{f}=0x{int(v):08X}")
        elif f.startswith("r"):
            iv = int(v)
            if iv > 256:
                parts.append(f"{f}=0x{iv:08X}")
            else:
                parts.append(f"{f}={iv}")
        else:
            parts.append(f"{f}={v}")
    return " ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="Trace diff analyzer with decode")
    parser.add_argument("--expected", required=True, type=Path)
    parser.add_argument("--actual", required=True, type=Path)
    parser.add_argument("--rom", type=Path, help="ROM file for instruction decode")
    parser.add_argument("--ignore-field", action="append", default=[])
    parser.add_argument("--context", type=int, default=3, help="Rows of context around mismatch")
    parser.add_argument("--all", action="store_true", help="Show ALL mismatches, not just first")
    args = parser.parse_args()

    with args.expected.open() as f:
        expected = list(csv.DictReader(f))
    with args.actual.open() as f:
        actual = list(csv.DictReader(f))

    ignored = set(args.ignore_field)
    fields = [n for n in expected[0].keys() if n not in ignored]

    rom_data = args.rom.read_bytes() if args.rom else None

    if len(expected) != len(actual):
        print(f"Row count: expected={len(expected)} actual={len(actual)}")

    mismatches = []
    for i, (e, a) in enumerate(zip(expected, actual)):
        for f in fields:
            if e[f] != a[f]:
                mismatches.append((i, f, e, a))
                break

    if not mismatches:
        print(f"Traces match for {len(actual)} rows across {len(fields)} fields.")
        return 0

    print(f"Found {len(mismatches)} mismatch(es) in {len(actual)} rows")
    print()

    limit = len(mismatches) if args.all else 1
    for idx, (row_i, first_field, e, a) in enumerate(mismatches[:limit]):
        all_diffs = []
        for f in fields:
            if e[f] != a[f]:
                ev = int(e[f])
                av = int(a[f])
                diff = av - ev
                all_diffs.append(f"{f}: ref=0x{ev:08X}({ev}) act=0x{av:08X}({av}) diff={diff})")

        print(f"=== Mismatch at row {row_i} (first: {first_field}) ===")
        print()

        start = max(0, row_i - args.context)
        end = min(len(expected), row_i + args.context + 1)
        for i in range(start, end):
            marker = ">>>" if i == row_i else "   "
            r = expected[i]
            a_r = actual[i]
            pc = int(r["pc"])
            thumb = (int(r["cpsr"]) >> 5) & 1

            rom_off = pc - 0x08000000
            instr_str = ""
            if rom_data and 0 <= rom_off < len(rom_data) - 1:
                if thumb:
                    hw = struct.unpack_from("<H", rom_data, rom_off)[0]
                    instr_str = f" [{decode_thumb_quick(hw, pc)}]"
                else:
                    instr = struct.unpack_from("<I", rom_data, rom_off)[0]
                    instr_str = f" [{decode_arm_quick(instr, pc)}]"

            state = "THUMB" if thumb else "ARM"
            match = "MATCH" if all(r[f] == a_r[f] for f in fields) else "DIFF"
            print(f"{marker} row {i:4d}: PC=0x{pc:08X} [{state}]{instr_str}  ({match})")
            if match == "DIFF":
                diffs = []
                for f in fields:
                    if r[f] != a_r[f]:
                        diffs.append(f"    {f}: ref={r[f]} act={a_r[f]}")
                print("\n".join(diffs))

        if idx < limit - 1:
            print()

    if not args.all and len(mismatches) > 1:
        print(f"... and {len(mismatches) - 1} more mismatches (use --all to show)")

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
