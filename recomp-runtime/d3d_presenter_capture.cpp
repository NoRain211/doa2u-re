#include "d3d_presenter_capture.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>

namespace {

using Draw = RecompD3dPresenterDrawCommand;
constexpr const void *Draw::*pointer_fields[] = {
    &Draw::vertex_bytes, &Draw::index_bytes, &Draw::texture_bytes,
    &Draw::palette_bytes, &Draw::alpha_mask_bytes,
    &Draw::alpha_mask_palette, &Draw::reflection_bytes, &Draw::reflection_stage2_bytes,
};
// No readable guest span can exceed the runner's entire 64 MiB RAM region.
constexpr uint64_t max_span_bytes = 64u * 1024u * 1024u;
constexpr size_t null_offset = SIZE_MAX;

// One pointer reused for thousands of small draws (rain) made a pointer-only key quadratic.
uint64_t spanKey(const void *source, size_t size)
{
    uint64_t key = (reinterpret_cast<uintptr_t>(source) * 0x9e3779b97f4a7c15ull) ^ size;
    const auto *bytes = static_cast<const unsigned char *>(source);
    for (size_t i = 0u; size != 0u && i < 8u; ++i) {
        uint64_t word = 0u;
        std::memcpy(&word, bytes + (size <= 8u ? 0u : (size - 8u) * i / 7u), size < 8u ? size : 8u);
        key = (key ^ word) * 0x100000001b3ull;
    }
    return key;
}

// spanKey's multiplies only mix upward; fold the high bits into the slot index.
size_t spanSlot(uint64_t key, size_t mask)
{
    return static_cast<size_t>(key ^ (key >> 32)) & mask;
}

// Resident texture samples, as d3d_presenter.h's textureFingerprint.
constexpr uint32_t kResidentSamples = 128u;

void copyRange(Draw &to, const Draw &from, size_t begin, size_t end)
{
    std::memcpy(reinterpret_cast<char *>(&to) + begin, reinterpret_cast<const char *>(&from) + begin, end - begin);
}

/* Fixed-function draws copy only the sections the presenter reads under their
   flags; skipped bytes stay uninitialized. Keep in step with d3d_presenter.h. */
void copyCommand(RecompD3dPresenterCommand &to, const RecompD3dPresenterCommand &from)
{
    if ((from.type != RECOMP_D3D_PRESENTER_COMMAND_DRAW &&
         from.type != RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE) || from.data.draw.program_count != 0u) {
        to = from;
        return;
    }
    using Lighting = RecompD3dDirectionalLighting;
    const Draw &draw = from.data.draw;
    Draw &out = to.data.draw;
    to.type = from.type;
    constexpr size_t lighting = offsetof(Draw, directional);
    copyRange(out, draw, 0u, offsetof(Draw, program));
    copyRange(out, draw, offsetof(Draw, program_alpha_mask), lighting + (draw.directional.enabled
        ? sizeof(Lighting) : offsetof(Lighting, normal_transforms)));
    // Unlit stage_mode draws still read the stage selection.
    if (!draw.directional.enabled) std::memcpy(out.directional.stage, draw.directional.stage, sizeof draw.directional.stage);
    copyRange(out, draw, lighting + sizeof(Lighting), offsetof(Draw, reflection_world_view));
    if (draw.has_reflection || draw.texgen_reflection || draw.texture_transform_uv || draw.texgen_projected)
        copyRange(out, draw, offsetof(Draw, reflection_world_view), offsetof(Draw, reflection_stage2));
    copyRange(out, draw, offsetof(Draw, reflection_stage2), sizeof(Draw));
}

} // namespace

RecompD3dPresenterError D3dCapturePacket::add(
    const RecompD3dPresenterCommand &command)
{
    if (sealed_ || static_cast<unsigned>(command.type) >
        RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }

    uint64_t sizes[8]{};
    bool ineligible[8]{};
    if (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW ||
        command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE) {
        const auto &draw = command.data.draw;
        sizes[0] = uint64_t(draw.vertex_count) * draw.vertex_stride;
        sizes[1] = uint64_t(draw.index_count) * 2u;
        if (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW) {
            sizes[2] = draw.texture_byte_count;
            sizes[3] = draw.palette_byte_count;
            sizes[4] = draw.alpha_mask_byte_count;
            sizes[5] = draw.alpha_mask_palette_byte_count;
            sizes[6] = draw.reflection_byte_count;
            sizes[7] = draw.reflection_stage2_byte_count;
            // Linear surfaces and palettes are rewritten in place: a linear
            // surface refreshes from its pointer on every bind, and a draw
            // names no palette address to drop when that resource is locked.
            ineligible[2] = draw.texture.linear;
            ineligible[3] = ineligible[5] = true;
            ineligible[4] = draw.alpha_mask.linear;
            ineligible[6] = draw.reflection_texture.linear;
            ineligible[7] = draw.reflection_stage2_texture.linear;
        }
        for (uint64_t size : sizes) {
            if (size > max_span_bytes) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
        }
    }

    // A fill that opens after a present may replace standing texture data; a
    // fill in progress must keep every byte its queued draws already name.
    if (commands_.empty() && (resident_bytes_.size() > kResidentByteLimit / 8u ||
            resident_live_ >= kResidentSpanLimit ||
            resident_.size() > resident_live_ * 2u + 64u)) {
        clearResident();
    }

    const size_t old_payload_size = payload_.size();
    const size_t old_command_count = commands_.size();
    try {
        CapturedCommand &captured = commands_.emplace_back();
        captured.resident_mask = 0u;
        copyCommand(captured.value, command);
        if (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW ||
            command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE) {
            for (size_t i = 0u; i < 8u; ++i) {
                const void *source = command.data.draw.*pointer_fields[i];
                captured.value.data.draw.*pointer_fields[i] = nullptr;
                captured.offsets[i] = null_offset;
                if (source == nullptr || (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE && i >= 2u)) continue;

                const size_t size = static_cast<size_t>(sizes[i]);
                if (i >= 2u && !ineligible[i]) {
                    const uint32_t address = i == 2u ? command.data.draw.texture.data
                        : i == 4u ? command.data.draw.alpha_mask.data
                        : i == 6u ? command.data.draw.reflection_texture.data
                        : command.data.draw.reflection_stage2_texture.data;
                    const size_t reused = residentReuse(residentKey(address, source, size),
                        address, source, size);
                    if (reused != null_offset) {
                        captured.offsets[i] = reused;
                        captured.resident_mask |= static_cast<uint8_t>(1u << i);
                        continue;
                    }
                }
                const uint64_t key = spanKey(source, size);
                if (!spans_.empty()) {
                    const size_t mask = spans_.size() - 1u;
                    for (size_t slot = spanSlot(key, mask); spans_[slot].offset != null_offset; slot = (slot + 1u) & mask) {
                        const Span &span = spans_[slot];
                        if (span.key == key && span.size == size && (size == 0u ||
                            std::memcmp(source, payload_.data() + span.offset, size) == 0)) {
                            captured.offsets[i] = span.offset;
                            break;
                        }
                    }
                }
                if (captured.offsets[i] != null_offset) continue;

                const size_t offset = payload_.size();
                // Preserve non-null empty spans without reading their source.
                const size_t words = size == 0u ? 1u : (size + 7u) / 8u;
                if (words > payload_.max_size() - offset) throw std::length_error("capture payload");
                payload_.resize(offset + words);
                // The allocator leaves new words uninitialized; define the tail before it can be relocated.
                payload_[offset + words - 1u] = 0u;
                if (size != 0u) std::memcpy(payload_.data() + offset, source, size);
                insertSpan(key, size, offset);
                captured.offsets[i] = offset;
            }
        }
        entries_.push_back({{COMMAND, 0u, 0u}, old_command_count});
        return RECOMP_D3D_PRESENTER_OK;
    } catch (const std::bad_alloc &) {
    } catch (const std::length_error &) {
    }

    // Forgetting every span only costs later reuse; none may point past the rolled-back payload.
    clearSpans();
    commands_.resize(old_command_count);
    payload_.resize(old_payload_size);
    return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
}

void D3dCapturePacket::insertSpan(uint64_t key, size_t size, size_t offset)
{
    if ((span_count_ + 1u) * 2u > spans_.size()) {
        std::vector<Span> grown(spans_.empty() ? 1024u : spans_.size() * 2u, Span{0u, 0u, null_offset});
        const size_t mask = grown.size() - 1u;
        for (const Span &span : spans_) {
            if (span.offset == null_offset) continue;
            size_t slot = spanSlot(span.key, mask);
            while (grown[slot].offset != null_offset) slot = (slot + 1u) & mask;
            grown[slot] = span;
        }
        spans_.swap(grown);
    }
    const size_t mask = spans_.size() - 1u;
    size_t slot = spanSlot(key, mask);
    while (spans_[slot].offset != null_offset) slot = (slot + 1u) & mask;
    spans_[slot] = Span{key, size, offset};
    ++span_count_;
}

void D3dCapturePacket::clearSpans()
{
    if (span_count_ == 0u) return;
    std::fill(spans_.begin(), spans_.end(), Span{0u, 0u, null_offset});
    span_count_ = 0u;
}

/* The key names the guest span, its length and sampled contents. A rewrite that
   leaves every sampled word intact is missed, the same gamble the presenter's
   texture fingerprint takes; lock and Register wrappers normally invalidate it. */
uint64_t D3dCapturePacket::residentKey(uint32_t address, const void *bytes, size_t size) const
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ (static_cast<uint64_t>(address) << 32) ^ size;
    if (size <= 1024u) {
        for (size_t i = 0u; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
        return hash;
    }
    for (uint32_t i = 0u; i < kResidentSamples; ++i) {
        uint64_t word;
        std::memcpy(&word, data + static_cast<uint64_t>(size - 8u) * i / (kResidentSamples - 1u), 8u);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

// Returns the standing copy's word offset, storing it first when it is new.
size_t D3dCapturePacket::residentReuse(uint64_t key, uint32_t address, const void *bytes, size_t size)
{
    const auto found = resident_index_.find(key);
    if (found != resident_index_.end() && resident_[found->second].live &&
        resident_[found->second].address == address && resident_[found->second].size == size) {
        return resident_[found->second].offset;
    }
    if (resident_live_ >= kResidentSpanLimit) return null_offset;
    const size_t offset = resident_used_;
    const size_t words = size == 0u ? 1u : (size + 7u) / 8u;
    if (words > resident_bytes_.max_size() - offset ||
        (offset + words) * 8u > kResidentByteLimit) {
        return null_offset;
    }
    bool stored = false;
    const size_t old_resident = resident_.size();
    try {
        resident_bytes_.resize(offset + words);
        resident_bytes_[offset + words - 1u] = 0u;
        if (size != 0u) std::memcpy(resident_bytes_.data() + offset, bytes, size);
        resident_.push_back({key, address, static_cast<uint32_t>(size), offset, true});
        resident_index_[key] = resident_.size() - 1u;
        stored = true;
    } catch (const std::bad_alloc &) {
    } catch (const std::length_error &) {
    }
    if (!stored) {
        resident_.resize(old_resident);
        resident_bytes_.resize(offset);
        return null_offset;
    }
    resident_used_ += words;
    resident_live_ += 1u;
    resident_added_ += words;
    return offset;
}

/* A lock or Register covers [base, base + size): its standing copies are stale.
   Bytes already queued for this fill stay valid; the arena is append-only. */
void D3dCapturePacket::dropResident(uint32_t base, uint32_t size)
{
    // A release may name the cached alias the presenter folds in d3d_backend_release_memory.
    if (base >= 0x80000000u && base < 0x84000000u) base -= 0x80000000u;
    const uint64_t first = base, last = uint64_t(base) + size;
    for (size_t i = 0u; i < resident_.size(); ++i) {
        Resident &entry = resident_[i];
        if (!entry.live) continue;
        const uint64_t span_last = uint64_t(entry.address) + entry.size;
        if (entry.address >= last || first >= span_last) continue;
        entry.live = false;
        const auto found = resident_index_.find(entry.key);
        // A newer copy of the same span can own the key; leave that one standing.
        if (found != resident_index_.end() && found->second == i) resident_index_.erase(found);
        --resident_live_;
    }
}

void D3dCapturePacket::clearResident()
{
    resident_.clear();
    resident_index_.clear();
    resident_bytes_.clear();
    resident_used_ = 0u;
    resident_live_ = 0u;
}

RecompD3dPresenterError D3dCapturePacket::addRecord(
    Kind kind, uint32_t base, uint32_t size)
{
    if (sealed_) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    try {
        entries_.push_back({{kind, base, size}, 0u});
        return RECOMP_D3D_PRESENTER_OK;
    } catch (const std::bad_alloc &) {
    } catch (const std::length_error &) {
    }
    return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
}

RecompD3dPresenterError D3dCapturePacket::addRelease(uint32_t base, uint32_t size)
{
    dropResident(base, size);
    return addRecord(RELEASE, base, size);
}

RecompD3dPresenterError D3dCapturePacket::addInvalidate(uint32_t base, uint32_t size)
{
    dropResident(base, size);
    return addRecord(INVALIDATE, base, size);
}

RecompD3dPresenterError D3dCapturePacket::addReport()
{
    return addRecord(REPORT, 0u, 0u);
}

void D3dCapturePacket::seal()
{
    if (sealed_) return;
    for (auto &captured : commands_) {
        if (captured.value.type != RECOMP_D3D_PRESENTER_COMMAND_DRAW &&
            captured.value.type != RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE) continue;
        for (size_t i = 0u; i < 8u; ++i) {
            const bool resident = (captured.resident_mask & (1u << i)) != 0u;
            const uint64_t *words = resident ? resident_bytes_.data() : payload_.data();
            captured.value.data.draw.*pointer_fields[i] = captured.offsets[i] == null_offset
                ? nullptr : words + captured.offsets[i];
        }
    }
    sealed_ = true;
}

size_t D3dCapturePacket::count() const
{
    return entries_.size();
}

const D3dCapturePacket::Record &D3dCapturePacket::record(size_t index) const
{
    return entries_[index].record;
}

const RecompD3dPresenterCommand &D3dCapturePacket::command(size_t index) const
{
    assert(sealed_ && entries_[index].record.kind == COMMAND);
    return commands_[entries_[index].command_index].value;
}

void D3dCapturePacket::clear()
{
    entries_.clear();
    commands_.clear();
    payload_.clear();
    clearSpans();
    // Standing texture bytes and their table outlive the fill that copied them.
    resident_added_ = 0u;
    sealed_ = false;
}

uint64_t D3dCapturePacket::bytes() const
{
    return uint64_t(commands_.size()) * sizeof(RecompD3dPresenterCommand) +
        uint64_t(entries_.size()) * sizeof(Record) + uint64_t(payload_.size()) * sizeof(uint64_t) +
        uint64_t(resident_added_) * sizeof(uint64_t);
}
