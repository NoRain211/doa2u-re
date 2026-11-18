/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d_draw_shader.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kStride = 28u; /* XYZRHW, diffuse, one texcoord. */
constexpr uint32_t kTexcoordOffset = 16u;
constexpr uint16_t kQuad[6] = {0u, 1u, 2u, 0u, 2u, 3u};

void set_uv(uint8_t *vertices, unsigned vertex, float u, float v)
{
    std::memcpy(vertices + vertex * kStride + kTexcoordOffset, &u, sizeof u);
    std::memcpy(vertices + vertex * kStride + kTexcoordOffset + 4u, &v, sizeof v);
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

bool expect(const char *label, const float out[4], float lo_u, float lo_v, float hi_u, float hi_v)
{
    if (near(out[0], lo_u) && near(out[1], lo_v) && near(out[2], hi_u) && near(out[3], hi_v)) return true;
    std::fprintf(stderr, "FAIL %s: got [%g %g %g %g]\n", label, out[0], out[1], out[2], out[3]);
    return false;
}

bool expect_off(const char *label, const float out[4])
{
    if (near(out[0], 1.0f) && near(out[1], 1.0f) && near(out[2], 0.0f) && near(out[3], 0.0f)) return true;
    std::fprintf(stderr, "FAIL %s: expected off, got [%g %g %g %g]\n", label, out[0], out[1], out[2], out[3]);
    return false;
}

} // namespace

int main()
{
    static RecompD3dPresenterDrawCommand draw;
    static uint8_t vertices[64u * kStride];
    RecompD3dVertexLayout layout = {};
    layout.stride = kStride;
    layout.texcoord_offset = kTexcoordOffset;
    layout.texcoord_count = 1u;
    layout.pretransformed = true;
    const auto reset = [&]() {
        draw = {};
        draw.has_texture = true;
        draw.texture.width = 64u;
        draw.texture.height = 32u;
        draw.address_u = draw.address_v = 3u; /* D3DTADDRESS_CLAMP */
        draw.vertex_bytes = vertices;
        draw.vertex_stride = kStride;
        draw.vertex_count = 4u;
        draw.index_bytes = kQuad;
        draw.index_count = 6u;
    };
    std::memset(vertices, 0, sizeof vertices);
    float out[4];
    bool ok = true;

    /* One quad of a sheet: the box is pulled in by half a texel on each side. */
    set_uv(vertices, 0u, 0.25f, 0.5f);
    set_uv(vertices, 1u, 0.5f, 0.5f);
    set_uv(vertices, 2u, 0.25f, 0.75f);
    set_uv(vertices, 3u, 0.5f, 0.75f);
    reset();
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect("quad", out, 0.25f + 0.5f / 64.0f, 0.5f + 0.5f / 32.0f,
        0.5f - 0.5f / 64.0f, 0.75f - 0.5f / 32.0f);

    /* A piece narrower than one texel samples its centre instead of inverting. */
    set_uv(vertices, 1u, 0.25f + 1.0f / 128.0f, 0.5f);
    set_uv(vertices, 3u, 0.25f + 1.0f / 128.0f, 0.75f);
    reset();
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect("subtexel", out, 0.25f + 0.5f / 128.0f, 0.5f + 0.5f / 32.0f,
        0.25f + 0.5f / 128.0f, 0.75f - 0.5f / 32.0f);

    /* Linear textures carry texel coordinates, scaled by width and height. */
    set_uv(vertices, 0u, 16.0f, 16.0f);
    set_uv(vertices, 1u, 32.0f, 16.0f);
    set_uv(vertices, 2u, 16.0f, 24.0f);
    set_uv(vertices, 3u, 32.0f, 24.0f);
    reset();
    draw.texture.linear = true;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect("linear", out, 0.25f + 0.5f / 64.0f, 0.5f + 0.5f / 32.0f,
        0.5f - 0.5f / 64.0f, 0.75f - 0.5f / 32.0f);

    /* Back to the sheet quad for the remaining state checks. */
    set_uv(vertices, 0u, 0.25f, 0.5f);
    set_uv(vertices, 1u, 0.5f, 0.5f);
    set_uv(vertices, 2u, 0.25f, 0.75f);
    set_uv(vertices, 3u, 0.5f, 0.75f);

    /* A draw that leaves the texture, or uses many vertices, is not one piece. */
    reset();
    set_uv(vertices, 0u, -0.1f, 0.5f);
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("outside", out);
    set_uv(vertices, 0u, 0.25f, 0.5f);
    reset();
    draw.index_bytes = nullptr;
    draw.index_count = 0u;
    draw.vertex_count = 65u;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("many-vertices", out);

    /* Only a pretransformed draw under a clamp address mode is inset. */
    reset();
    layout.pretransformed = false;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("transformed", out);
    layout.pretransformed = true;
    reset();
    recomp_d3d_draw_uv_clamp(draw, layout, false, out);
    ok &= expect_off("unbound", out);
    reset();
    draw.has_texture = false;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("untextured", out);

    /* Wrapped, mirrored and unknown modes read across the box on purpose. */
    reset();
    draw.address_u = 0u;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("address-unknown", out);
    reset();
    draw.address_u = 1u;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("address-wrap-u", out);
    reset();
    draw.address_v = 2u;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("address-mirror-v", out);
    reset();
    draw.address_u = draw.address_v = 1u;
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect_off("address-wrap", out);
    reset();
    draw.address_u = draw.address_v = 4u; /* D3DTADDRESS_BORDER */
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect("address-border", out, 0.25f + 0.5f / 64.0f, 0.5f + 0.5f / 32.0f,
        0.5f - 0.5f / 64.0f, 0.75f - 0.5f / 32.0f);
    reset();
    draw.address_u = draw.address_v = 5u; /* D3DTADDRESS_MIRRORONCE */
    recomp_d3d_draw_uv_clamp(draw, layout, true, out);
    ok &= expect("address-mirroronce", out, 0.25f + 0.5f / 64.0f, 0.5f + 0.5f / 32.0f,
        0.5f - 0.5f / 64.0f, 0.75f - 0.5f / 32.0f);

    if (!ok) return 1;
    std::puts("PASS 2D texcoord clamp");
    return 0;
}
