#!/usr/bin/env python3
"""Capture CPU trace from mGBA via GDB remote protocol.

mGBA's GDB stub sends register values in little-endian byte order within
each 8-hex-char field.  We byte-swap each u32 to get the correct value.

Runs mGBA WITHOUT a BIOS file so it uses its internal boot stub, which
jumps to the ROM entry (0x08000000) in ~6 steps.  Trace capture begins
once PC reaches the ROM region.

Usage:
    python3 tools/mgba_gdb_trace.py --rom tests/assets/roms/gba_suite_arm.gba \
        --steps 5000 --output trace_ref_arm.csv
"""
from __future__ import annotations

import argparse
import csv
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

MGBA_BIN = "/opt/homebrew/Cellar/mgba/0.10.5_2/mGBA.app/Contents/MacOS/mGBA"
GDB_PORT = 2345
HOST = "127.0.0.1"
ROM_BASE = 0x08000000
ROM_END = 0x0E000000


def _checksum(data: bytes) -> bytes:
    return format(sum(data) & 0xFF, "02x").encode()


def _make_packet(cmd: str) -> bytes:
    return b"$" + cmd.encode() + b"#" + _checksum(cmd.encode())


def _recv_packet(sock: socket.socket, timeout: float = 5.0) -> bytes:
    sock.settimeout(timeout)
    buf = b""
    while True:
        ch = sock.recv(1)
        if ch == b"$":
            break
        if not ch:
            raise ConnectionError("Connection closed waiting for $")
    while True:
        ch = sock.recv(1)
        if ch == b"#":
            break
        if not ch:
            raise ConnectionError("Connection closed in packet body")
        buf += ch
    _ = sock.recv(2)
    return buf


def _xchg(sock: socket.socket, cmd: str) -> bytes:
    sock.sendall(_make_packet(cmd))
    resp = _recv_packet(sock)
    sock.sendall(b"+")
    return resp


def _parse_le_reg(raw: bytes) -> int:
    hex_str = raw.decode()
    raw_val = int(hex_str, 16)
    return struct.unpack("<I", struct.pack(">I", raw_val))[0]


def _read_regs(sock: socket.socket) -> list[int]:
    raw = _xchg(sock, "g")
    return [_parse_le_reg(raw[i * 8 : (i + 1) * 8]) for i in range(17)]


def _step(sock: socket.socket) -> None:
    _xchg(sock, "s")


def capture_trace(rom: Path, steps: int, output: Path) -> None:
    cmd = [MGBA_BIN, "-g", str(rom)]
    print(f"Launching: {' '.join(cmd)}", file=sys.stderr)
    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    sock = None
    connected = False
    for attempt in range(60):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(1.0)
            sock.connect((HOST, GDB_PORT))
            connected = True
            break
        except (ConnectionRefusedError, OSError):
            sock.close()
            time.sleep(0.25)

    if not connected:
        proc.terminate()
        proc.wait()
        raise RuntimeError(f"Could not connect to mGBA GDB on port {GDB_PORT}")

    print("Connected to mGBA GDB server", file=sys.stderr)

    sock.settimeout(2.0)
    try:
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"+":
                break
    except socket.timeout:
        pass

    _xchg(sock, "?")

    regs = _read_regs(sock)
    pc = regs[15]
    print(f"Initial PC=0x{pc:08X}", file=sys.stderr)

    if pc < ROM_BASE:
        print("Advancing to ROM entry...", file=sys.stderr)
        for i in range(1000):
            _step(sock)
            regs = _read_regs(sock)
            pc = regs[15]
            if ROM_BASE <= pc < ROM_END:
                print(f"ROM entry at step {i+1}: PC=0x{pc:08X}", file=sys.stderr)
                break
        else:
            proc.terminate()
            proc.wait()
            raise RuntimeError(
                f"PC never reached ROM after 1000 steps (last PC=0x{pc:08X})"
            )

    with output.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "step",
                "cycle",
                "cpsr",
                "pc",
                "r0",
                "r1",
                "r2",
                "r3",
                "r4",
                "r5",
                "r6",
                "r7",
                "r8",
                "r9",
                "r10",
                "r11",
                "r12",
                "r13",
                "r14",
                "r15",
            ]
        )

        for step in range(steps):
            if step % 500 == 0:
                print(f"  step {step}/{steps}", file=sys.stderr)
            regs = _read_regs(sock)
            r0_r15 = regs[:16]
            cpsr = regs[16]
            pc = r0_r15[15]
            cycle = step
            writer.writerow([step, cycle, cpsr, pc] + r0_r15)
            _step(sock)

    sock.close()
    proc.terminate()
    proc.wait(timeout=5)
    print(f"Wrote {steps} rows to {output}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description="mGBA GDB trace capture")
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--steps", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    capture_trace(args.rom, args.steps, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
