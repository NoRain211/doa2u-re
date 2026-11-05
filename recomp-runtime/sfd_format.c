#include "sfd_format.h"

#include <stdlib.h>
#include <string.h>

int32_t recomp_sfd_packet(const uint8_t *bytes, uint32_t size, RecompSfdPacket *packet)
{
    if (size < 4u) return 0;
    if (bytes[0] != 0u || bytes[1] != 0u || bytes[2] != 1u) return -1;
    const uint32_t id = bytes[3];
    *packet = (RecompSfdPacket){.id = id};
    if (id == RECOMP_SFD_END) return 4;
    if (id == 0xbau) return size < 12u ? 0 : (bytes[4] & 0xf0u) == 0x20u ? 12 : -1;
    if (id < 0xbbu || size < 6u) return id < 0xbbu ? -1 : 0;
    const uint32_t length = (uint32_t)bytes[4] << 8 | bytes[5];
    if (size < 6u + length) return 0;
    uint32_t header = 0u;
    if (id == 0xbdu || id >= 0xc0u) {
        /* MPEG-1 PES: stuffing, an optional STD buffer size, then the time stamps. */
        const uint8_t *p = bytes + 6;
        while (header < length && p[header] == 0xffu) ++header;
        if (header < length && (p[header] & 0xc0u) == 0x40u) header += 2u;
        if (header >= length) return -1;
        header += (p[header] & 0xf0u) == 0x20u ? 5u : (p[header] & 0xf0u) == 0x30u ? 10u
            : p[header] == 0x0fu ? 1u : length + 1u;
        if (header > length) return -1;
    }
    if (id != 0xbbu && id != 0xbeu) {
        packet->payload = bytes + 6 + header;
        packet->size = length - header;
    }
    return (int32_t)(6u + length);
}

/* Each tag is an 8-byte name, its value length as 7 hex digits and a NUL,
   then the value; these four hold further tags. */
static int container(const uint8_t *name)
{
    static const char names[][8] = {"CRITAGS", "USRINFS", "CHDAT", "TIMEDAT"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
        if (memcmp(name, names[i], 8u) == 0) return 1;
    return 0;
}

static uint32_t number(const uint8_t *value, uint32_t length)
{
    char text[16] = "";
    memcpy(text, value, length < sizeof text - 1u ? length : sizeof text - 1u);
    return (uint32_t)strtoul(text, NULL, 10);
}

uint32_t recomp_sfd_subtitles(const uint8_t *bytes, uint32_t size, RecompSfdCue *cues, uint32_t capacity)
{
    uint32_t at = 0u, count = 0u, channel = 0u, unit = 0u, start = 0u, duration = 0u;
    while (at + 7u <= size && memcmp(bytes + at, "CRITAGS", 7u) != 0) ++at;
    while (at + 16u <= size && memcmp(bytes + at, "CRITAGE", 8u) != 0) {
        const uint8_t *name = bytes + at, *value = bytes + at + 16u;
        char hex[8];
        memcpy(hex, bytes + at + 8u, 8u);
        hex[7] = '\0';
        const uint32_t length = (uint32_t)strtoul(hex, NULL, 16);
        if (container(name)) { at += 16u; continue; }
        if (length > size - at - 16u) break;
        if (memcmp(name, "CHNO\0", 5u) == 0) channel = number(value, length);
        else if (memcmp(name, "TUNIT\0", 6u) == 0) unit = number(value, length);
        else if (memcmp(name, "INTIME\0", 7u) == 0) start = number(value, length);
        else if (memcmp(name, "DURTIME\0", 8u) == 0) duration = number(value, length);
        else if (memcmp(name, "CONTENT\0", 8u) == 0 && count < capacity && unit != 0u) {
            RecompSfdCue *cue = &cues[count++];
            *cue = (RecompSfdCue){channel, unit, start, duration, ""};
            memcpy(cue->content, value, length < RECOMP_SFD_CUE_TEXT - 1u ? length : RECOMP_SFD_CUE_TEXT - 1u);
        }
        at += 16u + length;
    }
    return count;
}

