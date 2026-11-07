#include "movie_color_model.h"

/* Sofdec's luma curve (DOA2.xbe 0x325a20): it darkens, so mid grey 128 shows as 80. */
static int sofdec_luma(int i)
{
    if (i < 16) return 3 * (i / 2) + (i & 1);
    if (i < 176) return 24 + (i - 16) / 2;
    if (i < 192) return 104 + (i - 176);
    return 120 + 2 * (i - 192);
}

/* Its chroma curve: within 24 of neutral a third of the offset is kept, so near-grey stays grey. */
static int sofdec_chroma(int i)
{
    if (i < 104) return i * 120 / 104;
    if (i > 152) return 136 + (i - 152) * 119 / 103;
    return i >= 128 ? 128 + (i - 128) / 3 : 128 - (128 - i) / 3;
}

void recomp_movie_color_sofdec_table(int16_t table[RECOMP_MOVIE_COLOR_TABLE_ENTRIES])
{
    /* Sofdec's RGB-to-YUV matrix, inverted by cofactors in 1/64 steps as 0x325d00 does. */
    static const double m[9] = {
        77 / 256.0, 150 / 256.0, 29 / 256.0, -44 / 256.0, -87 / 256.0, 131 / 256.0,
        131 / 256.0, -110 / 256.0, -21 / 256.0};
    const double r = 1.0 / ((m[7] * m[3] * m[2] + m[6] * m[5] * m[1] + m[8] * m[4] * m[0]) -
        (m[6] * m[4] * m[2] + m[8] * m[3] * m[1] + m[7] * m[5] * m[0]));
    const double inverse[9] = {
        (m[8] * m[4] - m[7] * m[5]) * r * 64.0, (m[8] * m[1] - m[7] * m[2]) * r * -64.0,
        (m[5] * m[1] - m[4] * m[2]) * r * 64.0, (m[8] * m[3] - m[6] * m[5]) * r * -64.0,
        (m[8] * m[0] - m[6] * m[2]) * r * 64.0, (m[5] * m[0] - m[3] * m[2]) * r * -64.0,
        (m[7] * m[3] - m[6] * m[4]) * r * 64.0, (m[7] * m[0] - m[6] * m[1]) * r * -64.0,
        (m[4] * m[0] - m[3] * m[1]) * r * 64.0};
    for (int i = 0; i < 256; ++i) {
        const double in[3] = {sofdec_luma(i), sofdec_chroma(i) - 128, sofdec_chroma(i) - 128};
        for (int plane = 0; plane < 3; ++plane) {
            int16_t *entry = table + plane * 1024 + i * 4;
            /* B, G, R, A; the luma entry carries the opaque alpha. The cast truncates as _ftol does. */
            for (int channel = 0; channel < 3; ++channel)
                entry[channel] = (int16_t)(in[plane] * inverse[(2 - channel) * 3 + plane] + 0.5);
            entry[3] = plane == 0 ? 0x3fc0 : 0;
        }
    }
}

static bool span_fits(size_t capacity, size_t stride, size_t row_bytes, uint32_t rows)
{
    return rows != 0u && row_bytes != 0u && stride >= row_bytes &&
        capacity >= row_bytes && (rows - 1u) <= (capacity - row_bytes) / stride;
}

bool recomp_movie_color_convert(
    const RecompMoviePlane planes[3], uint32_t width, uint32_t height,
    const int16_t *color_table, size_t table_entries,
    uint8_t *destination, size_t destination_bytes, size_t destination_pitch)
{
    size_t row_bytes = (size_t)width * 4u;
    uint32_t chroma_width = width / 2u + width % 2u;
    uint32_t chroma_height = height / 2u + height % 2u;

    if (planes == NULL || color_table == NULL || destination == NULL ||
        table_entries < RECOMP_MOVIE_COLOR_TABLE_ENTRIES || width == 0u ||
        height == 0u || row_bytes / 4u != width ||
        !span_fits(destination_bytes, destination_pitch, row_bytes, height)) {
        return false;
    }
    for (unsigned plane = 0u; plane < 3u; ++plane) {
        if (planes[plane].data == NULL ||
            !span_fits(planes[plane].size_bytes, planes[plane].stride_bytes,
                plane == 0u ? width : chroma_width,
                plane == 0u ? height : chroma_height)) {
            return false;
        }
    }

    for (uint32_t y = 0u; y < height; ++y) {
        const uint8_t *luma = planes[0].data + y * planes[0].stride_bytes;
        const uint8_t *u = planes[1].data + (y / 2u) * planes[1].stride_bytes;
        const uint8_t *v = planes[2].data + (y / 2u) * planes[2].stride_bytes;
        uint8_t *out = destination + y * destination_pitch;
        for (uint32_t x = 0u; x < width; ++x) {
            const int16_t *yy = color_table + luma[x] * 4u;
            const int16_t *uu = color_table + 1024u + u[x / 2u] * 4u;
            const int16_t *vv = color_table + 2048u + v[x / 2u] * 4u;
            for (unsigned channel = 0u; channel < 4u; ++channel) {
                /* PADDW keeps the low 16 bits after both additions. */
                uint16_t word = (uint16_t)((int32_t)yy[channel] + uu[channel] + vv[channel]);
                int32_t sum = word < 32768u ? (int32_t)word : (int32_t)word - 65536;
                /* Clamp before dividing: every negative arithmetic-shift result
                   clamps to zero, without relying on signed right-shift rules. */
                out[(size_t)x * 4u + channel] = sum <= 0 ? 0u :
                    sum >= 255 * 64 ? 255u : (uint8_t)(sum / 64);
            }
        }
    }
    return true;
}
