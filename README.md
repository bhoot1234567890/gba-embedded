# Embedded GBA Emulator

Portable, cycle-scheduled Game Boy Advance emulator core aimed at embedded targets, with `i.MX RT1170` as the primary bring-up platform.

## Current Scope

- ARM7TDMI core with ARM/Thumb interpreter framework
- Cycle-based scheduler in master GBA cycles
- Unified memory bus with GBA memory map and MMIO routing
- Scanline PPU bring-up with bitmap modes first
- Timers, IRQs, DMA, and Direct Sound FIFO foundations
- Portable core plus RT1170 BSP stub
- Host-buildable test executable for early verification

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Layout

- `include/gba/core`: public emulator core interfaces
- `include/gba/platform`: platform abstraction and RT1170 BSP interface
- `src/core`: emulator implementation
- `src/platform/rt1170`: RT1170-oriented BSP stub
- `tests`: host-side smoke and subsystem tests
- `tools`: host-side validation and emulation helper scripts
- `docs`: design notes

## Working Docs

- [Architecture](/Users/chaitanyamalhotra/development/gba%20embedded/docs/architecture.md)
- [Component Roadmap](/Users/chaitanyamalhotra/development/gba%20embedded/docs/component-roadmap.md)
- [Validation Playbook](/Users/chaitanyamalhotra/development/gba%20embedded/docs/validation-playbook.md)
- [Hardware Partitioning](/Users/chaitanyamalhotra/development/gba%20embedded/docs/hardware-partitioning.md)
- [Smallest-First Plan](/Users/chaitanyamalhotra/development/gba%20embedded/docs/smallest-first-plan.md)

## Validation Workflow

Run the in-tree deterministic component tests:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For CPU ROM validation against a reference emulator trace:

```bash
./build/gba_trace_runner --rom <rom.gba> --bios <bios.bin> --steps 100000 --output trace_actual.csv
python3 tools/compare_traces.py --expected trace_ref.csv --actual trace_actual.csv --ignore-field cycle
```

For ESP32 BSP-level smoke checks with QEMU:

```bash
python3 tools/esp32_qemu_smoke.py --firmware <firmware.elf> --expect "Boot"
```

## Design Notes

The emulator is organized around one master GBA cycle domain:

1. Run the CPU until the next scheduled hardware event.
2. Service timers, DMA, IRQ, PPU, and APU at that exact cycle.
3. Repeat until a scanline or frame is ready.

This keeps timing decisions centralized and makes it practical to port the same core to desktop-host tests and MCU BSPs.
