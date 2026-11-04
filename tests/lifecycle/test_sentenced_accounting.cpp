// Executable lifecycle test for the dev/tc-memory-ports accounting change.
// Uses the repo's real DelayedDestructionRing header and a faithful mirror of
// the ported TickFrame/DeleteImage accounting. Physical model: an image's
// bytes count toward driver usage from construction until the ring destroys
// it, which is exactly the delayed-residency behavior under test.
// NOT covered: the real TextureCache integration (needs a Vulkan device);
// the mirror is verified line-by-line against the ported code.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include "../../src/video_core/delayed_destruction_ring.h"

using u64 = unsigned long long;

static u64 driver_usage = 0; // simulated runtime.GetDeviceMemoryUsage()
static int live = 0;         // net TrackedImage objects alive

struct TrackedImage {
    u64 bytes;
    TrackedImage(u64 b) : bytes(b) { ++live; driver_usage += bytes; }
    TrackedImage(TrackedImage&& o) noexcept : bytes(o.bytes) { ++live; driver_usage += bytes; }
    TrackedImage(const TrackedImage&) = delete;
    ~TrackedImage() { --live; driver_usage -= bytes; }
};

// --- mirror of the ported members (texture_cache_base.h) ---
static constexpr size_t TICKS_TO_DESTROY = 8;
VideoCommon::DelayedDestructionRing<TrackedImage, TICKS_TO_DESTROY> sentenced_images;
std::array<u64, TICKS_TO_DESTROY> sentenced_image_bytes{};
size_t sentenced_image_index = 0;

// --- mirror of the ported TickFrame driver-path accounting ---
static u64 effective_total_driver_path() {
    u64 total_used_memory = driver_usage;
    for (const u64 bytes : sentenced_image_bytes) {
        total_used_memory -= std::min(total_used_memory, bytes);
    }
    return total_used_memory;
}
static void tick_frame() {
    // Real order in the ported code: ring tick (destroys wrapped slot,
    // physically freeing those bytes) then index advance + slot zeroing.
    sentenced_images.Tick();
    sentenced_image_index = (sentenced_image_index + 1) % TICKS_TO_DESTROY;
    sentenced_image_bytes[sentenced_image_index] = 0;
}
// --- mirror of the ported DeleteImage tail (non-retained, non-immediate) ---
static u64 estimate_total = 0; // estimate path: subtracted at delete time
static void delete_image(u64 freed_bytes) {
    estimate_total -= std::min(estimate_total, freed_bytes); // estimate path untouched by ring
    sentenced_images.Push(TrackedImage{freed_bytes});        // temporary nets to zero on usage
    sentenced_image_bytes[sentenced_image_index] += freed_bytes;
}
static void reset_state() {
    // drain the ring fully (8 ticks) so no object leaks between cases
    for (size_t i = 0; i < TICKS_TO_DESTROY; ++i) tick_frame();
    sentenced_image_bytes.fill(0);
    sentenced_image_index = 0;
}

int main() {
    // 1. Ring destruction delay: pushed object destroyed exactly 8 ticks later,
    //    and its bytes stay in driver usage until that tick (physically resident).
    {
        reset_state();
        driver_usage = 0; live = 0;
        delete_image(400);
        assert(driver_usage == 400 && "deleted image must stay physically resident");
        for (int i = 0; i < 7; ++i) { tick_frame(); assert(live == 1 && driver_usage == 400); }
        tick_frame();
        assert(live == 0 && driver_usage == 0 && "must be destroyed and freed at wrap");
    }
    // 2. Accounting invariant across frames with two staggered deletes.
    {
        reset_state();
        driver_usage = 0; live = 0;
        const u64 other_usage = 600; // unrelated live allocations (editor's confound: kept distinct)
        driver_usage = other_usage;
        delete_image(400);           // slot0
        tick_frame();
        delete_image(300);           // slot1
        for (int frame = 0; frame < 7; ++frame) {
            assert(driver_usage == 600 + 400 + 300 && "residency changed early");
            assert(effective_total_driver_path() == 600 && "ring bytes not discounted");
            tick_frame();
        }
        // 8 ticks after the first push: 400 destroyed (physically freed), 300 still resident.
        assert(live == 1 && driver_usage == 600 + 300);
        assert(effective_total_driver_path() == 600 && "stale slot not zeroed at wrap");
        tick_frame();
        assert(live == 0 && driver_usage == 600);
        assert(effective_total_driver_path() == 600 && "residual discount after all frees");
    }
    // 3. Clamp: ring bytes larger than usage never underflows.
    {
        reset_state();
        driver_usage = 0; live = 0;
        driver_usage = 100; // less than the image about to be deleted
        delete_image(400);  // pushes driver_usage to 500; force the clamp case below
        driver_usage = 100; // (driver could report less than ring bytes after other frees)
        assert(effective_total_driver_path() == 0 && "clamp failed");
        for (size_t i = 0; i < TICKS_TO_DESTROY; ++i) tick_frame();
        assert(live == 0);
    }
    // 4. Accumulation exactness at physical extremes + clamp. (u64 slot
    //    accumulation cannot wrap in practice: values are VRAM bytes, < 2^48;
    //    a wrap would need ~16M TiB in one slot. Noted as a reviewed bound.)
    {
        reset_state();
        driver_usage = 0; live = 0;
        constexpr u64 TiB = 1ULL << 40;
        delete_image(TiB);
        delete_image(TiB); // same slot: bytes accumulate exactly
        assert(sentenced_image_bytes[sentenced_image_index] == 2 * TiB && "sum inexact");
        driver_usage = 3 * TiB;
        assert(effective_total_driver_path() == TiB && "exact subtraction failed");
        driver_usage = TiB; // reported usage below ring bytes: clamp
        assert(effective_total_driver_path() == 0 && "clamp failed");
    }
    // 5. Ring rollover: fill all 8 slots, then reuse; per-slot zeroing must not
    //    discount bytes of images pushed into a just-cleared slot this frame.
    {
        reset_state();
        driver_usage = 0; live = 0;
        driver_usage = 8000;
        for (int i = 0; i < 8; ++i) { delete_image(100); tick_frame(); }
        // All 8 original objects destroyed by now except the last push (slot of idx-1)...:
        // after 8 pushes+8 ticks, the first 7 pushes are destroyed; the 8th sits one slot back.
        // Core invariant: a fresh delete raises physical residency AND the
        // discount by the same amount, so the collector's effective total must
        // not move on the deleting frame.
        const u64 before = effective_total_driver_path();
        delete_image(1000);
        assert(effective_total_driver_path() == before && "delete moved the effective total");
    }
    // 6. Estimate path is never discounted by the ring (no double count).
    {
        reset_state();
        driver_usage = 0; live = 0; estimate_total = 1000;
        delete_image(400); // estimate subtracts at once
        assert(estimate_total == 600 && "estimate path must subtract at delete");
        tick_frame(); tick_frame();
        assert(estimate_total == 600 && "estimate path must not be re-discounted");
    }
    std::puts("all lifecycle accounting tests passed");
    return 0;
}
