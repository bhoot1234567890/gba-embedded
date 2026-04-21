# Validation Playbook

This playbook maps each emulator bring-up stage to concrete checks you can run independently.

## 1) CPU correctness (ARM/Thumb)

Goal: verify instruction semantics and flags before broad hardware integration.

1. Run targeted CPU test ROMs (for example: `gba-suite`, `arm-wrestler`) using the trace runner:
   - `./build/gba_trace_runner --rom <test.gba> --bios <bios.bin> --steps <N> --output trace_actual.csv`
2. Generate a reference trace from mGBA/no$gba with the same ROM and step window.
3. Compare traces:
   - `python3 tools/compare_traces.py --expected trace_ref.csv --actual trace_actual.csv --ignore-field cycle`

Pass criteria: register/PC/CPSR trace rows match reference for the selected window.

## 2) Bus/MMIO and memory behavior

Goal: guarantee address decode and memory semantics are deterministic.

Run `ctest` and rely on these in-tree checks:

- region routing sanity (`EWRAM`/`IWRAM` path coverage)
- alignment behavior for word writes
- open-bus reads for unmapped addresses
- palette byte-write replication
- VRAM byte-write rules in OBJ region

Pass criteria: all deterministic bus tests pass.

## 3) Timers/IRQ/DMA timing

Goal: verify trigger cycles and interrupt flags for critical timing hardware.

Run `ctest` and rely on:

- exact timer overflow cycle checks
- IRQ assertion/acknowledge behavior
- DMA immediate transfer correctness
- HBlank-start DMA trigger and DMA IRQ flag checks

Pass criteria: each event occurs at expected cycle boundary with expected IRQ flags.

## 4) PPU bring-up (Mode 3 first)

Goal: confirm scanline output is stable and regression-friendly.

Run `ctest` and rely on framebuffer hash checks:

- Mode 3 scanline hash golden
- Mode 4 scanline hash golden
- HBlank transition timing check

Pass criteria: framebuffer hashes match known-good values.

## 5) Audio/Input

Goal: verify timer-driven direct sound cadence and key matrix behavior.

Run `ctest` and rely on:

- FIFO + timer-driven sample emission checks
- FIFO refill request generation
- KEYINPUT active-low behavior
- KEYCNT read/write MMIO checks

Pass criteria: audio cadence and key MMIO behavior match expected invariants.

## ESP32 emulation setup (BSP-level smoke tests)

Use QEMU to validate firmware startup and serial output, not emulation accuracy:

```bash
python3 tools/esp32_qemu_smoke.py --firmware <firmware.elf> --expect "Boot"
```

If you do not already have a valid ESP32 application ELF, build one from ESP-IDF `hello_world`:

```bash
docker run --rm -v "$PWD/tests/assets/esp32-work:/project" -w /project espressif/idf:release-v5.2 \
  bash -lc 'cp -R /opt/esp/idf/examples/get-started/hello_world ./hello_world && cd hello_world && idf.py set-target esp32 && idf.py build'
```

Expected ELF path:

- `tests/assets/esp32-work/hello_world/build/hello_world.elf`

Notes:

- This validates BSP wiring and runtime startup only.
- Keep core accuracy validation on host with deterministic tests + trace comparison.
