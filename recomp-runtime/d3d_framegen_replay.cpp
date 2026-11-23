// SPDX-License-Identifier: GPL-3.0-or-later
/* Replays a RECOMP_D3D_FRAMEGEN_CAPTURE folder through the SDL_GPU
   presenter's frame generation, offline: for each real frame N, the k - 1
   generated frames between N-1 and N, then N, after gamma, as the window
   shows them at k times 60 Hz. docs/frame-generation.md describes it.

   recomp_d3d_framegen_replay <capture> <output> [--k N] [--crossfade | --original]
       [--ffmpeg <ffmpeg output options>...]

   output is a folder of <N>-<slot>.bmp files, or with --ffmpeg the file the
   raw frames are piped to ffmpeg (on PATH) for. --crossfade generates the
   plain crossfade, --original repeats N k times (the game at 60 Hz). */

#include "d3d_framegen_capture.h"
#include "d3d_presenter_backend.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// d3d_vblank.cpp's guest pacing reaches the kernel; the replay never paces.
extern "C" void recomp_kernel_wait_for_vblank(uint64_t) {}
extern "C" uint32_t recomp_kernel_current_thread_id(void) { return 1u; }

static bool writeBmp(const std::string &path, const std::vector<uint32_t> &pixels, uint32_t width, uint32_t height)
{
    const uint32_t padded = (width * 3u + 3u) & ~3u, image = padded * height, total = 54u + image;
    uint8_t header[54]{'B', 'M'};
    const uint32_t offset = 54u, info = 40u;
    const uint16_t planes = 1u, bpp = 24u;
    std::memcpy(header + 2, &total, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &info, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image, 4);
    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    bool written = std::fwrite(header, 1, sizeof header, file) == sizeof header;
    std::vector<uint8_t> row(padded, 0u);
    for (uint32_t y = height; y-- > 0u && written;) {
        for (uint32_t x = 0u; x < width; ++x) std::memcpy(&row[x * 3u], &pixels[static_cast<size_t>(y) * width + x], 3u);
        written = std::fwrite(row.data(), 1, padded, file) == padded;
    }
    return std::fclose(file) == 0 && written;
}

int main(int argc, char **argv)
{
    unsigned k = 4u;
    bool crossfade = false, original = false, usage = argc < 3;
    std::string ffmpeg;
    int arg = 3;
    for (; arg < argc && ffmpeg.empty() && !usage; ++arg) {
        if (std::strcmp(argv[arg], "--k") == 0 && arg + 1 < argc) k = static_cast<unsigned>(std::atoi(argv[++arg]));
        else if (std::strcmp(argv[arg], "--crossfade") == 0) crossfade = true;
        else if (std::strcmp(argv[arg], "--original") == 0) original = true;
        else if (std::strcmp(argv[arg], "--ffmpeg") == 0) ffmpeg = "ffmpeg";
        else usage = true;
    }
    FramegenCapture capture;
    if (usage || k < 2u || k > 8u || (crossfade && original)) {
        std::fprintf(stderr, "usage: recomp_d3d_framegen_replay <capture> <output> [--k N] [--crossfade | --original] "
            "[--ffmpeg <ffmpeg output options>...]\n");
        return 2;
    }
    if (!framegenCaptureOpen(argv[1], capture) || capture.frames.empty()) {
        std::fprintf(stderr, "framegen replay: no capture in %s\n", argv[1]);
        return 1;
    }
    const uint32_t width = capture.width, height = capture.height;
    FILE *pipe = nullptr;
    if (!ffmpeg.empty()) {
        char input[160];
        std::snprintf(input, sizeof input, " -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgra -video_size %ux%u "
            "-framerate %u -i -", width, height, 60u * k);
        ffmpeg += input;
        for (int i = arg; i <= argc; ++i) {
            // cmd.exe runs the line: one of these in an argument would end its quotes or expand there.
            const char *text = i < argc ? argv[i] : argv[2];
            if (std::strpbrk(text, "\"%^&|<>\r\n") != nullptr) {
                std::fprintf(stderr, "framegen replay: --ffmpeg arguments and the output cannot hold \" %% ^ & | < > "
                    "or line breaks: %s\n", text);
                return 2;
            }
            ffmpeg += std::string(" \"") + text + "\"";
        }
        pipe = _popen(ffmpeg.c_str(), "wb");
        if (pipe == nullptr) {
            std::fprintf(stderr, "framegen replay: cannot run ffmpeg\n");
            return 1;
        }
    } else if (std::error_code error; !std::filesystem::create_directories(argv[2], error) && error) {
        std::fprintf(stderr, "framegen replay: cannot create %s: %s\n", argv[2], error.message().c_str());
        return 1;
    }

    // A hidden presenter whose main target is the capture's size, at scale 1.
    for (const char *name : {"RECOMP_D3D_SCALE", "RECOMP_D3D_WIDESCREEN", "RECOMP_D3D_MSAA", "RECOMP_D3D_SMAA",
             "RECOMP_D3D_SHARPEN", "RECOMP_D3D_FRAMEGEN", "RECOMP_D3D_FRAMEGEN_CAPTURE"}) _putenv_s(name, "");
    _putenv_s("RECOMP_HEADLESS", "1");
    const RecompD3dPresenterConfig config = {width, height, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    RecompD3dPresenter *presenter = nullptr;
    if (d3d_backend_create(&config, &presenter) != RECOMP_D3D_PRESENTER_OK) {
        std::fprintf(stderr, "framegen replay: no SDL_GPU presenter\n");
        return 1;
    }
    if (crossfade) d3d_backend_framegen_crossfade(presenter);

    std::vector<uint32_t> picture, shown(static_cast<size_t>(width) * height);
    std::vector<uint8_t> hud;
    uint8_t ramp[3][256];
    bool passed = true;
    for (size_t i = 0u; i < capture.frames.size() && passed; ++i) {
        const FramegenCaptureFrame &frame = capture.frames[i];
        passed = framegenCaptureLoad(capture, frame, picture, hud, ramp) &&
            d3d_backend_framegen_push(presenter, picture.data(), hud.data(), frame.restart || i == 0u);
        if (!passed) std::fprintf(stderr, "framegen replay: frame %u cannot be read or pushed\n", frame.frame);
        for (unsigned slot = 0u; slot < k && passed; ++slot) {
            unsigned size[2]{};
            if (slot + 1u == k || original) {
                shown = picture;
            } else {
                passed = d3d_backend_framegen_generate(presenter, static_cast<float>(slot + 1u) / k, 1u, size,
                    shown.data(), shown.size()) >= 0.0 && size[0] == width && size[1] == height;
            }
            // The gamma pass maps each 8-bit channel through the ramp, as here.
            for (uint32_t &pixel : shown) {
                pixel = (pixel & 0xff000000u) | ramp[0][pixel >> 16 & 255u] << 16 | ramp[1][pixel >> 8 & 255u] << 8 |
                    ramp[2][pixel & 255u];
            }
            if (passed && pipe != nullptr) {
                passed = std::fwrite(shown.data(), 4u, shown.size(), pipe) == shown.size();
            } else if (passed) {
                char name[32];
                std::snprintf(name, sizeof name, "/%08u-%u.bmp", frame.frame, slot);
                passed = writeBmp(argv[2] + std::string(name), shown, width, height);
            }
            if (!passed) std::fprintf(stderr, "framegen replay: frame %u slot %u failed\n", frame.frame, slot);
        }
        if ((i + 1u) % 600u == 0u) std::fprintf(stderr, "framegen replay: %zu of %zu real frames\n", i + 1u,
            capture.frames.size());
    }
    d3d_backend_destroy(&presenter);
    if (pipe != nullptr && _pclose(pipe) != 0) passed = false;
    std::fprintf(stderr, "framegen replay: %s, %zu real frames, k=%u\n", passed ? "done" : "failed",
        capture.frames.size(), k);
    return passed ? 0 : 1;
}
