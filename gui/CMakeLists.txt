add_executable(teleprompter
    src/main.cc
    src/app.cc
    src/text.cc
    src/layout.cc
    src/fonts.cc)

target_link_libraries(teleprompter PRIVATE
    teleprompter_core
    app_shell
    app_shell_wayland
    vk_canvas_core
    vk_canvas_wayland)

target_compile_definitions(teleprompter PRIVATE
    TP_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(teleprompter PRIVATE /W3 /utf-8)
else()
    target_compile_options(teleprompter PRIVATE -Wall -Wextra)
endif()

add_dependencies(teleprompter compile_font_shaders compile_canvas_shaders)

add_custom_target(teleprompter_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:teleprompter>/assets
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:teleprompter>/fonts
    COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:teleprompter>/samples
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/samples/test-script.txt
            $<TARGET_FILE_DIR:teleprompter>/samples/test-script.txt
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/assets $<TARGET_FILE_DIR:teleprompter>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets/fonts
            $<TARGET_FILE_DIR:teleprompter>/fonts
    COMMENT "Copying shaders and fonts next to teleprompter")
add_dependencies(teleprompter_assets compile_font_shaders compile_canvas_shaders)
add_dependencies(teleprompter teleprompter_assets)
