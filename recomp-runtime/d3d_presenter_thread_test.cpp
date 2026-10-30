#include "d3d_presenter.h"
#include "d3d_presenter_backend.h"
#include "d3d_card_model.h"
#include "d3d_draw_model.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "win32_compat.h"
#include <SDL3/SDL.h>
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>
#include <vector>

// d3d_vblank.cpp's guest pacing reaches the kernel; these tests use only its host sleep.
extern "C" void recomp_kernel_wait_for_vblank(uint64_t) {}

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

#ifdef _WIN32
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
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS five CLEAR/PRESENT ticks and partial shutdown");
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
        passed &= expect("projected draw", d3d_backend_submit(presenter, &command));
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
            // the projected quad reads red at s/q 0.31 (s 0.63 would be green), green at 0.69.
            const int pixels[][4] = {{25, 40, 0, 128}, {55, 40, 0, 64}, {85, 40, 0, 255}, {115, 40, 0, 0},
                {145, 40, 0, 128}, {175, 40, 0, 0}, {100, 200, 255, 0}, {220, 200, 0, 255},
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
    if (passed) std::puts("PASS stage-effect overlays: texture select, factor alpha, UV1 mask/kill, UV0 kill, projected texgen");
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
// r0 = s + oD1 t1.a - 1 + t3.a t1.
static bool testStageWater()
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
        const uint32_t reflection[2] = {0xffff0000u, 0x8000ff00u}, environment = 0x808080ffu;
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
            unsigned char bgr[3]{};
            std::fseek(file, 54 + (239 - 120) * 960 + 160 * 3, SEEK_SET);
            passed &= std::fread(bgr, 1, 3, file) == 3;
            const bool pixel_ok = bgr[0] < 4 && std::abs(bgr[1] - 128) <= 4 && std::abs(bgr[2] - 64) <= 4;
            if (!pixel_ok) std::fprintf(stderr, "FAIL stage water pixel BGR=%u,%u,%u\n", bgr[0], bgr[1], bgr[2]);
            passed &= pixel_ok;
            std::fclose(file);
        } else passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS stage water bump: offset reflection, oD1 and dot^8 highlight");
    return passed;
}

static bool testClose(bool idle)
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create close", recomp_d3d_presenter_create(&config, &presenter))) return false;
#ifndef _WIN32
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
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (IsWindow(window) && GetTickCount64() < deadline) Sleep(1);
        passed = !IsWindow(window);
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
#ifdef _WIN32
    // The render thread publishes nothing before the flush.
    passed &= !complete;
#endif
    passed &= expect("unknown index", recomp_d3d_presenter_visibility_result(presenter, 9u, &complete, &pixels[0]),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    passed &= expect("publish visibility", recomp_d3d_presenter_flush(presenter, false));
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
#ifdef _WIN32
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
#ifndef _WIN32
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
#ifdef _WIN32
        RECT rect{};
        const unsigned client_height = (std::min)(height, static_cast<unsigned>(GetSystemMetrics(SM_CYSCREEN)));
        passed &= GetClientRect(presenterWindow(), &rect) &&
            rect.right == (client_height * 16u + 8u) / 9u && rect.bottom == client_height;
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

int main()
{
    recomp_d3d_presenter_set_immediate_present(false);
    if (!testResampledMsaaTarget() ||
        !testWidescreenAntialiasing(false, false) || !testWidescreenAntialiasing(true, false) ||
        !testWidescreenAntialiasing(false, true) || !testWidescreenAntialiasing(true, true) ||
        !testWidescreenAntialiasing(false, false, 1080u) || !testWidescreenAntialiasing(true, true, 1080u, 32u) ||
        !testDrawCoverage() || !testDrawCoverage(true) || !testQueuedDrawCoverage() ||
        !testTicks() || !testCapturedDraw() || !testMipSampler() || !testLines() || !testLighting() ||
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
        !testProgramLitSge() ||
        !testStageBump() ||
        !testStageWater() ||
        !testVisibility() ||
        !testClose(false) || !testClose(true)) return 1;
    std::puts("PASS presenter render thread");
    return 0;
}
