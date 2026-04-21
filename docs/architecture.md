# Architecture

## Mental Model

Treat the GBA as a hardware simulator with a cycle scheduler, not as a framebuffer app with a CPU attached.

The core runs in master GBA cycles:

- `16,777,216` cycles per second
- `1232` cycles per scanline
- `228` scanlines per frame
- `280,896` cycles per frame

## Subsystems

### CPU

- ARM7TDMI interpreter
- ARM and Thumb states
- Banked registers and modes
- Exception entry/return
- Per-instruction cycle accounting including bus timing

### Bus

- Full GBA memory map
- Waitstate-aware reads/writes
- MMIO dispatch to PPU, timers, DMA, audio, and IRQ state
- VRAM/palette/OAM access rules

### Scheduler

- Global cycle counter
- Tracks the next hardware event for each subsystem
- Picks the earliest pending event without building a heavyweight dynamic queue

### PPU

- LCD timing state machine
- Scanline renderer
- Bitmap modes first: 3, 4, then 5
- Tile/sprite effects staged in later passes

### Timers / DMA / IRQ / APU

- Timers drive overflow IRQs and Direct Sound playback cadence
- DMA owns the bus while active
- IRQ line is recomputed from `IME`, `IE`, and `IF`
- Direct Sound starts with FIFO A/B and timer-driven refill requests

## Portability

The core is kept platform-neutral. Embedded BSPs handle:

- frame presentation
- audio output
- keypad input
- storage / ROM loading
- host timing integration

The first BSP in-tree is an RT1170-oriented stub so the host build and the target structure share the same interfaces.

## Weaker MCU Guidance

If the target MCU is weaker, the right question is not "what code can be deleted?" but "which responsibilities can move to helper silicon without breaking the cycle model?"

### Good external offloads

- External RAM for ROM cache, framebuffers, tile caches, audio buffers, and cold guest memory.
- External flash or SD storage for ROM libraries, BIOS, save files, and snapshots.
- Display scanout controllers for LCD timing, RGB/LVDS signalling, backlight PWM, and host-framebuffer presentation.
- Audio DACs/codecs for digital-to-analog conversion and analog output stages.
- Small FPGAs or CPLDs for programmable video timing, simple DMA/timer assist, or display glue.

### What must stay on the main MCU

- The master GBA cycle scheduler.
- ARM7TDMI instruction semantics and exception handling.
- The bus/MMIO model and register side effects.
- IRQ pending logic and CPU interrupt entry.
- Any hardware behavior not moved into a programmable coprocessor.

### Rule of thumb

- If the MCU is memory-starved, add external RAM and storage first.
- If the MCU is display-IO starved, add a display controller or use host-side DMA scanout.
- If the MCU is compute-starved, only a programmable helper such as an FPGA or second processor changes the ceiling in a meaningful way.
