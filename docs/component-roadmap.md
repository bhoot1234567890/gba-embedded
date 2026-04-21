# GBA Emulator Component Roadmap (Embedded Target)

To run GBA games on an embedded system, you should build the emulator as modular components and integrate them through a single cycle scheduler.

## Core Components (Build Independently, Integrate Together)

| # | Component | Why it exists | Minimum milestone before moving on |
|---|---|---|---|
| 1 | Cartridge + BIOS + Save media | Load ROM/BIOS and provide non-volatile save behavior | Boot ROM data accessible at correct addresses; save media read/write plumbing works |
| 2 | ARM7TDMI CPU (ARM + Thumb) | Execute game code correctly | Stable fetch/decode/execute loop, flags, modes, exceptions, branch behavior |
| 3 | Memory bus + memory map | Connect CPU to WRAM/VRAM/MMIO with correct semantics | Correct region routing, alignment behavior, MMIO access hooks |
| 4 | Waitstates + open-bus behavior | Preserve hardware quirks required by real games | Region timing applied, unmapped/protected reads produce hardware-like values |
| 5 | Global cycle scheduler | Keep all subsystems synchronized in GBA master cycles | CPU runs until next hardware event, then device service at exact cycle |
| 6 | Interrupt controller (IME/IE/IF) | Deliver IRQs exactly when requested | IRQ pending logic and exception entry/return behavior reliable |
| 7 | Timers (0-3, cascade, IRQ) | Drive timing-dependent gameplay/audio events | Accurate increment/overflow/reload and IRQ signaling |
| 8 | DMA (0-3) | Critical for graphics/audio/memory movement | Immediate/VBlank/HBlank/special starts and transfer completion IRQs |
| 9 | PPU timing + renderer | Produce visible frames | Correct scanline/VBlank/HBlank cadence; Mode 3 first, then 4/5, then tile/sprite pipeline |
| 10 | Audio (Direct Sound + PSG) | Required for playability and compatibility | FIFO/timer-driven direct sound first, then PSG channels/mix path |
| 11 | Input (KEYINPUT/KEYCNT) | Accept player controls and keypad IRQ behavior | Button matrix reads are correct; keypad IRQ path functional |
| 12 | Embedded BSP layer | Bridge core to target hardware | Frame output, audio output, input polling, ROM loading, host tick integration |
| 13 | Test harness + diagnostics | Catch regressions and debug edge cases | CPU/PPU/DMA/timer test ROM workflow and trace/log compare pipeline |

## Suggested Bring-up Order

1. Cartridge + BIOS + CPU + Bus + Scheduler
2. IRQ + Timers + basic DMA
3. PPU (Mode 3 first) + input
4. Direct Sound FIFO audio
5. Compatibility pass: tile/sprite/blending/windows, full DMA timing, open-bus edge cases, save types

## Notes for Independent Work

- Keep each component behind a clear interface and test it with targeted ROMs before broad integration.
- Use GBA master cycles as the only timing source of truth; avoid local "best effort" timers inside subsystems.
- Treat undocumented behavior as compatibility work items, not optional polish.
