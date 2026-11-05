#include "sfd_format.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "SFD format: line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static uint32_t tag(uint8_t *out, const char *name, const char *value, uint32_t length)
{
    memset(out, 0, 16u);
    memcpy(out, name, strlen(name));
    snprintf((char *)out + 8, 8, "%07x", length);
    if (value != NULL) memcpy(out + 16, value, length);
    return 16u + (value != NULL ? length : 0u);
}

int main(void)
{
    RecompSfdPacket packet;

    /* MPEG-1 pack header, then a video packet with stuffing, STD buffer and PTS+DTS. */
    static const uint8_t pack[12] = {0, 0, 1, 0xba, 0x21, 0, 1, 0, 1, 0x80, 0, 1};
    CHECK(recomp_sfd_packet(pack, sizeof pack, &packet) == 12 && packet.size == 0u);
    CHECK(recomp_sfd_packet(pack, 11u, &packet) == 0);
    uint8_t video[6 + 16] = {0, 0, 1, 0xe0, 0, 16, 0xff, 0x40, 0x10, 0x31, 0, 1, 0, 1, 0x11, 0, 1, 0, 1, 'a', 'b', 'c'};
    CHECK(recomp_sfd_packet(video, sizeof video, &packet) == 22);
    CHECK(packet.id == 0xe0u && packet.size == 3u && memcmp(packet.payload, "abc", 3u) == 0);
    CHECK(recomp_sfd_packet(video, 21u, &packet) == 0);
    /* Audio without a time stamp, private stream 2 carried whole, padding dropped. */
    static const uint8_t audio[] = {0, 0, 1, 0xc0, 0, 3, 0x0f, 'x', 'y'};
    CHECK(recomp_sfd_packet(audio, sizeof audio, &packet) == 9 && packet.size == 2u && packet.payload[0] == 'x');
    static const uint8_t record[] = {0, 0, 1, 0xbf, 0, 2, 'C', 'R'};
    CHECK(recomp_sfd_packet(record, sizeof record, &packet) == 8 && packet.size == 2u && packet.payload[0] == 'C');
    static const uint8_t padding[] = {0, 0, 1, 0xbe, 0, 2, 0xff, 0xff};
    CHECK(recomp_sfd_packet(padding, sizeof padding, &packet) == 8 && packet.size == 0u);
    /* An MPEG-2 pack, a lost start code and a header longer than its packet fail. */
    static const uint8_t mpeg2[12] = {0, 0, 1, 0xba, 0x44};
    CHECK(recomp_sfd_packet(mpeg2, sizeof mpeg2, &packet) == -1);
    static const uint8_t lost[] = {0, 1, 1, 0xe0};
    CHECK(recomp_sfd_packet(lost, sizeof lost, &packet) == -1);
    static const uint8_t short_header[] = {0, 0, 1, 0xe0, 0, 2, 0x21, 0};
    CHECK(recomp_sfd_packet(short_header, sizeof short_header, &packet) == -1);

    /* Two cues on channel 0 at 29.97 units per second, after unrelated bytes. */
    uint8_t tags[512] = "SofdecStream";
    uint32_t at = 32u;
    at += tag(tags + at, "CRITAGS", NULL, 0u);
    at += tag(tags + at, "USRINFS", NULL, 0u);
    at += tag(tags + at, "TYPE", "SUBTITLE", 9u);
    at += tag(tags + at, "CHDAT", NULL, 0u);
    at += tag(tags + at, "CHNO", "0", 2u);
    at += tag(tags + at, "TUNIT", "29970", 6u);
    at += tag(tags + at, "TIMEDAT", NULL, 0u);
    at += tag(tags + at, "INTIME", "480000", 7u);
    at += tag(tags + at, "DURTIME", "60000", 6u);
    at += tag(tags + at, "CONTENT", "MA000,MJ000", 12u);
    at += tag(tags + at, "TIMEDAT", NULL, 0u);
    at += tag(tags + at, "INTIME", "610000", 7u);
    at += tag(tags + at, "DURTIME", "30000", 6u);
    at += tag(tags + at, "CONTENT", "MA001,MJ001", 12u);
    at += tag(tags + at, "USRINFE", NULL, 0u);
    at += tag(tags + at, "CRITAGE", NULL, 0u);
    RecompSfdCue cues[4];
    CHECK(recomp_sfd_subtitles(tags, at, cues, 4u) == 2u);
    CHECK(cues[0].channel == 0u && cues[0].unit == 29970u && cues[0].start == 480000u);
    CHECK(cues[0].duration == 60000u && strcmp(cues[0].content, "MA000,MJ000") == 0);
    CHECK(cues[1].start == 610000u && strcmp(cues[1].content, "MA001,MJ001") == 0);
    CHECK(recomp_sfd_subtitles(tags, at, cues, 1u) == 1u);
    /* A value running past the bytes ends the scan. */
    CHECK(recomp_sfd_subtitles(tags, at - 40u, cues, 4u) == 1u);
    return 0;
}

