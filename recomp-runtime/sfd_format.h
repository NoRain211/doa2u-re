#ifndef DOA2U_RECOMP_SFD_FORMAT_H
#define DOA2U_RECOMP_SFD_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

/* Sofdec SFD: an MPEG-1 program stream with MPEG-1 video on 0xE0, ADX on
   0xC0 and CRI's own records (SofdecStream, CRITAGS) in private stream 2. */
enum { RECOMP_SFD_VIDEO = 0xe0, RECOMP_SFD_AUDIO = 0xc0, RECOMP_SFD_PRIVATE = 0xbf, RECOMP_SFD_END = 0xb9 };

typedef struct RecompSfdPacket {
    uint32_t id;              /* stream id, 0xba/0xbb for pack and system headers */
    const uint8_t *payload;   /* PES header removed */
    uint32_t size;
} RecompSfdPacket;

/* Splits the packet at bytes. Returns the bytes it spans, 0 when more input
   is needed, or -1 when bytes does not start a valid MPEG-1 packet. */
int32_t recomp_sfd_packet(const uint8_t *bytes, uint32_t size, RecompSfdPacket *packet);

enum { RECOMP_SFD_CUE_TEXT = 64 };
typedef struct RecompSfdCue {
    uint32_t channel, unit, start, duration; /* start and duration in 1/unit seconds */
    char content[RECOMP_SFD_CUE_TEXT];
} RecompSfdCue;

/* Reads the SUBTITLE cues of a CRITAGS block found in bytes (private-stream
   payloads in file order). Returns the cues stored, at most capacity. */
uint32_t recomp_sfd_subtitles(const uint8_t *bytes, uint32_t size, RecompSfdCue *cues, uint32_t capacity);

/* True when the SofdecStream record in bytes (private-stream payloads in file
   order) marks its video for Sofdec's own color table (DOA2.xbe 0x30da60). */
bool recomp_sfd_sofdec_colors(const uint8_t *bytes, uint32_t size);

#endif

