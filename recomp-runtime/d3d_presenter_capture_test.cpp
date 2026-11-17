#include "d3d_presenter_capture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL capture line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main()
{
    using Draw = RecompD3dPresenterDrawCommand;
    const void *Draw::*fields[] = {
        &Draw::vertex_bytes, &Draw::index_bytes, &Draw::texture_bytes,
        &Draw::palette_bytes, &Draw::alpha_mask_bytes,
        &Draw::alpha_mask_palette, &Draw::reflection_bytes, &Draw::reflection_stage2_bytes,
    };
    const size_t sizes[] = {33u, 6u, 65u, 1024u, 17u, 1024u, 31u, 16u};
    std::array<std::vector<uint8_t>, 8> sources;
    RecompD3dPresenterCommand input{};
    input.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = input.data.draw;
    draw.vertex_count = 3u;
    draw.vertex_stride = 11u;
    draw.index_count = 3u;
    draw.texture_byte_count = 65u;
    draw.palette_byte_count = 1024u;
    draw.alpha_mask_byte_count = 17u;
    draw.alpha_mask_palette_byte_count = 1024u;
    draw.reflection_byte_count = 31u;
    draw.reflection_stage2_byte_count = 16u;
    draw.texture.data = 0x101000u;
    draw.alpha_mask.data = 0x202000u;
    draw.reflection_texture.data = 0x303000u;
    // Swizzled, P8 and linear uploads all own their queued bytes.
    draw.texture.format_byte = draw.alpha_mask.format_byte =
        draw.reflection_texture.format_byte = 0u;
    draw.alpha_mask.format_byte = 0x0bu;
    draw.reflection_texture.format_byte = 0x12u;
    draw.depth_bias = -1;
    draw.slope_scaled_depth_bias = -0.25f;
    draw.program_count = 1u;
    draw.target.color.data = 0x404000u;
    draw.target.depth.data = 0x505000u;
    draw.program[135][3] = 0x12345678u;
    draw.program_constants[191][3] = 1.25f;
    draw.directional.colors[7][3] = 0.75f;
    for (size_t i = 0u; i < sources.size(); ++i) {
        sources[i].assign(sizes[i], static_cast<uint8_t>(i + 1u));
        draw.*fields[i] = sources[i].data();
    }

    D3dCapturePacket packet;
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    for (size_t i = 0u; i < sources.size(); ++i) {
        std::fill(sources[i].begin(), sources[i].end(), static_cast<uint8_t>(i + 21u));
    }
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addRelease(0x123000u, 0x456u) == RECOMP_D3D_PRESENTER_OK);
    for (auto &source : sources) std::fill(source.begin(), source.end(), 0xeeu);
    packet.seal();
    CHECK(packet.count() == 3u);
    CHECK(packet.record(0).kind == D3dCapturePacket::COMMAND);
    CHECK(packet.record(1).kind == D3dCapturePacket::COMMAND);
    CHECK(packet.record(2).kind == D3dCapturePacket::RELEASE);
    CHECK(packet.record(2).base == 0x123000u && packet.record(2).size == 0x456u);
    for (size_t record = 0u; record < 2u; ++record) {
        const auto &captured = packet.command(record).data.draw;
        CHECK(reinterpret_cast<uintptr_t>(&packet.command(record)) % 8u == 0u);
        for (size_t i = 0u; i < sources.size(); ++i) {
            const auto *bytes = static_cast<const uint8_t *>(captured.*fields[i]);
            CHECK(bytes != sources[i].data());
            CHECK(reinterpret_cast<uintptr_t>(bytes) % 8u == 0u);
            CHECK(std::all_of(bytes, bytes + sizes[i], [=](uint8_t byte) {
                return byte == i + 1u + record * 20u;
            }));
        }
        CHECK(captured.texture.data == draw.texture.data);
        CHECK(captured.alpha_mask.data == draw.alpha_mask.data);
        CHECK(captured.reflection_texture.data == draw.reflection_texture.data);
        CHECK(captured.target.color.data == draw.target.color.data);
        CHECK(captured.target.depth.data == draw.target.depth.data);
        CHECK(captured.program[135][3] == 0x12345678u);
        CHECK(captured.program_constants[191][3] == 1.25f);
        CHECK(captured.directional.colors[7][3] == 0.75f);
        CHECK(captured.depth_bias == -1 && captured.slope_scaled_depth_bias == -0.25f);
    }
    const uint64_t sealed_bytes = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    CHECK(packet.addRelease(0u, 1u) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    CHECK(packet.addReport() == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    packet.seal();
    CHECK(packet.count() == 3u && packet.bytes() == sealed_bytes);

    D3dCapturePacket moved(std::move(packet));
    const void *moved_vertices = moved.command(0).data.draw.vertex_bytes;
    packet.clear();
    packet = std::move(moved);
    CHECK(packet.command(0).data.draw.vertex_bytes == moved_vertices);
    CHECK(*static_cast<const uint8_t *>(moved_vertices) == 1u);
    packet.clear();
    CHECK(packet.count() == 0u && packet.bytes() == 0u);

    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    const uint64_t first_bytes = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    const uint64_t dedup_delta = packet.bytes() - first_bytes;
    CHECK(dedup_delta == sizeof(RecompD3dPresenterCommand) + sizeof(D3dCapturePacket::Record));
    sources[0][0] = 0xabu;
    const uint64_t before_changed = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.bytes() - before_changed > dedup_delta);
    sources[0][0] = 0xeeu;
    const uint64_t before_reverted = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.bytes() - before_reverted == dedup_delta);

    const size_t before_count = packet.count();
    const uint64_t before_bytes = packet.bytes();
    RecompD3dPresenterCommand invalid = input;
    invalid.data.draw.vertex_count = UINT32_MAX;
    invalid.data.draw.vertex_stride = UINT32_MAX;
    CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    invalid = input;
    invalid.data.draw.index_count = UINT32_MAX;
    CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    uint32_t Draw::*counts[] = {
        &Draw::texture_byte_count, &Draw::palette_byte_count,
        &Draw::alpha_mask_byte_count, &Draw::alpha_mask_palette_byte_count,
        &Draw::reflection_byte_count,
    };
    for (auto field : counts) {
        invalid = input;
        invalid.data.draw.*field = UINT32_MAX;
        CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    }
    invalid.type = static_cast<RecompD3dPresenterCommandType>(99);
    CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    CHECK(packet.count() == before_count && packet.bytes() == before_bytes);
    packet.seal();
    CHECK(packet.command(0).data.draw.vertex_bytes == packet.command(1).data.draw.vertex_bytes);
    CHECK(packet.command(0).data.draw.vertex_bytes == packet.command(3).data.draw.vertex_bytes);
    CHECK(*static_cast<const uint8_t *>(packet.command(2).data.draw.vertex_bytes) == 0xabu);
    CHECK(packet.command(0).data.draw.texture_bytes == packet.command(2).data.draw.texture_bytes);

    packet.clear();
    RecompD3dPresenterCommand value{};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR;
    value.data.clear.color = 0xabcdef12u;
    value.data.clear.z = 0.5f;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addReport() == RECOMP_D3D_PRESENTER_OK);
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    value.data.present.swap_counter = 42u;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addRelease(7u, 9u) == RECOMP_D3D_PRESENTER_OK);
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_GAMMA;
    std::memset(value.data.gamma, 0x7au, sizeof value.data.gamma);
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    value.data.draw.texture.format_byte = value.data.draw.alpha_mask.format_byte =
        value.data.draw.reflection_texture.format_byte = 0x12u;
    for (size_t i = 0u; i < sources.size(); ++i) value.data.draw.*fields[i] = sources[i].data();
    // Lit and reflective: the guarded sections are captured.
    value.data.draw.directional.enabled = true;
    value.data.draw.has_reflection = true;
    value.data.draw.reflection_world_view[12] = 3.0f;
    value.data.draw.reflection_blend_normal[2][15] = 4.0f;
    value.data.draw.reflection_stage1_transform[3] = 5.0f;
    value.data.draw.reflection_diffuse[2] = 0.5f;
    value.data.draw.directional.world_transforms[3][14] = 12.0f;
    value.data.draw.directional.positions[7][2] = 30.0f;
    value.data.draw.directional.positions[7][3] = 1.0f;
    value.data.draw.directional.attenuation[7][1] = 0.05f;
    value.data.draw.directional.attenuation[7][3] = 100.0f;
    value.data.draw.directional.ambient[7][0] = 0.25f;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    value.data.draw.directional = {};
    // Unlit stage selection without reflection keeps the stage and flags.
    value.data.draw.directional.stage_mode = true;
    value.data.draw.directional.stage[2] = 1.0f;
    value.data.draw.has_reflection = false;
    value.data.draw.texture.format_byte = 0u; // Swizzled uploads also own their bytes.
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    for (auto field : fields) value.data.draw.*field = nullptr;
    value.data.draw.texture_byte_count = 12u;
    // Projected texgen alone still reads the reflection matrices.
    value.data.draw.texgen_projected = true;
    value.data.draw.reflection_transform[5] = 2.5f;
    value.data.draw.projected_stage_count = 2u;
    value.data.draw.projected_slots[1] = -1;
    value.data.draw.projected_transforms[1][12] = 0.5f;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.count() == 8u);
    CHECK(packet.command(0).data.clear.color == 0xabcdef12u);
    CHECK(packet.command(0).data.clear.z == 0.5f);
    CHECK(packet.record(1).kind == D3dCapturePacket::REPORT);
    CHECK(packet.command(2).data.present.swap_counter == 42u);
    CHECK(packet.record(3).kind == D3dCapturePacket::RELEASE);
    for (const auto &plane : packet.command(4).data.gamma) {
        for (uint8_t byte : plane) CHECK(byte == 0x7au);
    }
    for (size_t i = 0u; i < sources.size(); ++i) {
        CHECK(packet.command(5).data.draw.*fields[i] != nullptr);
        CHECK(packet.command(5).data.draw.*fields[i] != sources[i].data());
        CHECK(packet.command(7).data.draw.*fields[i] == nullptr);
    }
    CHECK(packet.command(5).data.draw.directional.world_transforms[3][14] == 12.0f);
    CHECK(packet.command(5).data.draw.directional.positions[7][2] == 30.0f);
    CHECK(packet.command(5).data.draw.directional.positions[7][3] == 1.0f);
    CHECK(packet.command(5).data.draw.directional.attenuation[7][1] == 0.05f);
    CHECK(packet.command(5).data.draw.directional.attenuation[7][3] == 100.0f);
    CHECK(packet.command(5).data.draw.directional.ambient[7][0] == 0.25f);
    CHECK(packet.command(5).data.draw.directional.enabled && packet.command(5).data.draw.has_reflection);
    CHECK(packet.command(5).data.draw.reflection_world_view[12] == 3.0f);
    CHECK(packet.command(5).data.draw.reflection_blend_normal[2][15] == 4.0f);
    CHECK(packet.command(5).data.draw.reflection_stage1_transform[3] == 5.0f);
    CHECK(packet.command(5).data.draw.reflection_diffuse[2] == 0.5f);
    CHECK(!packet.command(6).data.draw.directional.enabled && !packet.command(6).data.draw.has_reflection);
    CHECK(packet.command(6).data.draw.directional.stage_mode && packet.command(6).data.draw.directional.stage[2] == 1.0f);
    CHECK(packet.command(7).data.draw.texgen_projected && packet.command(7).data.draw.reflection_transform[5] == 2.5f);
    CHECK(packet.command(7).data.draw.projected_stage_count == 2u && packet.command(7).data.draw.projected_slots[1] == -1 &&
        packet.command(7).data.draw.projected_transforms[1][12] == 0.5f);
    CHECK(packet.command(6).data.draw.texture_bytes != sources[2].data());
    CHECK(packet.command(6).data.draw.vertex_bytes != sources[0].data());
    packet.clear();
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR;
    value.data.copy_destination.data = 0x280000u;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.command(0).data.copy_destination.data == 0x280000u);
    packet.clear();
    input.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE;
    input.data.draw.program_count = 0u;
    for (size_t i = 2u; i < sources.size(); ++i)
        input.data.draw.*fields[i] = reinterpret_cast<const void *>(1u);
    for (auto field : counts) input.data.draw.*field = UINT32_MAX;
    input.data.draw.reflection_stage2_byte_count = UINT32_MAX;
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.command(0).type == RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE);
    const auto &geometry = packet.command(0).data.draw;
    for (size_t i = 0u; i < 2u; ++i) {
        CHECK(geometry.*fields[i] != sources[i].data());
        CHECK(std::memcmp(geometry.*fields[i], sources[i].data(), sizes[i]) == 0);
    }
    for (size_t i = 2u; i < sources.size(); ++i) CHECK(geometry.*fields[i] == nullptr);

    packet.clear();
    RecompD3dPresenterCommand vertices{};
    vertices.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    vertices.data.draw.vertex_count = 3u;
    vertices.data.draw.vertex_stride = 11u;
    vertices.data.draw.vertex_bytes = sources[0].data();
    sources[0][0] = sources[0][1] = 0u;
    const std::vector<uint8_t> early_vertices = sources[0];
    for (size_t i = 0u; i < 600u; ++i) {
        sources[0][0] = static_cast<uint8_t>(i);
        sources[0][1] = static_cast<uint8_t>(i >> 8);
        CHECK(packet.add(vertices) == RECOMP_D3D_PRESENTER_OK);
    }
    sources[0] = early_vertices;
    CHECK(packet.add(vertices) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.command(600).data.draw.vertex_bytes == packet.command(0).data.draw.vertex_bytes);
    CHECK(std::memcmp(packet.command(600).data.draw.vertex_bytes, early_vertices.data(), early_vertices.size()) == 0);

    packet.clear();
    sources[0][0] = 0x11u;
    CHECK(packet.add(vertices) == RECOMP_D3D_PRESENTER_OK);
    const std::vector<uint8_t> first_vertices = sources[0];
    sources[0][0] = 0x22u;
    CHECK(packet.add(vertices) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.command(0).data.draw.vertex_bytes != packet.command(1).data.draw.vertex_bytes);
    CHECK(std::memcmp(packet.command(0).data.draw.vertex_bytes, first_vertices.data(), first_vertices.size()) == 0);
    CHECK(std::memcmp(packet.command(1).data.draw.vertex_bytes, sources[0].data(), sources[0].size()) == 0);
    // Standing texture bytes: a later fill reuses them until a lock or release
    // drops the span; sampled changes are copied, palettes and linear surfaces
    // always are.
    std::vector<uint8_t> texture(4096u), linear(4096u), palette(1024u);
    for (size_t i = 0u; i < texture.size(); ++i) {
        texture[i] = static_cast<uint8_t>(i);
        linear[i] = static_cast<uint8_t>(i + 3u);
    }
    for (size_t i = 0u; i < palette.size(); ++i) palette[i] = static_cast<uint8_t>(i + 7u);
    const auto standingDraw = [&](bool is_linear) {
        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        auto &lit = command.data.draw;
        lit.vertex_count = 3u;
        lit.vertex_stride = 11u;
        lit.index_count = 3u;
        lit.vertex_bytes = sources[0].data();
        lit.index_bytes = sources[1].data();
        lit.has_texture = true;
        lit.texture.format_byte = 0u;
        lit.texture.linear = is_linear;
        lit.texture.data = is_linear ? 0xa00000u : 0x900000u;
        lit.texture.width = lit.texture.height = 64u;
        lit.texture_bytes = is_linear ? linear.data() : texture.data();
        lit.texture_byte_count = 4096u;
        lit.palette_bytes = palette.data();
        lit.palette_byte_count = 1024u;
        return command;
    };

    packet.clear();
    CHECK(packet.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    const void *first_texture = packet.command(0).data.draw.texture_bytes;
    CHECK(std::memcmp(first_texture, texture.data(), texture.size()) == 0);
    CHECK(packet.command(0).data.draw.palette_bytes != palette.data());

    // An unsampled byte leaves the standing copy alone; the sampled word does not.
    texture[8] = 0xffu;
    palette[0] = 0x5au;
    packet.clear();
    CHECK(packet.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.command(0).data.draw.texture_bytes == first_texture);
    CHECK(static_cast<const uint8_t *>(first_texture)[8] != 0xffu);
    // A palette names no address to drop, so it always refreshes.
    CHECK(static_cast<const uint8_t *>(packet.command(0).data.draw.palette_bytes)[0] == 0x5au);

    // A sampled word and a lock copy afresh; a release drops the span too.
    texture[0] = 0xfeu;
    packet.clear();
    CHECK(packet.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(static_cast<const uint8_t *>(packet.command(0).data.draw.texture_bytes)[0] == 0xfeu);
    texture[0] = 0u;

    packet.clear();
    CHECK(packet.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addInvalidate(0x900000u, 4096u) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(std::memcmp(packet.command(2).data.draw.texture_bytes, texture.data(), texture.size()) == 0);

    packet.clear();
    CHECK(packet.addRelease(0x900000u, 4096u) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(std::memcmp(packet.command(1).data.draw.texture_bytes, texture.data(), texture.size()) == 0);

    // Every packet keeps its own copy: a lock while another packet is open must
    // drop this one too, or a later fill serves the bytes from before the lock.
    {
        D3dCapturePacket older, open;
        CHECK(older.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
        older.seal();
        CHECK(open.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
        open.seal();
        texture[8] = 0x77u;  // outside every sampled word
        open.clear();
        older.clear();
        CHECK(open.addInvalidate(0x900000u, 4096u) == RECOMP_D3D_PRESENTER_OK);
        older.dropResident(0x900000u, 4096u);  // the presenter thread scans the other packets
        CHECK(older.add(standingDraw(false)) == RECOMP_D3D_PRESENTER_OK);
        older.seal();
        CHECK(static_cast<const uint8_t *>(older.command(0).data.draw.texture_bytes)[8] == 0x77u);
        texture[8] = 0xffu;
    }

    // The mask and both reflection textures drop by their own addresses.
    {
        std::vector<uint8_t> mask(2048u, 0x21u), reflection(3072u, 0x31u), reflection2(2048u, 0x41u);
        const auto extraDraw = [&] {
            RecompD3dPresenterCommand command = standingDraw(false);
            auto &lit = command.data.draw;
            lit.has_alpha_mask = true;
            lit.alpha_mask.format_byte = 0u;
            lit.alpha_mask.data = 0xb00000u;
            lit.alpha_mask.width = lit.alpha_mask.height = 32u;
            lit.alpha_mask_bytes = mask.data();
            lit.alpha_mask_byte_count = static_cast<uint32_t>(mask.size());
            lit.has_reflection = true;
            lit.reflection_texture.format_byte = 0u;
            lit.reflection_texture.data = 0xc00000u;
            lit.reflection_texture.width = lit.reflection_texture.height = 48u;
            lit.reflection_bytes = reflection.data();
            lit.reflection_byte_count = static_cast<uint32_t>(reflection.size());
            lit.reflection_stage2 = true;
            lit.reflection_stage2_texture.format_byte = 0u;
            lit.reflection_stage2_texture.data = 0xd00000u;
            lit.reflection_stage2_texture.width = lit.reflection_stage2_texture.height = 32u;
            lit.reflection_stage2_bytes = reflection2.data();
            lit.reflection_stage2_byte_count = static_cast<uint32_t>(reflection2.size());
            return command;
        };
        packet.clear();
        CHECK(packet.add(extraDraw()) == RECOMP_D3D_PRESENTER_OK);
        packet.seal();
        CHECK(std::memcmp(packet.command(0).data.draw.alpha_mask_bytes, mask.data(), mask.size()) == 0);
        CHECK(std::memcmp(packet.command(0).data.draw.reflection_bytes, reflection.data(), reflection.size()) == 0);
        CHECK(std::memcmp(packet.command(0).data.draw.reflection_stage2_bytes, reflection2.data(), reflection2.size()) == 0);

        // Unsampled changes to all three: only the locked texture refreshes.
        mask[8] = 0x22u;
        reflection[8] = 0x32u;
        reflection2[8] = 0x42u;
        const auto stale = [&](const void *bytes, uint8_t expected) {
            return static_cast<const uint8_t *>(bytes)[8] == expected;
        };
        packet.clear();
        CHECK(packet.add(extraDraw()) == RECOMP_D3D_PRESENTER_OK);
        packet.seal();
        CHECK(stale(packet.command(0).data.draw.alpha_mask_bytes, 0x21u));
        CHECK(stale(packet.command(0).data.draw.reflection_bytes, 0x31u));
        CHECK(stale(packet.command(0).data.draw.reflection_stage2_bytes, 0x41u));

        packet.clear();
        CHECK(packet.addInvalidate(0xb00000u, static_cast<uint32_t>(mask.size())) == RECOMP_D3D_PRESENTER_OK);
        CHECK(packet.add(extraDraw()) == RECOMP_D3D_PRESENTER_OK);
        packet.seal();
        CHECK(stale(packet.command(1).data.draw.alpha_mask_bytes, 0x22u));
        CHECK(stale(packet.command(1).data.draw.reflection_bytes, 0x31u));
        CHECK(stale(packet.command(1).data.draw.reflection_stage2_bytes, 0x41u));

        packet.clear();
        CHECK(packet.addInvalidate(0xc00000u, static_cast<uint32_t>(reflection.size())) == RECOMP_D3D_PRESENTER_OK);
        CHECK(packet.add(extraDraw()) == RECOMP_D3D_PRESENTER_OK);
        packet.seal();
        CHECK(stale(packet.command(1).data.draw.reflection_bytes, 0x32u));
        CHECK(stale(packet.command(1).data.draw.reflection_stage2_bytes, 0x41u));

        packet.clear();
        CHECK(packet.addInvalidate(0xd00000u, static_cast<uint32_t>(reflection2.size())) == RECOMP_D3D_PRESENTER_OK);
        CHECK(packet.add(extraDraw()) == RECOMP_D3D_PRESENTER_OK);
        packet.seal();
        CHECK(stale(packet.command(1).data.draw.reflection_stage2_bytes, 0x42u));
    }

    // A linear surface is rewritten in place each frame and never stands.
    packet.clear();
    CHECK(packet.add(standingDraw(true)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    const void *first_linear = packet.command(0).data.draw.texture_bytes;
    CHECK(std::memcmp(first_linear, linear.data(), linear.size()) == 0);
    linear[0] = 0x99u;
    packet.clear();
    CHECK(packet.add(standingDraw(true)) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(static_cast<const uint8_t *>(packet.command(0).data.draw.texture_bytes)[0] == 0x99u);

    std::puts("PASS capture ownership, order, deduplication, overflow, alignment, move and reuse");
    return 0;
}
