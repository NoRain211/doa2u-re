// SPDX-License-Identifier: GPL-3.0-or-later
/* d3d_presenter_thread_test.cpp's xBRZ test, included after its helpers. It
   sits in its own file to keep that one under public-export.json's file size
   limit. */

// 2D art's xBRZ copy is drawn off the presenter's thread (#305): the first
// frame draws the plain texture, and a draw after the copy is ready swaps it in.
static bool testUpscaleSwap()
{
    char folder[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, folder) || !GetTempFileNameA(folder, "xbrz", 0, path)) return false;
    _putenv_s("RECOMP_D3D_FRAME_DUMP", path);
    _putenv_s("RECOMP_D3D_FRAME_DUMP_COUNT", "3");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "0");
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("create upscale", d3d_backend_create(&config, &presenter));
    if (passed) {
        // 16x16 noise, drawn 160 pixels square: xBRZ redraws it at 64x64.
        std::vector<uint32_t> texels(16u * 16u);
        uint32_t seed = 305u;
        for (uint32_t &texel : texels) {
            seed = seed * 1103515245u + 12345u;
            texel = 0xff000000u | (seed >> 8);
        }
        struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
        const Vertex vertices[4] = {{0.0f, 0.0f, 0.25f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
            {160.0f, 0.0f, 0.25f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
            {0.0f, 160.0f, 0.25f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
            {160.0f, 160.0f, 0.25f, 1.0f, 0xffffffffu, 1.0f, 1.0f}};
        const uint16_t indices[] = {0, 1, 2, 3};
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
        draw.texture.mip_levels = 1u;
        draw.texture.data = 0x10000u;
        draw.texture_bytes = texels.data();
        draw.texture_byte_count = static_cast<uint32_t>(texels.size() * 4u);
        RecompD3dPresenterCommand present{};
        present.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        present.data.present = {5u, 1u};
        for (unsigned frame = 0u; frame < 3u && passed; ++frame) {
            auto clear = clearCommand();
            clear.data.clear.color = 0xff000000u;
            passed &= expect("upscale clear", d3d_backend_submit(presenter, &clear));
            passed &= expect("upscale draw", d3d_backend_submit(presenter, &command));
            passed &= expect("upscale present", d3d_backend_submit(presenter, &present));
            if (frame == 0u) d3d_backend_wait_upscales(presenter);
        }
        passed &= expect("destroy upscale", d3d_backend_destroy(&presenter));
    }
    // The quad's pixels in each dump (24-bit BMP, rows bottom up).
    std::vector<unsigned char> frames[3];
    for (unsigned i = 0u; i < 3u && passed; ++i) {
        const std::string dump = std::string(path) + "." + "00" + std::to_string(i) + ".bmp";
        if (FILE *file = std::fopen(dump.c_str(), "rb")) {
            frames[i].resize(160u * 160u * 3u);
            for (unsigned y = 0u; y < 160u && passed; ++y) {
                std::fseek(file, 54 + static_cast<long>(240u - 1u - y) * 320 * 3, SEEK_SET);
                passed &= std::fread(frames[i].data() + y * 160u * 3u, 1, 160u * 3u, file) == 160u * 3u;
            }
            std::fclose(file);
        } else {
            std::fprintf(stderr, "FAIL upscale dump %u is missing\n", i);
            passed = false;
        }
        DeleteFileA(dump.c_str());
    }
    if (passed && frames[0] == frames[1]) {
        std::fprintf(stderr, "FAIL upscale: the frame after the redraw still shows the plain texture\n");
        passed = false;
    }
    if (passed && frames[1] != frames[2]) {
        std::fprintf(stderr, "FAIL upscale: the larger copy changed between frames\n");
        passed = false;
    }
    _putenv_s("RECOMP_D3D_FRAME_DUMP", "");
    _putenv_s("RECOMP_D3D_FRAME_DUMP_COUNT", "");
    _putenv_s("RECOMP_D3D_WIDESCREEN", "");
    DeleteFileA(path);
    if (passed) std::puts("PASS 2D art draws plain until its xBRZ copy is ready, then the copy");
    return passed;
}
