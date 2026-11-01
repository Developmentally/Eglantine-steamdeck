// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

KeplerMemory::KeplerMemory(MemoryManager& memory_manager_)
    : upload_state{memory_manager_, regs.upload}, memory_manager{memory_manager_}
{}

KeplerMemory::~KeplerMemory() = default;

void KeplerMemory::FrontNoteMethod(u64 split_position, u32 method, u32 argument) {
    constexpr u32 upload_base = KEPLERMEMORY_REG_INDEX(upload);
    if (method >= upload_base && method < upload_base + front_upload.size()) {
        front_upload[method - upload_base] = argument;
        return;
    }
    if (method != KEPLERMEMORY_REG_INDEX(exec)) {
        return;
    }
    const u32 length = front_upload[0];
    const u32 lines = front_upload[1];
    const GPUVAddr address = (GPUVAddr{front_upload[2]} << 32) | front_upload[3];
    const u32 pitch = front_upload[4];
    const u64 span = u64{lines} * (std::max)(length, (argument & 1) != 0 ? length : pitch);
    if (const auto device = memory_manager.GpuToCpuAddress(address); device && span != 0) {
        Nx2PendingGpuWrites::Get().Note(*device, span, split_position);
    }
}

void KeplerMemory::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    upload_state.BindRasterizer(rasterizer_);

    execution_mask.reset();
    execution_mask[KEPLERMEMORY_REG_INDEX(exec)] = true;
    execution_mask[KEPLERMEMORY_REG_INDEX(data)] = true;
}

void KeplerMemory::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void KeplerMemory::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid KeplerMemory register, increase the size of the Regs structure");

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLERMEMORY_REG_INDEX(exec): {
        upload_state.ProcessExec(regs.exec.linear != 0);
        break;
    }
    case KEPLERMEMORY_REG_INDEX(data): {
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    }
}

void KeplerMemory::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    switch (method) {
    case KEPLERMEMORY_REG_INDEX(data):
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

} // namespace Tegra::Engines
