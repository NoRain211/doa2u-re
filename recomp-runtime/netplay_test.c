#include "netplay.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000u)
#endif

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static RecompInputGamepad pad(uint16_t buttons, int16_t thumb)
{
    RecompInputGamepad value;
    memset(&value, 0, sizeof value);
    value.buttons = buttons;
    value.analog_buttons[3] = 0xffu;
    value.thumb_ly = thumb;
    return value;
}

int main(void)
{
    static RecompRollbackSession a, b;
    static RecompNetplay net_a, net_b;
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET];

    recomp_rollback_init(&a, 0u);
    recomp_rollback_init(&b, 0u);
    for (uint16_t i = 0u; i < 5u; ++i) {
        const RecompInputGamepad value = pad(i, (int16_t)(-1000 * i));
        recomp_rollback_add_local(&a, &value);
    }
    size_t size = recomp_netplay_encode(&net_a, &a, -3, packet);
    check("encode size", size == RECOMP_NETPLAY_HEADER_BYTES + 5u * RECOMP_NETPLAY_PAD_BYTES);
    check("truncated rejected", !recomp_netplay_decode(&net_b, &b, packet, size - 1u));
    packet[0] ^= 1u;
    check("bad magic rejected", !recomp_netplay_decode(&net_b, &b, packet, size));
    packet[0] ^= 1u;
    check("decode", recomp_netplay_decode(&net_b, &b, packet, size));
    check("pads arrive", b.remote_count == 5u && b.remote[4].buttons == 4u &&
        b.remote[4].thumb_ly == -4000 && b.remote[4].analog_buttons[3] == 0xffu);
    check("advantage arrives", net_b.peer_advantage == -3 && net_b.peer_seen);
    check("duplicate ignored", recomp_netplay_decode(&net_b, &b, packet, size) && b.remote_count == 5u);

    // The ack trims what the next packet repeats.
    size = recomp_netplay_encode(&net_b, &b, 0, packet);
    check("ack decode", recomp_netplay_decode(&net_a, &a, packet, size) && net_a.peer_ack == 5u);
    check("acked pads not resent",
        recomp_netplay_encode(&net_a, &a, 0, packet) == RECOMP_NETPLAY_HEADER_BYTES);
    check("ack beyond our pads rejected", (packet[8] = 9u, !recomp_netplay_decode(&net_b, &b, packet,
        RECOMP_NETPLAY_HEADER_BYTES)));

    // Frame wait: a side 4 frames ahead waits 2, then backs off.
    memset(&net_a, 0, sizeof net_a);
    for (unsigned i = 0u; i < RECOMP_NETPLAY_SYNC_FRAMES; ++i) net_a.peer_samples[i] = -2;
    net_a.peer_count = RECOMP_NETPLAY_SYNC_FRAMES;
    uint32_t waited = 0u;
    for (unsigned i = 0u; i < RECOMP_NETPLAY_SYNC_FRAMES; ++i) waited += recomp_netplay_wait_frames(&net_a, 2);
    check("ahead side waits", waited == 2u);
    check("cooldown", recomp_netplay_wait_frames(&net_a, 2) == 0u);
    memset(&net_b, 0, sizeof net_b);
    for (unsigned i = 0u; i < RECOMP_NETPLAY_SYNC_FRAMES; ++i) net_b.peer_samples[i] = 2;
    net_b.peer_count = RECOMP_NETPLAY_SYNC_FRAMES;
    waited = 0u;
    for (unsigned i = 0u; i < 2u * RECOMP_NETPLAY_SYNC_FRAMES; ++i) waited += recomp_netplay_wait_frames(&net_b, -2);
    check("behind side never waits", waited == 0u);

    // A peer frame is sampled once, however many packets repeat it.
    recomp_rollback_init(&a, 0u);
    memset(&net_a, 0, sizeof net_a);
    a.frame = 3u;
    size = recomp_netplay_encode(&net_a, &a, 6, packet);
    memset(&net_b, 0, sizeof net_b);
    recomp_rollback_init(&b, 0u);
    for (int i = 0; i < 5; ++i) recomp_netplay_decode(&net_b, &b, packet, size);
    check("one sample per peer frame", net_b.peer_count == 1u && net_b.peer_samples[0] == 6);

    // Real sockets on loopback: pads cross both ways.
    recomp_rollback_init(&a, 0u);
    recomp_rollback_init(&b, 0u);
    check("open a", recomp_netplay_open(&net_a, 47311u, "127.0.0.1", 47312u));
    check("open b", recomp_netplay_open(&net_b, 47312u, "127.0.0.1", 47311u));
    const RecompInputGamepad seven = pad(7u, 0), nine = pad(9u, 0);
    recomp_rollback_add_local(&a, &seven);
    recomp_rollback_add_local(&b, &nine);
    for (unsigned tries = 0u; tries < 200u && (a.remote_count == 0u || b.remote_count == 0u); ++tries) {
        recomp_netplay_send(&net_a, &a, 0);
        recomp_netplay_send(&net_b, &b, 0);
        sleep_ms(5u);
        recomp_netplay_receive(&net_a, &a);
        recomp_netplay_receive(&net_b, &b);
    }
    check("socket pads", a.remote_count == 1u && a.remote[0].buttons == 9u &&
        b.remote_count == 1u && b.remote[0].buttons == 7u);
    recomp_netplay_close(&net_a);
    recomp_netplay_close(&net_b);

    static const char *const allowed[] = {"PLAYER1", "Ayane", "Raccoon", "Tycoon", "Spice", "Nigeria",
        "Homer", "Pakistan", "Hayabusa 2", "Nazir", "Maryann", "Fascia", "Heilman", "Kasumi1988",
        "Shelby88", ""};
    for (size_t i = 0u; i < sizeof allowed / sizeof *allowed; ++i)
        check(allowed[i], !recomp_netplay_name_banned(allowed[i]));
    static const char *const banned[] = {"N1GG3R", "xX_n i g g a_Xx", "f4g", "FAGS", "FAG123", "Tr4nny",
        "c00n", "Dyke_99", "niiiggggerrr", "\xc3\xa9" "faggot", "1488", "14_88", "Kasumi14 88", "H1TL3R",
        "Sieg Heil", "N4ZI", "Nazis", "xXneonaziXx", "Fascist", "facist", "WhitePower", "kkk_lord",
        "Aryan", "HH88"};
    for (size_t i = 0u; i < sizeof banned / sizeof *banned; ++i)
        check(banned[i], recomp_netplay_name_banned(banned[i]));
    char longName[300];
    memset(longName, 'a', sizeof longName - 1u);
    longName[sizeof longName - 1u] = 0;
    check("too long to check", recomp_netplay_name_banned(longName));

    if (failures != 0) return 1;
    puts("PASS netplay carries pads, acks and advantage over UDP and screens names");
    return 0;
}
