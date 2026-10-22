# ---------------------------------------------------------------------------
# teleprompter_add_test(<name> SOURCES <src>... [INCLUDES <dir>...] [LIBS <lib>...])
#
# Registers one assert-based test executable with CTest. The convention is the
# workspace's plain assert() with `#undef NDEBUG` in the source, no framework,
# Debug-only, run the binary directly.
#
# Debug-only is enforced HERE rather than at every call site: a Release
# configure simply produces no test targets, which is the intent — tests are
# never built into a Release app.
#
# Keep each call NARROW: link the one source file under test, not the whole
# library. If a test needs more, the source picked up a dependency it should
# not have.
#
# Ported from Klend-ario's cmake/TestHelpers.cmake.
# ---------------------------------------------------------------------------
function(teleprompter_add_test name)
    if(NOT TP_BUILD_TESTS)
        return()
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        return()
    endif()

    cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
    if(NOT T_SOURCES)
        message(FATAL_ERROR "teleprompter_add_test(${name}) requires SOURCES")
    endif()

    add_executable(${name} ${T_SOURCES})
    if(T_INCLUDES)
        target_include_directories(${name} PRIVATE ${T_INCLUDES})
    endif()
    if(T_LIBS)
        target_link_libraries(${name} PRIVATE ${T_LIBS})
    endif()
    if(MSVC)
        target_compile_options(${name} PRIVATE /W3 /utf-8)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME ${name} COMMAND ${name})
endfunction()
