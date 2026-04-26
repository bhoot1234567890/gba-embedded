# ESP32-S3 GBA Emulator Guide

This document is the comprehensive guide for building, running, and optimizing the GBA emulator on ESP32-S3 hardware. It covers deployment instructions, current port analysis, accuracy tradeoffs, and the long-term optimization plan.

## 1. Hardware Prerequisites

*   **ESP32-S3-WROOM-1-N8R8** (or equivalent with 8MB octal PSRAM and >=2MB flash)
*   **ST7735 128x128 SPI display** (green tab)
*   USB cable for flashing
*   Optional: 6-10 buttons for D-pad, A/B, Start/Select, L/R

### Display Wiring

| ST7735 Pin | ESP32-S3 GPIO | Function |
| :--- | :--- | :--- |
| SCLK | GPIO 4 | SPI clock |
| MOSI (SDA) | GPIO 5 | SPI data |
| DC (AO) | GPIO 6 | Data/command |
| CS | GPIO 7 | Chip select |
| RST | GPIO 8 | Reset |
| BL | GPIO 9 | Backlight |

*Note: SPI runs at 26 MHz. Green-tab offset is column +2, row +1.*

---

## 2. Building & Flashing

### Environment Setup

```bash
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
. ./export.sh
```

### Build Modes

**Mode 1: QEMU Test**
Runs unit tests without hardware. No PSRAM or display.
```bash
cd platform/esp32s3
cp sdkconfig.defaults.qemu sdkconfig.defaults
idf.py set-target esp32s3
idf.py build
idf.py qemu monitor
```

**Mode 2: Hardware Test Runner**
Runs the same unit tests on real hardware (requires PSRAM).
```bash
cd platform/esp32s3
cp sdkconfig.defaults sdkconfig.defaults  # Ensure hardware config
idf.py set-target esp32s3
idf.py -p /dev/ttyUSB0 flash monitor
```

**Mode 3: Full Runtime**
Full emulator with dual-core pipeline and ST7735 output.
```bash
cd platform/esp32s3
idf.py -DGBA_BUILD_RUNTIME=ON -p /dev/ttyUSB0 flash monitor
```

---

## 3. Current Port Status & Implementation Requirements

The emulator is cross-platform but actively optimized for ESP32-S3. The codebase successfully allocates memory across SRAM/PSRAM and uses dual-core rendering, but there are several critical blockers and performance issues that must be addressed before playable speeds can be achieved.

### 3.1 Blockers (Must Fix to Run Games)
*   **ROM Loading**: `gba_runtime.cpp` never calls `emulator.load_rom()`. Must implement loading via SD card, HTTP, SPIFFS, or firmware embedding.
*   **Input Handling**: `set_keys()` is never called. GPIO debounce and reading must be implemented.
*   **Watchdog Timer**: The emulator loop lacks `esp_task_wdt_reset()`, which will cause reboots on emulation stalls.

### 3.2 Performance-Critical Changes
*   **APU `std::deque` Allocation**: The APU uses `std::deque` which causes heap fragmentation on ESP32. Must be replaced with a static ring buffer.
*   **FPU Soft-Float Bloat**: SWI handlers in `cpu.cpp` use `<cmath>` (`std::sqrt`, `std::atan2`), triggering double-precision emulation. Replace with integer math.
*   **PSRAM Cache Thrashing**: The emulator walks >400KB of PSRAM/frame, evicting the flash cache. Critical functions like `Bus::read`/`Bus::write` and `Timers::advance_to` must be placed in `IRAM`.
*   **WiFi Stalls**: WiFi is currently enabled in `sdkconfig`. It uses IRAM and SPI1 flash ops, causing stutters. Disable WiFi for runtime builds.

### 3.3 Memory Map

| Data | Size | Location |
| :--- | :--- | :--- |
| CPU State / Scheduler / IRQ / Bus | ~500B | SRAM |
| IWRAM / Palette / OAM | 34 KB | SRAM |
| EWRAM (guest) | 256 KB | PSRAM |
| VRAM (guest) | 96 KB | PSRAM |
| Framebuffer (240x160) | 75 KB | PSRAM |
| Display Buffers (x2) | 64 KB | PSRAM |

---

## 4. Emulation Accuracy vs. Performance Tradeoffs

GBA needs 280,896 cycles/frame at 59.7fps. The ESP32-S3 running at 240MHz provides ~4M cycles/frame (~14x headroom), but host cycle overhead per emulated cycle is high.

### Recommended Cuts
*   **APU**: Disable or heavily downsample (32kHz → 16kHz). Biggest cycle sink.
*   **PPU Rendering**: Only render visible scanlines (0-159). Skip vblank rendering. For the 128x128 display, directly crop-and-decimate instead of downscaling.
*   **GamePak Prefetch**: Replace the 8-halfword cycle-accurate prefetch with a simple 1-cycle-penalty model.
*   **HLE BIOS**: Keep HLE, avoid emulating real BIOS.

### Borderline/Aggressive Cuts (If needed)
*   **Timer Cascade Precision**: Checkpoint timers at scanlines instead of per-instruction. Breaks some timing tests but saves cycles.
*   **Stub SIO**: No single-player games need serial link. Stub completely.
*   **Frameskip**: Skipping to 30fps is highly playable and gives 2x time budget.

### Do Not Cut
*   BIOS protection latch, VRAM byte replication, OAM byte-write ignores, and MMIO register correctness.

---

## 5. Optimization Roadmap

### Phase 1: PPU & Display pipeline
1.  **Direct-to-128x128 Rendering**: Eliminate the 75KB 240x160 framebuffer. Crop and decimate horizontally/vertically during the scanline loop.
2.  **Dirty Scanline Tracking**: Re-render only when VRAM/palette/OAM changes.
3.  **DMA Display Output**: Use `esp_lcd_panel_io_spi` for hardware DMA SPI transfers.

### Phase 2: CPU Interpreter
1.  **Computed Goto / Dispatch**: Eliminate sequential if/else cascade for instruction decoding. Pre-decode ARM categories into a jump table.
2.  **Lambda Overhead**: Hoist memory-access lambdas out of `execute_arm`/`execute_thumb`.
3.  **Inline Step**: Inline the fetch-decode-execute cycle inside `cpu_run_until` to avoid function call overhead.

### Phase 3: Dual-Core Architecture
1.  **Core 1 (APP)**: Emulation loop (CPU, Timers, DMA, IRQ, Bus).
2.  **Core 0 (PRO)**: Display rendering, SPI DMA output, WiFi/networking (if added).
3.  **Lockless Sync**: Double-buffer scanlines between cores using atomic flags.

### Phase 4: Advanced (If performance is still lacking)
1.  Tile caching in PPU.
2.  Idle loop detection (skip to next IRQ/Vblank when spinning).
3.  Pre-decoded instruction cache for hot ARM/Thumb blocks.
