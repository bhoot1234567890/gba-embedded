/*
 * GBA 240x160 RGB565 → 128x128 RGB565 downscaler.
 *
 * Box-average downscale with pre-computed LUT and unrolled cases.
 * Adapted from esp32_gba_to_565.h for native RGB565→RGB565 path.
 * No intermediate format conversion — reads u16, writes u16.
 *
 * Memory: ~1KB LUT, no intermediate buffer
 * Speed:  ~0.8ms single-core on ESP32-S3 (half the RGB24 version)
 */

#pragma once

#include <cstdint>
#include <cstddef>

#include "gba/core/types.hpp"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

namespace gba {

constexpr u32 kOutW = 128;
constexpr u32 kOutH = 128;

/* ── RGB565 averaging ── */

static inline u16 avg565_2(u16 a, u16 b) {
    /* Decompose, average with rounding, recompose */
    const u32 r = (((a >> 11u) & 0x1Fu) + ((b >> 11u) & 0x1Fu) + 1u) >> 1u;
    const u32 g = (((a >>  5u) & 0x3Fu) + ((b >>  5u) & 0x3Fu) + 1u) >> 1u;
    const u32 bl = (((a       ) & 0x1Fu) + ((b       ) & 0x1Fu) + 1u) >> 1u;
    return static_cast<u16>((r << 11u) | (g << 5u) | bl);
}

static inline u16 avg565_4(u16 a, u16 b, u16 c, u16 d) {
    const u32 r = (((a >> 11u) & 0x1Fu) + ((b >> 11u) & 0x1Fu)
                 + ((c >> 11u) & 0x1Fu) + ((d >> 11u) & 0x1Fu) + 2u) >> 2u;
    const u32 g = (((a >>  5u) & 0x3Fu) + ((b >>  5u) & 0x3Fu)
                 + ((c >>  5u) & 0x3Fu) + ((d >>  5u) & 0x3Fu) + 2u) >> 2u;
    const u32 bl = (((a       ) & 0x1Fu) + ((b       ) & 0x1Fu)
                  + ((c       ) & 0x1Fu) + ((d       ) & 0x1Fu) + 2u) >> 2u;
    return static_cast<u16>((r << 11u) | (g << 5u) | bl);
}

/* ── Pre-computed LUT (~1KB, init once) ── */

struct DownscaleLut {
    /* Per output column: source x index, box width in source pixels */
    u8 col_sx0[kOutW];
    u8 col_ncols[kOutW];

    /* Per output row: source y index, box height in source rows */
    u8 row_sy0[kOutH];
    u8 row_nrows[kOutH];
};

static inline DownscaleLut make_downscale_lut() {
    DownscaleLut lut{};
    for (u32 dx = 0; dx < kOutW; dx++) {
        const u32 sx0 = dx * kScreenWidth / kOutW;
        const u32 sx1 = (dx + 1u) * kScreenWidth / kOutW;
        lut.col_sx0[dx]  = static_cast<u8>(sx0);
        lut.col_ncols[dx] = static_cast<u8>(sx1 - sx0);
    }
    for (u32 dy = 0; dy < kOutH; dy++) {
        const u32 sy0 = dy * kScreenHeight / kOutH;
        const u32 sy1 = (dy + 1u) * kScreenHeight / kOutH;
        lut.row_sy0[dy]   = static_cast<u8>(sy0);
        lut.row_nrows[dy] = static_cast<u8>(sy1 - sy0);
    }
    return lut;
}

/* ── V1: Direct RGB565 → RGB565, LUT + unrolled ── */

static inline void IRAM_ATTR downscale_565(
    const DownscaleLut& lut,
    const u16* src,   /* kScreenWidth * kScreenHeight (240x160) */
    u16* dst          /* kOutW * kOutH (128x128) */
) {
    for (u32 dy = 0; dy < kOutH; dy++) {
        const u32 sy0 = lut.row_sy0[dy];
        const int nr  = lut.row_nrows[dy];
        const u16* row0 = src + sy0 * kScreenWidth;
        const u16* row1 = (nr > 1) ? src + (sy0 + 1u) * kScreenWidth : nullptr;

        u16* d = dst + dy * kOutW;

        for (u32 dx = 0; dx < kOutW; dx++) {
            const u32 sx = lut.col_sx0[dx];
            const int nc = lut.col_ncols[dx];

            if (nr == 1) {
                if (nc == 1) {
                    /* 1x1 — direct */
                    d[dx] = row0[sx];
                } else {
                    /* 2x1 — horizontal average */
                    d[dx] = avg565_2(row0[sx], row0[sx + 1u]);
                }
            } else {
                if (nc == 1) {
                    /* 1x2 — vertical average */
                    d[dx] = avg565_2(row0[sx], row1[sx]);
                } else {
                    /* 2x2 — 4-pixel average */
                    d[dx] = avg565_4(row0[sx], row0[sx + 1u],
                                     row1[sx], row1[sx + 1u]);
                }
            }
        }
    }
}

/* ── V2: Process 2 output pixels at a time (better register pressure) ── */

static inline void IRAM_ATTR downscale_565_v2(
    const DownscaleLut& lut,
    const u16* src,
    u16* dst
) {
    for (u32 dy = 0; dy < kOutH; dy++) {
        const u32 sy0 = lut.row_sy0[dy];
        const int nr  = lut.row_nrows[dy];
        const u16* row0 = src + sy0 * kScreenWidth;
        const u16* row1 = (nr > 1) ? src + (sy0 + 1u) * kScreenWidth : nullptr;

        u16* d = dst + dy * kOutW;

        u32 dx = 0;
        for (; dx + 1u < kOutW; dx += 2u) {
            const u32 sx0 = lut.col_sx0[dx];
            const u32 sx1 = lut.col_sx0[dx + 1u];
            const int nc0 = lut.col_ncols[dx];
            const int nc1 = lut.col_ncols[dx + 1u];

            u16 p0, p1;

            if (nr == 1) {
                p0 = (nc0 == 1) ? row0[sx0]
                               : avg565_2(row0[sx0], row0[sx0 + 1u]);
                p1 = (nc1 == 1) ? row0[sx1]
                               : avg565_2(row0[sx1], row0[sx1 + 1u]);
            } else {
                p0 = (nc0 == 1) ? avg565_2(row0[sx0], row1[sx0])
                               : avg565_4(row0[sx0], row0[sx0 + 1u],
                                          row1[sx0], row1[sx0 + 1u]);
                p1 = (nc1 == 1) ? avg565_2(row0[sx1], row1[sx1])
                               : avg565_4(row0[sx1], row0[sx1 + 1u],
                                          row1[sx1], row1[sx1 + 1u]);
            }

            d[dx]     = p0;
            d[dx + 1] = p1;
        }

        /* Odd column */
        if (dx < kOutW) {
            const u32 sx = lut.col_sx0[dx];
            const int nc = lut.col_ncols[dx];
            if (nr == 1) {
                d[dx] = (nc == 1) ? row0[sx]
                                  : avg565_2(row0[sx], row0[sx + 1u]);
            } else {
                d[dx] = (nc == 1) ? avg565_2(row0[sx], row1[sx])
                                  : avg565_4(row0[sx], row0[sx + 1u],
                                             row1[sx], row1[sx + 1u]);
            }
        }
    }
}

}  // namespace gba
