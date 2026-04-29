# ESP32-S3 Display Pipeline

Architecture: double-buffered DMA SPI framebuffer with dual-core task partitioning.

## Memory Layout

| Buffer | Location | Size | Allocation | Purpose |
|--------|----------|------|-----------|---------|
| `display_bufs[0]` | PSRAM | 32 KB | `MALLOC_CAP_SPIRAM` | Ping-pong framebuffer A |
| `display_bufs[1]` | PSRAM | 32 KB | `MALLOC_CAP_SPIRAM` | Ping-pong framebuffer B |
| `s_swap_buf` | Internal DRAM | 32 KB | `MALLOC_CAP_DMA \| MALLOC_CAP_INTERNAL` → fallback `MALLOC_CAP_DMA` | Byte-swap staging (SSD1351 only) |
| `ui_buf` | PSRAM | 32 KB | `MALLOC_CAP_SPIRAM` | ROM selector UI (freed before emulation) |

- PSRAM framebuffers: `heap_caps_malloc(kOutputPixels * sizeof(u16), MALLOC_CAP_SPIRAM)` at `gba_runtime.cpp:527-528`
- ESP32-S3 GDMA supports PSRAM directly (`CONFIG_SOC_PSRAM_DMA_CAPABLE=y`, `CONFIG_SOC_AHB_GDMA_SUPPORT_PSRAM=y`)
- ST7735 path: PSRAM pointer passed directly to `esp_lcd_panel_io_tx_color` — no staging copy
- SSD1351 path: CPU copies PSRAM → internal DRAM with byte-swap (`gba_display.cpp:58-62`), then internal DRAM → DMA

## Dual-Core Pipeline

```
Core 1 (gba_cpu, priority 5)                    Core 0 (app_main loop)
──────────────────────────                      ──────────────────────────
                                                 ulTaskNotifyTake()  ← BLOCKS
buf = back.load()
ppu.set_external_fb(display_bufs[buf])
emulator->run_frame()
  PPU renders center 128x128 crop
  of 240x160 directly into PSRAM[buf]
convert_rgb555_to_rgb565_inplace(buf)
front.store(buf)                                 │
back.store(buf ^ 1)                              │
xTaskNotifyGive() ──────────────────────────→    WAKES
[starts next frame into OTHER buffer]            front.load() → buf
                                                 FOR y=0..127 STEP 16:
                                                   display_draw(0, y, 128, 16, pixels+y*128)
                                                     → cmd(CASET) [SPI blocking]
                                                     → cmd(RASET) [SPI blocking]
                                                     → esp_lcd_panel_io_tx_color [SPI DMA blocking]
                                                 ulTaskNotifyTake()  ← BLOCKS
```

Source: `gba_runtime.cpp:312-400` (Core 1), `gba_runtime.cpp:565-597` (Core 0).

## Synchronization

- **Buffer swap**: `std::atomic<int> front/back` with XOR flip (`gba_runtime.cpp:362-363`). Front and back always index opposite buffers — no simultaneous read/write.
- **Inter-core wake**: `xTaskNotifyGive` / `ulTaskNotifyTake` (`gba_runtime.cpp:365, 566`). FreeRTOS task notifications provide acquire/release semantics — all PPU writes are visible to Core 0 before it starts DMA.
- **DMA fence**: `esp_lcd_panel_io_tx_color` is a blocking call — it does not return until the SPI DMA transfer completes. Core 0 finishes all 8 chunks before calling `ulTaskNotifyTake` again. No async callback needed. `on_color_trans_done = nullptr` at `gba_display.cpp:321`.

## Per-Chunk SPI Sequence

Each 16-row chunk (128 × 16 × 2 = 4 KB) triggers 3 SPI transactions:

1. `cmd(CASET)` — set column address range
2. `cmd(RASET)` — set row address range
3. `esp_lcd_panel_io_tx_color(RAMWR, pixels, 4096)` — blocking DMA pixel transfer

8 chunks × 3 transactions = 24 SPI transactions per frame (ST7735 128×128).
SSD1351 at 128×96 is 6 chunks = 18 transactions.

## SPI Configuration

| Parameter | ST7735 | SSD1351 |
|-----------|--------|---------|
| Bus | SPI2_HOST | SPI2_HOST |
| Clock | 26 MHz | 8 MHz |
| DMA channel | SPI_DMA_CH_AUTO | SPI_DMA_CH_AUTO |
| Queue depth | 10 | 10 |
| Max transfer size | 32 KB | 32 KB |
| Byte swap needed | No | Yes |

Source: `gba_board_profile.h:101-115`, `gba_display.cpp:305-330`.

## PPU Output

PPU renders in direct 128×128 mode (`ppu.cpp:289-312`):
- Crops center of 240×160 GBA framebuffer: `kCropX = (240-128)/2 = 56`, `kCropY = (160-128)/2 = 16`
- Writes RGB555 pixels directly into the external PSRAM buffer with stride `kOutW = 128`
- Post-frame conversion: `convert_rgb555_to_rgb565_inplace` shifts R and G channels left by 1 bit (`gba_runtime.cpp:305-309`)

## Frame Skip

If a frame exceeds the 16.667ms budget (`gba_runtime.cpp:374`), the next frame's PPU render is skipped (`set_skip_render(true)`) — CPU still advances but no pixels are written. Audio still processes.

## Known Considerations

1. **Chunked transfers not pipelined**: `trans_queue_depth = 10` but only 1 transaction is in-flight at a time. Each `display_draw` blocks until complete (3 sequential blocking SPI calls per chunk). Potential optimization: queue multiple chunks and use `on_color_trans_done` callback for async pipeline. Marginal gain — the sequential overhead between chunks is small compared to SPI transfer time.

2. **Cache coherency (theoretical)**: Core 1 PPU writes to cached PSRAM; GDMA reads PSRAM directly. CPU writes go through write-back cache — may not be visible to DMA without explicit writeback. In practice, the 16K-pixel conversion loop + FreeRTOS task notification barrier cause natural cache writeback. Adding `esp_cache_msync(buf, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M)` before the notify would be defensive for future ESP-IDF versions. Not currently causing issues.

3. **SSD1351 byte-swap is scalar**: The swap loop at `gba_display.cpp:58-62` copies and byte-swaps ~2048 pixels per chunk in a scalar loop on Core 0. CPU cost is negligible (~0.01ms per chunk at 240 MHz) compared to SPI transfer time (~4ms per chunk at 8 MHz SPI).

4. **PSRAM allocation flags**: Framebuffers use `MALLOC_CAP_SPIRAM` only. ESP32-S3 GDMA handles PSRAM natively, but adding `MALLOC_CAP_DMA` would guarantee DMA-safe alignment. ESP-IDF's SPI driver handles alignment internally, but explicit is better.

## File Reference

| File | Purpose |
|------|---------|
| `platform/esp32s3/main/gba_runtime.cpp` | Dual-core pipeline, buffer alloc, frame loop |
| `platform/esp32s3/main/gba_display.cpp` | SPI driver, DMA transfers, panel init (ST7735 + SSD1351) |
| `platform/esp32s3/main/gba_display.h` | Display API surface |
| `platform/esp32s3/main/gba_board_profile.h` | Pin mappings, driver selection, dimensions, clocks |
| `src/core/ppu.cpp` | Direct 128×128 crop render mode |
