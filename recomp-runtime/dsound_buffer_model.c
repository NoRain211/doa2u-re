#include "dsound_buffer_model.h"

#include <stddef.h>

static int buffer_configured(const RecompDsoundBufferModel *model)
{
    return model != NULL && model->size_bytes != 0u &&
        model->sample_rate != 0u && model->block_align != 0u &&
        model->size_bytes % model->block_align == 0u &&
        model->loop_start_bytes < model->loop_end_bytes &&
        model->loop_end_bytes <= model->size_bytes &&
        model->loop_end_bytes % model->block_align == 0u &&
        model->loop_start_bytes % model->block_align == 0u;
}

uint32_t recomp_dsound_buffer_configure(
    RecompDsoundBufferModel *model, uint32_t size_bytes,
    uint32_t sample_rate, uint32_t block_align, uint64_t now_ms)
{
    if (model == NULL || size_bytes == 0u || sample_rate == 0u ||
        block_align == 0u || size_bytes % block_align != 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    *model = (RecompDsoundBufferModel){
        .size_bytes = size_bytes,
        .sample_rate = sample_rate,
        .original_sample_rate = sample_rate,
        .block_align = block_align,
        .last_ms = now_ms,
        .loop_end_bytes = size_bytes,
    };
    return RECOMP_DSOUND_OK;
}

uint32_t recomp_dsound_buffer_cursor(
    RecompDsoundBufferModel *model, uint64_t now_ms)
{
    if (!buffer_configured(model)) {
        return 0u;
    }
    if ((model->play_flags & RECOMP_DSOUND_PLAY_LOOPING) &&
        model->cursor_bytes >= model->loop_end_bytes) {
        model->cursor_bytes = model->loop_start_bytes +
            (model->cursor_bytes - model->loop_end_bytes) %
            (model->loop_end_bytes - model->loop_start_bytes);
    }
    if (!model->playing || now_ms <= model->last_ms) {
        return model->cursor_bytes;
    }
    uint64_t elapsed = now_ms - model->last_ms;
    uint64_t seconds = elapsed / 1000u;
    uint64_t fraction =
        (elapsed % 1000u) * model->sample_rate + model->frame_remainder;
    uint64_t frames = ((model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)
        ? model->loop_end_bytes : model->size_bytes) / model->block_align;
    uint64_t position = model->cursor_bytes / model->block_align;
    model->last_ms = now_ms;
    model->frame_remainder = (uint32_t)(fraction % 1000u);
    uint64_t remaining = frames - position;
    if (seconds <= remaining / model->sample_rate &&
        seconds * model->sample_rate + fraction / 1000u < remaining) {
        model->cursor_bytes = (uint32_t)((position + seconds * model->sample_rate +
            fraction / 1000u) * model->block_align);
        return model->cursor_bytes;
    }
    if (!(model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
        model->playing = 0u;
        model->cursor_bytes = 0u;
        model->frame_remainder = 0u;
        return 0u;
    }
    uint64_t loop_start = model->loop_start_bytes / model->block_align;
    uint64_t loop_frames = frames - loop_start;
    /* Subtract the first passage to the end before wrapping within the tail.
       Reduce before multiplying so even a uint64_t elapsed time cannot overflow. */
    uint64_t advance =
        ((seconds % loop_frames) * model->sample_rate) % loop_frames + fraction / 1000u;
    model->cursor_bytes = (uint32_t)((loop_start +
        (advance + loop_frames - remaining % loop_frames) % loop_frames) *
        model->block_align);
    return model->cursor_bytes;
}

uint32_t recomp_dsound_buffer_consume(
    RecompDsoundBufferModel *model, uint64_t now_ms, uint32_t *offset_bytes)
{
    if (!buffer_configured(model) || offset_bytes == NULL ||
        !model->playing || now_ms <= model->last_ms) {
        return 0u;
    }
    uint64_t elapsed = now_ms - model->last_ms;
    uint32_t position = recomp_dsound_buffer_cursor(model, model->last_ms);
    uint32_t remainder = model->frame_remainder;
    recomp_dsound_buffer_cursor(model, now_ms);
    /* A stall over 100 ms discards its span instead of replaying stale ring data;
       the output pump thread keeps ordinary spans near 10 ms. */
    if (elapsed > 100u) {
        return 0u;
    }
    uint64_t frames = (elapsed * model->sample_rate + remainder) / 1000u;
    if (!(model->play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
        uint32_t remaining = (model->size_bytes - position) / model->block_align;
        if (frames > remaining) {
            frames = remaining;
        }
    }
    if (frames > UINT32_MAX / model->block_align) {
        return 0u;
    }
    *offset_bytes = position;
    return (uint32_t)frames * model->block_align;
}

uint32_t recomp_dsound_buffer_play(
    RecompDsoundBufferModel *model, uint32_t flags, uint64_t now_ms)
{
    if (!buffer_configured(model) ||
        (flags & ~(RECOMP_DSOUND_PLAY_LOOPING | RECOMP_DSOUND_PLAY_FROMSTART))) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    if (flags & RECOMP_DSOUND_PLAY_FROMSTART) {
        model->cursor_bytes = 0u;
        model->frame_remainder = 0u;
    }
    if ((flags & RECOMP_DSOUND_PLAY_LOOPING) &&
        model->cursor_bytes >= model->loop_end_bytes) {
        model->cursor_bytes = model->loop_start_bytes;
    }
    model->play_flags = flags;
    model->playing = 1u;
    if (now_ms > model->last_ms) {
        model->last_ms = now_ms;
    }
    return RECOMP_DSOUND_OK;
}

void recomp_dsound_buffer_stop(
    RecompDsoundBufferModel *model, uint64_t now_ms)
{
    if (model != NULL) {
        recomp_dsound_buffer_cursor(model, now_ms);
        model->playing = 0u;
    }
}

uint32_t recomp_dsound_buffer_set_position(
    RecompDsoundBufferModel *model, uint32_t position_bytes, uint64_t now_ms)
{
    if (!buffer_configured(model) || position_bytes >= model->size_bytes ||
        position_bytes % model->block_align != 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    model->cursor_bytes = position_bytes;
    model->frame_remainder = 0u;
    return RECOMP_DSOUND_OK;
}

uint32_t recomp_dsound_buffer_set_frequency(
    RecompDsoundBufferModel *model, uint32_t sample_rate, uint64_t now_ms)
{
    if (!buffer_configured(model)) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    if (sample_rate == 0u) {
        sample_rate = model->original_sample_rate;
    }
    if (sample_rate == 0u) {
        return RECOMP_DSOUND_INVALID_PARAM;
    }
    recomp_dsound_buffer_cursor(model, now_ms);
    model->sample_rate = sample_rate;
    return RECOMP_DSOUND_OK;
}
