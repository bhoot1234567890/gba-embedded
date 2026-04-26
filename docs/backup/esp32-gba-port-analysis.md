# GBA Emulator → ESP32-S3: Complete Port Analysis

Audit date: 2026-04-24
Status: Cross-referenced against ESP-IDF v5.x docs, real-world ESP32 GBA projects (retro-go, 44vba), gpSP optimization docs, ESPHome deque/fragmentation issues, and ESP-IDF C++ support docs.
Verification: 17/17 claims independently confirmed against source code and config files.

---

## Codebase Summary

Cycle-scheduled GBA emulator in C++20. Core is platform-neutral; ESP32-S3 is the primary target (RT1170 is a legacy stub).

### What Already Works for ESP32

- `GBA_PLATFORM_ESP32` guards in bus.cpp, cpu.cpp, ppu.cpp, emulator.cpp, cartridge.cpp, log.cpp
- PSRAM allocation for EWRAM (256KB), VRAM (96KB), framebuffer (75KB)
- Internal SRAM for IWRAM (32KB), palette (1KB), OAM (1KB) — already correct in bus.cpp:137-143
- IRAM_ATTR on 5 hot functions: `cpu_run_until`, `execute_arm`, `execute_thumb`, `downscale_565`, `downscale_565_v2`
- Dual-core pipeline (Core 1 = emulator, Core 0 = SPI display)
- ST7735 128x128 SPI DMA display driver with green-tab offset
- 240x160 → 128x128 RGB565 downscaler (~0.8ms on ESP32-S3)
- ESP-IDF CMake integration at `platform/esp32s3/`
- `std::filesystem` correctly guarded with `#ifndef GBA_PLATFORM_ESP32`
- HLE BIOS SWI auto-detection (infinite-loop vector → C++ handler)
- RTTI already off in generated sdkconfig despite defaults requesting it

---

## CATEGORY 1: BLOCKERS — Nothing Runs Without These

### 1. ROM Loading — Completely Missing

**Location:** `platform/esp32s3/main/gba_runtime.cpp:89`

The runtime creates an `Emulator`, calls `reset()`, and runs frames — but **never loads a ROM or BIOS**. The emulator will execute open bus / undefined memory.

**Options (per ESP-IDF docs):**
- SD card (ESP-IDF FAT filesystem + SPI SD card driver) — best for multi-ROM
- Embedded in firmware via `EMBED_TXTFILES` in CMakeLists.txt — simplest for a single test ROM
- SPIFFS/LittleFS partition — for a few ROMs without SD card hardware
- WiFi HTTP — advanced, requires WiFi stack (uses IRAM, competes with emulator)

**ESP-IDF confirms `std::filesystem` is not supported.** The codebase already handles this correctly with `#ifndef GBA_PLATFORM_ESP32` guards. No change needed there.

### 2. Input Handling — Completely Missing

**Location:** `platform/esp32s3/main/gba_runtime.cpp`

`set_keys()` is never called. Need GPIO-debounced button reading for D-pad + A/B + Start/Select + L/R (10 buttons total), wired into the frame loop before `run_frame()`.

### 3. Watchdog Timer — Not Handled

**Location:** `platform/esp32s3/sdkconfig:1308` — TWDT enabled at 5s default.

No `esp_task_wdt_reset()` anywhere in ESP32 code. Single frames (15-50ms) won't trigger it, but an emulator stall (infinite loop in HLE SWI, unimplemented instruction) will reboot the chip.

**Mitigations:**
- Feed `esp_task_wdt_reset()` periodically in the CPU task loop
- Or increase TWDT timeout via `CONFIG_ESP_TASK_WDT_TIMEOUT_S`

---

## CATEGORY 2: PERFORMANCE-CRITICAL CHANGES — Must Do for Playable Speed

### 4. APU `std::deque` → Static Ring Buffer

**Files:** `include/gba/core/apu.hpp:48-49`, `src/core/apu.cpp:264`

| Current | Problem |
|---------|---------|
| `std::deque<s8> fifo_[2]` | Allocates 512-byte chunks per push on ESP32 |
| `std::vector<s16> mix_buffer_` | Grows dynamically, heap fragmentation |
| `std::vector<s16> consume_audio_chunk()` | Returns new vector each call |

**Cross-reference:** ESPHome issue #11279 documents this exact pattern — `std::deque` on ESP32 causes excessive memory usage and fragmentation. ESPHome PR #14733 replaced it with `FixedRingBuffer`. ESPHome PR #11305 replaced another deque with vector to avoid 512-byte upfront allocation.

On ESP32 with PSRAM, each `push_back` to a deque may trigger a PSRAM allocation (~100ns vs ~10ns for SRAM). The APU FIFO is pushed per timer overflow — this is a hot path.

**Fix:** Replace with fixed-size pre-allocated ring buffer:

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

Replace `consume_audio_chunk()` with a span-based API that writes into a pre-allocated buffer.

### 5. `<cmath>` → Integer-Only Math for HLE BIOS

**Files:** `src/core/cpu.cpp:754-770`

Only 3 actual uses in the entire codebase, all in HLE SWI handlers:

| Line | SWI | Current | Replacement |
|------|-----|---------|-------------|
| 754 | 0x08 BIOS Sqrt | `std::sqrt(double)` | Integer Newton-Raphson (~10 instructions) |
| 761 | 0x09 ArcTan | `std::atan2(double)` | CORDIC or lookup table |
| 769 | 0x0A ArcTan2 | `std::atan2(double)` | CORDIC or lookup table |

**Why this matters:** ESP32-S3 has a **single-precision FPU only**. All three calls use `double` arguments, which triggers soft-float emulation on Xtensa. This bloats the binary with soft-float library code and is slow.

The real GBA ARM7TDMI BIOS uses integer-only polynomial approximations. The HLE should too. After replacement, `<cmath>` include can be removed from cpu.cpp entirely.

### 6. PSRAM Cache Thrashing

**Source:** ESP-IDF external RAM documentation states:

> "External RAM uses the same cache region as the external flash. When accessing large chunks of data (> 32KB), the cache can be insufficient, and speeds will fall back to the access speed of the external RAM. Moreover, accessing large chunks of data can push out cached flash, possibly making the execution of code slower afterwards."

The emulator touches **>400KB of PSRAM per frame:**
- EWRAM: 256KB (CPU reads during bus access)
- VRAM: 96KB (PPU reads during scanline render)
- Framebuffer: 75KB (read during downscale)
- Display buffers: 64KB (read during SPI DMA)

Walking 256KB of EWRAM + 96KB of VRAM in a tight loop **will evict the flash cache** holding CPU interpreter code. The IRAM_ATTR placement of 5 hot functions mitigates this partially, but other code still runs from flash cache.

**Only 5 functions have IRAM_ATTR:**
- `Arm7tdmi::cpu_run_until` (cpu.cpp:324)
- `Arm7tdmi::execute_arm` (cpu.cpp:778)
- `Arm7tdmi::execute_thumb` (cpu.cpp:1388)
- `downscale_565` (downscale.hpp:79)
- `downscale_565_v2` (downscale.hpp:120)

**NOT in IRAM — ranked by impact:**

| Priority | Function | Called From | Frequency | Why It Matters |
|----------|----------|-------------|-----------|----------------|
| **Critical** | `Bus::read` / `Bus::write` | `execute_arm`/`execute_thumb` lambdas (read8/16/32, write8/16/32) | Every load/store instruction (~1-3x per ARM instruction) | ~970-line dispatch in bus.cpp. Runs from flash cache. EWRAM/VRAM walks evict it. |
| **High** | `Timers::advance_to` | `service_due_hardware` → every scheduler event | Every ~1000 cycles | Timer cascade overflow depends on timely servicing. |
| **Medium** | `DmaEngine::service_due` | `service_due_hardware` | On DMA trigger (VBlank/HBlank/FIFO) | DMA owns the bus while active. Stalls here delay the whole frame. |
| **Low** | `Ppu::advance_to` | `service_due_hardware` | Per-scanline (~228x/frame) | State machine transitions, not per-pixel work. |
| **Low** | `Emulator::refresh_schedule` | Main loop iteration | ~4x per scheduler event | Flat 4-slot array scan, tiny function. |

**Next IRAM candidates (in order):** `Bus::read`/`Bus::write` >> `Timers::advance_to` >> `DmaEngine::service_due`. Adding Bus to IRAM would likely matter more than all other non-IRAM functions combined.

**Mitigations (priority order):**
1. Add `Bus::read`/`Bus::write` to IRAM — highest-impact non-IRAM fix
2. Add `Timers::advance_to` to IRAM — second priority
3. Batch VRAM/EWRAM accesses — don't interleave with code execution
4. Consider `CONFIG_SPIRAM_XIP_FROM_PSRAM` — moves flash `.text` to PSRAM. ESP-IDF says octal PSRAM at 80MHz is faster than quad flash at 80MHz. Eliminates flash cache contention entirely at the cost of PSRAM bandwidth.
5. Keep palette (1KB) and OAM (1KB) in internal SRAM — **already done correctly**

### 7. Display Buffer Allocation — Implicit PSRAM Routing

**Location:** `gba_runtime.cpp:100-101`

```cpp
ctx.display_bufs[0] = new u16[128 * 128];  // 32KB
ctx.display_bufs[1] = new u16[128 * 128];  // 32KB
```

Comment says "PSRAM" but uses plain `new`. With `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, allocations >16KB *should* route to PSRAM automatically (32KB > 16KB threshold). This is implicit behavior that depends on:
- The threshold staying unchanged
- Heap state at allocation time
- No future config regression

**This is unverified on real hardware.** The threshold-based routing should work, but hasn't been confirmed. Add a verification call after allocation:
```cpp
ctx.display_bufs[0] = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
ctx.display_bufs[1] = static_cast<u16*>(heap_caps_malloc(128 * 128 * sizeof(u16), MALLOC_CAP_SPIRAM));
heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);  // Verify PSRAM placement
```

**Fix:** Use `heap_caps_malloc(MALLOC_CAP_SPIRAM)` explicitly for guaranteed placement. Not urgent, but removes a correctness assumption.

### 8. WiFi Enabled in Runtime Config — Must Disable

**Location:** `platform/esp32s3/sdkconfig:1350` — `CONFIG_ESP_WIFI_ENABLED=y`

WiFi is **actually enabled** in the generated config. This is worse than a precaution — it's a live problem:
- WiFi task pinned to Core 0 (`sdkconfig:1367`) — same core as display loop
- WiFi stack uses IRAM (competes with emulator hot code for limited IRAM space)
- WiFi SPI operations cause flash cache stalls (both cores freeze during SPI1 flash ops)
- WiFi buffers consume significant SRAM and PSRAM

**Fix:** Create a separate `sdkconfig.defaults.runtime` that explicitly disables WiFi and Bluetooth for the runtime build:
```ini
CONFIG_ESP_WIFI_ENABLED=n
CONFIG_BT_ENABLED=n
```

---

## CATEGORY 3: CORRECTNESS / STABILITY CHANGES

### 9. C++ Exceptions — Keep for Tests, Disable for Runtime

**Current state:**
- `sdkconfig:612` — `CONFIG_COMPILER_CXX_EXCEPTIONS=y` (enabled)
- `sdkconfig:2280` — `CONFIG_CXX_EXCEPTIONS=y` (also enabled)
- `sdkconfig:614` — `# CONFIG_COMPILER_CXX_RTTI is not set` (already off despite defaults)

ESP-IDF C++ docs confirm: "Enabling [RTTI] typically increases the binary size by tens of kB." RTTI is already off — no action needed.

Exceptions are needed for the test runner (`gba_test_main.cpp` uses `try/catch`). For the runtime build, create a separate sdkconfig that disables both.

ESP-IDF also warns: "Vtables are placed into Flash and are not accessible when the flash cache is disabled. Therefore, virtual function calls should be avoided in IRAM-Safe Interrupt Handlers." The `Platform` base class uses virtual dispatch but is not in an ISR — this is fine.

### 10. Flash Concurrency / SPI Bus Contention

**Source:** ESP-IDF flash concurrency documentation:

> "The SPI0/1 bus is shared between the instruction & data cache (for firmware execution) and the SPI1 peripheral... operations to SPI1 will cause significant influence to the whole system."

The ST7735 display is on SPI2 (separate from SPI0/1 flash) — this is correct. But:
- WiFi NVS operations use SPI1 flash → stalls both cores
- Any `esp_partition` write (OTA, NVS, SPIFFS) → stalls both cores
- During stalls, IRAM code runs fine, flash code freezes

**For initial bring-up:** Keep WiFi/BT disabled. No NVS writes during emulation. Long-term: consider `CONFIG_SPI_FLASH_AUTO_SUSPEND` or `CONFIG_SPIRAM_XIP_FROM_PSRAM`.

### 11. Platform Abstraction Inconsistency

**Files:** `include/gba/platform/platform.hpp`, `src/platform/rt1170/rt1170_platform.cpp`

`Platform` base class has virtual functions. `Rt1170Platform` is the only implementation (stub). The ESP32 runtime (`gba_runtime.cpp`) bypasses it entirely — calls emulator methods directly.

**Options:**
- (a) Remove `Platform` abstraction (cleanest — no one uses it for ESP32)
- (b) Implement `Esp32Platform` and use it consistently

Either way, the virtual dispatch is not in a hot path (called once per frame, not per instruction), so this is code hygiene, not a performance issue.

### 12. Save File Persistence — Volatile

**File:** `include/gba/core/cartridge.hpp:56` — `std::vector<u8> save_`

Save data stored in RAM only. On power-off, all saves are lost.

**Fix:** Persist to SPIFFS/LittleFS/SD card. The `std::vector` for save data is fine (max 128KB for Flash128K saves, fits in PSRAM). Trigger writes on save-type write with a dirty flag + periodic flush. Avoid writing during frame emulation — buffer and flush in the display loop on Core 0.

---

## CATEGORY 4: FUTURE OPTIMIZATIONS — Nice-to-Have

### 13. PPU Direct-to-128x128 Rendering

**Current:** Renders 240x160 (75KB framebuffer) → downscale to 128x128 (32KB) via LUT-based box average (~0.8ms).

**Wastes:**
- 75KB PSRAM for full framebuffer
- ~0.8ms CPU time for downscaling per frame
- Double VRAM bandwidth (render + read-back)

**Fix:** Render only the 128x128 pixels the display needs. Crop-and-decimate during scanline render. Eliminates the 75KB framebuffer and downscaler entirely.

### 14. Frame Skipping

**Source:** gpSP implements audio-driven frameskip. For ESP32, simple timed frameskip is more practical.

If `run_frame()` takes >16.7ms, skip PPU rendering on the next frame. CPU still advances state. Gives ~2x headroom for complex games.

```cpp
if (frame_time_us > 16742) {
    skip_next_render = true;
}
```

### 15. CPU Computed Goto Dispatch

**Source:** gpSP optimization docs confirm computed goto (label-as-value) eliminates branch prediction misses on tight interpreter loops. Estimated 15-20% speedup.

**Caveat:** ESP-IDF docs note "Various section attributes (such as `IRAM_ATTR`) are ignored when used with template functions." Computed goto uses labels-as-values (GCC extension, not templates), so this should work with IRAM_ATTR. But needs testing.

### 16. Idle Loop Detection

**Source:** gpSP detects games spinning in VBlank-wait loops and fast-forwards to the next hardware event.

Many GBA games spend 50%+ of frames in `while(!VBlank) {}`. Detecting these idle-loop PC addresses and fast-forwarding gives enormous speedups. The codebase already has halt/wakeup infrastructure — extending with idle-loop PC detection would be high-value.

### 17. Pre-Decoded Instruction Cache

**Current state:** `execute_arm` (cpu.cpp:806-1168) has an **11-level sequential if-cascade** to determine instruction category:

```
1. BX            (instruction & 0x0FFFFFF0) == 0x012FFF10     // line 806
2. MUL           (instruction & 0x0FC000F0) == 0x00000090     // line 814
3. MULL          (instruction & 0x0F8000F0) == 0x00800090     // line 837
4. Halfword L/S  (instruction & 0x0E000090) == 0x00000090     // line 888
5. MRS           (instruction & 0x0FBF0FFF) == 0x010F0000     // line 942
6. MSR           (instruction & 0x0DB0F000) == 0x0120F000     // line 948
7. Branch        ((instruction >> 25) & 0x7) == 0x5           // line 990
8. LDR/STR       ((instruction >> 26) & 0x3) == 0x1           // line 1001
9. LDM/STM       ((instruction >> 25) & 0x7) == 0x4           // line 1051
10. SWI          (instruction & 0x0F000000) == 0x0F000000     // line 1155
11. Data proc    ((instruction >> 26) & 0x3) == 0x0  (fallback) // line 1168
```

For the most common instruction type (data processing — ALU ops like MOV, ADD, CMP), **all 11 checks are evaluated** before hitting the fallback.

**Fix:** A 256-entry lookup table indexed by `instruction >> 24` (top 8 bits determine ARM instruction category) reduces dispatch to 1-2 comparisons. For full precision, a 4096-entry table indexed by `instruction >> 20` (bits [31:20]) gives exact single-lookup categorization at the cost of 4KB of table storage (fits in SRAM). Estimated 1.3-1.5x speedup.

---

## PRIORITY TABLE

| Pri | Item | Effort | Impact | Verified By |
|-----|------|--------|--------|-------------|
| **P0** | ROM loading | M | Nothing runs | Code: gba_runtime.cpp:89 |
| **P0** | Input GPIO | S | No control | Code: set_keys() never called |
| **P0** | Watchdog handling | S | Prevents reboot | sdkconfig:1308 TWDT=5s |
| **P1** | APU ring buffer | S | Eliminates heap frag + latency | ESPHome #11279, #14733 |
| **P1** | `<cmath>` → integer math | S | Removes soft-float bloat | cpu.cpp:754,761,769 |
| **P1** | WiFi disable for runtime | S | ~100KB IRAM, removes stalls | sdkconfig:1350 WIFI_ENABLED=y |
| **P1** | Display buf explicit PSRAM | T | Correctness guarantee | gba_runtime.cpp:100-101 |
| **P1** | Separate runtime sdkconfig | S | Exceptions/RTTI/WiFi split | sdkconfig:612,614,1350 |
| **P2** | Save file persistence | M | Game saves | cartridge.hpp:56 |
| **P2** | Platform abstraction cleanup | S | Code hygiene | platform.hpp virtuals |
| **P3** | PPU direct 128x128 render | M | -75KB PSRAM, -0.8ms/frame | ppu.hpp framebuffer_ |
| **P3** | Frame skipping | S | 2x headroom | emulator.cpp run_frame() |
| **P3** | Computed goto CPU | L | 15-20% speedup | gpSP docs |
| **P3** | Idle loop detection | M | 30-50% for many games | gpSP docs |
| **P3** | Pre-decoded instruction cache | L | 1.3-1.5x speedup | cpu.cpp execute_arm/thumb |

---

## MEMORY MAP (ESP32-S3, Verified Against Code)

| Data | Size | Location | Allocation | Source |
|------|------|----------|------------|--------|
| CPU registers (CpuState) | ~200B | Internal SRAM | Struct member | cpu.hpp |
| Scheduler (4 slots) | ~64B | Internal SRAM | Struct member | scheduler.hpp |
| IRQ controller | ~32B | Internal SRAM | Struct member | irq.hpp |
| Timers (4 channels) | ~128B | Internal SRAM | Struct member | timers.hpp |
| DMA engine (4 channels) | ~128B | Internal SRAM | Struct member | dma.hpp |
| Bus logic (waitcnt, prefetch) | ~64B | Internal SRAM | Struct member | bus.hpp |
| APU control registers | ~128B | Internal SRAM | Struct member | apu.hpp |
| IWRAM (guest) | 32 KB | Internal SRAM | `MALLOC_CAP_INTERNAL` | bus.cpp:140 |
| Palette RAM | 1 KB | Internal SRAM | `MALLOC_CAP_INTERNAL` | bus.cpp:141 |
| OAM | 1 KB | Internal SRAM | `MALLOC_CAP_INTERNAL` | bus.cpp:143 |
| EWRAM (guest) | 256 KB | PSRAM | `MALLOC_CAP_SPIRAM` | bus.cpp:139 |
| VRAM (guest) | 96 KB | PSRAM | `MALLOC_CAP_SPIRAM` | bus.cpp:142 |
| Framebuffer (240x160) | 75 KB | PSRAM | `MALLOC_CAP_SPIRAM` | ppu.cpp:30 |
| Display buffers (x2) | 64 KB | PSRAM (implicit) | `new u16[]` via threshold | gba_runtime.cpp:100 |
| Downscale LUT | 512 B | Stack | — | downscale.hpp |

**Total PSRAM: ~491 KB** (fits in 8MB with room to spare)
**Total internal SRAM: ~100 KB** (fits in 512KB)

---

## KEY CORRECTIONS FROM CROSS-CHECK

| Original Claim | Correction | Source |
|----------------|-----------|--------|
| "Palette/OAM go to PSRAM, move to SRAM" | **Already in SRAM.** `bus.cpp:141-143` passes `prefer_internal=true` | bus.cpp:141-143 |
| "WiFi should be disabled as precaution" | **WiFi is actually enabled** in sdkconfig:1350. More urgent than stated. | sdkconfig:1350 |
| "`std::optional` is a concern" | Wrong. ESP-IDF supports C++20/23. `std::optional` has no heap allocation. No change needed. | ESP-IDF C++ docs |
| "RTTI needs to be disabled for runtime" | **Already off** in generated sdkconfig:614 despite defaults requesting it. Lower priority. | sdkconfig:614 |
| "`<cmath>` is a stale include" | **Actually used** — SWI 0x08/0x09/0x0A use `std::sqrt(double)` and `std::atan2(double)`. Must replace with integer math. | cpu.cpp:754,761,769 |
| "PSRAM cache thrashing is moderate concern" | **Bigger than stated.** ESP-IDF docs: >32KB PSRAM access evicts flash cache. Emulator touches >400KB/frame. Only 5 of many hot functions are in IRAM. | ESP-IDF external RAM docs |

---

## EXTERNAL REFERENCES

- ESP-IDF External RAM: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/external-ram.html
- ESP-IDF Flash Concurrency: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/spi_flash/spi_flash_concurrency.html
- ESP-IDF C++ Support: https://docs.espressif.com/projects/esp-idf/en/v5.3.5/esp32s3/api-guides/cplusplus.html
- ESPHome deque fragmentation: https://github.com/esphome/esphome/issues/11279
- ESPHome deque → FixedRingBuffer: https://github.com/esphome/esphome/pull/14733
- retro-go GBA on ESP32: https://github.com/ducalex/retro-go/issues/96
- gpSP Performance Techniques: https://deepwiki.com/libretro/gpsp/7.3-performance-optimization-techniques
- mGBA Accuracy/Speed: https://mgba.io/2017/04/30/emulation-accuracy/
- mGBA Cycle Counting/Prefetch: https://mgba.io/2015/06/27/cycle-counting-prefetch/
