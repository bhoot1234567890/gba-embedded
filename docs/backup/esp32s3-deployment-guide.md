# ESP32-S3 Deployment Guide

Complete guide for building, flashing, and running the GBA emulator on ESP32-S3 hardware.

## Prerequisites

### Hardware

- **ESP32-S3-WROOM-1-N8R8** or equivalent with octal PSRAM (8MB) and >=2MB flash
- ST7735 128x128 SPI display (green tab)
- USB cable for flashing
- Optional: 6 buttons for D-pad + A/B, Start/Select, L/R

### Software

```bash
# Install ESP-IDF v5.x (follow https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
. ./export.sh

# Verify
idf.py --version
echo $IDF_PATH  # must be set
```

### Display Wiring

| ST7735 Pin | ESP32-S3 GPIO | Function |
|------------|---------------|----------|
| SCLK | GPIO 4 | SPI clock |
| MOSI (SDA) | GPIO 5 | SPI data |
| DC (AO) | GPIO 6 | Data/command |
| CS | GPIO 7 | Chip select |
| RST | GPIO 8 | Reset |
| BL | GPIO 9 | Backlight |

SPI runs at 26 MHz (ST7735 safe max). Green-tab offset: column +2, row +1.

## Build Modes

### Mode 1: QEMU Test (default, no hardware needed)

6 unit tests run in QEMU. No PSRAM, no display driver.

```bash
cd platform/esp32s3

# Configure for QEMU (disables PSRAM)
cp sdkconfig.defaults.qemu sdkconfig.defaults

idf.py set-target esp32s3
idf.py build

# Run in QEMU
idf.py qemu monitor
# or manually:
qemu-system-xtensa -nographic -M esp32s3 -build/qemu-flash-image.bin
```

Expects output: `ALL TESTS PASSED on ESP32-S3!`

Tests: scheduler, IRQ controller, timer overflow, audio FIFO, ARM ADD, Thumb ADD. Each test allocates/deletes subsystems individually to fit within ~320KB SRAM (no PSRAM in QEMU).

### Mode 2: Real Hardware — Test Runner

Same test runner but running on actual hardware with PSRAM available.

```bash
cd platform/esp32s3

# Restore hardware config (octal PSRAM)
cp sdkconfig.defaults sdkconfig.defaults  # already correct

idf.py set-target esp32s3
idf.py -p /dev/ttyUSB0 flash monitor
```

### Mode 3: Real Hardware — Full Runtime (GBA emulation + display)

The full emulator with dual-core rendering pipeline, downscaler, and ST7735 output.

```bash
cd platform/esp32s3

# Enable runtime build (adds display driver + runtime code)
idf.py -DGBA_BUILD_RUNTIME=ON -p /dev/ttyUSB0 flash monitor
```

## Architecture on ESP32-S3

### Memory Map

| Data | Size | Location | Allocation |
|------|------|----------|------------|
| CPU registers (Arm7tdmi) | ~200B | Internal SRAM | Stack/struct member |
| Scheduler (4 slots) | ~64B | Internal SRAM | Struct member |
| IRQ controller | ~32B | Internal SRAM | Struct member |
| Timers | ~128B | Internal SRAM | Struct member |
| DMA engine | ~128B | Internal SRAM | Struct member |
| Bus logic (waitcnt, prefetch) | ~64B | Internal SRAM | Struct member |
| IWRAM (guest) | 32 KB | Internal SRAM | `MALLOC_CAP_INTERNAL` |
| Palette RAM | 1 KB | Internal SRAM | `MALLOC_CAP_INTERNAL` |
| OAM | 1 KB | Internal SRAM | `MALLOC_CAP_INTERNAL` |
| EWRAM (guest) | 256 KB | PSRAM | `MALLOC_CAP_SPIRAM` |
| VRAM (guest) | 96 KB | PSRAM | `MALLOC_CAP_SPIRAM` |
| Framebuffer (240x160) | 75 KB | PSRAM | `MALLOC_CAP_SPIRAM` |
| Display buffers (x2) | 64 KB | PSRAM | `new` (SPIRAM_USE_MALLOC) |
| Downscale LUT | 512 B | Stack | — |

**Total PSRAM: ~491 KB** (fits in 8MB with room to spare)
**Total internal SRAM: ~100 KB** (fits in 512KB)

### Dual-Core Pipeline

```
Core 1 (gba_cpu task)              Core 0 (main task)
─────────────────────              ──────────────────
emulator->run_frame()              ulTaskNotifyTake() [blocked]
  CPU + DMA + Timers + IRQ               ↓
  PPU renders scanlines            display_draw(front_buf)
downscale_565(lut, fb, back_buf)     SPI DMA to ST7735
atomic swap front/back             loop
xTaskNotifyGive(display_task)
```

- **Core 1** is the master timeline: CPU, DMA, timers, IRQ, bus, PPU rendering decisions
- **Core 0** is the output worker: reads completed frame from front buffer, sends via SPI DMA
- Sync: `xTaskNotifyGive` / `ulTaskNotifyTake` with ping-pong double buffers
- CPU task: 32KB stack, priority 5, pinned to Core 1

### IRAM-Resident Functions

Three hot functions are placed in IRAM (internal instruction RAM) to avoid flash cache misses:

- `Arm7tdmi::cpu_run_until()` — main emulation loop
- `Arm7tdmi::execute_arm()` — ARM instruction decoder
- `Arm7tdmi::execute_thumb()` — Thumb instruction decoder

Also in IRAM: `downscale_565()` and `downscale_565_v2()` from the downscaler.

### Downscaler

GBA native 240x160 RGB565 is downscaled to 128x128 RGB565 using a LUT-based box-average filter (~0.8ms on ESP32-S3). No intermediate buffers, no format conversion. The LUT is 512 bytes on the stack.

## sdkconfig Options

### Hardware Build (sdkconfig.defaults)

```ini
CONFIG_CXX_EXCEPTIONS=y                  # Required for test framework (throw/catch)
CONFIG_CXX_EXCEPTIONS_EMULATION_POOL_SIZE=64
CONFIG_CXX_RTTI=y                        # Required for dynamic_cast in tests
CONFIG_IDF_TARGET="esp32s3"
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y # Single large app partition
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768    # 32KB main task stack
CONFIG_SPIRAM=y                           # Enable PSRAM
CONFIG_SPIRAM_MODE_OCT=y                  # Octal PSRAM (higher bandwidth)
CONFIG_SPIRAM_SPEED_80M=y                 # 80MHz PSRAM clock
CONFIG_SPIRAM_USE_MALLOC=y               # malloc() can return PSRAM memory
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 # Allocations <=16KB stay in SRAM
```

### QEMU Build (sdkconfig.defaults.qemu)

Same as above but with `# CONFIG_SPIRAM is not set` — QEMU does not emulate PSRAM.

## Loading ROMs

The ESP32 build does **not** include `load_rom_from_file()` (guarded by `GBA_PLATFORM_ESP32`). ROMs must be loaded programmatically:

```cpp
// Option 1: Embed ROM in firmware (add to CMakeLists.txt EMBED_TXTFILES)
// Option 2: Load from SD card (implement your own SD card reader)
// Option 3: Load via HTTP/WiFi (implement WiFi receiver)
// Option 4: Load from SPIFFS/LittleFS partition

// Once you have the bytes:
emulator.load_rom(rom_data);      // std::vector<u8> or span
emulator.load_bios(bios_data);    // optional, HLE BIOS fallback available
emulator.reset();
```

The desktop `gba_suite_runner` includes a hand-crafted BIOS stub that works for test ROMs.

## Performance Expectations

Based on 44vba (VBA-Next fork on ESP32-S3-WROOM-1-N8R8): **~20 fps with frameskip 1** in gameplay. Our emulator is lighter in some areas (no dynarec overhead, flat-array scheduler) but heavier in others (more accurate timing, prefetch buffer). Real-world performance will depend on game complexity.

Performance levers:
- **Frameskip**: Skip PPU rendering every N frames when CPU is behind
- **Scanline dirty tracking**: Only re-render scanlines where VRAM/palette/OAM changed
- **Direct scanline output**: Render directly to 128x128 instead of downscaling 240x160
- **Cached interpreter**: Pre-decode instruction blocks (planned, ~1.3-1.5x speedup)

## Flashing

```bash
# Standard flash
idf.py -p /dev/ttyUSB0 flash

# With monitor (serial output)
idf.py -p /dev/ttyUSB0 flash monitor

# Specify baud rate
idf.py -p /dev/ttyUSB0 -b 460800 flash

# Erase flash completely first
idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash

# Exit monitor: Ctrl+]
```

Flash layout: bootloader @ 0x0000, partition-table @ 0x8000, app @ 0x10000.

## Troubleshooting

### Boot loop / crash on init
- Check PSRAM config matches your module: octal vs quad, 80MHz vs 40MHz
- Verify `CONFIG_SPIRAM_MODE_OCT=y` for WROOM-1-N8R8 modules
- Enable `CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y` for more output

### White screen / no display output
- Verify ST7735 wiring (pin assignment in gba_display.h)
- Check display tab color: green tab uses offset (2,1), red/blue may differ
- Try `CONFIG_SPIRAM_SPEED_40M=y` if display glitches (PSRAM bandwidth contention)

### Out of memory
- QEMU builds have no PSRAM — only lightweight tests pass
- On hardware, verify `CONFIG_SPIRAM=y` and `CONFIG_SPIRAM_USE_MALLOC=y`
- Check `idf.py size` for memory report

### Slow framerate
- Ensure `IRAM_ATTR` functions are actually in IRAM: check map file
- Profile with `esp_timer_get_time()` around `run_frame()`
- Reduce downscale cost: render directly to 128x128 (future work)
- Enable frameskip: skip rendering when frame takes >16.7ms
