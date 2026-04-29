#include "gba/core/rom_provider.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "gba/core/constants.hpp"

namespace gba {

namespace {

constexpr std::size_t kRomPageSize = 32u * 1024u;
constexpr std::size_t kContainsChunkSize = 16u * 1024u;
constexpr std::size_t kDefaultDesktopProfilePages = (4u * 1024u * 1024u) / kRomPageSize;
constexpr u64 kDefaultMissPenaltyUs = 700;

[[nodiscard]] std::size_t parse_cache_pages_env() {
#ifdef GBA_PLATFORM_ESP32
    return 0;
#else
    const char* value = std::getenv("GBA_ROM_PROFILE_CACHE_PAGES");
    if (!value || *value == '\0') {
        return kDefaultDesktopProfilePages;
    }

    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed > std::numeric_limits<std::size_t>::max()) {
        return kDefaultDesktopProfilePages;
    }
    return static_cast<std::size_t>(parsed);
#endif
}

[[nodiscard]] u64 parse_miss_penalty_us_env() {
#ifdef GBA_PLATFORM_ESP32
    return kDefaultMissPenaltyUs;
#else
    const char* value = std::getenv("GBA_ROM_PROFILE_MISS_US");
    if (!value || *value == '\0') {
        return kDefaultMissPenaltyUs;
    }

    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value) {
        return kDefaultMissPenaltyUs;
    }
    return static_cast<u64>(parsed);
#endif
}

[[nodiscard]] u64 miss_penalty_cycles(u64 penalty_us, u64 misses) {
    return (misses * penalty_us * kSystemClockHz) / 1000000u;
}

}  // namespace

u16 RomProvider::read16(u32 address) const {
    return static_cast<u16>(static_cast<u32>(read_byte(address)) |
                            (static_cast<u32>(read_byte(address + 1u)) << 8u));
}

u32 RomProvider::read32(u32 address) const {
    return static_cast<u32>(read_byte(address)) |
           (static_cast<u32>(read_byte(address + 1u)) << 8u) |
           (static_cast<u32>(read_byte(address + 2u)) << 16u) |
           (static_cast<u32>(read_byte(address + 3u)) << 24u);
}

bool RomProvider::read_bytes(u32 address, std::span<u8> out) const {
    if (out.empty()) {
        return true;
    }
    if (static_cast<std::size_t>(address) + out.size() > size()) {
        return false;
    }
    for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = read_byte(address + static_cast<u32>(index));
    }
    return true;
}

bool RomProvider::contains(std::string_view needle) const {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > size()) {
        return false;
    }

    const auto* contiguous = contiguous_span().data();
    const auto span = contiguous_span();
    if (contiguous && !span.empty()) {
        return std::search(span.begin(), span.end(), needle.begin(), needle.end(), [](u8 lhs, char rhs) {
            return lhs == static_cast<u8>(rhs);
        }) != span.end();
    }

    std::vector<u8> buffer(kContainsChunkSize + needle.size() - 1u);
    std::size_t carry = 0;
    for (std::size_t offset = 0; offset < size();) {
        const auto chunk = std::min(kContainsChunkSize, size() - offset);
        if (!read_bytes(static_cast<u32>(offset), std::span<u8>(buffer.data() + carry, chunk))) {
            return false;
        }
        const auto valid = carry + chunk;
        if (std::search(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(valid),
                        needle.begin(), needle.end(), [](u8 lhs, char rhs) {
                            return lhs == static_cast<u8>(rhs);
                        }) != buffer.begin() + static_cast<std::ptrdiff_t>(valid)) {
            return true;
        }

        carry = std::min(needle.size() - 1u, valid);
        if (carry != 0) {
            std::memmove(buffer.data(), buffer.data() + valid - carry, carry);
        }
        offset += chunk;
    }
    return false;
}

void RomProvider::prefetch(u32, std::size_t) const {}

RomAccessStats RomProvider::frame_stats() const {
    return {};
}

void RomProvider::reset_frame_stats() const {}

MemoryRomProvider::MemoryRomProvider(std::vector<u8> rom, std::size_t profile_cache_pages)
    : rom_(std::move(rom)), profile_cache_pages_(profile_cache_pages) {
    if (profile_cache_pages_ != 0) {
        lru_pages_.reserve(profile_cache_pages_);
        unique_pages_.reserve(profile_cache_pages_);
    }
}

u8 MemoryRomProvider::read_byte(u32 address) const {
    touch_range(address, 1, false);
    return rom_[address];
}

u16 MemoryRomProvider::read16(u32 address) const {
    touch_range(address, 2, false);
    return static_cast<u16>(static_cast<u32>(rom_[address]) |
                            (static_cast<u32>(rom_[address + 1u]) << 8u));
}

u32 MemoryRomProvider::read32(u32 address) const {
    touch_range(address, 4, false);
    return static_cast<u32>(rom_[address]) |
           (static_cast<u32>(rom_[address + 1u]) << 8u) |
           (static_cast<u32>(rom_[address + 2u]) << 16u) |
           (static_cast<u32>(rom_[address + 3u]) << 24u);
}

bool MemoryRomProvider::read_bytes(u32 address, std::span<u8> out) const {
    if (out.empty()) {
        return true;
    }
    if (static_cast<std::size_t>(address) + out.size() > rom_.size()) {
        return false;
    }
    touch_range(address, out.size(), false);
    std::memcpy(out.data(), rom_.data() + address, out.size());
    return true;
}

void MemoryRomProvider::prefetch(u32 address, std::size_t bytes) const {
    touch_range(address, bytes, true);
}

RomAccessStats MemoryRomProvider::frame_stats() const {
    auto stats = frame_stats_;
    stats.unique_pages = static_cast<u32>(unique_pages_.size());
    stats.miss_penalty_us = stats.cache_misses * parse_miss_penalty_us_env();
    stats.miss_penalty_cycles = miss_penalty_cycles(parse_miss_penalty_us_env(), stats.cache_misses);
    return stats;
}

void MemoryRomProvider::reset_frame_stats() const {
    frame_stats_ = {};
    unique_pages_.clear();
    prefetched_pages_.clear();
    has_last_demand_ = false;
    last_demand_end_ = 0;
    sequential_run_ = 0;
    last_prefetch_base_page_ = std::numeric_limits<u32>::max();
}

std::size_t MemoryRomProvider::default_profile_cache_pages() {
    return parse_cache_pages_env();
}

void MemoryRomProvider::touch_range(u32 address, std::size_t bytes, bool prefetch) const {
    if (profile_cache_pages_ == 0 || bytes == 0) {
        if (!prefetch) {
            frame_stats_.byte_reads += bytes;
        }
        return;
    }

    const auto sequential = prefetch ? false : note_demand_access(address, bytes);
    if (prefetch) {
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += bytes;
    } else {
        frame_stats_.byte_reads += bytes;
    }

    const auto first_page = static_cast<u32>(address / kRomPageSize);
    const auto last_address = static_cast<u64>(address) + bytes - 1u;
    const auto last_page = static_cast<u32>(last_address / kRomPageSize);
    bool had_miss = false;
    for (u32 page = first_page; page <= last_page; ++page) {
        const bool hit = touch_page(page, prefetch, sequential);
        had_miss = had_miss || !hit;
    }
    if (!prefetch) {
        maybe_prefetch_after_read(address, bytes, sequential, had_miss);
    }
}

bool MemoryRomProvider::touch_page(u32 page, bool prefetch, bool sequential) const {
    if (!prefetch && std::find(unique_pages_.begin(), unique_pages_.end(), page) == unique_pages_.end()) {
        unique_pages_.push_back(page);
    }

    const auto it = std::find(lru_pages_.begin(), lru_pages_.end(), page);
    if (it != lru_pages_.end()) {
        if (!prefetch) {
            ++frame_stats_.cache_hits;
            if (sequential) {
                ++frame_stats_.sequential_hits;
            }
            const auto prefetch_it = std::find(prefetched_pages_.begin(), prefetched_pages_.end(), page);
            if (prefetch_it != prefetched_pages_.end()) {
                ++frame_stats_.prefetch_hits;
                prefetched_pages_.erase(prefetch_it);
            }
        }
        const auto value = *it;
        lru_pages_.erase(it);
        lru_pages_.push_back(value);
        return true;
    }

    if (prefetch) {
        ++frame_stats_.prefetch_misses;
        prefetched_pages_.push_back(page);
    } else {
        ++frame_stats_.cache_misses;
    }
    lru_pages_.push_back(page);
    if (lru_pages_.size() > profile_cache_pages_) {
        const auto evicted = lru_pages_.front();
        lru_pages_.erase(lru_pages_.begin());
        const auto prefetch_it = std::find(prefetched_pages_.begin(), prefetched_pages_.end(), evicted);
        if (prefetch_it != prefetched_pages_.end()) {
            prefetched_pages_.erase(prefetch_it);
        }
    }
    return false;
}

bool MemoryRomProvider::note_demand_access(u32 address, std::size_t bytes) const {
    const auto end = static_cast<u32>(static_cast<u64>(address) + bytes);
    const auto sequential = has_last_demand_ && address == last_demand_end_;
    sequential_run_ = sequential ? sequential_run_ + 1u : 0u;
    has_last_demand_ = true;
    last_demand_end_ = end;
    return sequential;
}

void MemoryRomProvider::maybe_prefetch_after_read(u32 address,
                                                  std::size_t bytes,
                                                  bool sequential,
                                                  bool had_miss) const {
    if ((!sequential && !had_miss) || bytes == 0) {
        return;
    }

    const auto end = static_cast<std::size_t>(address) + bytes;
    const auto current_page = static_cast<u32>((end == 0 ? 0 : end - 1u) / kRomPageSize);
    if (!had_miss && current_page == last_prefetch_base_page_) {
        return;
    }
    last_prefetch_base_page_ = current_page;

    const auto pages_ahead = sequential_run_ >= 4u ? 2u : 1u;
    const auto start_page = current_page + 1u;
    const auto end_page = start_page + pages_ahead;
    for (u32 page = start_page; page < end_page; ++page) {
        if (static_cast<std::size_t>(page) * kRomPageSize >= rom_.size()) {
            break;
        }
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += kRomPageSize;
        (void)touch_page(page, true, false);
    }
}

}  // namespace gba
