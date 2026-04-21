# ESP32-S3 GBA Emulator: Optimization Plan

Target: ESP32-S3 (dual-core LX7 @ 240MHz, 512KB SRAM, octal PSRAM) with 128x128 SPI display.

This plan is ordered by impact and implementation order. Each phase is self-contained and testable.

---

## Phase 0: Memory Architecture (Foundation)

Everything else depends on this. Without PSRAM, nothing fits.

### 0.1 Enable octal PSRAM on real hardware

```ini
# sdkconfig.defaults additions for real hardware
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
```

### 0.2 Partition memory between SRAM and PSRAM

| Data | Size | Location | Why |
|---|---|---|---|
| CPU registers (CpuState) | ~200B | SRAM | Hot path, every instruction |
| Scheduler | ~64B | SRAM | Checked every step |
| IRQ controller | ~32B | SRAM | Checked every step |
| Timers | ~128B | SRAM | Frequent access |
| DMA engine | ~128B | SRAM | Frequent access |
| Bus logic (waitcnt, etc) | ~64B | SRAM | Every bus access |
| APU control registers | ~128B | SRAM | Register writes |
| EWRAM (guest) | 256KB | PSRAM | Bulk, rarely time-critical |
| VRAM (guest) | 96KB | PSRAM | Bulk, accessed PPU-side |
| Palette + OAM | 2KB | PSRAM | Small, PPU-side |
| Framebuffer | 32KB | PSRAM | 128x128 RGB565, not hot |
| APU FIFOs + mix buffer | ~8KB | PSRAM | Audio buffer, not hot |

**Implementation**: Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` for the large arrays. Refactor Bus to use dynamically-allocated spans instead of inline `std::array`.

### 0.3 Allocate Bus arrays on heap

Current code: `std::array<u8, kEwramSize> ewram_{};` inside Bus (stack-embedded).
Target: `std::unique_ptr<u8[]> ewram_;` allocated from PSRAM.

```cpp
// bus.hpp — change from:
std::array<u8, kEwramSize> ewram_{};
// To:
std::unique_ptr<u8[]> ewram_;
// bus.cpp constructor:
ewram_ = std::unique_ptr<u8[]>(new(heap_caps_malloc(kEwramSize, MALLOC_CAP_SPIRAM)) u8[kEwramSize]());
```

Same for `vram_`, `palette_`, `oam_`, `framebuffer_`.

---

## Phase 1: PPU — Render to 128x128 Directly

Current: renders 240x160 → 77KB framebuffer → downscale to 128x128 for SPI.
Target: render only what the 128x128 display needs.

### 1.1 Crop-and-decimate scanline renderer

GBA output: 240x160. Display: 128x128. Scale factor: 128/160 vertical, 128/240 horizontal.

Strategy: render every other scanline vertically (80 of 160), decimate horizontally (pick every other pixel, 120 of 240). Crop to center 128x128.

```
GBA scanlines: 0,2,4,6...158 → 80 lines
GBA pixels per line: pick 128 of 240 (center crop, offset 56)
```

### 1.2 Skip rendering during vblank

Lines 160-227 are vblank. No rendering needed. Free 68 scanlines of work.

### 1.3 Dirty scanline tracking

Only re-render scanlines where VRAM/palette/OAM/bgcnt actually changed.

```cpp
std::array<bool, 160> scanline_dirty_{};
// Set dirty on any VRAM write, palette write, OAM write, or BG register write
// Clear dirty after rendering
```

### 1.4 Skip invisible backgrounds

If a BG's priority bits make it fully covered by higher-priority layers, skip it. Simple check:
if the topmost enabled BG is opaque everywhere, skip lower BGs.

### 1.5 Stream to SPI instead of double-buffering

Instead of building a full frame then sending, push each scanline to SPI DMA as it completes.
Eliminates the 32KB framebuffer entirely — just a single scanline buffer (128 * 2 = 256 bytes).

---

## Phase 2: CPU Interpreter Optimization

The CPU is the #1 CPU-time consumer. GBA runs at 16.78MHz, ESP32 at 240MHz — 14.3x headroom.
But the interpreter has overhead per instruction. Every cycle counts.

### 2.1 Batch execution in cpu_run_until

Current: `while (current_cycle_ < target_cycle) { step(); }` — virtual call overhead per instruction.

Optimization: inline the fetch-decode-execute loop. Avoid function call per step.

```cpp
u64 Arm7tdmi::cpu_run_until(u64 target_cycle) {
    while (current_cycle_ < target_cycle) {
        // Inline halt/IRQ check only every N instructions or when cycle threshold nears
        if (thumb_state()) {
            const auto instruction = fetch_thumb();
            execute_thumb(instruction);
        } else {
            const auto instruction = fetch_arm();
            execute_arm(instruction);
        }
    }
    return current_cycle_;
}
```

Already mostly there. The `step()` call overhead is the issue — inline it.

### 2.2 Reduce lambda overhead in execute_arm/execute_thumb

Current: every instruction creates 6-8 lambdas (read8, read16, read32, write8, write16, write32, read_reg, write_pc).
These lambdas capture `this` and access bus_ through virtual-like patterns.

Optimization: hoist lambdas out of execute_arm/execute_thumb. Make them class members or use direct member access.

```cpp
// Instead of lambdas, use direct calls in the instruction handlers
// The compiler can inline these better than capturing lambdas
```

### 2.3 Pre-decode instruction categories

ARM has 4096 possible instruction patterns (bits [27:16]). Many games use <100 unique patterns.

Create a lookup table indexed by instruction bits, categorizing into:
- ALU immediate
- ALU register
- Branch
- LDR/STR
- LDM/STM
- MUL
- SWP
- MSR/MRS
- etc.

Replace the cascade of `if ((instruction & mask) == value)` checks with a single table lookup.

### 2.4 Condition check optimization

Current: `condition_passed()` called for every ARM instruction. 14 of 15 conditions are simple flag checks.

Fast-path: check condition in the dispatch table. Only call the full function for complex conditions (GE, LT, GT, LE).

### 2.5 Place hot CPU code in IRAM

ESP-IDF allows placing functions in IRAM (instruction RAM) for zero-flash-wait execution.

```cmake
# Place CPU interpreter in IRAM
set_source_files_properties(src/core/cpu.cpp PROPERTIES COMPILE_FLAGS "-fno-rtti")
idf_component_register(... SRCS src/core/cpu.cpp)
# Add IRAM attribute in code:
__attribute__((section(".iram1"))) u32 Arm7tdmi::execute_arm(...)
```

This ensures the interpreter loop never stalls on flash cache misses.

### 2.6 Avoid std::deque in APU FIFO

`std::deque` does heap allocation per push. Replace with a fixed-size circular buffer.

```cpp
template<typename T, size_t N>
class Fifo {
    std::array<T, N> buf_{};
    size_t head_ = 0, tail_ = 0, count_ = 0;
public:
    void push(T val) { buf_[tail_] = val; tail_ = (tail_+1) % N; ++count_; }
    T pop() { T v = buf_[head_]; head_ = (head_+1) % N; --count_; return v; }
    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
};
```

---

## Phase 3: Dual-Core Pipelining

ESP32-S3 has two cores. Use Core 1 for the CPU interpreter, Core 0 for PPU + SPI output.

### 3.1 Core assignment

- **Core 0 (PRO)**: Arduino/ESP-IDF default, WiFi/BT stack. Run PPU rendering + SPI DMA.
- **Core 1 (APP)**: Dedicated to GBA CPU + Bus + Timers + DMA + IRQ.

```cpp
void cpu_task(void* arg) {
    auto* emu = static_cast<Emulator*>(arg);
    while (true) {
        emu->run_frame();
        xTaskNotifyGive(display_task_handle); // signal frame done
    }
}

void app_main() {
    xTaskCreatePinnedToCore(cpu_task, "gba_cpu", 32768, &emu, 5, nullptr, 1);
    // Core 0 handles display SPI DMA
}
```

### 3.2 Double-buffer scanlines between cores

Core 1 renders scanlines into buffer A. Core 0 sends buffer B to SPI.
Swap buffers at vblank. Use a simple spinlock or atomic flag — no FreeRTOS queue overhead.

```cpp
alignas(32) u16 scanline_buf[2][128]; // ping-pong
std::atomic<int> ready_line{-1};
```

---

## Phase 4: SPI Display Optimization

### 4.1 Use ESP-IDF SPI DMA with LCD driver

Don't bit-bang SPI. Use the ESP32-S3 LCD peripheral or SPI DMA.

```cpp
// Use esp_lcd_panel_io_spi for DMA-backed SPI transfers
esp_lcd_panel_io_spi_config_t io_config = {
    .dc_gpio_num = DC_PIN,
    .spi_mode = 0,
    .pclk_hz = 40 * 1000 * 1000, // 40MHz SPI clock
    .trans_queue_depth = 10,
    // ...
};
```

### 4.2 Push partial updates

If scanlines haven't changed (dirty tracking), skip the SPI transfer for those lines.
Most GBA games don't redraw the entire screen every frame.

### 4.3 Use RGB565 natively

The SPI display likely accepts RGB565. Don't convert from ARGB8888 — the PPU already outputs RGB565.

---

## Phase 5: Audio Optimization

### 5.1 Skip PSG channels initially

PSG channels (square, sweep, noise, wave) are complex and many games barely use them.
Start with Direct Sound A/B only (FIFO-based). Already implemented.

### 5.2 Lower sample rate for SPI DAC

GBA produces ~65536 Hz samples. For a small speaker on ESP32, 22050 Hz or even 11025 Hz is fine.
Decimate by 3x or 6x in the mix buffer.

### 5.3 I2S DMA output

Use ESP32-S3 I2S peripheral with DMA. CPU just fills a ring buffer, DMA handles output.

---

## Phase 6: Advanced Techniques (If Still Too Slow)

### 6.1 Frame skipping

If a frame takes too long, skip rendering entirely and just advance CPU state.
Show the previous frame again. Configurable: skip 0/1/2 frames.

```cpp
if (frame_time_us > 16742) { // more than 16.7ms (60fps budget)
    skip_next_render = true;
}
```

### 6.2 Tile caching in PPU

Cache decoded 8x8 tile patterns. Most games reuse tiles across frames.
Cache key: (tile_index, palette_bank, hflip, vflip). ~1KB for a small LRU cache.

### 6.3 Self-modifying code detection (from gpSP)

Track writes to IWRAM/EWRAM that overlap with code regions. Only flush affected cached state.
Most games don't self-modify. Those that do (Golden Sun, Doom) modify small regions.

### 6.4 ARM7TDMI instruction predecode

Build a predecoded instruction cache. For each ARM instruction word, store:
- decoded operation type
- precomputed register indices
- precomputed immediate values
- precomputed shift parameters

Look up in a hash table keyed on PC. Invalidate on writes to code regions.

---

## Implementation Order and Milestones

| Milestone | What | Test |
|---|---|---|
| **M0** | PSRAM allocation, Bus on heap | All 6 QEMU tests pass with full Bus |
| **M1** | 128x128 PPU, dirty tracking | Render test pattern to SPI |
| **M2** | CPU inline step, reduce lambdas | Benchmark: cycles per GBA instruction |
| **M3** | Dual-core split | Core 1 runs CPU, Core 0 runs display |
| **M4** | SPI DMA, RGB565 direct | Smooth 60fps on actual display |
| **M5** | Audio I2S, PSG skip | Sound output works |
| **M6** | Frame skip, tile cache | Run demanding games acceptably |

---

## Budget Summary

| Resource | Available | Used | Free |
|---|---|---|---|
| SRAM | 512KB | ~50KB (CPU, scheduler, IRQ, timers, DMA, bus logic, stacks) | ~460KB |
| PSRAM | 8MB (typical) | ~400KB (EWRAM 256, VRAM 96, palette 2, OAM 1, audio 8, misc) | ~7.6MB |
| Flash | 8-16MB | App ~1MB + ROM storage | ~7-15MB |
| CPU Core 0 | 240MHz | PPU render + SPI DMA (~10-20MHz equivalent) | ~220MHz |
| CPU Core 1 | 240MHz | GBA CPU + Bus + DMA (~100-150MHz for fullspeed) | ~90-140MHz |

The ESP32-S3 with PSRAM has more than enough resources. The bottleneck is optimization, not capacity.

---

## Key References

- **gpSP by Exophase**: Reference GBA emulator for ARM. Static/dynamic buffer separation for SMC. Selective recompiler flushing. IWRAM code placement. [gpsp-dev.blogspot.com](http://gpsp-dev.blogspot.com/2007/05/iwram-code-vs-recompiler.html)
- **mGBA scanline renderer**: Per-scanline software rendering with background layers, sprites, windows, blending. [mgba-emu/mgba](https://github.com/mgba-emu/mgba)
- **ESP-IDF PSRAM + LCD**: `fb_in_psram` flag for PSRAM framebuffers, XIP from PSRAM, LCD RGB panel DMA. [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-guides/external-ram.html)
- **Computed goto dispatch**: GCC extension for faster interpreter dispatch vs switch/case. 20%+ speedup in tight loops. [StackOverflow](https://stackoverflow.com/questions/58774170)
