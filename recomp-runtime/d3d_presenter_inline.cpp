// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_presenter_backend.h"

#include <new>
#include <thread>
#include <unordered_map>

/* The presenter API run on the calling thread. SDL creates its window, takes
   its swapchain and pumps its events on one thread, the main thread on macOS,
   so the SDL_GPU backend cannot move to a worker as Direct3D 11 does
   (d3d_presenter_thread.cpp). The backend records commands as they arrive
   and submits them in batches, so the guest only waits for the GPU at a
   present or when it asks for a visibility result. */

namespace {

struct InlinePresenter {
    RecompD3dPresenter *backend = nullptr;
    std::thread::id owner = std::this_thread::get_id();
    // Guest visibility indices: ENDs submitted but unresolved, and the latest count.
    struct Visibility { unsigned pending = 0; uint32_t pixels = 0; };
    std::unordered_map<uint32_t, Visibility> visibility;
};

InlinePresenter *active;

RecompD3dPresenterError validate(RecompD3dPresenter *presenter)
{
    if (presenter == nullptr || presenter != reinterpret_cast<RecompD3dPresenter *>(active)) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    return std::this_thread::get_id() == active->owner
        ? RECOMP_D3D_PRESENTER_OK : RECOMP_D3D_PRESENTER_WRONG_THREAD;
}

// Publishes every resolved query; a failed poll fails the presenter.
RecompD3dPresenterError resolveVisibility(InlinePresenter &state)
{
    for (;;) {
        uint32_t index = 0u, pixels = 0u;
        const auto poll = d3d_backend_poll_visibility(state.backend, &index, &pixels);
        if (poll == D3D_BACKEND_VISIBILITY_FAILED) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        if (poll != D3D_BACKEND_VISIBILITY_READY) return RECOMP_D3D_PRESENTER_OK;
        const auto found = state.visibility.find(index);
        if (found != state.visibility.end() && found->second.pending != 0u) {
            found->second.pixels = pixels;
            --found->second.pending;
        }
    }
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
    if (*presenter != nullptr || active != nullptr) {
        return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    }
    InlinePresenter *created = new (std::nothrow) InlinePresenter;
    if (created == nullptr) return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    const auto error = d3d_backend_create(config, &created->backend);
    if (error != RECOMP_D3D_PRESENTER_OK) {
        delete created;
        return error;
    }
    active = created;
    *presenter = reinterpret_cast<RecompD3dPresenter *>(active);
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    // The backend reports a closed window before it checks the command.
    if (command == nullptr) return d3d_backend_submit(active->backend, command);
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END) {
        try {
            ++active->visibility[command->data.visibility_index].pending;
        } catch (const std::bad_alloc &) {
            return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
        }
    }
    try {
        error = d3d_backend_submit(active->backend, command);
    } catch (const std::bad_alloc &) {
        error = RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END &&
        error != RECOMP_D3D_PRESENTER_OK) {
        --active->visibility[command->data.visibility_index].pending;
    }
    return error;
}

RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    const auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    return d3d_backend_release_memory(active->backend, base, size, true);
}

RecompD3dPresenterError recomp_d3d_presenter_invalidate_textures(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    const auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    return d3d_backend_release_memory(active->backend, base, size, false);
}

/* Commands have already reached the backend; nothing is held here to publish. */
RecompD3dPresenterError recomp_d3d_presenter_flush(RecompD3dPresenter *presenter, bool wait)
{
    (void)wait;
    return validate(presenter);
}

bool recomp_d3d_presenter_present_ready(RecompD3dPresenter *presenter)
{
    return validate(presenter) != RECOMP_D3D_PRESENTER_OK || d3d_backend_present_ready(active->backend);
}

RecompD3dPresenterError recomp_d3d_presenter_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered)
{
    const auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (draw == nullptr || covered == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    return d3d_backend_draw_coverage(active->backend, draw, covered);
}

RecompD3dPresenterError recomp_d3d_presenter_visibility_result(
    RecompD3dPresenter *presenter, uint32_t index, bool *complete, uint32_t *pixels)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (complete == nullptr || pixels == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    error = resolveVisibility(*active);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    const auto found = active->visibility.find(index);
    if (found == active->visibility.end()) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    *complete = found->second.pending == 0u;
    *pixels = found->second.pixels;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_destroy(RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const auto error = validate(*presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    const auto destroyed = d3d_backend_destroy(&active->backend);
    delete active;
    active = nullptr;
    *presenter = nullptr;
    return destroyed;
}

void recomp_d3d_presenter_set_immediate_present(bool enabled)
{
    d3d_backend_set_immediate_present(enabled);
}

void recomp_d3d_presenter_report_draw_textures()
{
    if (active == nullptr || std::this_thread::get_id() != active->owner) return;
    d3d_backend_report_draw_textures();
}
