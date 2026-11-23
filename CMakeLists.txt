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

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TestHelpers)

# ---------------------------------------------------------------------------
# First-party framework submodules. The only two the product requires.
# ---------------------------------------------------------------------------
set(VK_CANVAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/firstparty/framework/Vk_Canvas_Lb_LAW"
    CACHE PATH "vk_canvas source root (consumed by app_shell too)" FORCE)
set(APP_SHELL_DIR "${VK_CANVAS_DIR}/app_shell"
    CACHE PATH "app_shell source root (inside the consolidated Vk_Canvas repo)" FORCE)

# A core-only build (TP_BUILD_GUI=OFF) must not need the submodules, the
# Vulkan loader, Wayland or slangc: it exists so `core` and its tests can be
# built on a machine that has none of them.
if(TP_BUILD_GUI)
    if(WIN32 OR ANDROID)
        message(FATAL_ERROR "The first teleprompter slice is Linux/Wayland.")
    endif()

    # What this application tells its shell.
    set(APP_SHELL_STATE_HOME ".local/state/teleprompter" CACHE STRING "" FORCE)
    set(APP_SHELL_APP_NAME "teleprompter" CACHE STRING "" FORCE)
    set(APP_SHELL_WIN_CLASS "TeleprompterMain" CACHE STRING "" FORCE)
    set(APP_SHELL_WINDOW_TITLE "Teleprompter" CACHE STRING "" FORCE)
    set(APP_SHELL_APP_ID "io.github.nerio.Teleprompter" CACHE STRING "" FORCE)

    add_subdirectory("${VK_CANVAS_DIR}/core" "${CMAKE_BINARY_DIR}/vk_canvas_build")

    # app_shell lives inside the consolidated Vk_Canvas repo and no longer
    # includes a generated legibility header; the prompter derives its own
    # monitor-height scale, so there is nothing to generate here.
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
        composite_vert composite_frag msdf_vert msdf_frag
        glyph_area glyph_resolve)
    # Compute entry points need the explicit stage.
    vce_compile_slang_stage(compile_font_compute_shaders
        "${CMAKE_BINARY_DIR}/assets/shaders"
        "${VK_CANVAS_DIR}/first_party/vulkan_font_engine/shaders_src"
        compute tiling coverage)

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
