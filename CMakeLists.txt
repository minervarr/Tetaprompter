cmake_minimum_required(VERSION 3.22)

# The build tool and `--version` read the same file, so the version cannot
# drift between the app and the friendly front end.
file(READ "${CMAKE_CURRENT_LIST_DIR}/manifest.json" TP_MANIFEST)
string(JSON TP_VERSION GET "${TP_MANIFEST}" version)
project(Teleprompter_Vk VERSION ${TP_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TP_BUILD_TESTS "Build the pure-logic test executables (Debug only)" ON)
option(TP_BUILD_GUI "Build the Vulkan/Wayland teleprompter window" ON)
option(TP_APPLY_FRAMEWORK_PATCHES
       "Apply the small App_shell/Vk_Canvas patches recorded in patches/" ON)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TestHelpers)

# ---------------------------------------------------------------------------
# First-party framework submodules. The only two the product requires.
# ---------------------------------------------------------------------------
set(VK_CANVAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/firstparty/framework/Vk_Canvas_Lb_LAW"
    CACHE PATH "vk_canvas source root (consumed by app_shell too)" FORCE)
set(APP_SHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/firstparty/framework/App_shell"
    CACHE PATH "app_shell source root" FORCE)

# The framework changes this port needs are kept as patches so a plain
# `git submodule update --init --recursive` (which pins the upstream SHAs)
# reproduces the build. Apply them idempotently at configure time, one patch
# at a time: a patch already present in the tree is skipped, so extending the
# series does not disturb a checkout configured against the shorter one.
function(tp_apply_framework_series source_dir)
    if(NOT EXISTS "${source_dir}/.git")
        message(FATAL_ERROR
            "${source_dir} is not initialized; run "
            "`git submodule update --init --recursive` first")
    endif()
    set(patches ${ARGN})
    if(NOT patches)
        return()
    endif()

    # How much of the series the tree already carries. A patch can only be
    # reverse-checked while the ones after it are absent (later patches touch
    # context earlier ones added), so scan from the tail down: the first
    # reverse-check that succeeds is the last applied patch, and everything
    # before it is in the tree too.
    list(LENGTH patches patch_count)
    set(applied ${patch_count})
    set(reversed ${patches})
    list(REVERSE reversed)
    foreach(patch IN LISTS reversed)
        execute_process(
            COMMAND git -C "${source_dir}" apply --reverse --check "${patch}"
            RESULT_VARIABLE present
            OUTPUT_QUIET ERROR_QUIET)
        if(present EQUAL 0)
            break()
        endif()
        math(EXPR applied "${applied} - 1")
    endforeach()

    set(index 0)
    foreach(patch IN LISTS patches)
        math(EXPR index "${index} + 1")
        if(index LESS_EQUAL applied)
            continue()
        endif()
        execute_process(
            COMMAND git -C "${source_dir}" apply --check "${patch}"
            RESULT_VARIABLE applicable
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT applicable EQUAL 0)
            message(FATAL_ERROR
                "Framework patch does not apply: ${patch}\n"
                "The submodule may have moved or be partially patched; reset it "
                "with `git submodule update --force` and reconfigure, or pass "
                "-DTP_APPLY_FRAMEWORK_PATCHES=OFF if it is already patched.")
        endif()
        execute_process(
            COMMAND git -C "${source_dir}" apply "${patch}"
            RESULT_VARIABLE apply_result
            OUTPUT_QUIET ERROR_VARIABLE apply_err)
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Could not apply ${patch}: ${apply_err}")
        endif()
        message(STATUS "Applied framework patch: ${patch}")
    endforeach()
endfunction()

# A core-only build (TP_BUILD_GUI=OFF) must not need the submodules, the
# Vulkan loader, Wayland or slangc: it exists so `core` and its tests can be
# built on a machine that has none of them.
if(TP_BUILD_GUI)
    if(WIN32 OR ANDROID)
        message(FATAL_ERROR "The first teleprompter slice is Linux/Wayland.")
    endif()

    if(TP_APPLY_FRAMEWORK_PATCHES)
        file(GLOB TP_APP_SHELL_PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/000*-*.patch")
        list(SORT TP_APP_SHELL_PATCHES)
        tp_apply_framework_series("${APP_SHELL_DIR}" ${TP_APP_SHELL_PATCHES})
        tp_apply_framework_series("${VK_CANVAS_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/patches/vk-canvas-0001-dispatch-timerfd.patch")
    endif()

    # What this application tells its shell.
    set(APP_SHELL_STATE_HOME ".local/state/teleprompter" CACHE STRING "" FORCE)
    set(APP_SHELL_APP_NAME "teleprompter" CACHE STRING "" FORCE)
    set(APP_SHELL_WIN_CLASS "TeleprompterMain" CACHE STRING "" FORCE)
    set(APP_SHELL_WINDOW_TITLE "Teleprompter" CACHE STRING "" FORCE)
    set(APP_SHELL_APP_ID "io.github.nerio.Teleprompter" CACHE STRING "" FORCE)

    # app_shell's UI metrics include a generated legibility floor. We do not use
    # its role scale (the prompter's type is derived from monitor height
    # directly), but the library still compiles ui_metrics.cc, so provide the
    # header up front instead of building the standalone geometry tool on every
    # configure.
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
