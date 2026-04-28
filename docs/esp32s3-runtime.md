# ESP32-S3 Runtime Guide

This is the single current ESP32-S3 note for the GBA runtime. Older analysis and backup documents were removed because they described stale boot blockers, QEMU-only config, or optimization plans that are now partly implemented.

## Hardware target

The runtime is tuned for an ESP32-S3 module with octal PSRAM, such as ESP32-S3-WROOM-1-N8R8.

- CPU: fixed 240 MHz.
- PSRAM: octal mode at 80 MHz.
- WiFi and Bluetooth: disabled.
- Display: 128x128 ST7735 over SPI.
- Audio: MAX98357A over I2S, no MCLK.
- Storage: SD card, defaulting to SDSPI in `platform/esp32s3/main/gba_board_profile.h`.
- Input: active-low buttons with internal pull-ups, defined in the board profile.

`platform/esp32s3/sdkconfig.defaults` is the hardware runtime config. `sdkconfig.defaults.qemu` is intentionally separate because QEMU does not emulate PSRAM.

## SD card layout

Put `.gba` files in the SD card root. The runtime presents a simple ROM picker and creates/loads `.sav` next to the selected ROM.

Optional real BIOS locations:

- `/sdcard/gba_bios.bin`
- `/sdcard/bios/gba_bios.bin`
- `/sdcard/BIOS/gba_bios.bin`

If a valid 16 KiB BIOS is found, the emulator boots through it. If no BIOS is found, the runtime uses skip-BIOS reset and starts execution at `0x08000000`.

When debugging a real-BIOS boot, seeing the CPU PC inside low BIOS addresses such as `0x000001B4` is not automatically a hang. The real BIOS is also used by games for halt/interrupt wait routines, so framebuffer progress and cycle/IRQ state are better boot indicators than PC alone.

## Boot flow

The runtime flow is:

1. Mount SD card.
2. Select ROM from the on-device menu.
3. Load optional BIOS.
4. Load ROM.
5. Auto-detect save type and load `.sav`.
6. Enable direct 128x128 PPU output.
7. Call `emulator.reset(skip_bios)`.
8. Start the CPU task and display task pipeline.

This order matters. Reset before loading the ROM or BIOS leaves the CPU executing from an empty or open-bus address space.

## Performance choices

Implemented:

- Direct PPU 128x128 render path is enabled on ESP32-S3.
- The old 240x160 to 128x128 runtime downscale loop is removed.
- The runtime still converts RGB555 to RGB565 after rendering because the ST7735 expects RGB565 pixels.
- Whole-frame render skip is enabled when the previous frame exceeds 16.7 ms.
- `Timers::advance_to`, `DmaEngine::service_due`, and `Ppu::advance_to` are marked for IRAM on ESP32 builds.
- CPU hot loops and bus reads/writes were already in IRAM.
- BIOS `Sqrt`, `ArcTan`, and `ArcTan2` HLE paths use integer math, avoiding ESP32 soft-float libm cost when skip-BIOS mode is active.
- The CPU task feeds the task watchdog once per emulated frame.
- Display transfers are chunked by rows to reduce SPI transfer size.

Important detail: because the runtime uses ping-pong display buffers, each rendered frame marks all 128 visible lines dirty before `run_frame()`. This avoids stale scanlines in the back buffer when only a subset of VRAM changed.

## Board profile

Default input pins are defined in `platform/esp32s3/main/gba_board_profile.h`.

Current defaults:

- A: GPIO10
- B: GPIO11
- Select: GPIO16
- Start: GPIO18
- Right: GPIO38
- Left: GPIO39
- Up: GPIO40
- Down: GPIO41
- R: GPIO42
- L: GPIO21

Buttons are active-low. Wire one side of each button to the GPIO and the other side to ground. If your board uses different wiring, override the `GBA_INPUT_PIN_*` macros in the build or edit the board profile.

Default MAX98357A audio pins:

- BCLK: GPIO35
- LRCLK/WS: GPIO36
- DIN: GPIO37

Wire MAX98357A `VIN` to 3.3V for modest volume or 5V for louder speaker output, `GND` to ground, and the speaker across the amplifier output terminals. Do not connect either speaker terminal to ground. The runtime sends 32768 Hz 16-bit stereo I2S; the MAX98357A board can select left, right, or mono mix in hardware.

## Build and flash

From the ESP32-S3 project directory:

```sh
cd platform/esp32s3
idf.py set-target esp32s3
idf.py reconfigure
idf.py build flash monitor
```

Run `idf.py reconfigure` after changing `sdkconfig.defaults`, especially after switching between QEMU and real hardware defaults.

## Remaining hardware-dependent work

- Verify the default GPIO map against the exact ESP32-S3 board before soldering.
- If the MAX98357A breakout exposes shutdown/gain/channel-select pins, strap them on the board for the desired gain and mono behavior.
- If a specific display module has color order or offset differences, adjust `gba_display.h`.
- If PSRAM instability appears, test 40 MHz PSRAM as a diagnostic only. The performance target is 80 MHz octal PSRAM.
