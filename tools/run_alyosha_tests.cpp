#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gba/core/constants.hpp"
#include "gba/core/emulator.hpp"

// Runs alyosha-tas/gba-tests ROMs against the emulator.
// Tests render pass/fail text in mode 4 bitmap (white on black).
// Pass: "All tests passed" at ~(56,76). Fail: "Failed test XXX" at ~(60,76).

using namespace gba;

static constexpr int kRunFrames = 120;

static void write_u32(std::vector<u8>& buf, u32 offset, u32 val) {
    buf[offset] = static_cast<u8>(val);
    buf[offset + 1] = static_cast<u8>(val >> 8);
    buf[offset + 2] = static_cast<u8>(val >> 16);
    buf[offset + 3] = static_cast<u8>(val >> 24);
}

static std::vector<u8> make_bios_stub() {
    std::vector<u8> bios(kBiosSize, 0xFF);
    auto put = [&](u32 addr, u32 instr) { write_u32(bios, addr, instr); };

    // Exception vectors
    put(0x00, 0xEA00001E); // B reset_handler (0x80)
    put(0x04, 0xEAFFFFFE); // B . (undefined loop)
    put(0x08, 0xEAFFFFFE); // B . (SWI - HLE intercepts)
    put(0x0C, 0xEAFFFFFE); // B . (prefetch abort)
    put(0x10, 0xEAFFFFFE); // B . (data abort)
    put(0x14, 0xEAFFFFFE); // B . (reserved)
    put(0x18, 0xEA000008); // B irq_handler (0x40)
    put(0x1C, 0xEAFFFFFE); // B . (FIQ)

    // irq_handler at 0x40
    put(0x40, 0xE92D500F); // STMFD SP!, {R0-R3,R12,LR}
    put(0x44, 0xE59FC01C); // LDR R12, [PC, #0x1C] → 0x68
    put(0x48, 0xE59CF000); // LDR R12, [R12]
    put(0x4C, 0xE35C0000); // CMP R12, #0
    put(0x50, 0x0A000002); // BEQ return
    put(0x54, 0xE1A0E00F); // MOV LR, PC
    put(0x58, 0xE1A0F00C); // MOV PC, R12
    put(0x5C, 0xE89D500F); // LDMFD SP!, {R0-R3,R12,LR}
    put(0x60, 0xE25EF004); // SUBS PC, LR, #4
    put(0x64, 0xEAFFFFFE); // B . (safety)
    put(0x68, 0x03007FFC); // data: handler pointer address

    // reset_handler at 0x80
    put(0x80, 0xE321F0D2); // MSR CPSR_c, #0xD2 (IRQ mode)
    put(0x84, 0xE59FD044); // LDR SP, [PC, #0x44] → 0xD0
    put(0x88, 0xE321F0D3); // MSR CPSR_c, #0xD3 (SVC mode)
    put(0x8C, 0xE59FD034); // LDR SP, [PC, #0x34] → 0xC8
    put(0x90, 0xE321F0DF); // MSR CPSR_c, #0xDF (System mode)
    put(0x94, 0xE59FD024); // LDR SP, [PC, #0x24] → 0xC0
    put(0x98, 0xE3A00000); // MOV R0, #0
    put(0x9C, 0xE59F101C); // LDR R1, [PC, #0x1C] → 0xC4
    put(0xA0, 0xE5810000); // STR R0, [R1]
    put(0xA4, 0xE59FF010); // LDR PC, [PC, #0x10] → 0xBC
    // Data pool
    put(0xBC, 0x08000000); // ROM entry point
    put(0xC0, 0x03007F00); // System SP
    put(0xC4, 0x03007FFC); // IRQ handler pointer address
    put(0xC8, 0x03007FE0); // SVC SP
    put(0xD0, 0x03007FA0); // IRQ SP

    return bios;
}

static void write_ppm(const char* path, std::span<const u16> fb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P3\n240 160\n255\n");
    for (int y = 0; y < 160; ++y) {
        for (int x = 0; x < 240; ++x) {
            u16 px = fb[y * 240 + x];
            int r = (px >> 8) & 0xF8;
            int g = (px >> 3) & 0xFC;
            int b = (px << 3) & 0xF8;
            std::fprintf(f, "%d %d %d ", r, g, b);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

static int count_text_pixels(std::span<const u16> fb) {
    int count = 0;
    for (int y = 72; y < 88; ++y) {
        for (int x = 48; x < 200; ++x) {
            if (fb[y * 240 + x] != 0) ++count;
        }
    }
    return count;
}

static bool detect_pass(std::span<const u16> fb) {
    int white_streak = 0;
    int max_streak = 0;
    for (int y = 76; y < 84; ++y) {
        white_streak = 0;
        for (int x = 50; x < 200; ++x) {
            u16 px = fb[y * 240 + x];
            if (px >= 0xEF7F) {
                ++white_streak;
                if (white_streak > max_streak) max_streak = white_streak;
            } else {
                white_streak = 0;
            }
        }
    }
    return max_streak >= 40;
}

static std::string get_test_name(const std::filesystem::path& p) {
    auto name = p.stem().string();
    auto dir = p.parent_path().filename().string();
    return dir + "/" + name;
}

int main(int argc, char** argv) {
    std::filesystem::path rom_dir = "/Users/chaitanyamalhotra/development/gba-tests-alyosha";
    std::filesystem::path bios_path;
    bool use_stub_bios = true;
    std::filesystem::path output_dir = "/tmp/alyosha_results";
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--rom-dir") == 0 && i + 1 < argc) {
            rom_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
            use_stub_bios = false;
        } else if (std::strcmp(argv[i], "--stub-bios") == 0) {
            use_stub_bios = true;
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        }
    }

    std::filesystem::create_directories(output_dir);

    std::vector<std::filesystem::path> roms;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(rom_dir)) {
        if (entry.path().extension() == ".gba") {
            roms.push_back(entry.path());
        }
    }
    std::sort(roms.begin(), roms.end());

    std::printf("=== Alyosha GBA Tests ===\n");
    std::printf("ROM dir: %s\n", rom_dir.c_str());
    std::printf("Output:  %s\n", output_dir.c_str());
    std::printf("Found %zu ROMs\n\n", roms.size());

    int pass_count = 0;
    int fail_count = 0;
    int no_result = 0;

    for (const auto& rom_path : roms) {
        auto test_name = get_test_name(rom_path);
        std::printf("%-50s ", test_name.c_str());
        std::fflush(stdout);

        Emulator emu;
        if (!use_stub_bios && !bios_path.empty()) {
            emu.load_bios_from_file(bios_path);
        } else {
            emu.load_bios(make_bios_stub());
        }

        if (!emu.load_rom_from_file(rom_path)) {
            std::printf("[LOAD FAIL]\n");
            ++no_result;
            continue;
        }

        emu.reset();

        for (int f = 0; f < kRunFrames; ++f) {
            emu.run_frame();
        }

        auto& cpu = emu.cpu();
        auto& state = cpu.state();
        auto fb = emu.framebuffer();
        int text_px = count_text_pixels(fb);

        auto ppm_name = test_name;
        std::replace(ppm_name.begin(), ppm_name.end(), '/', '_');
        auto ppm_path = output_dir / (ppm_name + ".ppm");
        write_ppm(ppm_path.c_str(), fb);

        if (text_px < 10) {
            std::printf("[NO RESULT] text_px=%d PC=0x%08X\n", text_px, state.regs[15]);
            ++no_result;
        } else if (detect_pass(fb)) {
            std::printf("[PASS] text_px=%d\n", text_px);
            ++pass_count;
        } else {
            std::printf("[FAIL] text_px=%d\n", text_px);
            ++fail_count;
        }

        if (verbose) {
            std::printf("  PC=0x%08X CPSR=0x%08X halted=%d cycle=%llu\n",
                        state.regs[15], state.cpsr, state.halted,
                        (unsigned long long)cpu.current_cycle());
        }
    }

    std::printf("\n=== Summary ===\n");
    std::printf("PASS: %d  FAIL: %d  NO RESULT: %d  TOTAL: %zu\n",
                pass_count, fail_count, no_result, roms.size());
    std::printf("PPM images in: %s\n", output_dir.c_str());
    return 0;
}
