#pragma once

#include <cstddef>
#include <memory>

#include "gba/core/rom_provider.hpp"

namespace gba {

[[nodiscard]] std::unique_ptr<RomProvider> make_esp32_mmap_rom_provider(
    const char* partition_label,
    std::size_t max_size,
    std::size_t window_bytes = 64u * 1024u,
    std::size_t window_count = 16);

[[nodiscard]] std::unique_ptr<RomProvider> make_esp32_sd_cache_rom_provider(
    const char* path,
    std::size_t cache_bytes,
    std::size_t page_bytes = 32u * 1024u);

}  // namespace gba
