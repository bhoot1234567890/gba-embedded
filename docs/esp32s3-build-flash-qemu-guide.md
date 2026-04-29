# ESP32-S3 Build, Flash, and QEMU Guide

This guide is for the local workspace at:

```sh
/Users/chaitanyamalhotra/development/gba embedded
```

The installed ESP-IDF is:

```sh
/Users/chaitanyamalhotra/.espressif/esp-idf-v5.5.2
```

The ESP-IDF Python environment used successfully on this machine is:

```sh
/Users/chaitanyamalhotra/.espressif/python_env/idf5.5_py3.13_env
```

The Espressif QEMU binary selected by `export.sh` is:

```sh
/Users/chaitanyamalhotra/.espressif/tools/qemu-xtensa/esp_develop_9.2.2_20250817/qemu/bin/qemu-system-xtensa
```

There is also another QEMU tool directory installed under `.espressif`, but the active ESP-IDF environment currently puts the `20250817` build on `PATH`. Prefer the tool selected by `export.sh` unless you are deliberately testing a different QEMU.

## Shell Setup

Run this in every new terminal before using `idf.py`:

```sh
cd "/Users/chaitanyamalhotra/development/gba embedded/platform/esp32s3"

export IDF_PYTHON_ENV_PATH=/Users/chaitanyamalhotra/.espressif/python_env/idf5.5_py3.13_env
source /Users/chaitanyamalhotra/.espressif/esp-idf-v5.5.2/export.sh

idf.py --version
which qemu-system-xtensa
qemu-system-xtensa --version
```

Expected IDF version:

```text
ESP-IDF v5.5.2
```

## Build Profiles

Keep separate build directories and separate `sdkconfig` files per profile. This avoids `idf.py set-target` rewriting the shared `platform/esp32s3/sdkconfig`.

Use this pattern:

```sh
idf.py -B <build-dir> \
  -D SDKCONFIG=<build-dir>/sdkconfig \
  -D SDKCONFIG_DEFAULTS="<defaults>" \
  -D IDF_TARGET=esp32s3 \
  <other -D options> \
  build
```

Avoid running `idf.py set-target` in the shared project directory unless you intentionally want ESP-IDF to rewrite `platform/esp32s3/sdkconfig`.

If you reuse an old build directory after changing defaults, clear it first:

```sh
idf.py -B <build-dir> fullclean
```

## Hardware Display Test

Use this first when validating the ESP32-S3 Super Mini and SSD1351 wiring from the Arduino test sketch:

```text
SSD1351 SCLK -> GPIO12
SSD1351 MOSI -> GPIO11
SSD1351 CS   -> GPIO10
SSD1351 DC   -> GPIO9
SSD1351 RST  -> GPIO8
SSD1351 VCC  -> 3V3
SSD1351 GND  -> GND
```

Build:

```sh
idf.py -B build_display_test_ssd1351 \
  -D SDKCONFIG=build_display_test_ssd1351/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.supermini" \
  -D IDF_TARGET=esp32s3 \
  -D GBA_BUILD_DISPLAY_TEST=ON \
  -D GBA_DISPLAY_DRIVER=2 \
  -D GBA_DISPLAY_WIDTH=128 \
  -D GBA_DISPLAY_HEIGHT=96 \
  -D GBA_DISPLAY_PIN_SCLK=GPIO_NUM_12 \
  -D GBA_DISPLAY_PIN_MOSI=GPIO_NUM_11 \
  -D GBA_DISPLAY_PIN_CS=GPIO_NUM_10 \
  -D GBA_DISPLAY_PIN_DC=GPIO_NUM_9 \
  -D GBA_DISPLAY_PIN_RST=GPIO_NUM_8 \
  -D GBA_DISPLAY_PIN_BL=GPIO_NUM_NC \
  build
```

Flash and monitor:

```sh
idf.py -B build_display_test_ssd1351 -p /dev/cu.usbmodem21301 flash monitor
```

If the port changes, find it with:

```sh
ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null
```

The display test should show red, green, and blue bands, a white border, and a moving yellow square.

## Playable Runtime Build

Pokemon Unbound is too large for the old full-ROM-in-RAM path. The playable ESP32-S3 path is the SD-backed ROM provider with a page cache. Put the ROM on a FAT32 SD card, usually in the root:

```text
/Pokemon Unbound (v2.1.1.1).gba
```

Optional BIOS locations on the SD card:

```text
/gba_bios.bin
/bios/gba_bios.bin
/BIOS/gba_bios.bin
```

The runtime writes `.sav` next to the ROM.

For the current SSD1351 display wiring, do not put SD on GPIO8, 9, 10, 11, or 12. A simple SDSPI wiring that avoids the display pins is:

```text
SD MISO -> GPIO13
SD MOSI -> GPIO14
SD SCLK -> GPIO15
SD CS   -> GPIO16
SD VCC  -> 3V3
SD GND  -> GND
```

Build a runtime with the SSD1351 display and SDSPI ROM cache:

```sh
idf.py -B build_supermini_ssd1351 \
  -D SDKCONFIG=build_supermini_ssd1351/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.supermini" \
  -D IDF_TARGET=esp32s3 \
  -D GBA_BUILD_RUNTIME=ON \
  -D GBA_BUILD_DISPLAY_TEST=OFF \
  -D GBA_DISPLAY_DRIVER=2 \
  -D GBA_DISPLAY_WIDTH=128 \
  -D GBA_DISPLAY_HEIGHT=96 \
  -D GBA_DISPLAY_PIN_SCLK=GPIO_NUM_12 \
  -D GBA_DISPLAY_PIN_MOSI=GPIO_NUM_11 \
  -D GBA_DISPLAY_PIN_CS=GPIO_NUM_10 \
  -D GBA_DISPLAY_PIN_DC=GPIO_NUM_9 \
  -D GBA_DISPLAY_PIN_RST=GPIO_NUM_8 \
  -D GBA_DISPLAY_PIN_BL=GPIO_NUM_NC \
  -D GBA_STORAGE_MODE=2 \
  -D GBA_ROM_BACKEND=1 \
  -D GBA_SDSPI_PIN_MISO=GPIO_NUM_13 \
  -D GBA_SDSPI_PIN_MOSI=GPIO_NUM_14 \
  -D GBA_SDSPI_PIN_SCLK=GPIO_NUM_15 \
  -D GBA_SDSPI_PIN_CS=GPIO_NUM_16 \
  build
```

Flash:

```sh
idf.py -B build_supermini_ssd1351 -p /dev/cu.usbmodem21301 flash monitor
```

If you do not have buttons or audio wired yet, disable those pins at build time so the firmware does not expect them:

```sh
-D GBA_INPUT_PIN_A=GPIO_NUM_NC \
-D GBA_INPUT_PIN_B=GPIO_NUM_NC \
-D GBA_INPUT_PIN_SELECT=GPIO_NUM_NC \
-D GBA_INPUT_PIN_START=GPIO_NUM_NC \
-D GBA_INPUT_PIN_RIGHT=GPIO_NUM_NC \
-D GBA_INPUT_PIN_LEFT=GPIO_NUM_NC \
-D GBA_INPUT_PIN_UP=GPIO_NUM_NC \
-D GBA_INPUT_PIN_DOWN=GPIO_NUM_NC \
-D GBA_INPUT_PIN_R=GPIO_NUM_NC \
-D GBA_INPUT_PIN_L=GPIO_NUM_NC \
-D GBA_AUDIO_PIN_BCK=GPIO_NUM_NC \
-D GBA_AUDIO_PIN_WS=GPIO_NUM_NC \
-D GBA_AUDIO_PIN_DOUT=GPIO_NUM_NC
```

For actual play, replace the `GPIO_NUM_NC` input pins with real button GPIOs. Buttons are active-low: one side to the GPIO, the other side to ground.

## QEMU Core Smoke Test

QEMU is useful for the ESP32-S3 core smoke firmware in `platform/esp32s3/main/gba_test_main.cpp`.

QEMU is not a playable GBA target for this project. It does not emulate the board-level pieces the runtime needs: PSRAM behavior, SPI display, SD card, MAX98357A audio, or physical GPIO buttons. Use QEMU to catch startup/core regressions, then use real hardware for display, SD, input, and performance testing.

Build the QEMU smoke firmware with an isolated sdkconfig:

```sh
idf.py -B build_qemu \
  -D SDKCONFIG=build_qemu/sdkconfig \
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.qemu \
  -D IDF_TARGET=esp32s3 \
  -D GBA_BUILD_RUNTIME=OFF \
  -D GBA_BUILD_DISPLAY_TEST=OFF \
  build
```

Run it:

```sh
idf.py -B build_qemu qemu --qemu-extra-args="-nographic"
```

The current QEMU command has been verified to boot the smoke firmware and reach `app_main()`.

Current known result on 2026-04-29:

```text
=== Results: 4 passed, 2 failed ===
SOME TESTS FAILED
```

The two current failing QEMU smoke cases are:

```text
timer_overflow_irq
audio_fifo
```

Those are smoke-test follow-ups, not evidence that the playable hardware runtime cannot boot. A future clean QEMU smoke run should print:

```text
ALL TESTS PASSED on ESP32-S3!
```

Stop QEMU from the terminal with `Ctrl-a x` when using `-nographic`.

The helper script `tools/esp32_qemu_smoke.py` exists, but the preferred path for ESP32-S3 is `idf.py qemu` because it builds and uses the QEMU flash image in the same way ESP-IDF expects.

## Build Artifacts

Useful outputs:

```text
platform/esp32s3/build_supermini_ssd1351/gba_esp32_test.bin
platform/esp32s3/build_supermini_ssd1351/gba_esp32_test.elf
platform/esp32s3/build_display_test_ssd1351/gba_esp32_test.bin
platform/esp32s3/build_qemu/gba_esp32_test.elf
```

For manual flashing, ESP-IDF prints a full `esptool.py write_flash` command at the end of each build. Prefer `idf.py -B <build-dir> flash monitor` unless you specifically need the raw `esptool.py` command.

## Quick Troubleshooting

- If the display is blank, re-run the display test build before the full runtime.
- If SD mount fails, confirm that SD pins do not overlap display pins and that the card is FAT32.
- If Unbound does not appear in the ROM menu, confirm the file has a `.gba` extension and is in the SD root or scanned directory.
- If boot logs show no PSRAM on real hardware, use `sdkconfig.defaults.supermini` for the 2MB quad PSRAM board or `sdkconfig.defaults` for an 8MB octal PSRAM module.
- If a build unexpectedly changes `platform/esp32s3/sdkconfig`, rebuild with `-D SDKCONFIG=<build-dir>/sdkconfig`.
