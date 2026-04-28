#include <SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gba/core/cartridge.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/emulator.hpp"

namespace {

using namespace gba;

struct Options {
    std::filesystem::path rom_path{};
    std::filesystem::path bios_path{"tests/assets/roms/gba_bios.bin"};
    std::optional<std::filesystem::path> save_path{};
    std::string save_type{"auto"};
    int scale = 3;
    u64 quit_after_frames = 0;
};

enum class ControlAction {
    None,
    Pause,
    Step,
    Reset,
};

enum class ControlShape {
    Rect,
    Circle,
};

struct Control {
    std::string label;
    SDL_Rect rect{};
    u16 mask = 0;
    ControlAction action = ControlAction::None;
    ControlShape shape = ControlShape::Rect;
};

struct Layout {
    SDL_Rect screen{};
    SDL_Rect panel{};
    int window_w = 0;
    int window_h = 0;
    std::vector<Control> controls;
};

void print_usage(const char* argv0) {
    std::fprintf(
        stderr,
        "Usage: %s --rom <path> [--bios <path>] [--save <path>] [--save-type auto|flash128|flash64|sram|eeprom|none] [--scale 2..5] [--frames <count>]\n",
        argv0
    );
}

bool parse_int(std::string_view text, int& value) {
    char* end = nullptr;
    const auto parsed = std::strtol(text.data(), &end, 10);
    if (end == text.data() || *end != '\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parse_u64(std::string_view text, u64& value) {
    char* end = nullptr;
    const auto parsed = std::strtoull(text.data(), &end, 10);
    if (end == text.data() || *end != '\0') {
        return false;
    }
    value = static_cast<u64>(parsed);
    return true;
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg{argv[index]};
        const auto require_value = [&](std::string_view name) -> const char* {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %.*s\n", static_cast<int>(name.size()), name.data());
                return nullptr;
            }
            ++index;
            return argv[index];
        };

        if (arg == "--rom") {
            const auto* value = require_value(arg);
            if (value == nullptr) {
                return false;
            }
            options.rom_path = value;
        } else if (arg == "--bios") {
            const auto* value = require_value(arg);
            if (value == nullptr) {
                return false;
            }
            options.bios_path = value;
        } else if (arg == "--save") {
            const auto* value = require_value(arg);
            if (value == nullptr) {
                return false;
            }
            options.save_path = std::filesystem::path(value);
        } else if (arg == "--save-type") {
            const auto* value = require_value(arg);
            if (value == nullptr) {
                return false;
            }
            options.save_type = value;
        } else if (arg == "--scale") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_int(value, options.scale)) {
                std::fprintf(stderr, "Invalid value for --scale\n");
                return false;
            }
        } else if (arg == "--frames") {
            const auto* value = require_value(arg);
            if (value == nullptr || !parse_u64(value, options.quit_after_frames)) {
                std::fprintf(stderr, "Invalid value for --frames\n");
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[index]);
            return false;
        }
    }

    if (options.rom_path.empty()) {
        std::fprintf(stderr, "--rom is required\n");
        return false;
    }

    options.scale = std::clamp(options.scale, 2, 5);
    return true;
}

std::vector<u8> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool write_binary_file(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

std::filesystem::path default_save_path(const std::filesystem::path& rom_path) {
    auto save_path = rom_path;
    save_path.replace_extension(".sav");
    return save_path;
}

std::optional<SaveType> parse_save_type(const std::string& text) {
    if (text == "none") {
        return SaveType::None;
    }
    if (text == "sram") {
        return SaveType::Sram;
    }
    if (text == "flash64") {
        return SaveType::Flash64K;
    }
    if (text == "flash128") {
        return SaveType::Flash128K;
    }
    if (text == "eeprom") {
        return SaveType::Eeprom;
    }
    return std::nullopt;
}

const char* save_type_name(SaveType save_type) {
    switch (save_type) {
    case SaveType::None:
        return "NONE";
    case SaveType::Sram:
        return "SRAM";
    case SaveType::Flash64K:
        return "FLASH64";
    case SaveType::Flash128K:
        return "FLASH128";
    case SaveType::Eeprom:
        return "EEPROM";
    }
    return "UNKNOWN";
}

std::array<const char*, 7> glyph_rows(char ch) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
    case 'A': return {"01110", "10001", "10001", "11111", "10001", "10001", "10001"};
    case 'B': return {"11110", "10001", "10001", "11110", "10001", "10001", "11110"};
    case 'C': return {"01111", "10000", "10000", "10000", "10000", "10000", "01111"};
    case 'D': return {"11110", "10001", "10001", "10001", "10001", "10001", "11110"};
    case 'E': return {"11111", "10000", "10000", "11110", "10000", "10000", "11111"};
    case 'F': return {"11111", "10000", "10000", "11110", "10000", "10000", "10000"};
    case 'G': return {"01111", "10000", "10000", "10111", "10001", "10001", "01110"};
    case 'H': return {"10001", "10001", "10001", "11111", "10001", "10001", "10001"};
    case 'I': return {"11111", "00100", "00100", "00100", "00100", "00100", "11111"};
    case 'J': return {"00111", "00010", "00010", "00010", "10010", "10010", "01100"};
    case 'K': return {"10001", "10010", "10100", "11000", "10100", "10010", "10001"};
    case 'L': return {"10000", "10000", "10000", "10000", "10000", "10000", "11111"};
    case 'M': return {"10001", "11011", "10101", "10101", "10001", "10001", "10001"};
    case 'N': return {"10001", "11001", "10101", "10011", "10001", "10001", "10001"};
    case 'O': return {"01110", "10001", "10001", "10001", "10001", "10001", "01110"};
    case 'P': return {"11110", "10001", "10001", "11110", "10000", "10000", "10000"};
    case 'Q': return {"01110", "10001", "10001", "10001", "10101", "10010", "01101"};
    case 'R': return {"11110", "10001", "10001", "11110", "10100", "10010", "10001"};
    case 'S': return {"01111", "10000", "10000", "01110", "00001", "00001", "11110"};
    case 'T': return {"11111", "00100", "00100", "00100", "00100", "00100", "00100"};
    case 'U': return {"10001", "10001", "10001", "10001", "10001", "10001", "01110"};
    case 'V': return {"10001", "10001", "10001", "10001", "10001", "01010", "00100"};
    case 'W': return {"10001", "10001", "10001", "10101", "10101", "10101", "01010"};
    case 'X': return {"10001", "10001", "01010", "00100", "01010", "10001", "10001"};
    case 'Y': return {"10001", "10001", "01010", "00100", "00100", "00100", "00100"};
    case 'Z': return {"11111", "00001", "00010", "00100", "01000", "10000", "11111"};
    case '0': return {"01110", "10001", "10011", "10101", "11001", "10001", "01110"};
    case '1': return {"00100", "01100", "00100", "00100", "00100", "00100", "01110"};
    case '2': return {"01110", "10001", "00001", "00010", "00100", "01000", "11111"};
    case '3': return {"11110", "00001", "00001", "01110", "00001", "00001", "11110"};
    case '4': return {"00010", "00110", "01010", "10010", "11111", "00010", "00010"};
    case '5': return {"11111", "10000", "10000", "11110", "00001", "00001", "11110"};
    case '6': return {"01110", "10000", "10000", "11110", "10001", "10001", "01110"};
    case '7': return {"11111", "00001", "00010", "00100", "01000", "01000", "01000"};
    case '8': return {"01110", "10001", "10001", "01110", "10001", "10001", "01110"};
    case '9': return {"01110", "10001", "10001", "01111", "00001", "00001", "01110"};
    case ':': return {"00000", "00100", "00100", "00000", "00100", "00100", "00000"};
    case '.': return {"00000", "00000", "00000", "00000", "00000", "01100", "01100"};
    case '-': return {"00000", "00000", "00000", "11111", "00000", "00000", "00000"};
    case '/': return {"00001", "00010", "00010", "00100", "01000", "01000", "10000"};
    case '_': return {"00000", "00000", "00000", "00000", "00000", "00000", "11111"};
    case ' ': return {"00000", "00000", "00000", "00000", "00000", "00000", "00000"};
    default: return {"01110", "10001", "00010", "00100", "00100", "00000", "00100"};
    }
}

void set_color(SDL_Renderer* renderer, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void fill_rect(SDL_Renderer* renderer, SDL_Rect rect, SDL_Color color) {
    set_color(renderer, color);
    SDL_RenderFillRect(renderer, &rect);
}

void draw_rect(SDL_Renderer* renderer, SDL_Rect rect, SDL_Color color) {
    set_color(renderer, color);
    SDL_RenderDrawRect(renderer, &rect);
}

void draw_text(SDL_Renderer* renderer, int x, int y, std::string_view text, int scale, SDL_Color color) {
    set_color(renderer, color);
    int cursor_x = x;
    for (const char raw_ch : text) {
        const auto rows = glyph_rows(raw_ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(col)] == '1') {
                    SDL_Rect pixel{
                        cursor_x + (col * scale),
                        y + (row * scale),
                        scale,
                        scale,
                    };
                    SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
        cursor_x += 6 * scale;
    }
}

int text_width(std::string_view text, int scale) {
    return static_cast<int>(text.size()) * 6 * scale;
}

void draw_centered_text(SDL_Renderer* renderer, SDL_Rect rect, std::string_view text, int scale, SDL_Color color) {
    const auto width = text_width(text, scale);
    const auto height = 7 * scale;
    draw_text(renderer, rect.x + ((rect.w - width) / 2), rect.y + ((rect.h - height) / 2), text, scale, color);
}

void fill_circle(SDL_Renderer* renderer, int cx, int cy, int radius, SDL_Color color) {
    set_color(renderer, color);
    const auto r2 = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if ((x * x) + (y * y) <= r2) {
                SDL_RenderDrawPoint(renderer, cx + x, cy + y);
            }
        }
    }
}

void draw_circle(SDL_Renderer* renderer, int cx, int cy, int radius, SDL_Color color) {
    set_color(renderer, color);
    const auto outer = radius * radius;
    const auto inner = (radius - 2) * (radius - 2);
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const auto d2 = (x * x) + (y * y);
            if (d2 <= outer && d2 >= inner) {
                SDL_RenderDrawPoint(renderer, cx + x, cy + y);
            }
        }
    }
}

Layout make_layout(int scale) {
    Layout layout{};
    layout.screen = SDL_Rect{16, 16, static_cast<int>(kScreenWidth) * scale, static_cast<int>(kScreenHeight) * scale};
    layout.window_h = std::max(layout.screen.h + 32, 520);
    layout.window_w = layout.screen.w + 332;
    layout.panel = SDL_Rect{layout.screen.x + layout.screen.w + 16, 16, 284, layout.window_h - 32};

    const auto px = layout.panel.x;
    const auto py = layout.panel.y;
    const auto add = [&](std::string label, SDL_Rect rect, u16 mask, ControlShape shape = ControlShape::Rect) {
        layout.controls.push_back(Control{std::move(label), rect, mask, ControlAction::None, shape});
    };
    const auto action = [&](std::string label, SDL_Rect rect, ControlAction control_action) {
        layout.controls.push_back(Control{std::move(label), rect, 0, control_action, ControlShape::Rect});
    };

    add("L", SDL_Rect{px + 18, py + 54, 106, 34}, kKeyL);
    add("R", SDL_Rect{px + 160, py + 54, 106, 34}, kKeyR);

    const int dpad_x = px + 24;
    const int dpad_y = py + 154;
    add("UP", SDL_Rect{dpad_x + 52, dpad_y, 52, 48}, kKeyUp);
    add("LEFT", SDL_Rect{dpad_x, dpad_y + 50, 76, 48}, kKeyLeft);
    add("RIGHT", SDL_Rect{dpad_x + 80, dpad_y + 50, 76, 48}, kKeyRight);
    add("DOWN", SDL_Rect{dpad_x + 52, dpad_y + 100, 52, 48}, kKeyDown);

    add("B", SDL_Rect{px + 172, py + 212, 62, 62}, kKeyB, ControlShape::Circle);
    add("A", SDL_Rect{px + 220, py + 160, 66, 66}, kKeyA, ControlShape::Circle);

    add("SELECT", SDL_Rect{px + 28, py + 336, 92, 34}, kKeySelect);
    add("START", SDL_Rect{px + 152, py + 336, 92, 34}, kKeyStart);

    const int action_y = layout.panel.y + layout.panel.h - 48;
    action("PAUSE", SDL_Rect{px + 18, action_y, 78, 34}, ControlAction::Pause);
    action("STEP", SDL_Rect{px + 104, action_y, 70, 34}, ControlAction::Step);
    action("RESET", SDL_Rect{px + 184, action_y, 82, 34}, ControlAction::Reset);

    return layout;
}

bool point_in_rect(int x, int y, const SDL_Rect& rect) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

u16 controls_at(const Layout& layout, int x, int y) {
    u16 mask = 0;
    for (const auto& control : layout.controls) {
        if (control.mask != 0 && point_in_rect(x, y, control.rect)) {
            mask = static_cast<u16>(mask | control.mask);
        }
    }
    return mask;
}

const Control* action_at(const Layout& layout, int x, int y) {
    for (const auto& control : layout.controls) {
        if (control.action != ControlAction::None && point_in_rect(x, y, control.rect)) {
            return &control;
        }
    }
    return nullptr;
}

u16 key_mask_for_scancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_RIGHT:
        return kKeyRight;
    case SDL_SCANCODE_LEFT:
        return kKeyLeft;
    case SDL_SCANCODE_UP:
        return kKeyUp;
    case SDL_SCANCODE_DOWN:
        return kKeyDown;
    case SDL_SCANCODE_X:
    case SDL_SCANCODE_J:
        return kKeyA;
    case SDL_SCANCODE_Z:
    case SDL_SCANCODE_K:
        return kKeyB;
    case SDL_SCANCODE_A:
        return kKeyL;
    case SDL_SCANCODE_S:
        return kKeyR;
    case SDL_SCANCODE_RETURN:
        return kKeyStart;
    case SDL_SCANCODE_BACKSPACE:
    case SDL_SCANCODE_RSHIFT:
        return kKeySelect;
    default:
        return 0;
    }
}

u16 controller_button_mask(SDL_GameControllerButton button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return kKeyA;
    case SDL_CONTROLLER_BUTTON_B:
        return kKeyB;
    case SDL_CONTROLLER_BUTTON_BACK:
        return kKeySelect;
    case SDL_CONTROLLER_BUTTON_START:
        return kKeyStart;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return kKeyRight;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return kKeyLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return kKeyUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return kKeyDown;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return kKeyR;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return kKeyL;
    default:
        return 0;
    }
}

SDL_GameController* open_first_controller() {
    const auto count = SDL_NumJoysticks();
    for (int index = 0; index < count; ++index) {
        if (SDL_IsGameController(index) == SDL_TRUE) {
            auto* controller = SDL_GameControllerOpen(index);
            if (controller != nullptr) {
                return controller;
            }
        }
    }
    return nullptr;
}

u16 controller_axis_mask(SDL_GameController* controller) {
    if (controller == nullptr) {
        return 0;
    }
    constexpr Sint16 kThreshold = 12000;
    u16 mask = 0;
    const auto x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    const auto y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    if (x <= -kThreshold) {
        mask = static_cast<u16>(mask | kKeyLeft);
    } else if (x >= kThreshold) {
        mask = static_cast<u16>(mask | kKeyRight);
    }
    if (y <= -kThreshold) {
        mask = static_cast<u16>(mask | kKeyUp);
    } else if (y >= kThreshold) {
        mask = static_cast<u16>(mask | kKeyDown);
    }
    return mask;
}

void update_texture(SDL_Texture* texture, std::span<const u16> framebuffer, std::vector<u32>& pixels) {
    for (std::size_t index = 0; index < framebuffer.size(); ++index) {
        const auto pixel = framebuffer[index];
        const auto r5 = static_cast<u8>(pixel & 0x1Fu);
        const auto g5 = static_cast<u8>((pixel >> 5u) & 0x1Fu);
        const auto b5 = static_cast<u8>((pixel >> 10u) & 0x1Fu);
        const auto r8 = static_cast<u32>((r5 << 3u) | (r5 >> 2u));
        const auto g8 = static_cast<u32>((g5 << 3u) | (g5 >> 2u));
        const auto b8 = static_cast<u32>((b5 << 3u) | (b5 >> 2u));
        pixels[index] = 0xFF000000u | (r8 << 16u) | (g8 << 8u) | b8;
    }
    SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(kScreenWidth * sizeof(u32)));
}

std::string hex_value(u32 value, int width) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

void draw_control(SDL_Renderer* renderer, const Control& control, bool active, bool paused) {
    const SDL_Color inactive_fill{42, 48, 58, 255};
    const SDL_Color active_fill{236, 190, 61, 255};
    const SDL_Color action_fill{54, 69, 85, 255};
    const SDL_Color border{170, 183, 198, 255};
    const SDL_Color text{238, 244, 250, 255};
    const SDL_Color active_text{22, 24, 29, 255};
    const auto is_action = control.action != ControlAction::None;
    auto fill = active ? active_fill : (is_action ? action_fill : inactive_fill);
    if (control.action == ControlAction::Pause && paused) {
        fill = SDL_Color{68, 186, 140, 255};
    }

    if (control.shape == ControlShape::Circle) {
        const auto cx = control.rect.x + (control.rect.w / 2);
        const auto cy = control.rect.y + (control.rect.h / 2);
        const auto radius = std::min(control.rect.w, control.rect.h) / 2;
        fill_circle(renderer, cx, cy, radius, fill);
        draw_circle(renderer, cx, cy, radius, border);
    } else {
        fill_rect(renderer, control.rect, fill);
        draw_rect(renderer, control.rect, border);
    }

    const auto label_scale = control.label.size() > 5 ? 2 : 3;
    draw_centered_text(renderer, control.rect, control.label, label_scale, active ? active_text : text);
}

void render_ui(SDL_Renderer* renderer,
               SDL_Texture* texture,
               const Layout& layout,
               Emulator& emulator,
               u64 frame,
               double fps,
               u16 active_mask,
               bool paused,
               bool turbo,
               const std::filesystem::path& save_path) {
    const SDL_Color background{18, 21, 26, 255};
    const SDL_Color screen_border{100, 115, 132, 255};
    const SDL_Color panel_fill{28, 32, 40, 255};
    const SDL_Color text{230, 236, 242, 255};
    const SDL_Color muted{145, 158, 176, 255};
    const SDL_Color accent{87, 201, 196, 255};

    fill_rect(renderer, SDL_Rect{0, 0, layout.window_w, layout.window_h}, background);
    SDL_RenderCopy(renderer, texture, nullptr, &layout.screen);
    draw_rect(renderer, SDL_Rect{layout.screen.x - 1, layout.screen.y - 1, layout.screen.w + 2, layout.screen.h + 2},
              screen_border);

    fill_rect(renderer, layout.panel, panel_fill);
    draw_rect(renderer, layout.panel, screen_border);

    const auto px = layout.panel.x;
    int y = layout.panel.y + 16;
    draw_text(renderer, px + 16, y, "GBA TEST PAD", 3, accent);
    y += 42;
    draw_text(renderer, px + 16, y, "KEYS X A  Z B", 2, muted);
    y += 20;
    draw_text(renderer, px + 16, y, "ARROWS DPAD", 2, muted);

    for (const auto& control : layout.controls) {
        const auto active = control.mask != 0 && (active_mask & control.mask) != 0;
        draw_control(renderer, control, active, paused);
    }

    const auto& cpu = emulator.cpu();
    const auto& state = cpu.state();
    const auto& irq = emulator.irq();
    int sy = layout.panel.y + 390;
    draw_text(renderer, px + 16, sy, paused ? "PAUSED" : (turbo ? "TURBO" : "RUNNING"), 2,
              paused ? SDL_Color{68, 186, 140, 255} : accent);
    sy += 20;
    draw_text(renderer, px + 16, sy, "FPS " + std::to_string(static_cast<int>(fps + 0.5)), 2, text);
    sy += 18;
    draw_text(renderer, px + 16, sy, "FRAME " + std::to_string(frame), 2, text);
    sy += 18;
    draw_text(renderer, px + 16, sy, "PC " + hex_value(state.regs[15], 8), 2, text);
    sy += 18;
    draw_text(renderer, px + 16, sy, "IE " + hex_value(irq.ie(), 4) + " IF " + hex_value(irq.iflags(), 4), 2, text);
    sy += 18;
    draw_text(renderer, px + 16, sy, "SAVE " + save_path.filename().string(), 1, muted);
}

bool load_save_if_present(Emulator& emulator, const std::filesystem::path& save_path) {
    if (!std::filesystem::exists(save_path)) {
        return true;
    }
    auto save = read_binary_file(save_path);
    if (save.empty()) {
        std::fprintf(stderr, "Failed to read save file: %s\n", save_path.c_str());
        return false;
    }
    emulator.cartridge().load_save(std::move(save));
    return true;
}

bool flush_save_if_dirty(Emulator& emulator, const std::filesystem::path& save_path) {
    auto& cartridge = emulator.cartridge();
    if (!cartridge.is_save_dirty()) {
        return true;
    }
    if (!write_binary_file(save_path, cartridge.save())) {
        std::fprintf(stderr, "Failed to write save file: %s\n", save_path.c_str());
        return false;
    }
    cartridge.clear_save_dirty();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse_args(argc, argv, options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    Emulator emulator;
    if (!emulator.load_bios_from_file(options.bios_path)) {
        std::fprintf(stderr, "Failed to load BIOS from %s\n", options.bios_path.c_str());
        return EXIT_FAILURE;
    }
    if (!emulator.load_rom_from_file(options.rom_path)) {
        std::fprintf(stderr, "Failed to load ROM from %s\n", options.rom_path.c_str());
        return EXIT_FAILURE;
    }

    if (options.save_type == "auto") {
        emulator.cartridge().auto_detect_save_type();
    } else if (const auto save_type = parse_save_type(options.save_type)) {
        emulator.set_save_type(*save_type);
    } else {
        std::fprintf(stderr, "Unknown save type: %s\n", options.save_type.c_str());
        return EXIT_FAILURE;
    }

    const auto save_path = options.save_path.value_or(default_save_path(options.rom_path));
    if (!load_save_if_present(emulator, save_path)) {
        return EXIT_FAILURE;
    }

    emulator.reset();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    const auto layout = make_layout(options.scale);

    SDL_Window* window = SDL_CreateWindow(
        "GBA SDL Frontend",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        layout.window_w,
        layout.window_h,
        SDL_WINDOW_SHOWN
    );
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == nullptr) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Texture* texture = SDL_CreateTexture(renderer,
                                            SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            static_cast<int>(kScreenWidth),
                                            static_cast<int>(kScreenHeight));
    if (texture == nullptr) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GameController* controller = open_first_controller();
    std::vector<u32> pixels(kFramebufferPixels);

    bool running = true;
    bool paused = false;
    bool step_requested = false;
    bool mouse_down = false;
    bool turbo = false;
    u16 keyboard_mask = 0;
    u16 mouse_mask = 0;
    u16 controller_mask = 0;
    u64 frame = 0;
    u32 frames_this_second = 0;
    double fps = 0.0;

    auto last_second = std::chrono::steady_clock::now();
    auto last_save_flush = last_second;
    auto next_frame_time = last_second;
    constexpr auto kFrameDuration = std::chrono::duration<double>(1.0 / 59.727500569606);

    std::cout << "Loaded ROM: " << options.rom_path << '\n';
    std::cout << "BIOS: " << options.bios_path << '\n';
    std::cout << "Save: " << save_path << " (" << save_type_name(emulator.cartridge().save_type()) << ")\n";
    std::cout << "Keyboard: arrows=dpad, X/J=A, Z/K=B, A=L, S=R, Enter=Start, Backspace=Select, P=pause, N=step, R=reset\n";

    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (controller == nullptr) {
                    controller = open_first_controller();
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (controller != nullptr &&
                    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
                    SDL_GameControllerClose(controller);
                    controller = nullptr;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                const auto mask =
                    controller_button_mask(static_cast<SDL_GameControllerButton>(event.cbutton.button));
                if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                    controller_mask = static_cast<u16>(controller_mask | mask);
                } else {
                    controller_mask = static_cast<u16>(controller_mask & ~mask);
                }
                break;
            }
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                const auto mask = key_mask_for_scancode(event.key.keysym.scancode);
                if (event.type == SDL_KEYDOWN) {
                    keyboard_mask = static_cast<u16>(keyboard_mask | mask);
                    if (event.key.repeat == 0) {
                        switch (event.key.keysym.scancode) {
                        case SDL_SCANCODE_ESCAPE:
                            running = false;
                            break;
                        case SDL_SCANCODE_P:
                            paused = !paused;
                            break;
                        case SDL_SCANCODE_N:
                            step_requested = true;
                            break;
                        case SDL_SCANCODE_R:
                            emulator.reset();
                            frame = 0;
                            break;
                        default:
                            break;
                        }
                    }
                    if (event.key.keysym.scancode == SDL_SCANCODE_TAB) {
                        turbo = true;
                    }
                } else {
                    keyboard_mask = static_cast<u16>(keyboard_mask & ~mask);
                    if (event.key.keysym.scancode == SDL_SCANCODE_TAB) {
                        turbo = false;
                    }
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    if (const auto* control = action_at(layout, event.button.x, event.button.y)) {
                        switch (control->action) {
                        case ControlAction::Pause:
                            paused = !paused;
                            break;
                        case ControlAction::Step:
                            step_requested = true;
                            break;
                        case ControlAction::Reset:
                            emulator.reset();
                            frame = 0;
                            break;
                        case ControlAction::None:
                            break;
                        }
                    }
                    mouse_down = true;
                    mouse_mask = controls_at(layout, event.button.x, event.button.y);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_down = false;
                    mouse_mask = 0;
                }
                break;
            case SDL_MOUSEMOTION:
                if (mouse_down) {
                    mouse_mask = controls_at(layout, event.motion.x, event.motion.y);
                }
                break;
            default:
                break;
            }
        }

        const auto active_mask =
            static_cast<u16>(keyboard_mask | mouse_mask | controller_mask | controller_axis_mask(controller));
        emulator.set_keys(active_mask);

        const auto frames_to_run = turbo ? 4 : 1;
        if (!paused || step_requested) {
            for (int index = 0; index < frames_to_run; ++index) {
                emulator.run_frame();
                ++frame;
                ++frames_this_second;
                if (paused) {
                    break;
                }
            }
            step_requested = false;
        }

        update_texture(texture, emulator.framebuffer(), pixels);
        render_ui(renderer, texture, layout, emulator, frame, fps, active_mask, paused, turbo, save_path);
        SDL_RenderPresent(renderer);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_second >= std::chrono::seconds(1)) {
            const auto elapsed = std::chrono::duration<double>(now - last_second).count();
            fps = static_cast<double>(frames_this_second) / elapsed;
            frames_this_second = 0;
            last_second = now;
            std::ostringstream title;
            title << "GBA SDL Frontend - FPS " << static_cast<int>(fps + 0.5)
                  << " - PC 0x" << hex_value(emulator.cpu().state().regs[15], 8);
            SDL_SetWindowTitle(window, title.str().c_str());
        }

        if (now - last_save_flush >= std::chrono::seconds(2)) {
            flush_save_if_dirty(emulator, save_path);
            last_save_flush = now;
        }

        if (options.quit_after_frames != 0 && frame >= options.quit_after_frames) {
            running = false;
        }

        if (!turbo) {
            next_frame_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(kFrameDuration);
            const auto sleep_until = next_frame_time;
            const auto before_sleep = std::chrono::steady_clock::now();
            if (sleep_until > before_sleep) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sleep_until - before_sleep);
                if (ms.count() > 1) {
                    SDL_Delay(static_cast<Uint32>(ms.count() - 1));
                }
            } else if (before_sleep - sleep_until > std::chrono::milliseconds(250)) {
                next_frame_time = before_sleep;
            }
        } else {
            next_frame_time = std::chrono::steady_clock::now();
        }
    }

    flush_save_if_dirty(emulator, save_path);

    if (controller != nullptr) {
        SDL_GameControllerClose(controller);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}
