cmake_minimum_required(VERSION 3.22)
project(Teleprompter_Vk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TP_BUILD_TESTS "Build the pure-logic test executables" ON)
option(TP_BUILD_GUI "Build the Vulkan/Wayland teleprompter window" ON)

include(CTest)

# ---------------------------------------------------------------------------
# First-party framework submodules. The only two the product requires.
# ---------------------------------------------------------------------------
set(VK_CANVAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/firstparty/framework/Vk_Canvas_Lb_LAW"
    CACHE PATH "vk_canvas source root (consumed by app_shell too)" FORCE)
set(APP_SHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/firstparty/framework/App_shell"
    CACHE PATH "app_shell source root" FORCE)

# What this application tells its shell.
set(APP_SHELL_STATE_HOME ".local/state/teleprompter" CACHE STRING "" FORCE)
set(APP_SHELL_APP_NAME "teleprompter" CACHE STRING "" FORCE)
set(APP_SHELL_WIN_CLASS "TeleprompterMain" CACHE STRING "" FORCE)
set(APP_SHELL_WINDOW_TITLE "Teleprompter" CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID "io.github.nerio.Teleprompter" CACHE STRING "" FORCE)

# app_shell's UI metrics include a generated legibility floor. We do not use its
# role scale (the prompter's type is derived from monitor height directly), but
# the library still compiles ui_metrics.cc, so provide the header up front
# instead of building the standalone geometry tool on every configure.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${CMAKE_BINARY_DIR}/generated/ui_min_text_size.gen.h"
"// Generated for Teleprompter_Vk. The prompter has its own monitor-derived scale.\n#pragma once\nconstexpr float kMinReadableTextSizePx = 16.0000f;\n")

add_subdirectory("${VK_CANVAS_DIR}/core" "${CMAKE_BINARY_DIR}/vk_canvas_build")

# app_shell's Debug test targets depend on this name; our header is already
# generated above, so the target is intentionally empty.
if(NOT TARGET generate_ui_min_text_size)
    add_custom_target(generate_ui_min_text_size)
endif()
add_subdirectory("${APP_SHELL_DIR}" "${CMAKE_BINARY_DIR}/app_shell_build")

if(TP_BUILD_GUI)
    if(WIN32 OR ANDROID)
        message(FATAL_ERROR "The first teleprompter slice is Linux/Wayland.")
    endif()

    add_subdirectory("${VK_CANVAS_DIR}/platform/linux" "${CMAKE_BINARY_DIR}/vk_canvas_linux_build")

    include("${VK_CANVAS_DIR}/cmake/VceShaders.cmake")
    if(NOT EXISTS "${VCE_SLANGC}")
        find_program(TP_SLANGC slangc)
        if(TP_SLANGC)
            set(VCE_SLANGC "${TP_SLANGC}" CACHE FILEPATH "Slang compiler" FORCE)
        else()
            message(FATAL_ERROR "slangc not found; install the Vulkan SDK or set -DVCE_SLANGC")
        endif()
    endif()

    vce_compile_slang(compile_font_shaders
        "${CMAKE_BINARY_DIR}/assets/shaders"
        "${VK_CANVAS_DIR}/first_party/vulkan_font_engine/shaders_src"
        composite_vert composite_frag tiling coverage msdf_vert msdf_frag
        glyph_area glyph_resolve)

    vce_compile_slang(compile_canvas_shaders
        "${CMAKE_BINARY_DIR}/assets/shaders"
        "${VK_CANVAS_DIR}/shaders_src"
        overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

    add_subdirectory(gui)
endif()

add_subdirectory(core)

if(TP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
