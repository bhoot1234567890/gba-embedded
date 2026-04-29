#include "gba/platform/esp32_rom_provider.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "gba/core/constants.hpp"

#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"

namespace gba {

namespace {

constexpr u32 kInvalidPage = std::numeric_limits<u32>::max();
constexpr u64 kSdMissPenaltyUs = 700;
constexpr u32 kHotPinThreshold = 96;
constexpr std::size_t kSdReadAlignment = 512;
const char* kTag = "rom_provider";

struct HeapCapsDeleter {
    void operator()(u8* ptr) const {
        if (ptr) {
            heap_caps_free(ptr);
        }
    }
};

[[nodiscard]] u8* alloc_rom_cache(std::size_t bytes) {
    auto* ptr = static_cast<u8*>(
        heap_caps_aligned_alloc(kSdReadAlignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ptr) {
        ptr = static_cast<u8*>(heap_caps_aligned_alloc(kSdReadAlignment, bytes, MALLOC_CAP_8BIT));
    }
    return ptr;
}

[[nodiscard]] u64 miss_penalty_cycles(u64 misses) {
    return (misses * kSdMissPenaltyUs * kSystemClockHz) / 1000000u;
}

[[nodiscard]] u16 IRAM_ATTR load_le16(const u8* ptr) {
    return static_cast<u16>(static_cast<u32>(ptr[0]) | (static_cast<u32>(ptr[1]) << 8u));
}

[[nodiscard]] u32 IRAM_ATTR load_le32(const u8* ptr) {
    return static_cast<u32>(ptr[0]) |
           (static_cast<u32>(ptr[1]) << 8u) |
           (static_cast<u32>(ptr[2]) << 16u) |
           (static_cast<u32>(ptr[3]) << 24u);
}

class Esp32MmapRomProvider final : public RomProvider {
public:
    Esp32MmapRomProvider(const esp_partition_t* partition,
                         std::size_t size,
                         std::size_t window_bytes,
                         std::size_t window_count)
        : partition_(partition),
          size_(size),
          window_bytes_(std::max<std::size_t>(4096u, window_bytes)),
          windows_(std::max<std::size_t>(1u, window_count)),
          window_slots_((size + window_bytes_ - 1u) / window_bytes_, -1) {}

    ~Esp32MmapRomProvider() override {
        for (auto& window : windows_) {
            if (window.valid) {
                esp_partition_munmap(window.handle);
            }
        }
    }

    [[nodiscard]] std::size_t size() const override { return size_; }

    [[nodiscard]] u8 IRAM_ATTR read_byte(u32 address) const override {
        if (static_cast<std::size_t>(address) >= size_) {
            return 0xFFu;
        }
        const auto* ptr = read_ptr(address, 1);
        return ptr ? ptr[0] : 0xFFu;
    }

    [[nodiscard]] u16 IRAM_ATTR read16(u32 address) const override {
        if (static_cast<std::size_t>(address) + 2u > size_) {
            return 0xFFFFu;
        }
        if ((static_cast<std::size_t>(address) % window_bytes_) + 2u > window_bytes_) {
            std::array<u8, 2> scratch{};
            return read_bytes(address, scratch) ? load_le16(scratch.data()) : 0xFFFFu;
        }
        const auto* ptr = read_ptr(address, 2);
        if (!ptr) {
            return 0xFFFFu;
        }
        return load_le16(ptr);
    }

    [[nodiscard]] u32 IRAM_ATTR read32(u32 address) const override {
        if (static_cast<std::size_t>(address) + 4u > size_) {
            return 0xFFFFFFFFu;
        }
        if ((static_cast<std::size_t>(address) % window_bytes_) + 4u > window_bytes_) {
            std::array<u8, 4> scratch{};
            return read_bytes(address, scratch) ? load_le32(scratch.data()) : 0xFFFFFFFFu;
        }
        const auto* ptr = read_ptr(address, 4);
        if (!ptr) {
            return 0xFFFFFFFFu;
        }
        return load_le32(ptr);
    }

    [[nodiscard]] bool read_bytes(u32 address, std::span<u8> out) const override {
        if (out.empty()) {
            return true;
        }
        if (static_cast<std::size_t>(address) + out.size() > size_) {
            return false;
        }

        const auto sequential = note_demand_access(address, out.size());
        frame_stats_.byte_reads += out.size();
        std::size_t copied = 0;
        while (copied < out.size()) {
            const auto absolute = static_cast<std::size_t>(address) + copied;
            const auto page = static_cast<u32>(absolute / window_bytes_);
            const auto offset_in_window = absolute % window_bytes_;
            const auto chunk = std::min(out.size() - copied, window_bytes_ - offset_in_window);
            const auto* window = ensure_window(page, false, sequential);
            if (!window) {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(copied),
                          out.begin() + static_cast<std::ptrdiff_t>(copied + chunk),
                          0xFFu);
            } else {
                std::memcpy(out.data() + copied, window + offset_in_window, chunk);
            }
            copied += chunk;
        }
        return true;
    }

    void prefetch(u32 address, std::size_t bytes) const override {
        if (bytes == 0 || static_cast<std::size_t>(address) >= size_) {
            return;
        }
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += bytes;
        const auto end = std::min<std::size_t>(size_, static_cast<std::size_t>(address) + bytes);
        for (auto cursor = static_cast<std::size_t>(address); cursor < end; cursor = ((cursor / window_bytes_) + 1u) * window_bytes_) {
            (void)ensure_window(static_cast<u32>(cursor / window_bytes_), true, false);
        }
    }

    [[nodiscard]] RomAccessStats frame_stats() const override {
        auto stats = frame_stats_;
        stats.unique_pages = static_cast<u32>(unique_pages_.size());
        return stats;
    }

    void reset_frame_stats() const override {
        frame_stats_ = {};
        unique_pages_.clear();
        has_last_demand_ = false;
        last_demand_end_ = 0;
        for (auto& window : windows_) {
            window.prefetched = false;
        }
    }

private:
    struct Window {
        bool valid = false;
        bool prefetched = false;
        u32 page = kInvalidPage;
        const u8* data = nullptr;
        std::size_t mapped_size = 0;
        u64 age = 0;
        esp_partition_mmap_handle_t handle = 0;
    };

    [[nodiscard]] const u8* IRAM_ATTR read_ptr(u32 address, std::size_t bytes) const {
        const auto sequential = note_demand_access(address, bytes);
        frame_stats_.byte_reads += bytes;
        const auto absolute = static_cast<std::size_t>(address);
        const auto page = static_cast<u32>(absolute / window_bytes_);
        const auto offset_in_window = absolute % window_bytes_;
        if (offset_in_window + bytes > window_bytes_) {
            return nullptr;
        }
        const auto* window = ensure_window(page, false, sequential);
        return window ? window + offset_in_window : nullptr;
    }

    [[nodiscard]] bool note_demand_access(u32 address, std::size_t bytes) const {
        const auto page = static_cast<u32>(static_cast<std::size_t>(address) / window_bytes_);
        if (std::find(unique_pages_.begin(), unique_pages_.end(), page) == unique_pages_.end()) {
            unique_pages_.push_back(page);
        }
        const auto end = static_cast<u32>(static_cast<u64>(address) + bytes);
        const auto sequential = has_last_demand_ && address == last_demand_end_;
        has_last_demand_ = true;
        last_demand_end_ = end;
        return sequential;
    }

    [[nodiscard]] const u8* IRAM_ATTR ensure_window(u32 page, bool prefetch, bool sequential) const {
        if (!prefetch && std::find(unique_pages_.begin(), unique_pages_.end(), page) == unique_pages_.end()) {
            unique_pages_.push_back(page);
        }
        if (page < window_slots_.size()) {
            const int slot = window_slots_[page];
            if (slot >= 0) {
                auto& window = windows_[static_cast<std::size_t>(slot)];
                if (window.valid && window.page == page) {
                    if (!prefetch) {
                        ++frame_stats_.cache_hits;
                        if (sequential) {
                            ++frame_stats_.sequential_hits;
                        }
                        if (window.prefetched) {
                            ++frame_stats_.prefetch_hits;
                            window.prefetched = false;
                        }
                    }
                    window.age = ++clock_;
                    return window.data;
                }
                window_slots_[page] = -1;
            }
        }
        for (auto& window : windows_) {
            if (window.valid && window.page == page) {
                if (!prefetch) {
                    ++frame_stats_.cache_hits;
                    if (sequential) {
                        ++frame_stats_.sequential_hits;
                    }
                    if (window.prefetched) {
                        ++frame_stats_.prefetch_hits;
                        window.prefetched = false;
                    }
                }
                window.age = ++clock_;
                if (page < window_slots_.size()) {
                    window_slots_[page] = static_cast<int>(&window - windows_.data());
                }
                return window.data;
            }
        }

        if (prefetch) {
            ++frame_stats_.prefetch_misses;
        } else {
            ++frame_stats_.cache_misses;
        }
        auto* victim = &windows_.front();
        for (auto& window : windows_) {
            if (!window.valid) {
                victim = &window;
                break;
            }
            if (window.age < victim->age) {
                victim = &window;
            }
        }

        if (victim->valid) {
            if (victim->page < window_slots_.size()) {
                window_slots_[victim->page] = -1;
            }
            esp_partition_munmap(victim->handle);
            *victim = {};
        }

        const auto offset = static_cast<std::size_t>(page) * window_bytes_;
        if (offset >= size_) {
            return nullptr;
        }

        const auto bytes = std::min(window_bytes_, size_ - offset);
        const void* ptr = nullptr;
        esp_partition_mmap_handle_t handle = 0;
        const auto ret = esp_partition_mmap(partition_, offset, bytes, ESP_PARTITION_MMAP_DATA, &ptr, &handle);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "esp_partition_mmap page=%lu offset=%u size=%u failed: %s",
                     static_cast<unsigned long>(page),
                     static_cast<unsigned>(offset),
                     static_cast<unsigned>(bytes),
                     esp_err_to_name(ret));
            return nullptr;
        }

        victim->valid = true;
        victim->prefetched = prefetch;
        victim->page = page;
        victim->data = static_cast<const u8*>(ptr);
        victim->mapped_size = bytes;
        victim->age = ++clock_;
        victim->handle = handle;
        if (page < window_slots_.size()) {
            window_slots_[page] = static_cast<int>(victim - windows_.data());
        }
        return victim->data;
    }

    const esp_partition_t* partition_ = nullptr;
    std::size_t size_ = 0;
    std::size_t window_bytes_ = 64u * 1024u;
    mutable std::vector<Window> windows_;
    mutable std::vector<int> window_slots_;
    mutable std::vector<u32> unique_pages_;
    mutable bool has_last_demand_ = false;
    mutable u32 last_demand_end_ = 0;
    mutable u64 clock_ = 0;
    mutable RomAccessStats frame_stats_{};
};

class Esp32SdCacheRomProvider final : public RomProvider {
public:
    Esp32SdCacheRomProvider(FILE* file,
                            std::size_t size,
                            std::unique_ptr<u8, HeapCapsDeleter> cache,
                            std::size_t page_bytes,
                            std::size_t page_count)
        : file_(file, &std::fclose),
          size_(size),
          cache_(std::move(cache)),
          page_bytes_(page_bytes),
          page_slots_((size + page_bytes - 1u) / page_bytes, -1),
          frame_seen_pages_((size + page_bytes - 1u) / page_bytes, 0),
          pages_(std::max<std::size_t>(1u, page_count)),
          max_pinned_pages_(std::max<std::size_t>(1u, std::max<std::size_t>(1u, page_count) / 4u)) {
        for (std::size_t index = 0; index < pages_.size(); ++index) {
            pages_[index].data = cache_.get() + index * page_bytes_;
        }
    }

    [[nodiscard]] std::size_t size() const override { return size_; }

    [[nodiscard]] u8 IRAM_ATTR read_byte(u32 address) const override {
        if (static_cast<std::size_t>(address) >= size_) {
            return 0xFFu;
        }
        const auto* ptr = read_ptr(address, 1);
        return ptr ? ptr[0] : 0xFFu;
    }

    [[nodiscard]] u16 IRAM_ATTR read16(u32 address) const override {
        if (static_cast<std::size_t>(address) + 2u > size_) {
            return 0xFFFFu;
        }
        if ((static_cast<std::size_t>(address) % page_bytes_) + 2u > page_bytes_) {
            std::array<u8, 2> scratch{};
            return read_bytes(address, scratch) ? load_le16(scratch.data()) : 0xFFFFu;
        }
        const auto* ptr = read_ptr(address, 2);
        if (!ptr) {
            return 0xFFFFu;
        }
        return load_le16(ptr);
    }

    [[nodiscard]] u32 IRAM_ATTR read32(u32 address) const override {
        if (static_cast<std::size_t>(address) + 4u > size_) {
            return 0xFFFFFFFFu;
        }
        if ((static_cast<std::size_t>(address) % page_bytes_) + 4u > page_bytes_) {
            std::array<u8, 4> scratch{};
            return read_bytes(address, scratch) ? load_le32(scratch.data()) : 0xFFFFFFFFu;
        }
        const auto* ptr = read_ptr(address, 4);
        if (!ptr) {
            return 0xFFFFFFFFu;
        }
        return load_le32(ptr);
    }

    [[nodiscard]] bool read_bytes(u32 address, std::span<u8> out) const override {
        if (out.empty()) {
            return true;
        }
        if (static_cast<std::size_t>(address) + out.size() > size_) {
            return false;
        }

        const auto streaming = note_demand_access(address, out.size());
        bool had_miss = false;
        frame_stats_.byte_reads += out.size();
        std::size_t copied = 0;
        while (copied < out.size()) {
            const auto absolute = static_cast<std::size_t>(address) + copied;
            const auto page = static_cast<u32>(absolute / page_bytes_);
            const auto offset_in_page = absolute % page_bytes_;
            const auto chunk = std::min(out.size() - copied, page_bytes_ - offset_in_page);
            const auto loaded = ensure_page(page, false, streaming);
            const auto* page_data = loaded.data;
            had_miss = had_miss || !loaded.hit;
            if (!page_data) {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(copied),
                          out.begin() + static_cast<std::ptrdiff_t>(copied + chunk),
                          0xFFu);
            } else {
                std::memcpy(out.data() + copied, page_data + offset_in_page, chunk);
            }
            copied += chunk;
        }
        maybe_prefetch_after_read(address, out.size(), streaming, had_miss);
        return true;
    }

    void prefetch(u32 address, std::size_t bytes) const override {
        if (bytes == 0 || static_cast<std::size_t>(address) >= size_) {
            return;
        }
        ++frame_stats_.prefetches;
        frame_stats_.prefetch_bytes += bytes;
        const auto end = std::min<std::size_t>(size_, static_cast<std::size_t>(address) + bytes);
        for (auto cursor = static_cast<std::size_t>(address); cursor < end; cursor = ((cursor / page_bytes_) + 1u) * page_bytes_) {
            (void)ensure_page(static_cast<u32>(cursor / page_bytes_), true, false);
        }
    }

    [[nodiscard]] RomAccessStats frame_stats() const override {
        auto stats = frame_stats_;
        stats.unique_pages = static_cast<u32>(unique_pages_.size());
        stats.miss_penalty_us = stats.cache_misses * kSdMissPenaltyUs;
        stats.miss_penalty_cycles = miss_penalty_cycles(stats.cache_misses);
        return stats;
    }

    void reset_frame_stats() const override {
        frame_stats_ = {};
        for (const auto page : unique_pages_) {
            if (page < frame_seen_pages_.size()) {
                frame_seen_pages_[page] = 0;
            }
        }
        unique_pages_.clear();
        has_last_demand_ = false;
        last_demand_end_ = 0;
        sequential_run_ = 0;
        last_prefetch_base_page_ = kInvalidPage;
        for (auto& page : pages_) {
            page.prefetched = false;
        }
    }

private:
    struct Page {
        bool valid = false;
        bool prefetched = false;
        bool pinned = false;
        u32 index = kInvalidPage;
        u8* data = nullptr;
        u64 age = 0;
        u32 hot_score = 0;
    };

    struct PageLoad {
        const u8* data = nullptr;
        bool hit = false;
    };

    [[nodiscard]] const u8* IRAM_ATTR read_ptr(u32 address, std::size_t bytes) const {
        const auto streaming = note_demand_access(address, bytes);
        frame_stats_.byte_reads += bytes;
        const auto absolute = static_cast<std::size_t>(address);
        const auto page = static_cast<u32>(absolute / page_bytes_);
        const auto offset_in_page = absolute % page_bytes_;
        if (offset_in_page + bytes > page_bytes_) {
            return nullptr;
        }
        auto loaded = ensure_page_fast(page, false, streaming);
        if (loaded.data == nullptr) {
            loaded = ensure_page_slow(page, false);
        }
        maybe_prefetch_after_read(address, bytes, streaming, !loaded.hit);
        return loaded.data ? loaded.data + offset_in_page : nullptr;
    }

    [[nodiscard]] bool note_demand_access(u32 address, std::size_t bytes) const {
        const auto page = static_cast<u32>(static_cast<std::size_t>(address) / page_bytes_);
        note_unique_page(page);

        const auto end = static_cast<u32>(static_cast<u64>(address) + bytes);
        const auto sequential = has_last_demand_ && address == last_demand_end_;
        sequential_run_ = sequential ? sequential_run_ + 1u : 0u;
        has_last_demand_ = true;
        last_demand_end_ = end;
        return sequential;
    }

    void note_unique_page(u32 page) const {
        if (page < frame_seen_pages_.size()) {
            if (frame_seen_pages_[page] == 0) {
                frame_seen_pages_[page] = 1;
                unique_pages_.push_back(page);
            }
            return;
        }
        if (std::find(unique_pages_.begin(), unique_pages_.end(), page) == unique_pages_.end()) {
            unique_pages_.push_back(page);
        }
    }

    void maybe_prefetch_after_read(u32 address, std::size_t bytes, bool streaming, bool had_miss) const {
        if (!streaming && !had_miss) {
            return;
        }
        const auto end = static_cast<std::size_t>(address) + bytes;
        const auto current_page = static_cast<u32>((end == 0 ? 0 : end - 1u) / page_bytes_);
        if (!had_miss && current_page == last_prefetch_base_page_) {
            return;
        }
        last_prefetch_base_page_ = current_page;

        const auto pages_ahead = sequential_run_ >= 4u ? 2u : 1u;
        const auto start_page = current_page + 1u;
        const auto end_page = start_page + pages_ahead;
        for (u32 page = start_page; page < end_page; ++page) {
            if (static_cast<std::size_t>(page) * page_bytes_ >= size_) {
                break;
            }
            ++frame_stats_.prefetches;
            frame_stats_.prefetch_bytes += page_bytes_;
            (void)ensure_page(page, true, false);
        }
    }

    void heat_page(Page& page) const {
        if (page.hot_score < kHotPinThreshold) {
            ++page.hot_score;
        }
        if (!page.pinned && page.hot_score >= kHotPinThreshold && pinned_pages_ < max_pinned_pages_) {
            page.pinned = true;
            ++pinned_pages_;
        }
    }

    [[nodiscard]] PageLoad ensure_page(u32 page_index, bool prefetch, bool sequential) const {
        const auto hit = ensure_page_fast(page_index, prefetch, sequential);
        if (hit.data != nullptr) {
            return hit;
        }
        return ensure_page_slow(page_index, prefetch);
    }

    [[nodiscard]] PageLoad IRAM_ATTR ensure_page_fast(u32 page_index, bool prefetch, bool sequential) const {
        if (!prefetch) {
            note_unique_page(page_index);
        }
        if (page_index < page_slots_.size()) {
            const int slot = page_slots_[page_index];
            if (slot >= 0) {
                auto& page = pages_[static_cast<std::size_t>(slot)];
                if (page.valid && page.index == page_index) {
                    if (!prefetch) {
                        ++frame_stats_.cache_hits;
                        if (sequential) {
                            ++frame_stats_.sequential_hits;
                        }
                        if (page.prefetched) {
                            ++frame_stats_.prefetch_hits;
                            page.prefetched = false;
                        }
                        heat_page(page);
                    }
                    page.age = ++clock_;
                    return {page.data, true};
                }
                page_slots_[page_index] = -1;
            }
        }
        return {};
    }

    [[nodiscard]] PageLoad ensure_page_slow(u32 page_index, bool prefetch) const {
        if (prefetch) {
            ++frame_stats_.prefetch_misses;
        } else {
            ++frame_stats_.cache_misses;
        }
        auto* victim = &pages_.front();
        bool found_unpinned = false;
        for (auto& page : pages_) {
            if (!page.valid) {
                victim = &page;
                found_unpinned = true;
                break;
            }
            if (page.pinned) {
                continue;
            }
            if (!found_unpinned || page.age < victim->age) {
                victim = &page;
                found_unpinned = true;
            }
        }
        if (!found_unpinned) {
            victim = &pages_.front();
            for (auto& page : pages_) {
                if (page.age < victim->age) {
                    victim = &page;
                }
            }
        }

        const auto offset = static_cast<std::size_t>(page_index) * page_bytes_;
        if (offset >= size_) {
            return {};
        }

        const auto victim_slot = static_cast<int>(victim - pages_.data());
        if (victim->valid && victim->index < page_slots_.size() &&
            page_slots_[victim->index] == victim_slot) {
            page_slots_[victim->index] = -1;
        }

        if (std::fseek(file_.get(), static_cast<long>(offset), SEEK_SET) != 0) {
            ESP_LOGE(kTag, "ROM cache seek failed offset=%u", static_cast<unsigned>(offset));
            return {};
        }

        const auto remaining = std::min(page_bytes_, size_ - offset);
        const auto read = std::fread(victim->data, 1, remaining, file_.get());
        if (read != remaining && std::ferror(file_.get())) {
            ESP_LOGE(kTag, "ROM cache read failed page=%lu", static_cast<unsigned long>(page_index));
            std::clearerr(file_.get());
            return {};
        }
        if (read < page_bytes_) {
            std::memset(victim->data + read, 0xFF, page_bytes_ - read);
        }

        if (victim->valid && victim->pinned && pinned_pages_ != 0) {
            --pinned_pages_;
        }
        victim->valid = true;
        victim->prefetched = prefetch;
        victim->pinned = false;
        victim->index = page_index;
        victim->age = ++clock_;
        victim->hot_score = prefetch ? 0 : 1;
        if (page_index < page_slots_.size()) {
            page_slots_[page_index] = victim_slot;
        }
        return {victim->data, false};
    }

    std::unique_ptr<FILE, int (*)(FILE*)> file_;
    std::size_t size_ = 0;
    std::unique_ptr<u8, HeapCapsDeleter> cache_;
    std::size_t page_bytes_ = 32u * 1024u;
    mutable std::vector<int> page_slots_;
    mutable std::vector<u8> frame_seen_pages_;
    mutable std::vector<Page> pages_;
    mutable std::vector<u32> unique_pages_;
    mutable u64 clock_ = 0;
    mutable bool has_last_demand_ = false;
    mutable u32 last_demand_end_ = 0;
    mutable u32 sequential_run_ = 0;
    mutable u32 last_prefetch_base_page_ = kInvalidPage;
    mutable std::size_t pinned_pages_ = 0;
    std::size_t max_pinned_pages_ = 0;
    mutable RomAccessStats frame_stats_{};
};

}  // namespace

std::unique_ptr<RomProvider> make_esp32_mmap_rom_provider(const char* partition_label,
                                                          std::size_t max_size,
                                                          std::size_t window_bytes,
                                                          std::size_t window_count) {
    const auto* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, partition_label);
    if (!partition) {
        ESP_LOGW(kTag, "ROM mmap partition '%s' not found", partition_label);
        return nullptr;
    }

    const auto size = std::min<std::size_t>(partition->size, max_size);
    ESP_LOGI(kTag, "ROM mmap provider partition=%s size=%u window=%u count=%u",
             partition_label,
             static_cast<unsigned>(size),
             static_cast<unsigned>(window_bytes),
             static_cast<unsigned>(window_count));
    return std::make_unique<Esp32MmapRomProvider>(partition, size, window_bytes, window_count);
}

std::unique_ptr<RomProvider> make_esp32_sd_cache_rom_provider(const char* path,
                                                             std::size_t cache_bytes,
                                                             std::size_t page_bytes) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        ESP_LOGE(kTag, "failed to open ROM file %s", path);
        return nullptr;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return nullptr;
    }
    const auto size_long = std::ftell(file);
    if (size_long <= 0) {
        std::fclose(file);
        return nullptr;
    }
    std::rewind(file);

    auto page_count = std::max<std::size_t>(1u, cache_bytes / page_bytes);
    while (page_count > 0) {
        const auto bytes = page_count * page_bytes;
        std::unique_ptr<u8, HeapCapsDeleter> cache(alloc_rom_cache(bytes));
        if (cache) {
            ESP_LOGI(kTag, "ROM SD cache provider path=%s size=%u cache=%u pages=%u",
                     path,
                     static_cast<unsigned>(size_long),
                     static_cast<unsigned>(bytes),
                     static_cast<unsigned>(page_count));
            return std::make_unique<Esp32SdCacheRomProvider>(
                file, static_cast<std::size_t>(size_long), std::move(cache), page_bytes, page_count);
        }
        page_count /= 2u;
    }

    ESP_LOGE(kTag, "failed to allocate ROM cache for %s", path);
    std::fclose(file);
    return nullptr;
}

}  // namespace gba
