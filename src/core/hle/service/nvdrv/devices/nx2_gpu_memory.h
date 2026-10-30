// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Service::Nvidia::Devices {

// True inside a Switch 2 GPU-only region (nx2:gpu sub 214): the client never maps those, so
// nothing the GPU writes there is read back through host memory. Declared apart from
// nx2_stub_device.h so video_core can ask without pulling the service headers (whose NvResult
// names collide with X11 macros in renderer translation units).
bool IsNx2GpuOnlyAddress(u64 gpu_address);

} // namespace Service::Nvidia::Devices
