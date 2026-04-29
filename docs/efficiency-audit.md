# Efficiency Audit Current Status

Last checked against code on 2026-04-29 after the ESP32-S3 performance pass.

## Implemented

- B1: CPU hardware service batching is fixed with a 16-instruction countdown.
- B2: duplicate undefined-instruction exception raising was removed.
- B3: PPU register writes now dirty scanlines without always invalidating the tile cache. VRAM, OAM, and palette writes still invalidate tile decode cache.
- B4: duplicate `refresh_schedule()` calls were removed from the frame/run loops.
- H1: bus read/write/peek hot paths no longer construct per-access spans; they use raw memory pointers with wrapped pointer helpers.
- H2/M11: bus read/write now compute common region cycles inside the main dispatch instead of calling a separate `region_cycles()` decode pass.
- H3/P8: desktop defaults to Release and ESP32-S3 uses performance optimization instead of debug `-Og`.
- M1/M18 partial: `Cartridge` caches ROM size and a direct contiguous read pointer when the provider allows it. Desktop profiling remains provider-routed by default so ROM cache statistics are preserved.
- M3 partial: CPU DMA servicing is guarded by `dma_next_event_cycle()` before calling the full service path.
- M4/M5: PPU tiebreaker lookup and affine scanline coefficient hoisting are implemented.
- M7: PPU object rendering now uses cached per-scanline active sprite lists instead of scanning all 128 sprites in every rendered line.
- M12 partial: DMA has direct simple incrementing RAM-to-VRAM fast paths, and the existing ROM-to-VRAM path now avoids per-unit span construction on the write side.
- M8/M9/M10: scheduler/hardware service, timers, PPU advance, and APU advance have ESP32 IRAM annotations where currently wired.
- M14: `Scheduler::next_event()` has a cached minimum.
- M15: ESP32 SD ROM page lookup is O(1), mmap window lookup is O(1), and the SD provider separates the IRAM cache-hit path from the slow SD miss path.
- M16 partial: CPU mode switching now uses an indexed R13/R14 bank table. A full register-pointer table is still open.
- P1/P2: ESP32 runtime does two-pixel RGB555 to RGB565 conversion and submits a full display frame in one draw call.
- P3 partial: ESP32 ROM provider read paths and SD cache-hit lookup are IRAM-marked. The display byte-swap loop is still scalar.
- P4/P5/P7/P9: audio chunking, aligned PSRAM framebuffers, cache-line-separated atomics, and ESP32 HLE BIOS compile definition are implemented.
- Display DMA safety: ESP32 display transfer now uses `esp_cache_msync()` and waits on `on_color_trans_done()` before reusing the transfer buffer.
- QEMU smoke status: ESP32-S3 smoke firmware now reports 6 passed, 0 failed under QEMU. The QEMU process is still terminated by the harness timeout after `app_main()` returns.

## Still Open

- H4/L1/L14: prefetch penalty and sequential-boundary logic is still duplicated across read/write/save paths.
- M1/M18: full ROM provider type erasure is still open. Non-contiguous providers and profiled desktop reads still use virtual provider calls intentionally.
- M2: ARM/Thumb execute paths still define many hot lambdas inside each instruction call.
- M3: a latched DMA-pending flag is still open; the current implementation uses the cheaper next-event guard.
- M6/M17/L3: window span optimization, sprite tile cache, and incremental sprite affine stepping are still open.
- M12: broader DMA fast paths are still open for ROM-to-RAM, RAM-to-RAM, decrement/fixed modes, and exact boundary-specialized burst loops.
- M13: `refresh_schedule()` still polls all subsystem next-event values; per-slot dirty scheduling is still open.
- M16/L10: the full CPU register pointer table is still open; only the R13/R14 bank dispatch has been flattened.
- P3: the display byte-swap loop is still scalar and not separately IRAM-specialized.
- P6: CPU task stack was reduced from 32KB to 16KB, but the audit's 8KB target still needs high-water-mark validation before shrinking further.
- Low-impact items L2-L13 are mostly still open unless covered above.

## Notes

- The old chunked-transfer finding is superseded. Runtime display output now submits one full frame, and the transfer is intentionally synchronized for safe buffer reuse.
- The old cache-coherency note is superseded. The display path now explicitly calls `esp_cache_msync()` before LCD DMA.
- The SSD1351 byte-swap loop is still scalar. It is now a full-frame swap into an internal DMA buffer rather than a per-chunk loop.

## Historical Notes Superseded By Current Code

1. Chunked display transfers were replaced by full-frame `display_draw()` calls in the runtime.
2. Display DMA cache coherency is no longer theoretical. `gba_display.cpp` now calls `esp_cache_msync()` before LCD DMA and waits for `on_color_trans_done()`.
3. The SSD1351 byte-swap loop is still scalar, but it now swaps a full-frame transfer buffer rather than repeated small chunks.
4. Runtime framebuffers now use 64-byte aligned PSRAM allocation. The display swap buffer is allocated with DMA-capable memory.

# Efficiency Audit & Bug Tracker

Cross-checked against source. All findings verified unless noted.

## Bugs

### B1. `_sc` service counter broken — timer/DMA/IRQ serviced every instruction

**Location:** `src/core/cpu.cpp:1105-1108`

```cpp
int _sc = 0;
while (current_cycle_ < target_cycle) {
    if (_sc <= 0) { _sc = 0; bus_.service_timers(...); ... }
    --_sc;
```

`_sc` starts at 0, decremented to -1, so `_sc <= 0` fires every iteration. The intent was to batch service calls every N instructions, but `_sc` is never assigned a positive value.

**Fix:** Change `_sc = 0` to `_sc = 16` (or 32) inside the `if` block.

**Impact:** HIGH. `service_timers()`, `service_dma()`, and `irq_.advance()` run on every single instruction. Batching to every 16 instructions eliminates ~94% of these calls.

---

### B2. Double `raise_exception(Undefined)` clobbers R14

**Location:** `src/core/cpu.cpp:2274+2282` (ARM), `2880+2888` (Thumb)

```cpp
arm_undefined:
raise_exception(ExceptionType::Undefined);  // correct — enters UND mode, sets SPSR/R14
// ... logging ...
raise_exception(ExceptionType::Undefined);  // BUG — overwrites R14 with wrong value
current_cycle_ += 3;
```

The first call correctly switches to UND mode, banks registers, sets SPSR and R14. The second call re-enters the exception handler and overwrites the saved return address.

**Fix:** Remove the second `raise_exception` call and the `current_cycle_ += 3` at lines 2282-2283 and 2888-2889.

**Impact:** HIGH. Any game hitting an undefined instruction gets a corrupted return address in R14.

---

### B3. `mark_all_dirty()` on every PPU register write — tile cache always cold

**Location:** `src/core/ppu.cpp:270-271, 771`

```cpp
void Ppu::write_register(...) {
    // ... register-specific handling ...
    update_dispstat_flags();
    mark_all_dirty();  // unconditionally
}
```

`mark_all_dirty()` increments `tile_cache_epoch_`, invalidating all 2048 tile cache entries. This fires on writes to DISPSTAT IRQ flags, GREENSWP, and other registers that don't affect tile rendering. Games that write DISPSTAT frequently (HBlank IRQ sync) suffer most.

**Fix:** Only call `mark_all_dirty()` for registers that affect BG/OBJ rendering: `dispcnt_` mode/bg-enable bits, `bgcnt_`, `bghofs_`, `bgvofs_`, affine parameters, VRAM/OAM. Writes to `dispstat_` IRQ flags, `greenswp_`, etc. should skip.

Additionally, split the single `mark_all_dirty()` into independent tracking:
- `screen_layout_dirty_` — set when BG enables, mosaic, blend, or window registers change. Triggers full recomposition.
- Tile cache epoch increment — only when tile data source changes (`bgcnt_` char base, VRAM writes, palette writes). Avoids trashing 2048 cache entries on unrelated MMIO writes.

**Impact:** HIGH. On ESP32 with PSRAM VRAM, destroying the tile cache forces redundant re-decode of tile data for the entire frame.

---

### B4. `refresh_schedule()` called twice per main loop iteration

**Location:** `src/core/emulator.cpp:55,63` and `169`

```cpp
void Emulator::run_until(u64 target_cycle) {
    while (cpu_.current_cycle() < target_cycle) {
        refresh_schedule();                // ← FIRST call (line 55)
        // ... run CPU ...
        service_due_hardware();            // → refresh_schedule() again (line 169)
    }
}
```

`run_frame()` has the same pattern at lines 70 and 77. `service_due_hardware()` unconditionally calls `refresh_schedule()` at line 169 as its final step. This means the first `refresh_schedule()` call's work (6× `next_event_cycle` + 6× `set_next_event`) is **immediately overwritten** by the call inside `service_due_hardware()` — pure wasted work.

**Fix:** Remove the explicit `refresh_schedule()` call from `run_until()` and `run_frame()`. Let `service_due_hardware()` handle it once after all hardware is advanced. The first call existed because `cpu_run_until()` needs a target cycle — but `scheduler_.next_event()` already computes that BEFORE the hardware is advanced, and `service_due_hardware()` recomputes it AFTER. Restructure: move `scheduler_.next_event()` before `cpu_run_until()`, and have `service_due_hardware()` call `refresh_schedule()` as its final step (already does).

**Impact:** MEDIUM. Cuts `refresh_schedule()` calls per frame iteration in half — from 2 to 1. Each call does 7 scheduler slot writes + 6 subsystem queries.

---

## High-Impact Throughput

### H1. 5 `std::span` objects constructed per bus `read()`/`write()`

**Location:** `src/core/bus.cpp:226-230` (read), `412-416` (write)

```cpp
const auto ewram_span = std::span<const u8>{ewram_.get(), kEwramSize};
const auto iwram_span = std::span<const u8>{iwram_.get(), kIwramSize};
const auto palette_span = std::span<const u8>{palette_.get(), kPaletteSize};
const auto vram_span = std::span<const u8>{vram_.get(), kVramSize};
const auto oam_span = std::span<const u8>{oam_.get(), kOamSize};
```

80 bytes of stack setup per call, hundreds of thousands of calls per frame. Pointers never change after construction.

**Fix:** Store as `const` member spans initialized once in `Bus` constructor or `reset()`.

---

### H2. `region_cycles()` re-does address decoding — every bus access decoded twice

**Location:** `src/core/bus.cpp:918-943`

`region_cycles()` repeats the same `(address & 0x0F000000u)` if-else cascade as `read()`/`write()`. Every bus access traverses region logic twice — once in `region_cycles()` for cycle computation, once in the main body for data routing.

**Fix:** Compute cycles as a side effect of the main dispatch. Return cycles from the region handler instead of calling a separate function.

---

### H3. No Release optimization flags in CMakeLists

**Location:** `CMakeLists.txt:44-56`

Only warning flags are set. The build commands in CLAUDE.md (`cmake -S . -B build`) don't specify `CMAKE_BUILD_TYPE`, defaulting to `-O0`.

**Fix:** Either:
- Build with `-DCMAKE_BUILD_TYPE=Release` (adds `-O3 -DNDEBUG`)
- Or add explicit flags: `-O3 -flto -march=native` for desktop benchmarks

---

### H4. Prefetch penalty logic duplicated across read/write/SRAM paths

**Location:** `src/core/bus.cpp:294-316` (read lambdas), `468-478` (write ROM), `483-489` (write SRAM)

The prefetch stop penalty computation appears in three places with slight variations:

- **Read path** (lines 294-304): two lambdas `stop_prefetch_penalty` and `data_prefetch_penalty` defined inside the ROM `if` block, re-created on every ROM access.
- **Write ROM path** (lines 468-478): same `half_duty_plus_one` + `countdown` logic inlined for write accesses — but uses different conditions than the read path for DMA vs non-DMA.
- **Write SRAM path** (lines 483-489): identical penalty logic for save-memory writes.

Each block independently computes `half_duty_plus_one = (prefetch_.duty >> 1) + 1` and checks `prefetch_.countdown == 1` with the same patterns. The read path also duplicates this inside two separate lambdas (one for stop penalties, one for data penalties, both calling the first). In total the same logic exists in **4 locations**.

**Fix:** Extract a single `[[nodiscard]] u32 prefetch_stop_penalty(bool is_dma, bool is_code_fetch) const` inline helper. Read path calls it directly instead of defining two lambdas. Write paths reuse the same helper.

**Impact:** MEDIUM. Reduces ROM-path code size, eliminates redundant lambda construction, and makes the prefetch model auditable in one place. The actual cycle cost is low (compiler inlines), but the duplication is a maintenance hazard for prefetch timing fixes.

---

## Medium-Impact Throughput

### M1. Virtual dispatch on every ROM access

**Location:** `include/gba/core/rom_provider.hpp:28-42`

`read_byte()`, `read16()`, `read32()` are virtual. ROM is the hottest bus path (every code fetch from GamePak). Virtual calls prevent inlining and cost ~2 extra cycles per call on Xtensa.

**Fix:** Replace with `std::variant<MemoryRomProvider, Esp32MmapRomProvider, Esp32SdCacheRomProvider>` + `std::visit` for monomorphization. Or template the cartridge on provider type.

---

### M2. 12+ lambdas defined per `execute_arm()`/`execute_thumb()` call

**Location:** `cpu.cpp:1633-1716` (ARM), `2289-2377` (Thumb)

~15 lambdas (`read_reg`, `write_pc`, `data_access`, `break_fetch_burst`, `read8/16/32`, `write8/16/32`, `trace_arm_gpio`, etc.) defined each call. Compiler likely inlines but adds code bloat in IRAM.

**Fix:** Lift hot lambdas to private member functions. Keep only the capture-heavy ones as lambdas.

---

### M3. `service_dma()` called after every CPU memory write

**Location:** `cpu.cpp:1700, 1707, 1714` (ARM), `2360, 2367, 2374` (Thumb)

Every `write8/16/32` lambda calls `bus_.service_dma()`. Most writes don't trigger DMA.

**Fix:** Add a `dma_pending_` flag in Bus/DmaEngine. Set when DMA is enabled/triggered, clear when all channels complete. CPU write lambdas check the flag — single branch instead of function call + 4-channel scan.

---

### M4. Tiebreaker lambda in PPU compositing loop

**Location:** `src/core/ppu.cpp:486-490`

```cpp
auto tiebreaker = [](u8 id) -> u8 {
    if (id == 4) return 0;
    if (id <= 3) return id + 1;
    return 5;
};
```

Lambda with branches, called 2× per pixel (lines 498, 506). 240 pixels × 160 scanlines = ~76,800 calls/frame.

**Fix:** Replace with `constexpr u8 kTiebreaker[] = {1, 2, 3, 4, 0, 5};` — single array lookup, no branches.

---

### M5. Affine BG: `line * coeff` computed per-pixel

**Location:** `src/core/ppu.cpp:672-673`

```cpp
const s32 x = (ref_x + static_cast<s32>(screen_x) * pa + line * bg_pb_[affine_index]) >> 8;
const s32 y = (ref_y + static_cast<s32>(screen_x) * pc + line * bg_pd_[affine_index]) >> 8;
```

`line * bg_pb_[affine_index]` and `line * bg_pd_[affine_index]` are constant for the entire scanline but recomputed for every pixel. Also no tile cache, and `% vram.size()` on every access.

**Fix:** Hoist to `const s32 line_dx = line * bg_pb_[affine_index]` before the loop. Add a single-entry tile cache. Elide `% vram.size()` when coordinates are within bounds.

---

### M6. Per-pixel window x-checks in compositing

**Location:** `src/core/ppu.cpp:444-460`

Window boundaries are precomputed once per scanline (lines 420-423), but the per-pixel work is still 2-4 comparisons per pixel for membership tests across up to 2 windows + OBJ window. 240 pixels × 160 lines × 4 comparisons.

**Note:** Boundaries are NOT recomputed per pixel (earlier overstatement corrected). The current membership test (`x >= win_l && x < win_r`) is minimal. Further optimization possible by building a per-scanline span table identifying regions where window state is constant, then processing each span without per-pixel branches.

**Impact:** MEDIUM. Current approach is reasonable. Span-based approach would help for games with complex windowing (e.g., Metroid Fusion, Castlevania).

---

### M7. Full 128-entry OAM scan per scanline

**Location:** `src/core/ppu.cpp:797`

`for (int i = 127; i >= 0; --i)` always iterates all 128 OAM entries. Most games have 0-30 active sprites per scanline.

**Fix:** Build an active-object list on OAM write (or on scanline entry). Pre-filter to sprites overlapping the current Y range. 128 iterations → typically 5-15.

---

### M8. `refresh_schedule()` not IRAM_ATTR

**Location:** `src/core/emulator.cpp:122-129`

Called every scheduler tick in the main frame loop (`run_frame` line 70, `run_until` line 55, `step_scheduler_event` line 112). Not marked `IRAM_ATTR`.

**Fix:** Add `IRAM_ATTR` to `refresh_schedule()` and `service_due_hardware()`.

---

### M9. `service_timers()` not individually IRAM_ATTR

**Location:** `src/core/bus.cpp:580` (delegates to `timers.cpp`)

Called every instruction via the broken `_sc` counter (Bug B1). Even after fixing B1, it runs every N instructions. `Bus::service_timers()` is not individually marked `IRAM_ATTR` (only `Bus::read`/`write` are).

**Fix:** Add `IRAM_ATTR` to `Bus::service_timers()` and `Timers::advance_to()`.

---

### M10. APU `advance_to()` not IRAM_ATTR

**Location:** `src/core/apu.cpp:209`

Runs at 32768 Hz from flash cache. While less hot than CPU/PPU paths, it fires every scheduler tick during audio-active frames.

**Fix:** Add `IRAM_ATTR` to `Apu::advance_to()` and its helper functions.

---

### M11. Bus address decode if-else chain — hot paths pay 6-7 comparisons

**Location:** `src/core/bus.cpp:261-400` (read), `418-492` (write)

```cpp
if ((address & 0x0F000000u) == 0x02000000u) { ... }      // EWRAM
else if ((address & 0x0F000000u) == 0x03000000u) { ... }  // IWRAM
else if ((address & 0x0F000000u) == 0x04000000u) { ... }  // MMIO
else if ((address & 0x0F000000u) == 0x05000000u) { ... }  // Palette
else if ((address & 0x0F000000u) == 0x06000000u) { ... }  // VRAM
else if ((address & 0x0F000000u) == 0x07000000u) { ... }  // OAM
else if (address >= 0x08000000u && address < 0x0E000000u) { ... }  // ROM
```

ROM is the hottest bus path (every code fetch, every LDR from GamePak). A code fetch from ROM traverses 6-7 failed mask-and-compare branches before reaching the handler. Each branch check uses `(address & 0x0F000000u)` separately — six identical mask operations that the compiler cannot always CSE.

**Fix:** Replace the if-else cascade with a `switch (address >> 24)` jump table. The compiler emits a single table lookup + indirect jump. All regions map to a unique 8-bit tag (0x02=EWRAM, 0x03=IWRAM, ..., 0x08-0x0D=ROM). Adjacent regions (0x08-0x0D) merge to the same case label.

**Impact:** MEDIUM. Every ROM code fetch and data access skips 6 useless comparisons.

---

### M12. DMA per-unit transfer goes through full `bus.read()`/`bus.write()`

**Location:** `src/core/dma.cpp:273-336`

```cpp
for (u32 unit = 0; unit < units; ++unit) {
    value = bus.read(source, width, ...).value;   // full address decode
    // ...
    (void)bus.write(destination, value, width, ...);  // full address decode
}
```

Each DMA word transfer calls the full `Bus::read()` and `Bus::write()` functions — address decode cascade, span construction, waitstate computation. The ROM→VRAM fast path at lines 280-292 only bypasses the write side via `bus_.vram_write()`. Source reads still go through the complete bus.

For a 32-word DMA transfer (common GBA HDMA pattern), that's 64 full bus decode passes.

**Fix:** Add a `Bus::dma_transfer_word(u32& src_addr, u32& dst_addr, ...)` fast path that uses direct memory region pointers (precomputed span members from H1 fix) instead of the full `read()`/`write()` cascade. For ROM→VRAM DMA especially, both source (cartridge ROM span) and destination (VRAM span) are directly accessible without decode. Preservation requirements: still call `mark_video_dirty()` for VRAM writes, `prefetch_stop()` for ROM reads, and apply correct cycle counts from the waitstate table — but skip the address-decode cascade and span construction.

**Impact:** MEDIUM. DMA runs in bursts of 1-2048 words; the overhead scales with transfer size.

---

### M13. `refresh_schedule()` polls all 6 subsystems every scheduler tick

**Location:** `src/core/emulator.cpp:122-129`

```cpp
void Emulator::refresh_schedule() {
    scheduler_.set_current_cycle(cpu_.current_cycle());
    scheduler_.set_next_event(SchedulerSlot::Ppu, ppu_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Timers, timers_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Dma, dma_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Apu, apu_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Serial, bus_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Irq, irq_.next_event_cycle());
}
```

Called every iteration of `run_frame()` (line 70), `run_until()` (line 55), and `step_scheduler_event()` (line 112) — each frame potentially hundreds of times. All 6 subsystems are queried and registered even when nothing changed. `next_event_cycle()` for timers scans 12 values, PPU computes frame position, APU checks sample timing — all redundant when a subsystem's event time hasn't changed.

**Fix:** Add a `dirty_` flag per scheduler slot. When a subsystem completes its work and recomputes `next_event_cycle`, it sets the flag. `refresh_schedule()` only calls `next_event_cycle()` for dirty slots, then clears the flags. Alternatively, have each subsystem call `scheduler_.set_next_event(slot, cycle)` directly when their event time changes, eliminating `refresh_schedule()` polling entirely.

**Impact:** MEDIUM. Reduces per-tick overhead from 6 full queries to 1-2 on average frames.

---

### M14. `Scheduler::next_event()` linear scan — no cached minimum

**Location:** `src/core/scheduler.cpp:22-29`

```cpp
u64 Scheduler::next_event() const {
    u64 earliest = std::numeric_limits<u64>::max();
    for (const auto cycle : events_) {
        if (cycle < earliest) earliest = cycle;
    }
    return earliest;
}
```

O(n) scan over 6 slots on every call (1× per `refresh_schedule` via `scheduler_.next_event()` at `emulator.cpp:57` or `62` or `114`). `set_next_event()` is a simple array write (line 18-20) with no running-minimum update.

**Fix:** Maintain `min_event_cycle_` member updated on every `set_next_event()`. `next_event()` becomes `return min_event_cycle_;` — O(1). On set, `min_event_cycle_ = std::min(min_event_cycle_, cycle)`. Need to handle slot changes (when a previous min slot is updated to a higher value, re-scan once to recompute). With 6 slots the amortized cost is still far lower than scanning every call.

**Impact:** LOW. 6 comparisons are fast on modern hardware, but the scan runs at 16.78 MHz equivalent pace. The optimization is trivial to implement and removes the last linear scan from the hot scheduler path.

### M15. ROM provider `ensure_page()` linear scan per access

**Location:** `esp32_rom_provider.cpp:497-513` (SD cache), `215-230` (mmap)

Two for-loops scanning all pages on every ROM read. SD cache with 64 pages = 64-iteration scan per read.

**Fix:** Use a page-index-to-slot direct-mapped array (sized to max pages). O(1) lookup replaces O(N) scan. For profiling `unique_pages_`, use an `std::unordered_set` or fixed-size flat set.

---

### M16. `switch_mode()` bulk register copies — O(N) per mode switch

**Location:** `src/core/cpu.cpp:2919-2999`

```cpp
void Arm7tdmi::switch_mode(CpuMode new_mode) {
    // SAVE: copy 5-7 r8–r12/r14 + 2 r13,r14 = 7-9 u32 writes
    if (old_mode == CpuMode::Fiq) {
        for (i = 0; i < 7; ++i) state_.banked_fiq_r8_r14[i] = state_.regs[8 + i];
    } else {
        for (i = 0; i < 5; ++i) state_.banked_usr_r8_r12[i] = state_.regs[8 + i];
    }
    // ... copy r13/r14 for old mode (2 writes) ...

    // RESTORE: copy 5-7 r8–r12/r14 + 2 r13,r14 = 7-9 u32 reads
    // ... symmetrical restore loop ...
}
```

FIQ→other: 16 u32 copies. Non-FIQ↔Non-FIQ: 14 copies. Called on every exception entry (IRQ, SWI, undefined), MSR, LDM with S-bit, and data-processing with Rd=R15 — potentially dozens of times per frame.

**Fix:** Use a Register Pointer Table: `state_.reg_ptrs[i]` points to the active physical register for each of R0–R15. Banked registers are separate storage. Mode switch becomes: swap `reg_ptrs[13]`, `reg_ptrs[14]`, and swap pointers for R8–R12 (user) vs R8–R14 (fiq). O(1) with zero data copies.

**Impact:** MEDIUM. Saves 14-16 u32 copies per mode switch. Most impactful for games with frequent IRQ/BIOS calls.

---

### M17. PPU: backgrounds have tile cache, sprites do not

**Location:** `src/core/ppu.cpp:610-638` (BG cache), `ppu.cpp:797-919` (sprite render)

Background rendering (`render_text_bg`) uses two 2048-entry caches (`tile_cache_4bpp_` / `tile_cache_8bpp_` at `ppu.hpp:105-111`) with epoch-based invalidation. Cache key = `(tile_cache_epoch_ << 20) ^ base_data_addr`, indexed via modulo. Hit → 8 pre-decoded pixels returned immediately. Miss → decode 8 pixels from VRAM into cache.

Sprite rendering (`render_objects`) reads VRAM raw on every pixel with no caching:
```
// 8bpp sprite: line 892
const u8 color_idx = vram[data_addr % vram.size()];
// 4bpp sprite: line 904-906
const u8 byte_val = vram[data_addr % vram.size()];
color_idx = (pixel_x & 1) ? (byte_val >> 4) : (byte_val & 0x0F);
```

`render_objects` scans all 128 OAM entries then per-pixel re-decodes. With no sprite tile cache, a 32×32 pixel 4bpp sprite decodes 1024 nibbles from VRAM every scanline it overlaps.

**Fix:** Add a sprite tile cache mirroring the BG cache design. Because sprites are smaller and more numerous, use a smaller cache (256-512 entries) keyed by `(tile_num << 2) | (y_off & 7) | (palette << 10)`. Invalidate on OAM write or `mark_all_dirty()`.

**Impact:** MEDIUM. Games like Pokémon with many on-screen sprites decode the same base tiles repeatedly across scanlines.

---

### M18. ROM access: 3 layers with virtual dispatch on every read

**Location:** `include/gba/core/rom_provider.hpp:26-43`, `src/core/cartridge.cpp:218-266`

ROM reads traverse three layers:

```
Bus (bus.cpp:279-380) → Cartridge::read_rom() → RomProvider::read_byte/read16/read32() [virtual]
```

The `RomProvider` is an abstract base with virtual methods — the only virtual dispatch in any hot path. Three concrete providers exist: `MemoryRomProvider` (desktop), `Esp32MmapRomProvider` (ESP32 file-mapped), `Esp32SdCacheRomProvider` (ESP32 SD card cached).

Every ROM access pays the vtable lookup + indirect call cost (~2-3 cycles on Xtensa), on top of the 3-layer function call overhead. ROM is the single hottest bus path — code fetches happen every instruction.

The CLAUDE.md already lists this at line 115: *"RomProvider virtual dispatch on every ROM access (only virtual dispatch in hot path)"*.

**Fix:** Either: (a) replace virtual dispatch with `std::variant<MemoryRomProvider, Esp32MmapRomProvider, Esp32SdCacheRomProvider>` + `std::visit` for monomorphization; or (b) provide a non-virtual `contiguous_span()` fast path — on desktop the ROM is a flat `std::vector<u8>`, so `Cartridge` could cache a `span<const u8>` pointer used directly by Bus without calling through Cartridge/RomProvider. For prefetch-aware GamePak access, the Bus already has a direct `rom` span member.

**Impact:** MEDIUM-HIGH. Virtual dispatch on every instruction fetch adds measurable overhead. Desktop `MemoryRomProvider` is a trivial wrapper over `std::vector<u8>` — the indirection is pure waste.

---

## Platform-Specific (ESP32-S3)

### P1. RGB555→RGB565 scalar loop on 16K pixels

**Location:** `platform/esp32s3/main/gba_runtime.cpp:305-309`

Processes one `u16` at a time from PSRAM. 16,384 iterations per frame.

**Fix:** Process two pixels per iteration using `u32` load/store:

```cpp
const auto* src32 = reinterpret_cast<const u32*>(pixels);
auto* dst32 = reinterpret_cast<u32*>(pixels);
for (std::size_t i = 0; i < count / 2; ++i) {
    u32 p = src32[i];
    u32 lo = p & 0xFFFFu;
    u32 hi = p >> 16u;
    lo = ((lo & 0x7C00u) << 1) | ((lo & 0x03E0u) << 1) | (lo & 0x001Fu);
    hi = ((hi & 0x7C00u) << 1) | ((hi & 0x03E0u) << 1) | (hi & 0x001Fu);
    dst32[i] = lo | (hi << 16u);
}
```

---

### P2. Display: 24 SPI transactions per frame (8 chunks × 3)

**Location:** `gba_runtime.cpp:41, 568-576`

16-row chunking forces 8 `CASET` + 8 `RASET` + 8 pixel writes per frame. `max_transfer_sz` supports full 32 KB frame.

**Fix:** Send entire frame in one `display_draw(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, pixels)` call. Reduces 24 transactions to 3.

---

### P3. No `IRAM_ATTR` on platform hot-path functions

**Location:** All `platform/esp32s3/main/*.cpp`

`convert_rgb555_to_rgb565_inplace`, ROM provider `ensure_page`, display byte-swap loop — all in flash cache.

**Fix:** Add `IRAM_ATTR` to `convert_rgb555_to_rgb565_inplace`, `Esp32SdCacheRomProvider::ensure_page`, and the `tx_pixels` byte-swap path.

---

### P4. Audio chunk exceeds I2S DMA buffer

**Location:** `gba_audio.cpp:80-98`, `gba_runtime.cpp:349-358`

I2S DMA buffer is 3 KB (3 × 256 × 4 bytes). Audio chunks are ≥2 KB (variable, up to 8 KB). `i2s_channel_write` with timeout=0 drops samples that don't fit.

**Note:** Chunk size is variable (≥2 KB, not always 8 KB), but still exceeds the 3 KB DMA pool in many cases.

**Fix:** Either split the chunk into sub-writes matching DMA buffer size, or reduce the APU mix buffer threshold to produce smaller chunks (~512 s16 = 1 KB).

---

### P5. PSRAM framebuffers lack cache-line alignment

**Location:** `gba_runtime.cpp:527-528`

`heap_caps_malloc(kOutputPixels * sizeof(u16), MALLOC_CAP_SPIRAM)` — no alignment guarantee. ESP32-S3 cache line is 64 bytes.

**Fix:** Use `heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM)`. Zero runtime cost, guarantees cache-line alignment.

---

### P6. CPU task stack oversized

**Location:** `gba_runtime.cpp:560`

`xTaskCreatePinnedToCore(cpu_task, "gba_cpu", 32768, ...)` — 32 KB of precious internal DRAM for a task that uses a few local variables. Emulator is heap-allocated.

**Fix:** Reduce to 8192 (8 KB). Monitor with `uxTaskGetStackHighWaterMark()` to confirm.

---

### P7. False sharing on `front`/`back` atomics between cores

**Location:** `gba_runtime.cpp:80-81`

```cpp
std::atomic<int> front{0};   /* Core 0 reads */
std::atomic<int> back{1};    /* Core 1 writes */
```

Adjacent 4-byte `std::atomic<int>` fields — guaranteed same 64-byte cache line. Core 1 writes `back` (line 363: `back.store(buf ^ 1)`). Core 0 reads `front` (line 567: `front.load()`). Each write to `back` invalidates Core 0's cache line; each read of `front` must re-fetch from PSRAM/L2. On ESP32-S3, this is a cache-coherency protocol ping-pong on every frame.

The data is never shared — `front` is Core 0's domain, `back` is Core 1's. But the hardware doesn't know that because they're on the same line.

**Fix:** Pad to separate cache lines (64 bytes on ESP32-S3):

```cpp
alignas(64) std::atomic<int> front{0};
alignas(64) std::atomic<int> back{1};
```

Or interleave with unrelated fields to push them apart. Cost: ~120 bytes of padding. Gain: no cross-core cache invalidations on every frame.

**Impact:** LOW-MEDIUM. One cache-line invalidation per frame is negligible (~40ns), but on ESP32 with PSRAM latency (~30ns/access), removing false sharing guarantees clean cache semantics. More defensive than performance-critical.

---

### P8. ESP32 sdkconfig uses debug optimization (`-Og`)

**Location:** `platform/esp32s3/sdkconfig:551, 2041`

```config
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
```

This compiles all emulator code at `-Og` (optimize for debug) — no inlining of non-marked functions, limited register allocation, no loop optimizations. For comparison, the desktop builds default to `-O2`/`-O3` in Release. The ESP32 target is permanently in debug-optimization mode.

Note: bootloader IS optimized for size (`CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_SIZE=y` at line 406), which is correct. Only the app is debug.

**Fix:** Change to `CONFIG_COMPILER_OPTIMIZATION_PERF=y` (`-O2`) or `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`). On ESP32-S3 at 240 MHz with PSRAM, `-Os` is typically the right choice — reduces IRAM pressure while keeping hot-path performance. Re-run benchmarks after switch to verify frame timing.

**Impact:** HIGH. Switching from `-Og` to `-Os`/`-O2` typically yields a 30-50% throughput improvement on Xtensa for interpreter-heavy code. The emulator is an interpreter — it benefits heavily from inlining, loop unrolling, and register allocation.

---

### P9. ESP32 component does not define `GBA_ENABLE_HLE_BIOS`

**Location:** `CMakeLists.txt:18-21` (desktop), `platform/esp32s3/` (absent)

Desktop builds define `GBA_ENABLE_HLE_BIOS` via `add_compile_definitions(GBA_ENABLE_HLE_BIOS)` in the root `CMakeLists.txt`. The ESP32 component (`platform/esp32s3/components/gba_core/`) does NOT inherit this definition — the `grep` for `GBA_ENABLE_HLE_BIOS` across the entire `platform/esp32s3/` tree returns zero results.

This means on ESP32:
- SWIs 0x00–0x0D work (those are always present regardless of the define)
- SWIs 0x0E–0x1F run as cycle-only **stubs** — LZ77, RL, Huff decompression, affine math, SoundBias are NOT executed
- Games that call LZ77UnComp on ESP32 get no decompressed data and will crash or misbehave
- The full HLE implementation (~2 KB of code) is compiled out

**Fix:** Add `GBA_ENABLE_HLE_BIOS` to the ESP32 component's compile definitions. In the component CMakeLists (`platform/esp32s3/components/gba_core/CMakeLists.txt`): `target_compile_definitions(gba_core PRIVATE GBA_ENABLE_HLE_BIOS)`. Or gate behind a Kconfig option if flash is tight.

**Impact:** HIGH. Without HLE decompression SWIs, games using LZ77/RL/Huff (nearly all commercial GBA games) cannot run on ESP32 without a real BIOS. The fix is a one-line CMake change.

---

## Minor / Low-Impact

| # | Location | Issue | Fix |
|---|----------|-------|-----|
| L1 | `bus.cpp:287,460,936` | Sequential access detection logic duplicated 3× (read ROM, write ROM, region_cycles) | Extract to shared inline helper; also affects prefetch penalty machinery (see H4) |
| L2 | `ppu.cpp:58-64` | `read16()` does double-modulo (`% bytes.size()`) on every palette/VRAM access. Palette (1024), OAM (1024), EWRAM (256KB), IWRAM (32KB) are all powers of two — `& (size-1)` mask replaces expensive `%` on Xtensa (no hardware divider) | Use `& (size-1)` for power-of-2 regions; keep `%` only for VRAM (0x18000 = non-power-of-2). Also applies to `read_array`/`write_array` in bus.cpp |
| L3 | `ppu.cpp:862-867` | Sprite affine: 4 multiplies per pixel | Use incremental iteration: `tex_x += pa; tex_y += pc;` |
| L4 | `cpu.cpp:1263-1265` | `pc_visible()` re-reads CPSR thumb bit ~8× per instruction | Cache at top of `execute_arm`/`execute_thumb` |
| L5 | `cpu.cpp:2160-2198,2261` | Double `update_nz` for opcodes 2-7 with S-bit | Skip second call for opcodes that already set NZ in the switch |
| L6 | `cpu.cpp:1651-1655` | `bus_.waitcnt()` read on every internal-cycle instruction | Cache in local variable at function entry |
| L7 | `bus.cpp:272,436,805` | VRAM mirror logic duplicated 3 places | Extract to shared inline helper, use cmov instead of branch |
| L8 | `ppu.cpp:774-779` | `clear_dirty()` O(160) scan on every call | Track dirty count, O(1) check |
| L9 | `ppu.cpp:564+` | Mosaic effects not implemented (correctness) | Quantize fine_y/fine_x when `cnt.mosaic()` is active |
| L10 | `cpu.cpp:2919-2999` | `switch_mode()` does bulk register copies (14-16 u32 per call) | Use Register Pointer Table — swap 3-5 pointers instead of copying 14-16 u32 |
| L11 | `cpu.cpp:1724+/2113` | ARM dispatch: flat 8-way switch on bits 27:25, ONE nested 16-way switch for data processing | Replace with 4096-entry computed-goto table or jump-table flattening (low priority — dispatch is already shallow) |
| L12 | `cpu.cpp:2160-2261` | Double `update_nz` for ADD/SUB/ADC/SBC/RSC with S-bit (opcodes 2-7) | Skip second `update_nz` for opcodes already setting NZ inside the switch (see L5 — same issue different scope) |
| L13 | `cpu.cpp:1100-1108` | Thumb ALU flags computed unconditionally (no `set_flags` gate unlike ARM) | Gate Thumb ALU flags behind `test_bit(state_.cpsr, 31) || test_bit(state_.cpsr, 30)` — but only if lazy flags prove worth it |
| L14 | `bus.cpp:294-316, 468-478, 483-489` | Prefetch penalty logic duplicated 4× (read lambdas + write ROM + write SRAM) | Extract `prefetch_stop_penalty()` helper; see H4 for full analysis |
| L15 | `gba_runtime.cpp:80-81` | `front`/`back` atomics on same cache line — false sharing between cores | Pad with `alignas(64)` or insert padding fields; see P7 |

## Fix Priority

**Do first (bugs + immediate throughput):**
1. B1 — `_sc` counter (1-line fix)
2. B2 — Double `raise_exception` (2-line deletion)
3. B3 — Selective dirty marking + split screen/tile-cache dirty flags
4. B4 — Remove duplicate `refresh_schedule()` call
5. H1 — Cache bus spans
6. H3 — Build with Release mode
7. P8 — Switch ESP32 sdkconfig to `-Os` optimization
8. P9 — Define `GBA_ENABLE_HLE_BIOS` on ESP32 component

**Do next (hot-path optimization):**
9. M18 — Eliminate virtual dispatch on ROM (direct span fast-path)
10. M5 — Hoist affine coefficients
11. M4 — Tiebreaker LUT
12. H4 — Consolidate prefetch penalty logic
13. M7 — Active sprite list
14. M17 — Add sprite tile cache
15. P1 — 32-bit-wide pixel conversion
16. P2 — Full-frame DMA

**Do when profiling shows it matters:**
17. M3 — DMA pending flag
18. M11 — Bus switch-on-byte decode
19. M12 — DMA fast-path transfer
20. M13 — Scheduler dirty-flag polling
21. M14 — Scheduler cached min
22. M16 — Register pointer table (lazy banking)
23. P7 — Pad atomics to separate cache lines
24. P3 — Platform IRAM_ATTR
25. M8-M10 — Core IRAM_ATTR
26. Everything in Low-Impact table
