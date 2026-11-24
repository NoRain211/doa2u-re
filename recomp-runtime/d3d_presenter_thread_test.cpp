#include "d3d_presenter.h"
#include "d3d_presenter_backend.h"
#include "d3d_card_model.h"
#include "d3d_draw_model.h"
#ifndef RECOMP_TEST_D3D11
#include "d3d_framegen_capture.h"
#endif
#include "xapi_time_adapter.h"
#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"

/* Both Windows presenters, Direct3D 11 and Vulkan, run on their own window
   thread; SDL_GPU on macOS and Linux runs inline on the caller's. */
#if defined(_WIN32) && !defined(RECOMP_PRESENTER_SDL_GPU)
#define RECOMP_TEST_D3D11
#else
#include <SDL3/SDL.h>
#endif
#ifdef _WIN32
#define RECOMP_TEST_WORKER
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "win32_compat.h"
#include <stdlib.h>

/* The SDL_GPU backend: _putenv_s with an empty value removes the variable. */
static int _putenv_s(const char *name, const char *value)
{
    return value[0] != '\0' ? setenv(name, value, 1) : unsetenv(name);
}

static ULONGLONG GetTickCount64()
{
    return SDL_GetTicks();
}
#endif

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>
#include <string>
#include <vector>

// d3d_vblank.cpp's guest pacing reaches the kernel; these tests use only its host sleep.
extern "C" void recomp_kernel_wait_for_vblank(uint64_t) {}
extern "C" uint32_t recomp_kernel_current_thread_id(void) { return 1u; }

static const RecompD3dPresenterConfig config = {
    320u, 240u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};

static bool expect(const char *label, RecompD3dPresenterError actual,
    RecompD3dPresenterError expected = RECOMP_D3D_PRESENTER_OK)
{
    if (actual == expected) return true;
    std::fprintf(stderr, "FAIL %s actual=%u expected=%u\n", label,
        static_cast<unsigned>(actual), static_cast<unsigned>(expected));
    return false;
}

static RecompD3dPresenterCommand clearCommand()
{
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR;
    command.data.clear = {true, true, false, 0xff204060u, 1.0f, 0u};
    return command;
}

static RecompD3dPresenterError tick(RecompD3dPresenter *presenter, uint32_t swap)
{
    auto command = clearCommand();
    const auto error = recomp_d3d_presenter_submit(presenter, &command);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    command = {};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present = {5u, swap};
    return recomp_d3d_presenter_submit(presenter, &command);
}

static bool destroy(RecompD3dPresenter *&presenter)
{
    const bool passed = expect("destroy", recomp_d3d_presenter_destroy(&presenter));
    if (presenter != nullptr) std::fprintf(stderr, "FAIL destroy retained handle\n");
    return passed && presenter == nullptr;
}

#ifdef RECOMP_TEST_D3D11
static HWND presenterWindow()
{
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window,
            L"DOA2URecompPresenterWindow", nullptr)) != nullptr) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId()) return window;
    }
    return nullptr;
}

// F11 and Alt+Enter switch the window between a captioned window and a borderless one over its monitor.
// A scaled presenter starts fullscreen and leaves it for a window centered in the monitor's work area.
static bool testFullscreenKeys(bool start_fullscreen = false)
{
    struct ScaleEnv { // cleared on every return, so a failure here leaves later tests unscaled
        explicit ScaleEnv(const char *value) { _putenv_s("RECOMP_D3D_SCALE", value); }
        ~ScaleEnv() { _putenv_s("RECOMP_D3D_SCALE", ""); }
    } scale_env(start_fullscreen ? "2" : "1");
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create fullscreen", recomp_d3d_presenter_create(&config, &presenter))) return false;
    uint32_t swap = 1;
    bool passed = expect("show window", tick(presenter, swap++));
    const HWND window = presenterWindow();
    if (window == nullptr) passed = false;
    const auto fullscreen = [&] {
        RECT rect{}, screen{};
        GetWindowRect(window, &rect);
        MONITORINFO monitor{sizeof monitor};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        screen = monitor.rcMonitor;
        return (GetWindowLongW(window, GWL_STYLE) & WS_CAPTION) == 0 && EqualRect(&rect, &screen);
    };
    // The presenter thread dispatches posted keys between packets, so keep ticking until it has.
    const auto press = [&](UINT message, WPARAM key, bool expected) {
        PostMessageW(window, message, key, message == WM_SYSKEYDOWN ? (1 << 29) : 0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fullscreen() != expected && std::chrono::steady_clock::now() < until)
            passed &= expect("tick", tick(presenter, swap++));
        if (fullscreen() != expected) std::fprintf(stderr, "FAIL fullscreen key %u\n", message);
        passed &= fullscreen() == expected;
    };
    if (passed) {
        passed &= fullscreen() == start_fullscreen;
        press(WM_KEYDOWN, VK_F11, !start_fullscreen);
        if (start_fullscreen) {
            RECT rect{};
            GetWindowRect(window, &rect);
            MONITORINFO monitor{sizeof monitor};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            const LONG dx = (rect.left + rect.right) - (monitor.rcWork.left + monitor.rcWork.right);
            const LONG dy = (rect.top + rect.bottom) - (monitor.rcWork.top + monitor.rcWork.bottom);
            if (std::abs(dx) > 1 || std::abs(dy) > 1) std::fprintf(stderr, "FAIL not centered dx=%ld dy=%ld\n", dx, dy);
            passed &= std::abs(dx) <= 1 && std::abs(dy) <= 1;
        }
        press(WM_SYSKEYDOWN, VK_RETURN, start_fullscreen);
        press(WM_SYSKEYDOWN, VK_RETURN, !start_fullscreen);
        press(WM_KEYDOWN, VK_F11, start_fullscreen);
    }
    passed &= destroy(presenter);
    if (passed) std::puts(start_fullscreen ? "PASS F11 leaves fullscreen centered" : "PASS F11 and Alt+Enter toggle fullscreen");
    return passed;
}

// Swap's guest wait ends only once the worker has run the last frame: the error
// that frame's bad CLEAR raises is already visible when it does.
static bool testOneFrameInFlight()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create in flight", recomp_d3d_presenter_create(&config, &presenter))) return false;
    bool passed = expect("in-flight tick", tick(presenter, 1));
    auto clear = clearCommand();
    clear.data.clear.z = 2.0f;
    passed &= expect("bad clear", recomp_d3d_presenter_submit(presenter, &clear));
    RecompD3dPresenterCommand present{};
    present.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    present.data.present = {5u, 2u};
    passed &= expect("bad clear present", recomp_d3d_presenter_submit(presenter, &present));
    const auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!recomp_d3d_presenter_present_ready(presenter) && std::chrono::steady_clock::now() < ready_by) {
        std::this_thread::yield();
    }
    clear = clearCommand();
    passed &= expect("frame ran before ready", recomp_d3d_presenter_submit(presenter, &clear),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    passed &= expect("destroy in flight", recomp_d3d_presenter_destroy(&presenter),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    if (passed) std::puts("PASS present ready waits for the last frame");
    return passed;
}
#endif

static bool testTicks()
{
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("null config", recomp_d3d_presenter_create(nullptr, &presenter),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    if (!expect("create ticks", recomp_d3d_presenter_create(&config, &presenter))) return false;
    RecompD3dPresenter *second = nullptr;
    passed &= expect("singleton", recomp_d3d_presenter_create(&config, &second),
        RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED);
    std::thread other([&] {
        passed &= expect("owner thread", tick(presenter, 1), RECOMP_D3D_PRESENTER_WRONG_THREAD);
        passed &= expect("flush owner", recomp_d3d_presenter_flush(presenter, true),
            RECOMP_D3D_PRESENTER_WRONG_THREAD);
    });
    other.join();
    for (uint32_t i = 1; i <= 5 && passed; ++i) passed &= expect("tick", tick(presenter, i));
    auto clear = clearCommand();
    passed &= expect("partial clear", recomp_d3d_presenter_submit(presenter, &clear));
    passed &= expect("partial release", recomp_d3d_presenter_release_memory(
        presenter, 0x00200000u, 4096u));
    passed &= expect("partial texture invalidation", recomp_d3d_presenter_invalidate_textures(
        presenter, 0x00200000u, 4096u));
    passed &= expect("publish without present", recomp_d3d_presenter_flush(presenter, false));
    passed &= expect("wait idle", recomp_d3d_presenter_flush(presenter, true));
    passed &= expect("empty wait idle", recomp_d3d_presenter_flush(presenter, true));
    // Once the host catches up, Swap's guest wait for the presenter ends.
    const auto ready_by = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!recomp_d3d_presenter_present_ready(presenter) && std::chrono::steady_clock::now() < ready_by) {
        std::this_thread::yield();
    }
    if (!recomp_d3d_presenter_present_ready(presenter)) {
        std::fprintf(stderr, "FAIL present ready after idle\n");
        passed = false;
    }
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS five CLEAR/PRESENT ticks and partial shutdown");
    return passed;
}

/* Under the frame clock the guest advances one frame per present, so the
   presenter holds real presents to 60 a second: a display accepting them
   faster would run the guest fast. */
static bool testFrameClockPacing()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create pacing", recomp_d3d_presenter_create(&config, &presenter))) return false;
    uint32_t swap = 1u;
    bool passed = expect("pacing first frame", tick(presenter, swap++));
    recomp_xapi_frame_clock_set_enabled(true);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0u; i < 6u && passed; ++i) passed &= expect("paced tick", tick(presenter, swap++));
    passed &= expect("paced flush", recomp_d3d_presenter_flush(presenter, true));
    const auto paced = std::chrono::steady_clock::now() - start;
    recomp_xapi_frame_clock_set_enabled(false);
    if (paced < std::chrono::milliseconds(70)) {
        std::fprintf(stderr, "FAIL six frame-clock presents took %lld us\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(paced).count()));
        passed = false;
    }
    passed &= destroy(presenter);
    if (passed) std::puts("PASS the frame clock holds presents to 60 Hz");
    return passed;
}

static bool testCapturedDraw()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create draw", recomp_d3d_presenter_create(&config, &presenter))) return false;
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
    Vertex vertices[] = {
        {20, 20, 0.25f, 1, 0xffff8040u, 0, 0},
        {200, 20, 0.25f, 1, 0xffff8040u, 1, 0},
        {20, 200, 0.25f, 1, 0xffff8040u, 0, 1},
        {200, 200, 0.25f, 1, 0xffff8040u, 1, 1},
    };
    uint16_t indices[] = {0, 1, 2, 3};
    auto clear = clearCommand();
    bool passed = expect("draw clear", recomp_d3d_presenter_submit(presenter, &clear));
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = command.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    passed &= expect("capture draw", recomp_d3d_presenter_submit(presenter, &command));
    // No PRESENT has published this packet, so these writes precede execution.
    std::memset(vertices, 0xff, sizeof vertices);
    std::memset(indices, 0xff, sizeof indices);
    command = {};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present = {5, 1};
    passed &= expect("draw present", recomp_d3d_presenter_submit(presenter, &command));
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS captured draw survives source overwrite");
    return passed;
}

/* A runner started without tools/run_game.py names a shader cache folder that
   nobody made. It runs first: Direct3D 11 keeps compiled shaders for the process. */
static bool testShaderCacheFolder()
{
    char temp[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, temp)) return false;
    const std::filesystem::path root =
        std::filesystem::path(temp) / ("recomp-shader-cache-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()));
    const std::filesystem::path folder = root / "cache";
    _putenv_s("RECOMP_SHADER_CACHE", folder.string().c_str());
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create shader cache", d3d_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        const Vertex vertices[] = {{20, 20, 0.25f, 1, 0xffff8040u, 0, 0}, {200, 20, 0.25f, 1, 0xffff8040u, 1, 0},
            {20, 200, 0.25f, 1, 0xffff8040u, 0, 1}, {200, 200, 0.25f, 1, 0xffff8040u, 1, 1}};
        const uint16_t indices[] = {0, 1, 2, 3};
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x144u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        passed &= expect("shader cache draw", d3d_backend_submit(presenter, &command));
        command = {};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        command.data.present = {5, 1};
        passed &= expect("shader cache present", d3d_backend_submit(presenter, &command));
        passed &= expect("destroy shader cache", d3d_backend_destroy(&presenter));
    }
    _putenv_s("RECOMP_SHADER_CACHE", "");
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error) || std::filesystem::is_empty(folder, error)) {
        std::fprintf(stderr, "FAIL shader cache folder is missing or empty\n");
        passed = false;
    }
    std::filesystem::remove_all(root, error);
    if (passed) std::puts("PASS the shader cache makes its folder");
    return passed;
}

/* The build's precompile pass fills the cache with every boot shader a launch
   may ask for, so the boot precompile that launch runs compiles nothing new.
   A scaled 4x MSAA run picks the depth-bias variants, and one synthetic guest
   vertex program covers the program walk. */
static bool testPrecompileBootShaders()
{
    char temp[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, temp)) return false;
    const std::filesystem::path root =
        std::filesystem::path(temp) / ("recomp-precompile-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()));
    const std::filesystem::path folder = root / "cache";
    _putenv_s("RECOMP_SHADER_CACHE", folder.string().c_str());
    _putenv_s("RECOMP_D3D_SCALE", "2");
    _putenv_s("RECOMP_D3D_MSAA", "4");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "0");
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    // One instruction writing o9, encoded as testBootProgramShaders does.
    RecompD3dBootProgram programs[1] = {};
    programs[0].fvf = 0x012u;
    programs[0].count = 1u;
    programs[0].tokens[0][1] = (11u << 21) | (2u << 9) | (2u << 13) | 0x1bu;
    programs[0].tokens[0][2] = (2u << 26) | (3u << 11) | (0x1bu << 17);
    programs[0].tokens[0][3] = (15u << 12) | (9u << 3) | 2048u | 1u;
    RecompD3dPresenterConfig boot_config = config;
    boot_config.programs = programs;
    boot_config.program_count = 1u;
    const auto entries = [&folder] {
        std::map<std::string, std::filesystem::file_time_type> found;
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(folder, error))
            found.emplace(entry.path().filename().string(), entry.last_write_time());
        return found;
    };
    const unsigned compiled = d3d_backend_precompile_boot_shaders(&boot_config);
    if (compiled == 0u) std::fprintf(stderr, "FAIL the precompile compiled no boot shaders\n");
    bool passed = compiled != 0u;
    const auto before = entries();
    if (before.empty()) {
        std::fprintf(stderr, "FAIL the precompile wrote no cache entries\n");
        passed = false;
    }
    RecompD3dPresenter *presenter = nullptr;
    passed &= expect("create precompiled", d3d_backend_create(&boot_config, &presenter));
    if (presenter != nullptr) {
        // Destroy would stop the boot precompile; it must finish to prove it finds every entry.
        d3d_backend_join_precompile(presenter);
        passed &= expect("destroy precompiled", d3d_backend_destroy(&presenter));
    }
    const auto after = entries();
    if (after != before) {
        std::fprintf(stderr, "FAIL the boot precompile wrote %d new cache entries\n",
            static_cast<int>(after.size()) - static_cast<int>(before.size()));
        passed = false;
    }
    _putenv_s("RECOMP_SHADER_CACHE", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    _putenv_s("RECOMP_D3D_MSAA", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (passed) std::puts("PASS the precompile fills the cache the boot precompile reads");
    return passed;
}

static bool testMipSampler()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "mip", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create mip sampler", d3d_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        Vertex vertices[] = {
            {20, 20, 0.25f, 1, 0xffffffffu, 0, 0},
            {84, 20, 0.25f, 1, 0xffffffffu, 16, 0},
            {20, 84, 0.25f, 1, 0xffffffffu, 0, 16},
            {84, 84, 0.25f, 1, 0xffffffffu, 16, 16},
        };
        uint16_t indices[] = {0, 1, 2, 3};
        // One solid BC1 block per mip: red 4x4, green 2x2, blue 1x1.
        const uint32_t mips[] = {0xf800f800u, 0u, 0x07e007e0u, 0u, 0x001f001fu, 0u};
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("mip clear", d3d_backend_submit(presenter, &clear));
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x144u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_DXT1;
        draw.texture.bits_per_pixel = 4;
        draw.texture.width = draw.texture.height = 4;
        draw.texture.mip_levels = 3;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = mips;
        draw.texture_byte_count = sizeof mips;
        // Texture alpha must survive a zero material alpha and pass the test.
        draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_TEXTURE;
        draw.depth.alpha_test_enable = true;
        draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
        draw.depth.alpha_ref = 128;
        // Exceed the cache once, then revisit the four samplers below.
        for (unsigned i = 0; i < 65 && passed; ++i) {
            draw.mip_lod_bias = -8.0f + i * 0.25f;
            passed &= expect("mip cache eviction", d3d_backend_submit(presenter, &command));
        }
        const float biases[] = {0.0f, 1.25f, 1.25f, -2.0f};
        for (unsigned i = 0; i < 4 && passed; ++i) {
            draw.point_mip_filter = i == 1 || i == 3;
            draw.mip_lod_bias = biases[i];
            passed &= expect("mip draw", d3d_backend_submit(presenter, &command));
            for (auto &vertex : vertices) vertex.x += 64.0f;
        }
        for (auto &vertex : vertices) {
            vertex.x -= 256.0f;
            vertex.y += 64.0f;
            vertex.diffuse = 0x00ffffffu;
        }
        draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
        draw.material_alpha = 1.0f;
        draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER_EQUAL;
        draw.mip_lod_bias = 0.0f;
        passed &= expect("vertex alpha discard", d3d_backend_submit(presenter, &command));
        // Lit sprites use material alpha once, with white expanded vertex alpha.
        for (auto &vertex : vertices) {
            vertex.x += 64.0f;
            vertex.u = vertex.v = 0.5f;
            vertex.diffuse = 0xffffffffu;
        }
        const uint32_t sprite_texel = 0x80ff0000u;
        draw.texture.format_byte = 0x12u;
        draw.texture.linear = true;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = draw.texture.height = draw.texture.mip_levels = 1;
        draw.texture.pitch = 4;
        draw.texture.data = 0x2000u;
        draw.texture_bytes = &sprite_texel;
        draw.texture_byte_count = sizeof sprite_texel;
        draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE;
        draw.material_alpha = 0.25f;
        draw.depth.alpha_test_enable = false;
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        passed &= expect("lit sprite alpha", d3d_backend_submit(presenter, &command));
        for (auto &vertex : vertices) { vertex.x += 64.0f; vertex.diffuse = 0x00123456u; }
        draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_NONE;
        draw.texture_factor = 0x40804020u;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[2] = 1;
        draw.directional.stage[1] = draw.directional.stage[3] = 2;
        passed &= expect("sprite texture factor", d3d_backend_submit(presenter, &command));
        for (float bias : {-16.01f, 16.0f, INFINITY, NAN}) {
            draw.mip_lod_bias = bias;
            passed &= expect("invalid mip bias", d3d_backend_submit(presenter, &command),
                RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        }
        command = {};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        command.data.present = {5, 1};
        passed &= expect("mip present", d3d_backend_submit(presenter, &command));
        passed &= expect("destroy mip sampler", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            for (unsigned i = 0; i < 7; ++i) {
                unsigned char bgr[3]{};
                const unsigned x = i >= 4 ? 52 + 64 * (i - 4) : 52 + 64 * i;
                const unsigned y = i >= 4 ? 116 : 52;
                std::fseek(file, 54 + (240 - 1 - y) * 320 * 3 + x * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool red = bgr[2] > 245 && bgr[1] < 10 && bgr[0] < 10;
                const bool green = bgr[1] > 245 && bgr[0] < 10 && bgr[2] < 10;
                const bool blended = bgr[1] > 170 && bgr[1] < 210 &&
                    bgr[0] > 45 && bgr[0] < 85 && bgr[2] < 10;
                const bool black = bgr[0] == 0 && bgr[1] == 0 && bgr[2] == 0;
                const bool sprite = std::abs(static_cast<int>(bgr[2]) - 32) <= 1 && bgr[0] == 0 && bgr[1] == 0;
                const bool factor_sprite = std::abs(static_cast<int>(bgr[2]) - 16) <= 1 && bgr[0] == 0 && bgr[1] == 0;
                const bool pixel_ok = i == 6 ? factor_sprite : i == 5 ? sprite : i == 4 ? black : i == 1 ? green : i == 2 ? blended : red;
                if (!pixel_ok) std::fprintf(stderr, "FAIL mip pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS mip selection, interpolation, bias, sampler cache eviction, texture alpha and vertex alpha");
    return passed;
}

/* The guest stores one swizzled surface per uncompressed mip level. A minified
   sample must read the guest's own smaller level, not a host-generated one. */
static bool testUncompressedMipChain()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "chain", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create mip chain", d3d_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        Vertex vertices[4] = {};
        uint16_t indices[] = {0, 1, 2, 3};
        // Three guest levels, a distinct solid colour each, swizzled per level
        // and laid end to end: 16x16 red, 8x8 green, 4x4 blue.
        std::vector<uint32_t> storage(16u * 16u + 8u * 8u + 4u * 4u);
        const uint32_t colors[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu};
        const uint32_t dims[] = {16u, 8u, 4u};
        size_t at = 0u;
        for (unsigned i = 0u; i < 3u; ++i) {
            uint32_t mask_x = 0u, mask_y = 0u;
            recomp_d3d_texture_swizzle_masks(dims[i], dims[i], &mask_x, &mask_y);
            for (uint32_t y = 0u; y < dims[i]; ++y) {
                for (uint32_t x = 0u; x < dims[i]; ++x) {
                    storage[at + recomp_d3d_texture_swizzle_offset(x, y, mask_x, mask_y)] = colors[i];
                }
            }
            at += static_cast<size_t>(dims[i]) * dims[i];
        }
        const auto quad = [&](float left, float top, float size, uint32_t byte_count, float bias) {
            const float corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
            for (unsigned i = 0u; i < 4u; ++i) {
                vertices[i] = {left + corner[i][0] * size, top + corner[i][1] * size,
                    0.25f, 1.0f, 0xffffffffu, corner[i][0], corner[i][1]};
            }
            RecompD3dPresenterCommand command{};
            command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4u;
            draw.triangle_count = 2u;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x144u;
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15u;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32u;
            draw.texture.width = draw.texture.height = 16u;
            draw.texture.mip_levels = 3u;
            draw.texture.data = 0x3000u;
            draw.texture_bytes = storage.data();
            draw.texture_byte_count = byte_count;
            draw.point_mip_filter = true;
            draw.mip_lod_bias = bias;
            return expect("mip chain draw", d3d_backend_submit(presenter, &command));
        };
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("mip chain clear", d3d_backend_submit(presenter, &clear));
        const uint32_t chain = static_cast<uint32_t>(storage.size() * 4u);
        // One texel per pixel red, 2.67 per pixel green and 4 per pixel blue.
        passed &= quad(20.0f, 20.0f, 16.0f, chain, 0.0f);
        passed &= quad(52.0f, 20.0f, 6.0f, chain, 0.0f);
        passed &= quad(76.0f, 20.0f, 4.0f, chain, 0.25f);
        // A span that holds only level 0 keeps the old single-level sample.
        passed &= quad(100.0f, 20.0f, 6.0f, 1024u, 0.0f);
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        command.data.present = {5u, 1u};
        passed &= expect("mip chain present", d3d_backend_submit(presenter, &command));
        passed &= expect("destroy mip chain", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            struct Sample { unsigned x, y, channel; const char *name; };
            const Sample samples[] = {
                {28u, 28u, 2u, "level 0"}, {55u, 23u, 1u, "level 1"},
                {78u, 22u, 0u, "level 2"}, {103u, 23u, 2u, "level 0 span"},
            };
            for (const Sample &sample : samples) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (240 - 1 - sample.y) * 320 * 3 + sample.x * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[sample.channel] > 245 && bgr[(sample.channel + 1u) % 3u] < 10 &&
                    bgr[(sample.channel + 2u) % 3u] < 10;
                if (!pixel_ok) std::fprintf(stderr, "FAIL mip chain %s BGR=%u,%u,%u\n",
                    sample.name, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS uncompressed guest mip levels at minifying scales");
    return passed;
}

/* A stage change uploads more texels in one frame than the SDL_GPU backend's
   staging buffer holds, so the buffer starts again before the frame's uploads
   run. Each texture must still get its own texels. */
static bool testTextureBurst()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "burst", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create texture burst", d3d_backend_create(&config, &presenter));
    // Five 4 MiB textures, a solid colour each (BGR below), drawn 16 pixels square in a row.
    const uint32_t colors[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u, 0xffff00ffu};
    const unsigned char bgr[][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 255, 255}, {255, 0, 255}};
    if (passed) {
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        uint16_t indices[] = {0, 1, 2, 3};
        std::vector<uint32_t> texels(1024u * 1024u);
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("texture burst clear", d3d_backend_submit(presenter, &clear));
        for (unsigned i = 0u; i < 5u; ++i) {
            // The backend reads the texels during the submit, so one buffer serves every texture.
            std::fill(texels.begin(), texels.end(), colors[i]);
            const float left = 20.0f + 32.0f * i;
            const Vertex vertices[4] = {{left, 20.0f, 0.25f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
                {left + 16.0f, 20.0f, 0.25f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
                {left, 36.0f, 0.25f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
                {left + 16.0f, 36.0f, 0.25f, 1.0f, 0xffffffffu, 1.0f, 1.0f}};
            RecompD3dPresenterCommand command{};
            command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4u;
            draw.triangle_count = 2u;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x144u;
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15u;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32u;
            draw.texture.width = draw.texture.height = 1024u;
            draw.texture.mip_levels = 1u;
            draw.texture.data = 0x400000u * (i + 1u);
            draw.texture_bytes = texels.data();
            draw.texture_byte_count = static_cast<uint32_t>(texels.size() * 4u);
            passed &= expect("texture burst draw", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        command.data.present = {5u, 1u};
        passed &= expect("texture burst present", d3d_backend_submit(presenter, &command));
        passed &= expect("destroy texture burst", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            for (unsigned i = 0u; i < 5u; ++i) {
                unsigned char pixel[3]{};
                std::fseek(file, 54 + (240 - 1 - 28) * 320 * 3 + (28 + 32 * static_cast<int>(i)) * 3, SEEK_SET);
                passed &= std::fread(pixel, 1, 3, file) == 3;
                if (std::memcmp(pixel, bgr[i], 3) != 0) {
                    std::fprintf(stderr, "FAIL texture burst %u BGR=%u,%u,%u\n", i, pixel[0], pixel[1], pixel[2]);
                    passed = false;
                }
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS a frame's texture uploads beyond the staging buffer keep their texels");
    return passed;
}

static bool testPointMinMagFilter()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "point", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create point sampler", d3d_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        // A 2x2 texture, red on the left and green on the right, magnified 32x.
        const uint32_t texels[] = {
            0xffff0000u, 0xff00ff00u,
            0xffff0000u, 0xff00ff00u,
        };
        Vertex vertices[] = {
            {20, 20, 0.25f, 1, 0xffffffffu, 0, 0},
            {84, 20, 0.25f, 1, 0xffffffffu, 2, 0},
            {20, 84, 0.25f, 1, 0xffffffffu, 0, 2},
            {84, 84, 0.25f, 1, 0xffffffffu, 2, 2},
        };
        uint16_t indices[] = {0, 1, 2, 3};
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("point clear", d3d_backend_submit(presenter, &clear));
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x144u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_texture = true;
        draw.texture.format_byte = 0x12u;
        draw.texture.linear = true;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = draw.texture.height = 2;
        draw.texture.pitch = 8;
        draw.texture.data = 0x3000u;
        draw.texture_bytes = texels;
        draw.texture_byte_count = sizeof texels;
        // Point mag takes the nearest texel; the same quad drawn linearly blends.
        draw.point_mag_filter = true;
        passed &= expect("point mag draw", d3d_backend_submit(presenter, &command));
        for (auto &vertex : vertices) vertex.x += 64.0f;
        draw.point_mag_filter = false;
        passed &= expect("linear mag draw", d3d_backend_submit(presenter, &command));
        command = {};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        command.data.present = {5, 1};
        passed &= expect("point present", d3d_backend_submit(presenter, &command));
        passed &= expect("destroy point sampler", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // x=51 and x=53 straddle the texel boundary at x=52; y=35 is mid-texel.
            const unsigned samples[][2] = {{51u, 35u}, {53u, 35u}, {115u, 35u}};
            unsigned char bgr[3][3]{};
            for (unsigned i = 0u; i < 3u; ++i) {
                std::fseek(file, 54 + (240 - 1 - samples[i][1]) * 320 * 3 + samples[i][0] * 3, SEEK_SET);
                passed &= std::fread(bgr[i], 1, 3, file) == 3;
            }
            std::fclose(file);
            const bool point_red = bgr[0][2] > 245 && bgr[0][1] < 10 && bgr[0][0] < 10;
            const bool point_green = bgr[1][1] > 245 && bgr[1][0] < 10 && bgr[1][2] < 10;
            const bool linear_blend = bgr[2][2] > 60 && bgr[2][2] < 200 &&
                bgr[2][1] > 60 && bgr[2][1] < 200 && bgr[2][0] < 10;
            if (!point_red) std::fprintf(stderr, "FAIL point mag left BGR=%u,%u,%u\n",
                bgr[0][0], bgr[0][1], bgr[0][2]);
            if (!point_green) std::fprintf(stderr, "FAIL point mag right BGR=%u,%u,%u\n",
                bgr[1][0], bgr[1][1], bgr[1][2]);
            if (!linear_blend) std::fprintf(stderr, "FAIL linear mag BGR=%u,%u,%u\n",
                bgr[2][0], bgr[2][1], bgr[2][2]);
            passed &= point_red && point_green && linear_blend;
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS point mag samples the nearest texel and linear mag still blends");
    return passed;
}

static bool testLines()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "lin", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create lines", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("lines clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; };
        Vertex vertices[2] = {{20,20.25f,0.5f,~0u}, {100,20.25f,0.5f,~0u}};
        const uint16_t indices[2] = {0,1};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = 2;
        draw.index_count = draw.vertex_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x42;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.has_transform = true;
        draw.transform[0] = 2.0f / 320;
        draw.transform[5] = -2.0f / 240;
        draw.transform[10] = draw.transform[15] = 1;
        draw.transform[12] = -1;
        draw.transform[13] = 1;
        draw.blend.color_write_mask = 15;
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        draw.line_antialias = true;
        draw.line_width = 1.25f;
        draw.cull_mode = RECOMP_D3D_CULL_CLOCKWISE;
        passed &= expect("fractional line", d3d_backend_submit(presenter, &command));
        for (auto &v : vertices) v.y = 40.5f;
        std::swap(vertices[0], vertices[1]);
        draw.line_width = 0.25f;
        passed &= expect("subpixel line", d3d_backend_submit(presenter, &command));
        std::swap(vertices[0], vertices[1]);
        // Near-plane clipping keeps the second half; the fully hidden line emits nothing.
        for (auto &v : vertices) v.y = 60.5f;
        vertices[0].z = -0.5f;
        draw.line_width = 1;
        passed &= expect("near crossing line", d3d_backend_submit(presenter, &command));
        for (auto &v : vertices) { v.y = 80.5f; v.z = -1; }
        passed &= expect("hidden line", d3d_backend_submit(presenter, &command));
        // A later aliased line must use the ordinary pipeline again.
        for (auto &v : vertices) { v.y = 100.5f; v.z = 0.5f; }
        draw.line_antialias = false;
        passed &= expect("ordinary line", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5,1};
        passed &= expect("lines present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy lines", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const unsigned pixels[][3] = {{50,20,223}, {50,19,96}, {50,18,0}, {50,40,64},
                {40,60,0}, {80,60,255}, {50,80,0}, {50,100,255}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239-pixel[1])*960 + pixel[0]*3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                for (unsigned c=0; c<3; ++c) {
                    const bool ok = std::abs(int(bgr[c])-int(pixel[2])) <= 2;
                    if (!ok) std::fprintf(stderr, "FAIL line pixel %u,%u channel=%u actual=%u expected=%u\n",
                        pixel[0], pixel[1], c, bgr[c], pixel[2]);
                    passed &= ok;
                }
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS transformed line widths, fractional alpha coverage, clipping and pipeline restore");
    return passed;
}

static bool testLighting()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "lit", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create lighting", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("lighting clear", d3d_backend_submit(presenter, &clear));
        for (unsigned i = 0; i < 24 && passed; ++i) {
            struct Vertex { float x, y, z, nx, ny, nz, u, v; };
            Vertex vertices[] = {
                {-0.1f, 0.1f, 0.5f, 0, 0, 1, 0, 0}, {0.1f, 0.1f, 0.5f, 0, 0, 1, 1, 0},
                {-0.1f, -0.1f, 0.5f, 0, 0, 1, 0, 1}, {0.1f, -0.1f, 0.5f, 0, 0, 1, 1, 1}};
            const uint16_t indices[] = {0, 1, 2, 3};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x112u;
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.has_transform = true;
            draw.transform[0] = draw.transform[5] = 1.5f;
            draw.transform[10] = draw.transform[15] = 1;
            draw.transform[12] = -0.75f + 0.5f * (i % 4);
            draw.transform[13] = i < 4 ? 0.5f : -0.5f;
            draw.depth.alpha_test_enable = true;
            draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
            draw.depth.alpha_ref = 128;
            auto &light = draw.directional;
            light.enabled = i != 5;
            light.stage_mode = light.normalize = true;
            light.count = 1;
            for (unsigned j = 0; j < 16; j += 5)
                light.normal_transforms[0][j] = light.world_transforms[0][j] = 1;
            const float base[] = {0.1f, 0.05f, 0.025f, 0};
            const float material[] = {0.5f, 0.25f, 0.75f, 0.75f};
            const float color[] = {0.6f, 0.8f, 0.4f, 0};
            const float ambient[] = {0.025f, 0.05f, 0.075f, 0};
            std::memcpy(light.ambient_emissive, base, sizeof base);
            std::memcpy(light.material_diffuse, material, sizeof material);
            std::memcpy(light.colors[0], color, sizeof color);
            std::memcpy(light.ambient[0], ambient, sizeof ambient);
            light.directions[0][2] = 1;
            if (i == 1) for (auto &vertex : vertices) vertex.nz = -1;
            if (i == 2 || i == 3) {
                light.world_transforms[0][14] = 1;
                light.positions[0][2] = 3.5f;
                light.positions[0][3] = 1;
                light.attenuation[0][0] = 1;
                light.attenuation[0][1] = 0.5f;
                light.attenuation[0][3] = i == 2 ? 3.0f : 1.0f;
            }
            if (i == 4 || i == 8 || i == 11) {
                if (i == 4) {
                    std::memset(light.colors, 0, sizeof light.colors);
                    std::memset(light.ambient, 0, sizeof light.ambient);
                }
                for (unsigned j = 0; j < 3; ++j)
                    light.specular[0][j] = i == 4 ? 0.2f + j * 0.1f : 0.05f + j * 0.05f;
                light.view[2] = 1;
                light.view[3] = 8;
            }
            if (i == 6) light.material_diffuse[3] = 0.25f;
            if (i == 7) {
                for (auto &vertex : vertices) { vertex.nx = 2; vertex.nz = 0; }
                light.normal_transforms[0][0] = light.normal_transforms[0][10] = 0;
                light.normal_transforms[0][2] = 1;
            }
            const bool vertex_diffuse = (i >= 9 && i < 14 && i != 11) || (i >= 16 && i < 18);
            const uint32_t base_pixel = i >= 20 ? ((i & 1u) ? 0xffc08040u : 0x00c08040u)
                : i >= 18 ? 0x000000ffu : i >= 14 ? 0x80c08040u : vertex_diffuse ? 0xc080e0ffu : 0x800000ffu;
            const uint32_t environment_pixel = i == 12 ? 0u : i == 13 ? 0xff804020u
                : vertex_diffuse ? 0x80ff8000u : 0x40ff0000u;
            if (i >= 8 && i < 14) {
                draw.transform[0] = 0.75f;
                draw.transform[12] = -5.0f / 6.0f + (i - 8) / 3.0f;
                draw.transform[13] = 0;
                draw.depth.alpha_test_enable = false;
                draw.blend.blend_enable = true;
                draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
                draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
                draw.has_texture = draw.has_reflection = draw.reflection_mesh_uv = true;
                draw.reflection_premultiplied = i >= 10;
                draw.texture.data = 0x1000u;
                draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
                draw.texture.bits_per_pixel = 32;
                draw.texture.width = draw.texture.height = 1;
                draw.texture_bytes = &base_pixel;
                draw.texture_byte_count = sizeof base_pixel;
                draw.reflection_texture = draw.texture;
                draw.reflection_texture.data = 0x2000u;
                draw.reflection_bytes = &environment_pixel;
                draw.reflection_byte_count = sizeof environment_pixel;
                std::memcpy(draw.reflection_diffuse, material, sizeof material);
            }
            if (i >= 14 && i < 18) {
                draw.transform[12] = (i & 1) ? 0.5f : -0.5f;
                draw.transform[13] = i >= 16 ? -0.85f : 0.85f;
                draw.depth.alpha_test_enable = false;
                draw.blend.blend_enable = true;
                draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
                draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
                draw.has_texture = true;
                draw.texture.data = 0x3000u;
                draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
                draw.texture.bits_per_pixel = 32;
                draw.texture.width = draw.texture.height = 1;
                draw.texture_bytes = &base_pixel;
                draw.texture_byte_count = sizeof base_pixel;
                draw.texture_factor = 0x804080c0u;
                light.stage[0] = light.stage[2] = 1;
                light.stage[1] = 2;
                light.stage[3] = i == 17 ? 1 : 2;
                if (i == 15) {
                    light.specular[0][0] = 0.2f;
                    light.specular[0][1] = 0.1f;
                    light.specular[0][2] = 0.05f;
                    light.view[2] = 1;
                    light.view[3] = 8;
                }
            }
            if (i >= 18) {
                draw.transform[12] = (i & 1) ? 0.5f : -0.5f;
                draw.transform[13] = 0.25f;
                draw.transform[5] = 0.4f;
                draw.has_texture = true;
                draw.texture.data = 0x4000u;
                draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
                draw.texture.bits_per_pixel = 32;
                draw.texture.width = draw.texture.height = 1;
                draw.texture_bytes = &base_pixel;
                draw.texture_byte_count = sizeof base_pixel;
                draw.alpha_kill = i == 19;
                light.stage[1] = light.stage[3] = 1;
            }
            if (i >= 20) {
                // SELECTARG1/2(TFACTOR) alpha ignores both texture and lit alpha.
                draw.transform[12] = -0.75f + 0.5f * (i - 20u);
                draw.transform[13] = 1.0f - 2.0f * 148.0f / 240.0f;
                draw.texture.data = 0x5000u + i * 4u;
                draw.texture_factor = i < 22 ? 0x804080c0u : 0x404080c0u;
                light.material_diffuse[3] = (i == 20 || i == 23) ? 0.125f : 0.875f;
                light.stage[0] = 1;
                light.stage[1] = light.stage[3] = 2;
                light.stage[2] = 0;
                draw.depth.alpha_func = RECOMP_D3D_COMPARE_EQUAL;
                draw.depth.alpha_ref = i < 22 ? 128 : 64;
                draw.blend.blend_enable = true;
                draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
                draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
            }
            struct DiffuseVertex { float x, y, z; uint32_t diffuse; float u, v; };
            DiffuseVertex diffuse_vertices[4]{};
            if (vertex_diffuse) {
                for (unsigned j = 0; j < 4; ++j)
                    diffuse_vertices[j] = {vertices[j].x, vertices[j].y, vertices[j].z,
                        i >= 16 ? 0x4080ffc0u : 0x8080ffc0u, vertices[j].u, vertices[j].v};
                draw.fvf = 0x142u;
                draw.vertex_stride = sizeof(DiffuseVertex);
                draw.vertex_bytes = diffuse_vertices;
                light.enabled = false;
            }
            if (i == 8) {
                draw.program_alpha_mask = true;
                passed &= expect("reflection mask overlap", d3d_backend_submit(presenter, &command),
                    RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
                draw.program_alpha_mask = false;
            }
            passed &= expect("lighting draw", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("lighting present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy lighting", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const int expected[][3] = {{102, 77, 108}, {26, 26, 32}, {54, 45, 67}, {6, 13, 26},
                {108, 89, 77}, {255, 0, 0}, {0, 0, 0}, {102, 77, 108}, {30, 7, 10}, {21, 39, 21},
                {33, 83, 44}, {46, 10, 48}, {36, 42, 12}, {12, 32, 32}, {12, 16, 12}, {15, 23, 25}, {12, 16, 12}, {6, 8, 6}, {102, 77, 108}, {0, 0, 0},
                {24, 32, 24}, {24, 32, 24}, {12, 16, 12}, {12, 16, 12}};
            // SRC_ALPHA over black checks reflection alpha as well as RGB. PM clamps before diffuse.
            for (unsigned i = 0; i < 24; ++i) {
                unsigned char bgr[3]{};
                const unsigned x = i >= 20 ? 40 + 80 * (i - 20) : i >= 14 ? 80 + 160 * (i & 1)
                    : i >= 8 ? (160 * (i - 8) + 80) / 3 : 40 + 80 * (i % 4);
                const unsigned y = i >= 20 ? 148 : i >= 18 ? 90 : i >= 16 ? 222 : i >= 14 ? 18 : i >= 8 ? 120 : 60 + 120 * (i / 4);
                std::fseek(file, 54 + (239 - y) * 960 + x * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                for (unsigned c = 0; c < 3; ++c) {
                    const bool pixel_ok = std::abs(int(bgr[c]) - expected[i][c]) <= 1;
                    if (!pixel_ok) std::fprintf(stderr, "FAIL lighting pixel %u channel=%u actual=%u expected=%d\n",
                        i, c, bgr[c], expected[i][c]);
                    passed &= pixel_ok;
                }
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS native lighting, normal and premultiplied reflections, stage saturation and alpha endpoints");
    return passed;
}

// mesh_uv: both blends read UV0 (coordinate mode 0), so stage 2 samples its red texel 0.
static bool testReflectionStage2(bool alpha_kill = false, bool mesh_uv = false)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "en3", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create third reflection", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("third reflection clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z, nx, ny, nz, u, v; };
        const uint16_t indices[] = {0, 1, 2, 3};
        const unsigned alphas[] = {0, 128, 255};
        for (unsigned i = 0; i < 3; ++i) {
            const uint32_t base = alpha_kill && i == 0 ? 0x000000ffu : 0xff0000ffu;
            const uint32_t environment = alpha_kill ? (i == 1 ? 0x00ff0000u : 0xffff0000u) : 0x80ff0000u;
            const float left = -1.0f + i * (2.0f / 3.0f), right = left + 2.0f / 3.0f;
            const Vertex vertices[] = {{left,1,0.5f,0,0,1,0,0}, {right,1,0.5f,0,0,1,0,0},
                {left,-1,0.5f,0,0,1,0,0}, {right,-1,0.5f,0,0,1,0,0}};
            const uint32_t third[] = {0xffff0000u, (alphas[i] << 24) | 0x0000ff00u};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.fvf = 0x112u;
            draw.has_transform = true;
            draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
            draw.blend.color_write_mask = 15;
            draw.blend.blend_enable = !alpha_kill;
            draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
            draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
            draw.has_texture = draw.has_reflection = true;
            draw.reflection_stage2 = !alpha_kill;
            draw.reflection_mesh_uv = draw.reflection_stage2_mesh_uv = mesh_uv;
            draw.alpha_kill = draw.reflection_alpha_kill = alpha_kill;
            draw.texture.data = 0x1000u;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = draw.texture.height = draw.texture.mip_levels = 1;
            draw.texture_bytes = &base;
            draw.texture_byte_count = sizeof base;
            draw.reflection_texture = draw.texture;
            draw.reflection_texture.data = 0x2000u;
            draw.reflection_bytes = &environment;
            draw.reflection_byte_count = sizeof environment;
            draw.reflection_diffuse[0] = draw.reflection_diffuse[3] = 0.5f;
            draw.reflection_diffuse[1] = 0.75f;
            draw.reflection_diffuse[2] = 0.25f;
            draw.reflection_world_view[0] = draw.reflection_world_view[5] =
                draw.reflection_world_view[10] = draw.reflection_world_view[15] = 1;
            std::memcpy(draw.reflection_normal, draw.reflection_world_view, sizeof draw.reflection_normal);
            draw.reflection_transform[12] = draw.reflection_transform[13] = 0.25f;
            draw.reflection_stage2_texture = draw.texture;
            // Refill the same guest storage: alpha 0 -> 128 must invalidate the cached view.
            draw.reflection_stage2_texture.data = 0x3000u;
            draw.reflection_stage2_texture.width = 2;
            draw.reflection_stage2_bytes = third;
            draw.reflection_stage2_byte_count = sizeof third;
            draw.reflection_stage2_address_u = draw.reflection_stage2_address_v = 3;
            draw.reflection_stage2_point_mip_filter = true;
            draw.reflection_stage2_mip_lod_bias = -511.0f / 256.0f;
            // Own TEXTURE2 clamps to green; TEXTURE1, UV0 or WRAP would sample red.
            draw.reflection_stage2_transform[12] = 1.25f;
            draw.reflection_stage2_transform[13] = 0.25f;
            passed &= expect("third reflection draw", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("third reflection present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy third reflection", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const int expected[3][3] = {{8,0,16}, {4,24,8}, {0,96,0}};
            const int killed[3][3] = {{0,0,0}, {0,0,0}, {0,0,128}};
            const int mesh[3] = {0, 0, 64};
            for (unsigned i = 0; i < 3; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + (53 + 107 * i) * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                for (unsigned c = 0; c < 3; ++c) {
                    const int value = alpha_kill ? killed[i][c] : mesh_uv ? mesh[c] : expected[i][c];
                    const bool ok = std::abs(int(bgr[c]) - value) <= 1;
                    if (!ok) std::fprintf(stderr, "FAIL third reflection pixel %u channel=%u actual=%u expected=%d\n",
                        i, c, bgr[c], value);
                    passed &= ok;
                }
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    return passed;
}

// texgen: stage 0 samples the base at generated coordinates (TEXTURE0 matrix)
// while the half-alpha environment keeps mesh UV, as the fight's chrome does.
// uv_transform: no reflection; UV0 is (u, v, 1, 0) times TEXTURE0.
// stage1_texgen: with texgen, stage 1 also generates through its own TEXTURE1 matrix.
// color_only: stage 1 keeps CURRENT alpha, so quarter base alpha survives the opaque environment.
static bool testReflectionAddress(bool texgen, bool uv_transform = false, bool skip_diffuse = false,
    bool stage1_texgen = false, float skin_weight = -1.0f, bool color_only = false)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "env", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create reflection", d3d_backend_create(&config, &presenter));
    if (passed) {
        struct Vertex { float x, y, z, nx, ny, nz, u, v; };
        const Vertex vertices[] = {
            {-1, 1, 0.5f, 0, 0, 1, 0, 0}, {1, 1, 0.5f, 0, 0, 1, 2, 0},
            {-1, -1, 0.5f, 0, 0, 1, 0, 1}, {1, -1, 0.5f, 0, 0, 1, 2, 1}};
        struct SkinVertex { float x, y, z, weight, nx, ny, nz, u, v; };
        SkinVertex skinned[4];
        for (unsigned i = 0; i < 4; ++i) {
            const auto &v = vertices[i];
            skinned[i] = {v.x, v.y, v.z, skin_weight, v.nx, v.ny, v.nz, v.u, v.v};
        }
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t base_pixels[2] = {color_only ? 0x400000ffu : 0xff0000ffu, 0xffffff00u};
        const uint32_t environment[2] = {texgen ? 0x80ff0000u : 0xffff0000u,
            texgen ? 0x8000ff00u : 0xff00ff00u};
        const float white[] = {1, 1, 1, 1};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x112u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_transform = true;
        draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
        draw.has_texture = draw.has_reflection = draw.reflection_mesh_uv = true;
        draw.texture.data = 0x1000u;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = draw.texture.height = 1;
        draw.texture_bytes = base_pixels;
        draw.texture_byte_count = sizeof base_pixels[0];
        draw.reflection_texture = draw.texture;
        draw.reflection_texture.data = 0x2000u;
        draw.reflection_texture.width = 2;
        draw.reflection_bytes = environment;
        draw.reflection_byte_count = sizeof environment;
        draw.reflection_address_u = draw.reflection_address_v = 3u;
        std::memcpy(draw.reflection_diffuse, white, sizeof white);
        if (skip_diffuse) {
            // A stage-2 pass-through leaves the blend unmodulated: grey diffuse must not dim it.
            const float grey[] = {0.5f, 0.5f, 0.5f, 1};
            std::memcpy(draw.reflection_diffuse, grey, sizeof grey);
            draw.reflection_skip_diffuse = true;
        }
        if (texgen) {
            draw.texture.width = 2;
            draw.texture_byte_count = sizeof base_pixels;
            draw.texgen_reflection = true;
            draw.reflection_world_view[0] = draw.reflection_world_view[5] = 1;
            draw.reflection_world_view[10] = draw.reflection_world_view[15] = 1;
            std::memcpy(draw.reflection_normal, draw.reflection_world_view, sizeof draw.reflection_normal);
            // Every generated coordinate lands on the yellow base texel.
            draw.reflection_transform[12] = 0.75f;
            draw.reflection_transform[13] = 0.25f;
            draw.reflection_transform[15] = 1;
        }
        if (stage1_texgen) {
            // Base U -0.25 wraps to yellow; environment U 0.75 picks green, and TEXTURE0
            // or an ungenerated (0, 0) coordinate would clamp to red.
            draw.reflection_mesh_uv = false;
            draw.reflection_transform[12] = -0.25f;
            draw.reflection_stage1_transform[12] = 0.75f;
            draw.reflection_stage1_transform[13] = 0.25f;
            draw.reflection_stage1_transform[15] = 1;
        }
        if (uv_transform) {
            draw.texgen_reflection = draw.has_reflection = draw.reflection_mesh_uv = false;
            draw.texture_transform_uv = true;
            std::memset(draw.reflection_transform, 0, sizeof draw.reflection_transform);
            draw.reflection_transform[0] = draw.reflection_transform[5] = 1;
            // The third row translates 2D coordinates; the fourth must not.
            draw.reflection_transform[8] = 0.5f;
            draw.reflection_transform[12] = 0.125f;
        }
        if (skin_weight >= 0.0f) {
            draw.fvf = 0x116u;
            draw.vertex_bytes = skinned;
            draw.vertex_stride = sizeof(SkinVertex);
            draw.blend_weight_count = 1u;
            std::memcpy(draw.blend_transforms[0], draw.transform, sizeof draw.transform);
            draw.reflection_mesh_uv = false;
            draw.reflection_normalize = true;
            // Endpoint eyes (1,0,1), (-1,0,1) and normals Z, X reflect right.
            // Their equal-weight blend reflects left; either missing blend fails.
            draw.reflection_world_view[12] = draw.reflection_world_view[14] = 1;
            draw.reflection_blend_world_view[0][12] = -1;
            draw.reflection_blend_world_view[0][14] = 1;
            draw.reflection_normal[10] = 1;
            draw.reflection_blend_normal[0][8] = 1;
            draw.reflection_transform[0] = draw.reflection_transform[12] = 0.5f;
            draw.reflection_transform[13] = 0.25f;
        }
        if (color_only) {
            draw.reflection_color_only = true;
            draw.blend.blend_enable = true;
            draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
            draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
            auto clear = clearCommand();
            clear.data.clear.color = 0xff000000u;
            passed &= expect("color-only reflection clear", d3d_backend_submit(presenter, &clear));
        }
        passed &= expect("clamped reflection draw", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("reflection present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy reflection", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // U 0.25 samples the red texel; U 1.25 clamps to green (WRAP would give red).
            // With texgen, yellow base halves with them (mesh UV would give blue base).
            const unsigned xs[] = {40, 200};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                // U 0.25 and 1.25 move to the yellow texel centre (0.75, wrapping).
                const bool pixel_ok = skin_weight >= 0.0f
                    ? bgr[0] < 10 && (skin_weight == 0.5f
                        ? bgr[2] > 245 && bgr[1] < 10 : bgr[1] > 245 && bgr[2] < 10)
                    : color_only ? bgr[0] < 10 && std::abs(int(bgr[2 - i]) - 64) <= 2 && bgr[1 + i] < 10
                    : uv_transform ? bgr[2] > 245 && bgr[1] > 245 && bgr[0] < 10
                    : stage1_texgen
                    ? bgr[0] < 10 && bgr[1] > 250 && std::abs(int(bgr[2]) - 128) <= 3
                    : texgen
                    ? bgr[0] < 10 && std::abs(int(bgr[2 - i]) - 255) <= 3 && std::abs(int(bgr[1 + i]) - 128) <= 3
                    : i == 0 ? bgr[2] > 245 && bgr[1] < 10 : bgr[1] > 245 && bgr[2] < 10;
                if (!pixel_ok) std::fprintf(stderr, "FAIL reflection address pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts(skin_weight >= 0.0f ? "PASS skinned reflection eye, normal and remainder weights"
        : color_only ? "PASS color-only reflection blend keeps stage-0 alpha"
        : stage1_texgen ? "PASS both stages generate through their own texture matrices"
        : skip_diffuse ? "PASS reflection stage-2 pass-through skips the final diffuse"
        : uv_transform ? "PASS stage-0 texture matrix translates 2D UV by its third row"
        : texgen ? "PASS stage-0 texgen pairs with a mesh-UV reflection blend"
        : "PASS reflection texture samples with its own stage address mode");
    return passed;
}

static bool testScreenSpaceOffsetAndScissor()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "off", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create screen offset", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("offset clear", d3d_backend_submit(presenter, &clear));
        const float offsets[][2] = {{12, 10}, {-12, -10}, {20, -14}, {-20, 14}};
        const uint16_t indices[] = {0, 1, 2, 3};
        for (unsigned i = 0; i < 4 && passed; ++i) {
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            std::memcpy(draw.screen_space_offset, offsets[i], sizeof offsets[i]);
            struct ScreenVertex { float x, y, z, rhw; uint32_t diffuse; };
            ScreenVertex screen[] = {{20, 20, 0.25f, 0.5f, 0xffffffffu}, {28, 20, 0.25f, 0.25f, 0xffffffffu},
                {20, 28, 0.25f, 0.5f, 0xffffffffu}, {28, 28, 0.25f, 0.25f, 0xffffffffu}};
            struct BlendVertex { float x, y, z, weights[3]; uint32_t diffuse; };
            const BlendVertex blended[] = {{-0.5f, 1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu},
                {0.5f, 1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu},
                {-0.5f, -1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu},
                {0.5f, -1, 0.5f, {0.1f, 0.2f, 0.3f}, 0xffffffffu}};
            struct Vertex { float x, y, z; uint32_t diffuse; };
            const Vertex vertices[] = {{-0.05f, 0.1f, 0.5f, 0xffffffffu}, {0.05f, 0.1f, 0.5f, 0xffffffffu},
                {-0.05f, -0.1f, 0.5f, 0xffffffffu}, {0.05f, -0.1f, 0.5f, 0xffffffffu}};
            if (i < 2) {
                if (i == 1) for (auto &vertex : screen) vertex.x += 60;
                draw.fvf = 0x44u;
                draw.vertex_stride = sizeof(ScreenVertex);
                draw.vertex_bytes = screen;
            } else {
                draw.has_transform = true;
                if (i == 2) {
                    draw.fvf = 0x4au;
                    draw.vertex_stride = sizeof(BlendVertex);
                    draw.vertex_bytes = blended;
                    draw.blend_weight_count = 3;
                    draw.transform[0] = -0.7f;
                    draw.transform[3] = 1.5f;
                    draw.transform[5] = 0.1f;
                    draw.transform[10] = 1;
                    draw.transform[12] = -1;
                    draw.transform[15] = 2;
                    for (auto &transform : draw.blend_transforms)
                        std::memcpy(transform, draw.transform, sizeof draw.transform);
                } else {
                    draw.fvf = 0x42u;
                    draw.vertex_stride = sizeof(Vertex);
                    draw.vertex_bytes = vertices;
                    for (unsigned j = 0; j < 16; j += 5) draw.transform[j] = 1;
                    const float viewport[] = {160, 80, 160, 120, 0, 1};
                    std::memcpy(draw.viewport, viewport, sizeof viewport);
                }
            }
            passed &= expect("screen offset draw", d3d_backend_submit(presenter, &command));
            if (i == 0) {
                for (unsigned axis = 0; axis < 2; ++axis) {
                    for (float invalid : {INFINITY, NAN}) {
                        draw.screen_space_offset[axis] = invalid;
                        passed &= expect("nonfinite screen offset", d3d_backend_submit(presenter, &command),
                            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
                    }
                    draw.screen_space_offset[axis] = offsets[i][axis];
                }
                const float program_vertices[4][8]{};
                draw.fvf = 0x112u;
                draw.vertex_stride = sizeof program_vertices[0];
                draw.vertex_bytes = program_vertices;
                draw.program_count = 1;
                draw.program[0][3] = 1; // One synthetic terminating NOP.
                draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
                draw.screen_space_offset[0] = draw.screen_space_offset[1] = 0;
                passed &= expect("zero program screen offset", d3d_backend_submit(presenter, &command));
                draw.screen_space_offset[0] = 0.5f;
                passed &= expect("unsupported program screen offset", d3d_backend_submit(presenter, &command),
                    RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
            }
        }
        struct ScreenVertex { float x, y, z, rhw; uint32_t diffuse; };
        ScreenVertex vertices[] = {{20, 180, 0.25f, 1, 0xffffffffu}, {148, 180, 0.25f, 1, 0xffffffffu},
            {20, 236, 0.25f, 1, 0xffffffffu}, {148, 236, 0.25f, 1, 0xffffffffu}};
        RecompD3dPresenterCommand clipped{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = clipped.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.fvf = 0x44u;
        draw.vertex_stride = sizeof(ScreenVertex);
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.screen_space_offset[0] = 8;
        draw.screen_space_offset[1] = 4;
        draw.has_scissor = true;
        const int32_t scissor[] = {40, 192, 100, 216};
        std::memcpy(draw.scissor, scissor, sizeof scissor);
        passed &= expect("scissor draw", d3d_backend_submit(presenter, &clipped));
        draw.scissor[2] = 39;
        passed &= expect("inverted scissor", d3d_backend_submit(presenter, &clipped),
            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        draw.scissor[2] = 100;
        draw.has_scissor = false;
        for (auto &vertex : vertices) vertex.x += 152;
        passed &= expect("scissor disabled draw", d3d_backend_submit(presenter, &clipped));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("offset present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy offset", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // Offset positions move; scissor edges stay absolute and exclude right/bottom.
            const unsigned pixels[][3] = {{34, 32, 255}, {22, 22, 0}, {70, 12, 255}, {82, 22, 0},
                {98, 106, 255}, {80, 120, 0}, {106, 106, 0}, {220, 154, 255}, {240, 140, 0},
                {40, 192, 255}, {99, 215, 255}, {39, 200, 0}, {100, 200, 0}, {60, 191, 0}, {60, 216, 0},
                {200, 200, 255}, {280, 224, 255}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - pixel[1]) * 960 + pixel[0] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] == pixel[2] && bgr[1] == pixel[2] && bgr[2] == pixel[2];
                if (!pixel_ok) std::fprintf(stderr, "FAIL offset/scissor pixel (%u,%u) BGR=%u,%u,%u expected=%u\n",
                    pixel[0], pixel[1], bgr[0], bgr[1], bgr[2], pixel[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS screen-space offsets, XYZRHW, blended transforms, clip W, subviewport and scissor reset");
    return passed;
}

static bool testBrightPass()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "brt", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create bright pass", d3d_backend_create(&config, &presenter));
    if (passed) {
        // 3x3 tiles: red 0.75 with a 1.0 centre, green 0.9, blue 0.5 (below threshold).
        uint32_t texels[9 * 9];
        for (unsigned y = 0; y < 9; ++y)
            for (unsigned x = 0; x < 9; ++x)
                texels[y * 9 + x] = (x % 3 == 1 && y % 3 == 1 ? 0xffff0000u : 0xffbf0000u) | 0xe580u;
        struct Vertex { float x, y, z, rhw, u, v; };
        const Vertex vertices[] = {{40, 40, 0, 1, 0, 0}, {43, 40, 0, 1, 9, 0},
            {40, 43, 0, 1, 0, 9}, {43, 43, 0, 1, 9, 9}};
        const uint16_t indices[] = {0, 1, 2, 3};
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("bright clear", d3d_backend_submit(presenter, &clear));
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x104u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        // The game's -0.5 offset puts each output centre on its tile's middle texel.
        draw.screen_space_offset[0] = draw.screen_space_offset[1] = -0.5f;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = 0x12u;
        draw.texture.linear = true;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = draw.texture.height = 9;
        draw.texture.pitch = 36;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x2000u;
        draw.texture_bytes = texels;
        draw.texture_byte_count = sizeof texels;
        draw.bright_pass = true;
        draw.bright_pass_threshold = 0x1a1a1a1au;
        passed &= expect("bright pass draw", d3d_backend_submit(presenter, &command));
        draw.four_tap_filter = true;
        passed &= expect("bright pass excludes four tap", d3d_backend_submit(presenter, &command),
            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        // Four texel-centred taps of a 2x2 texture: red 0.6, 0.3, 0.9, 0; green 1.
        const uint32_t taps[] = {0xff99ff00u, 0xff4dff00u, 0xffe6ff00u, 0xff00ff00u};
        struct TapVertex { float x, y, z, rhw, uv[8]; };
        TapVertex quad[4]{};
        for (unsigned i = 0; i < 4; ++i) {
            quad[i] = {100.0f + (i & 1) * 8, 40.0f + (i >> 1) * 8, 0, 1,
                {0.5f, 0.5f, 1.5f, 0.5f, 0.5f, 1.5f, 1.5f, 1.5f}};
        }
        RecompD3dPresenterCommand tap{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &filter = tap.data.draw;
        filter.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        filter.index_count = filter.vertex_count = 4;
        filter.triangle_count = 2;
        filter.vertex_stride = sizeof(TapVertex);
        filter.fvf = 0x404u;
        filter.vertex_bytes = quad;
        filter.index_bytes = indices;
        filter.blend.color_write_mask = 15;
        filter.has_texture = true;
        filter.texture = draw.texture;
        filter.texture.width = filter.texture.height = 2;
        filter.texture.pitch = 8;
        filter.texture.data = 0x3000u;
        filter.texture_bytes = taps;
        filter.texture_byte_count = sizeof taps;
        filter.four_tap_filter = true;
        // Identity variant: 1/3 and 1/6 weights; doubled variant: 0x40 weights, x2 then /2.
        const uint32_t weights[2][2] = {{0x55555555u, 0x2b2b2b2bu}, {0x40404040u, 0x40404040u}};
        for (unsigned v = 0; v < 2 && passed; ++v) {
            for (unsigned i = 0; i < 4; ++i) filter.four_tap_factors[i] = weights[v][i & 1];
            filter.four_tap_scale = v ? 2.0f : 1.0f;
            filter.four_tap_sum_scale = v ? 0.5f : 1.0f;
            passed &= expect("four tap draw", d3d_backend_submit(presenter, &tap));
            for (auto &vertex : quad) vertex.x += 20;
        }
        // Composite: 2 * texture * 0.8, saturating at red 0.75 and keeping green 0.5 -> 0.8.
        const uint32_t composite_texel = 0xffbf8000u;
        RecompD3dPresenterCommand composite = command;
        auto &mod = composite.data.draw;
        mod.bright_pass = mod.four_tap_filter = false;
        mod.texture.width = mod.texture.height = 1;
        mod.texture.pitch = 4;
        mod.texture.data = 0x4000u;
        mod.texture_bytes = &composite_texel;
        mod.texture_byte_count = sizeof composite_texel;
        mod.texture_factor = 0xccccccccu;
        mod.texture_factor_color_scale = 2u;
        mod.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
        mod.material_alpha = 1.0f;
        Vertex composite_quad[4];
        std::memcpy(composite_quad, vertices, sizeof vertices);
        for (auto &vertex : composite_quad) { vertex.x += 100; vertex.u = vertex.v = 0.5f; }
        mod.vertex_bytes = composite_quad;
        passed &= expect("modulate2x draw", d3d_backend_submit(presenter, &composite));
        // Trail strips: 1x texture * factor, red 0.6 and green 0.4.
        mod.texture_factor_color_scale = 0u;
        mod.texture_factor_modulate = true;
        Vertex trail_quad[4];
        std::memcpy(trail_quad, composite_quad, sizeof composite_quad);
        for (auto &vertex : trail_quad) vertex.x += 40;
        mod.vertex_bytes = trail_quad;
        passed &= expect("modulate draw", d3d_backend_submit(presenter, &composite));
        // 4x color clamps red and leaves alpha independent of the factor's zero alpha.
        mod.texture_factor_modulate = false;
        mod.texture_factor_color_scale = 4u;
        mod.texture_factor = 0x00663300u;
        mod.depth.alpha_test_enable = true;
        mod.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
        mod.depth.alpha_ref = 128u;
        for (auto &vertex : trail_quad) vertex.x += 40;
        passed &= expect("modulate4x draw", d3d_backend_submit(presenter, &composite));
        mod.texture_factor_color_scale = 3u;
        passed &= expect("invalid color scale", d3d_backend_submit(presenter, &composite),
            RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
        // Luminance tint: l = 0.298 + 0.588 * 0.502 = 0.593 times (1, 0.5, 0).
        const uint32_t tint_texel = 0xffff8000u;
        mod.texture_factor_color_scale = 0u;
        mod.depth.alpha_test_enable = false;
        mod.texture_bytes = &tint_texel;
        mod.texture_byte_count = sizeof tint_texel;
        mod.texture.data = 0x5000u;
        mod.luminance_tint = true;
        mod.luminance_tint_factors[0] = 0x004c961du;
        mod.luminance_tint_factors[1] = 0xffff8000u;
        for (auto &vertex : trail_quad) vertex.x += 40;
        passed &= expect("luminance tint draw", d3d_backend_submit(presenter, &composite));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("bright present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy bright pass", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // Gaussian red 0.8125 -> 107; linear 1.0 would give 203, a box filter 90.
            const unsigned pixels[][5] = {{40, 40, 107, 152, 0}, {42, 41, 107, 152, 0}, {43, 43, 0, 0, 0},
                {104, 44, 141, 255, 0}, {124, 44, 115, 255, 0}, {141, 41, 255, 204, 0}, {181, 41, 153, 102, 0}, {221, 41, 255, 102, 0},
                {261, 41, 151, 76, 0}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - pixel[1]) * 960 + pixel[0] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[2] - static_cast<int>(pixel[2])) <= 2 &&
                    std::abs(bgr[1] - static_cast<int>(pixel[3])) <= 2 && bgr[0] == pixel[4];
                if (!pixel_ok) std::fprintf(stderr, "FAIL bright pixel (%u,%u) BGR=%u,%u,%u\n",
                    pixel[0], pixel[1], bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS Gaussian-cubic bright pass, four-tap filters, 2x/4x composites and luminance tint");
    return passed;
}

// DOT_ST distortion: t2 is read at (dot(uv1, uv2.x; n), dot(uv2.y, uv3; n)) with n the signed t0 texel.
static bool testDistortion()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "dst", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create distortion", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("distortion clear", d3d_backend_submit(presenter, &clear));
        // Normal texel (255, 128, 128) is n = (1, 0, 0); ZERO_TO_ONE would give (1, 0.5, 0.5).
        const uint32_t normal = 0xffff8080u;
        const uint32_t scene[2] = {0xffff0000u, 0xff00ff00u};
        struct Vertex { float x, y, z, rhw, uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        // Quad A reads s = 0.5 (red; unsigned n would add 2 and clamp to green), quad B s = 1.5 (green).
        const float s[2] = {0.5f, 1.5f};
        for (unsigned q = 0; q < 2 && passed; ++q) {
            Vertex quad[4]{};
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {40.0f + q * 160 + (i & 1) * 20, 100.0f + (i >> 1) * 40, 0, 1,
                    {0.5f, 0.5f, s[q], 4, 0, 0.5f, 4, 4}};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x404u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = 0x12u;
            draw.texture.linear = true;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = 2;
            draw.texture.height = 1;
            draw.texture.pitch = 8;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x2000u;
            draw.texture_bytes = scene;
            draw.texture_byte_count = sizeof scene;
            draw.reflection_texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.reflection_texture.bits_per_pixel = 32;
            draw.reflection_texture.width = draw.reflection_texture.height = 1;
            draw.reflection_texture.mip_levels = 1;
            draw.reflection_texture.data = 0x3000u;
            draw.reflection_bytes = &normal;
            draw.reflection_byte_count = sizeof normal;
            draw.reflection_address_u = draw.reflection_address_v = 3;
            draw.distortion = true;
            passed &= expect("distortion draw", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("distortion present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy distortion", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const unsigned xs[] = {50, 210};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] < 10 && (i == 0 ? bgr[2] > 245 && bgr[1] < 10 : bgr[1] > 245 && bgr[2] < 10);
                if (!pixel_ok) std::fprintf(stderr, "FAIL distortion pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS DOT_ST distortion reads the scene at signed normal dot products");
    return passed;
}

// Stage-effect overlays: unlit SELECTARG1(TEXTURE) over diffuse, texture-factor
// alpha, a stage-1 UV1 alpha mask with ALPHAKILL, and projected camera-space texgen.
static bool testStageEffect()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "eff", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create stage effect", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("effect clear", d3d_backend_submit(presenter, &clear));
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t green = 0xff00ff00u;
        // Pass 2 is FUN_0027be10's kill-only mask over UV0 (FVF 0x144).
        const uint32_t masks[3][2] = {{0xff000000u, 0x80000000u}, {0xff000000u, 0x00000000u},
            {0x80000000u, 0x00000000u}};
        struct Screen { float x, y, z, rhw; uint32_t diffuse; float u0, v0, u1, v1; };
        for (unsigned pass = 0; pass < 3 && passed; ++pass) {
            Screen quad[4];
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {20.0f + pass * 60 + (i & 1) * 40, 20.0f + (i >> 1) * 40, 0, 1, 0xffff0000u,
                    pass == 2 ? float(i & 1) : 0.5f, 0.5f, pass == 2 ? 0.5f : float(i & 1), 0.5f};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Screen);
            draw.fvf = pass == 2 ? 0x144u : 0x244u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.blend.blend_enable = pass != 1;
            draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
            draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
            draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = draw.texture.height = 1;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = &green;
            draw.texture_byte_count = sizeof green;
            draw.reflection_texture = draw.texture;
            draw.reflection_texture.width = 2;
            draw.reflection_texture.data = 0x2000u + pass * 0x100u;
            draw.reflection_bytes = masks[pass];
            draw.reflection_byte_count = sizeof masks[pass];
            draw.reflection_address_u = draw.reflection_address_v = 3;
            draw.directional.stage_mode = true;
            draw.directional.stage[0] = draw.directional.stage[3] = 1;
            draw.texture_factor = 0x80ffffffu;
            draw.modulate_texture_factor_alpha = true;
            draw.stage1_alpha_mask = draw.stage1_alpha_kill = true;
            draw.stage1_kill_only = pass == 2;
            passed &= expect("masked overlay draw", d3d_backend_submit(presenter, &command));
        }
        // Eight texels, red then green; s / q = (x + 1) / 2 across the screen.
        uint32_t strip[8];
        for (unsigned i = 0; i < 8; ++i) strip[i] = i < 4 ? 0xffff0000u : 0xff00ff00u;
        struct World { float x, y, z; uint32_t diffuse; float u, v; };
        // The lower half of the screen; the overlays stay above it.
        const World quad[] = {{-1, 0, 0.5f, 0xff0000ffu, 0, 0}, {1, 0, 0.5f, 0xff0000ffu, 0, 0},
            {-1, -1, 0.5f, 0xff0000ffu, 0, 0}, {1, -1, 0.5f, 0xff0000ffu, 0, 0}};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(World);
        draw.fvf = 0x142u;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_transform = true;
        draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 8;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x3000u;
        draw.texture_bytes = strip;
        draw.texture_byte_count = sizeof strip;
        draw.texgen_projected = true;
        draw.reflection_world_view[0] = draw.reflection_world_view[5] = 1;
        draw.reflection_world_view[10] = draw.reflection_world_view[15] = 1;
        // s = x + 2z, t = 2z, q = 4z: with z = 0.5, q = 2 and only the divide maps s into [0, 1].
        draw.reflection_transform[0] = 1;
        draw.reflection_transform[8] = 2;
        draw.reflection_transform[9] = 2;
        draw.reflection_transform[10] = 4;
        draw.reflection_transform[15] = 1;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[2] = draw.directional.stage[3] = 1;
        draw.texture_factor = 0xffffffffu;
        draw.modulate_texture_factor_alpha = true;
        // 0x257ee0's ADD stages: slot 1 adds (64, 64), an unbound stage passes, slot 2 adds 16 red.
        const uint32_t added[2] = {0xff404000u, 0xff100000u};
        draw.reflection_texture = draw.reflection_stage2_texture = draw.texture;
        draw.reflection_texture.width = draw.reflection_stage2_texture.width = 1;
        draw.reflection_texture.data = 0x5000u;
        draw.reflection_stage2_texture.data = 0x6000u;
        draw.reflection_bytes = &added[0];
        draw.reflection_stage2_bytes = &added[1];
        draw.reflection_byte_count = draw.reflection_stage2_byte_count = sizeof added[0];
        draw.projected_stage_count = 3;
        draw.projected_slots[0] = 1;
        draw.projected_slots[1] = -1;
        draw.projected_slots[2] = 2;
        for (auto &m : draw.projected_transforms) m[0] = m[5] = m[10] = m[15] = 1;
        passed &= expect("projected draw", d3d_backend_submit(presenter, &command));
        draw.projected_stage_count = 0;
        // COUNT2 uses Q=1. Mesh UVs are zero; camera translation and z contribute to S.
        const World unprojected[] = {{0.375f, 0.75f, 0.5f, 0xff0000ffu, 0, 0},
            {0.875f, 0.75f, 0.5f, 0xff0000ffu, 0, 0},
            {0.375f, 0.25f, 0.5f, 0xff0000ffu, 0, 0}, {0.875f, 0.25f, 0.5f, 0xff0000ffu, 0, 0}};
        draw.vertex_bytes = unprojected;
        draw.reflection_world_view[12] = 0.25f;
        std::memset(draw.reflection_transform, 0, sizeof draw.reflection_transform);
        draw.reflection_transform[0] = 2;
        draw.reflection_transform[8] = 1;
        draw.reflection_transform[12] = -1.75f;
        draw.reflection_transform[13] = 0.5f;
        draw.reflection_transform[14] = 1;
        passed &= expect("unprojected position draw", d3d_backend_submit(presenter, &command));
        // FVF 0x102 has no diffuse stream: texture times factor in both channels.
        const float white_quad[4][5] = {{-0.875f,1.0f/3,0.5f,0,0}, {-0.625f,1.0f/3,0.5f,0,0},
            {-0.875f,1.0f/6,0.5f,0,0}, {-0.625f,1.0f/6,0.5f,0,0}};
        draw.fvf = 0x102u;
        draw.vertex_stride = sizeof white_quad[0];
        draw.vertex_bytes = white_quad;
        draw.texgen_projected = false;
        draw.texture.width = 1;
        draw.texture.data = 0x4000u;
        draw.texture_bytes = &green;
        draw.texture_byte_count = sizeof green;
        draw.directional.stage[0] = draw.directional.stage[2] = 1;
        draw.directional.stage[1] = draw.directional.stage[3] = 2;
        draw.texture_factor = 0x804080c0u;
        draw.modulate_texture_factor_alpha = false;
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        passed &= expect("white texture-factor draw", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("effect present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy stage effect", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // {x, y, R, G}: masked green at alpha 0.5 and 0.25, then 1.0 and killed, then
            // kill-only at factor alpha 0.5 and killed;
            // the projected quad reads red at s/q 0.31 (s 0.63 would be green), green at 0.69,
            // plus its ADD stages, saturating red.
            const int pixels[][4] = {{25, 40, 0, 128}, {55, 40, 0, 64}, {85, 40, 0, 255}, {115, 40, 0, 0},
                {145, 40, 0, 128}, {175, 40, 0, 0}, {100, 200, 255, 64}, {220, 200, 80, 255},
                {240, 60, 255, 0}, {280, 60, 0, 255}, {40, 90, 0, 64}};
            for (const auto &pixel : pixels) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - pixel[1]) * 960 + pixel[0] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[2] - pixel[2]) <= 3 && std::abs(bgr[1] - pixel[3]) <= 3 &&
                    bgr[0] < 4;
                if (!pixel_ok) std::fprintf(stderr, "FAIL stage effect pixel (%d,%d) BGR=%u,%u,%u\n",
                    pixel[0], pixel[1], bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS stage-effect overlays: texture select, factor alpha, UV1 mask/kill, UV0 kill, projected texgen and ADD stages");
    return passed;
}

// Vertex-program MAX and SLT over a position/normal stream (FVF 0x012), as in
// the projected shadow program: max(v0, c1) clamps x >= 0 and slt(v2, c2) = 1.
static bool testProgramMaxSlt()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "vps", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create program", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("program clear", d3d_backend_submit(presenter, &clear));
        const float vertices[4][6] = {{-1, 1, 0.5f, 0, 0, 0}, {1, 1, 0.5f, 0, 0, 0},
            {-1, -1, 0.5f, 0, 0, 0}, {1, -1, 0.5f, 0, 0, 0}};
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t texels[2] = {0xffff0000u, 0xff00ff00u};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof vertices[0];
        draw.fvf = 0x012u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 2;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = texels;
        draw.texture_byte_count = sizeof texels;
        // a: mac, attribute, constant, A swizzle; b: A and B mux/register/swizzle; c: output.
        const uint32_t xyzw = 0x1bu;
        draw.program_count = 2;
        draw.program[0][1] = (10u << 21) | (1u << 13) | xyzw;
        draw.program[0][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
        draw.program[0][3] = (15u << 12) | 2048u;
        draw.program[1][1] = (11u << 21) | (2u << 9) | (2u << 13) | xyzw;
        draw.program[1][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
        draw.program[1][3] = (15u << 12) | (9u << 3) | 2048u | 1u;
        const float c1[4] = {0, -2, 0.5f, 1}, c2[4] = {1, 1, 1, 2};
        std::memcpy(draw.program_constants[1], c1, sizeof c1);
        std::memcpy(draw.program_constants[2], c2, sizeof c2);
        draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
        passed &= expect("max/slt program draw", d3d_backend_submit(presenter, &command));
        // Position/color/UV (FVF 0x142): oD0 = v3 feeds MODULATE(TEXTURE, oD0) color;
        // alpha is texture times the factor's 0.5, ignoring oD0's 0.5 alpha.
        struct Colored { float x, y, z; uint32_t color; float u, v; };
        const Colored colored[] = {{-1, 1, 0.5f, 0x80ff0000u, 0, 0}, {0, 1, 0.5f, 0x80ff0000u, 0, 0},
            {-1, -1, 0.5f, 0x80ff0000u, 0, 0}, {0, -1, 0.5f, 0x80ff0000u, 0, 0}};
        const uint32_t white = 0xffffffffu;
        RecompD3dPresenterCommand second = command;
        auto &color = second.data.draw;
        color.fvf = 0x142u;
        color.vertex_stride = sizeof(Colored);
        color.vertex_bytes = colored;
        color.texture.width = 1;
        color.texture.data = 0x2000u;
        color.texture_bytes = &white;
        color.texture_byte_count = sizeof white;
        color.blend.blend_enable = true;
        color.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        color.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        color.blend.op = RECOMP_D3D_BLEND_OP_ADD;
        color.directional.stage_mode = true;
        color.directional.stage[0] = color.directional.stage[1] = color.directional.stage[2] = 1;
        color.texture_factor = 0x80ffffffu;
        color.modulate_texture_factor_alpha = true;
        const uint32_t movs[3][2] = {{0u, 0u}, {3u, 3u}, {9u, 9u}};
        color.program_count = 3;
        for (unsigned i = 0; i < 3; ++i) {
            color.program[i][1] = (1u << 21) | (movs[i][0] << 9) | xyzw;
            color.program[i][2] = 2u << 26;
            color.program[i][3] = (15u << 12) | (movs[i][1] << 3) | 2048u | (i == 2 ? 1u : 0u);
        }
        passed &= expect("color program draw", d3d_backend_submit(presenter, &second));
        // Clamp U and wrap V independently: (1.25, 1.25) selects top-right green.
        const Colored addressed[] = {{0, 1, 0.5f, white, 1.25f, 1.25f}, {1, 1, 0.5f, white, 1.25f, 1.25f},
            {0, 0, 0.5f, white, 1.25f, 1.25f}, {1, 0, 0.5f, white, 1.25f, 1.25f}};
        const uint32_t corners[] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, white};
        color.vertex_bytes = addressed;
        color.texture.width = color.texture.height = 2;
        color.texture.data = 0x3000u;
        color.texture_bytes = corners;
        color.texture_byte_count = sizeof corners;
        color.address_u = 3;
        color.address_v = 1;
        color.blend.blend_enable = false;
        color.directional.stage_mode = false;
        color.modulate_texture_factor_alpha = false;
        passed &= expect("independent program addressing", d3d_backend_submit(presenter, &second));
        // Kill tests sampled alpha, before a zero TFACTOR makes output alpha zero.
        for (unsigned q = 0; q < 2; ++q) {
            Colored quad[4];
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {q * 0.5f + (i & 1) * 0.5f, -1.0f / 3 - (i >> 1) * (2.0f / 3),
                    0.5f, white, 0.5f, 0.5f};
            const uint32_t texel = q == 0 ? 0x00ff0000u : 0xffff0000u;
            color.vertex_bytes = quad;
            color.texture.width = color.texture.height = 1;
            color.texture.data = 0x4000u + q * 0x100u;
            color.texture_bytes = &texel;
            color.texture_byte_count = sizeof texel;
            color.alpha_kill = color.modulate_texture_factor_alpha = true;
            color.texture_factor = 0x00ffffffu;
            passed &= expect("program alpha kill", d3d_backend_submit(presenter, &second));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("program present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy program", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // MAX keeps the left half for the color program (red at half alpha);
            // SLT's (1, 1, 1, 1) samples the green texel.
            const unsigned xs[] = {80, 240};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] < 4 && (i == 0 ? bgr[1] < 4 && std::abs(bgr[2] - 128) <= 3
                    : bgr[2] < 4 && bgr[1] > 250);
                if (!pixel_ok) std::fprintf(stderr, "FAIL program max/slt pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            unsigned char bgr[3]{};
            std::fseek(file, 54 + (239 - 60) * 960 + 240 * 3, SEEK_SET);
            passed &= std::fread(bgr, 1, 3, file) == 3;
            const bool addressed_ok = bgr[0] < 4 && bgr[1] > 250 && bgr[2] < 4;
            if (!addressed_ok) std::fprintf(stderr, "FAIL program U clamp V wrap BGR=%u,%u,%u\n",
                bgr[0], bgr[1], bgr[2]);
            passed &= addressed_ok;
            for (unsigned q = 0; q < 2; ++q) {
                std::fseek(file, 54 + (239 - 200) * 960 + (200 + q * 80) * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool killed_ok = bgr[0] < 4 && (q == 0 ? bgr[1] > 250 && bgr[2] < 4
                    : bgr[2] > 250 && bgr[1] < 4);
                if (!killed_ok) std::fprintf(stderr, "FAIL program alpha kill %u BGR=%u,%u,%u\n",
                    q, bgr[0], bgr[1], bgr[2]);
                passed &= killed_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS vertex-program MAX/SLT over a position/normal stream");
    return passed;
}

#ifndef RECOMP_TEST_D3D11
/* The boot precompile builds each guest vertex program's draw shader, so the
   program's first draw finds it and compiles nothing; a program the config
   lacks, or holds past RECOMP_D3D_BOOT_PROGRAMS, compiles once. The presenter
   keeps its own copy of the programs. */
static bool testBootProgramShaders(bool scaled)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "vpb", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_SCALE", scaled ? "2" : "");
    _putenv_s("RECOMP_D3D_MSAA", scaled ? "4" : "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "0");
    // Two instructions writing oPos, encoded as the MAX/SLT program test does.
    const uint32_t xyzw = 0x1bu;
    const auto setProgram = [xyzw](uint32_t (*program)[4]) {
        program[0][1] = (10u << 21) | (1u << 13) | xyzw;
        program[0][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
        program[0][3] = (15u << 12) | 2048u;
        program[1][1] = (11u << 21) | (2u << 9) | (2u << 13) | xyzw;
        program[1][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
        program[1][3] = (15u << 12) | (9u << 3) | 2048u | 1u;
    };
    RecompD3dBootProgram programs[RECOMP_D3D_BOOT_PROGRAMS + 1u] = {};
    programs[0].fvf = 0x012u;
    programs[0].count = 2u;
    // True: a scaled target draws it with the sample depth bias; scale 1 ignores it.
    programs[0].depth_bias = true;
    setProgram(programs[0].tokens);
    for (RecompD3dBootProgram &program : programs) program = programs[0];
    // The late program below, one past the limit: the presenter must not build it.
    programs[RECOMP_D3D_BOOT_PROGRAMS].tokens[1][3] = (15u << 12) | (10u << 3) | 2048u | 1u;
    auto boot_config = config;
    boot_config.programs = programs;
    boot_config.program_count = RECOMP_D3D_BOOT_PROGRAMS + 1u;
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create boot program", d3d_backend_create(&boot_config, &presenter));
    std::memset(programs, 0, sizeof programs);
    if (passed) {
        const unsigned boot = d3d_backend_draw_shader_count(presenter);
        if (boot == 0u) std::fprintf(stderr, "FAIL boot program built no shaders\n");
        passed &= boot != 0u;
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("boot program clear", d3d_backend_submit(presenter, &clear));
        const float vertices[4][6] = {{-1, 1, 0.5f, 0, 0, 0}, {1, 1, 0.5f, 0, 0, 0},
            {-1, -1, 0.5f, 0, 0, 0}, {1, -1, 0.5f, 0, 0, 0}};
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t texels[2] = {0xffff0000u, 0xff00ff00u};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof vertices[0];
        draw.fvf = 0x012u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 2;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = texels;
        draw.texture_byte_count = sizeof texels;
        draw.slope_scaled_depth_bias = scaled ? 1.0f / 16 : 0.0f;
        draw.program_count = 2u;
        setProgram(draw.program);
        draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
        passed &= expect("boot program draw", d3d_backend_submit(presenter, &command));
        const unsigned after = d3d_backend_draw_shader_count(presenter);
        if (after != boot) std::fprintf(stderr, "FAIL boot program draw compiled %u shaders, had %u\n",
            after, boot);
        passed &= after == boot;
        // One output index away, so the tokens hash differently.
        RecompD3dPresenterCommand other = command;
        other.data.draw.program[1][3] = (15u << 12) | (10u << 3) | 2048u | 1u;
        passed &= expect("late program draw", d3d_backend_submit(presenter, &other));
        const unsigned late = d3d_backend_draw_shader_count(presenter);
        if (late != boot + 1u) std::fprintf(stderr, "FAIL late program compiled %u shaders, had %u\n",
            late, boot);
        passed &= late == boot + 1u;
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("boot program present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy boot program", d3d_backend_destroy(&presenter));
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    _putenv_s("RECOMP_D3D_MSAA", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
    DeleteFileA(path);
    if (passed) std::printf("PASS boot vertex program%s compiled before its first draw\n",
        scaled ? " with scaled MSAA depth bias" : "");
    return passed;
}
#endif

// Vertex-program LIT and SGE over the stage effect's four-UV stream (FVF 0x442):
// oT0 = lit(v10).zwxx * sge(v11, c3).yyyy, so x = v10.y only when v10.x > 0.
static bool testProgramLitSge()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "vpl", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create lit", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("lit clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; float uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t texels[2] = {0xffff0000u, 0xff00ff00u};
        const uint32_t xyzw = 0x1bu;
        // Left quad: v10.x > 0, so lit.z = 0.75 picks green. Right: lit.z = 0 keeps red.
        const float lit_x[2] = {0.5f, -0.5f};
        for (unsigned q = 0; q < 2 && passed; ++q) {
            Vertex quad[4]{};
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {-1.0f + q + (i & 1), 1.0f - 2.0f * (i >> 1), 0.5f, 0xffffffffu,
                    {0, 0, lit_x[q], 0.75f, 0.25f, 0.75f, 0, 0}};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x442u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = 2;
            draw.texture.height = 1;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = texels;
            draw.texture_byte_count = sizeof texels;
            draw.program_count = 5;
            // oPos = v0; oD0 = v3; r0 = lit(v10); r2 = sge(v11, c3); oT0 = r0.zwxx * r2.yyyy.
            draw.program[0][1] = (1u << 21) | xyzw;
            draw.program[0][2] = 2u << 26;
            draw.program[0][3] = (15u << 12) | 2048u;
            draw.program[1][1] = (1u << 21) | (3u << 9) | xyzw;
            draw.program[1][2] = 2u << 26;
            draw.program[1][3] = (15u << 12) | (3u << 3) | 2048u;
            draw.program[2][1] = (7u << 25) | (10u << 9);
            draw.program[2][2] = xyzw << 2;
            draw.program[2][3] = (2u << 28) | (15u << 16);
            draw.program[3][1] = (12u << 21) | (11u << 9) | (3u << 13) | xyzw;
            draw.program[3][2] = (2u << 26) | (3u << 11) | (xyzw << 17);
            draw.program[3][3] = (2u << 20) | (15u << 24);
            draw.program[4][1] = (2u << 21) | 0xb0u;
            draw.program[4][2] = (1u << 26) | (1u << 11) | (2u << 13) | (0x55u << 17);
            draw.program[4][3] = (15u << 12) | (9u << 3) | 2048u | 1u;
            const float c3[4] = {0.5f, 0.5f, 0.5f, 0.5f};
            std::memcpy(draw.program_constants[3], c3, sizeof c3);
            draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
            passed &= expect("lit program draw", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("lit present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy lit", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            const unsigned xs[] = {80, 240};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = bgr[0] < 4 && (i == 0 ? bgr[1] > 250 && bgr[2] < 4
                    : bgr[2] > 250 && bgr[1] < 4);
                if (!pixel_ok) std::fprintf(stderr, "FAIL program lit/sge pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS vertex-program LIT/SGE over a four-UV stream");
    return passed;
}

// Stage-effect bump pass: t2 is read at oT2 + M2 n with n the signed V8U8 map
// texel, masked by 1 - t3.a^4 and blended toward the fog color by oFog.
static bool testStageBump()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "bmp", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create bump", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("bump clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; float uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        const uint32_t scene[2] = {0xffff0000u, 0xff00ff00u}, mask = 0x80ffffffu;
        // U = +64 and V = -64: u moves from 0.25 to 0.75 (green) only if U is signed and first.
        const uint8_t map[2] = {0x40u, 0xc0u};
        const uint32_t xyzw = 0x1bu;
        for (unsigned q = 0; q < 2 && passed; ++q) {
            Vertex quad[4]{};
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {-1.0f + q + (i & 1), 1.0f - 2.0f * (i >> 1), 0.5f, 0xffffffffu,
                    {0, 0, 0, 0, 0.25f, 0.5f, 0.5f, 0.5f}};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x442u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.address_u = draw.address_v = 3;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = 2;
            draw.texture.height = 1;
            draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = scene;
            draw.texture_byte_count = sizeof scene;
            draw.reflection_texture = draw.texture;
            draw.reflection_texture.width = 1;
            draw.reflection_texture.data = 0x2000u;
            draw.reflection_bytes = &mask;
            draw.reflection_byte_count = sizeof mask;
            draw.reflection_address_u = draw.reflection_address_v = 3;
            draw.alpha_mask.format_byte = 0x28u;
            draw.alpha_mask.bits_per_pixel = 16;
            draw.alpha_mask.width = draw.alpha_mask.height = draw.alpha_mask.mip_levels = 1;
            draw.alpha_mask.data = 0x3000u;
            draw.alpha_mask_bytes = map;
            draw.alpha_mask_byte_count = sizeof map;
            draw.bump_address_u = draw.bump_address_v = 1;
            draw.bump_env = true;
            draw.bump_matrices[0][0] = 1.0f;
            // Right quad: f = 0 + 1 * oFog = 0.25 toward blue fog.
            draw.bump_fog[1] = 1.0f;
            draw.bump_fog[2] = q ? 1.0f : 0.0f;
            draw.fog_color = 0xff0000ffu;
            // oPos = v0, oD0 = v3, oT0 = v9, oT2 = v11, oT3 = v12, oFog = c5.
            const uint32_t moves[6][2] = {{0u, 0u}, {3u, 3u}, {9u, 9u}, {11u, 11u}, {12u, 12u}, {5u, 5u}};
            draw.program_count = 6;
            for (unsigned i = 0; i < 6; ++i) {
                const bool constant = i == 5;
                draw.program[i][1] = (1u << 21) | (constant ? moves[i][0] << 13 : moves[i][0] << 9) | xyzw;
                draw.program[i][2] = (constant ? 3u : 2u) << 26;
                draw.program[i][3] = (15u << 12) | (moves[i][1] << 3) | 2048u | (constant ? 1u : 0u);
            }
            draw.program_constants[5][0] = 0.25f;
            draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
            passed &= expect("bump draw", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("bump present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy bump", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // 1 - (128/255)^4 keeps 239 of 255; fog 0.25 leaves 60 green and 191 blue.
            const unsigned xs[] = {80, 240}, blue[] = {0, 191}, green[] = {239, 60};
            for (unsigned i = 0; i < 2; ++i) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + (239 - 120) * 960 + xs[i] * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[0] - int(blue[i])) <= 3 &&
                    std::abs(bgr[1] - int(green[i])) <= 3 && bgr[2] < 4;
                if (!pixel_ok) std::fprintf(stderr, "FAIL stage bump pixel %u BGR=%u,%u,%u\n",
                    i, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS stage-effect bump: signed V8U8 offsets, t3.a^4 mask and linear fog");
    return passed;
}

// Water bump pass: t1 at oT1 + M1 n, s = dot(expand oD0, expand t3)^8, then
// r0 = max(0, s + oD1 t1.a - 1) + t3.a t1. With dark set, a flat environment
// texel turns the highlight off, so the first sum is negative and must clamp
// to 0 instead of darkening the reflection (the safari stage's distant water).
static bool testStageWater(bool dark)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "wtr", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create water", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("water clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z; uint32_t color; float uv[8]; };
        const uint16_t indices[] = {0, 1, 2, 3};
        // Opaque red at u = 0.25; half-alpha green where the +64 U offset lands.
        const uint32_t reflection[2] = {0xffff0000u, 0x8000ff00u};
        const uint32_t environment = dark ? 0x80808080u : 0x808080ffu;
        const uint8_t map[2] = {0x40u, 0x00u};
        const uint32_t xyzw = 0x1bu;
        Vertex quad[4]{};
        for (unsigned i = 0; i < 4; ++i)
            quad[i] = {-1.0f + 2.0f * (i & 1), 1.0f - 2.0f * (i >> 1), 0.5f, 0xff8080ffu,
                {0, 0, 0.25f, 0.5f, 0, 0, 0.5f, 0.5f}};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x442u;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.address_u = draw.address_v = 3;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 2;
        draw.texture.height = 1;
        draw.texture.mip_levels = 1;
        draw.texture.data = 0x1000u;
        draw.texture_bytes = reflection;
        draw.texture_byte_count = sizeof reflection;
        draw.reflection_texture = draw.texture;
        draw.reflection_texture.width = 1;
        draw.reflection_texture.data = 0x2000u;
        draw.reflection_bytes = &environment;
        draw.reflection_byte_count = sizeof environment;
        draw.reflection_address_u = draw.reflection_address_v = 3;
        draw.alpha_mask.format_byte = 0x28u;
        draw.alpha_mask.bits_per_pixel = 16;
        draw.alpha_mask.width = draw.alpha_mask.height = draw.alpha_mask.mip_levels = 1;
        draw.alpha_mask.data = 0x3000u;
        draw.alpha_mask_bytes = map;
        draw.alpha_mask_byte_count = sizeof map;
        draw.bump_address_u = draw.bump_address_v = 1;
        draw.bump_env = draw.bump_water = true;
        draw.bump_matrices[0][0] = draw.bump_matrices[0][3] = 1.0f;
        // oPos = v0, oD0 = v3, oT0 = v9, oT1 = v10, oT3 = v12, oD1 = c6.
        const uint32_t moves[6][2] = {{0u, 0u}, {3u, 3u}, {9u, 9u}, {10u, 10u}, {12u, 12u}, {6u, 4u}};
        draw.program_count = 6;
        for (unsigned i = 0; i < 6; ++i) {
            const bool constant = i == 5;
            draw.program[i][1] = (1u << 21) | (constant ? moves[i][0] << 13 : moves[i][0] << 9) | xyzw;
            draw.program[i][2] = (constant ? 3u : 2u) << 26;
            draw.program[i][3] = (15u << 12) | (moves[i][1] << 3) | 2048u | (constant ? 1u : 0u);
        }
        draw.program_constants[6][0] = 0.5f;
        draw.program_constants[58][0] = draw.program_constants[58][1] = draw.program_constants[58][2] = 1;
        passed &= expect("water draw", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("water present", d3d_backend_submit(presenter, &present));
        passed &= expect("destroy water", d3d_backend_destroy(&presenter));
        if (FILE *file = std::fopen(path, "rb")) {
            // s = 1, t1 = green at 0.502 alpha, t3.a = 0.502: r0 = (0.25, 0.5, 0).
            // Dark: s = 0, so the first sum (-0.75, -1, -1) clamps to 0: r0 = (0, 0.5, 0).
            unsigned char bgr[3]{};
            std::fseek(file, 54 + (239 - 120) * 960 + 160 * 3, SEEK_SET);
            passed &= std::fread(bgr, 1, 3, file) == 3;
            const int red = dark ? 0 : 64;
            const bool pixel_ok = bgr[0] < 4 && std::abs(bgr[1] - 128) <= 4 && std::abs(bgr[2] - red) <= 4;
            if (!pixel_ok) std::fprintf(stderr, "FAIL stage water pixel BGR=%u,%u,%u\n", bgr[0], bgr[1], bgr[2]);
            passed &= pixel_ok;
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed)
        std::puts(dark ? "PASS stage water bump: unlit sum clamps before the reflection"
                       : "PASS stage water bump: offset reflection, oD1 and dot^8 highlight");
    return passed;
}

static bool testClose(bool idle)
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create close", recomp_d3d_presenter_create(&config, &presenter))) return false;
#ifndef RECOMP_TEST_D3D11
    /* SDL delivers a close to the presenter's own thread at its next present. */
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    bool passed = SDL_PushEvent(&quit);
    RecompD3dPresenterError error = RECOMP_D3D_PRESENTER_OK;
    uint32_t ticks = 0;
    while (passed && error == RECOMP_D3D_PRESENTER_OK && ticks < 600) {
        error = tick(presenter, ++ticks);
    }
    passed &= expect("close status", error, RECOMP_D3D_PRESENTER_CLOSED);
    passed &= expect("sticky close", recomp_d3d_presenter_submit(presenter, nullptr),
        RECOMP_D3D_PRESENTER_CLOSED);
    passed &= destroy(presenter);
    if (passed) std::printf("PASS %s close ticks=%u\n", idle ? "idle" : "queued", ticks);
    return passed;
#else
    const HWND window = presenterWindow();
    bool passed = window != nullptr;
    DWORD owner = GetWindowThreadProcessId(window, nullptr);
    passed &= owner != 0 && owner != GetCurrentThreadId();
    if (passed) passed = PostMessageW(window, WM_CLOSE, 0, 0) != 0;
    if (passed && idle) {
        /* WM_CLOSE hides the window; releasePresenter destroys it later. */
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (IsWindowVisible(window) && GetTickCount64() < deadline) Sleep(1);
        passed = !IsWindowVisible(window);
    }
    RecompD3dPresenterError error = RECOMP_D3D_PRESENTER_OK;
    uint32_t ticks = 0;
    while (passed && error == RECOMP_D3D_PRESENTER_OK && ticks < 600) {
        error = tick(presenter, ++ticks);
    }
    passed &= expect("close status", error, RECOMP_D3D_PRESENTER_CLOSED);
    passed &= expect("sticky close", recomp_d3d_presenter_submit(presenter, nullptr),
        RECOMP_D3D_PRESENTER_CLOSED);
    passed &= destroy(presenter);
    if (passed) std::printf("PASS %s close ticks=%u\n", idle ? "idle" : "queued", ticks);
    else std::fprintf(stderr, "FAIL close idle=%d window=%p owner=%lu\n", idle,
        static_cast<void *>(window), owner);
    return passed;
#endif
}

// A far quad behind a near quad covering the left half counts only the right half.
static bool testVisibility()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create visibility", recomp_d3d_presenter_create(&config, &presenter))) return false;
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; };
    const uint16_t indices[] = {0, 1, 2, 3};
    // The occluder's right edge avoids pixel centers with or without a half-pixel shift.
    const Vertex near_quad[] = {{-10, -10, 0.25f, 1, ~0u}, {159.9f, -10, 0.25f, 1, ~0u},
        {-10, 250, 0.25f, 1, ~0u}, {159.9f, 250, 0.25f, 1, ~0u}};
    const Vertex far_quad[] = {{-10, -10, 0.5f, 1, ~0u}, {330, -10, 0.5f, 1, ~0u},
        {-10, 250, 0.5f, 1, ~0u}, {330, 250, 0.5f, 1, ~0u}};
    RecompD3dPresenterCommand draw_command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    auto &draw = draw_command.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x44u;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.depth.depth_test_enable = true;
    draw.depth.depth_write_enable = true;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw.vertex_bytes = near_quad;
    RecompD3dPresenterCommand begin{RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN};
    RecompD3dPresenterCommand end{RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END};
    auto clear = clearCommand();
    bool passed = expect("visibility clear", recomp_d3d_presenter_submit(presenter, &clear));
    passed &= expect("occluder", recomp_d3d_presenter_submit(presenter, &draw_command));
    draw.vertex_bytes = far_quad;
    draw.depth.depth_write_enable = false;
    for (uint32_t index : {7u, 8u}) {
        draw.depth.depth_test_enable = index == 7u;
        end.data.visibility_index = index;
        passed &= expect("visibility begin", recomp_d3d_presenter_submit(presenter, &begin));
        passed &= expect("counted draw", recomp_d3d_presenter_submit(presenter, &draw_command));
        passed &= expect("visibility end", recomp_d3d_presenter_submit(presenter, &end));
    }
    bool complete = true;
    uint32_t pixels[2]{};
    passed &= expect("queued result", recomp_d3d_presenter_visibility_result(presenter, 7u, &complete, &pixels[0]));
#ifdef RECOMP_TEST_WORKER
    // The render thread publishes nothing before the flush.
    passed &= !complete;
#endif
    passed &= expect("unknown index", recomp_d3d_presenter_visibility_result(presenter, 9u, &complete, &pixels[0]),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= expect("publish visibility", recomp_d3d_presenter_flush(presenter, false));
#ifdef RECOMP_TEST_WORKER
    passed &= expect("wait for visibility", recomp_d3d_presenter_flush(presenter, true));
    complete = false;
    passed &= expect("index 7 resolves before the wait returns",
        recomp_d3d_presenter_visibility_result(presenter, 7u, &complete, &pixels[0]));
    passed &= complete;
    complete = false;
    passed &= expect("index 8 resolves before the wait returns",
        recomp_d3d_presenter_visibility_result(presenter, 8u, &complete, &pixels[1]));
    passed &= complete;
#endif
    const ULONGLONG deadline = GetTickCount64() + 5000u;
    for (uint32_t i = 0; i < 2u && passed; ++i) {
        complete = false;
        while (passed && !complete && GetTickCount64() < deadline) {
            passed &= expect("poll visibility", recomp_d3d_presenter_visibility_result(
                presenter, 7u + i, &complete, &pixels[i]));
        }
        passed &= complete;
    }
    passed &= pixels[0] == 160u * 240u && pixels[1] == 320u * 240u;
    /* Five more tests on the same target. On SDL_GPU the first two reuse the
       counting targets kept above, with the coverage swapped so a stale count
       shows; the fifth to finish makes a kept set too many. */
    for (uint32_t index = 7u; index < 12u; ++index) {
        draw.depth.depth_test_enable = index % 2u == 0u;
        end.data.visibility_index = index;
        passed &= expect("reuse begin", recomp_d3d_presenter_submit(presenter, &begin));
        passed &= expect("reuse draw", recomp_d3d_presenter_submit(presenter, &draw_command));
        passed &= expect("reuse end", recomp_d3d_presenter_submit(presenter, &end));
    }
    passed &= expect("publish reused visibility", recomp_d3d_presenter_flush(presenter, true));
    const ULONGLONG reuse_deadline = GetTickCount64() + 5000u;
    for (uint32_t index = 7u; index < 12u && passed; ++index) {
        uint32_t counted = 0u;
        complete = false;
        while (passed && !complete && GetTickCount64() < reuse_deadline) {
            passed &= expect("poll reused visibility", recomp_d3d_presenter_visibility_result(
                presenter, index, &complete, &counted));
        }
        if (!complete || counted != (index % 2u == 0u ? 160u : 320u) * 240u) {
            std::fprintf(stderr, "FAIL reused visibility index=%u pixels=%u\n", index, counted);
            passed = false;
        }
    }
#ifdef RECOMP_TEST_WORKER
    // An END the worker cannot match to a BEGIN fails the presenter.
    passed &= expect("unbegun end", recomp_d3d_presenter_submit(presenter, &end));
    passed &= expect("unbegun end fails", recomp_d3d_presenter_flush(presenter, true),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
#else
    // The inline presenter rejects an END without a BEGIN when it is submitted.
    passed &= expect("unbegun end", recomp_d3d_presenter_submit(presenter, &end),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
#endif
    recomp_d3d_presenter_destroy(&presenter);
    if (passed) std::puts("PASS occlusion counts partially hidden and unoccluded quads");
    else std::fprintf(stderr, "FAIL visibility complete=%d pixels=%u,%u\n", complete, pixels[0], pixels[1]);
    return passed;
}

static bool testVolume(bool camera_position = false)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "vol", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create volume", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        passed &= expect("volume clear", d3d_backend_submit(presenter, &clear));
        // Independent 2x1x2 Morton fixture: red/blue, then green/yellow.
        const uint32_t texels[] = {0xffff0000u, 0xff0000ffu, 0xff00ff00u, 0xffffff00u};
        const uint16_t indices[] = {0, 1, 2, 3};
        const float r[] = {0.25f, 0.75f, 0.5f, 1.25f, 1.25f, 0.25f};
        struct Vertex { float x, y, z; uint32_t color; float u, v; };
        for (unsigned pass = 0; pass < 6 && passed; ++pass) {
            Vertex quad[4];
            for (unsigned i = 0; i < 4; ++i)
                quad[i] = {-1.0f + (pass * 50.0f + (i & 1) * 50.0f) / 160.0f,
                    i < 2 ? 1.0f : -1.0f, 0.5f, 0xffffffffu, pass == 5 ? 0.75f : 0.25f, 0.5f};
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
            draw.index_count = draw.vertex_count = 4;
            draw.triangle_count = 2;
            draw.vertex_stride = sizeof(Vertex);
            draw.fvf = 0x142u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.blend.color_write_mask = 15;
            draw.has_transform = true;
            draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
            draw.address_u = draw.address_v = 3;
            draw.address_w = pass == 4 ? 3 : 1;
            draw.has_texture = true;
            draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
            draw.texture.bits_per_pixel = 32;
            draw.texture.width = draw.texture.volume_depth = 2;
            draw.texture.height = draw.texture.mip_levels = 1;
            draw.texture.data = 0x1000u;
            draw.texture_bytes = texels;
            draw.texture_byte_count = sizeof texels;
            draw.texture_transform_uv = true;
            draw.reflection_transform[0] = draw.reflection_transform[5] = 1;
            draw.reflection_transform[10] = r[pass];
            if (camera_position) {
                // Mesh UVs select the opposite texel; translated eye Z selects the slice.
                for (auto &vertex : quad) vertex.u = pass == 5 ? 0.25f : 0.75f;
                draw.texture_transform_uv = false;
                draw.texgen_projected = true;
                draw.reflection_world_view[0] = draw.reflection_world_view[5] =
                    draw.reflection_world_view[10] = draw.reflection_world_view[15] = 1;
                draw.reflection_world_view[14] = r[pass] - 0.5f;
                draw.reflection_transform[0] = draw.reflection_transform[5] = 0;
                draw.reflection_transform[10] = 1;
                draw.reflection_transform[12] = pass == 5 ? 0.75f : 0.25f;
                draw.reflection_transform[13] = 0.5f;
            }
            passed &= expect("volume sample", d3d_backend_submit(presenter, &command));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("volume present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    if (passed) {
        FILE *file = std::fopen(path, "rb");
        if (file) {
            const int rgb[][3] = {{255,0,0}, {0,255,0}, {128,128,0}, {255,0,0}, {0,255,0}, {0,0,255}};
            for (unsigned pass = 0; pass < 6; ++pass) {
                unsigned char bgr[3]{};
                std::fseek(file, 54 + 119 * 960 + (25 + pass * 50) * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const bool pixel_ok = std::abs(bgr[2] - rgb[pass][0]) <= 3 &&
                    std::abs(bgr[1] - rgb[pass][1]) <= 3 && std::abs(bgr[0] - rgb[pass][2]) <= 3;
                if (!pixel_ok) std::fprintf(stderr, "FAIL volume pixel %u BGR=%u,%u,%u\n", pass, bgr[0], bgr[1], bgr[2]);
                passed &= pixel_ok;
            }
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts(camera_position
        ? "PASS camera volume S/T/R, translated eye Z, slices, interpolation and W addressing"
        : "PASS volume slices, trilinear blend, W wrap/clamp, and BGRA channels");
    return passed;
}

// PC Settings' quality applies after a present: the main target is rebuilt at
// the new scale and keeps the picture drawn before it. The request is global,
// so this runs last and leaves 1x without MSAA behind.
static bool testLiveQuality()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "qty", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", "3");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create live quality", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xffff0000u;
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5,1};
        passed &= expect("red frame", d3d_backend_submit(presenter, &clear));
        passed &= expect("first present", d3d_backend_submit(presenter, &present));
        d3d_backend_set_quality(2.0f, 1u, false, 0.0f, false);
        passed &= expect("present then rebuild", d3d_backend_submit(presenter, &present));
        passed &= expect("carried present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    d3d_backend_set_quality(1.0f, 1u, false, 0.0f, false);
    if (passed) {
        FILE *file = std::fopen(path, "rb");
        int32_t size[2]{};
        unsigned char bgr[3]{};
        if (file) {
            std::fseek(file, 18, SEEK_SET);
            passed &= std::fread(size, sizeof size[0], 2, file) == 2;
            std::fseek(file, 54 + 240 * 640 * 3 + 320 * 3, SEEK_SET);
            passed &= std::fread(bgr, 1, 3, file) == 3;
            std::fclose(file);
        } else passed = false;
        passed &= size[0] == 640 && size[1] == 480 && bgr[2] > 250 && bgr[1] < 4 && bgr[0] < 4;
        if (!passed) std::fprintf(stderr, "FAIL live quality %dx%d BGR=%u,%u,%u\n",
            size[0], size[1], bgr[0], bgr[1], bgr[2]);
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS live quality rebuilds the main target at 2x and keeps its picture");
    return passed;
}

static bool testFrameBufferBlend()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "frm", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", "2");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create frame blend", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xffff0000u;
        passed &= expect("red first frame", d3d_backend_submit(presenter, &clear));
        const uint16_t indices[] = {0,1,2,3};
        const float quad[4][7] = {{0,0,0,1,0,0,0}, {320,0,0,1,0,320,0},
            {0,240,0,1,0,0,240}, {320,240,0,1,0,320,240}};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.fvf = 0x144u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof quad[0];
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.has_texture = draw.texture_is_backbuffer = draw.texture_is_front_buffer = true;
        draw.texture.format_byte = 0x12u;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 320; draw.texture.height = 240; draw.texture.pitch = 1280;
        draw.texture.linear = true;
        draw.address_u = draw.address_v = 3;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[3] = 1;
        passed &= expect("prime previous copy", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5,1};
        passed &= expect("first frame present", d3d_backend_submit(presenter, &present));
        clear.data.clear.color = 0xff0000ffu;
        passed &= expect("blue current frame", d3d_backend_submit(presenter, &clear));
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_CONSTANT_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA;
        draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
        draw.blend.constant_color = 0x80ff0000u;
        passed &= expect("previous frame constant alpha", d3d_backend_submit(presenter, &command));
        passed &= expect("blended frame present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    if (passed) {
        FILE *file = std::fopen(path, "rb");
        unsigned char bgr[3]{};
        if (file) {
            std::fseek(file, 54 + 119 * 960 + 160 * 3, SEEK_SET);
            passed &= std::fread(bgr, 1, 3, file) == 3;
            std::fclose(file);
        } else passed = false;
        passed &= std::abs(bgr[2] - 128) <= 3 && bgr[1] < 4 && std::abs(bgr[0] - 127) <= 3;
        if (!passed) std::fprintf(stderr, "FAIL frame blend BGR=%u,%u,%u\n", bgr[0], bgr[1], bgr[2]);
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS previous host frame blends over the scene with constant alpha");
    return passed;
}

static bool testDepthBias(bool scaled = false)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "bias", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_SCALE", scaled ? "2" : "1");
    RecompD3dPresenter *presenter = nullptr;
    auto bias_config = config;
    if (scaled) bias_config.width = 360; // Scaled 4:3 output has unequal X/Y ratios.
    bool passed = expect("create depth bias", d3d_backend_create(&bias_config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff808080u;
        passed &= expect("bias clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z, rhw; uint32_t color; };
        const uint16_t indices[] = {0, 1, 2, 3};
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x44u;
        draw.index_bytes = indices;
        draw.depth.depth_test_enable = draw.depth.depth_write_enable = true;
        draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
        draw.blend.color_write_mask = 15;
        const int bias[] = {0, -1, 0, 0, 0, 1, 0, 0};
        const float slope[] = {0, 0, 0, -0.25f, 0.25f, 0, -0.25f, 0.25f};
        for (unsigned i = 0; i < 8; ++i) {
            const float x = 8.0f + i * 40.0f;
            const bool tilted = i == 3 || i == 4, tilted_x = i == 6 || i == 7;
            Vertex vertices[] = {
                {x, 20, tilted || tilted_x ? 0.4f : 0.5f, 1, 0xffffffffu},
                {x + 32, 20, tilted_x ? 0.6f : tilted ? 0.4f : 0.5f, 1, 0xffffffffu},
                {x, 220, tilted_x ? 0.4f : tilted ? 0.6f : 0.5f, 1, 0xffffffffu},
                {x + 32, 220, tilted || tilted_x ? 0.6f : 0.5f, 1, 0xffffffffu}};
            draw.vertex_bytes = vertices;
            draw.depth_bias = 0;
            draw.slope_scaled_depth_bias = 0;
            passed &= expect("unbiased floor", d3d_backend_submit(presenter, &command));
            for (auto &vertex : vertices) {
                vertex.color = 0xff000000u;
                if (tilted) vertex.z += 0.0002f;
                if (tilted_x) vertex.z += 0.0013f;
            }
            draw.depth_bias = bias[i];
            draw.slope_scaled_depth_bias = slope[i];
            draw.depth.depth_write_enable = false;
            draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS;
            passed &= expect("coplanar receiver", d3d_backend_submit(presenter, &command));
            draw.depth.depth_write_enable = true;
            draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("bias present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    if (passed) {
        FILE *file = std::fopen(path, "rb");
        if (!file) passed = false;
        else {
            int32_t width = 0, height = 0;
            std::fseek(file, 18, SEEK_SET);
            passed &= std::fread(&width, sizeof width, 1, file) == 1;
            passed &= std::fread(&height, sizeof height, 1, file) == 1;
            if (width <= 0 || height <= 0) passed = false;
            for (unsigned i = 0; i < 8 && width > 0 && height > 0; ++i) {
                unsigned char bgr[3]{};
                const int x = (24 + i * 40) * width / bias_config.width, y = height / 2;
                std::fseek(file, 54 + (height - 1 - y) * ((width * 3 + 3) & ~3) + x * 3, SEEK_SET);
                passed &= std::fread(bgr, 1, 3, file) == 3;
                const unsigned want = i == 1 || i == 3 || i == 6 ? 0 : 255;
                for (unsigned c = 0; c < 3; ++c) {
                    if (bgr[c] != want) {
                        std::fprintf(stderr, "FAIL depth bias pixel %u channel=%u actual=%u expected=%u\n", i, c, bgr[c], want);
                        passed = false;
                    }
                }
            }
            std::fclose(file);
        }
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS D24 receiver unit/slope bias, sign and restoration");
    return passed;
}

static bool testCard()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "card", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_SCALE", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create card", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("card clear", d3d_backend_submit(presenter, &clear));
        std::vector<uint32_t> pixels(16 * 9, 0xff4080c0u);
        RecompD3dCardVertex vertices[4];
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        // A 16:9 card on a 4:3 display fills 3/4 of the height: rows 30..209.
        recomp_d3d_card_draw(pixels.data(), 16, 9, 320.0f, 240.0f, 4.0f / 3.0f, 128,
            vertices, &command.data.draw);
        passed &= expect("card draw", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("card present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    FILE *file = passed ? std::fopen(path, "rb") : nullptr;
    passed &= file != nullptr;
    const struct { int y; unsigned b, g, r; } probes[] = {{10, 0, 0, 0}, {120, 96, 64, 32}, {229, 0, 0, 0}};
    for (const auto &probe : probes) {
        unsigned char bgr[3]{};
        if (file == nullptr) break;
        std::fseek(file, 54 + (239 - probe.y) * 960 + 160 * 3, SEEK_SET);
        passed &= std::fread(bgr, 1, 3, file) == 3;
        if (std::abs(bgr[0] - int(probe.b)) > 3 || std::abs(bgr[1] - int(probe.g)) > 3 ||
            std::abs(bgr[2] - int(probe.r)) > 3) {
            std::fprintf(stderr, "FAIL card row %d BGR=%u,%u,%u\n", probe.y, bgr[0], bgr[1], bgr[2]);
            passed = false;
        }
    }
    if (file != nullptr) std::fclose(file);
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS dedication card letterboxes and fades a linear BGRA image");
    return passed;
}

static bool testScaledMsaaDepthBias()
{
#ifndef RECOMP_TEST_D3D11
    /* Lavapipe (Linux CI) evaluates the position at each sample when a shader
       runs per sample, so this exact-edge test moves; Metal and RADV pass it. */
    if (std::getenv("RECOMP_TEST_SOFTWARE_VULKAN") != nullptr) {
        std::puts("SKIP scaled 4x MSAA bias on a software Vulkan rasterizer");
        return true;
    }
#endif
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "msb", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_SCALE", "2");
    _putenv_s("RECOMP_D3D_MSAA", "4");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "0");
    auto bias_config = config;
    bias_config.width = 360; // Host 640x480: unequal X/Y scale, integer panel spacing.
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create scaled MSAA bias", d3d_backend_create(&bias_config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xffffffffu;
        passed &= expect("MSAA bias clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z, rhw; uint32_t color; };
        const uint16_t indices[] = {0, 1, 2, 3};
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &draw = command.data.draw;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof(Vertex);
        draw.fvf = 0x44u;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.depth.depth_test_enable = true;
        // Each pair compares an unbiased plane with an equivalent biased plane.
        // Native depth interpolation is the oracle, including partial sample visibility.
        for (unsigned pair = 0; pair < 2; ++pair) {
            const float slope = pair ? -1.0f / 256 : 1.0f / 256;
            const float factor = pair ? 0.25f : -0.25f;
            const float top = pair ? 130.0f : 20.0f, bottom = top + 80;
            for (unsigned panel = 0; panel < 2; ++panel) {
                const float left = 8.0f + panel * 90;
                Vertex vertices[] = {{left, top, 0.5f, 1, 0xffffffffu},
                    {left + 72, top, 0.5f, 1, 0xffffffffu},
                    {left, bottom, 0.5f, 1, 0xffffffffu},
                    {left + 72, bottom, 0.5f, 1, 0xffffffffu}};
                draw.vertex_bytes = vertices;
                draw.depth.depth_write_enable = true;
                draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
                draw.depth.alpha_test_enable = false;
                draw.depth_bias = 0;
                draw.slope_scaled_depth_bias = 0;
                passed &= expect("MSAA flat floor", d3d_backend_submit(presenter, &command));
                for (auto &vertex : vertices) {
                    vertex.z += slope * (vertex.y - (top + 40.25f));
                    if (panel) vertex.z -= factor * std::abs(slope);
                    vertex.color = vertex.x == left ? 0x00000000u : 0xff000000u;
                }
                draw.depth_bias = pair ? 4096 : -4096;
                draw.slope_scaled_depth_bias = panel ? factor : 0;
                draw.depth.depth_write_enable = false;
                draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS;
                draw.depth.alpha_test_enable = true;
                draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
                draw.depth.alpha_ref = 128;
                draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_VERTEX_DIFFUSE;
                passed &= expect("MSAA intersecting receiver", d3d_backend_submit(presenter, &command));
            }
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5, 1};
        passed &= expect("MSAA bias present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    if (passed) {
        FILE *file = std::fopen(path, "rb");
        if (!file) passed = false;
        else {
            int32_t width = 0, height = 0;
            std::fseek(file, 18, SEEK_SET);
            passed &= std::fread(&width, sizeof width, 1, file) == 1;
            passed &= std::fread(&height, sizeof height, 1, file) == 1;
            passed &= width == 640 && height == 480;
            unsigned mismatches = 0, partial = 0, black = 0, white = 0;
            if (passed) {
                for (int y = 50; y < 410; ++y) {
                    for (int x = 20; x < 136; ++x) {
                        unsigned char reference[3]{}, biased[3]{};
                        const long row = 54 + (height - 1 - y) * ((width * 3 + 3) & ~3);
                        std::fseek(file, row + x * 3, SEEK_SET);
                        passed &= std::fread(reference, 1, 3, file) == 3;
                        std::fseek(file, row + (x + 160) * 3, SEEK_SET);
                        passed &= std::fread(biased, 1, 3, file) == 3;
                        for (unsigned c = 0; c < 3; ++c)
                            mismatches += std::abs(int(reference[c]) - int(biased[c])) > 1;
                        partial += reference[0] > 0 && reference[0] < 255;
                        black += reference[0] == 0;
                        white += reference[0] == 255;
                    }
                }
            }
            if (mismatches || !partial || !black || !white) {
                std::fprintf(stderr, "FAIL scaled 4x MSAA bias mismatches=%u partial=%u black=%u white=%u\n",
                    mismatches, partial, black, white);
                passed = false;
            }
            std::fclose(file);
        }
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    _putenv_s("RECOMP_D3D_MSAA", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS scaled 4x MSAA receiver intersection, constant bias and pixel-center alpha discard");
    return passed;
}

static bool testDrawCoverage(bool scaled_msaa = false)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "cov", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_SCALE", scaled_msaa ? "2" : "1");
    _putenv_s("RECOMP_D3D_MSAA", scaled_msaa ? "4" : "1");
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create coverage", d3d_backend_create(&config, &presenter))) return false;
    struct Vertex { float x, y, z, nx, ny, nz, u, v; };
    Vertex vertices[] = {{-1,-1,0.5f}, {1,-1,0.5f}, {0,1,0.5f}};
    for (auto &vertex : vertices) vertex.nx = vertex.u = NAN;
    const uint16_t indices[] = {0,1,2};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x112u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLELIST;
    draw.vertex_bytes = vertices;
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_count = draw.index_count = 3u;
    draw.triangle_count = 1u;
    draw.index_bytes = indices;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    // Neither obstructing tests nor incomplete/invalid shading inputs belong to coverage.
    draw.has_reflection = draw.has_texture = draw.texgen_reflection = true;
    draw.depth.depth_test_enable = draw.depth.depth_write_enable = true;
    draw.depth.depth_func = draw.depth.alpha_func = draw.depth.stencil_func = RECOMP_D3D_COMPARE_NEVER;
    draw.depth.alpha_test_enable = draw.depth.stencil_enable = true;
    draw.depth.stencil_write_mask = 255u;
    draw.depth.stencil_ref = 99u;
    draw.depth.stencil_fail = draw.depth.stencil_zfail = draw.depth.stencil_pass = RECOMP_D3D_STENCIL_REPLACE;
    draw.blend.color_write_mask = 15u;
    draw.blend.blend_enable = true;
    auto clear = clearCommand();
    clear.data.clear.z = 0.25f;
    clear.data.clear.clear_stencil = true;
    clear.data.clear.stencil = 7u;
    bool passed = expect("coverage clear", d3d_backend_submit(presenter, &clear));
    const auto coverage = [&](const char *label, bool expected) {
        bool covered = !expected;
        const auto error = d3d_backend_draw_coverage(presenter, &draw, &covered);
        const bool ok = expect(label, error) && covered == expected;
        if (!ok) std::fprintf(stderr, "FAIL %s covered=%d expected=%d\n", label, covered, expected);
        passed &= ok;
    };
    coverage("visible coverage", true);
    draw.transform[0] = draw.transform[10] = 0;
    coverage("collapsed coverage", false);
    draw.transform[0] = draw.transform[10] = 1;
    {
        // A full converted quad batch has 98,304 indices; only the tail has area.
        std::vector<uint16_t> many_indices(98304u, 0u);
        many_indices[many_indices.size() - 2u] = 1u;
        many_indices.back() = 2u;
        draw.index_bytes = many_indices.data();
        draw.index_count = static_cast<uint32_t>(many_indices.size());
        draw.triangle_count = draw.index_count / 3u;
        coverage("large index batch tail coverage", true);
        draw.transform[0] = draw.transform[10] = 0;
        coverage("large collapsed index batch coverage", false);
        draw.transform[0] = draw.transform[10] = 1;
        draw.index_bytes = indices;
        draw.index_count = 3u;
        draw.triangle_count = 1u;
    }
    for (uint32_t primitive : {5u,6u,7u}) {
        draw.primitive_type = primitive;
        coverage("polygon topology coverage", true);
    }
    draw.primitive_type = 5u;
    draw.has_scissor = true;
    std::memset(draw.scissor, 0, sizeof draw.scissor);
    coverage("empty scissor coverage", false);
    draw.scissor[0] = 80;
    draw.scissor[1] = 180;
    draw.scissor[2] = 81;
    draw.scissor[3] = 181;
    coverage("one pixel scissor coverage", true);
    draw.viewport[0] = draw.viewport[1] = 20;
    draw.viewport[2] = draw.viewport[3] = 100;
    draw.viewport[5] = 1;
    coverage("viewport outside scissor", false);
    std::memset(draw.viewport, 0, sizeof draw.viewport);
    draw.has_scissor = false;
    draw.transform[14] = -2;
    coverage("near clipped coverage", false);
    draw.transform[14] = 0;
    draw.screen_space_offset[0] = 400;
    coverage("screen offset outside target", false);
    draw.screen_space_offset[0] = 0;
    draw.cull_mode = RECOMP_D3D_CULL_CLOCKWISE;
    bool clockwise = false, counterclockwise = false;
    passed &= expect("clockwise coverage", d3d_backend_draw_coverage(presenter, &draw, &clockwise));
    draw.cull_mode = RECOMP_D3D_CULL_COUNTER_CLOCKWISE;
    passed &= expect("counterclockwise coverage", d3d_backend_draw_coverage(presenter, &draw, &counterclockwise));
    if (clockwise == counterclockwise) { std::fprintf(stderr, "FAIL coverage ignored culling\n"); passed = false; }
    draw.cull_mode = RECOMP_D3D_CULL_NONE;
    {
        // The first standard 4x sample is (3/8,1/8); the pixel center misses this triangle.
        const float width = scaled_msaa ? 640.0f : 320.0f, height = scaled_msaa ? 480.0f : 240.0f;
        const Vertex sample[] = {
            {2 * 20.25f / width - 1, 1 - 2 * 20.0f / height, 0.5f},
            {2 * 20.5f / width - 1, 1 - 2 * 20.0f / height, 0.5f},
            {2 * 20.375f / width - 1, 1 - 2 * 20.25f / height, 0.5f}};
        draw.vertex_bytes = sample;
        coverage("sample coverage without pixel-center coverage", scaled_msaa);
        draw.vertex_bytes = vertices;
    }
    if (!scaled_msaa) {
        // A rank-one affine map can gain area when screen coordinates snap to 1/256 pixels.
        // (125,127.1), (130,128.6), (135,130.1) snap around the sample (128,128).
        const Vertex line[] = {{0,0,0}, {0,1,0}, {0,2,0}};
        draw.vertex_bytes = line;
        std::memset(draw.transform, 0, sizeof draw.transform);
        draw.transform[4] = 1.0f / 8192.0f;
        draw.transform[5] = -1.0f / 20480.0f;
        draw.transform[12] = -0.9969482421875f;
        draw.transform[13] = 1.0f - 2.0f * (127.1f / 256.0f) / 240.0f;
        draw.transform[14] = 0.5f;
        draw.transform[15] = 1;
        coverage("rank-one snapped sample coverage", true);
        draw.vertex_bytes = vertices;
        std::memset(draw.transform, 0, sizeof draw.transform);
        draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    }
    struct Blended { float x, y, z, weight, nx, ny, nz, u, v; };
    Blended blended[] = {{-1,-1,0.5f,0,NAN}, {1,-1,0.5f,0,NAN}, {0,1,0.5f,0,NAN}};
    draw.fvf = 0x116u;
    draw.vertex_stride = sizeof(Blended);
    draw.vertex_bytes = blended;
    draw.blend_weight_count = 1u;
    std::memcpy(draw.blend_transforms[0], draw.transform, sizeof draw.transform);
    draw.transform[0] = draw.transform[10] = 0;
    coverage("remaining blend influence coverage", true);
    for (auto &vertex : blended) vertex.weight = 1;
    coverage("collapsed weighted influence coverage", false);
    draw.fvf = 0x112u;
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_bytes = vertices;
    draw.blend_weight_count = 0u;
    draw.transform[0] = draw.transform[10] = 1;
    draw.target.offscreen = true;
    draw.target.no_depth = true;
    draw.target.color.data = 0x10000u;
    draw.target.color.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.target.color.width = draw.target.color.height = 64u;
    coverage("offscreen coverage", true);
    draw.transform[0] = draw.transform[10] = 0;
    coverage("offscreen collapsed coverage", false);
    draw.transform[0] = draw.transform[10] = 1;
    draw.target = {};
    bool covered = false;
    passed &= expect("uninitialized coverage", d3d_backend_draw_coverage(nullptr, &draw, &covered),
        RECOMP_D3D_PRESENTER_NOT_INITIALIZED);
    passed &= covered;
    passed &= expect("null coverage draw", d3d_backend_draw_coverage(presenter, nullptr, &covered),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= covered;
    passed &= expect("null coverage result", d3d_backend_draw_coverage(presenter, &draw, nullptr),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    draw.index_bytes = reinterpret_cast<const void *>(1u);
    draw.index_count = 64u * 1024u * 1024u / sizeof(uint16_t) + 1u;
    draw.triangle_count = draw.index_count / 3u;
    covered = false;
    passed &= expect("oversized coverage indices rejected before read",
        d3d_backend_draw_coverage(presenter, &draw, &covered), RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= covered;
    draw.index_bytes = indices;
    draw.index_count = 3u;
    draw.triangle_count = 1u;
    draw.vertex_bytes = reinterpret_cast<const void *>(1u);
    draw.vertex_stride = UINT32_MAX / draw.vertex_count;
    covered = false;
    passed &= expect("oversized coverage vertices rejected before read",
        d3d_backend_draw_coverage(presenter, &draw, &covered), RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= covered;
    draw.vertex_bytes = vertices;
    draw.vertex_stride = sizeof(Vertex);
    draw.program_count = 1u;
    passed &= expect("program coverage rejected", d3d_backend_draw_coverage(presenter, &draw, &covered),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    draw.program_count = 0u;
    draw.fvf = 0x144u;
    passed &= expect("pretransformed coverage rejected", d3d_backend_draw_coverage(presenter, &draw, &covered),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    draw.fvf = 0x112u;
    draw.primitive_type = 2u;
    passed &= expect("line coverage rejected", d3d_backend_draw_coverage(presenter, &draw, &covered),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    draw.primitive_type = 5u;
    std::thread other([&] {
        passed &= expect("coverage owner thread", d3d_backend_draw_coverage(presenter, &draw, &covered),
            RECOMP_D3D_PRESENTER_WRONG_THREAD);
    });
    other.join();
    vertices[0].x = NAN;
    passed &= expect("nonfinite coverage position", d3d_backend_draw_coverage(presenter, &draw, &covered),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= covered;
    vertices[0].x = -1;
    // This fails after Begin(query); a following probe must still succeed.
    draw.has_scissor = true;
    draw.scissor[0] = 2;
    draw.scissor[2] = 1;
    passed &= expect("invalid coverage scissor", d3d_backend_draw_coverage(presenter, &draw, &covered),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    passed &= covered;
    draw.has_scissor = false;
    RecompD3dPresenterCommand begin{RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN};
    RecompD3dPresenterCommand end{RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END};
    end.data.visibility_index = 17u;
    passed &= expect("guest visibility begin", d3d_backend_submit(presenter, &begin));
    passed &= expect("open guest visibility rejects probe", d3d_backend_draw_coverage(presenter, &draw, &covered),
        RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND);
    passed &= covered;
    passed &= expect("guest visibility end", d3d_backend_submit(presenter, &end));
    coverage("coverage after query error", true);
    uint32_t query_index = 0u, pixels = ~0u;
    const auto poll = d3d_backend_poll_visibility(presenter, &query_index, &pixels);
    if (poll != D3D_BACKEND_VISIBILITY_READY || query_index != 17u || pixels != 0u) {
        std::fprintf(stderr, "FAIL coverage changed guest visibility poll=%u pixels=%u\n", poll, pixels);
        passed = false;
    }
    // Paint only the right half, conditioned on the original depth AND stencil.
    struct Witness { float x, y, z; uint32_t color; };
    const Witness witness[] = {{0,1,0.25f,0xff00ff00u}, {1,1,0.25f,0xff00ff00u},
        {0,-1,0.25f,0xff00ff00u}, {1,-1,0.25f,0xff00ff00u}};
    const uint16_t quad[] = {0,1,2,3};
    RecompD3dPresenterCommand paint{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    auto &check = paint.data.draw;
    check.fvf = 0x42u;
    check.vertex_bytes = witness;
    check.vertex_stride = sizeof(Witness);
    check.vertex_count = check.index_count = 4u;
    check.index_bytes = quad;
    check.triangle_count = 2u;
    check.primitive_type = 6u;
    check.has_transform = true;
    check.transform[0] = check.transform[5] = check.transform[10] = check.transform[15] = 1;
    check.blend.color_write_mask = 15u;
    check.depth.depth_test_enable = check.depth.stencil_enable = true;
    check.depth.depth_func = check.depth.stencil_func = RECOMP_D3D_COMPARE_EQUAL;
    check.depth.stencil_ref = 7u;
    check.depth.stencil_read_mask = 255u;
    passed &= expect("depth stencil preservation witness", d3d_backend_submit(presenter, &paint));
    RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
    present.data.present = {5,1};
    passed &= expect("coverage present", d3d_backend_submit(presenter, &present));
    passed &= expect("destroy coverage", d3d_backend_destroy(&presenter));
    if (FILE *file = std::fopen(path, "rb")) {
        uint32_t width = 0u, height = 0u;
        std::fseek(file, 18, SEEK_SET);
        passed &= std::fread(&width, sizeof width, 1, file) == 1 &&
            std::fread(&height, sizeof height, 1, file) == 1;
        passed &= width == (scaled_msaa ? 640u : 320u) && height == (scaled_msaa ? 480u : 240u);
        std::fseek(file, 54, SEEK_SET);
        unsigned mismatches = 0;
        for (unsigned pixel = 0; pixel < width * height; ++pixel) {
            unsigned char bgr[3]{};
            const bool right = pixel % width >= width / 2u;
            if (std::fread(bgr, 1, 3, file) != 3 || bgr[0] != (right ? 0 : 96) ||
                bgr[1] != (right ? 255 : 64) || bgr[2] != (right ? 0 : 32)) ++mismatches;
        }
        if (mismatches) { std::fprintf(stderr, "FAIL coverage target writes pixels=%u\n", mismatches); passed = false; }
        std::fclose(file);
    } else passed = false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    _putenv_s("RECOMP_D3D_MSAA", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS native geometry coverage, target preservation and guest query isolation");
    return passed;
}

static bool testWidescreenAntialiasing(bool smaa, bool msaa, unsigned height = 480u, unsigned samples = 4u)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "aa", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_WIDESCREEN", "1");
    _putenv_s("RECOMP_D3D_SCALE", std::to_string(height / 240.0f).c_str());
    _putenv_s("RECOMP_D3D_MSAA", msaa ? std::to_string(samples).c_str() : "1");
    _putenv_s("RECOMP_D3D_SMAA", smaa ? "1" : "0");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create widescreen AA", d3d_backend_create(&config, &presenter));
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff000000u;
        passed &= expect("AA clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z, rhw; uint32_t color; };
        const Vertex occluder[] = {{0,0,0.25f,1,0}, {160,0,0.25f,1,0},
            {0,240,0.25f,1,0}, {160,240,0.25f,1,0}};
        const uint16_t indices[] = {0,1,2,3};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.fvf = 0x44u;
        draw.vertex_stride = sizeof(Vertex);
        draw.vertex_count = draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.vertex_bytes = occluder;
        draw.index_bytes = indices;
        draw.depth.depth_test_enable = draw.depth.depth_write_enable = true;
        draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS;
        passed &= expect("AA main occluder", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterTarget target{};
        target.offscreen = true;
        target.color.data = 0x10000u;
        target.color.format_byte = 0x12u;
        target.color.bits_per_pixel = 32u;
        target.color.width = 320u; target.color.height = 240u; target.color.pitch = 1280u;
        target.color.linear = true;
        clear.data.clear.target = target;
        clear.data.clear.clear_depth = false;
        passed &= expect("AA offscreen color clear keeps depth", d3d_backend_submit(presenter, &clear));
        const Vertex triangle[] = {{20,20,0.5f,1,0xffffffffu}, {300,20,0.5f,1,0xffffffffu},
            {300,220,0.5f,1,0xffffffffu}};
        draw.vertex_bytes = triangle;
        draw.vertex_count = draw.index_count = 3u;
        draw.triangle_count = 1u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLELIST;
        draw.depth.depth_write_enable = false;
        draw.blend.color_write_mask = 15u;
        draw.target = target;
        passed &= expect("AA offscreen triangle shares main depth", d3d_backend_submit(presenter, &command));
        const float quad[4][7] = {{0,0,0,1,0,0,0}, {320,0,0,1,0,320,0},
            {0,240,0,1,0,0,240}, {320,240,0,1,0,320,240}};
        draw = {};
        draw.fvf = 0x144u;
        draw.vertex_stride = sizeof quad[0];
        draw.vertex_count = draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15u;
        draw.has_texture = true;
        draw.texture = target.color;
        draw.address_u = draw.address_v = 3u;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[3] = 1u;
        passed &= expect("AA resolved offscreen composite", d3d_backend_submit(presenter, &command));
        if (smaa && msaa) {
            RecompD3dPresenterCommand gamma{RECOMP_D3D_PRESENTER_COMMAND_GAMMA};
            for (auto &channel : gamma.data.gamma)
                for (unsigned i = 0u; i < 256u; ++i) channel[i] = static_cast<uint8_t>((i * i + 127u) / 255u);
            passed &= expect("AA with gamma", d3d_backend_submit(presenter, &gamma));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5u,1u};
        passed &= expect("AA present", d3d_backend_submit(presenter, &present));
#ifdef RECOMP_TEST_D3D11
        RECT rect{};
        // A scaled window covers the screen; the picture is fitted into it.
        passed &= GetClientRect(presenterWindow(), &rect) &&
            rect.right == GetSystemMetrics(SM_CXSCREEN) && rect.bottom == GetSystemMetrics(SM_CYSCREEN);
#endif
        passed &= expect("AA target release", d3d_backend_release_memory(presenter, 0x10000u, 4096u, true));
    }
    d3d_backend_destroy(&presenter);
    unsigned partial = 0u, changed = 0u;
    static std::vector<unsigned char> baseline;
    if (FILE *file = std::fopen(path, "rb")) {
        uint32_t width = 0u, dumped_height = 0u;
        std::fseek(file, 18, SEEK_SET);
        passed &= std::fread(&width, 4, 1, file) == 1 && std::fread(&dumped_height, 4, 1, file) == 1;
        passed &= width == (height * 16u + 8u) / 9u && dumped_height == height;
        const unsigned stride = (width * 3u + 3u) & ~3u;
        std::vector<unsigned char> row(stride);
        if (!smaa && !msaa) baseline.resize(static_cast<size_t>(stride) * height);
        std::fseek(file, 54, SEEK_SET);
        for (unsigned y = 0u; y < height; ++y) {
            passed &= std::fread(row.data(), stride, 1, file) == 1;
            for (unsigned x = 0u; x < width; ++x) {
                const unsigned color = row[x * 3u];
                partial += color > 0u && color < 255u;
                const size_t offset = static_cast<size_t>(y) * stride + x * 3u;
                if (!smaa && !msaa) baseline[offset] = static_cast<unsigned char>(color);
                else if (offset < baseline.size()) changed += std::abs(static_cast<int>(color) - baseline[offset]) > 2;
                if (x == width / 4u && y == height * 3u / 4u) passed &= color == 0u;
                if (x == width * 3u / 4u && y == height * 3u / 4u) passed &= color == 255u;
            }
        }
        std::fclose(file);
    } else passed = false;
    if (smaa || msaa) passed &= partial > 100u && changed > 100u;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    _putenv_s("RECOMP_D3D_MSAA", "");
    _putenv_s("RECOMP_D3D_SMAA", "");
    DeleteFileA(path);
    std::fprintf(passed ? stdout : stderr, "%s widescreen height=%u smaa=%d requested_msaa=%u partial=%u changed=%u and shared depth\n",
        passed ? "PASS" : "FAIL", height, smaa, msaa ? samples : 1u, partial, changed);
    return passed;
}

/* CAS in the gamma pass gives a gray step a darker and a lighter rim and
   leaves flat gray alone. With ramp, an inverting gamma ramp applies after
   it; with live, a quality request turns it off by the third frame. */
static bool testSharpen(bool ramp, bool live)
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "cas", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", live ? "3" : "1");
    _putenv_s("RECOMP_D3D_SHARPEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create sharpen", d3d_backend_create(&config, &presenter));
    if (passed && ramp) {
        RecompD3dPresenterCommand gamma{RECOMP_D3D_PRESENTER_COMMAND_GAMMA};
        for (auto &channel : gamma.data.gamma)
            for (unsigned i = 0u; i < 256u; ++i) channel[i] = static_cast<uint8_t>(255u - i);
        passed &= expect("sharpen gamma", d3d_backend_submit(presenter, &gamma));
    }
    for (unsigned frame = 0u; passed && frame < (live ? 3u : 1u); ++frame) {
        if (live && frame == 1u) d3d_backend_set_quality(1.0f, 1u, false, 0.0f, false);
        auto clear = clearCommand();
        clear.data.clear.color = 0xff404040u;
        passed &= expect("sharpen clear", d3d_backend_submit(presenter, &clear));
        struct Vertex { float x, y, z, rhw; uint32_t color; };
        const Vertex quad[] = {{160,0,0.5f,1,0xffc0c0c0u}, {320,0,0.5f,1,0xffc0c0c0u},
            {160,240,0.5f,1,0xffc0c0c0u}, {320,240,0.5f,1,0xffc0c0c0u}};
        const uint16_t indices[] = {0,1,2,3};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.fvf = 0x44u;
        draw.vertex_stride = sizeof(Vertex);
        draw.vertex_count = draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15u;
        passed &= expect("sharpen step", d3d_backend_submit(presenter, &command));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5u,1u};
        passed &= expect("sharpen present", d3d_backend_submit(presenter, &present));
    }
    d3d_backend_destroy(&presenter);
    unsigned dark = 0u, dark_rim = 0u, light_rim = 0u, light = 0u;
    if (FILE *file = std::fopen(path, "rb")) {
        uint32_t width = 0u, height = 0u;
        std::fseek(file, 18, SEEK_SET);
        passed &= std::fread(&width, 4, 1, file) == 1 && std::fread(&height, 4, 1, file) == 1 &&
            width == 320u && height == 240u;
        std::vector<unsigned char> row((width * 3u + 3u) & ~3u);
        std::fseek(file, static_cast<long>(54u + row.size() * 120u), SEEK_SET);
        passed &= std::fread(row.data(), row.size(), 1, file) == 1;
        std::fclose(file);
        dark = row[80u * 3u];
        dark_rim = row[159u * 3u];
        light_rim = row[160u * 3u];
        light = row[240u * 3u];
    } else passed = false;
    const auto within1 = [](unsigned value, unsigned want) { return value + 1u >= want && value <= want + 1u; };
    // An inverting ramp swaps which side is dark; the rims still overshoot outwards.
    const unsigned low = ramp ? 191u : 64u, high = ramp ? 63u : 192u;
    passed &= within1(dark, low) && within1(light, high);
    if (live) passed &= within1(dark_rim, low) && within1(light_rim, high);
    else if (ramp) passed &= dark_rim > low + 8u && light_rim + 8u < high;
    else passed &= dark_rim + 8u < low && light_rim > high + 8u;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", "");
    _putenv_s("RECOMP_D3D_SHARPEN", "");
    DeleteFileA(path);
    std::fprintf(passed ? stdout : stderr, "%s sharpen ramp=%d live=%d step %u|%u -> %u|%u\n",
        passed ? "PASS" : "FAIL", ramp, live, dark, light, dark_rim, light_rim);
    return passed;
}

/* A frame dump read back as 320x240 BGR rows, bottom-up as BMP stores them. */
static bool readFrame(const char *path, std::vector<unsigned char> &pixels)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    uint32_t width = 0u, height = 0u;
    std::fseek(file, 18, SEEK_SET);
    bool ok = std::fread(&width, 4, 1, file) == 1 && std::fread(&height, 4, 1, file) == 1 &&
        width == 320u && height == 240u;
    pixels.resize(320u * 240u * 3u);
    ok = ok && std::fread(pixels.data(), pixels.size(), 1, file) == 1;
    std::fclose(file);
    return ok;
}

static unsigned framePixel(const std::vector<unsigned char> &pixels, unsigned x, unsigned y)
{
    return pixels[((239u - y) * 320u + x) * 3u + 1u];  // BGR, the scene is grey
}

/* Ambient occlusion darkens the crease where a wall meets the floor and leaves
   an open floor, the wall's flat middle and a later 2D draw alone. With live,
   a quality request turns it off by the third frame. */
static bool testAmbientOcclusion(bool live)
{
    char folder[MAX_PATH], on_path[MAX_PATH], off_path[MAX_PATH], msaa_path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "ao", 0, on_path) ||
        !GetTempFileNameA(folder, "ao", 0, off_path) ||
        !GetTempFileNameA(folder, "ao", 0, msaa_path)) return false;
    // A floor and a wall at a right angle, then a pretransformed white band.
    const auto drawScene = [](RecompD3dPresenter *presenter) {
        struct Vertex { float x, y, z; uint32_t color; };
        const Vertex floor[] = {{-4,-1,2,~0u}, {4,-1,2,~0u}, {4,-1,6,~0u}, {-4,-1,6,~0u}};
        const uint16_t floor_indices[] = {0,1,2, 0,2,3};
        const Vertex wall[] = {{-2,-1,3,~0u}, {2,-1,3,~0u}, {2,0.6f,3,~0u}, {-2,0.6f,3,~0u}};
        const uint16_t wall_indices[] = {0,1,2, 0,2,3};
        const auto triangle = [](const void *vertices, size_t stride, const uint16_t *indices) {
            RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
            auto &draw = command.data.draw;
            draw.fvf = 0x42u;
            draw.vertex_stride = static_cast<uint32_t>(stride);
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.index_count = 6u;
            draw.triangle_count = 2u;
            draw.vertex_count = 4u;
            draw.primitive_type = RECOMP_D3D_PT_TRIANGLELIST;
            draw.cull_mode = RECOMP_D3D_CULL_NONE;
            draw.blend.color_write_mask = 15u;
            draw.depth.depth_test_enable = draw.depth.depth_write_enable = true;
            draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
            draw.has_transform = true;
            // Near 0.5 and far 100 in the game's row-vector projection.
            float *m = draw.transform;
            m[0] = m[5] = 1.0f;
            m[10] = 100.0f / 99.5f;
            m[11] = 1.0f;
            m[14] = -0.5f * 100.0f / 99.5f;
            m[15] = 0.0f;
            std::memcpy(draw.projection, m, sizeof draw.transform);
            return command;
        };
        auto clear = clearCommand();
        clear.data.clear.color = 0xff808080u;
        bool ok = expect("ao clear", d3d_backend_submit(presenter, &clear));
        ok &= expect("ao floor", d3d_backend_submit(presenter, &triangle(floor, sizeof floor[0], floor_indices)));
        ok &= expect("ao wall", d3d_backend_submit(presenter, &triangle(wall, sizeof wall[0], wall_indices)));
        struct ScreenVertex { float x, y, z, rhw; uint32_t color; };
        const ScreenVertex band[] = {{20,30,0.5f,1,~0u}, {300,30,0.5f,1,~0u},
            {20,70,0.5f,1,~0u}, {300,70,0.5f,1,~0u}};
        const uint16_t band_indices[] = {0,1,2,3};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.fvf = 0x44u;
        draw.vertex_stride = sizeof(ScreenVertex);
        draw.vertex_bytes = band;
        draw.index_bytes = band_indices;
        draw.index_count = draw.vertex_count = 4u;
        draw.triangle_count = 2u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.blend.color_write_mask = 15u;
        ok &= expect("ao band", d3d_backend_submit(presenter, &command));
        return ok;
    };
    const auto run = [&](const char *path, const char *at, const char *ao, bool turnOff) {
        _putenv_s("RECOMP_D3D_AO", ao);
        _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
        _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", at);
        RecompD3dPresenter *presenter = nullptr;
        bool ok = expect("create ao", d3d_backend_create(&config, &presenter));
        const unsigned frames = turnOff ? 3u : 1u;
        for (unsigned frame = 0u; ok && frame < frames; ++frame) {
            if (turnOff && frame == 1u) d3d_backend_set_quality(1.0f, 1u, false, 0.0f, false);
            ok &= drawScene(presenter);
            RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
            present.data.present = {5, frame + 1u};
            ok &= expect("ao present", d3d_backend_submit(presenter, &present));
        }
        d3d_backend_destroy(&presenter);
        return ok;
    };
    bool passed = run(off_path, "1", "0", false);
    passed &= run(on_path, live ? "3" : "1", "1", live);
    /* With MSAA on, the crease darkens the same way: Direct3D 11 reads sample 0
       of its depth, SDL_GPU the resolved copy its draws wrote. */
    if (!live) {
        _putenv_s("RECOMP_D3D_MSAA", "4");
        passed &= run(msaa_path, "1", "1", false);
        _putenv_s("RECOMP_D3D_MSAA", "");
    }
    std::vector<unsigned char> off, on;
    passed &= readFrame(off_path, off) && readFrame(on_path, on);
    const auto at = [](const std::vector<unsigned char> &pixels, unsigned x, unsigned y) {
        return pixels.empty() ? 0u : framePixel(pixels, x, y);
    };
    const unsigned crease_off = at(off, 160, 157), crease_on = at(on, 160, 157);
    const unsigned flat_off = at(off, 160, 120), flat_on = at(on, 160, 120);
    const unsigned floor_off = at(off, 280, 175), floor_on = at(on, 280, 175);
    const unsigned band_off = at(off, 160, 50), band_on = at(on, 160, 50);
    unsigned crease_msaa = 0u, flat_msaa = 0u, floor_msaa = 0u;
    const auto close = [](unsigned a, unsigned b) { return a + 2u >= b && a <= b + 2u; };
    if (live) {
        // A quality request turns it off by the third frame: back to baseline.
        passed &= close(crease_on, crease_off) && close(flat_on, flat_off) && close(floor_on, floor_off);
        passed &= band_on == band_off;
    } else {
        passed &= crease_on + 8u < crease_off;  // the crease darkens
        passed &= close(flat_on, flat_off) && close(floor_on, floor_off);  // open surfaces stay
        passed &= band_on == band_off && band_off > 200u;  // the later 2D layer is untouched
        std::vector<unsigned char> msaa;
        passed &= readFrame(msaa_path, msaa);
        // Direct3D 11 reads depth sample 0, off the pixel centre, so the crease may move a pixel.
        crease_msaa = 255u, flat_msaa = at(msaa, 160, 120), floor_msaa = at(msaa, 280, 175);
        for (unsigned y = 153u; y <= 161u; ++y) if (at(msaa, 160, y) < crease_msaa) crease_msaa = at(msaa, 160, y);
        passed &= crease_msaa + 8u < crease_off && close(flat_msaa, flat_off) && close(floor_msaa, floor_off);
    }
    _putenv_s("RECOMP_D3D_AO", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP_AT", "");
    DeleteFileA(on_path);
    DeleteFileA(off_path);
    DeleteFileA(msaa_path);
    std::fprintf(passed ? stdout : stderr,
        "%s ambient occlusion live=%d crease %u->%u flat %u->%u floor %u->%u band %u->%u msaa %u/%u/%u\n",
        passed ? "PASS" : "FAIL", live, crease_off, crease_on, flat_off, flat_on, floor_off, floor_on,
        band_off, band_on, crease_msaa, flat_msaa, floor_msaa);
    return passed;
}

static bool testQueuedDrawCoverage()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create queued coverage", recomp_d3d_presenter_create(&config, &presenter))) return false;
    const float vertices[][8] = {{-1,-1,0.5f}, {1,-1,0.5f}, {0,1,0.5f}};
    const uint16_t indices[] = {0,1,2};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x112u;
    draw.primitive_type = 5u;
    draw.vertex_bytes = vertices;
    draw.vertex_stride = sizeof vertices[0];
    draw.index_bytes = indices;
    draw.index_count = draw.vertex_count = 3u;
    draw.triangle_count = 1u;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    auto clear = clearCommand();
    bool passed = expect("queued coverage clear", recomp_d3d_presenter_submit(presenter, &clear));
    bool covered = false;
    passed &= expect("queued visible coverage", recomp_d3d_presenter_draw_coverage(presenter, &draw, &covered));
    passed &= covered;
    draw.transform[0] = draw.transform[10] = 0;
    passed &= expect("queued collapsed coverage", recomp_d3d_presenter_draw_coverage(presenter, &draw, &covered));
    passed &= !covered;
    draw.transform[0] = draw.transform[10] = 1;
    passed &= expect("queued visible after collapsed", recomp_d3d_presenter_draw_coverage(presenter, &draw, &covered));
    passed &= covered;
    passed &= destroy(presenter);
    if (passed) std::puts("PASS queued coverage returns each native result after earlier work");
    else std::fprintf(stderr, "FAIL queued coverage result\n");
    return passed;
}

/* A 4:3 high-resolution target into a smaller window exercises the present mips. */
static bool testPresentDownscale()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "dn", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_WIDESCREEN", "0");
    _putenv_s("RECOMP_D3D_SCALE", "6");
    const RecompD3dPresenterConfig downscale_config = {
        720u, 480u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    // A 4:3 target at scale 6 is 3840x2880, whatever the window.
    const uint32_t target_height = downscale_config.height * 6u, target_width = target_height * 4u / 3u;
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create present downscale", d3d_backend_create(&downscale_config, &presenter));
    bool skipped = false;
    if (passed) {
        auto clear = clearCommand();
        clear.data.clear.color = 0xff336699u;
        passed &= expect("downscale clear", d3d_backend_submit(presenter, &clear));
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5u, 1u};
        passed &= expect("downscale present", d3d_backend_submit(presenter, &present));
#ifdef RECOMP_TEST_D3D11
        RECT rect{};
        // The fitted picture must be smaller than the target, or the mip path never runs.
        if (!GetClientRect(presenterWindow(), &rect)) {
            std::fprintf(stderr, "FAIL downscale window has no client rect\n");
            passed = false;
        } else if (static_cast<uint32_t>(rect.right) >= target_width &&
                static_cast<uint32_t>(rect.bottom) >= target_height) {
            std::printf("SKIP downscale window %ldx%ld is not smaller than %ux%u\n",
                rect.right, rect.bottom, target_width, target_height);
            skipped = true;
        }
#endif
    }
    d3d_backend_destroy(&presenter);
    if (FILE *file = std::fopen(path, "rb")) {
        uint32_t width = 0u, height = 0u;
        std::fseek(file, 18, SEEK_SET);
        passed &= std::fread(&width, 4, 1, file) == 1 && std::fread(&height, 4, 1, file) == 1;
        std::fclose(file);
        passed &= width == target_width && height == target_height;
    } else passed = false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    DeleteFileA(path);
    if (passed && !skipped) std::puts("PASS 4:3 high-resolution present downscales into a smaller window");
    return passed;
}
// A full-frame MSAA target sampled, redrawn and sampled again must show the redraw.
static bool testResampledMsaaTarget()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "rs", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_SCALE", "2");
    _putenv_s("RECOMP_D3D_MSAA", "4");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create resampled target", d3d_backend_create(&config, &presenter));
    if (passed) {
        RecompD3dPresenterTarget target{};
        target.offscreen = true;
        target.color.data = 0x10000u;
        target.color.format_byte = 0x12u;
        target.color.bits_per_pixel = 32u;
        target.color.width = 320u; target.color.height = 240u; target.color.pitch = 1280u;
        target.color.linear = true;
        const float quad[4][7] = {{0,0,0,1,0,0,0}, {320,0,0,1,0,320,0},
            {0,240,0,1,0,0,240}, {320,240,0,1,0,320,240}};
        const uint16_t indices[] = {0,1,2,3};
        RecompD3dPresenterCommand composite{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = composite.data.draw;
        draw.fvf = 0x144u;
        draw.vertex_stride = sizeof quad[0];
        draw.vertex_count = draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15u;
        draw.has_texture = true;
        draw.texture = target.color;
        draw.address_u = draw.address_v = 3u;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[3] = 1u;
        for (const uint32_t color : {0xffff0000u, 0xff00ff00u}) {
            auto clear = clearCommand();
            clear.data.clear.target = target;
            clear.data.clear.clear_depth = false;
            clear.data.clear.color = color;
            passed &= expect("resampled target clear", d3d_backend_submit(presenter, &clear));
            passed &= expect("resampled target composite", d3d_backend_submit(presenter, &composite));
        }
        RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        present.data.present = {5u, 1u};
        passed &= expect("resampled present", d3d_backend_submit(presenter, &present));
        passed &= expect("resampled release", d3d_backend_release_memory(presenter, 0x10000u, 4096u, true));
    }
    d3d_backend_destroy(&presenter);
    unsigned char bgr[3]{};
    if (FILE *file = std::fopen(path, "rb")) {
        std::fseek(file, 54 + 239 * 1920 + 320 * 3, SEEK_SET);
        passed &= std::fread(bgr, 1, 3, file) == 3;
        std::fclose(file);
    } else passed = false;
    passed &= bgr[0] < 8u && bgr[1] > 247u && bgr[2] < 8u;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_SCALE", "");
    _putenv_s("RECOMP_D3D_MSAA", "");
    DeleteFileA(path);
    std::fprintf(passed ? stdout : stderr, "%s resampled MSAA target shows its redraw BGR=%u,%u,%u\n",
        passed ? "PASS" : "FAIL", bgr[0], bgr[1], bgr[2]);
    return passed;
}

/* Four real frames that each blend the previous one (GetBackBuffer(-1)) over
   a new color, through a gamma ramp, with frame generation's slots presented
   between them.
   Returns false on an error; engaged reports frame generation's schedule, and
   k and probe its first generated slot after the last frame. */
static bool framegenFrames(bool framegen, const char *path, bool &engaged, unsigned &k, uint32_t (&probe)[3])
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", framegen ? "1" : "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_FRAME_DUMP_COUNT", "4");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen", d3d_backend_create(&config, &presenter));
    engaged = false;
    k = 0u;
    if (passed) {
        RecompD3dPresenterCommand gamma{RECOMP_D3D_PRESENTER_COMMAND_GAMMA};
        for (auto &channel : gamma.data.gamma)
            for (unsigned i = 0u; i < 256u; ++i) channel[i] = static_cast<uint8_t>((i * i + 127u) / 255u);
        passed &= expect("framegen gamma", d3d_backend_submit(presenter, &gamma));
        const uint16_t indices[] = {0,1,2,3};
        const float quad[4][7] = {{0,0,0,1,0,0,0}, {320,0,0,1,0,320,0},
            {0,240,0,1,0,0,240}, {320,240,0,1,0,320,240}};
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = command.data.draw;
        draw.fvf = 0x144u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4;
        draw.triangle_count = 2;
        draw.vertex_stride = sizeof quad[0];
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15;
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_CONSTANT_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA;
        draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
        draw.blend.constant_color = 0x80000000u;
        draw.has_texture = draw.texture_is_backbuffer = draw.texture_is_front_buffer = true;
        draw.texture.format_byte = 0x12u;
        draw.texture.bits_per_pixel = 32;
        draw.texture.width = 320; draw.texture.height = 240; draw.texture.pitch = 1280;
        draw.texture.linear = true;
        draw.address_u = draw.address_v = 3;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[3] = 1;
        const uint32_t colors[4] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu};
        for (uint32_t frame = 1u; frame <= 4u && passed; ++frame) {
            auto clear = clearCommand();
            clear.data.clear.color = colors[frame - 1u];
            passed &= expect("framegen clear", d3d_backend_submit(presenter, &clear));
            passed &= expect("framegen previous frame", d3d_backend_submit(presenter, &command));
            RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
            present.data.present = {5u, frame};
            passed &= expect("framegen present", d3d_backend_submit(presenter, &present));
            // Present due slots until the next frame, as the worker does while it waits.
            const auto next = std::chrono::steady_clock::now() + std::chrono::microseconds(16667);
            for (bool first = true; std::chrono::steady_clock::now() < next; first = false) {
                const uint64_t until = d3d_backend_idle(presenter);
                if (first && until != UINT64_MAX) engaged = true;
                std::this_thread::sleep_for(std::chrono::microseconds(until == UINT64_MAX ? 1000u
                    : static_cast<unsigned>((std::min)(until / 10u, uint64_t{1000u}))));
            }
        }
        if (passed && engaged) k = d3d_backend_framegen_probe(presenter, 0u, probe);
    }
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP_COUNT", "");
    return passed;
}

static std::vector<char> readFile(const std::string &path)
{
    std::vector<char> bytes;
    if (FILE *file = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        for (size_t got; (got = std::fread(buffer, 1, sizeof buffer, file)) != 0u;) bytes.insert(bytes.end(), buffer, buffer + got);
        std::fclose(file);
    }
    return bytes;
}

// Frame generation must leave the real frames and the previous frame the guest samples unchanged.
static bool testFramegenKeepsRealFrames()
{
    char folder[MAX_PATH], off[MAX_PATH], on[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "fg0", 0, off) ||
        !GetTempFileNameA(folder, "fg1", 0, on)) return false;
    bool engaged_off = false, engaged = false;
    unsigned k = 0u;
    uint32_t probe[3]{};
    bool passed = framegenFrames(false, off, engaged_off, k, probe) && framegenFrames(true, on, engaged, k, probe);
    passed &= !engaged_off;
    // Slot 0 crossfades real frames 3 and 4 at t = 1/k, before gamma.
    if (passed && engaged) {
        passed = k >= 2u;
        for (unsigned shift = 0u; shift < 32u && passed; shift += 8u) {
            const int a = static_cast<int>(probe[0] >> shift & 255u), b = static_cast<int>(probe[1] >> shift & 255u);
            const int got = static_cast<int>(probe[2] >> shift & 255u);
            const int want = static_cast<int>(std::lround(a + (b - a) / static_cast<double>(k)));
            passed = std::abs(got - want) <= 1 && (shift == 24u || a != b);
        }
        if (!passed) {
            std::fprintf(stderr, "FAIL framegen crossfade k=%u frame 3=%08x frame 4=%08x slot 0=%08x\n",
                k, probe[0], probe[1], probe[2]);
        }
    }
    for (unsigned i = 0u; i < 4u && passed; ++i) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, ".%03u.bmp", i);
        const std::vector<char> a = readFile(off + std::string(suffix)), b = readFile(on + std::string(suffix));
        if (a.empty() || a != b) {
            std::fprintf(stderr, "FAIL framegen real frame %u differs (%zu and %zu bytes)\n", i + 1u, a.size(), b.size());
            passed = false;
        }
    }
    for (unsigned i = 0u; i < 4u; ++i) {
        char suffix[16];
        std::snprintf(suffix, sizeof suffix, ".%03u.bmp", i);
        DeleteFileA((off + std::string(suffix)).c_str());
        DeleteFileA((on + std::string(suffix)).c_str());
    }
    DeleteFileA(off);
    DeleteFileA(on);
    if (passed) {
        std::printf(engaged ? "PASS frame generation keeps real frames and the previous frame and crossfades them\n"
            : "PASS frame generation off matches; SKIP its schedule, see the framegen unsupported line\n");
    }
    return passed;
}

/* A slot shown while a real frame's commands are open must not lose them. On a
   slot's submit SDL's Vulkan backend moves the textures of fragmented memory:
   targets released between kept ones leave holes, and more small targets
   than one 16 MiB page of SDL's holds mark the holes' page to move. Each
   frame then clears a kept target, lets the slots show, and draws the target. */
static bool testFramegenKeepsOpenCommands()
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen open commands", d3d_backend_create(&config, &presenter));
    bool engaged = false;
    uint32_t swap = 0u;
    RecompD3dPresenterTarget target{};
    target.offscreen = target.no_depth = true;
    target.color.format_byte = 0x12u;
    target.color.bits_per_pixel = 32u;
    target.color.linear = true;
    const auto clear = [&](uint32_t index, uint32_t size, uint32_t color) {
        target.color.data = 0x100000u + index * 0x10000u;
        target.color.width = target.color.height = size;
        target.color.pitch = size * 4u;
        auto command = clearCommand();
        command.data.clear.target = target;
        command.data.clear.clear_depth = false;
        command.data.clear.color = color;
        passed &= expect("framegen open commands clear", d3d_backend_submit(presenter, &command));
    };
    // Shows the due slots for one guest frame, as the worker does between a frame's records.
    const auto idle = [&] {
        const auto next = std::chrono::steady_clock::now() + std::chrono::microseconds(16667);
        while (std::chrono::steady_clock::now() < next) {
            const uint64_t until = d3d_backend_idle(presenter);
            engaged |= until != UINT64_MAX;
            std::this_thread::sleep_for(std::chrono::microseconds(until == UINT64_MAX ? 1000u
                : static_cast<unsigned>((std::min)(until / 10u, uint64_t{1000u}))));
        }
    };
    const auto present = [&] {
        RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
        command.data.present = {5u, ++swap};
        passed &= expect("framegen open commands present", d3d_backend_submit(presenter, &command));
    };
    const auto frames = [&](unsigned count) {
        for (unsigned i = 0u; i < count && passed; ++i) {
            auto command = clearCommand();
            passed &= expect("framegen open commands frame", d3d_backend_submit(presenter, &command));
            present();
            idle();
        }
    };
    unsigned lost = 0u;
    if (passed) {
        frames(3u);
        for (uint32_t i = 0u; i < 8u; ++i) clear(i, 96u, 0xff000000u);
        frames(1u);
        for (uint32_t i = 0u; i < 8u; i += 2u) {
            passed &= expect("framegen open commands release",
                d3d_backend_release_memory(presenter, 0x100000u + i * 0x10000u, 0x1000u, true));
        }
        frames(2u);
        for (uint32_t i = 8u; i < 18u; ++i) clear(i, 700u, 0xff000000u);
        const float quad[4][7] = {{0,0,0,1,0,0,0}, {320,0,0,1,0,96,0}, {0,240,0,1,0,0,96}, {320,240,0,1,0,96,96}};
        const uint16_t indices[] = {0,1,2,3};
        RecompD3dPresenterCommand composite{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
        auto &draw = composite.data.draw;
        draw.fvf = 0x144u;
        draw.vertex_stride = sizeof quad[0];
        draw.vertex_count = draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.blend.color_write_mask = 15u;
        draw.has_texture = true;
        draw.address_u = draw.address_v = 3u;
        draw.directional.stage_mode = true;
        draw.directional.stage[0] = draw.directional.stage[3] = 1u;
        for (uint32_t frame = 1u; frame <= 12u && passed && engaged; ++frame) {
            const uint32_t color = 0xff000000u | frame * 0x140c07u;
            auto command = clearCommand();
            passed &= expect("framegen open commands frame", d3d_backend_submit(presenter, &command));
            clear(1u, 96u, color);
            idle();
            draw.texture = target.color;
            passed &= expect("framegen open commands draw", d3d_backend_submit(presenter, &composite));
            present();
            uint32_t probe[3]{};
            if (passed && d3d_backend_framegen_probe(presenter, 0u, probe) == 0u) {
                std::fprintf(stderr, "FAIL framegen open commands probe at frame %u\n", frame);
                passed = false;
            }
            if (passed && ((probe[1] ^ color) & 0xffffffu) != 0u) {
                std::fprintf(stderr, "FAIL framegen frame %u shows %06x, its target was cleared to %06x\n",
                    frame, probe[1] & 0xffffffu, color & 0xffffffu);
                ++lost;
            }
        }
    }
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    passed = passed && lost == 0u;
    if (passed) {
        std::printf(engaged ? "PASS frame generation's slots keep a real frame's open commands\n"
            : "SKIP frame generation's open commands, see the framegen unsupported line\n");
    }
    return passed;
}

/* Frame generation's flow test pictures: three octaves of hashed noise (2, 8
   and 32 pixel cells), not periodic, defined for any coordinate. */
static uint32_t flowNoise(int x, int y, uint32_t seed)
{
    unsigned sum = 0u;
    for (int shift = 1; shift <= 5; shift += 2) {
        uint32_t h = static_cast<uint32_t>(x >> shift) * 73856093u ^ static_cast<uint32_t>(y >> shift) * 19349663u ^
            (seed + static_cast<uint32_t>(shift)) * 83492791u;
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
        sum += h & 255u;
    }
    return 0xff000000u | sum / 3u * 0x010101u;
}

struct FlowRect { int x0, y0, x1, y1; };
static bool flowInside(FlowRect a, FlowRect b) { return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1; }
static bool flowTouches(FlowRect a, FlowRect b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
static FlowRect flowGrow(FlowRect r, int by) { return {r.x0 - by, r.y0 - by, r.x1 + by, r.y1 + by}; }
static FlowRect flowMove(FlowRect r, int dx, int dy) { return {r.x0 + dx, r.y0 + dy, r.x1 + dx, r.y1 + dy}; }

// A uniform patch in frame A, so moved in B, and a patch of B replaced by new noise.
static const FlowRect kFlowUniform{224, 32, 288, 96}, kFlowChanged{32, 144, 96, 208};

static uint32_t flowFrameA(int x, int y)
{
    return flowInside({x, y, x + 1, y + 1}, kFlowUniform) ? 0xff808080u : flowNoise(x, y, 1u);
}

// B is A moved by (dx, dy), except kFlowChanged.
static uint32_t flowFrameB(int x, int y, int dx, int dy)
{
    return flowInside({x, y, x + 1, y + 1}, kFlowChanged) ? flowNoise(x, y, 7u) : flowFrameA(x - dx, y - dy);
}

/* Checks the fields of A then B moved by (dx, dy), 4 floats per 8x8 block:
   - blocks at least 16 pixels from the edges and the two patches (in either
     frame): forward motion within 0.5 pixel of (dx, dy), backward of
     (-dx, -dy), both with confidence >= 0.75;
   - blocks that lie 12 pixels inside the uniform patch in both frames, where
     any motion matches: confidence < 0.5 both ways;
   - blocks whose content is replaced (backward: the block lies in the changed
     patch; forward: it lands there): confidence < 0.5. */
static bool checkFlow(const std::vector<float> &fields, const unsigned size[2], int dx, int dy)
{
    const size_t stride = static_cast<size_t>(size[0]) * size[1] * 4u;
    const FlowRect image{0, 0, 320, 240};
    unsigned interior = 0u, uniform = 0u, changed = 0u, bad = 0u;
    float worst_error = 0.0f, lowest = 1.0f, highest_uniform = 0.0f, highest_changed = 0.0f;
    for (unsigned direction = 0u; direction < 2u; ++direction) {
        const int sx = direction == 0u ? dx : -dx, sy = direction == 0u ? dy : -dy;
        for (unsigned by = 0u; by < size[1]; ++by) {
            for (unsigned bx = 0u; bx < size[0]; ++bx) {
                const float *v = &fields[direction * stride + (static_cast<size_t>(by) * size[0] + bx) * 4u];
                const FlowRect block{static_cast<int>(bx) * 8, static_cast<int>(by) * 8,
                    static_cast<int>(bx) * 8 + 8, static_cast<int>(by) * 8 + 8};
                const FlowRect landed = flowMove(block, sx, sy);
                // In frame A's coordinates for the uniform patch, B's for the changed one.
                const FlowRect in_a = direction == 0u ? block : landed, in_b = direction == 0u ? landed : block;
                const FlowRect around = flowGrow(block, 16 + (std::max)(std::abs(dx), std::abs(dy)));
                if (flowInside(around, image) && !flowTouches(around, kFlowUniform) &&
                    !flowTouches(around, flowMove(kFlowUniform, dx, dy)) && !flowTouches(around, kFlowChanged) &&
                    !flowTouches(around, flowMove(kFlowChanged, -dx, -dy))) {
                    ++interior;
                    const float error = std::hypot(v[0] - sx, v[1] - sy);
                    worst_error = (std::max)(worst_error, error);
                    lowest = (std::min)(lowest, v[2]);
                    if (error > 0.5f || !(v[2] >= 0.75f)) {
                        if (++bad <= 4u) std::fprintf(stderr, "FAIL flow (%d,%d) %s block %u,%u = (%.2f,%.2f) confidence %.2f\n",
                            dx, dy, direction ? "backward" : "forward", bx, by, v[0], v[1], v[2]);
                    }
                }
                if (flowInside(flowGrow(in_a, 12), kFlowUniform) &&
                    flowInside(flowGrow(in_b, 12), flowMove(kFlowUniform, dx, dy))) {
                    ++uniform;
                    highest_uniform = (std::max)(highest_uniform, v[2]);
                    if (!(v[2] < 0.5f) && ++bad <= 8u) std::fprintf(stderr, "FAIL flow (%d,%d) %s uniform block %u,%u confidence %.2f\n",
                        dx, dy, direction ? "backward" : "forward", bx, by, v[2]);
                }
                if (flowInside(in_b, kFlowChanged)) {
                    ++changed;
                    highest_changed = (std::max)(highest_changed, v[2]);
                    if (!(v[2] < 0.5f) && ++bad <= 8u) std::fprintf(stderr, "FAIL flow (%d,%d) %s changed block %u,%u confidence %.2f\n",
                        dx, dy, direction ? "backward" : "forward", bx, by, v[2]);
                }
            }
        }
    }
    std::printf("flow (%d,%d): %u interior blocks, worst error %.3f px, lowest confidence %.2f; "
        "%u uniform blocks, highest confidence %.2f; %u changed blocks, highest confidence %.2f\n",
        dx, dy, interior, worst_error, lowest, uniform, highest_uniform, changed, highest_changed);
    return bad == 0u && interior >= 100u && uniform >= 4u && changed >= 16u;
}

// Draws picture (320x240 0xAARRGGBB) over the main target and presents it as a real frame.
/* Draws picture over the 320x240 frame, then overlays, and presents it; shift
   moves it by fractions of a texel, which needs linear filtering. */
static bool flowFrame(RecompD3dPresenter *presenter, const std::vector<uint32_t> &picture, uint32_t address, uint32_t swap,
    float shift_x = 0.0f, float shift_y = 0.0f, bool linear = false,
    const std::vector<RecompD3dPresenterCommand> &overlays = {})
{
    const uint16_t indices[] = {0,1,2,3};
    // Legacy pixel centers are integers: texel centers need the half texel.
    const float u = 0.5f - shift_x, v = 0.5f - shift_y;
    const float quad[4][7] = {{0,0,0,1,0,u,v}, {320,0,0,1,0,320+u,v},
        {0,240,0,1,0,u,240+v}, {320,240,0,1,0,320+u,240+v}};
    RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    auto &draw = command.data.draw;
    draw.fvf = 0x144u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof quad[0];
    draw.vertex_bytes = quad;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.has_texture = true;
    draw.texture.format_byte = 0x12u;
    draw.texture.bits_per_pixel = 32;
    draw.texture.width = 320; draw.texture.height = 240; draw.texture.pitch = 1280;
    draw.texture.linear = true;
    draw.texture.data = address;
    draw.texture_bytes = picture.data();
    draw.texture_byte_count = static_cast<uint32_t>(picture.size() * 4u);
    draw.point_min_filter = draw.point_mag_filter = !linear;
    draw.address_u = draw.address_v = 3;
    draw.directional.stage_mode = true;
    draw.directional.stage[0] = draw.directional.stage[3] = 1;
    RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
    present.data.present = {5u, swap};
    bool passed = expect("flow picture", d3d_backend_submit(presenter, &command));
    for (const RecompD3dPresenterCommand &overlay : overlays) passed &= expect("flow overlay", d3d_backend_submit(presenter, &overlay));
    return passed && expect("flow present", d3d_backend_submit(presenter, &present));
}

/* Block matching finds a known motion: B is A moved by 3,-2 (less than a
   block) and by 13,9 (beyond the finest level's search radius of 2, so only
   the pyramid finds it). Engages only where frame generation does. */
static bool testFramegenFlow()
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen flow", d3d_backend_create(&config, &presenter));
    bool engaged = false;
    std::vector<uint32_t> a(320u * 240u), b(a.size());
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) a[static_cast<size_t>(y) * 320u + x] = flowFrameA(x, y);
    const int shifts[2][2] = {{3, -2}, {13, 9}};
    uint32_t swap = 0u;
    for (unsigned i = 0u; i < 2u && passed; ++i) {
        const int dx = shifts[i][0], dy = shifts[i][1];
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x) b[static_cast<size_t>(y) * 320u + x] = flowFrameB(x, y, dx, dy);
        passed &= flowFrame(presenter, a, 0x100000u, ++swap) && flowFrame(presenter, b, 0x200000u + i * 0x100000u, ++swap);
        unsigned size[2]{};
        std::vector<float> fields(2u * 64u * 64u * 4u);
        uint32_t probe[3]{};
        if (passed && d3d_backend_framegen_flow(presenter, 0u, size, fields.data(), fields.size()) >= 0.0 &&
            d3d_backend_framegen_probe(presenter, 0u, probe) != 0u) {
            engaged = true;
            // The history must hold the pictures' colors exactly (alpha is the vertex color's).
            if (((probe[0] ^ a[120u * 320u + 160u]) | (probe[1] ^ b[120u * 320u + 160u])) & 0xffffffu) {
                std::fprintf(stderr, "FAIL flow history %08x %08x, drew %08x %08x\n",
                    probe[0], probe[1], a[120u * 320u + 160u], b[120u * 320u + 160u]);
                passed = false;
            }
            passed = passed && size[0] == 40u && size[1] == 30u && checkFlow(fields, size, dx, dy);
        }
    }
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    if (passed) {
        std::printf(engaged ? "PASS frame generation's flow finds known motion and doubts what it cannot match\n"
            : "SKIP frame generation's flow, see the framegen unsupported line\n");
    }
    return passed;
}

// Frame A's gray level at (x, y) in pixels, between pixel centers bilinear.
static double flowSampleA(double x, double y)
{
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    const auto at = [](int px, int py) { return static_cast<double>(flowFrameA(px, py) & 255u); };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
        (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

/* Generated frames follow known motion: at t = 1/4 and 1/2 between A and B =
   A moved by 3,-2 and by 13,9, pixels away from the edges and the two patches
   must show A moved by t of it, much closer than the crossfade does. Between
   A and unrelated noise (a cut), every pixel must be A, so the cut lands with
   the real frame. Engages only where frame generation does. */
static bool testFramegenWarp()
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen warp", d3d_backend_create(&config, &presenter));
    bool engaged = false;
    std::vector<uint32_t> a(320u * 240u), b(a.size()), out(a.size());
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) a[static_cast<size_t>(y) * 320u + x] = flowFrameA(x, y);
    const int shifts[3][2] = {{3, -2}, {13, 9}, {0, 0}};
    uint32_t swap = 0u;
    for (unsigned i = 0u; i < 3u && passed; ++i) {
        const int dx = shifts[i][0], dy = shifts[i][1];
        const bool cut = i == 2u;
        for (int y = 0; y < 240; ++y) {
            for (int x = 0; x < 320; ++x)
                b[static_cast<size_t>(y) * 320u + x] = cut ? flowNoise(x, y, 9u) : flowFrameB(x, y, dx, dy);
        }
        passed &= flowFrame(presenter, a, 0x100000u, ++swap) && flowFrame(presenter, b, 0x500000u + i * 0x100000u, ++swap);
        for (const double t : {0.25, 0.5}) {
            unsigned size[2]{};
            if (!passed || d3d_backend_framegen_generate(presenter, static_cast<float>(t), 1u, size, out.data(),
                    out.size()) < 0.0) break;
            engaged = true;
            passed = size[0] == 320u && size[1] == 240u;
            const FlowRect image{0, 0, 320, 240};
            double warp_sum = 0.0, cross_sum = 0.0, warp_max = 0.0;
            unsigned count = 0u, off = 0u;
            for (int y = 0; y < 240 && passed; ++y) {
                for (int x = 0; x < 320; ++x) {
                    const size_t at = static_cast<size_t>(y) * 320u + x;
                    const double got = out[at] & 255u, from = a[at] & 255u, to = b[at] & 255u;
                    const double crossfade = from + (to - from) * t;
                    if (cut) {
                        off += std::fabs(got - from) > 1.0;
                        continue;
                    }
                    const FlowRect around = flowGrow({x, y, x + 1, y + 1}, 16 + (std::max)(std::abs(dx), std::abs(dy)));
                    if (!flowInside(around, image) || flowTouches(around, kFlowUniform) ||
                        flowTouches(around, flowMove(kFlowUniform, dx, dy)) || flowTouches(around, kFlowChanged) ||
                        flowTouches(around, flowMove(kFlowChanged, -dx, -dy))) continue;
                    const double want = flowSampleA(x - t * dx, y - t * dy);
                    warp_sum += std::fabs(got - want);
                    warp_max = (std::max)(warp_max, std::fabs(got - want));
                    cross_sum += std::fabs(crossfade - want);
                    ++count;
                }
            }
            if (cut) {
                std::printf("warp cut t=%.2f: %u pixels differ from the earlier real frame by more than 1\n", t, off);
                passed = passed && off == 0u;
            } else {
                const double warp = warp_sum / count, crossfade = cross_sum / count;
                std::printf("warp (%d,%d) t=%.2f: %u pixels, mean error %.2f (max %.0f), crossfade %.2f\n",
                    dx, dy, t, count, warp, warp_max, crossfade);
                passed = passed && count >= 10000u && warp < 2.0 && warp * 8.0 < crossfade;
            }
            if (!passed) std::fprintf(stderr, "FAIL framegen warp (%d,%d) t=%.2f\n", dx, dy, t);
        }
    }
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    if (passed) {
        std::printf(engaged ? "PASS frame generation's warp follows known motion and holds the earlier frame at a cut\n"
            : "SKIP frame generation's warp, see the framegen unsupported line\n");
    }
    return passed;
}

// The larger green step to the right or below each pixel.
static std::vector<int> flowSteps(const std::vector<int> &image, int w, int h)
{
    std::vector<int> steps(image.size(), 0);
    for (int y = 0; y + 1 < h; ++y) {
        for (int x = 0; x + 1 < w; ++x) {
            const size_t at = static_cast<size_t>(y) * w + x;
            steps[at] = (std::max)(std::abs(image[at + 1] - image[at]), std::abs(image[at + w] - image[at]));
        }
    }
    return steps;
}

static std::vector<int> flowGreen(const std::vector<uint32_t> &pixels)
{
    std::vector<int> green(pixels.size());
    for (size_t i = 0u; i < pixels.size(); ++i) green[i] = static_cast<int>(pixels[i] >> 8 & 255u);
    return green;
}

// Pixels of a 2160x1620 picture with a step above the wanted one's within 3 pixels by more than 16, and the worst excess.
static unsigned flowCutEdges(const std::vector<int> &steps, std::vector<int> wanted, int &worst)
{
    const int w = 2160, h = 1620;
    std::vector<int> grown(wanted.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int most = 0;
                for (int d = -3; d <= 3; ++d) {
                    const int px = pass == 0 ? std::clamp(x + d, 0, w - 1) : x, py = pass == 1 ? std::clamp(y + d, 0, h - 1) : y;
                    most = (std::max)(most, wanted[static_cast<size_t>(py) * w + px]);
                }
                grown[static_cast<size_t>(y) * w + x] = most;
            }
        }
        wanted.swap(grown);
    }
    unsigned cut = 0u;
    worst = 0;
    for (int y = 16; y < h - 16; ++y) {
        for (int x = 16; x < w - 16; ++x) {
            const size_t at = static_cast<size_t>(y) * w + x;
            worst = (std::max)(worst, steps[at] - wanted[at]);
            cut += steps[at] > wanted[at] + 16;
        }
    }
    return cut;
}

/* Generated frames cut no edges that neither the true in-between picture nor
   the crossfade has. At flow scale 3 (main target 2160x1620) the frames are
   drawn with linear filtering, so their own edges are soft. In the first
   scene B is A moved by 3,-2 texels with kFlowChanged replaced, and the truth
   is A moved by half of that. In the second, A slides by 4,-2 left of column
   160 and by -2,4 right of it. Motion and trust taken in whole finest-level
   pixels, or a pick trusted against the motion around it, leave hard-edged
   patches. Engages only where frame generation does. */
static bool testFramegenWarpEdges()
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen warp edges", d3d_backend_create(&config, &presenter));
    d3d_backend_set_quality(6.75f, 1u, false, 0.0f, false);
    const int w = 2160, h = 1620;
    std::vector<uint32_t> a(320u * 240u), b[2], truth_picture[2], out, from, to, truth;
    for (std::vector<uint32_t> *picture : {&b[0], &b[1], &truth_picture[0], &truth_picture[1]}) picture->resize(a.size());
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            const size_t at = static_cast<size_t>(y) * 320u + x;
            a[at] = truth_picture[0][at] = flowFrameA(x, y);
            b[0][at] = flowFrameB(x, y, 3, -2);
            b[1][at] = x < 160 ? flowFrameA(x - 4, y + 2) : flowFrameA(x + 2, y - 4);
            truth_picture[1][at] = x < 160 ? flowFrameA(x - 2, y + 1) : flowFrameA(x + 1, y - 2);
        }
    }
    const float truth_shift[2][2] = {{1.5f, -1.0f}, {0.0f, 0.0f}};
    unsigned size[2]{};
    bool wrong_size = false;
    // False when frame generation does not engage, a failure once it has; a wrong size sets wrong_size.
    const auto generate = [&](float t, std::vector<uint32_t> &pixels) {
        pixels.assign(static_cast<size_t>(w) * h, 0u);
        if (d3d_backend_framegen_generate(presenter, t, 1u, size, pixels.data(), pixels.size()) < 0.0) return false;
        if (size[0] != static_cast<unsigned>(w) || size[1] != static_cast<unsigned>(h)) {
            std::fprintf(stderr, "FAIL framegen warp edges made %ux%u, not %dx%d\n", size[0], size[1], w, h);
            wrong_size = true;
        }
        return true;
    };
    RecompD3dPresenterCommand clear = clearCommand(), present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
    present.data.present = {5u, 1u};
    passed = passed && expect("edges clear", d3d_backend_submit(presenter, &clear)) &&
        expect("edges present", d3d_backend_submit(presenter, &present));
    bool engaged = false;
    uint32_t swap = 1u;
    for (unsigned scene = 0u; scene < 2u && passed; ++scene) {
        passed = flowFrame(presenter, a, 0x100000u, ++swap, 0.0f, 0.0f, true) &&
            flowFrame(presenter, b[scene], 0x200000u + scene * 0x100000u, ++swap, 0.0f, 0.0f, true);
        if (!passed) break;
        if (!generate(0.5f, out)) {
            if (engaged) {
                passed = false;
                std::fprintf(stderr, "FAIL framegen warp edges stopped generating in scene %u\n", scene);
            }
            break;
        }
        engaged = true;
        // generate(0) and generate(1) are the real frames themselves.
        passed = generate(0.0f, from) && generate(1.0f, to) &&
            flowFrame(presenter, truth_picture[scene], 0x400000u + scene * 0x100000u, ++swap,
                truth_shift[scene][0], truth_shift[scene][1], true) && generate(1.0f, truth) && !wrong_size;
        if (!passed) break;
        std::vector<int> crossfade = flowGreen(from);
        const std::vector<int> later = flowGreen(to);
        for (size_t i = 0u; i < crossfade.size(); ++i) crossfade[i] = (crossfade[i] + later[i] + 1) / 2;
        std::vector<int> wanted = flowSteps(crossfade, w, h);
        const std::vector<int> true_steps = flowSteps(flowGreen(truth), w, h);
        for (size_t i = 0u; i < wanted.size(); ++i) wanted[i] = (std::max)(wanted[i], true_steps[i]);
        int worst = 0;
        const unsigned cut = flowCutEdges(flowSteps(flowGreen(out), w, h), wanted, worst);
        std::printf("warp edges scene %u at 2160x1620: %u pixels cut an edge more than 16 above the truth and crossfade, worst %d\n",
            scene, cut, worst);
        passed = cut == 0u;
        if (!passed) std::fprintf(stderr, "FAIL framegen warp cuts edges in scene %u\n", scene);
    }
    d3d_backend_set_quality(1.0f, 1u, false, 0.0f, false);
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    if (passed) {
        std::printf(engaged ? "PASS frame generation's warp cuts no edges the truth and crossfade lack\n"
            : "SKIP frame generation's warp edges, see the framegen unsupported line\n");
    }
    return passed;
}

struct HudVertex {
    float x, y, z, rhw;
    uint32_t color;
};

// A pretransformed XYZRHW|DIFFUSE quad, blended over the scene by its alpha when blend.
static RecompD3dPresenterCommand hudQuad(const HudVertex *quad, bool blend)
{
    static const uint16_t indices[] = {0,1,2,3};
    RecompD3dPresenterCommand command{RECOMP_D3D_PRESENTER_COMMAND_DRAW};
    auto &draw = command.data.draw;
    draw.fvf = 0x44u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof quad[0];
    draw.vertex_bytes = quad;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.blend.blend_enable = blend;
    draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
    draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
    draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
    return command;
}

/* The HUD mask keeps the 2D layer still and out of the scene. The scene (A,
   then A moved by 13,9) is drawn full width, then a red 2D quad that does not
   move, then a full-width quarter-black fade over both. At t = 1/4 and 1/2:
   the red quad's pixels must be the nearer real frame (it does not move); no
   scene pixel may take any of its red; and away from it, the faded scene must
   still follow the motion (the fade spans the width, so it is a screen
   effect, not HUD). Engages only where frame generation does. */
static bool testFramegenHud()
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen hud", d3d_backend_create(&config, &presenter));
    const int dx = 13, dy = 9;
    const FlowRect hud{128, 96, 192, 144};
    std::vector<uint32_t> a(320u * 240u), b(a.size()), out, from, to;
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            a[static_cast<size_t>(y) * 320u + x] = flowFrameA(x, y);
            b[static_cast<size_t>(y) * 320u + x] = flowFrameA(x - dx, y - dy);
        }
    }
    const float x0 = static_cast<float>(hud.x0), y0 = static_cast<float>(hud.y0);
    const float x1 = static_cast<float>(hud.x1), y1 = static_cast<float>(hud.y1);
    const HudVertex red[4] = {{x0, y0, 0, 1, 0xffff0000u}, {x1, y0, 0, 1, 0xffff0000u},
        {x0, y1, 0, 1, 0xffff0000u}, {x1, y1, 0, 1, 0xffff0000u}};
    const HudVertex fade[4] = {{0, 0, 0, 1, 0x40000000u}, {320, 0, 0, 1, 0x40000000u},
        {0, 240, 0, 1, 0x40000000u}, {320, 240, 0, 1, 0x40000000u}};
    const std::vector<RecompD3dPresenterCommand> overlays = {hudQuad(red, false), hudQuad(fade, true)};
    unsigned size[2]{};
    const auto generate = [&](float t, std::vector<uint32_t> &pixels) {
        pixels.assign(a.size(), 0u);
        return d3d_backend_framegen_generate(presenter, t, 1u, size, pixels.data(), pixels.size()) >= 0.0 &&
            size[0] == 320u && size[1] == 240u;
    };
    // The first real frame fills both frames' history, so the mask's first pair is A, A.
    passed = passed && flowFrame(presenter, a, 0x100000u, 1u, 0.0f, 0.0f, false, overlays) &&
        flowFrame(presenter, a, 0x100000u, 2u, 0.0f, 0.0f, false, overlays) &&
        flowFrame(presenter, b, 0x200000u, 3u, 0.0f, 0.0f, false, overlays);
    bool engaged = false;
    if (passed && generate(0.0f, from) && generate(1.0f, to)) {
        engaged = true;
        const double faded = 1.0 - 64.0 / 255.0;
        for (const double t : {0.25, 0.5}) {
            const bool generated = generate(static_cast<float>(t), out);
            unsigned moved = 0u, red_scene = 0u, count = 0u;
            double warp_sum = 0.0, cross_sum = 0.0;
            for (int y = 0; y < 240 && generated; ++y) {
                for (int x = 0; x < 320; ++x) {
                    const size_t at = static_cast<size_t>(y) * 320u + x;
                    const int r = out[at] >> 16 & 255u, g = out[at] >> 8 & 255u, bl = out[at] & 255u;
                    if (flowInside({x, y, x + 1, y + 1}, hud)) {
                        for (unsigned shift : {0u, 8u, 16u}) {
                            const double want = ((t < 0.5 ? from[at] : to[at]) >> shift) & 255u;
                            if (std::fabs((out[at] >> shift & 255u) - want) > 1.0) {
                                ++moved;
                                break;
                            }
                        }
                        continue;
                    }
                    red_scene += std::abs(r - g) > 2 || std::abs(bl - g) > 2;
                    const FlowRect around = flowGrow({x, y, x + 1, y + 1}, 16 + dx);
                    if (!flowInside(around, {0, 0, 320, 240}) || flowTouches(around, hud) ||
                        flowTouches(around, kFlowUniform) || flowTouches(around, flowMove(kFlowUniform, dx, dy))) continue;
                    const double want = flowSampleA(x - t * dx, y - t * dy) * faded;
                    warp_sum += std::fabs(g - want);
                    cross_sum += std::fabs((from[at] >> 8 & 255u) * (1 - t) + (to[at] >> 8 & 255u) * t - want);
                    ++count;
                }
            }
            const double warp = count ? warp_sum / count : 1e9, crossfade = count ? cross_sum / count : 0.0;
            std::printf("hud t=%.2f: %u of %u HUD pixels off the nearer frame, %u scene pixels with its red; "
                "faded scene %u pixels, mean error %.2f, crossfade %.2f\n",
                t, moved, static_cast<unsigned>((hud.x1 - hud.x0) * (hud.y1 - hud.y0)), red_scene, count, warp, crossfade);
            passed = passed && generated && moved == 0u && red_scene == 0u && count >= 5000u && warp < 2.0 && warp * 8.0 < crossfade;
            if (!passed) std::fprintf(stderr, "FAIL framegen hud t=%.2f\n", t);
        }
    }
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    if (passed) {
        std::printf(engaged ? "PASS frame generation keeps the HUD still and out of the scene, and warps a fade\n"
            : "SKIP frame generation's HUD mask, see the framegen unsupported line\n");
    }
    return passed;
}

static bool fastForwardOn() { return true; }

/* Generated frames never mix real frames that do not belong together. A fade
   (A a quarter darker) and a white flash (A three quarters white) are not
   cuts: their generated frames are the crossfade, not A. After frame
   generation was off (fast-forward, or the frame clock of netplay and
   rollback), after a real frame came late (a load), and after a quality
   change, the next real frame C starts again: its generated frames are C,
   with nothing of A. Engages only where frame generation does. */
static bool testFramegenCuts()
{
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen cuts", d3d_backend_create(&config, &presenter));
    std::vector<uint32_t> a(320u * 240u), faded(a.size()), flash(a.size()), c(a.size()), out(a.size());
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            const size_t at = static_cast<size_t>(y) * 320u + x;
            a[at] = flowFrameA(x, y);
            const uint32_t gray = a[at] & 255u;
            faded[at] = 0xff000000u | gray * 3u / 4u * 0x010101u;
            flash[at] = 0xff000000u | (gray / 4u + 191u) * 0x010101u;
            c[at] = flowNoise(x, y, 11u);
        }
    }
    unsigned size[2]{};
    const auto generate = [&](float t) {
        return d3d_backend_framegen_generate(presenter, t, 1u, size, out.data(), out.size()) >= 0.0 &&
            size[0] == 320u && size[1] == 240u;
    };
    uint32_t swap = 0u;
    bool engaged = false;
    const std::vector<uint32_t> *changes[2] = {&faded, &flash};
    // A real frame over 25 ms late starts the history again; this one takes the first present's shader work.
    passed = passed && flowFrame(presenter, a, 0x100000u, ++swap);
    for (unsigned i = 0u; i < 2u && passed; ++i) {
        const std::vector<uint32_t> &b = *changes[i];
        passed = flowFrame(presenter, a, 0x100000u, ++swap) && flowFrame(presenter, b, 0x200000u + i * 0x100000u, ++swap);
        for (const double t : {0.25, 0.5}) {
            if (!passed || !generate(static_cast<float>(t))) break;
            engaged = true;
            unsigned off = 0u;
            for (size_t at = 0u; at < a.size(); ++at) {
                const double from = a[at] & 255u, to = b[at] & 255u;
                off += std::fabs((out[at] & 255u) - (from + (to - from) * t)) > 2.0;
            }
            std::printf("framegen %s t=%.2f: %u pixels off the crossfade by more than 2\n", i ? "flash" : "fade", t, off);
            passed = off <= a.size() / 100u;
            if (!passed) std::fprintf(stderr, "FAIL framegen %s is taken for a cut\n", i ? "flash" : "fade");
        }
    }
    /* A camera move of 48 pixels a real frame, beyond the cut test's reach:
       the first such pair is held like a cut, the next one is not. */
    std::vector<uint32_t> moved[2] = {a, a};
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            for (int step = 0; step < 2; ++step)
                moved[step][static_cast<size_t>(y) * 320u + x] = flowFrameA(x - 48 * (step + 1), y);
        }
    }
    passed = passed && flowFrame(presenter, a, 0x100000u, ++swap) && flowFrame(presenter, moved[0], 0x400000u, ++swap) &&
        flowFrame(presenter, moved[1], 0x500000u, ++swap) && generate(0.5f);
    if (passed && engaged) {
        unsigned held = 0u;
        for (size_t at = 0u; at < a.size(); ++at)
            held += std::abs(static_cast<int>(out[at] & 255u) - static_cast<int>(moved[0][at] & 255u)) <= 1;
        std::printf("framegen second fast pair: %u of %zu pixels equal the earlier real frame\n", held, a.size());
        passed = held < a.size() / 2u;
        if (!passed) std::fprintf(stderr, "FAIL framegen takes a fast camera move for a cut twice in a row\n");
    }
    const char *const restarts[4] = {"fast-forward", "frame clock", "late frame", "quality change"};
    for (unsigned way = 0u; way < 4u && passed && engaged; ++way) {
        passed = flowFrame(presenter, a, 0x100000u, ++swap);
        if (way == 0u) recomp_xapi_fast_forward = fastForwardOn;
        if (way == 1u) recomp_xapi_frame_clock_set_enabled(true);
        if (way == 3u) d3d_backend_set_quality(1.0f, 2u, false, 0.0f, false);  // applied after the next real frame
        if (way == 2u) {
            d3d_backend_framegen_set_late_ns(25000000);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        } else passed = passed && flowFrame(presenter, faded, 0x200000u, ++swap);
        recomp_xapi_fast_forward = nullptr;
        recomp_xapi_frame_clock_set_enabled(false);
        passed = passed && flowFrame(presenter, c, 0x400000u, ++swap) && generate(0.25f);
        d3d_backend_framegen_set_late_ns(INT64_MAX);
        unsigned off = 0u;
        // Away from the edges, which MSAA blends with the clear.
        for (int y = 2; y < 238 && passed; ++y) {
            for (int x = 2; x < 318; ++x) {
                const size_t at = static_cast<size_t>(y) * 320u + x;
                off += std::abs(static_cast<int>(out[at] & 255u) - static_cast<int>(c[at] & 255u)) > 2;
            }
        }
        std::printf("framegen after a %s: %u pixels of the first generated frame off the new real frame\n", restarts[way], off);
        passed = passed && off == 0u;
        if (!passed) std::fprintf(stderr, "FAIL framegen mixes real frames across a %s\n", restarts[way]);
    }
    d3d_backend_set_quality(1.0f, 1u, false, 0.0f, false);
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    if (passed) {
        std::printf(engaged ? "PASS frame generation takes no fade or flash for a cut and starts again after a break\n"
            : "SKIP frame generation's cuts, see the framegen unsupported line\n");
    }
    return passed;
}

#ifndef RECOMP_TEST_D3D11
/* RECOMP_D3D_FRAMEGEN_CAPTURE keeps what frame generation got for each real
   frame; pushed into a new presenter, the capture gives the in-process
   generated frames bit for bit: A twice, A moved by 13,9, then a cut to
   noise, each under a still red HUD quad, and the gamma ramp it was shown
   through. Engages only where frame generation does. */
static bool testFramegenCapture()
{
    char folder[MAX_PATH], capture_dir[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "fgc", 0, capture_dir)) return false;
    DeleteFileA(capture_dir);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    _putenv_s("RECOMP_D3D_FRAMEGEN_CAPTURE", capture_dir);
    std::vector<uint32_t> a(320u * 240u), b(a.size()), c(a.size());
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
            const size_t at = static_cast<size_t>(y) * 320u + x;
            a[at] = flowFrameA(x, y);
            b[at] = flowFrameA(x - 13, y - 9);
            c[at] = flowNoise(x, y, 11u);
        }
    }
    const HudVertex red[4] = {{128, 96, 0, 1, 0xffff0000u}, {192, 96, 0, 1, 0xffff0000u},
        {128, 144, 0, 1, 0xffff0000u}, {192, 144, 0, 1, 0xffff0000u}};
    const std::vector<RecompD3dPresenterCommand> overlays = {hudQuad(red, false)};
    const std::vector<uint32_t> *const frames[4] = {&a, &a, &b, &c};
    const float slots[3] = {0.25f, 0.5f, 0.75f};
    std::vector<std::vector<uint32_t>> expected, got;
    const auto generate = [&](RecompD3dPresenter *presenter, std::vector<std::vector<uint32_t>> &into) {
        for (const float t : slots) {
            unsigned size[2]{};
            into.emplace_back(a.size());
            if (d3d_backend_framegen_generate(presenter, t, 1u, size, into.back().data(), a.size()) < 0.0) return false;
        }
        return true;
    };
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create framegen capture", d3d_backend_create(&config, &presenter));
    RecompD3dPresenterCommand gamma{RECOMP_D3D_PRESENTER_COMMAND_GAMMA};
    for (auto &channel : gamma.data.gamma)
        for (unsigned i = 0u; i < 256u; ++i) channel[i] = static_cast<uint8_t>((i * i + 127u) / 255u);
    passed = passed && expect("framegen capture gamma", d3d_backend_submit(presenter, &gamma));
    bool engaged = passed;
    for (uint32_t i = 0u; i < 4u && passed && engaged; ++i) {
        passed = flowFrame(presenter, *frames[i], 0x100000u * (i + 1u), i + 1u, 0.0f, 0.0f, false, overlays);
        if (passed && i >= 2u) engaged = generate(presenter, expected);
    }
    d3d_backend_destroy(&presenter);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    _putenv_s("RECOMP_D3D_FRAMEGEN_CAPTURE", "");
    FramegenCapture capture;
    if (passed && engaged) {
        passed = framegenCaptureOpen(capture_dir, capture) && capture.frames.size() == 4u &&
            capture.width == 320u && capture.height == 240u &&
            expect("create framegen replay", d3d_backend_create(&config, &presenter));
        std::vector<uint32_t> picture;
        std::vector<uint8_t> hud;
        uint8_t ramp[3][256];
        for (size_t i = 0u; i < capture.frames.size() && passed; ++i) {
            passed = framegenCaptureLoad(capture, capture.frames[i], picture, hud, ramp) && capture.frames[i].gamma &&
                std::memcmp(ramp, gamma.data.gamma, sizeof ramp) == 0 &&
                d3d_backend_framegen_push(presenter, picture.data(), hud.data(), capture.frames[i].restart || i == 0u) &&
                (i < 2u || generate(presenter, got));
        }
        d3d_backend_destroy(&presenter);
        passed = passed && got == expected;
        if (!passed) std::fprintf(stderr, "FAIL framegen capture replays the generated frames bit for bit\n");
    }
    for (const FramegenCaptureFrame &frame : capture.frames) {
        for (const char *suffix : {"bgra", "hud", "gamma"}) {
            char path[MAX_PATH + 32];
            std::snprintf(path, sizeof path, "%s/%08u.%s", capture_dir, frame.frame, suffix);
            DeleteFileA(path);
        }
    }
    DeleteFileA((capture_dir + std::string("/framegen.txt")).c_str());
    RemoveDirectoryA(capture_dir);
    if (passed) {
        std::printf(engaged ? "PASS frame generation's capture replays its generated frames bit for bit\n"
            : "SKIP frame generation's capture, see the framegen unsupported line\n");
    }
    return passed;
}

/* RECOMP_FRAMEGEN_FLOW_BENCH=1: the flow's GPU time at 480 and 2160 (16:9),
   from the fence wait of 200 extra flow runs less that of none. */
static void benchFramegenFlow()
{
    if (!std::getenv("RECOMP_FRAMEGEN_FLOW_BENCH")) return;
    _putenv_s("RECOMP_D3D_FRAMEGEN", "1");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "1");
    const RecompD3dPresenterConfig wide = {640u, 480u, config.color_format, config.depth_format};
    for (const float scale : {1.0f, 1.5f, 4.5f}) {
        RecompD3dPresenter *presenter = nullptr;
        if (d3d_backend_create(&wide, &presenter) == RECOMP_D3D_PRESENTER_OK) {
            d3d_backend_set_quality(scale, 1u, false, 0.0f, false);
            for (uint32_t swap = 1u; swap <= 3u; ++swap) {
                auto clear = clearCommand();
                RecompD3dPresenterCommand present{RECOMP_D3D_PRESENTER_COMMAND_PRESENT};
                present.data.present = {5u, swap};
                d3d_backend_submit(presenter, &clear);
                d3d_backend_submit(presenter, &present);
            }
            unsigned size[2]{};
            double none = 1e9, runs = 1e9;
            for (unsigned i = 0u; i < 5u; ++i) {
                none = (std::min)(none, d3d_backend_framegen_flow(presenter, 0u, size, nullptr, 0u));
                runs = (std::min)(runs, d3d_backend_framegen_flow(presenter, 200u, size, nullptr, 0u));
            }
            std::printf("flow bench scale %.1f: %ux%u blocks, %.4f ms per run (fence %.3f ms for none, %.3f for 200)\n",
                scale, size[0], size[1], (runs - none) / 200.0, none, runs);
            none = runs = 1e9;
            for (unsigned i = 0u; i < 5u; ++i) {
                none = (std::min)(none, d3d_backend_framegen_generate(presenter, 0.5f, 0u, size, nullptr, 0u));
                runs = (std::min)(runs, d3d_backend_framegen_generate(presenter, 0.5f, 200u, size, nullptr, 0u));
            }
            std::printf("warp bench scale %.1f: %ux%u, %.4f ms per generated frame (fence %.3f ms for none, %.3f for 200)\n",
                scale, size[0], size[1], (runs - none) / 200.0, none, runs);
            // A HUD of 100 half-transparent 32x16 quads, a sixth of the 640x480 screen.
            for (unsigned i = 0u; i < 100u; ++i) {
                const float x = 16.0f + (i % 10u) * 60.0f, y = 16.0f + (i / 10u) * 44.0f;
                const HudVertex quad[4] = {{x, y, 0, 1, 0x80ffffffu}, {x + 32, y, 0, 1, 0x80ffffffu},
                    {x, y + 16, 0, 1, 0x80ffffffu}, {x + 32, y + 16, 0, 1, 0x80ffffffu}};
                const RecompD3dPresenterCommand command = hudQuad(quad, true);
                d3d_backend_submit(presenter, &command);
            }
            unsigned draws = 0u;
            none = runs = 1e9;
            for (unsigned i = 0u; i < 5u; ++i) {
                none = (std::min)(none, d3d_backend_framegen_cover(presenter, 0u, &draws));
                runs = (std::min)(runs, d3d_backend_framegen_cover(presenter, 200u, &draws));
            }
            std::printf("hud mask bench scale %.1f: %u draws, %.4f ms per real frame (fence %.3f ms for none, %.3f for 200)\n",
                scale, draws, (runs - none) / 200.0, none, runs);
        }
        d3d_backend_destroy(&presenter);
    }
    d3d_backend_set_quality(1.0f, 1u, false, 0.0f, false);
    _putenv_s("RECOMP_D3D_FRAMEGEN", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
}
#endif

#if defined(_WIN32) && !defined(RECOMP_TEST_D3D11)
/* Direct3D 12 takes only DXIL, so the shaders the game's first frames use and
   SMAA's three passes compile through SDL_shadercross and become SDL_GPU
   shaders on a Direct3D 12 device. */
static bool testDxilShaders()
{
    if (!recomp_shader_dxil_available()) {
        std::puts("SKIP dxil shaders: no dxcompiler.dll and dxil.dll beside the test");
        return true;
    }
    SDL_GPUDevice *device = SDL_InitSubSystem(SDL_INIT_VIDEO)
        ? SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, false, "direct3d12") : nullptr;
    if (device == nullptr) {
        std::printf("SKIP dxil shaders: no Direct3D 12 device: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return true;
    }
    bool passed = true;
    const auto create = [device, &passed](const std::string &source, const char *entry, bool vertex,
                             const std::vector<std::string> &defines, Uint32 samplers,
                             Uint32 uniform_buffers) {
        RecompCompiledShader compiled;
        std::string error;
        if (!recomp_shader_compile(source, entry, vertex, RecompShaderTarget::Dxil, defines, compiled,
                error)) {
            std::fprintf(stderr, "FAIL dxil %s compile: %s\n", entry, error.c_str());
            passed = false;
            return false;
        }
        SDL_GPUShaderCreateInfo info{};
        info.code = compiled.code.data();
        info.code_size = compiled.code.size();
        info.entrypoint = compiled.entry_point.c_str();
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniform_buffers;
        SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
        if (shader == nullptr) {
            std::fprintf(stderr, "FAIL dxil %s create: %s\n", entry, SDL_GetError());
            passed = false;
            return false;
        }
        SDL_ReleaseGPUShader(device, shader);
        return true;
    };
    unsigned draws = 0u;
    for (const RecompD3dBootDrawShader &boot : kRecompD3dBootDrawShaders) {
        const uint32_t fvf = boot.fvf;
        const RecompD3dDrawShaderKey key{fvf, 0u, nullptr, false, boot.depth_bias, false, boot.projected_stages};
        RecompD3dVertexLayout layout{};
        std::string source;
        if (!recomp_d3d_draw_shader_source(key, layout, source)) {
            std::fprintf(stderr, "FAIL dxil draw shader fvf=0x%03X has no source\n", fvf);
            passed = false;
            continue;
        }
        source = recomp_shader_draw_bindings(source);
        source = recomp_shader_keep_interpolants(source);
        const bool vertex = create(source, "vs_main", true, {}, 0u, 2u);
        const bool fragment = create(source, "ps_main", false, {}, 4u, 1u);
        if (vertex && fragment) ++draws;
    }
    std::string smaa;
    std::vector<std::string> smaa_defines;
    d3d_backend_smaa_source(1920u, 1080u, smaa, smaa_defines);
    static const char *const passes[3] = {"edges", "weights", "blend"};
    for (const char *pass : passes) {
        const std::string vertex = std::string("vs_") + pass;
        const std::string fragment = std::string("ps_") + pass;
        passed &= create(smaa, vertex.c_str(), true, smaa_defines, 0u, 0u);
        passed &= create(smaa, fragment.c_str(), false, smaa_defines, 5u, 0u);
    }
    SDL_DestroyGPUDevice(device);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (passed) {
        std::printf("PASS dxil: %u boot draw shaders and SMAA's three passes\n", draws);
    }
    return passed;
}
#endif

int main()
{
#if defined(_WIN32) && !defined(RECOMP_TEST_D3D11)
    // Hosted Windows CI has no GPU driver; ctest counts this as a skip. A run
    // that asked for Direct3D 12 creates that device instead of Vulkan.
    SDL_GPUDevice *gpu = SDL_InitSubSystem(SDL_INIT_VIDEO)
        ? SDL_CreateGPUDevice(recomp_shader_dxil_requested()
            ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr) : nullptr;
    if (gpu == nullptr) {
        std::printf("SKIP no SDL_GPU device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("SDL_GPU driver: %s\n", SDL_GetGPUDeviceDriver(gpu));
    SDL_DestroyGPUDevice(gpu);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (!testDxilShaders()) return 1;
#endif
    recomp_d3d_presenter_set_immediate_present(false);
    if (!testShaderCacheFolder() || !testPrecompileBootShaders() || !testResampledMsaaTarget() ||
        !testWidescreenAntialiasing(false, false) || !testWidescreenAntialiasing(true, false) ||
        !testWidescreenAntialiasing(false, true) || !testWidescreenAntialiasing(true, true) ||
        !testWidescreenAntialiasing(false, false, 1080u) || !testWidescreenAntialiasing(true, true, 1080u, 32u) ||
        !testSharpen(false, false) || !testSharpen(true, false) ||
        !testDrawCoverage() || !testDrawCoverage(true) || !testQueuedDrawCoverage() ||
#ifdef RECOMP_TEST_D3D11
        !testFullscreenKeys() || !testFullscreenKeys(true) || !testOneFrameInFlight() ||
#endif
        !testTicks() || !testFrameClockPacing() ||
        !testCapturedDraw() || !testMipSampler() || !testUncompressedMipChain() ||
        !testTextureBurst() || !testPointMinMagFilter() ||
        !testLines() || !testLighting() ||
        !testReflectionStage2() || !testReflectionStage2(true) || !testReflectionStage2(false, true) ||
        !testReflectionAddress(false) || !testReflectionAddress(true) || !testReflectionAddress(true, true) ||
        !testReflectionAddress(false, false, true) || !testReflectionAddress(true, false, false, true) ||
        !testReflectionAddress(false, false, false, false, -1.0f, true) ||
        !testReflectionAddress(false, false, false, false, 0.0f) ||
        !testReflectionAddress(false, false, false, false, 0.5f) ||
        !testReflectionAddress(false, false, false, false, 1.0f) ||
        !testScreenSpaceOffsetAndScissor() || !testBrightPass() || !testDistortion() || !testStageEffect() ||
        !testVolume() || !testVolume(true) || !testFrameBufferBlend() || !testDepthBias() || !testDepthBias(true) ||
        !testCard() ||
        !testScaledMsaaDepthBias() || !testProgramMaxSlt() ||
#ifndef RECOMP_TEST_D3D11
        !testBootProgramShaders(false) || !testBootProgramShaders(true) ||
#endif
        !testProgramLitSge() ||
        !testStageBump() ||
        !testStageWater(false) || !testStageWater(true) ||
        !testVisibility() ||
        !testClose(false) || !testClose(true) || !testPresentDownscale() ||
        !testLiveQuality() || !testSharpen(false, true) ||
        !testAmbientOcclusion(false) || !testAmbientOcclusion(true)) return 1;
    // These tests push frames unpaced; only testFramegenCuts' late frame keeps the 25 ms rule.
    d3d_backend_framegen_set_late_ns(INT64_MAX);
    if (!testFramegenKeepsRealFrames() || !testFramegenKeepsOpenCommands() || !testFramegenFlow() ||
        !testFramegenWarp() || !testFramegenWarpEdges() || !testFramegenHud() || !testFramegenCuts()) return 1;
#ifndef RECOMP_TEST_D3D11
    if (!testFramegenCapture()) return 1;
    benchFramegenFlow();
#endif
    std::puts("PASS presenter render thread");
    return 0;
}
