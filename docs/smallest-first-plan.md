# Smallest-First Implementation Plan

This plan is intentionally biased toward the smallest useful tasks first.

The repo already has the broad architecture in place. The next steps should tighten correctness with tiny, testable increments before adding large compatibility features.

## Guiding rule

Before adding a new large subsystem behavior, first add:

1. a deterministic test
2. the smallest code change that makes it pass
3. a validation note if the behavior will later need ROM-level comparison

## Phase 0: Lock the foundation we already have

These are the smallest, highest-leverage tasks because they reduce future debugging cost.

### Task 0.1: Document the hardware partitioning model

Done in:

- [hardware-partitioning.md](/Users/chaitanyamalhotra/development/gba%20embedded/docs/hardware-partitioning.md)

### Task 0.2: Keep architecture and roadmap docs aligned

Done in:

- [architecture.md](/Users/chaitanyamalhotra/development/gba%20embedded/docs/architecture.md)
- [component-roadmap.md](/Users/chaitanyamalhotra/development/gba%20embedded/docs/component-roadmap.md)

### Task 0.3: Add or maintain deterministic micro-tests first

Priority:

- scheduler event ordering
- IRQ acknowledge behavior
- timer overflow edge cases
- DMA immediate copy behavior
- Mode 3 scanline rendering
- tiny ARM and Thumb instruction smoke cases

## Phase 1: Smallest missing behaviors in the current core

These are narrow, local changes that improve correctness without expanding the architecture.

### Task 1.1: Keypad MMIO coverage

Implement or tighten:

- `KEYINPUT` active-low reads
- `KEYCNT` read/write behavior
- keypad IRQ request logic

Tests to add:

- key press/release readback
- `KEYCNT` IRQ enable mask behavior

### Task 1.2: HALT and wakeup edge cases

Implement or tighten:

- CPU halt interaction with pending IRQs
- correct wakeup when `IME`, `IE`, and `IF` permit service

Tests to add:

- halt with no IRQ
- halt with masked IRQ
- halt with pending enabled IRQ

### Task 1.3: Bus open-bus and alignment micro-cases

Implement or tighten:

- misaligned word/halfword reads
- open-bus propagation after unmapped or protected accesses
- palette/VRAM/OAM write quirks

Tests to add:

- misaligned load rotation checks
- palette byte replication
- VRAM byte-write restrictions
- OAM byte-write ignore behavior

### Task 1.4: Minimal BIOS/HLE SWI coverage

Implement only the smallest SWIs needed for bring-up:

- `SoftReset`
- `RegisterRamReset`
- `VBlankIntrWait` or equivalent bring-up helper path
- tiny memory helpers only if a test ROM immediately needs them

Tests to add:

- SWI entry/return
- register preservation expectations for each supported HLE SWI

## Phase 2: Small CPU correctness slices

Do not chase the whole instruction set in one pass. Add missing instructions in narrow batches.

### Task 2.1: ARM data-processing edge cases

Focus:

- carry/overflow flag correctness
- register-shifted operand timing/behavior
- PC-as-operand edge cases
- `MRS` / `MSR` behavior cleanup

### Task 2.2: ARM load/store edge cases

Focus:

- signed halfword and byte loads
- pre/post indexing writeback cases
- `LDM` / `STM` corner behavior

### Task 2.3: Thumb instruction-family completion

Focus:

- load/store families not yet covered
- stack-relative forms
- push/pop edge cases
- branch exchange and long-branch-with-link cleanup

Tests to add for each slice:

- one focused unit-style instruction test
- one short ROM trace comparison if the behavior is timing-sensitive

## Phase 3: Timing hardware in tiny steps

### Task 3.1: Timer reload and cascade edge cases

Implement or tighten:

- exact reload timing
- cascade increments
- per-timer IRQ behavior

### Task 3.2: DMA trigger micro-cases

Implement or tighten:

- VBlank-triggered DMA
- HBlank-triggered DMA
- FIFO-triggered DMA refill
- repeat and destination reload behavior

### Task 3.3: Scheduler ordering invariants

Implement or tighten:

- same-cycle event ordering between timers, DMA, IRQ, and PPU transitions
- CPU cycle advance after DMA steals bus time

Tests to add:

- event-order regression tests for each new rule

## Phase 4: PPU growth from the easiest path

### Task 4.1: Finish bitmap-mode confidence

Order:

- Mode 3 cleanup
- Mode 4 palette/page behavior
- Mode 5 size/page behavior

### Task 4.2: Add tile backgrounds before sprites

Order:

- BG mode 0 text backgrounds
- BG mode 1 layering
- BG mode 2 affine backgrounds

### Task 4.3: Add sprites after tile backgrounds are trustworthy

Then later:

- windows
- blending
- mosaic

Reason:

- sprites, windows, and blending multiply debugging cost. Keep them out until base scanline composition is stable.

## Phase 5: Audio after timer/DMA confidence

### Task 5.1: Direct Sound FIFO correctness

Focus:

- FIFO drain timing
- DMA refill request thresholds
- timer select behavior

### Task 5.2: Only then consider PSG channels

Reason:

- Direct Sound is the better correctness checkpoint for scheduler quality.

## Phase 6: Embedded-target work after host correctness

### Task 6.1: Keep BSP work thin

Do:

- frame presentation
- audio output
- key polling
- storage hooks

Do not:

- fork emulator logic into target-specific code paths

### Task 6.2: Add weak-MCU helpers in this order

1. external RAM
2. external storage
3. display/audio helper chips
4. programmable coprocessor only if profiling says compute is the blocker

## Immediate next checklist

If we continue from the current repo state, the best smallest-first order is:

1. Add `KEYINPUT` and `KEYCNT` deterministic tests.
2. Add keypad IRQ behavior.
3. Add HALT wakeup tests and fix any edge cases.
4. Add bus alignment/open-bus regression tests.
5. Add one minimal HLE SWI path with tests.
6. Add one focused missing Thumb instruction-family test and implementation.
7. Add one focused DMA trigger test beyond immediate mode.

## Definition of done for each task

Each small task is done only when:

- the behavior is covered by a deterministic test
- the code change is isolated and documented by the test name
- `ctest --test-dir build --output-on-failure` still passes
- the docs still describe the current behavior accurately
