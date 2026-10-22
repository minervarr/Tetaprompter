add_library(teleprompter_core STATIC
    src/script.cc
    src/recents.cc
    src/recorder.cc
    src/settings.cc)

target_include_directories(teleprompter_core PUBLIC include)
target_compile_features(teleprompter_core PUBLIC cxx_std_17)
target_compile_definitions(teleprompter_core PUBLIC
    TP_SAMPLE_PATH="${CMAKE_SOURCE_DIR}/samples/test-script.txt")
target_link_libraries(teleprompter_core PUBLIC pthread)

if(MSVC)
    target_compile_options(teleprompter_core PRIVATE /W3 /utf-8)
else()
    target_compile_options(teleprompter_core PRIVATE -Wall -Wextra)
endif()
