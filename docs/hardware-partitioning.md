# Hardware Partitioning For Weaker MCUs

This note captures what can be pushed out of the main MCU when the target is too small to do everything comfortably.

The important framing is unchanged: the emulator is still a cycle-scheduled hardware model. External chips can reduce memory pressure, pin pressure, scanout work, and analog/audio burden, but they do not make the GBA CPU disappear.

## What can be offloaded cleanly

### 1. External RAM

Use external RAM for:

- ROM caching
- host framebuffers
- tile and sprite caches
- audio ring buffers
- less-hot guest memory mirrors

This is the first offload to consider when the MCU is mostly constrained by SRAM size, not raw compute.

Examples:

- Infineon HyperRAM / HYPERRAM family, which explicitly targets high-bandwidth buffer use and advertises HYPERBUS with up to 400 MB/s throughput on current parts:
  - [Infineon S27KL0642](https://www.infineon.com/part/S27KL0642DPBHB023)

### 2. External flash or SD storage

Use external storage for:

- ROM sets
- BIOS images
- save files
- savestates
- configuration blobs

This does not reduce emulation compute, but it reduces on-chip storage requirements and simplifies multi-ROM systems.

Examples:

- Winbond serial NOR family, including current `W25Q128JV` listings in the product guide:
  - [Winbond code-storage guide](https://www.winbond.com/export/sites/winbond/product-selection-guide/file/2025-Product-Selection-Guide-Winbond-Code-Storage-Flash-Memory.pdf?__locale=en)

### 3. Display scanout hardware

Use a display controller when the MCU should not spend effort on:

- LCD timing generation
- RGB/LVDS signalling
- backlight PWM
- touchscreen controller glue
- presenting a prepared host framebuffer

This helps a lot for board design and scanout stability. It does not replace the GBA PPU unless the helper is programmable enough to implement GBA-specific raster behavior.

Examples:

- Bridgetek `BT817/818`, which provide display, audio, and touch support over a host-command model:
  - [BT817/818 datasheet](https://brtchip.com/wp-content/uploads/Support/Documentation/Datasheets/ICs/EVE/DS_BT817_8.pdf)

### 4. Audio DAC or codec

Use a DAC/codec to offload:

- digital-to-analog conversion
- line/headphone drive stages
- audio clocking and output formatting

This improves output quality and lowers analog work on the MCU board, but it does not materially reduce emulation CPU time by itself.

Examples:

- SPI DAC:
  - [Microchip MCP4822](https://ww1.microchip.com/downloads/en/DeviceDoc/21953a.pdf)
- I2S DAC:
  - [Cirrus Logic WM8524](https://statics.cirrus.com/pubs/proDatasheet/WM8524_v4.1.pdf)

### 5. Small FPGA or programmable logic

This is the first helper that can actually move real emulator work away from the MCU.

Candidate jobs:

- LCD timing and scanout
- a host framebuffer pipeline
- simple bus arbitration helpers
- timer overflow and DMA trigger assist
- portions of the PPU raster path
- Direct Sound FIFO drain and sample packing

Examples:

- [Lattice iCE40 UltraPlus overview](https://www.latticesemi.com/Products/FPGAandCPLD/iCE40UltraPlus.)
- [Lattice iCE40 UltraPlus datasheet](https://www.latticesemi.com/-/media/LatticeSemi/Documents/DataSheets/iCE/FPGA-DS-02008-1-9-iCE40-UltraPlus-Family-Data-Sheet.ashx?document_id=51968)

## What must stay on the main MCU

Unless you add a real programmable coprocessor, the main MCU must still own:

- the master cycle scheduler
- ARM7TDMI instruction execution
- CPSR/SPSR, mode banking, and exception semantics
- bus waitstate logic
- MMIO side effects
- IRQ pending logic
- cartridge and save behavior
- overall system ordering between CPU, DMA, timers, PPU, and APU

In practice: a display controller can present pixels, but it cannot "be the GBA PPU" unless it is programmable enough to reproduce GBA-specific behavior.

## Offloads that look attractive but usually disappoint

- A generic graphics controller:
  - Good for scanout.
  - Not good for tile priorities, windows, blending, VRAM restrictions, and raster timing.
- External RAM:
  - Great for capacity.
  - Can hurt if hot CPU state and timing-critical structures move off-chip.
- DACs/codecs:
  - Great for audio output quality.
  - Do not solve FIFO timing or mixer correctness.

## Practical partition options

### Option A: Slightly weak MCU

Main MCU:

- full emulator core
- scheduler
- CPU
- bus/MMIO
- PPU
- DMA/timers/IRQ/APU

External helpers:

- RAM
- flash/SD
- LCD controller or host DMA display path
- DAC/codec

Best when the MCU is mainly short on memory or pins.

### Option B: Moderately weak MCU

Main MCU:

- CPU
- scheduler
- MMIO/bus control
- overall ordering

Programmable helper:

- PPU scanout
- some timer/DMA/audio assist

Best when the MCU can still run the guest CPU but needs help with video/audio housekeeping.

### Option C: Very weak MCU

Main MCU alone is not enough for a full-speed, broad-compatibility emulator.

Choices:

- accept partial compatibility
- accept reduced speed
- add an FPGA or second processor

## Rules of thumb

- If the target is memory-starved, add RAM first.
- If the target is I/O-starved, add display/audio helpers next.
- If the target is compute-starved, only programmable compute offload materially changes feasibility.
- Keep hot emulator state on the main MCU whenever possible, even if bulk assets live off-chip.

## Candidate target framing

- `i.MX RT1170`: full core on the MCU is realistic; external helpers are optional quality-of-life additions.
- `STM32H7`: full core is plausible, but external RAM and strong display/audio plumbing help a lot.
- `ESP32-P4`: external RAM and careful display partitioning are likely part of the design from day one.
- `RP2350`: treat this as a partial-compatibility or helper-heavy target unless the scope is intentionally narrow.
