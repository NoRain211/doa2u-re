// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOA2U_RECOMP_TEXTURE_UPSCALE_H
#define DOA2U_RECOMP_TEXTURE_UPSCALE_H

#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

/* The game's 2D art (menus, fonts, the HUD) is drawn at 480-line size and
   looks jagged when stretched to a modern display. The presenter redraws each
   such texture once, when first uploaded, with xBRZ (Zenju's edge-preserving
   "scale by rules" filter, GPL-3.0; fetched by the build, see NOTICE).

   The factor for a width x height texture: 4 up to 256 texels on a side, 2 up
   to 512, else 1 (no scaling). RECOMP_UI_UPSCALE=0 turns it off. */
uint32_t recomp_texture_upscale_factor(uint32_t width, uint32_t height);

/* Scales BGRA pixels (rows top down, alpha kept) by factor (2-6) into out,
   on up to max_threads worker threads. False if the factor or size is invalid. */
bool recomp_texture_upscale(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t factor,
    std::vector<uint8_t> &out, unsigned max_threads = 8u);

/* Redraws textures on a thread of its own (#305). xBRZ of one 256x256 menu
   texture takes 40-55 ms, which held up the frame that first drew it, so the
   presenter draws the plain texture until take() hands it the larger one,
   usually a few frames later. Each presenter owns one; its thread starts with
   the first job and uses half the hardware threads, leaving the rest to the
   game and the renderer.

   Memory is capped. Waiting jobs keep at most queue_bytes of plain pixels:
   a job that would go past it drops the oldest waiting ones. Finished copies
   not yet taken keep at most done_bytes: a new one past it drops the oldest.
   A dropped job finishes with no pixels, so its art stays at its own size, as
   with RECOMP_UI_UPSCALE=0. The defaults (16 MiB, 64 MiB) hold 64 plain and
   16 finished 256x256 textures at 4x. */
class RecompTextureUpscaler {
public:
    static constexpr size_t kQueueBytes = size_t{16} << 20;
    static constexpr size_t kDoneBytes = size_t{64} << 20;
    RecompTextureUpscaler() = default;
    RecompTextureUpscaler(size_t queue_bytes, size_t done_bytes) : queue_cap_(queue_bytes), done_cap_(done_bytes) {}
    RecompTextureUpscaler(const RecompTextureUpscaler &) = delete;
    RecompTextureUpscaler &operator=(const RecompTextureUpscaler &) = delete;
    ~RecompTextureUpscaler();

    /* Queues bgra (width x height, rows top down) to be scaled by factor and
       returns the job, never 0. Without a thread the job runs here. */
    uint64_t submit(std::vector<uint8_t> bgra, uint32_t width, uint32_t height, uint32_t factor);
    /* True once job is finished, which forgets it: out holds the larger
       pixels, or is empty when they could not be made. False while it waits. */
    bool take(uint64_t job, std::vector<uint8_t> &out);
    /* Forgets a job whose texture is gone. 0 and a taken job are ignored. */
    void cancel(uint64_t job);
    /* For tests: returns once every queued job has finished. */
    void wait();

private:
    struct Job {
        uint64_t id;
        std::vector<uint8_t> bgra;
        uint32_t width, height, factor;
    };
    void run();
    void finish(Job &job, unsigned threads);
    void trimDone();  // with lock_ held

    std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::deque<Job> queue_;
    std::map<uint64_t, std::vector<uint8_t>> done_;  // by job, oldest first
    const size_t queue_cap_ = kQueueBytes, done_cap_ = kDoneBytes;
    size_t queue_bytes_ = 0u, done_bytes_ = 0u;
    uint64_t next_ = 0u;
    uint64_t running_ = 0u;  // the job being scaled, or 0 (also once it is cancelled)
    bool stop_ = false;
    std::thread thread_;
};

#endif
