# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(nx2-shader-check main.cpp)
target_link_libraries(nx2-shader-check PRIVATE common shader_recompiler fmt::fmt Threads::Threads)
target_include_directories(nx2-shader-check PRIVATE ${CMAKE_SOURCE_DIR}/src)
create_target_directory_groups(nx2-shader-check)
