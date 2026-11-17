/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_UTF8_TEXT_H
#define DOA2U_UTF8_TEXT_H
#include <stdint.h>

/* Invalid input consumes one byte so a damaged name cannot hide its suffix. */
static uint32_t recomp_utf8_next(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    const unsigned char first = *p;
    *cursor += first != 0u;
    if (first < 0x80u) return first;
    const unsigned count = first >= 0xc2u && first <= 0xdfu ? 2u :
        first >= 0xe0u && first <= 0xefu ? 3u : first >= 0xf0u && first <= 0xf4u ? 4u : 0u;
    if (count == 0u) return '?';
    uint32_t value = first & (0x7fu >> count);
    for (unsigned i = 1u; i < count; ++i) {
        if ((p[i] & 0xc0u) != 0x80u) return '?';
        value = value << 6 | (p[i] & 0x3fu);
    }
    if ((count == 3u && value < 0x800u) || (count == 4u && value < 0x10000u) ||
        (value >= 0xd800u && value <= 0xdfffu) || value > 0x10ffffu) return '?';
    *cursor = (const char *)p + count;
    return value;
}
#endif
