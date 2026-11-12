cmake_minimum_required(VERSION 3.20)
project(doa2u_recomp_runtime C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MINGW)
    # Match the Windows SDK default so Windows 10 media and audio declarations exist.
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)
    # Link libc++ and libunwind in so the runner needs no toolchain DLLs.
    add_link_options(-static)
endif()

# Official SDL3 VC build for the Windows input host; SDL3.dll is copied beside
# each target that uses it. macOS and Linux render, play audio and read input
# through SDL3 (docs/building.md).
if(WIN32)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-VC.zip
        URL_HASH SHA256=1a784cb2a5c64d56fe7a62090fe9d242d9865f235e4ea9678f1a6ba4e693e7de)
    FetchContent_MakeAvailable(SDL3)
    find_package(SDL3 3.4 CONFIG REQUIRED PATHS "${sdl3_SOURCE_DIR}/cmake" NO_DEFAULT_PATH)
    function(recomp_use_sdl3 target)
        target_link_libraries(${target} PRIVATE SDL3::SDL3)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E
            copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>")
    endfunction()
    # The Vulkan renderer compiles its HLSL with shaderc at run time. LWJGL's
    # build is one self-contained DLL (BSD-3-Clause and Apache-2.0; see NOTICE).
    set(_shaderc_jar "${CMAKE_CURRENT_BINARY_DIR}/lwjgl-shaderc-3.4.3-natives-windows.jar")
    file(DOWNLOAD https://repo1.maven.org/maven2/org/lwjgl/lwjgl-shaderc/3.4.3/lwjgl-shaderc-3.4.3-natives-windows.jar
        "${_shaderc_jar}"
        EXPECTED_HASH SHA256=e8e631d007b36edf1440cb4d42654a9b5c03bcb91b9c333c6a81ec6a3911f8bc)
    # Restoring the jar's file times fails on some player drives; SDL3 above skips them too (CMP0135).
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        set(_shaderc_touch TOUCH)
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_shaderc_jar}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/shaderc"
        PATTERNS windows/x64/org/lwjgl/shaderc/shaderc.dll ${_shaderc_touch})
    set(RECOMP_SHADERC_DLL "${CMAKE_CURRENT_BINARY_DIR}/shaderc/windows/x64/org/lwjgl/shaderc/shaderc.dll")
    function(recomp_use_shader_compiler target)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E
            copy_if_different "${RECOMP_SHADERC_DLL}" "$<TARGET_FILE_DIR:${target}>")
    endfunction()
else()
    # macOS builds need SDL3 (brew install sdl3). Linux uses the system SDL3,
    # or builds it when RECOMP_FETCH_SDL3 is on, as build_game.py asks (Ubuntu
    # 24.04 has no package). Hosts with neither only configure.
    option(RECOMP_FETCH_SDL3 "Build SDL3 when the system has none" OFF)
    find_package(SDL3 QUIET)
    if(APPLE AND NOT SDL3_FOUND)
        message(FATAL_ERROR "SDL3 is required: brew install sdl3")
    endif()
    if(NOT SDL3_FOUND AND RECOMP_FETCH_SDL3)
        if(POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)
        endif()
        include(FetchContent)
        # The release DOA3's Steam Deck build was tested with, pinned like the Windows zip.
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-3.4.12.tar.gz
            URL_HASH SHA256=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
        set(SDL3_FOUND TRUE)
    endif()
    function(recomp_use_sdl3 target)
        if(SDL3_FOUND)
            target_link_libraries(${target} PRIVATE SDL3::SDL3)
        endif()
    endfunction()
    # The SDL_GPU presenter compiles its HLSL with shaderc. Linux loads
    # libshaderc at run time; macOS links Homebrew's shaderc and turns the
    # SPIR-V into MSL with SPIRV-Cross (brew install shaderc spirv-cross).
    if(APPLE)
        find_path(RECOMP_SHADERC_INCLUDE shaderc/shaderc.h REQUIRED)
        find_library(RECOMP_SHADERC_LIBRARY shaderc_shared REQUIRED)
        find_path(RECOMP_SPIRV_CROSS_INCLUDE spirv_cross/spirv_cross_c.h REQUIRED)
        set(RECOMP_SPIRV_CROSS_LIBRARIES)
        foreach(_component c msl glsl hlsl cpp reflect core)
            find_library(_spirv_cross_${_component} spirv-cross-${_component} REQUIRED)
            list(APPEND RECOMP_SPIRV_CROSS_LIBRARIES "${_spirv_cross_${_component}}")
        endforeach()
    endif()
    function(recomp_use_shader_compiler target)
        if(APPLE)
            target_compile_definitions(${target} PRIVATE RECOMP_SHADERC_LINKED)
            target_include_directories(${target} PRIVATE
                "${RECOMP_SHADERC_INCLUDE}" "${RECOMP_SPIRV_CROSS_INCLUDE}")
            target_link_libraries(${target} PRIVATE "${RECOMP_SHADERC_LIBRARY}" ${RECOMP_SPIRV_CROSS_LIBRARIES})
        else()
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endfunction()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_compile_definitions(_GNU_SOURCE)
    # glibc keeps libm separate; macOS links it with libSystem.
    link_libraries(Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# Clang contracts a*b+c into FMA by default on AArch64. Keep IEEE operation
# order so guest float results match the x86 hosts and the Windows build.
# Generated code reads the same guest bytes as float and integer and relies on
# 32-bit wraparound, so GCC and Clang must not optimize on strict aliasing or
# signed overflow; MSVC does neither.
if(NOT MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>
        $<$<COMPILE_LANGUAGE:C,CXX>:-fno-strict-aliasing>
        $<$<COMPILE_LANGUAGE:C,CXX>:-fwrapv>)
endif()

# Same upstream SMAA revision as DOA3; keep its license with the source. Both
# presenters compile SMAA.hlsl: D3DCompile on Windows, shaderc elsewhere.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(smaa
    URL https://codeload.github.com/iryoku/smaa/tar.gz/71c806a838bdd7d517df19192a20f0c61b3ca29d
    URL_HASH SHA256=046a435f5713f8bfe8148fe6305c2abf1409fc2122bee6070061c675e1e55f57)
FetchContent_MakeAvailable(smaa)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${smaa_SOURCE_DIR}/SMAA.hlsl")
file(READ "${smaa_SOURCE_DIR}/SMAA.hlsl" _smaa_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _smaa_hex "${_smaa_hex}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/smaa_hlsl.inc" CONTENT "${_smaa_hex}")
set_source_files_properties(d3d_presenter_d3d11.cpp d3d_presenter_sdl_gpu.cpp
    PROPERTIES COMPILE_DEFINITIONS RECOMP_SMAA
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${smaa_SOURCE_DIR}/Textures")

# Dear ImGui draws the runtime's overlay text (the free camera's panel) over
# the game through the presenter (Direct3D 11 on Windows, SDL_GPU elsewhere);
# see overlay_*.cpp. Set FETCHCONTENT_SOURCE_DIR_IMGUI to configure offline.
if(WIN32 OR SDL3_FOUND)
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz
        URL_HASH SHA256=3872a5f90df78fced023c1945f4466b654fd74573370b77b17742149763a7a7c)
    FetchContent_MakeAvailable(imgui)
    add_library(recomp_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
    target_include_directories(recomp_imgui PUBLIC "${imgui_SOURCE_DIR}")
    if(WIN32)
        # The Vulkan renderer draws the overlay through SDL_GPU.
        target_sources(recomp_imgui PRIVATE "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp")
        target_link_libraries(recomp_imgui PUBLIC d3d11 d3dcompiler SDL3::SDL3)
    else()
        target_sources(recomp_imgui PRIVATE "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp")
        recomp_use_sdl3(recomp_imgui)
    endif()
endif()

# POSIX fibers switch stacks in assembly (native_fiber.c).
if(NOT WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(RECOMP_FIBER_ASM fiber_switch_arm64.S)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(RECOMP_FIBER_ASM fiber_switch_x86_64.S)
    endif()
    if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        set(RECOMP_FIBER_ASM fiber_switch_arm64.S)
    elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(RECOMP_FIBER_ASM fiber_switch_x86_64.S)
    endif()
    enable_language(ASM)
endif()

include(CTest)
option(RECOMP_NATIVE_AUDIO_TEST "Test PCM consumption on an installed audio endpoint" OFF)

# These targets use synthetic fixtures, with no guest address bindings.
add_executable(recomp_model_test
    model_test.c
    apu_model.c apu_model_test.c
    ohci_model.c ohci_model_test.c
    fiber_model.c fiber_model_test.c
    directory_model.c directory_model_test.c
    symbolic_link_model.c symbolic_link_model_test.c
    cri_service_model.c cri_service_model_test.c
    d3d_frame_model.c d3d_frame_model_test.c
    d3d_device_model.c d3d_device_model_test.c
    d3d_tile_model.c d3d_tile_model_test.c)
add_test(NAME recomp-runtime-models COMMAND recomp_model_test)

add_executable(recomp_manual_test program_manual.c program_manual_test.c)
add_test(NAME recomp-runtime-unbound-lookups COMMAND recomp_manual_test)
add_executable(recomp_netplay_guest_test netplay_guest.c netplay_guest_test.c)
add_test(NAME recomp-runtime-netplay-guest COMMAND recomp_netplay_guest_test)

add_executable(recomp_d3d8_adapter_test d3d8_adapter.c d3d8_adapter_test.c xapi_clock.cpp
    d3d_device_model.c d3d_frame_model.c d3d_tile_model.c d3d_vertex_shader_model.c d3d_card_model.c)
add_test(NAME recomp-runtime-d3d8-adapter COMMAND recomp_d3d8_adapter_test)
add_executable(recomp_stage_select_test stage_select.c stage_select_test.c)
add_test(NAME recomp-runtime-stage-select COMMAND recomp_stage_select_test)
add_executable(recomp_unlock_profile_test unlock_profile.c unlock_profile_test.c)
add_test(NAME recomp-runtime-unlock-profile COMMAND recomp_unlock_profile_test)
add_executable(recomp_pc_settings_test pc_settings.c runner_assets.c d3d_texture_model.c pc_settings_test.c)
add_test(NAME recomp-runtime-pc-settings COMMAND recomp_pc_settings_test)
add_executable(recomp_menu_quit_test menu_quit.c quit_icon.c quit_prompt.c runner_assets.c d3d_texture_model.c
    menu_quit_test.c)
add_test(NAME recomp-runtime-menu-quit COMMAND recomp_menu_quit_test)
add_executable(recomp_hit_voice_test hit_voice.c hit_voice_test.c)
add_test(NAME recomp-runtime-hit-voice COMMAND recomp_hit_voice_test)
add_executable(recomp_pc_settings_store_test pc_settings_store.cpp pc_settings_store_test.cpp)
add_test(NAME recomp-runtime-pc-settings-store COMMAND recomp_pc_settings_store_test)

add_executable(recomp_d3d8_state_test d3d8_state_adapter.c d3d8_state_adapter_test.c
    d3d_light_model.c d3d_vertex_shader_model.c d3d_render_state_model.c)
add_test(NAME recomp-runtime-d3d8-state COMMAND recomp_d3d8_state_test)
add_executable(recomp_d3d8_resource_test d3d8_resource_adapter.c d3d_texture_model.c
    d3d8_resource_adapter_test.c)
add_test(NAME recomp-runtime-d3d8-resource COMMAND recomp_d3d8_resource_test)
add_executable(recomp_d3d8_draw_test d3d8_draw_adapter.c d3d8_draw_adapter_test.c
    d3d_draw_model.c d3d_texture_model.c d3d_render_state_model.c d3d_vertex_shader_model.c)
add_test(NAME recomp-runtime-d3d8-draw COMMAND recomp_d3d8_draw_test)
add_executable(recomp_d3d_light_test d3d_light_model.c d3d_light_model_test.c)
add_test(NAME recomp-runtime-d3d-light COMMAND recomp_d3d_light_test)
add_executable(recomp_d3d_vertex_declaration_test
    d3d_vertex_shader_model.c d3d_vertex_shader_model_test.c)
add_test(NAME recomp-runtime-d3d-vertex-declaration COMMAND recomp_d3d_vertex_declaration_test)
if(NOT MSVC)
    target_link_libraries(recomp_manual_test PRIVATE m)
    target_link_libraries(recomp_pc_settings_test PRIVATE m)
    target_link_libraries(recomp_menu_quit_test PRIVATE m)
    target_link_libraries(recomp_model_test PRIVATE m)
    target_link_libraries(recomp_d3d8_adapter_test PRIVATE m)
    target_link_libraries(recomp_d3d8_state_test PRIVATE m)
    target_link_libraries(recomp_d3d8_draw_test PRIVATE m)
    target_link_libraries(recomp_d3d_light_test PRIVATE m)
endif()

add_executable(recomp_xbox_adpcm_test xbox_adpcm.c xbox_adpcm_test.c)
add_test(NAME recomp-runtime-xbox-adpcm COMMAND recomp_xbox_adpcm_test)

add_executable(recomp_movie_color_test movie_color_model.c movie_color_model_test.c)
add_test(NAME recomp-runtime-movie-color COMMAND recomp_movie_color_test)

add_executable(recomp_adx_decoder_test adx_decoder.c adx_decoder_test.c)
add_test(NAME recomp-runtime-adx-decoder COMMAND recomp_adx_decoder_test)

add_executable(recomp_sfd_format_test sfd_format.c sfd_format_test.c)
add_test(NAME recomp-runtime-sfd-format COMMAND recomp_sfd_format_test)

add_executable(recomp_dsound_hrtf_test dsound_hrtf.c dsound_hrtf_test.c)
if(NOT MSVC)
    target_link_libraries(recomp_dsound_hrtf_test PRIVATE m)
endif()
add_test(NAME recomp-runtime-dsound-hrtf COMMAND recomp_dsound_hrtf_test)

add_executable(recomp_dsound_buffer_test dsound_buffer_model.c dsound_buffer_model_test.c)
add_test(NAME recomp-runtime-dsound-buffer COMMAND recomp_dsound_buffer_test)
add_executable(recomp_dsound_device_test dsound_device_model.c dsound_device_model_test.c)
add_test(NAME recomp-runtime-dsound-device COMMAND recomp_dsound_device_test)
add_executable(recomp_dsound_api_test dsound_api_adapter.c dsound_device_model.c dsound_api_adapter_test.c)
add_test(NAME recomp-runtime-dsound-api COMMAND recomp_dsound_api_test)
add_executable(recomp_dsound_buffer_api_test dsound_buffer_adapter.c dsound_buffer_adapter_test.c
    dsound_buffer_model.c dsound_hrtf.c xbox_adpcm.c)
target_compile_definitions(recomp_dsound_buffer_api_test PRIVATE RECOMP_DSOUND_TEST_CLOCK)
add_test(NAME recomp-runtime-dsound-buffer-api COMMAND recomp_dsound_buffer_api_test)
add_executable(recomp_effects_image_test dsound_effects_image.c dsound_effects_image_test.c)
add_test(NAME recomp-runtime-effects-image COMMAND recomp_effects_image_test)
add_executable(recomp_effects_api_test dsound_effects_adapter.c dsound_effects_image.c dsound_effects_adapter_test.c)
add_test(NAME recomp-runtime-effects-api COMMAND recomp_effects_api_test)
add_executable(recomp_dsound_mix_test dsound_mix_model.c dsound_mix_model_test.c)
add_test(NAME recomp-runtime-dsound-mix COMMAND recomp_dsound_mix_test)
target_sources(recomp_dsound_buffer_api_test PRIVATE dsound_mix_model.c)
if(NOT MSVC)
    target_link_libraries(recomp_dsound_mix_test PRIVATE m)
    target_link_libraries(recomp_dsound_buffer_api_test PRIVATE m)
endif()

add_executable(recomp_save_transaction_test save_transaction.cpp save_transaction_test.cpp)
add_test(NAME recomp-runtime-save-transaction COMMAND recomp_save_transaction_test)

add_executable(recomp_controller_settings_test controller_settings.cpp controller_settings_test.cpp)
add_test(NAME recomp-runtime-controller-settings COMMAND recomp_controller_settings_test)

add_executable(recomp_input_adapter_test input_adapter.c input_model.c input_adapter_test.c
    runtime.c stop_report.c kernel_ordinals.c apu_model.c ohci_model.c)
add_test(NAME recomp-runtime-input-adapter COMMAND recomp_input_adapter_test)

add_executable(recomp_input_pulse_source_test input_pulse_source.c input_pulse_source_test.c)
target_compile_definitions(recomp_input_pulse_source_test PRIVATE RECOMP_INPUT_PULSE_SOURCE_STANDALONE)
add_test(NAME recomp-runtime-input-pulses COMMAND recomp_input_pulse_source_test)

add_library(recomp_keyboard STATIC input_keyboard_win32.c)
add_executable(recomp_input_host_test input_host_win32.c input_host_win32_test.c)
target_compile_definitions(recomp_input_host_test PRIVATE RECOMP_INPUT_TEST_FOCUSED)
target_link_libraries(recomp_input_host_test PRIVATE recomp_keyboard)
recomp_use_sdl3(recomp_input_host_test)
if(WIN32)
    target_link_libraries(recomp_input_host_test PRIVATE user32)
    add_test(NAME recomp-runtime-input-host-sdl COMMAND recomp_input_host_test)
elseif(SDL3_FOUND)
    add_test(NAME recomp-runtime-input-host-sdl COMMAND recomp_input_host_test)
endif()
if(TEST recomp-runtime-input-host-sdl)
    add_test(NAME recomp-runtime-input-host-keymap COMMAND recomp_input_host_test keymap)
    set_tests_properties(recomp-runtime-input-host-keymap PROPERTIES
        ENVIRONMENT "RECOMP_KEYMAP=a=20,bogus,start=74,x=53;RECOMP_TEST_FOCUSED=1")
endif()
if(WIN32)
    add_library(recomp_audio STATIC audio_output_xaudio2.cpp audio_xtalk_xapo.cpp
        audio_capture_xapo.cpp dsound_hrtf.c)
    target_include_directories(recomp_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    # mingw-w64 names the Windows 10 XAudio2 import library xaudio2_9.
    set(RECOMP_XAUDIO2 $<IF:$<BOOL:${MINGW}>,xaudio2_9,xaudio2>)
    target_link_libraries(recomp_audio PUBLIC ${RECOMP_XAUDIO2} ole32)
    add_executable(recomp_audio_output_test audio_output_xaudio2_test.cpp
        audio_xtalk_xapo.cpp dsound_hrtf.c)
    target_link_libraries(recomp_audio_output_test PRIVATE ole32)
    add_test(NAME recomp-runtime-audio-output COMMAND recomp_audio_output_test)
    # CXAPOBase (the audio recorder) and the SDK reverb reference exist only with MSVC.
    if(MSVC)
        target_link_libraries(recomp_audio PUBLIC xapobase)
        add_executable(recomp_audio_capture_test audio_capture_xapo_test.cpp)
        target_link_libraries(recomp_audio_capture_test PRIVATE xapobase)
        add_test(NAME recomp-runtime-audio-capture COMMAND recomp_audio_capture_test)
        add_executable(recomp_audio_host_test audio_output_xaudio2_host_test.cpp)
        target_sources(recomp_audio_host_test PRIVATE audio_xtalk_xapo.cpp dsound_hrtf.c)
        target_link_libraries(recomp_audio_host_test PRIVATE xaudio2 xapobase ole32)
        if(RECOMP_NATIVE_AUDIO_TEST)
            add_test(NAME recomp-runtime-audio-host COMMAND recomp_audio_host_test)
            set_tests_properties(recomp-runtime-audio-host PROPERTIES TIMEOUT 10)
        endif()
        add_executable(recomp_xaudio2_reverb_test xaudio2_reverb_test.cpp)
        add_test(NAME recomp-runtime-xaudio2-reverb COMMAND recomp_xaudio2_reverb_test)
    endif()
else()
    add_library(recomp_audio STATIC audio_output_sdl.cpp audio_reverb.cpp dsound_hrtf.c)
    target_include_directories(recomp_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    recomp_use_sdl3(recomp_audio)
    add_executable(recomp_audio_output_test audio_output_sdl_test.cpp)
    target_link_libraries(recomp_audio_output_test PRIVATE recomp_audio)
    recomp_use_sdl3(recomp_audio_output_test)
    if(SDL3_FOUND)
        add_test(NAME recomp-runtime-audio-output COMMAND recomp_audio_output_test)
    endif()
endif()
add_executable(recomp_d3d_vblank_test d3d_vblank.cpp xapi_clock.cpp d3d_vblank_test.cpp)
add_test(NAME recomp-runtime-d3d-vblank COMMAND recomp_d3d_vblank_test)
add_executable(recomp_xapi_clock_test xapi_clock.cpp xapi_clock_test.cpp)
add_test(NAME recomp-runtime-xapi-clock COMMAND recomp_xapi_clock_test)
add_executable(recomp_synctest_test synctest.c guest_snapshot.c synctest_test.c)
add_test(NAME recomp-runtime-synctest COMMAND recomp_synctest_test)
add_executable(recomp_rollback_test rollback.c rollback_test.c)
add_test(NAME recomp-runtime-rollback COMMAND recomp_rollback_test)
add_executable(recomp_replay_test replay.c replay_test.c)
add_test(NAME recomp-runtime-replay COMMAND recomp_replay_test)
add_executable(recomp_netplay_test netplay.c netplay_room.c netplay_names.c rollback.c netplay_test.c)
if(WIN32)
    target_link_libraries(recomp_netplay_test PRIVATE ws2_32)
endif()
add_test(NAME recomp-runtime-netplay COMMAND recomp_netplay_test)
add_executable(recomp_netplay_lobby_test netplay.c netplay_room.c netplay_names.c rollback.c netplay_lobby_test.cpp)
if(WIN32)
    target_link_libraries(recomp_netplay_lobby_test PRIVATE ws2_32)
endif()
add_test(NAME recomp-runtime-netplay-lobby COMMAND recomp_netplay_lobby_test)
add_executable(recomp_netplay_settings_test netplay_settings.cpp netplay_settings_test.cpp)
add_test(NAME recomp-runtime-netplay-settings COMMAND recomp_netplay_settings_test)
add_executable(recomp_netplay_profile_test netplay_profile.cpp netplay_profile_test.cpp)
target_link_libraries(recomp_netplay_profile_test PRIVATE recomp_xbe)
add_test(NAME recomp-runtime-netplay-profile COMMAND recomp_netplay_profile_test)
# One x87 transcendental implementation for every host (portable_math.h).
add_library(recomp_portable_math STATIC
    musl_math/sin.c musl_math/cos.c musl_math/tan.c musl_math/__sin.c musl_math/__cos.c musl_math/__tan.c
    musl_math/__rem_pio2.c musl_math/__rem_pio2_large.c musl_math/atan.c musl_math/atan2.c
    musl_math/log2.c musl_math/log1p.c musl_math/expm1.c)
target_include_directories(recomp_portable_math PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT WIN32)
    target_link_libraries(recomp_portable_math PUBLIC m)
endif()
add_executable(recomp_portable_math_test portable_math_test.c)
target_link_libraries(recomp_portable_math_test PRIVATE recomp_portable_math)
add_test(NAME recomp-runtime-portable-math COMMAND recomp_portable_math_test)
add_test(NAME recomp-runtime-unpaced COMMAND "${CMAKE_COMMAND}" -E env
    RECOMP_UNPACED=1 "$<TARGET_FILE:recomp_d3d_vblank_test>")
add_executable(recomp_d3d_capture_test d3d_presenter_capture_test.cpp d3d_presenter_capture.cpp)
add_test(NAME recomp-runtime-d3d-capture COMMAND recomp_d3d_capture_test)
# The draw shader and constants are shared; the rest of the presenter is
# Direct3D 11 on Windows and SDL_GPU elsewhere. Windows also builds the SDL_GPU
# presenter as its Vulkan renderer.
set(RECOMP_PRESENTER_SHARED d3d_draw_shader.cpp d3d_vertex_program.cpp d3d_draw_model.c
    d3d_texture_model.c d3d_vblank.cpp)
# Both presenters draw the runtime's overlay text and save screenshots.
set(RECOMP_PRESENTER_SHARED ${RECOMP_PRESENTER_SHARED} overlay_imgui_panel.cpp overlay_text.c screenshot.c)
if(WIN32)
    add_library(recomp_d3d_presenter STATIC
        d3d_presenter_d3d11.cpp d3d_presenter_thread.cpp d3d_presenter_capture.cpp overlay_d3d11.cpp
        ${RECOMP_PRESENTER_SHARED})
    target_include_directories(recomp_d3d_presenter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    # screenshot.c asks the shell for the Pictures folder.
    target_link_libraries(recomp_d3d_presenter PUBLIC d3d11 d3dcompiler dxgi user32 shell32 ole32 uuid
        recomp_keyboard recomp_imgui)
    set(_sdl_gpu_presenter recomp_d3d_presenter_vulkan)
else()
    set(_sdl_gpu_presenter recomp_d3d_presenter)
endif()
add_library(${_sdl_gpu_presenter} STATIC
    d3d_presenter_sdl_gpu.cpp d3d_presenter_inline.cpp d3d_shader_compiler.cpp overlay_sdl_gpu.cpp
    ${RECOMP_PRESENTER_SHARED})
target_include_directories(${_sdl_gpu_presenter} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${_sdl_gpu_presenter} PUBLIC recomp_keyboard)
if(TARGET recomp_imgui)
    target_link_libraries(${_sdl_gpu_presenter} PUBLIC recomp_imgui)
endif()
# xBRZ by Zenju (GPL-3.0; see NOTICE) redraws the game's 2D art larger once
# per texture (texture_upscale.cpp). PPSSPP keeps a copy in ext/xbrz; the
# build fetches it at one revision and patches out PPSSPP's own config header.
set(_xbrz_url https://raw.githubusercontent.com/hrydgard/ppsspp/9f286ee4ca1093ba7ad3541d9c66b68fbeda0f23/ext/xbrz)
set(_xbrz_dir "${CMAKE_CURRENT_BINARY_DIR}/xbrz")
file(DOWNLOAD ${_xbrz_url}/xbrz.cpp "${_xbrz_dir}/xbrz.cpp"
    EXPECTED_HASH SHA256=35283301d48ff518c8394c674195d086325697a51bf4d72213e636f1df213c7e)
file(DOWNLOAD ${_xbrz_url}/config.h "${_xbrz_dir}/config.h"
    EXPECTED_HASH SHA256=bbb7d3ab1c25851673a71c126188cc1f02dc483819f04af8a8241dac71e9fe1a)
file(DOWNLOAD ${_xbrz_url}/xbrz.h "${CMAKE_CURRENT_BINARY_DIR}/xbrz_upstream.h"
    EXPECTED_HASH SHA256=f0c59293edf8268abe7e749f017fe84c9ae38303aef2afc3244b2fdba9aeb69c)
include("${CMAKE_CURRENT_SOURCE_DIR}/xbrz_patch.cmake")
recomp_patch_xbrz("${CMAKE_CURRENT_BINARY_DIR}/xbrz_upstream.h" "${_xbrz_dir}/xbrz.h")
# One copy serves both renderers (Windows links the Direct3D 11 and Vulkan ones together).
add_library(recomp_texture_upscale STATIC texture_upscale.cpp "${_xbrz_dir}/xbrz.cpp")
target_include_directories(recomp_texture_upscale PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${_sdl_gpu_presenter} PUBLIC recomp_texture_upscale)
if(WIN32)
    target_link_libraries(recomp_d3d_presenter PUBLIC recomp_texture_upscale)
endif()
set_source_files_properties(texture_upscale.cpp "${_xbrz_dir}/xbrz.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_xbrz_dir}")
add_executable(recomp_texture_upscale_test texture_upscale.cpp "${_xbrz_dir}/xbrz.cpp" texture_upscale_test.cpp)
target_include_directories(recomp_texture_upscale_test PRIVATE "${_xbrz_dir}")
add_test(NAME recomp-runtime-texture-upscale COMMAND recomp_texture_upscale_test)
if(WIN32)
    # Tests and the runner tell the inline SDL_GPU presenter from the Direct3D 11 one.
    target_compile_definitions(${_sdl_gpu_presenter} PUBLIC RECOMP_PRESENTER_SDL_GPU)
    target_link_libraries(${_sdl_gpu_presenter} PUBLIC SDL3::SDL3)
else()
    recomp_use_sdl3(${_sdl_gpu_presenter})
    recomp_use_shader_compiler(${_sdl_gpu_presenter})
endif()
add_executable(recomp_d3d_presenter_test d3d_presenter_thread_test.cpp xapi_clock.cpp d3d_card_model.c)
target_link_libraries(recomp_d3d_presenter_test PRIVATE recomp_d3d_presenter)
if(NOT WIN32)
    target_sources(recomp_d3d_presenter_test PRIVATE win32_compat.c)
    recomp_use_sdl3(recomp_d3d_presenter_test)
endif()
if(WIN32 OR SDL3_FOUND)
    add_test(NAME recomp-runtime-d3d-presenter COMMAND recomp_d3d_presenter_test)
    set_tests_properties(recomp-runtime-d3d-presenter PROPERTIES TIMEOUT 60)
endif()
if(WIN32)
    add_executable(recomp_d3d_presenter_vulkan_test d3d_presenter_thread_test.cpp xapi_clock.cpp d3d_card_model.c)
    target_link_libraries(recomp_d3d_presenter_vulkan_test PRIVATE recomp_d3d_presenter_vulkan)
    recomp_use_sdl3(recomp_d3d_presenter_vulkan_test)
    recomp_use_shader_compiler(recomp_d3d_presenter_vulkan_test)
    add_test(NAME recomp-runtime-d3d-presenter-vulkan COMMAND recomp_d3d_presenter_vulkan_test)
    # The two presenter tests share the GPU; side by side they pass their timeouts.
    set_tests_properties(recomp-runtime-d3d-presenter-vulkan PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
        RESOURCE_LOCK gpu)
    set_tests_properties(recomp-runtime-d3d-presenter PROPERTIES RESOURCE_LOCK gpu)
endif()

if(WIN32 OR SDL3_FOUND)
    add_executable(recomp_perf_overlay_test perf_overlay.c perf_overlay_test.c)
    recomp_use_sdl3(recomp_perf_overlay_test)
    add_test(NAME recomp-runtime-perf-overlay COMMAND recomp_perf_overlay_test)
endif()
add_executable(recomp_free_camera_test free_camera.c free_camera_test.c)
add_test(NAME recomp-runtime-free-camera COMMAND recomp_free_camera_test)
add_executable(recomp_quit_prompt_test quit_prompt.c quit_prompt_test.c)
add_test(NAME recomp-runtime-quit-prompt COMMAND recomp_quit_prompt_test)
add_executable(recomp_overlay_text_test overlay_text.c overlay_text_test.c)
add_test(NAME recomp-runtime-overlay-text COMMAND recomp_overlay_text_test)
add_executable(recomp_scene_freeze_test scene_freeze.c scene_freeze_test.c)
add_test(NAME recomp-runtime-scene-freeze COMMAND recomp_scene_freeze_test)
add_executable(recomp_screenshot_test screenshot.c screenshot_test.c)
add_test(NAME recomp-runtime-screenshot COMMAND recomp_screenshot_test)
if(WIN32)
    # screenshot.c asks the shell for the Pictures folder.
    target_link_libraries(recomp_screenshot_test PRIVATE shell32 ole32 uuid)
endif()

add_executable(recomp_program_abi_call_test program_abi_call_test.c)
add_test(NAME recomp-runtime-program-abi-call COMMAND recomp_program_abi_call_test)

add_executable(recomp_guest_heap_test guest_heap.c guest_heap_test.c)
add_test(NAME recomp-runtime-guest-heap COMMAND recomp_guest_heap_test)
add_executable(recomp_guest_snapshot_test guest_snapshot.c guest_snapshot_test.c)
add_test(NAME recomp-runtime-guest-snapshot COMMAND recomp_guest_snapshot_test)

add_library(recomp_xbe STATIC ../xbe/sha.cpp ../xbe/xbe_parser.cpp runner_load.cpp)
target_include_directories(recomp_xbe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../xbe" "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(recomp_runner_load_test runner_load_test.cpp)
target_link_libraries(recomp_runner_load_test PRIVATE recomp_xbe)
add_test(NAME recomp-runtime-runner-load COMMAND recomp_runner_load_test)
add_executable(recomp_sha_test sha_test.cpp ../xbe/sha.cpp)
target_include_directories(recomp_sha_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../xbe")
add_test(NAME recomp-runtime-sha COMMAND recomp_sha_test)
if(WIN32)
    target_link_libraries(recomp_xbe PUBLIC bcrypt)
    target_link_libraries(recomp_sha_test PRIVATE bcrypt)
else()
    add_executable(recomp_win32_compat_test win32_compat.c win32_compat_test.c)
    add_test(NAME recomp-runtime-win32-compat COMMAND recomp_win32_compat_test)
endif()

    add_library(recomp_kernel STATIC
        runtime.c stop_report.c guest_heap.c guest_snapshot.c xapi_clock.cpp native_fiber.c
        fiber_model.c fiber_adapter.c
        kernel_config.c kernel_crypto.c kernel_debug.c kernel_device.c
        kernel_dispatch.c kernel_file.c kernel_interrupt.c kernel_memory.c
        kernel_ordinals.c kernel_rtl.c kernel_startup.c kernel_thread.c kernel_video.c
        device_model.c apu_model.c ohci_model.c directory_model.c symbolic_link_model.c
        save_transaction.cpp)
    target_include_directories(recomp_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(recomp_kernel PUBLIC bcrypt)
    else()
        target_sources(recomp_kernel PRIVATE win32_compat.c ${RECOMP_FIBER_ASM})
    endif()
    add_executable(recomp_kernel_test kernel_test.c kernel_device_test.c kernel_crypto_test.c
        kernel_file_test.c kernel_memory_test.c kernel_rtl_test.c kernel_thread_test.c kernel_video_test.c
        flag_macro_test.c sse_semantics_test.c)
    target_link_libraries(recomp_kernel_test PRIVATE recomp_kernel)
    add_test(NAME recomp-runtime-kernel COMMAND recomp_kernel_test)
    add_executable(recomp_kernel_scheduler_test kernel_scheduler_test.c)
    add_executable(recomp_fiber_adapter_test fiber_adapter_test.c)
    target_link_libraries(recomp_fiber_adapter_test PRIVATE recomp_kernel)
    add_test(NAME recomp-runtime-fiber-adapter COMMAND recomp_fiber_adapter_test)
    target_link_libraries(recomp_kernel_scheduler_test PRIVATE recomp_kernel)
    add_test(NAME recomp-runtime-kernel-scheduler COMMAND recomp_kernel_scheduler_test)
    set_tests_properties(recomp-runtime-kernel-scheduler PROPERTIES TIMEOUT 10)

set(RECOMP_PROGRAM_DIR "" CACHE PATH "Authenticated complete generated-source snapshot")
set(RECOMP_PROGRAM_MANIFEST_SHA256 "" CACHE STRING "SHA-256 of the generated-source manifest")
set(RECOMP_XBE_SHA256 "" CACHE STRING "SHA-256 of the executable used to generate this program")
set(RECOMP_PROGRAM_EBP_EXPECTED 0 CACHE STRING "Expected uninitialized generated EBP prologue count")
if((RECOMP_PROGRAM_DIR AND NOT RECOMP_PROGRAM_MANIFEST_SHA256) OR
   (RECOMP_PROGRAM_MANIFEST_SHA256 AND NOT RECOMP_PROGRAM_DIR))
    message(FATAL_ERROR "RECOMP_PROGRAM_DIR and RECOMP_PROGRAM_MANIFEST_SHA256 must be supplied together")
endif()

if(RECOMP_PROGRAM_DIR)
    get_filename_component(_program_dir "${RECOMP_PROGRAM_DIR}" ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
    get_filename_component(_program_dir "${_program_dir}" REALPATH)
    get_filename_component(_private_dir "${CMAKE_CURRENT_SOURCE_DIR}/../private" REALPATH)
    file(RELATIVE_PATH _private_relative "${_private_dir}" "${_program_dir}")
    if(_private_relative MATCHES [=[^\.\.]=] OR IS_ABSOLUTE "${_private_relative}")
        message(FATAL_ERROR "Generated program source must stay under private/")
    endif()
    file(GLOB _program_names CONFIGURE_DEPENDS LIST_DIRECTORIES false RELATIVE "${_program_dir}"
        "${_program_dir}/recomp_[0-9][0-9][0-9][0-9].c")
    list(SORT _program_names)
    if(NOT _program_names)
        message(FATAL_ERROR "Authenticated program has no source chunks")
    endif()
    set(_chunk_index 0)
    foreach(_name IN LISTS _program_names)
        string(REGEX REPLACE [=[^recomp_0*([0-9]+)\.c$]=] "\\1" _number "${_name}")
        if(NOT _number EQUAL _chunk_index)
            message(FATAL_ERROR "Authenticated program chunk sequence has a gap: ${_name}")
        endif()
        math(EXPR _chunk_index "${_chunk_index} + 1")
    endforeach()
    list(APPEND _program_names recomp_dispatch.c recomp_funcs.h)
    set(_program_manifest "")
    foreach(_name IN LISTS _program_names)
        set(_path "${_program_dir}/${_name}")
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR "Authenticated program member is missing: ${_name}")
        endif()
        file(SIZE "${_path}" _size)
        file(SHA256 "${_path}" _sha256)
        string(APPEND _program_manifest "${_name}\t${_size}\t${_sha256}\n")
    endforeach()
    string(SHA256 _manifest_sha256 "${_program_manifest}")
    string(TOLOWER "${RECOMP_PROGRAM_MANIFEST_SHA256}" _expected_manifest_sha256)
    if(NOT _manifest_sha256 STREQUAL _expected_manifest_sha256)
        message(FATAL_ERROR "RECOMP_PROGRAM_DIR does not match its authenticated manifest")
    endif()
    if(NOT RECOMP_PROGRAM_EBP_EXPECTED STREQUAL "0")
        message(FATAL_ERROR "Uninitialized generated EBP must be fixed in the lifter, then regenerated")
    endif()
    string(LENGTH "${RECOMP_XBE_SHA256}" _xbe_sha256_length)
    if(NOT _xbe_sha256_length EQUAL 64 OR NOT RECOMP_XBE_SHA256 MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "RECOMP_XBE_SHA256 must authenticate the generated program's executable")
    endif()
    string(TOLOWER "${RECOMP_XBE_SHA256}" _xbe_sha256)
    # Isolate the lifter's recomp_types.h while keeping all game-derived files private.
    get_filename_component(_program_parent "${_program_dir}" DIRECTORY)
    set(_derived_dir "${_program_parent}/runtime-sources")
    file(MAKE_DIRECTORY "${_derived_dir}")
    set(_missing_source "${_derived_dir}/recomp_program_missing.c")
    # Scanning the generated C takes most of a configure; reuse its stubs while
    # the authenticated program and these rules are unchanged.
    file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _rules_sha256)
    set(_scan_key "${_manifest_sha256} ${_rules_sha256}")
    set(_scan TRUE)
    if(EXISTS "${_missing_source}.stamp" AND EXISTS "${_missing_source}")
        file(READ "${_missing_source}.stamp" _old_scan_key)
        file(SHA256 "${_missing_source}" _missing_sha256)
        if(_old_scan_key STREQUAL "${_scan_key} ${_missing_sha256}")
            set(_scan FALSE)
        endif()
    endif()
    set(_program_sources)
    set(_definitions)
    set(_references)
    foreach(_name IN LISTS _program_names)
        set(_path "${_program_dir}/${_name}")
        configure_file("${_path}" "${_derived_dir}/${_name}" COPYONLY)
        if(_name MATCHES [=[\.c$]=])
            list(APPEND _program_sources "${_derived_dir}/${_name}")
        endif()
        if(_scan AND _name MATCHES [=[\.c$]=])
            file(READ "${_path}" _source_text)
            if(_source_text MATCHES "\n[ \t]+uint32_t ebp;[ \r]*\n")
                message(FATAL_ERROR "Authenticated program has an uninitialized EBP prologue")
            endif()
            string(REGEX MATCHALL "void[ \t\r\n]+sub_[0-9A-Fa-f]+[ \t\r\n]*\\([ \t\r\n]*void[ \t\r\n]*\\)[ \t\r\n]*\\{" _matches "${_source_text}")
            foreach(_match IN LISTS _matches)
                string(REGEX MATCH "sub_[0-9A-Fa-f]+" _symbol "${_match}")
                list(APPEND _definitions "${_symbol}")
            endforeach()
            string(REGEX MATCHALL "sub_[0-9A-Fa-f]+" _matches "${_source_text}")
            list(APPEND _references ${_matches})
        endif()
    endforeach()
    if(_scan)
        list(REMOVE_DUPLICATES _definitions)
        list(REMOVE_DUPLICATES _references)
        file(WRITE "${_missing_source}" "#include <stdint.h>\nvoid recomp_program_missing(uint32_t);\n")
        set(_missing_count 0)
        foreach(_symbol IN LISTS _references)
            list(FIND _definitions "${_symbol}" _defined_index)
            if(_defined_index EQUAL -1)
                string(SUBSTRING "${_symbol}" 4 -1 _address)
                file(APPEND "${_missing_source}" "void ${_symbol}(void) { recomp_program_missing(0x${_address}u); }\n")
                math(EXPR _missing_count "${_missing_count} + 1")
            endif()
        endforeach()
        file(SHA256 "${_missing_source}" _missing_sha256)
        file(WRITE "${_missing_source}.stamp" "${_scan_key} ${_missing_sha256}")
        message(STATUS "Generated ${_missing_count} fail-loud stubs for absent snapshot bodies")
    else()
        message(STATUS "Reused the fail-loud stubs for this authenticated program")
    endif()
    # The netplay host forwards its UDP port through the router with UPnP.
    set(UPNPC_BUILD_SHARED OFF CACHE INTERNAL "")
    set(UPNPC_BUILD_TESTS OFF CACHE INTERNAL "")
    set(UPNPC_BUILD_SAMPLE OFF CACHE INTERNAL "")
    set(UPNPC_NO_INSTALL ON CACHE INTERNAL "")
    FetchContent_Declare(miniupnpc
        URL https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz
        URL_HASH SHA256=8cf2c833b3e76fc4893ff29c2a376e3394962449e5970e373c0a91421724d222
        SOURCE_SUBDIR miniupnpc)
    FetchContent_MakeAvailable(miniupnpc)
    # The generated chunks are most of the build. As an object library with no
    # target dependencies they compile while the runtime libraries do; inside the
    # runner, the Makefile generator would start them only after every library.
    add_library(recomp_program_objects OBJECT "${_missing_source}" ${_program_sources})
    target_include_directories(recomp_program_objects PRIVATE "${_derived_dir}" "${CMAKE_CURRENT_SOURCE_DIR}")
    # Both Windows renderers link these once-compiled runner objects.
    add_library(recomp_runner_objects OBJECT runner.cpp program_adapters.c program_manual.c netplay_guest.c
        d3d8_adapter.c d3d8_state_adapter.c d3d8_resource_adapter.c d3d8_draw_adapter.c
        d3d_device_model.c d3d_light_model.c d3d_render_state_model.c d3d_card_model.c
        d3d_frame_model.c d3d_tile_model.c d3d_vertex_shader_model.c perf_overlay.c
        dsound_api_adapter.c dsound_device_model.c dsound_buffer_adapter.c
        dsound_buffer_model.c dsound_effects_adapter.c dsound_effects_image.c dsound_mix_model.c
        xbox_adpcm.c mwply_adapter.c sfd_movie.c sfd_format.c movie_color_model.c adx_decoder.c stage_select.c
        input_model.c input_adapter.c input_host_win32.c input_pulse_source.c
        synctest.c synctest_modules.c rollback.c replay.c netplay.c netplay_room.c netplay_names.c netplay_upnp.c netplay_menu.cpp
        netplay_menu_room.cpp netplay_menu_find.cpp cpu_costume.c combo_hud.c hit_voice.c unlock_profile.c pc_settings.c
        pc_settings_store.cpp runner_assets.c menu_quit.c quit_icon.c
        netplay_settings.cpp netplay_profile.cpp netplay_session.cpp game_state.c overlay.c
        free_camera.c free_camera_adapter.c scene_freeze.c scene_freeze_adapter.c quit_prompt.c
        host_diagnostics.cpp)
    # Netplay pairs runners of one release (VERSION) or commit and one program,
    # whatever the host; a runner's own bytes differ by platform and compiler.
    set(RECOMP_NETPLAY_VERSION "" CACHE STRING "Release or commit netplay pairs on; empty reads VERSION or Git")
    set(_netplay_version "${RECOMP_NETPLAY_VERSION}")
    if(NOT _netplay_version AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" _netplay_version LIMIT_COUNT 1)
    elseif(NOT _netplay_version)
        execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _netplay_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        # Configure again when HEAD moves: a checkout rewrites HEAD, a commit its branch's ref and HEAD's log.
        execute_process(COMMAND git symbolic-ref -q HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _git_branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT _git_branch)
            set(_git_branch HEAD)
        endif()
        execute_process(COMMAND git rev-parse --git-path HEAD --git-path logs/HEAD --git-path "${_git_branch}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _git_head_files OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REPLACE "\n" ";" _git_head_files "${_git_head_files}")
        foreach(_git_head_file IN LISTS _git_head_files)
            get_filename_component(_git_head_file "${_git_head_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            if(EXISTS "${_git_head_file}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_head_file}")
            endif()
        endforeach()
    endif()
    if(NOT _netplay_version)
        message(FATAL_ERROR "Netplay needs a VERSION file, a Git checkout or RECOMP_NETPLAY_VERSION")
    endif()
    string(SHA256 _netplay_build "${_netplay_version}\n${_manifest_sha256}\n${_xbe_sha256}\n")
    message(STATUS "Netplay build ${_netplay_build} (${_netplay_version})")
    # RECOMP_PORTABLE_MATH gives every host the same transcendentals and RCPSS/RSQRTSS.
    foreach(_target recomp_runner_objects recomp_program_objects)
        target_compile_definitions(${_target} PRIVATE RECOMP_XBE_SHA256="${_xbe_sha256}" RECOMP_PORTABLE_MATH)
        if(MSVC)
            # /MP compiles the large generated chunks side by side inside one project.
            target_compile_options(${_target} PRIVATE /we4013 /MP)
        else()
            target_compile_options(${_target} PRIVATE -Werror=implicit-function-declaration)
        endif()
    endforeach()
    # Only runner.cpp reads it, so a new commit does not recompile the generated program.
    set_source_files_properties(runner.cpp PROPERTIES COMPILE_DEFINITIONS RECOMP_NETPLAY_BUILD="${_netplay_build}")
    if(NOT MSVC)
        # The lifter writes conditions as if ((a == b)); clang would warn on every one.
        set_source_files_properties(${_program_sources} PROPERTIES COMPILE_OPTIONS -Wno-parentheses-equality)
    endif()
    set(_runner_libraries recomp_kernel recomp_xbe recomp_audio recomp_portable_math libminiupnpc-static)
    if(WIN32)
        list(APPEND _runner_libraries dbghelp user32 winmm ws2_32)
    endif()
    # The presenter only lends its headers here; each runner links its own.
    target_link_libraries(recomp_runner_objects PRIVATE ${_runner_libraries} recomp_d3d_presenter)
    if(TARGET SDL3::SDL3)
        target_link_libraries(recomp_runner_objects PRIVATE SDL3::SDL3)
    endif()
    target_include_directories(recomp_runner_objects PRIVATE "${_derived_dir}")
    add_executable(recomp_program_runner
        $<TARGET_OBJECTS:recomp_runner_objects> $<TARGET_OBJECTS:recomp_program_objects>)
    target_link_libraries(recomp_program_runner PRIVATE ${_runner_libraries} recomp_d3d_presenter)
    recomp_use_sdl3(recomp_program_runner)
    if(WIN32)
        # The Vulkan renderer: the same runner with the SDL_GPU presenter.
        add_executable(recomp_program_runner_vulkan
            $<TARGET_OBJECTS:recomp_runner_objects> $<TARGET_OBJECTS:recomp_program_objects>)
        target_link_libraries(recomp_program_runner_vulkan PRIVATE ${_runner_libraries} recomp_d3d_presenter_vulkan)
        recomp_use_sdl3(recomp_program_runner_vulkan)
        recomp_use_shader_compiler(recomp_program_runner_vulkan)
    endif()
    # Sofdec movies decode their MPEG-1 video with pl_mpeg (MIT; see NOTICE),
    # patched for TMPGEnc's 11-bit intra DC.
    file(DOWNLOAD https://raw.githubusercontent.com/phoboslab/pl_mpeg/c871f2be022ece7ef4f64230b4fb8e1fb9eb6023/pl_mpeg.h
        "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg_upstream.h"
        EXPECTED_HASH SHA256=3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9)
    include("${CMAKE_CURRENT_SOURCE_DIR}/pl_mpeg_patch.cmake")
    recomp_patch_pl_mpeg("${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg_upstream.h" "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg/pl_mpeg.h")
    set_source_files_properties(sfd_movie.c PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/pl_mpeg")

    # Settings launcher for launcher.sh. SteamOS has no compiler, so it is built
    # beside the runner rather than on first use like the macOS launcher.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(recomp_launcher ../tools/launcher_sdl.cpp)
        recomp_use_sdl3(recomp_launcher)
    endif()
endif()
