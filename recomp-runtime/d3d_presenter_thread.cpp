#include "d3d_presenter_capture.h"
#include "d3d_presenter_backend.h"
#include "d3d_vblank.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>


namespace {

struct PresenterThread {
    DWORD owner_thread = GetCurrentThreadId();
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    HANDLE wake = nullptr;
    D3dCapturePacket packets[3];
    unsigned open = 0;
    unsigned front = 0;
    unsigned pending = 0; // Includes the packet executing outside the mutex.
    bool started = false;
    bool shutdown = false;
    RecompD3dPresenterError created = RECOMP_D3D_PRESENTER_OK;
    RecompD3dPresenterError status = RECOMP_D3D_PRESENTER_OK;
    RecompD3dPresenterError destroyed = RECOMP_D3D_PRESENTER_OK;
    unsigned draw_declines = 0;
    bool covered = true;
    // Guest visibility indices: ENDs submitted but unresolved, and the latest count.
    struct Visibility { unsigned pending = 0; uint32_t pixels = 0; };
    std::unordered_map<uint32_t, Visibility> visibility;
    // RECOMP_PERF_COUNTER pacing on the game side, reported once per second.
    bool pacing = false;
    double last_frame_ms = 0.0;
    double frame_max_ms = 0.0;
    double busy_max_ms = 0.0;  // game frame minus its vblank sleeps: the guest's own CPU work
    double busy_total_ms = 0.0;
    unsigned frames = 0u;
    double queue_wait_ms = 0.0;
    ULONGLONG pacing_start = 0u;

    ~PresenterThread() { if (wake != nullptr) CloseHandle(wake); }
};

PresenterThread *active_thread;

RecompD3dPresenterError validate(RecompD3dPresenter *presenter)
{
    if (presenter == nullptr ||
        presenter != reinterpret_cast<RecompD3dPresenter *>(active_thread)) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    return GetCurrentThreadId() == active_thread->owner_thread
        ? RECOMP_D3D_PRESENTER_OK : RECOMP_D3D_PRESENTER_WRONG_THREAD;
}

RecompD3dPresenterError status(PresenterThread &thread)
{
    std::lock_guard<std::mutex> lock(thread.mutex);
    return thread.status;
}

void fail(PresenterThread &thread, RecompD3dPresenterError error)
{
    if (error == RECOMP_D3D_PRESENTER_OK) return;
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        if (thread.status == RECOMP_D3D_PRESENTER_OK) thread.status = error;
    }
    thread.changed.notify_all();
}

void pump(PresenterThread &thread)
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            std::fprintf(stderr, "recomp d3d presenter: WM_QUIT\n");
            fail(thread, RECOMP_D3D_PRESENTER_HOST_FAILURE);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_CLOSE && message.hwnd != nullptr) {
            fail(thread, RECOMP_D3D_PRESENTER_CLOSED);
        }
    }
}

void execute(PresenterThread &thread, RecompD3dPresenter *backend,
    const D3dCapturePacket &packet)
{
    for (size_t i = 0; i < packet.count(); ++i) {
        if (status(thread) != RECOMP_D3D_PRESENTER_OK) break;
        const auto &record = packet.record(i);
        RecompD3dPresenterError error = RECOMP_D3D_PRESENTER_OK;
        bool draw = false;
        try {
            switch (record.kind) {
            case D3dCapturePacket::COMMAND: {
                const auto &command = packet.command(i);
                draw = command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW;
                if (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE) {
                    bool covered = true;
                    error = d3d_backend_draw_coverage(backend, &command.data.draw, &covered);
                    std::lock_guard<std::mutex> lock(thread.mutex);
                    thread.covered = covered;
                } else error = d3d_backend_submit(backend, &command);
                break;
            }
            case D3dCapturePacket::RELEASE:
            case D3dCapturePacket::INVALIDATE:
                error = d3d_backend_release_memory(backend, record.base, record.size,
                    record.kind == D3dCapturePacket::RELEASE);
                break;
            case D3dCapturePacket::REPORT:
                d3d_backend_report_draw_textures();
                break;
            }
        } catch (const std::bad_alloc &) {
            error = RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
        } catch (...) {
            error = RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        if (draw && error != RECOMP_D3D_PRESENTER_OK &&
            error != RECOMP_D3D_PRESENTER_CLOSED) {
            const unsigned declines = ++thread.draw_declines;
            // Report while running; the shutdown total is lost when a probe is killed.
            if ((declines & (declines - 1u)) == 0u) {
                const auto &d = packet.command(i).data.draw;
                std::fprintf(stderr,
                    "recomp d3d presenter: draw declines=%u error=%d fvf=0x%08X"
                    " program=%u prim=%u target=0x%08X fmt=0x%02X %ux%u"
                    " texture=%d fmt=0x%02X\n", declines, static_cast<int>(error),
                    d.fvf, d.program_count, d.primitive_type, d.target.color.data,
                    d.target.color.format_byte, d.target.color.width,
                    d.target.color.height, d.has_texture ? 1 : 0, d.texture.format_byte);
            }
        } else {
            if (error != RECOMP_D3D_PRESENTER_OK &&
                status(thread) == RECOMP_D3D_PRESENTER_OK) {
                std::fprintf(stderr,
                    "recomp d3d presenter: thread failed error=%d kind=%d"
                    " command=%d\n", static_cast<int>(error),
                    static_cast<int>(record.kind),
                    record.kind == D3dCapturePacket::COMMAND
                        ? static_cast<int>(packet.command(i).type) : -1);
            }
            fail(thread, error);
        }
    }
}

// Publishes every resolved query; returns true while one is still pending.
bool resolveVisibility(PresenterThread &thread, RecompD3dPresenter *backend)
{
    for (;;) {
        uint32_t index = 0u, pixels = 0u;
        const auto state = d3d_backend_poll_visibility(backend, &index, &pixels);
        if (state == D3D_BACKEND_VISIBILITY_FAILED) {
            fail(thread, RECOMP_D3D_PRESENTER_HOST_FAILURE);
            return false;
        }
        if (state != D3D_BACKEND_VISIBILITY_READY) return state == D3D_BACKEND_VISIBILITY_PENDING;
        std::lock_guard<std::mutex> lock(thread.mutex);
        const auto found = thread.visibility.find(index);
        if (found != thread.visibility.end() && found->second.pending != 0u) {
            found->second.pixels = pixels;
            --found->second.pending;
        }
    }
}

void run(PresenterThread &thread, RecompD3dPresenterConfig config)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    RecompD3dPresenter *backend = nullptr;
    RecompD3dPresenterError created;
    try {
        created = d3d_backend_create(&config, &backend);
    } catch (const std::bad_alloc &) {
        created = RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    } catch (...) {
        created = RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        thread.created = created;
        thread.status = created;
        thread.started = true;
    }
    thread.changed.notify_all();
    if (created != RECOMP_D3D_PRESENTER_OK) return;

    bool querying = false;
    for (;;) {
        pump(thread);
        D3dCapturePacket *packet = nullptr;
        {
            std::lock_guard<std::mutex> lock(thread.mutex);
            if (thread.pending != 0) packet = &thread.packets[thread.front];
            else if (thread.shutdown) break;
        }
        if (packet == nullptr) {
            // Resolve queries off the GPU while idle, polling until none remain.
            if (querying) querying = resolveVisibility(thread, backend);
            if (MsgWaitForMultipleObjects(1, &thread.wake, FALSE,
                    querying ? 1u : INFINITE, QS_ALLINPUT) == WAIT_FAILED) {
                fail(thread, RECOMP_D3D_PRESENTER_HOST_FAILURE);
            }
            continue;
        }
        execute(thread, backend, *packet);
        packet->clear();
        querying = resolveVisibility(thread, backend);
        {
            std::lock_guard<std::mutex> lock(thread.mutex);
            thread.front = (thread.front + 1) % 3;
            --thread.pending;
        }
        thread.changed.notify_all();
    }
    thread.destroyed = d3d_backend_destroy(&backend);
    std::fprintf(stderr, "recomp d3d presenter thread: draw declines=%u\n",
        thread.draw_declines);
}

double clock_ms()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Separates a slow game frame (tick_max_ms high) from a render thread that
   cannot keep up (queue_wait_ms high). */
void notePacing(PresenterThread &thread, double wait_start_ms, double wait_end_ms)
{
    if (thread.last_frame_ms != 0.0) {
        const double frame_ms = wait_start_ms - thread.last_frame_ms;
        const double busy_ms = (std::max)(0.0, frame_ms - recomp_d3d_vblank_wait_ms);
        thread.frame_max_ms = (std::max)(thread.frame_max_ms, frame_ms);
        thread.busy_max_ms = (std::max)(thread.busy_max_ms, busy_ms);
        thread.busy_total_ms += busy_ms;
        ++thread.frames;
    }
    recomp_d3d_vblank_wait_ms = 0.0;
    thread.last_frame_ms = wait_end_ms;
    thread.queue_wait_ms += wait_end_ms - wait_start_ms;
    const ULONGLONG now = GetTickCount64();
    if (thread.pacing_start == 0u) thread.pacing_start = now;
    if (now - thread.pacing_start < 1000u) return;
    std::fprintf(stderr, "recomp pacing: tick_ms=%llu tick_max_ms=%.1f queue_wait_ms=%.1f "
        "busy_avg_ms=%.1f busy_max_ms=%.1f\n",
        static_cast<unsigned long long>(now), thread.frame_max_ms, thread.queue_wait_ms,
        thread.frames != 0u ? thread.busy_total_ms / thread.frames : 0.0, thread.busy_max_ms);
    thread.pacing_start = now;
    thread.frame_max_ms = 0.0;
    thread.busy_max_ms = 0.0;
    thread.busy_total_ms = 0.0;
    thread.frames = 0u;
    thread.queue_wait_ms = 0.0;
}

RecompD3dPresenterError publish(PresenterThread &thread, bool destroying)
{
    const double wait_start_ms = thread.pacing ? clock_ms() : 0.0;
    std::unique_lock<std::mutex> lock(thread.mutex);
    thread.changed.wait(lock, [&] {
        return thread.pending < 2 ||
            (!destroying && thread.status != RECOMP_D3D_PRESENTER_OK);
    });
    if (thread.pacing) notePacing(thread, wait_start_ms, clock_ms());
    if (!destroying && thread.status != RECOMP_D3D_PRESENTER_OK) return thread.status;
    thread.packets[thread.open].seal();
    thread.open = (thread.open + 1) % 3;
    ++thread.pending;
    lock.unlock();
    SetEvent(thread.wake);
    return RECOMP_D3D_PRESENTER_OK;
}

} // namespace

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr || config->width == 0 ||
        config->height == 0 ||
        config->color_format != RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM ||
        config->depth_format != RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != nullptr || active_thread != nullptr) {
        return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    }
    std::unique_ptr<PresenterThread> thread;
    try {
        thread = std::make_unique<PresenterThread>();
        const char *performance = std::getenv("RECOMP_PERF_COUNTER");
        thread->pacing = performance != nullptr && std::strcmp(performance, "1") == 0;
        thread->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (thread->wake == nullptr) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        thread->worker = std::thread(run, std::ref(*thread), *config);
    } catch (const std::bad_alloc &) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    } catch (const std::system_error &) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    std::unique_lock<std::mutex> lock(thread->mutex);
    thread->changed.wait(lock, [&] { return thread->started; });
    const auto error = thread->created;
    lock.unlock();
    if (error != RECOMP_D3D_PRESENTER_OK) {
        thread->worker.join();
        return error;
    }
    active_thread = thread.release();
    *presenter = reinterpret_cast<RecompD3dPresenter *>(active_thread);
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    error = status(thread);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (command == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END) {
        // Counted before the worker can resolve it, so a read never sees stale completion.
        try {
            std::lock_guard<std::mutex> lock(thread.mutex);
            ++thread.visibility[command->data.visibility_index].pending;
        } catch (const std::bad_alloc &) {
            return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
        }
        error = thread.packets[thread.open].add(*command);
        if (error != RECOMP_D3D_PRESENTER_OK) {
            std::lock_guard<std::mutex> lock(thread.mutex);
            --thread.visibility[command->data.visibility_index].pending;
        }
        return error;
    }
    error = thread.packets[thread.open].add(*command);
    if (thread.pacing) ++recomp_d3d_submit_count;
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (thread.pacing && command->type == RECOMP_D3D_PRESENTER_COMMAND_PRESENT)
        recomp_d3d_submit_mb = thread.packets[thread.open].bytes() / 1048576.0;
    return command->type == RECOMP_D3D_PRESENTER_COMMAND_PRESENT
        ? publish(thread, false) : RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    error = status(thread);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    return thread.packets[thread.open].addRelease(base, size);
}

RecompD3dPresenterError recomp_d3d_presenter_invalidate_textures(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    error = status(thread);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    return thread.packets[thread.open].addInvalidate(base, size);
}

RecompD3dPresenterError recomp_d3d_presenter_flush(RecompD3dPresenter *presenter, bool wait)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    error = status(thread);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (thread.packets[thread.open].count() != 0u) {
        error = publish(thread, false);
        if (error != RECOMP_D3D_PRESENTER_OK) return error;
    }
    if (!wait) return RECOMP_D3D_PRESENTER_OK;
    std::unique_lock<std::mutex> lock(thread.mutex);
    thread.changed.wait(lock, [&] { return thread.pending == 0u ||
        thread.status != RECOMP_D3D_PRESENTER_OK; });
    return thread.status;
}

RecompD3dPresenterError recomp_d3d_presenter_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (draw == nullptr || covered == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE;
    command.data.draw = *draw;
    error = recomp_d3d_presenter_submit(presenter, &command);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    error = recomp_d3d_presenter_flush(presenter, true);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    std::lock_guard<std::mutex> lock(active_thread->mutex);
    *covered = active_thread->covered;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_visibility_result(
    RecompD3dPresenter *presenter, uint32_t index, bool *complete, uint32_t *pixels)
{
    const auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (complete == nullptr || pixels == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    auto &thread = *active_thread;
    std::lock_guard<std::mutex> lock(thread.mutex);
    if (thread.status != RECOMP_D3D_PRESENTER_OK) return thread.status;
    const auto found = thread.visibility.find(index);
    if (found == thread.visibility.end()) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    *complete = found->second.pending == 0u;
    *pixels = found->second.pixels;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_destroy(RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const auto error = validate(*presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    if (thread.packets[thread.open].count() != 0) publish(thread, true);
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        thread.shutdown = true;
    }
    SetEvent(thread.wake);
    thread.worker.join();
    // A close already reached the owner through PRESENT; report other pending work errors.
    const auto pending = thread.status == RECOMP_D3D_PRESENTER_CLOSED
        ? RECOMP_D3D_PRESENTER_OK : thread.status;
    const auto destroyed = thread.destroyed != RECOMP_D3D_PRESENTER_OK
        ? thread.destroyed : pending;
    delete active_thread;
    active_thread = nullptr;
    *presenter = nullptr;
    return destroyed;
}

void recomp_d3d_presenter_set_immediate_present(bool enabled)
{
    d3d_backend_set_immediate_present(enabled);
}

void recomp_d3d_presenter_report_draw_textures()
{
    if (active_thread == nullptr || GetCurrentThreadId() != active_thread->owner_thread) return;
    auto &thread = *active_thread;
    if (status(thread) == RECOMP_D3D_PRESENTER_OK) {
        fail(thread, thread.packets[thread.open].addReport());
    }
}
