// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_set>

#if defined(__x86_64__)
#include <x86intrin.h>
#elif defined(_M_X64)
#include <intrin.h>
#endif
#include "common/common_types.h"
#include "common/logging.h"
#include "common/cpu_features.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    SplitFull,    ///< Split GPU thread: the front waits for ring space
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait", "split full",
};

// Read once at startup: Enabled() runs several times per method write (no guard to check).
inline const bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread). On x64
// the wall clock fences its rdtsc on both sides; a zone boundary does not need that, and the
// fences were 18% of the GPU back thread with the profile on (perf). Unfenced rdtsc, scaled by
// the wall clock's own rate.
inline u64 Now() {
#if defined(__x86_64__) || defined(_M_X64)
    // The TSC, scaled by the wall clock's rate when it is TSC-based, else by a 2 ms calibration
    // against steady_clock at first use (the Deck's wall clock is not "native", and the
    // clock_gettime fallback was 3.5% of the back thread with profiling on).
    static const double ns_per_tick = [] {
        if (Common::g_wall_clock.IsNative()) {
            const u64 ticks = Common::g_wall_clock.NsToTicks(std::chrono::nanoseconds{1'000'000'000});
            if (ticks != 0) {
                return 1e9 / static_cast<double>(ticks);
            }
        }
        const auto start_time = std::chrono::steady_clock::now();
        const u64 start_tsc = __rdtsc();
        auto end_time = start_time;
        while (end_time - start_time < std::chrono::milliseconds{2}) {
            end_time = std::chrono::steady_clock::now();
        }
        const u64 end_tsc = __rdtsc();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        return end_tsc != start_tsc ? static_cast<double>(ns) / static_cast<double>(end_tsc - start_tsc) : 1.0;
    }();
    return static_cast<u64>(static_cast<double>(__rdtsc()) * ns_per_tick);
#else
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
#endif
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 36> counters{}; ///< See Counter
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Now();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Now() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
    DrainAcquire,   ///< Back drains for a semaphore acquire (puller)
    DrainPoll,      ///< Back drains polling a suspended channel's acquire
    DrainDmaRead,   ///< Back drains for a native MME DMA read
    DrainSync,      ///< Back drains for a synced command list (dma_pusher)
    DrainOther,     ///< Back drains elsewhere (front state hand-over, verify, compute)
    AcquireMetByDrain,  ///< Acquires met only once the back had run (its queued releases)
    AcquireMetByFlush,  ///< Of those: the buffer cache held the word (memory was stale)
    AcquireMetByFences, ///< Acquires met only after ReleaseFences()
    WaitEndedByMemory,  ///< Suspended channels resumed by: the word in guest memory
    WaitEndedByQuery,   ///< a puller/3D release landing (fence)
    WaitEndedByQmd,     ///< a compute QMD release landing (fence)
    WaitEndedByDrain,   ///< the periodic drain (buffer cache copy or prediction)
    AcquirePredicted,   ///< Acquire checks answered by a queued release's predicted value
    PredictionConflicts, ///< Predictions dropped because memory contradicted them for 50 ms
    PredictionExpired,  ///< Predictions dropped for outliving the back's execution by 100 ms
    DrainParameters,    ///< Front drains for macro parameters the GPU may have written (dma_pusher)
    ParameterFlushesSkipped, ///< Parameter entries on indirect draw records flushed since the last reset
    DrainCommandLists,  ///< Front drains for command lists on pages the GPU has written (dma_pusher)
    DrainEngineWrites,  ///< Front drains for command lists a queued copy or upload writes (dma_pusher)
    DmaReadPredicted,   ///< Native MME DMA reads answered without a drain (queued releases applied)
    DrainQmdRead,       ///< Front drains for a QMD a queued copy or upload writes (compute mirror)
    DrainMappings,      ///< Front drains for command lists in a range a queued map/unmap changes
    AcquireBehindBack,  ///< Acquire checks held because a release queued for the back has not run
    AliasSyncs,         ///< SynchronizeAliases calls (texture cache, per sampled aliased image)
    AliasSyncWalked,    ///< Alias entries walked by them
    SnapshotsUnwritten, ///< Entries read from memory because they were zeros when submitted
    ChannelYields,      ///< A channel handed the GPU thread to one whose wait ended
    AcquireProbes,      ///< Flushed semaphore reads asked of the back instead of a drain
};

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
inline void Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window > zoned ? window - zoned : 0;
    const double wall = static_cast<double>(window);
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / wall;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / wall;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / wall,
                        static_cast<double>(data.counters[1]) * 1e9 / wall,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / wall,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / wall);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / wall,
                        static_cast<double>(data.counters[5]) * 1e9 / wall);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / wall,
                        static_cast<double>(data.counters[7]) * 1e9 / wall);
    line += fmt::format(" | drains: acquire {:.0f} poll {:.0f} dma read {:.0f} (predicted {:.0f}) qmd read {:.0f} remap {:.0f} sync {:.0f} other {:.0f}",
                        static_cast<double>(data.counters[8]) * 1e9 / wall,
                        static_cast<double>(data.counters[9]) * 1e9 / wall,
                        static_cast<double>(data.counters[10]) * 1e9 / wall,
                        static_cast<double>(data.counters[27]) * 1e9 / wall,
                        static_cast<double>(data.counters[28]) * 1e9 / wall,
                        static_cast<double>(data.counters[29]) * 1e9 / wall,
                        static_cast<double>(data.counters[11]) * 1e9 / wall,
                        static_cast<double>(data.counters[12]) * 1e9 / wall);
    line += fmt::format(" | acquires met by drain {:.0f} (flush {:.0f}), by fences {:.0f}",
                        static_cast<double>(data.counters[13]) * 1e9 / wall,
                        static_cast<double>(data.counters[14]) * 1e9 / wall,
                        static_cast<double>(data.counters[15]) * 1e9 / wall);
    line += fmt::format(" | alias syncs {:.0f}/s walking {:.0f} aliases/s | unwritten at submit {:.0f}/s | channel yields {:.0f}/s | acquire probes {:.0f}/s",
                        static_cast<double>(data.counters[31]) * 1e9 / wall,
                        static_cast<double>(data.counters[32]) * 1e9 / wall,
                        static_cast<double>(data.counters[33]) * 1e9 / wall,
                        static_cast<double>(data.counters[34]) * 1e9 / wall,
                        static_cast<double>(data.counters[35]) * 1e9 / wall);
    line += fmt::format(" | predicted {:.0f} ({:.0f} dropped, {:.0f} expired, {:.0f} behind the back)",
                        static_cast<double>(data.counters[20]) * 1e9 / wall,
                        static_cast<double>(data.counters[21]) * 1e9 / wall,
                        static_cast<double>(data.counters[22]) * 1e9 / wall,
                        static_cast<double>(data.counters[30]) * 1e9 / wall);
    line += fmt::format(" | waits ended by: memory {:.0f} query release {:.0f} QMD release {:.0f} drain {:.0f}",
                        static_cast<double>(data.counters[16]) * 1e9 / wall,
                        static_cast<double>(data.counters[17]) * 1e9 / wall,
                        static_cast<double>(data.counters[18]) * 1e9 / wall,
                        static_cast<double>(data.counters[19]) * 1e9 / wall);
    line += fmt::format(" | parameter drains {:.0f} (record flushes skipped {:.0f}), command list drains {:.0f}, engine write drains {:.0f}",
                        static_cast<double>(data.counters[23]) * 1e9 / wall,
                        static_cast<double>(data.counters[24]) * 1e9 / wall,
                        static_cast<double>(data.counters[25]) * 1e9 / wall,
                        static_cast<double>(data.counters[26]) * 1e9 / wall);
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    data = {};
    data.window_start = now;
}

} // namespace VideoCommon::Nx2Profile
