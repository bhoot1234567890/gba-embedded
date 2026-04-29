#include "gba/core/rom_provider.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace gba {

namespace {

constexpr std::size_t kRomPageSize = 32u * 1024u;
constexpr std::size_t kContainsChunkSize = 16u * 1024u;
constexpr std::size_t kDefaultDesktopProfilePages = (4u * 1024u * 1024u) / kRomPageSize;

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

}  // namespace

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
    if (static_cast<std::size_t>(address) >= rom_.size()) {
        return 0xFFu;
    }
    touch_range(address, 1, false);
    return rom_[address];
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
    return stats;
}

void MemoryRomProvider::reset_frame_stats() const {
    frame_stats_ = {};
    unique_pages_.clear();
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

    if (prefetch) {
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += bytes;
    } else {
        frame_stats_.byte_reads += bytes;
    }

    const auto first_page = static_cast<u32>(address / kRomPageSize);
    const auto last_address = static_cast<u64>(address) + bytes - 1u;
    const auto last_page = static_cast<u32>(last_address / kRomPageSize);
    for (u32 page = first_page; page <= last_page; ++page) {
        touch_page(page, prefetch);
    }
}

void MemoryRomProvider::touch_page(u32 page, bool) const {
    if (std::find(unique_pages_.begin(), unique_pages_.end(), page) == unique_pages_.end()) {
        unique_pages_.push_back(page);
    }

    const auto it = std::find(lru_pages_.begin(), lru_pages_.end(), page);
    if (it != lru_pages_.end()) {
        ++frame_stats_.cache_hits;
        const auto value = *it;
        lru_pages_.erase(it);
        lru_pages_.push_back(value);
        return;
    }

    ++frame_stats_.cache_misses;
    lru_pages_.push_back(page);
    if (lru_pages_.size() > profile_cache_pages_) {
        lru_pages_.erase(lru_pages_.begin());
    }
}

}  // namespace gba
