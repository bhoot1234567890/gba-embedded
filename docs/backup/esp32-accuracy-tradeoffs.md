# ESP32 Accuracy vs Performance Tradeoffs

GBA emulator running on ESP32-S3 (Xtensa LX7, dual-core 240MHz, 512KB SRAM, 8MB PSRAM).
GBA runs at 16.78 MHz with 280896 cycles/frame at 59.7 fps.

## Easy Wins (minimal game impact)

### 1. APU — skip or decimate
- Audio is the single biggest cycle sink. Most ESP32 GBA projects run with sound off.
- If keeping audio: downsample 32kHz → 16kHz or lower, skip interpolation (linear/bicubic → nearest), reduce FIFO buffer size.
- APU uses `std::deque` — replace with static ring buffer to save heap overhead.

### 2. PPU — render less
- Skip every other scanline on 128x128 ST7735 (already downscaling).
- Only render visible scanlines (0-159 for mode 3/4), skip vblank rendering entirely.
- Skip window effects if game doesn't use them.
- Delayed-render → scanline-render tradeoff: push pixels directly during scanline instead of building full line buffer.

### 3. GamePak prefetch buffer — simplify
- 8-halfword duty-cycle prefetch model is the #1 source of timing suite failures and costs CPU.
- Replace with simple 1-cycle-penalty model: first non-sequential access pays full waitstates, sequential gets 1 cycle discount.
- Most games won't care. Only timing-sensitive homebrew/tests notice.

### 4. HLE BIOS — keep, don't emulate real BIOS
- Already implemented (auto-detect infinite loop at SWI vector).
- Skip SIO/timing suites that need real BIOS timing.

## Medium Cuts (some games break)

### 5. SIO — stub entirely
- Suites 11 & 12 are all SIO (serial link). No single-player game uses it.
- Stub reads to return default values, ignore writes.
- Saves emulating full SIO state machine.

### 6. Timer cascade precision — relax
- Suite 04 (timer count-up) is biggest failure area.
- On ESP32, checkpoint timers only at scanline boundaries (every 1232 cycles) instead of per-instruction.
- Loses cascade accuracy but saves constant timer checks.

### 7. DMA — drop ROM-source word transfer accuracy
- Suite 10 failures are ROM → RAM word DMA with prefetch interaction.
- Simplify: always use sequential timing for DMA, ignore prefetch interaction with DMA.
- Most "fast enough" emulators do this.

## Aggressive Cuts (for borderline performance)

### 8. Frame skip
- Render every other frame. GBA is 59.7fps — skipping to 30fps is very playable.
- Dual-core setup: Core 1 can skip frames while Core 0 still pushes display.

### 9. PSRAM cache-aware access patterns
- ESP32-S3 PSRAM shares cache with flash. EWRAM (256KB), VRAM (96KB), palette, OAM all in PSRAM.
- Accessing >32KB in a tight loop evicts flash cache → code stalls.
- Batch VRAM/EWRAM accesses, don't interleave.

### 10. CPU interpreter — computed goto
- Switch-based dispatch works but computed goto (label-as-value) eliminates branch prediction misses.
- ~15-20% speedup on Xtensa. Worth it for hot functions already in IRAM.

## What NOT to Cut

- **BIOS protection latch** — some games rely on it
- **VRAM byte replication** — many tile-based games break without it
- **OAM byte-write ignore** — hardware behavior, games depend on it
- **MMIO register correctness** — Suite 02 is 130/130, don't regress it

## Current Test Suite Baseline (2026-04-23)

| Suite | Score | Notes |
|-------|-------|-------|
| 01 Memory | 1552/1552 | 100% — DMA store was flaky, now passing |
| 02 I/O read | 130/130 | 100% |
| 03 Timing | 1721/1722 | ~99.9% — nearly perfect |
| 04 Timer count-up | 480/936 | ~51% — timer cascade edge cases |
| 05 Timer IRQ | 69/90 | ~77% — nop-count sensitivity |
| 06 Shifter | 140/140 | 100% |
| 07 Carry | 93/93 | 100% |
| 08 Multiply long | 72/72 | 100% |
| 09 BIOS math | 615/615 | 100% (HLE) |
| 10 DMA | 1220/1256 | ~97% — ROM-source word transfers |
| 11 SIO register R/W | 53/90 | SIO stub candidates |
| 12 SIO timing | 0/4 | Can skip entirely |
| 13 Misc edge case | 0/10 | DMA prefetch break + HBlank timing |
| 14 Video | unknown | Needs frame comparison |

### ESP32 Impact per Cut (estimated)

| Cut | Cycles saved/frame | % of budget | Game risk |
|-----|-------------------|-------------|-----------|
| Disable APU | ~40-60K | 15-20% | None (no sound) |
| Frame skip 1:1 | 280896 | 50% | Visual only |
| Simple prefetch | ~10-20K | 5-7% | Timing tests only |
| Stub SIO | ~2-5K | 1-2% | No multiplayer |
| Scanline timer checkpoint | ~5-10K | 2-4% | Rare games |
| Computed goto CPU | N/A (faster dispatch) | 15-20% | None |

ESP32-S3 budget: 240MHz / 59.7fps = ~4M cycles/frame available. GBA needs 280896 emulated cycles. Headroom is ~14x, but each emulated cycle costs 5-15 host cycles depending on path.

## Sources

- mGBA: Emulation Accuracy, Speed, and Optimization — https://mgba.io/2017/04/30/emulation-accuracy/
- mGBA: Cycle Counting, Prefetch — https://mgba.io/2015/06/27/cycle-counting-prefetch/
- ESP-IDF: PSRAM External RAM — https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html
- ESP-IDF: Performance Optimization — https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/performance/index.html
- Retro-Go GBA on ESP32-S3 — https://github.com/ducalex/retro-go/issues/96
