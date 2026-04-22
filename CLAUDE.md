# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
# Desktop build
cmake -S . -B build && cmake --build build -j$(sysctl -n hw.ncpu)

# Run unit tests (36 host-side tests)
ctest --test-dir build --output-on-failure

# Run GBA test suite (14 suites against ROM)
./build/gba_suite_runner

# CPU trace validation against reference
./build/gba_trace_runner --rom <rom.gba> --bios <bios.bin> --steps 100000 --output trace.csv
python3 tools/compare_traces.py --expected ref.csv --actual trace.csv --ignore-field cycle

# Parse and compare suite logs
python3 tools/parse_gba_suite_logs.py <log1> <log2> --compare --show-examples 5

# ESP32 QEMU smoke test
python3 tools/esp32_qemu_smoke.py --firmware <firmware.elf> --expect "Boot"
```

Build options: `-DGBA_BUILD_TESTS=ON` (default), `-DGBA_BUILD_TOOLS=ON` (default), `-DGBA_WARNINGS_AS_ERRORS=OFF`.
Strict warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`.

## Architecture

GBA emulator core in C++20, targeting ESP32-S3 (Xtensa LX7, dual-core 240MHz) with desktop builds for testing.

### Cycle Scheduler Model

Everything runs in master GBA cycles (16.78 MHz). The main loop:
1. `refresh_schedule()` — query each subsystem's `next_event_cycle()`, register with Scheduler (4 slots, flat array, no heap)
2. `cpu_run_until(next_event)` — ARM7TDMI interpreter executes until next hardware event
3. `service_due_hardware()` — advance Timers, PPU, DMA, APU at exact cycle boundaries
4. Repeat until `ppu_.frame_ready()`

Timing constants: 1232 cycles/scanline, 228 scanlines/frame, 280896 cycles/frame.

### Subsystem Ownership

`Emulator` owns all subsystems. `Bus` receives references to PPU, Timers, DMA, APU, IRQ for MMIO routing. `Arm7tdmi` receives Bus + IRQ.

Inside `cpu_run_until()`, the CPU also calls `bus_.service_timers()` and `bus_.service_dma()` before each instruction for inline event checking.

### CPU Interpreter

`src/core/cpu.cpp` (~1800 lines). Switch-based dispatch, no computed goto (yet).

- `execute_arm()`: bitmask cascade (BX → MUL → MULL → halfword → MRS/MSR → branch → LDR/STR → LDM/STM → SWI → data processing → undefined)
- `execute_thumb()`: top-bit-range dispatch (shifts → add/sub → mov/cmp → ALU → hi-reg → PC-load → load/store variants → push/pop → branches → BL → undefined)
- Condition check via `condition_passed()` for every ARM instruction (15 conditions from CPSR flags)
- HLE SWI auto-detection: if BIOS SWI vector is an infinite loop, handles SWI 0x02/0x05/0x0B/0x0C in C++

### Bus Address Decoding

`src/core/bus.cpp` (~970 lines). Range-based cascade: BIOS (pipeline latch protection) → EWRAM → IWRAM → MMIO → Palette → VRAM (mirroring, byte replication) → OAM (byte writes ignored) → GamePak ROM (waitstates + prefetch buffer) → SRAM → open bus.

GamePak prefetch: 8-halfword buffer with head/tail addressing, duty-cycle model. Controlled by WAITCNT register. This is the source of most current test failures (Suites 03, 10, 13).

### Platform: ESP32-S3

Guarded by `GBA_PLATFORM_ESP32`:
- Large arrays (EWRAM, VRAM, palette, OAM, framebuffer) on PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- Hot functions (`cpu_run_until`, `execute_arm`, `execute_thumb`) in IRAM via `IRAM_ATTR`
- `<filesystem>` excluded, HLE SWI logging suppressed
- Dual-core: Core 1 runs CPU + downscale, Core 0 runs SPI display (ST7735 128x128)
- ESP-IDF project at `platform/esp32s3/`, component at `platform/esp32s3/components/gba_core/`

### Test Suite Status (2026-04-22)

7/14 suites pass 100%: Memory, I/O read, Timer IRQ, Shifter, Carry, Multiply long, BIOS math.

Remaining failures are all timing/prefetch: Thumb ROM prefetch (Suite 03, -26), timer cascade (Suite 04, -309), ROM-source DMA word transfers (Suite 10, -36), DMA prefetch break + HBlank timing (Suite 13, -10).

Reference emulator: NanoBoyAdvance at `/Users/chaitanyamalhotra/development/NanoBoyAdvance/` — passes all timing suites.

## Key Design Constraints

- No `std::function` or virtual dispatch in hot paths (CPU interpreter, scheduler)
- No heap allocation in per-instruction paths (APU uses `std::deque` — known debt, plan to replace with ring buffer)
- Bus arrays dynamically allocated for ESP32 PSRAM placement; `std::array` on desktop
- VRAM byte writes replicate to both bytes of halfword; byte writes to OBJ region silently ignored
- Palette byte writes replicate; OAM byte writes ignored
- BIOS protection: code-fetches latch, data reads outside BIOS return latched value
- All MMIO register addresses in `include/gba/core/constants.hpp`
