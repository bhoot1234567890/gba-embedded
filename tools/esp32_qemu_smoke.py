#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import select
import subprocess
import sys
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run an ESP32 firmware image in QEMU and perform a smoke check."
    )
    parser.add_argument("--firmware", required=True, type=Path, help="ESP32 ELF firmware path")
    parser.add_argument(
        "--qemu-bin",
        default="qemu-system-xtensa",
        help="QEMU binary name/path (default: qemu-system-xtensa)",
    )
    parser.add_argument("--machine", default="esp32", help="QEMU machine name (default: esp32)")
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=10.0,
        help="time window to wait for boot evidence",
    )
    parser.add_argument(
        "--expect",
        default=None,
        help="substring that must appear in serial output",
    )
    parser.add_argument(
        "--extra-qemu-arg",
        action="append",
        default=[],
        help="extra QEMU arg (repeatable)",
    )
    args = parser.parse_args()

    if not args.firmware.exists():
        print(f"Firmware file not found: {args.firmware}", file=sys.stderr)
        return 1

    command = [
        args.qemu_bin,
        "-nographic",
        "-M",
        args.machine,
        "-kernel",
        str(args.firmware),
        *args.extra_qemu_arg,
    ]

    print("Launching:", " ".join(command))
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=False,
        bufsize=0,
    )

    output = ""
    deadline = time.monotonic() + args.timeout_seconds

    try:
        while True:
            if process.stdout is not None:
                ready, _, _ = select.select([process.stdout], [], [], 0.05)
                if ready:
                    chunk = os.read(process.stdout.fileno(), 4096)
                    if chunk:
                        decoded = chunk.decode("utf-8", errors="replace")
                        output += decoded
                        print(decoded, end="", flush=True)
                    if args.expect is not None and args.expect in output:
                        print("Expected boot marker observed; smoke check passed.")
                        return 0

            if process.poll() is not None:
                if args.expect is None and process.returncode == 0:
                    print("QEMU exited cleanly within smoke window.")
                    return 0
                print("QEMU exited before smoke criteria were met.", file=sys.stderr)
                return process.returncode if process.returncode is not None else 1

            if time.monotonic() >= deadline:
                if args.expect is None:
                    print("QEMU stayed alive for timeout window; smoke check passed.")
                    return 0
                print(
                    f"Did not observe expected marker '{args.expect}' within timeout.",
                    file=sys.stderr,
                )
                return 1

            time.sleep(0.01)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
