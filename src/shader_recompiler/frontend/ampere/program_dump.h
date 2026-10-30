// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include "common/common_types.h"

namespace Shader::Ampere {

// Sidecar written next to an EDEN_NX2_SHADER_DUMP program (<name>.meta) with the state the
// offline checker (tools/nx2-shader-check) needs to compile the program the way the runtime does.
struct ProgramDumpInfo {
    static constexpr u32 Magic = 0x4853584e; // "NXSH"
    static constexpr u32 Version = 1;

    u32 magic = Magic;
    u32 version = Version;
    u32 stage;              ///< Shader::Stage
    u32 texture_bound;      ///< Constant buffer holding texture handles, before lowering overrides it
    std::array<u32, 3> workgroup_size;
    u32 shared_memory_size;
    u32 local_memory_size;
    std::array<u32, 8> gp_passthrough_mask;
    std::array<u8, 128> header; ///< Native program header (graphics); zero for compute
};
static_assert(sizeof(ProgramDumpInfo) == 196);

} // namespace Shader::Ampere
