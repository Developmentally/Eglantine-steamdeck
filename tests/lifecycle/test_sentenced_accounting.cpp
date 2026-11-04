// Executable lifecycle test for the dev/tc-memory-ports accounting change.
// Uses the repo's real DelayedDestructionRing header and a faithful mirror of
// the ported TickFrame/DeleteImage accounting.
//
// Physical model, matching production ownership: an image's bytes become
// driver-visible at REGISTRATION (allocation), stay visible while the image
// sits in the deletion ring (move TRANSFERS residency - the move adds nothing
// and the moved-from husk owns nothing), and stop being visible when the ring
// destroys the image. Unbacked images are modeled with 0 resident bytes: the
// driver never counted them.
//
// NOT covered: the real TextureCache integration (needs a Vulkan device);
// the mirror is verified line-by-line against the ported code but executes no
// production DeleteImage/TickFrame/SlotVector/destructor code.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>
#include "../../src/video_core/delayed_destruction_ring.h"

using u64 = unsigned long long;

static u64 driver_usage = 0; // simulated runtime.GetDeviceMemoryUsage()

struct TrackedImage {
    u64 resident; // driver-visible bytes (0 while unbacked)
    u64 estimate; // booked estimate (RegisterImage/DeleteImage accounting)
    bool gated;   // unbacked or sparse: estimate excluded from the discount
    TrackedImage(u64 r, u64 e, bool g) : resident(r), estimate(e), gated(g) {
        driver_usage += resident; // registration: the allocation becomes visible
    }
    // Move transfers residency: no new driver bytes, and the husk owns none.
    TrackedImage(TrackedImage&& o) noexcept
        : resident(std::exchange(o.resident, 0)), estimate(o.estimate), gated(o.gated) {}
    TrackedImage(const TrackedImage&) = delete;
    TrackedImage& operator=(const TrackedImage&) = delete;
    ~TrackedImage() { driver_usage -= resident; } // destruction frees what it still owns
};

// --- mirror of the ported members (texture_cache_base.h) ---
static constexpr size_t TICKS_TO_DESTROY = 8;
VideoCommon::DelayedDestructionRing<TrackedImage, TICKS_TO_DESTROY> sentenced_images;
std::array<u64, TICKS_TO_DESTROY> sentenced_image_bytes{};
size_t sentenced_image_index = 0;

// --- mirror of the ported TickFrame driver-path accounting ---
static u64 driver_used_memory = 0; // mirror of the raw member: telemetry reads this
static u64 effective_total_driver_path() {
    driver_used_memory = driver_usage;
    u64 sentenced_pending_bytes = 0;
    for (const u64 bytes : sentenced_image_bytes) {
        sentenced_pending_bytes += bytes;
    }
    return driver_used_memory - std::min(driver_used_memory, sentenced_pending_bytes);
}
static void tick_frame() {
    // Real order in the ported code: ring tick (destroys wrapped slot,
    // physically freeing those bytes) then index advance + slot zeroing.
    sentenced_images.Tick();
    sentenced_image_index = (sentenced_image_index + 1) % TICKS_TO_DESTROY;
    sentenced_image_bytes[sentenced_image_index] = 0;
}
// --- mirror of RegisterImage/DeleteImage (non-retained, non-immediate) ---
static u64 estimate_total = 0;   // estimate path: added at register, subtracted at delete
static std::vector<TrackedImage> registry; // live images; deleted ones stay as empty husks
static size_t register_image(u64 resident, u64 estimate, bool gated = false) {
    estimate_total += estimate;
    registry.emplace_back(resident, estimate, gated);
    return registry.size() - 1;
}
static void delete_image(size_t idx) {
    TrackedImage& img = registry[idx];
    estimate_total -= std::min(estimate_total, img.estimate); // estimate path untouched by ring
    // Ported gate: unbacked images were never backed; sparse images commit only populated
    // pages. Neither estimate tracks driver bytes, so both stay out of the discount.
    const u64 freed_bytes = img.gated ? 0 : img.estimate;
    sentenced_images.Push(std::move(img)); // transfer residency into the ring
    // Mirror of the ported checked add: on u64 overflow keep the previous total
    // (that delete's discount is dropped), never wrap.
    u64& slot_bytes = sentenced_image_bytes[sentenced_image_index];
    if (freed_bytes <= ~0ULL - slot_bytes) {
        slot_bytes += freed_bytes;
    }
}
static void reset_state() {
    registry.clear(); // destroy husks (all own 0 by then)
    // drain the ring fully (8 ticks) so no object leaks between cases
    for (size_t i = 0; i < TICKS_TO_DESTROY; ++i) tick_frame();
    sentenced_image_bytes.fill(0);
    sentenced_image_index = 0;
    driver_usage = 0;
    estimate_total = 0;
}

int main() {
    // 1. Ring destruction delay: a deleted image's bytes stay driver-visible
    //    (physically resident) for exactly 8 ticks after the delete.
    {
        reset_state();
        const size_t a = register_image(400, 400);
        assert(driver_usage == 400 && "registration must be driver-visible");
        delete_image(a);
        assert(driver_usage == 400 && "delete must not free: residency transfers to the ring");
        for (int i = 0; i < 7; ++i) { tick_frame(); assert(driver_usage == 400); }
        tick_frame();
        assert(driver_usage == 0 && "must be destroyed and freed at wrap");
    }
    // 2. Accounting invariant across frames with two staggered deletes and
    //    unrelated live usage kept distinct.
    {
        reset_state();
        register_image(600, 600);      // unrelated, never deleted
        const size_t a = register_image(400, 400);
        const size_t b = register_image(300, 300);
        delete_image(a);               // slot0
        tick_frame();
        delete_image(b);               // slot1
        for (int frame = 0; frame < 7; ++frame) {
            assert(driver_usage == 600 + 400 + 300 && "residency changed early");
            assert(effective_total_driver_path() == 600 && "ring bytes not discounted");
            tick_frame();
        }
        // 8 ticks after the first push: a destroyed (physically freed), b still resident.
        assert(driver_usage == 600 + 300);
        assert(effective_total_driver_path() == 600 && "stale slot not zeroed at wrap");
        tick_frame();
        assert(driver_usage == 600);
        assert(effective_total_driver_path() == 600 && "residual discount after all frees");
    }
    // 3. Clamp: ring bytes larger than reported usage never underflows.
    //    Other driver-side frees drop the report below the ring's booked bytes;
    //    fresh allocations raise it again before the ring destroys the image,
    //    so the model never destroys more than is currently visible.
    {
        reset_state();
        const size_t a = register_image(400, 400);
        delete_image(a);               // resident 400, discount 400
        driver_usage = 100;            // driver-side frees outside the cache's view
        assert(effective_total_driver_path() == 0 && "clamp failed");
        driver_usage = 400;            // new allocations before destruction
        for (size_t i = 0; i < TICKS_TO_DESTROY; ++i) tick_frame();
        assert(driver_usage == 0 && "ring must free exactly what it owns");
    }
    // 4. Accumulation exactness at physical extremes + clamp. Overflow itself is
    //    boundary-tested in case 9.
    {
        reset_state();
        constexpr u64 TiB = 1ULL << 40;
        const size_t a = register_image(TiB, TiB);
        const size_t b = register_image(TiB, TiB);
        delete_image(a);
        delete_image(b); // same slot: bytes accumulate exactly
        assert(sentenced_image_bytes[sentenced_image_index] == 2 * TiB && "sum inexact");
        driver_usage += TiB; // an unrelated TiB allocation
        assert(effective_total_driver_path() == TiB && "exact subtraction failed");
        driver_usage -= 2 * TiB; // driver-side frees drop the report below the ring bytes
        assert(effective_total_driver_path() == 0 && "clamp failed");
        driver_usage += 2 * TiB; // fresh allocations before the ring destroys anything
    }
    // 5. Ring rollover: pushes interleaved with ticks age one slot per frame.
    //    After 8 push+tick pairs only the FIRST-pushed image has been destroyed
    //    (aged 8 ticks); pushes 2..8 are still resident, aged 7..1 ticks.
    {
        reset_state();
        register_image(8000, 0); // unrelated resident usage
        std::vector<size_t> imgs;
        for (int i = 0; i < 8; ++i) { imgs.push_back(register_image(100, 100)); delete_image(imgs.back()); tick_frame(); }
        assert(driver_usage == 8000 + 700 && "only the first push may be destroyed");
        assert(effective_total_driver_path() == 8000 && "wrong pending total after rollover");
        // Core invariant: a register+delete pair raises physical residency AND the
        // discount by the same amount, so the collector's effective total must
        // not move on the deleting frame.
        const u64 before = effective_total_driver_path();
        const size_t c = register_image(1000, 1000);
        delete_image(c);
        assert(effective_total_driver_path() == before && "delete moved the effective total");
    }
    // 6. Estimate path is never discounted by the ring (no double count).
    {
        reset_state();
        const size_t a = register_image(400, 400); // estimate 400 booked at register
        estimate_total = 1000;                     // plus unrelated booked estimate
        delete_image(a);                           // estimate subtracts at once
        assert(estimate_total == 600 && "estimate path must subtract at delete");
        tick_frame(); tick_frame();
        assert(estimate_total == 600 && "estimate path must not be re-discounted");
    }
    // 7. Gated images (unbacked/sparse) stay out of the discount: deleting one
    //    changes neither the discount nor the effective total. An unbacked
    //    image is modeled with 0 resident bytes - the driver never saw it.
    {
        reset_state();
        register_image(700, 700);                        // unrelated
        const size_t u = register_image(0, 500, true);   // unbacked: no driver bytes
        delete_image(u);
        assert(sentenced_image_bytes[sentenced_image_index] == 0 && "gated bytes booked");
        assert(driver_usage == 700 && "unbacked image counted by the driver");
        assert(effective_total_driver_path() == 700 && "gate changed the effective total");
        tick_frame();
        const size_t n = register_image(400, 400);       // normal image, next slot
        delete_image(n);
        assert(effective_total_driver_path() == 700 && "normal delete moved effective total");
        tick_frame();
        assert(driver_usage == 1100 && "residency wrong");
        assert(effective_total_driver_path() == 700 && "discount did not stick");
    }
    // 8. Raw driver figure stays separate from the collector figure while a
    //    discount is pending (telemetry honesty split).
    {
        reset_state();
        register_image(900, 0); // unrelated resident usage
        const size_t a = register_image(300, 300);
        delete_image(a);
        const u64 collector = effective_total_driver_path();
        assert(driver_used_memory == 1200 && "raw figure lost");
        assert(collector == 900 && "discount missing from collector figure");
        assert(driver_used_memory - collector == 300 && "split mismatch");
    }
    // 9. Checked accumulation boundary: when the slot total would overflow u64,
    //    the add is dropped (slot keeps its prior total) instead of wrapping.
    {
        reset_state();
        const size_t a = register_image(100, 100);
        delete_image(a);
        sentenced_image_bytes[sentenced_image_index] = ~0ULL - 10; // force near-overflow
        const size_t b = register_image(100, 100);
        delete_image(b); // 100 > 10 headroom: add must be dropped, not wrapped
        assert(sentenced_image_bytes[sentenced_image_index] == ~0ULL - 10 &&
               "overflow wrapped the slot total");
        // The kept total still discounts correctly (clamped at the driver figure).
        assert(effective_total_driver_path() == 0 && "kept total misapplied");
    }
    std::puts("all lifecycle accounting tests passed");
    return 0;
}
