#!/usr/bin/env python3
"""Decode ARM/THUMB instructions from a GBA ROM at given addresses."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

COND_NAMES = ["EQ","NE","CS","CC","MI","PL","VS","VC","HI","LS","GE","LT","GT","LE","AL","NV"]
OP_NAMES = ["AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN"]
SHIFT_NAMES = ["LSL","LSR","ASR","ROR"]
THUMB_ALU_NAMES = ["AND","EOR","LSL","LSR","ASR","ADC","SBC","ROR","TST","NEG","CMP","CMN","ORR","MUL","BIC","MVN"]


def decode_arm(instr: int, addr: int) -> str:
    cond = (instr >> 28) & 0xF
    cn = COND_NAMES[cond]
    typ = (instr >> 26) & 3

    if typ == 2:
        offset = instr & 0x00FFFFFF
        if offset & 0x800000:
            offset -= 0x1000000
        target = addr + 8 + offset * 4
        link = "L" if (instr >> 24) & 1 else ""
        return f"{cn} B{link} 0x{target:08X}"

    if typ == 0 or typ == 1:
        op = (instr >> 21) & 0xF
        S = "S" if (instr >> 20) & 1 else ""
        Rn = (instr >> 16) & 0xF
        Rd = (instr >> 12) & 0xF
        I = (instr >> 25) & 1
        on = OP_NAMES[op]

        if (instr & 0x0DB0F000) == 0x0120F000 or (instr & 0x0FB0F000) == 0x0320F000:
            target_spsr = (instr >> 22) & 1
            if I:
                imm = instr & 0xFF
                rot = ((instr >> 8) & 0xF) * 2
                return f"{cn} MSR {'SPSR' if target_spsr else 'CPSR'}_f, #0x{imm:02X} rot={rot}"
            rm = instr & 0xF
            return f"{cn} MSR {'SPSR' if target_spsr else 'CPSR'}_f, r{rm}"

        if (instr & 0x0FBF0FFF) == 0x010F0000:
            rd = Rd
            return f"{cn} MRS r{rd}, {'SPSR' if (instr >> 22) & 1 else 'CPSR'}"

        write = op not in (8, 9, 10, 11)
        if I:
            imm = instr & 0xFF
            rot = ((instr >> 8) & 0xF) * 2
            if op == 13:
                return f"{cn} MOV{S} r{Rd}, #0x{imm:02X} rot={rot}"
            if not write:
                return f"{cn} {on}{S} r{Rn}, #0x{imm:02X} rot={rot}"
            return f"{cn} {on}{S} r{Rd}, r{Rn}, #0x{imm:02X} rot={rot}"
        else:
            rm = instr & 0xF
            shift_type = (instr >> 5) & 3
            if (instr >> 4) & 1:
                rs = (instr >> 8) & 0xF
                sn = SHIFT_NAMES[shift_type]
                if op == 13:
                    return f"{cn} MOV{S} r{Rd}, r{rm} {sn} r{rs}"
                if not write:
                    return f"{cn} {on}{S} r{Rn}, r{rm} {sn} r{rs}"
                return f"{cn} {on}{S} r{Rd}, r{Rn}, r{rm} {sn} r{rs}"
            else:
                sa = (instr >> 7) & 0x1F
                sn = SHIFT_NAMES[shift_type]
                if sa == 0 and shift_type == 0:
                    shift_str = ""
                elif sa == 0 and shift_type == 1:
                    shift_str = f" {sn} #32"
                elif sa == 0 and shift_type == 2:
                    shift_str = f" {sn} #32"
                elif sa == 0 and shift_type == 3:
                    shift_str = " RRX"
                else:
                    shift_str = f" {sn} #{sa}"
                if rm == 15:
                    shift_str = f" (pc{shift_str})"
                if op == 13:
                    return f"{cn} MOV{S} r{Rd}, r{rm}{shift_str}"
                if not write:
                    return f"{cn} {on}{S} r{Rn}, r{rm}{shift_str}"
                return f"{cn} {on}{S} r{Rd}, r{Rn}, r{rm}{shift_str}"

    return f"{cn} (type={typ})"


def decode_thumb(hw: int, addr: int) -> str:
    if (hw >> 13) == 0:
        op = (hw >> 11) & 3
        rm = (hw >> 3) & 7
        rd = hw & 7
        if op == 0:
            sa = (hw >> 6) & 0x1F
            return f"LSL r{rd}, r{rm}, #{sa}"
        if op == 1:
            sa = (hw >> 6) & 0x1F
            return f"LSR r{rd}, r{rm}, #{sa or 32}"
        if op == 2:
            sa = (hw >> 6) & 0x1F
            return f"ASR r{rd}, r{rm}, #{sa or 32}"
        if (hw >> 9) & 1:
            rs = (hw >> 3) & 7
            rd = hw & 7
            shift_type = (hw >> 11) & 3
            return f"{SHIFT_NAMES[shift_type]} r{rd}, r{rs}"
        imm = (hw >> 6) & 7
        rn = (hw >> 3) & 7
        rd = hw & 7
        return f"ADD r{rd}, r{rn}, #{imm}"

    if (hw >> 13) == 1:
        op = (hw >> 11) & 3
        rd = (hw >> 8) & 7
        imm = hw & 0xFF
        if op == 0:
            return f"MOV r{rd}, #0x{imm:02X}"
        if op == 1:
            return f"CMP r{rd}, #0x{imm:02X}"
        if op == 2:
            return f"ADD r{rd}, #0x{imm:02X}"
        return f"SUB r{rd}, #0x{imm:02X}"

    if (hw >> 13) == 2:
        op = (hw >> 11) & 3
        if op == 0:
            rd = (hw >> 8) & 7
            imm = hw & 0xFF
            return f"MOV r{rd}, #0x{imm:02X}"
        if op == 1:
            rs = (hw >> 3) & 7
            rd = hw & 7
            op4 = (hw >> 6) & 0xF
            name = THUMB_ALU_NAMES[op4]
            return f"{name} r{rd}, r{rs}"
        if op == 2:
            rm = (hw >> 3) & 7
            rd = hw & 7
            return f"CMP r{rd} (hi), r{rm} (hi)"
        if (hw >> 7) & 1:
            rm = (hw >> 3) & 7
            rd = hw & 7
            l = (hw >> 7) & 1
            return f"B{'L' if l else 'X'} r{rm}"
        rm = (hw >> 3) & 7
        rd = hw & 7
        return f"ADD r{rd}, r{rm} (hi)"

    if (hw >> 12) == 5:
        return f"LDR/STR (offset5=0x{(hw>>6)&0x1F:02X})"

    if (hw >> 12) == 6:
        return f"LDR/STR (word/byte)"

    if (hw >> 13) == 3:
        rd = (hw >> 8) & 7
        imm = (hw & 0xFF) * 4
        return f"LDR r{rd}, [PC, #0x{imm:02X}]"

    if (hw >> 13) == 4:
        return f"LDR/STR (reg offset)"

    if (hw >> 12) == 0x9:
        rd = (hw >> 8) & 7
        imm = (hw & 0xFF) * 4
        l = (hw >> 11) & 1
        return f"{'LDR' if l else 'STR'} r{rd}, [SP, #0x{imm:02X}]"

    if (hw >> 12) == 0xA:
        rd = (hw >> 8) & 7
        imm = (hw & 0xFF) * 4
        return f"ADD r{rd}, {'SP' if (hw>>11)&1 else 'PC'}, #0x{imm:02X}"

    if (hw >> 12) == 0xB:
        return f"SP adjust"

    if (hw >> 12) == 0xC:
        return f"STM/LDM (multiple)"

    if (hw >> 12) == 0xD:
        cond = (hw >> 8) & 0xF
        if cond == 0xF:
            return f"SWI 0x{hw & 0xFF:02X}"
        offset = (hw & 0xFF)
        if offset & 0x80:
            offset -= 0x100
        target = addr + 4 + offset * 2
        return f"B{COND_NAMES[cond]} 0x{target:08X}"

    if (hw >> 11) == 0x1C:
        offset = hw & 0x7FF
        if offset & 0x400:
            offset -= 0x800
        target = addr + 4 + offset * 2
        return f"B 0x{target:08X}"

    if (hw >> 11) in (0x1E, 0x1F):
        return f"BL prefix/suffix"

    return f"(undecoded 0x{hw:04X})"


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode GBA ARM/THUMB instructions")
    parser.add_argument("rom", type=Path)
    parser.add_argument("address", nargs="+", help="hex addresses (e.g. 0x08000100 or 100)")
    parser.add_argument("--arm", action="store_true", default=True)
    parser.add_argument("--thumb", action="store_true")
    args = parser.parse_args()

    rom_data = args.rom.read_bytes()
    for addr_str in args.address:
        addr = int(addr_str, 16) if addr_str.startswith("0x") else int(addr_str, 0)
        off = addr - 0x08000000
        if 0 <= off < len(rom_data) - 1:
            if args.thumb:
                hw = struct.unpack_from("<H", rom_data, off)[0]
                print(f"0x{addr:08X}: 0x{hw:04X}  {decode_thumb(hw, addr)}")
            else:
                instr = struct.unpack_from("<I", rom_data, off)[0]
                print(f"0x{addr:08X}: 0x{instr:08X}  {decode_arm(instr, addr)}")
        else:
            print(f"0x{addr:08X}: (out of ROM, size=0x{len(rom_data):X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
