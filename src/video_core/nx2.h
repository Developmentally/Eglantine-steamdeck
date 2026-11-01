// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// NVN2 queues release their progress values into the second page of a two-page GPU-accessible
// region and GPU acquires read the same offsets in the first page (the CPU polls the second).
// Something on the console copies one into the other; until that is known, the puller mirrors
// releases that land in a second page. Every queue has such a region, so keep a small table.
inline constexpr size_t Nx2MaxSemaphoreRegions = 32;
inline std::array<std::atomic<u64>, Nx2MaxSemaphoreRegions> g_nx2_semaphore_regions{}; // starts

inline void Nx2AddSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        if (region.load() == start) {
            return;
        }
    }
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = 0;
        if (region.compare_exchange_strong(expected, start)) {
            return;
        }
    }
}

/// The region's memory was destroyed: stop mirroring into it.
inline void Nx2RemoveSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = start;
        region.compare_exchange_strong(expected, 0);
    }
}

/// A new emulation session starts: no queue has a region yet.
inline void Nx2ClearSemaphoreRegions() {
    for (auto& region : g_nx2_semaphore_regions) {
        region.store(0);
    }
}

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1
// runs command lists the GPU writes each frame (20-byte entries its culling fills in); read
// without a flush they are stale and the draws behind them (trees) go missing. The bits never
// clear: the dispatch that rewrites a page may still be queued on the split's back when its front
// reads the command list. One bit per 4 KiB of the 36-bit device address space (2 MiB).
// (From zortos293/Eglantine.)
inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_gpu_written_pages.size() * 64 - 1);
    const u64 first = device_address >> Nx2WrittenPageBits;
    for (u64 index = first / 64; first <= last && index <= last / 64; ++index) {
        const u64 low = index == first / 64 ? first % 64 : 0;
        const u64 high = index == last / 64 ? last % 64 : 63;
        const u64 bits = (~u64{0} >> (63 - high)) & (~u64{0} << low);
        auto& word = g_nx2_gpu_written_pages[index];
        if ((word.load(std::memory_order_relaxed) & bits) != bits) {
            word.fetch_or(bits, std::memory_order_relaxed);
        }
    }
}

/// A new emulation session starts: no page has been written by its GPU yet.
inline void Nx2ClearGpuWrittenPages() {
    for (auto& word : g_nx2_gpu_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) & (u64{1} << (page % 64))) != 0;
}

// Switch 2 sparse regions (nx2:gpu 214, word 7 bit 0x200: title 1's terrain texture arrays and
// layer pools, 170 MB-5.8 GB each, of which the game fills a little) get memory per block when the
// GPU first writes there, not in full. nx2:gpu installs the handler and the span its regions cover;
// the GPU memory manager calls Nx2BackSparseWrite before each write.
using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size)>;
inline std::recursive_mutex g_nx2_sparse_backing_mutex;
inline Nx2SparseBackingHandler g_nx2_sparse_backing;
inline std::atomic<u64> g_nx2_sparse_low{~u64{0}};
inline std::atomic<u64> g_nx2_sparse_high{0};

inline void Nx2SetSparseBacking(Nx2SparseBackingHandler handler) {
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    g_nx2_sparse_backing = std::move(handler);
    g_nx2_sparse_low.store(~u64{0});
    g_nx2_sparse_high.store(0);
}

/// Widens the span that writes are checked against (a new lazily backed region).
inline void Nx2AddSparseSpan(u64 gpu_address, u64 size) {
    u64 low = g_nx2_sparse_low.load();
    while (gpu_address < low && !g_nx2_sparse_low.compare_exchange_weak(low, gpu_address)) {
    }
    u64 high = g_nx2_sparse_high.load();
    while (gpu_address + size > high &&
           !g_nx2_sparse_high.compare_exchange_weak(high, gpu_address + size)) {
    }
}

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size) {
    if (gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    if (g_nx2_sparse_backing) {
        g_nx2_sparse_backing(gpu_address, size);
    }
}

// Which blocks of each sparse region have been written (got memory), for the texture cache: an
// image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last:
// the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
};
inline std::mutex g_nx2_sparse_state_mutex;
inline std::map<u64, Nx2SparseRegionState> g_nx2_sparse_state;

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state[gpu_address] = {size, block_size,
                                       std::vector<bool>((size + block_size - 1) / block_size)};
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.erase(gpu_address);
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (const auto it = g_nx2_sparse_state.find(region_address); it != g_nx2_sparse_state.end()) {
        it->second.written[block] = true;
    }
}

/// True when [gpu_address, +size) lies in one sparse region.
inline bool Nx2InSparseRegion(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    return gpu_address + size <= it->first + it->second.size;
}

/// The GPU renders into [gpu_address, +size): count its blocks as written, so images made there
/// later get memory (their contents live in the host image, not in guest memory).
inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    for (auto& [start, region] : g_nx2_sparse_state) {
        const u64 end = start + region.size;
        if (gpu_address >= end || gpu_address + size <= start) {
            continue;
        }
        const u64 first = ((std::max)(gpu_address, start) - start) / region.block_size;
        const u64 last = ((std::min)(gpu_address + size, end) - 1 - start) / region.block_size;
        for (u64 block = first; block <= last; ++block) {
            region.written[block] = true;
        }
    }
}

/// True when [gpu_address, +size) lies in one sparse region and none of it has been written.
inline bool Nx2SparseUnwritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    const auto& [start, region] = *it;
    if (gpu_address + size > start + region.size) {
        return false;
    }
    const u64 last = (gpu_address + size - 1 - start) / region.block_size;
    for (u64 block = (gpu_address - start) / region.block_size; block <= last; ++block) {
        if (region.written[block]) {
            return false;
        }
    }
    return true;
}

/// EDEN_NX2_TRACE_DST=<hex> with EDEN_NX2_TRACE_DST_MIB=<n> (32): true when a write of `size`
/// at `gpu_address` touches [dst, dst + n MiB). The DMA engines, Fermi 2D and written storage
/// buffers log their writes there, to find out what fills a texture.
/// Set at run time (EDEN_NX2_VS_TEX_TRACE) to trace the writers of an image a shader samples.
inline std::atomic<u64> g_nx2_trace_dst_start{0};
inline std::atomic<u64> g_nx2_trace_dst_size{0};

/// Native program key (the name of its dump file) by program address, filled as programs are
/// captured, so draw-time diagnostics can be keyed stably across sessions.
inline std::mutex g_nx2_program_keys_lock;
inline std::unordered_map<u64, u64> g_nx2_program_keys;

inline u64 Nx2ProgramKey(u64 program_base) {
    std::scoped_lock lock{g_nx2_program_keys_lock};
    const auto it = g_nx2_program_keys.find(program_base);
    return it != g_nx2_program_keys.end() ? it->second : 0;
}

/// Set by the rasterizer while EDEN_NX2_TRACE_FRAME_AFTER traces a frame, so the native MME
/// can log its DMA reads (indirect draw arguments) in sequence with the draws.
inline std::atomic<bool> g_nx2_frame_trace_active{false};

inline bool Nx2TracedDestination(u64 gpu_address, u64 size) {
    static const u64 start = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_DST");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    static const u64 length = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_DST_MIB");
        return (value != nullptr ? std::strtoull(value, nullptr, 10) : 32ULL) << 20;
    }();
    if (start != 0 && gpu_address < start + length && gpu_address + size > start) {
        return true;
    }
    const u64 dyn_start = g_nx2_trace_dst_start.load(std::memory_order_relaxed);
    const u64 dyn_size = g_nx2_trace_dst_size.load(std::memory_order_relaxed);
    return dyn_start != 0 && gpu_address < dyn_start + dyn_size && gpu_address + size > dyn_start;
}

/// GPU VA in the first page that mirrors a release at `address`, or 0.
inline u64 Nx2SemaphoreMirror(u64 address) {
    for (const auto& region : g_nx2_semaphore_regions) {
        const u64 start = region.load(std::memory_order_relaxed);
        if (start == 0) {
            continue;
        }
        if (address >= start + 0x1000 && address < start + 0x2000) {
            return address - 0x1000;
        }
    }
    return 0;
}

/// The last few thousand semaphore events by device address, for the report a channel makes
/// after waiting 2 s on an acquire: what was queued for, landed on, or dropped at the words
/// around it (a release that never lands is a deadlock on the console's side of things).
/// Title 4's terrain indirect draw records: a reset program zeroes them (run on the CPU, see
/// RasterizerVulkan::ResetIndirectRecordsOnCpu), a culling pass counts triangles into them on the
/// GPU, and the colour pass reads them as macro parameters (DmaPusher::Step). They are flushed
/// together once after each reset the front sent, instead of draining the back at each of the
/// ~1,000 parameter entries a frame.
class Nx2IndirectRecords {
public:
    static Nx2IndirectRecords& Get() {
        static Nx2IndirectRecords records;
        return records;
    }

    static constexpr u64 RecordBytes = 20;

    /// Back: the records the reset program just rewrote (GPU addresses).
    void SetPointers(const std::vector<u64>& pointers) {
        std::vector<u64> sorted;
        sorted.reserve(pointers.size());
        for (const u64 pointer : pointers) {
            if (pointer != 0) {
                sorted.push_back(pointer);
            }
        }
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        std::scoped_lock lock{mutex};
        records = std::move(sorted);
    }

    /// Whether gpu_addr lies in one of the records.
    [[nodiscard]] bool Contains(u64 gpu_addr) const {
        std::scoped_lock lock{mutex};
        const auto next = std::upper_bound(records.begin(), records.end(), gpu_addr);
        return next != records.begin() && gpu_addr < *(next - 1) + RecordBytes;
    }

    [[nodiscard]] std::vector<u64> Records() const {
        std::scoped_lock lock{mutex};
        return records;
    }

    /// Front: reset draws sent to the back, and the count at the last flush of the records.
    std::atomic<u64> resets_sent{};
    u64 resets_flushed{};

private:
    mutable std::mutex mutex;
    std::vector<u64> records;
};

/// Guest memory the split's back half (gpu_split.h) will write when it runs an engine copy the
/// front has already sent: copy engine launches and inline-to-memory uploads, by device address.
/// The back writes the CPU side of these copies itself (DMACopy's mirror, Upload::State), so a
/// command list the game builds with one (Donkey Kong Bananza's menus) holds the previous
/// contents until then; parsed early it is a stale, differently sized list, and the pusher
/// falls out of step (`034813`: a bogus syncpoint id in SignalSyncPoint, a SEGV; `040345`: a
/// swallowed semaphore, a deadlock). Noted and read on the front GPU thread only.
class Nx2PendingGpuWrites {
public:
    static Nx2PendingGpuWrites& Get() {
        static Nx2PendingGpuWrites writes;
        return writes;
    }

    void Note(u64 device_address, u64 size, u64 split_position) {
        if (size == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        if (writes.size() >= MaxWrites) {
            writes.erase(writes.begin());
        }
        writes.push_back({device_address, device_address + size, split_position});
    }

    /// Whether a copy the back has not run yet writes into the range. `back_position` is what
    /// the back has executed up to; a copy sent at a position before it has run and is forgotten.
    [[nodiscard]] bool Pending(u64 device_address, u64 size, u64 back_position) {
        std::scoped_lock lk{mutex};
        std::erase_if(writes, [back_position](const Write& write) {
            return write.position < back_position;
        });
        const u64 end = device_address + size;
        return std::ranges::any_of(writes, [device_address, end](const Write& write) {
            return write.begin < end && device_address < write.end;
        });
    }

private:
    struct Write {
        u64 begin;
        u64 end;
        u64 position;
    };
    static constexpr size_t MaxWrites = 65536;
    std::mutex mutex;
    std::vector<Write> writes;
};

class Nx2SemaphoreHistory {
public:
    enum Source : u32 {
        QueryLanded = 1,  ///< Query-cache release written (puller, 3D, DMA)
        QmdLanded = 2,    ///< Compute QMD release written
        Queued = 10,      ///< Front noted a release for the back (prediction)
        Invalidated = 20, ///< Query-cache release dropped: the query was invalidated
        Unsynced = 21,    ///< Query-cache release dropped: value never synced
        GpuWrite = 30,    ///< Engine write of up to 64 bytes (first word kept)
    };

    static Nx2SemaphoreHistory& Get() {
        static Nx2SemaphoreHistory history;
        return history;
    }

    void Record(u64 device_address, u32 value, u32 source) {
        const u64 now = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::scoped_lock lk{mutex};
        entries[next++ % entries.size()] = {device_address, now, value, source};
    }

    /// Events on the 64 bytes at `block` (newest last), as "+offset=value source age".
    [[nodiscard]] std::string Describe(u64 block) {
        static constexpr std::array<const char*, 31> names{
            "?", "landed", "qmd", "?", "?", "?", "?", "?", "?", "?", "queued", "?", "?", "?",
            "?", "?", "?", "?", "?", "?", "dropped:invalidated", "dropped:unsynced", "?", "?",
            "?", "?", "?", "?", "?", "?", "gpu-write"};
        const u64 now = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::string text;
        std::scoped_lock lk{mutex};
        const size_t total = (std::min)(next, entries.size());
        u32 shown{};
        for (size_t i = 0; i < total && shown < 48; ++i) {
            const auto& e = entries[(next - total + i) % entries.size()];
            if ((e.address & ~u64{0x3f}) != block) {
                continue;
            }
            ++shown;
            text += fmt::format(" +{:#x}={:#x} {} {}ms", e.address - block, e.value,
                                e.source < names.size() ? names[e.source] : "?",
                                (now - e.time) / 1'000'000);
        }
        return text.empty() ? " (none)" : text;
    }

private:
    struct Entry {
        u64 address;
        u64 time;
        u32 value;
        u32 source;
    };
    std::mutex mutex;
    std::array<Entry, 16384> entries{};
    size_t next{};
};

/// Channels suspended on a semaphore acquire. A console channel polls and sees every release;
/// Eden writes releases in bursts when fences signal and resumes waiters only between command
/// lists, so an acquire-equal could miss its value. Writes latch the waiters they satisfy.
class Nx2SemaphoreWaiters {
public:
    // Puller GpuSemaphoreOperation values.
    static constexpr u32 AcquireEqual = 0x1;
    static constexpr u32 AcquireGequal = 0x4;
    static constexpr u32 AcquireMask = 0x8;

    static Nx2SemaphoreWaiters& Get() {
        static Nx2SemaphoreWaiters waiters;
        return waiters;
    }

    void Add(const void* owner, u64 device_address, u32 value, u32 operation) {
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        waiters.push_back({owner, device_address, value, operation, false});
        count.store(waiters.size(), std::memory_order_release);
    }

    void Remove(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        count.store(waiters.size(), std::memory_order_release);
    }

    [[nodiscard]] bool Latched(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(waiters,
                                   [owner](const Waiter& w) { return w.owner == owner && w.latched; });
    }

    /// Who wrote the value a latched waiter saw (0 none): 1 a query-cache release (puller or
    /// 3D engine), 2 a compute QMD release. Profiling only.
    [[nodiscard]] u32 LatchedBy(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return 0;
        }
        std::scoped_lock lk{mutex};
        for (const auto& w : waiters) {
            if (w.owner == owner && w.latched) {
                return w.source;
            }
        }
        return 0;
    }

    /// A GPU write of `value` to the u32 at `device_address`.
    void OnWrite(u64 device_address, u32 value, u32 source = 1) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        bool latched_any = false;
        {
            std::scoped_lock lk{mutex};
            for (auto& w : waiters) {
                if (w.address == device_address && !w.latched && Matches(w, value)) {
                    w.latched = true;
                    w.source = source;
                    latched_any = true;
                }
            }
        }
        if (latched_any) {
            {
                std::scoped_lock lk{latch_mutex};
                ++latch_generation;
            }
            latch_cv.notify_all();
        }
    }

    /// Sleeps until a waiter latches or `timeout` passes: the GPU thread's poll loop, so a
    /// release landing resumes its channel at once rather than after a host sleep.
    template <typename Rep, typename Period>
    void WaitForLatch(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lk{latch_mutex};
        const u64 seen = latch_generation;
        latch_cv.wait_for(lk, timeout, [&] { return latch_generation != seen; });
    }

private:
    struct Waiter {
        const void* owner;
        u64 address;
        u32 value;
        u32 operation;
        bool latched;
        u32 source{};
    };

    static bool Matches(const Waiter& w, u32 current) {
        switch (w.operation) {
        case AcquireEqual:
            return current == w.value;
        case AcquireGequal:
            return current >= w.value;
        case AcquireMask:
            return (current & w.value) != 0;
        default:
            return false;
        }
    }

    std::mutex mutex;
    std::vector<Waiter> waiters;
    std::atomic<size_t> count{};
    std::mutex latch_mutex;
    std::condition_variable latch_cv;
    u64 latch_generation{};
};

/// Semaphore releases queued for the split GPU thread's back half (gpu_split.h) whose payload
/// has not been written to guest memory yet, by device address: the puller's releases and the
/// 3D engine's REPORT_SEMAPHORE releases the front parses. The value an acquire sees in order
/// is the last of them, not the stale word in memory; and one it meets needs no drain of the
/// back, which runs that release before anything queued after the acquire, as the hardware
/// does in order. Noted and read on the front GPU thread; writes land from the back or the
/// fencing thread (Nx2SemaphoreWaiters::OnWrite).
class Nx2QueuedReleases {
public:
    static Nx2QueuedReleases& Get() {
        static Nx2QueuedReleases releases;
        return releases;
    }

    void Note(u64 device_address, u32 payload, u64 split_position) {
        Nx2SemaphoreHistory::Get().Record(device_address, payload, Nx2SemaphoreHistory::Queued);
        std::scoped_lock lk{mutex};
        auto& release = releases[device_address];
        release.payload = payload;
        release.position = split_position;
        release.passed_since = {};
        ++release.outstanding;
        count.store(releases.size(), std::memory_order_release);
    }

    /// The payload the word will hold once the releases queued for it have landed, if any
    /// are still outstanding. `split_position` is the split's send position (entries from
    /// before a restart go); `back_position` what the back has executed. A release the back
    /// has run only waits for its host fence, so one still outstanding long after that never
    /// lands (the query cache drops invalidated reports) and its prediction goes: predicting a
    /// value memory will never hold lets an acquire pass early, and a channel that runs ahead
    /// of its releases deadlocks the game (`011932`).
    [[nodiscard]] std::optional<u32> Pending(u64 device_address, u64 split_position,
                                             u64 back_position) {
        if (count.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        std::scoped_lock lk{mutex};
        const auto it = releases.find(device_address);
        if (it == releases.end()) {
            return std::nullopt;
        }
        auto& release = it->second;
        if (release.position > split_position) {
            releases.erase(it);
            count.store(releases.size(), std::memory_order_release);
            return std::nullopt;
        }
        if (release.position <= back_position) {
            const auto now = std::chrono::steady_clock::now();
            if (release.passed_since == std::chrono::steady_clock::time_point{}) {
                release.passed_since = now;
            } else if (now - release.passed_since > std::chrono::milliseconds{100}) {
                releases.erase(it);
                count.store(releases.size(), std::memory_order_release);
                expired.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
        }
        return release.payload;
    }

    /// Predictions dropped for outliving the back by 100 ms (diagnostics).
    [[nodiscard]] u64 Expired() const noexcept {
        return expired.load(std::memory_order_relaxed);
    }

    /// A GPU write to the u32 at `device_address` landed: one fewer release outstanding; the
    /// last one landing retires the prediction whatever it wrote (a wrong one must not stay).
    void OnWrite(u64 device_address) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        const auto it = releases.find(device_address);
        if (it != releases.end() && --it->second.outstanding == 0) {
            releases.erase(it);
            count.store(releases.size(), std::memory_order_release);
        }
    }

    /// Drops a prediction memory has contradicted for a while (a release the back never made).
    void Forget(u64 device_address) {
        std::scoped_lock lk{mutex};
        releases.erase(device_address);
        count.store(releases.size(), std::memory_order_release);
    }

    [[nodiscard]] bool Empty() const noexcept {
        return count.load(std::memory_order_acquire) == 0;
    }

private:
    struct Release {
        u32 payload{};
        u64 position{};
        u32 outstanding{};
        std::chrono::steady_clock::time_point passed_since{}; ///< First seen behind the back
    };
    std::mutex mutex;
    std::unordered_map<u64, Release> releases;
    std::atomic<size_t> count{};
    std::atomic<u64> expired{};
};

/// A GPU semaphore write landed in guest memory: wakes waiters, retires the queued release.
inline void Nx2OnSemaphoreWrite(u64 device_address, u32 value, u32 source = 1) {
    Nx2SemaphoreHistory::Get().Record(device_address, value, source);
    Nx2SemaphoreWaiters::Get().OnWrite(device_address, value, source);
    Nx2QueuedReleases::Get().OnWrite(device_address);
}

} // namespace Tegra
