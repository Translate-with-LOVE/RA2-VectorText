# SPDX-FileCopyrightText: 2026 VectorText contributors
# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.21)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Compiler probes should use the same release toolchain as the default build.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_C_FLAGS_INIT "/utf-8")
set(CMAKE_CXX_FLAGS_INIT "/utf-8")
project(VectorText LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "VectorText requires the MSVC x86 toolchain (vcvars32.bat).")
endif()
if(MSVC_VERSION LESS 1929 OR MSVC_VERSION GREATER_EQUAL 1950)
    message(FATAL_ERROR "Windows 7 requires MSVC 14.29 or 14.3x/14.4x; VS 2026's 14.50+ runtime is unsupported. Use tools/cmake_build.bat with a compatible installed toolset.")
endif()
include(cmake/msvc_utf8_dependencies.cmake)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    WINVER=0x0601 _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000)

foreach(dependency freetype minhook)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}/include")
        message(FATAL_ERROR "Missing ${dependency} submodule. Run git submodule update --init --recursive.")
    endif()
endforeach()

# Keep the existing minimal, static FreeType module set; no external packages.
set(ft "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype")
add_library(vt_freetype STATIC
    ${ft}/src/base/ftsystem.c ${ft}/src/base/ftinit.c ${ft}/src/base/ftdebug.c
    ${ft}/src/base/ftbase.c ${ft}/src/base/ftbitmap.c ${ft}/src/base/ftmm.c
    ${ft}/src/base/ftgasp.c ${ft}/src/gzip/ftgzip.c ${ft}/src/truetype/truetype.c
    ${ft}/src/sfnt/sfnt.c ${ft}/src/smooth/smooth.c ${ft}/src/raster/raster.c
    ${ft}/src/autofit/autofit.c ${ft}/src/psnames/psmodule.c)
target_include_directories(vt_freetype PUBLIC ${ft}/include PRIVATE third_party)
target_compile_definitions(vt_freetype PRIVATE FT2_BUILD_LIBRARY FT_CONFIG_MODULES_H="ftmodule.min.h")
target_compile_options(vt_freetype PRIVATE /W3 /wd4819)

add_library(vt_minhook STATIC
    third_party/minhook/src/buffer.c third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde32.c)
target_include_directories(vt_minhook PUBLIC third_party/minhook/include)
target_compile_options(vt_minhook PRIVATE /W3)

# This core is shared by the DLL and offline tests. Presentation callbacks are
# supplied by each test where appropriate, so its own sources stay separate.
add_library(vt_glyphs STATIC
    src/GlyphSource.cpp src/glyph/Fonts.cpp src/glyph/Settings.cpp
    src/glyph/Raster.cpp
    src/PixelWriter.cpp)
target_include_directories(vt_glyphs PUBLIC src include)
target_link_libraries(vt_glyphs PUBLIC vt_freetype)
target_compile_options(vt_glyphs PRIVATE /W4)
add_library(vt_config STATIC src/Config.cpp)
target_include_directories(vt_config PUBLIC src)
target_compile_options(vt_config PRIVATE /W4)
add_library(vt_takeover STATIC
    src/Takeover.cpp src/takeover/LineLayout.cpp src/takeover/Measurement.cpp
    src/takeover/Diagnostics.cpp src/Logger.cpp)
target_link_libraries(vt_takeover PUBLIC vt_glyphs vt_config)
target_compile_options(vt_takeover PRIVATE /W4)
# Math depends only on leaf pixel types; no reverse link to the plane or glyphs.
add_library(vt_plane_math STATIC src/plane/PixelMath.cpp)
target_compile_options(vt_plane_math PRIVATE /W4)
add_library(vt_plane STATIC
    src/PixelPlane.cpp src/plane/Paint.cpp src/plane/Composition.cpp)
target_link_libraries(vt_plane PUBLIC vt_glyphs PRIVATE vt_plane_math)
target_compile_options(vt_plane PRIVATE /W4)

# Public entry points stay in the top-level files; private modules own their
# state and algorithms. The display test compiles the same presentation sources.
set(VT_HOOK_SOURCES
    src/Hooks.cpp src/hooks/Observation.cpp src/hooks/Metrics.cpp
    src/hooks/Background.cpp src/hooks/Text.cpp)
set(VT_PRESENTATION_SOURCES
    src/Presentation32.cpp src/presentation/State.cpp src/presentation/Surfaces.cpp
    src/presentation/Gdi.cpp src/presentation/D3d9.cpp src/presentation/OpenGL.cpp
    src/presentation/Atlas.cpp src/presentation/GlOverlay.cpp)
add_library(VectorText SHARED src/DllMain.cpp src/RuntimeHooks.cpp src/RuntimeHookStub.cpp ${VT_HOOK_SOURCES} ${VT_PRESENTATION_SOURCES})
target_compile_definitions(VectorText PRIVATE SYR_VER=2)
target_compile_options(VectorText PRIVATE /W4 /we4834)
target_link_libraries(VectorText PRIVATE vt_takeover vt_plane vt_minhook)
target_link_options(VectorText PRIVATE /SUBSYSTEM:WINDOWS,6.01 /OSVERSION:6.1 /MACHINE:X86 /INCREMENTAL:NO)
set_target_properties(VectorText PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(VectorText PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# Keep every normal build's DLL directory redistributable, including when the
# DLL is already up to date but a notice or the sample configuration changed.
add_custom_target(distribution_files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/VectorText.ini"
            "${CMAKE_BINARY_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES" "${CMAKE_BINARY_DIR}/bin/LICENSES"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/docs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/rendering.md" "${CMAKE_BINARY_DIR}/bin/docs/rendering.md"
    VERBATIM)

set(VT_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Game deployment directory")
set(VT_RA2_GAME_DIR "${VT_GAME_DIR}" CACHE PATH "Original RA2 directory for native ABI tests")
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:VectorText>" "${VT_GAME_DIR}/VectorText.dll"
    COMMAND ${CMAKE_COMMAND} "-DSOURCE_INI=${CMAKE_CURRENT_SOURCE_DIR}/VectorText.ini"
            "-DDEST_INI=${VT_GAME_DIR}/VectorText.ini" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_config.cmake"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VT_GAME_DIR}/VectorText-licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "${VT_GAME_DIR}/VectorText-licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES" "${VT_GAME_DIR}/VectorText-licenses/LICENSES"
    DEPENDS VectorText distribution_files VERBATIM)

include(CTest)
if(BUILD_TESTING)
    function(vt_test_executable target directory)
        add_executable(${target} ${ARGN})
        target_compile_options(${target} PRIVATE /W4 /we4834)
        target_compile_definitions(${target} PRIVATE VT_GAME_DIR="${VT_GAME_DIR}" VT_FONT_DIR="${VT_GAME_DIR}")
        target_include_directories(${target} PRIVATE src include)
        target_link_options(${target} PRIVATE /SUBSYSTEM:CONSOLE,6.01 /OSVERSION:6.1 /MACHINE:X86)
        set(out "${CMAKE_CURRENT_SOURCE_DIR}/build/${directory}")
        file(MAKE_DIRECTORY "${out}")
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${out}")
        foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
            set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${out}")
        endforeach()
    endfunction()
    vt_test_executable(baseline_test baseline-test tools/baseline_test.cpp)
    target_link_libraries(baseline_test PRIVATE vt_plane)
    vt_test_executable(line_render_test line-test tools/line_render_test.cpp src/RuntimeHookStub.cpp)
    target_link_libraries(line_render_test PRIVATE vt_takeover)
    add_dependencies(line_render_test VectorText)
    target_compile_definitions(line_render_test PRIVATE VT_RA2_GAME_DIR="${VT_RA2_GAME_DIR}")
    vt_test_executable(render_quality render-qa tools/render_quality_test.cpp)
    target_link_libraries(render_quality PRIVATE vt_takeover vt_plane)
    vt_test_executable(takeover_test test tools/takeover_test.cpp)
    target_link_libraries(takeover_test PRIVATE vt_takeover)
    vt_test_executable(hooktest_draw test tools/hooktest_draw.cpp)
    add_dependencies(hooktest_draw VectorText)
    vt_test_executable(unsupported_host_test host-test tools/unsupported_host_test.cpp)
    add_dependencies(unsupported_host_test VectorText)
    vt_test_executable(present32_test present32-test tools/present32_test.cpp)
    target_link_libraries(present32_test PRIVATE vt_plane)
    vt_test_executable(pixel_contract_test pixel-contract-test tools/pixel_contract_test.cpp)
    target_link_libraries(pixel_contract_test PRIVATE vt_plane)
    vt_test_executable(runtime_transaction_test runtime-transaction-test tools/runtime_transaction_test.cpp src/RuntimeHookStub.cpp)
    vt_test_executable(config_validation_test config-validation-test tools/config_validation_test.cpp)
    target_link_libraries(config_validation_test PRIVATE vt_config)
    vt_test_executable(scaled_composition_test scaled-test tools/scaled_composition_test.cpp)
    target_link_libraries(scaled_composition_test PRIVATE vt_plane)
    vt_test_executable(cnc_present_test cnc-test tools/cnc_present_test.cpp
        ${VT_PRESENTATION_SOURCES} src/presentation/TestAccess.cpp)
    target_compile_definitions(cnc_present_test PRIVATE VT_PRESENT_TEST)
    target_link_libraries(cnc_present_test PRIVATE vt_takeover vt_plane vt_minhook user32 gdi32)

    function(vt_check name target)
        add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
            "-DEXECUTABLE=$<TARGET_FILE:${target}>" "-DARGS=${ARGN}"
            "-DSOURCE_INI=${CMAKE_CURRENT_SOURCE_DIR}/VectorText.ini"
            "-DSOURCE_DLL=$<TARGET_FILE:VectorText>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_test.cmake")
        # Several checks intentionally rewrite their isolated INI/log files.
        set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE)
    endfunction()
    vt_check(baseline baseline_test)
    vt_check(line line_render_test)
    vt_check(unsupported_host unsupported_host_test)
    if(EXISTS "${VT_RA2_GAME_DIR}/game.exe")
        vt_check(line_ra2 line_render_test --ra2)
    endif()
    foreach(value true false numeric missing invalid)
        vt_check(config_${value} line_render_test --config-encoding ${value})
    endforeach()
    foreach(mode off observe draw unknown)
        vt_check(hook_${mode} hooktest_draw --mode ${mode})
    endforeach()
    vt_check(takeover takeover_test)
    vt_check(takeover_mono takeover_test --aa 0)
    vt_check(present32 present32_test)
    vt_check(pixel_contract pixel_contract_test)
    vt_check(runtime_transaction runtime_transaction_test)
    vt_check(config_validation config_validation_test)
    vt_check(quality_rgb565 render_quality raster-after.bmp --check --line)
    vt_check(quality_rgb565_scene render_quality raster-scene.bmp --check --line --scene)
    vt_check(quality_bgra render_quality bgra-after.bmp --check --line --bgra)
    vt_check(quality_bgra_scene render_quality bgra-scene.bmp --check --line --scene --bgra)
    vt_check(quality_guides render_quality bgra-guides.bmp --check --line --bgra --guides)
    vt_check(quality_loading_2x render_quality loading-2x.bmp --check --line --bgra --loading --hidpi)
    vt_check(quality_subtitles_2x render_quality subtitles-2x.bmp --check --line --bgra --subtitles --hidpi)
    vt_check(quality_yellow_subtitles_5x render_quality yellow-subtitles-5x.bmp --check --line --bgra --yellow-subtitles --hidpi --raster 5)
    vt_check(quality_startup_5x render_quality startup-5x.bmp --check --line --bgra --startup --hidpi --raster 5)
    vt_check(cpu_loading_text cnc_present_test --cpu-text)
    vt_check(startup_before_present cnc_present_test --startup-offline)
    vt_check(startup_hidpi_disabled cnc_present_test --startup-offline disabled)
    vt_check(async_movie_frames cnc_present_test --movie-frames)
    vt_check(scaled_composition scaled_composition_test)
    vt_check(fractional_overlay cnc_present_test --fractional-offline)
    # These checks need an interactive desktop; the offline preset excludes them.
    add_test(NAME cnc_display COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/cnc_present_test.bat")
    set_tests_properties(cnc_display PROPERTIES LABELS display RUN_SERIAL TRUE
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
