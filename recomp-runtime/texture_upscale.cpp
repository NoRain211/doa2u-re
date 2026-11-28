// SPDX-License-Identifier: GPL-3.0-or-later
#include "texture_upscale.h"

#include "xbrz.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>

uint32_t recomp_texture_upscale_factor(uint32_t width, uint32_t height)
{
    static const bool enabled = [] {
        const char *setting = std::getenv("RECOMP_UI_UPSCALE");
        return setting == nullptr || std::strcmp(setting, "0") != 0;
    }();
    if (!enabled || width == 0u || height == 0u) return 1u;
    const uint32_t side = std::max(width, height);
    return side <= 256u ? 4u : side <= 512u ? 2u : 1u;
}

bool recomp_texture_upscale(const uint8_t *bgra, uint32_t width, uint32_t height, uint32_t factor,
    std::vector<uint8_t> &out, unsigned max_threads)
{
    if (bgra == nullptr || factor < 2u || factor > 6u || width == 0u || height == 0u ||
        width > 4096u || height > 4096u) return false;
    /* Little-endian BGRA bytes are xBRZ's ARGB words. */
    std::vector<uint32_t> source(static_cast<size_t>(width) * height);
    std::memcpy(source.data(), bgra, source.size() * 4u);
    std::vector<uint32_t> target(source.size() * factor * factor);
    /* Slices of at least 16 rows, one per hardware thread (xBRZ allows any
       non-overlapping source rows per thread). */
    const unsigned threads =
        std::max(1u, std::min({std::thread::hardware_concurrency(), max_threads, 8u, height / 16u}));
    const auto slice = [&](unsigned i) {
        const int first = static_cast<int>(static_cast<uint64_t>(height) * i / threads);
        const int last = static_cast<int>(static_cast<uint64_t>(height) * (i + 1u) / threads);
        xbrz::scale(factor, source.data(), target.data(), static_cast<int>(width), static_cast<int>(height),
            xbrz::ColorFormat::ARGB, xbrz::ScalerCfg(), first, last);
    };
    std::vector<std::thread> workers;
    try {
        for (unsigned i = 1u; i < threads; ++i) workers.emplace_back(slice, i);
    } catch (const std::system_error &) {
        // Fewer threads: the rest run here.
    }
    slice(0u);
    for (unsigned i = static_cast<unsigned>(workers.size()) + 1u; i < threads; ++i) slice(i);
    for (std::thread &worker : workers) worker.join();
    out.resize(target.size() * 4u);
    std::memcpy(out.data(), target.data(), out.size());
    return true;
}

RecompTextureUpscaler::~RecompTextureUpscaler()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void RecompTextureUpscaler::trimDone()
{
    // Oldest first; dropped and failed jobs hold no pixels and are kept.
    for (auto it = done_.begin(); done_bytes_ > done_cap_ && it != done_.end(); ++it) {
        done_bytes_ -= it->second.size();
        std::vector<uint8_t>().swap(it->second);
    }
}

void RecompTextureUpscaler::finish(Job &job, unsigned threads)
{
    std::vector<uint8_t> larger;
    if (!recomp_texture_upscale(job.bgra.data(), job.width, job.height, job.factor, larger, threads) ||
        larger.size() != job.bgra.size() * job.factor * job.factor) larger.clear();
    std::lock_guard<std::mutex> guard(lock_);
    if (running_ == job.id) {
        done_bytes_ += larger.size();
        done_[job.id] = std::move(larger);
        trimDone();
    }
    running_ = 0u;
}

uint64_t RecompTextureUpscaler::submit(std::vector<uint8_t> bgra, uint32_t width, uint32_t height,
    uint32_t factor)
{
    Job job{0u, std::move(bgra), width, height, factor};
    std::unique_lock<std::mutex> guard(lock_);
    job.id = ++next_;
    if (!thread_.joinable()) {
        try {
            thread_ = std::thread(&RecompTextureUpscaler::run, this);
        } catch (const std::system_error &) {
            // No thread: scale it now, as before the thread existed.
            running_ = job.id;
            guard.unlock();
            finish(job, 8u);
            return job.id;
        }
    }
    // Past the cap, the oldest waiting jobs finish now with no pixels.
    while (!queue_.empty() && queue_bytes_ + job.bgra.size() > queue_cap_) {
        queue_bytes_ -= queue_.front().bgra.size();
        done_.emplace(queue_.front().id, std::vector<uint8_t>());
        queue_.pop_front();
    }
    const uint64_t id = job.id;
    queue_bytes_ += job.bgra.size();
    queue_.push_back(std::move(job));
    guard.unlock();
    wake_.notify_one();
    return id;
}

bool RecompTextureUpscaler::take(uint64_t job, std::vector<uint8_t> &out)
{
    std::lock_guard<std::mutex> guard(lock_);
    const auto found = done_.find(job);
    if (found == done_.end()) return false;
    done_bytes_ -= found->second.size();
    out = std::move(found->second);
    done_.erase(found);
    return true;
}

void RecompTextureUpscaler::cancel(uint64_t job)
{
    if (job == 0u) return;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (running_ == job) running_ = 0u;
        const auto found = done_.find(job);
        if (found != done_.end()) {
            done_bytes_ -= found->second.size();
            done_.erase(found);
        }
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (it->id == job) {
                queue_bytes_ -= it->bgra.size();
                queue_.erase(it);
                break;
            }
        }
    }
    idle_.notify_all();
}

void RecompTextureUpscaler::wait()
{
    std::unique_lock<std::mutex> guard(lock_);
    idle_.wait(guard, [this] { return queue_.empty() && running_ == 0u; });
}

void RecompTextureUpscaler::run()
{
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2u);
    std::unique_lock<std::mutex> guard(lock_);
    for (;;) {
        wake_.wait(guard, [this] { return stop_ || !queue_.empty(); });
        if (stop_) return;
        Job job = std::move(queue_.front());
        queue_.pop_front();
        queue_bytes_ -= job.bgra.size();
        running_ = job.id;
        guard.unlock();
        finish(job, threads);
        guard.lock();
        if (queue_.empty()) idle_.notify_all();
    }
}
