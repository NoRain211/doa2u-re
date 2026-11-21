// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_D3D_FRAMEGEN_CAPTURE_H
#define DOA2U_RECOMP_D3D_FRAMEGEN_CAPTURE_H

/* Reads a RECOMP_D3D_FRAMEGEN_CAPTURE folder (framegenCapture in
   d3d_presenter_sdl_gpu.cpp writes it), for d3d_framegen_replay.cpp and the
   presenter test. */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct FramegenCaptureFrame {
    unsigned frame = 0u;
    bool restart = false;  // N started frame generation's history again
    bool gamma = false;    // <N>.gamma holds the ramp shown with N
    float aspect = 0.0f;   // the game's picture aspect, 0 for the window's
};

struct FramegenCapture {
    std::string folder;
    uint32_t width = 0u, height = 0u;
    unsigned k = 0u;  // the capturing run's slots per real frame
    std::vector<FramegenCaptureFrame> frames;
};

inline bool framegenCaptureOpen(const std::string &folder, FramegenCapture &capture)
{
    capture = {};
    capture.folder = folder;
    FILE *index = std::fopen((folder + "/framegen.txt").c_str(), "r");
    if (index == nullptr) return false;
    bool read = std::fscanf(index, "recomp framegen capture 1 target=%ux%u k=%u", &capture.width, &capture.height,
        &capture.k) == 3;
    FramegenCaptureFrame frame;
    int restart = 0, gamma = 0;
    while (read && std::fscanf(index, " %u restart=%d gamma=%d aspect=%f", &frame.frame, &restart, &gamma,
               &frame.aspect) == 4) {
        frame.restart = restart != 0;
        frame.gamma = gamma != 0;
        capture.frames.push_back(frame);
    }
    std::fclose(index);
    return read && capture.width != 0u && capture.height != 0u;
}

inline bool framegenCaptureRead(const FramegenCapture &capture, unsigned frame, const char *suffix, void *bytes,
    size_t size)
{
    char name[32];
    std::snprintf(name, sizeof name, "/%08u.%s", frame, suffix);
    FILE *file = std::fopen((capture.folder + name).c_str(), "rb");
    if (file == nullptr) return false;
    const bool read = std::fread(bytes, 1, size, file) == size && std::fgetc(file) == EOF;
    std::fclose(file);
    return read;
}

/* N's picture (0xAARRGGBB before gamma), HUD mask (white without one) and
   gamma ramp (identity unless frame.gamma). False on a missing or
   wrong-sized file, as after a quality change in the capturing run. */
inline bool framegenCaptureLoad(const FramegenCapture &capture, const FramegenCaptureFrame &frame,
    std::vector<uint32_t> &picture, std::vector<uint8_t> &hud, uint8_t (&ramp)[3][256])
{
    const size_t pixels = static_cast<size_t>(capture.width) * capture.height;
    picture.resize(pixels);
    hud.assign(pixels, 255u);
    for (unsigned channel = 0u; channel < 3u; ++channel)
        for (unsigned i = 0u; i < 256u; ++i) ramp[channel][i] = static_cast<uint8_t>(i);
    if (!framegenCaptureRead(capture, frame.frame, "bgra", picture.data(), pixels * 4u)) return false;
    char name[32];
    std::snprintf(name, sizeof name, "/%08u.hud", frame.frame);
    if (FILE *file = std::fopen((capture.folder + name).c_str(), "rb")) {
        std::fclose(file);
        if (!framegenCaptureRead(capture, frame.frame, "hud", hud.data(), pixels)) return false;
    }
    return !frame.gamma || framegenCaptureRead(capture, frame.frame, "gamma", ramp, sizeof ramp);
}

#endif
