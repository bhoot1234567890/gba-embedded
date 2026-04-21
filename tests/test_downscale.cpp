/*
 * Standalone test for the GBA RGB565 downscaler.
 * Compiles on desktop (no ESP32 needed).
 *
 * Build:  g++ -O2 -std=c++20 -o test_downscale tests/test_downscale.cpp -Iinclude
 * Run:    ./test_downscale
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gba/core/constants.hpp"
#include "gba/core/downscale.hpp"
#include "gba/core/types.hpp"

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
    std::printf("  PASS: %s\n", message);
}

/* Fill source framebuffer with a known gradient pattern */
void fill_gradient(std::array<gba::u16, gba::kFramebufferPixels>& fb) {
    for (gba::u32 y = 0; y < gba::kScreenHeight; y++) {
        for (gba::u32 x = 0; x < gba::kScreenWidth; x++) {
            /* Red gradient horizontal, green gradient vertical, blue fixed */
            const auto r = static_cast<gba::u16>((x * 31u) / (gba::kScreenWidth - 1u));
            const auto g = static_cast<gba::u16>((y * 63u) / (gba::kScreenHeight - 1u));
            const auto b = gba::u16{16};  /* fixed mid-blue */
            fb[y * gba::kScreenWidth + x] = static_cast<gba::u16>((r << 11u) | (g << 5u) | b);
        }
    }
}

/* Fill with a checkerboard pattern */
void fill_checkerboard(std::array<gba::u16, gba::kFramebufferPixels>& fb) {
    for (gba::u32 y = 0; y < gba::kScreenHeight; y++) {
        for (gba::u32 x = 0; x < gba::kScreenWidth; x++) {
            const bool white = ((x / 8u) + (y / 8u)) % 2u == 0u;
            fb[y * gba::kScreenWidth + x] = white ? 0xFFFFu : 0x0000u;
        }
    }
}

gba::u16 extract_r(gba::u16 pixel) { return (pixel >> 11u) & 0x1Fu; }
gba::u16 extract_g(gba::u16 pixel) { return (pixel >> 5u) & 0x3Fu; }
gba::u16 extract_b(gba::u16 pixel) { return pixel & 0x1Fu; }

}  // namespace

int main() {
    using namespace gba;

    std::printf("=== Downscaler Tests ===\n\n");

    /* Test 1: LUT sanity */
    {
        std::printf("Test 1: LUT sanity\n");
        const auto lut = make_downscale_lut();

        /* First output column maps to source column 0 */
        expect(lut.col_sx0[0] == 0, "col_sx0[0] == 0");

        /* Last output column maps near end of source */
        expect(lut.col_sx0[kOutW - 1] > 0, "last col_sx0 > 0");

        /* First output row maps to source row 0 */
        expect(lut.row_sy0[0] == 0, "row_sy0[0] == 0");

        /* Box widths are 1 or 2 */
        bool all_valid = true;
        for (u32 i = 0; i < kOutW; i++) {
            if (lut.col_ncols[i] < 1 || lut.col_ncols[i] > 2) all_valid = false;
        }
        expect(all_valid, "all col_ncols in [1,2]");

        bool all_rows_valid = true;
        for (u32 i = 0; i < kOutH; i++) {
            if (lut.row_nrows[i] < 1 || lut.row_nrows[i] > 2) all_rows_valid = false;
        }
        expect(all_rows_valid, "all row_nrows in [1,2]");
    }

    /* Test 2: Gradient downscale preserves corners */
    {
        std::printf("\nTest 2: Gradient downscale\n");
        std::array<u16, kFramebufferPixels> src{};
        fill_gradient(src);

        const auto lut = make_downscale_lut();
        std::array<u16, kOutW * kOutH> dst{};

        downscale_565(lut, src.data(), dst.data());

        /* Top-left should be near black (low R, low G) */
        const auto tl = dst[0];
        expect(extract_r(tl) <= 1, "top-left red near 0");
        expect(extract_g(tl) <= 1, "top-left green near 0");

        /* Bottom-right should be near max */
        const auto br = dst[(kOutH - 1) * kOutW + (kOutW - 1)];
        expect(extract_r(br) >= 29, "bottom-right red near max");
        expect(extract_g(br) >= 60, "bottom-right green near max");

        /* Blue should be preserved (~16) everywhere */
        const auto mid = dst[kOutH / 2 * kOutW + kOutW / 2];
        expect(extract_b(mid) >= 14 && extract_b(mid) <= 18, "blue channel preserved (~16)");
    }

    /* Test 3: Checkerboard averaging */
    {
        std::printf("\nTest 3: Checkerboard averaging\n");
        std::array<u16, kFramebufferPixels> src{};
        fill_checkerboard(src);

        const auto lut = make_downscale_lut();
        std::array<u16, kOutW * kOutH> dst{};

        downscale_565(lut, src.data(), dst.data());

        /* With 8x8 checkerboard on 240x160 → 128x128, most output pixels
         * are within a single 8x8 block (same color). But some will land
         * on the boundary and get averaged. Check that:
         * - Output pixels are either 0x0000, 0xFFFF, or an average */
        bool found_avg = false;
        bool all_valid = true;
        for (u32 i = 0; i < kOutW * kOutH; i++) {
            const auto p = dst[i];
            if (p != 0x0000u && p != 0xFFFFu) {
                found_avg = true;
                /* Average of 0x0000 and 0xFFFF should be ~0x7BEF
                 * (R=15.5→15, G=31.5→31, B=15.5→15 = 0x7BEF) */
                const auto r = extract_r(p);
                const auto g = extract_g(p);
                const auto b = extract_b(p);
                if (r > 16 || g > 32 || b > 16) all_valid = false;
            }
        }
        expect(found_avg, "found averaged pixels at checkerboard boundaries");
        expect(all_valid, "all averaged pixels have reasonable values");
    }

    /* Test 4: V1 and V2 produce identical output */
    {
        std::printf("\nTest 4: V1/V2 identical output\n");
        std::array<u16, kFramebufferPixels> src{};
        fill_gradient(src);

        const auto lut = make_downscale_lut();
        std::array<u16, kOutW * kOutH> dst1{};
        std::array<u16, kOutW * kOutH> dst2{};

        downscale_565(lut, src.data(), dst1.data());
        downscale_565_v2(lut, src.data(), dst2.data());

        bool identical = true;
        for (u32 i = 0; i < kOutW * kOutH; i++) {
            if (dst1[i] != dst2[i]) {
                identical = false;
                std::fprintf(stderr, "  Mismatch at pixel %u: v1=0x%04X v2=0x%04X\n",
                             i, dst1[i], dst2[i]);
                break;
            }
        }
        expect(identical, "V1 and V2 produce identical output");
    }

    /* Test 5: Solid color passthrough */
    {
        std::printf("\nTest 5: Solid color passthrough\n");
        std::array<u16, kFramebufferPixels> src{};
        /* Pure red (R=31, G=0, B=0) */
        src.fill(static_cast<u16>(0xF800u));

        const auto lut = make_downscale_lut();
        std::array<u16, kOutW * kOutH> dst{};

        downscale_565(lut, src.data(), dst.data());

        bool all_red = true;
        for (u32 i = 0; i < kOutW * kOutH; i++) {
            if (dst[i] != 0xF800u) {
                all_red = false;
                break;
            }
        }
        expect(all_red, "solid red passthrough — avg565_2/4 of identical values returns same value");
    }

    std::printf("\n=== All downscaler tests passed ===\n");
    return 0;
}
