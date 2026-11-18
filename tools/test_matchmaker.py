# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import hashlib
import io
import json
import os
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

# `python -m unittest tools/test_matchmaker.py` runs from the repository root,
# where the matchmaker module is not on the path; `discover -s tools` already
# has it there.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import matchmaker as mm

HOST_AT = ("203.0.113.7", 47701)
JOINER_AT = ("198.51.100.9", 51234)
A_AT = ("203.0.113.7", 47701)
B_AT = ("198.51.100.9", 51234)
C_AT = ("192.0.2.44", 40000)
BUILD = b"a" * mm.DIGEST
OTHER_BUILD = b"b" * mm.DIGEST
LAN = socket.inet_aton("192.168.1.20") + struct.pack(">H", 47701)
HOST_AT6 = ("2001:db8::7", 40001)  # the same players over IPv6: one socket, one nonce, two endpoints
JOINER_AT6 = ("2001:db8:1::9", 40002)
NONCE_OF = {HOST_AT6: HOST_AT, JOINER_AT6: JOINER_AT}
NO_TOKENS = bytes(2 * mm.TOKEN)  # MatchmakerTest.send fills in the sender's nonce and cookie


def proof_for(secret):
    """The 32-byte proof a client makes from a 32-byte secret."""
    return hashlib.sha256(b"doa2u-live" + secret).digest()


def xuid_for(proof):
    """The XUID the server accepts with this proof."""
    return mm.xuid_of(proof)


PROOF = proof_for(bytes(32))
OTHER_PROOF = proof_for(bytes((1,)) * 32)
THIRD_PROOF = proof_for(bytes((2,)) * 32)
A, B, C = xuid_for(PROOF), xuid_for(OTHER_PROOF), xuid_for(THIRD_PROOF)
PROOFS = {A: PROOF, B: OTHER_PROOF, C: THIRD_PROOF}
NO_KID = bytes(8)
# DOA2U's title id, from its XBE certificate (VA 0x10184 + 8), as LIVE_TITLE in
# recomp-runtime/live_client.c.
TITLE = 0x54430006


def nonce(sender):
    """A stand-in for the random nonce of the client socket at sender."""
    return mm.endpoint(NONCE_OF.get(sender, sender)) + b"\0\0"


def hosting(name=b"KASUMI", is_open=1, build=BUILD, lan=LAN, rtt=0):
    return mm.header(mm.HOST) + NO_TOKENS + mm.HOST_BODY.pack(build, 2, is_open, len(name), name, lan, rtt)


def listing(build=BUILD, size=mm.LIST_BYTES):
    return (mm.header(mm.LIST) + bytes(mm.TOKEN) + build).ljust(size, b"\0")


def joining(room_id, name=b"AYANE", build=BUILD, lan=LAN):
    return mm.header(mm.JOIN) + NO_TOKENS + mm.JOIN_BODY.pack(room_id, build, len(name), name, lan)


def saying_v6():
    return mm.header(mm.ADDR6) + NO_TOKENS


def relaying(game=b"pads"):
    return mm.header(mm.RELAY) + NO_TOKENS + game


def relayed(to, game=b"pads"):
    return mm.header(mm.RELAY) + nonce(to) + game


def rooms(answer):
    count = answer[6 + mm.TOKEN]
    assert len(answer) == 7 + mm.TOKEN + count * mm.ENTRY.size
    return [mm.ENTRY.unpack_from(answer, 7 + mm.TOKEN + i * mm.ENTRY.size) for i in range(count)]


def live(kind, body):
    return mm.header(kind) + NO_TOKENS + body


def guest(ina=0x0a0100ff, port=1000, payload=b"pads", magic=mm.DATAGRAM_MAGIC, version=1):
    """One guest datagram as live_transport.c wraps it."""
    return magic + bytes((version,)) + struct.pack(">I", ina) + struct.pack(">H", port) + payload


def live_relaying(destination, **fields):
    """A LIVE_RELAY to destination's virtual address, carrying one guest datagram."""
    return live(mm.LIVE_RELAY, struct.pack(">I", destination) + guest(**fields))


def challenge_body(req=1, xuid=A):
    return mm.CHALLENGE_BODY.pack(req, xuid)


def logon_body(req=1, xuid=A, proof=None, digest=BUILD, title=TITLE, state=0, kid=NO_KID, tag=b"KASUMI",
               nonce=None):
    """A LOGON as a client with this proof sends it: the proof itself for a new XUID (no nonce),
    else its answer to the server's nonce."""
    if proof is None:
        proof = PROOFS[xuid]
    body = mm.LOGON_BODY.pack(req, xuid, proof, digest, title, state, kid, len(tag), tag.ljust(15, b"\0"))
    if nonce is None:
        return body
    answer = mm.logon_answer(hashlib.sha256(proof).digest(), nonce, body)
    return body[:mm.LOGON_ANSWER.start] + answer + body[mm.LOGON_ANSWER.stop:]


def put_body(req=2, kid=NO_KID, slots=(1, 7, 0, 0), attrs=b"\x01\x02"):
    return mm.SESSION_PUT_BODY.pack(req, kid, *slots, len(attrs)) + attrs


def search_body(req=3, procedure=0, filters=b""):
    return mm.SEARCH_BODY.pack(req, procedure, len(filters)) + filters


def find_body(req=4, kid=NO_KID):
    return mm.FIND_BODY.pack(req, kid)


def connect_body(req=5, ina=0):
    return mm.CONNECT_BODY.pack(req, ina)


def friends_body(req=6):
    return mm.FRIENDS_BODY.pack(req)


def friend_op_body(req=7, op=1, xuid=B, answer=0, kid=NO_KID):
    return mm.FRIEND_OP_BODY.pack(req, op, xuid, answer, kid)


def mutes_body(req=8):
    return mm.MUTES_BODY.pack(req)


def feedback_body(req=9, xuid=B, kind=1):
    return mm.FEEDBACK_BODY.pack(req, xuid, kind)


def write_body(req=10, board=1, xuid=A, stats=((1, 1, 100),)):
    return (mm.STAT_WRITE_BODY.pack(req, board, xuid, len(stats)) +
            b"".join(mm.STAT_ITEM.pack(*stat) for stat in stats))


def group_7_stats():
    """The 61 entries a group-7 write carries: ids 0..59 and the score -2."""
    return [(stat_id, 2, 0x100000000 + stat_id) for stat_id in range(60)] + [(0xFFFE, 2, 1234567890123)]


def read_body(req=11, board=1, xuids=(A,)):
    return mm.STAT_READ_BODY.pack(req, board, len(xuids)) + b"".join(struct.pack(">Q", xuid) for xuid in xuids)


def leaders_body(req=12, board=1, pivot=0, first=1, count=10):
    return mm.LEADERS_BODY.pack(req, board, pivot, first, count)


class MatchmakerTest(unittest.TestCase):
    def setUp(self):
        self.ids = iter(range(1, 1000))
        self.server = mm.Matchmaker(lambda: next(self.ids))
        self.sent = []

    def send(self, data, sender, now, cookie=None):
        """Sends data from sender with its nonce and, unless one is given, the server's current cookie."""
        if len(data) >= mm.ASKED and data[:4] == mm.MAGIC:
            if cookie is None:
                cookie = self.server.cookie(sender, int(now // mm.COOKIE_SECONDS))
            data = data[:6] + nonce(sender) + (data[14:mm.ASKED] if data[5] == mm.LIST else cookie) + data[mm.ASKED:]
        answers = self.server.handle(data, sender, now)
        for packet, to in answers:
            # No answer outgrows its ask, so a forged sender gains nothing.
            self.assertLessEqual(len(packet), len(data))
            # Each carries its receiver's nonce, so a forged one from elsewhere is ignored.
            self.assertEqual(packet[6:6 + mm.TOKEN], nonce(to))
        return answers

    def host(self, now=0.0, sender=HOST_AT, **fields):
        [(answer, to)] = self.send(hosting(**fields), sender, now)
        self.assertEqual((to, answer[5]), (sender, mm.HOSTED))
        return struct.unpack_from(">I", answer, 6 + mm.TOKEN)[0], answer[10 + mm.TOKEN:]

    def test_host_learns_its_room_and_public_endpoint(self):
        room_id, public = self.host()
        self.assertEqual(public, mm.endpoint(HOST_AT))
        self.assertEqual(self.host(now=5.0)[0], room_id)  # a heartbeat keeps the room

    def test_list_shows_open_rooms_of_the_same_build_newest_first(self):
        first, _ = self.host(now=0.0)
        second, _ = self.host(now=1.0, sender=("203.0.113.8", 47701), name=b"HAYATE")
        self.host(now=1.0, sender=("203.0.113.9", 47701), build=OTHER_BUILD)
        self.host(now=1.0, sender=("203.0.113.10", 47701), is_open=0)
        [(answer, _)] = self.send(listing(), JOINER_AT, 3.0)
        listed = rooms(answer)
        self.assertEqual([(entry[0], entry[1], entry[4][:entry[3]]) for entry in listed],
                         [(second, 2, b"HAYATE"), (first, 3, b"KASUMI")])
        self.assertEqual(self.send(listing(size=100), JOINER_AT, 3.0), [])

    def test_a_list_carries_each_hosts_round_trip_and_never_its_address(self):
        first, _ = self.host(now=0.0, rtt=45)
        second, _ = self.host(now=1.0, sender=("203.0.113.8", 47702), name=b"HAYATE")
        [(answer, _)] = self.send(listing(), JOINER_AT, 3.0)
        self.assertEqual([(entry[0], entry[4][:entry[3]], entry[5]) for entry in rooms(answer)],
                         [(second, b"HAYATE", 0), (first, b"KASUMI", 45)])
        for host in (HOST_AT[0], "203.0.113.8"):
            self.assertNotIn(socket.inet_aton(host), answer)
        self.host(now=5.0, rtt=60)  # the latest heartbeat's round trip
        self.assertEqual(rooms(self.send(listing(), JOINER_AT, 6.0)[0][0])[1][5], 60)

    def test_a_full_list_stays_within_its_ask(self):
        for i in range(13):  # one more than a list holds
            newest, _ = self.host(now=i * 0.01, sender=(f"203.0.113.{i + 1}", 47701))
        listed = rooms(self.send(listing(), JOINER_AT, 1.0)[0][0])
        self.assertEqual(len(listed), (mm.LIST_BYTES - 7 - mm.TOKEN) // mm.ENTRY.size)
        self.assertEqual(listed[0][0], newest)  # the room left out is the oldest

    def test_join_sends_each_side_the_others_endpoints(self):
        room_id, _ = self.host()
        answers = dict((to, packet) for packet, to in self.send(joining(room_id), JOINER_AT, 1.0))
        to_host, to_joiner = (packet[:6] + packet[6 + mm.TOKEN:] for packet in (answers[HOST_AT], answers[JOINER_AT]))
        self.assertEqual(to_host[:6], mm.header(mm.PEER))
        # Behind different routers, neither learns the other's LAN address.
        self.assertEqual(to_host[6:18], mm.endpoint(JOINER_AT) + bytes(6))
        self.assertEqual(to_host[19:19 + to_host[18]], b"AYANE")
        self.assertEqual(to_joiner[6:18], mm.endpoint(HOST_AT) + bytes(6))
        self.assertEqual(to_joiner[19:19 + to_joiner[18]], b"KASUMI")
        # Claimed while the two connect; a heartbeat still saying open lists it again later.
        self.assertEqual(rooms(self.send(listing(), ("192.0.2.1", 1), 2.0)[0][0]), [])
        self.host(now=12.0)
        self.assertEqual(len(rooms(self.send(listing(), ("192.0.2.1", 1), 12.0)[0][0])), 1)

    def test_two_players_with_ipv6_are_handed_each_others_endpoints_after_the_ipv4_ones(self):
        self.send(saying_v6(), HOST_AT6, 0.0)
        room_id, _ = self.host(now=0.1)
        self.send(saying_v6(), JOINER_AT6, 0.2)
        answers = dict((to, packet) for packet, to in self.send(joining(room_id), JOINER_AT, 1.0))
        self.assertEqual(answers[HOST_AT][-18:], mm.endpoint(JOINER_AT6))
        self.assertEqual(answers[JOINER_AT][-18:], mm.endpoint(HOST_AT6))
        # The IPv4 part is as without IPv6.
        self.assertEqual(answers[HOST_AT][6 + mm.TOKEN:12 + mm.TOKEN], mm.endpoint(JOINER_AT))

    def test_one_player_without_ipv6_leaves_the_short_answer_for_both(self):
        self.send(saying_v6(), HOST_AT6, 0.0)
        room_id, _ = self.host(now=0.1)
        for packet, _ in self.send(joining(room_id), JOINER_AT, 1.0):
            self.assertEqual(len(packet), 6 + mm.TOKEN + 6 + 6 + 1 + mm.NAME)

    def test_an_ipv6_endpoint_is_stored_only_with_a_cookie_and_expires(self):
        self.send(saying_v6(), HOST_AT6, 0.0, cookie=bytes(mm.TOKEN))  # forged: nothing stored
        self.assertEqual(self.server.v6, {})
        self.send(saying_v6(), HOST_AT6, 0.0)
        self.assertEqual(len(self.server.v6), 1)
        self.send(listing(), JOINER_AT, mm.ADDR6_SECONDS + 5.0)
        self.assertEqual(self.server.v6, {})

    def test_a_full_ipv6_table_still_refreshes_what_it_holds(self):
        self.send(saying_v6(), HOST_AT6, 0.0)
        self.server.v6.update((bytes(2) + i.to_bytes(6, "big"), (JOINER_AT6, 0.0)) for i in range(2 * mm.MAX_ROOMS))
        self.send(saying_v6(), HOST_AT6, 15.0)
        self.assertEqual(self.server.v6[nonce(HOST_AT)][1], 15.0)

    def test_ipv6_senders_may_only_list_and_say_their_address(self):
        self.assertEqual(self.send(hosting(), HOST_AT6, 0.0), [])
        self.assertEqual(self.server.rooms, {})
        [(answer, to)] = self.send(listing(), JOINER_AT6, 0.0)
        self.assertEqual((answer[5], to), (mm.ROOMS, JOINER_AT6))

    def test_join_refuses_a_missing_claimed_or_other_build_room(self):
        room_id, _ = self.host()
        for packet in (joining(room_id + 1), joining(room_id, build=OTHER_BUILD)):
            [(answer, to)] = self.send(packet, JOINER_AT, 1.0)
            self.assertEqual((to, answer[5]), (JOINER_AT, mm.GONE))
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(joining(room_id), ("192.0.2.1", 1), 2.0)[0][0][5], mm.GONE)

    def test_only_private_lan_endpoints_with_a_port_are_passed_on_behind_one_router(self):
        same_router = (HOST_AT[0], 51234)
        for lan, kept in ((LAN, True), (socket.inet_aton("10.1.2.3") + b"\x01\x02", True),
                          (socket.inet_aton("169.254.9.9") + b"\x01\x02", True),
                          (socket.inet_aton("8.8.8.8") + b"\x00\x35", False),
                          (socket.inet_aton("127.0.0.1") + b"\x01\x02", False),
                          (socket.inet_aton("192.168.1.20") + bytes(2), False)):
            self.setUp()
            room_id, _ = self.host(lan=lan)
            answers = dict((to, packet) for packet, to in self.send(joining(room_id, lan=lan), same_router, 1.0))
            self.assertEqual(len(answers), 2)
            for packet in answers.values():
                self.assertEqual(packet[12 + mm.TOKEN:18 + mm.TOKEN], lan if kept else bytes(6))

    def test_one_address_holds_one_claim(self):
        first, _ = self.host()
        second, _ = self.host(sender=("203.0.113.8", 47701))
        self.send(joining(first), JOINER_AT, 1.0)
        self.send(joining(second), (JOINER_AT[0], 51235), 1.0)
        listed = [entry[0] for entry in rooms(self.send(listing(), ("192.0.2.1", 1), 2.0)[0][0])]
        self.assertEqual(listed, [first])

    def test_joins_are_rate_limited_per_address(self):
        room_id, _ = self.host()
        answered = sum(bool(self.send(joining(room_id), (JOINER_AT[0], 50000 + i), 1.0))
                       for i in range(mm.JOINS_PER_SECOND + 5))
        self.assertEqual(answered, mm.JOINS_PER_SECOND)
        self.assertTrue(self.send(joining(room_id), JOINER_AT, 2.5))  # a new second

    def test_a_repeated_join_answers_again(self):
        room_id, _ = self.host()
        first = self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(joining(room_id), JOINER_AT, 1.5), first)

    def test_a_restarted_server_pairs_a_room_and_its_joiner_again(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.server = mm.Matchmaker()  # everything is forgotten
        self.assertEqual(self.send(relaying(), JOINER_AT, 2.0), [])
        self.assertEqual(self.send(joining(room_id), JOINER_AT, 2.0)[0][0][5], mm.GONE)  # until the host's heartbeat
        self.assertEqual(self.host(now=3.0)[0], room_id)
        answers = self.send(joining(room_id), JOINER_AT, 4.0)  # both sides hear PEER again
        self.assertEqual(sorted(to for _, to in answers), sorted([HOST_AT, JOINER_AT]))
        self.assertTrue(all(packet[5] == mm.PEER for packet, _ in answers))
        self.assertEqual(self.send(relaying(), JOINER_AT, 5.0), [(relayed(HOST_AT), HOST_AT)])

    def test_relay_only_hides_endpoints(self):
        self.server.relay_only = True
        room_id, _ = self.host()
        for packet, _ in self.send(joining(room_id), JOINER_AT, 1.0):
            self.assertEqual(packet[6 + mm.TOKEN:18 + mm.TOKEN], bytes(12))
        self.assertEqual(self.send(relaying(), JOINER_AT, 2.0), [(relayed(HOST_AT), HOST_AT)])

    def test_relay_forwards_only_within_a_pair_and_caps_the_rate(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(relaying(), JOINER_AT, 2.0), [(relayed(HOST_AT), HOST_AT)])
        self.assertEqual(self.send(relaying(), HOST_AT, 2.0), [(relayed(JOINER_AT), JOINER_AT)])
        self.assertEqual(self.send(relaying(), ("192.0.2.1", 1), 2.0), [])
        forwarded = sum(len(self.send(relaying(), JOINER_AT, 3.5)) for _ in range(mm.RELAY_PER_SECOND + 10))
        self.assertEqual(forwarded, mm.RELAY_PER_SECOND)

    def test_one_address_relays_and_pairs_only_so_much(self):
        hosts = [(f"203.0.113.{i + 1}", 47701) for i in range(mm.PAIRS_PER_ADDRESS + 1)]
        ids = [self.host(sender=host)[0] for host in hosts]
        joiners = [(JOINER_AT[0], 50000 + i) for i in range(len(hosts))]
        for i, joiner in enumerate(joiners):  # under the JOIN rate, and each new claim lets go of the last
            kind = self.send(joining(ids[i]), joiner, 1.0 + i)[0][0][5]
            self.assertEqual(kind, mm.PEER if i < mm.PAIRS_PER_ADDRESS else mm.GONE)
        # Each pair sends past its own cap; what that cap drops is not counted against the address.
        forwarded = [sum(len(self.send(relaying(), joiner, 30.5)) for _ in range(mm.RELAY_PER_SECOND + 10))
                     for joiner in joiners[:3]]
        self.assertEqual(forwarded, [mm.RELAY_PER_SECOND, mm.RELAY_PER_SECOND, 0])
        self.send(listing(), JOINER_AT, 30.5 + mm.PAIR_SECONDS + 1.0)  # a sweep
        self.assertEqual(self.server.pairs, {})

    def test_a_sender_without_a_cookie_gets_one_and_nothing_is_stored(self):
        room_id, _ = self.host()
        forged = ("192.0.2.1", 1)
        stale = self.server.cookie(forged, -2)  # two minutes old
        others = self.server.cookie(HOST_AT, 0)  # the right epoch, but another endpoint's
        for packet, sender in ((hosting(), forged), (joining(room_id), forged), (joining(room_id), JOINER_AT),
                               (mm.header(mm.LEAVE) + NO_TOKENS, HOST_AT), (relaying(), JOINER_AT)):
            for cookie in (bytes(mm.TOKEN), stale, others if sender != HOST_AT else stale):
                [(answer, to)] = self.send(packet, sender, 1.0, cookie=cookie)
                self.assertEqual((to, answer[:6], answer[6 + mm.TOKEN:]),
                                 (sender, mm.header(mm.COOKIE), self.server.cookie(sender, 0)))
        self.assertEqual(list(self.server.rooms), [HOST_AT])
        self.assertEqual((self.server.pairs, self.server.claims, self.server.join_counts), ({}, {}, {}))
        # The cookie it was given lets it in, through the next minute too, with a new cookie then.
        room_id, _ = self.host(now=55.0)  # a new room; the first expired
        [(answer, _)] = self.send(joining(room_id), JOINER_AT, 55.0, cookie=bytes(mm.TOKEN))
        answers = self.send(joining(room_id), JOINER_AT, 61.0, cookie=answer[6 + mm.TOKEN:])
        self.assertEqual(sorted(packet[5] for packet, _ in answers), [mm.PEER, mm.PEER, mm.COOKIE])

    def test_rooms_expire_leave_and_are_capped_per_address(self):
        self.host()
        self.send(mm.header(mm.LEAVE) + NO_TOKENS, HOST_AT, 1.0)
        self.assertEqual(self.server.rooms, {})
        self.host(now=2.0)
        self.send(listing(), JOINER_AT, 2.0 + mm.ROOM_SECONDS + 1.0)
        self.assertEqual(self.server.rooms, {})
        for port in range(mm.ROOMS_PER_ADDRESS):
            self.host(now=60.0, sender=(HOST_AT[0], 50000 + port))
        self.assertEqual(self.send(hosting(), (HOST_AT[0], 60000), 60.0), [])

    def test_an_address_opens_few_rooms_a_minute(self):
        # Leaving and hosting again from new ports would keep one address's rooms at the top of the list.
        for port in range(mm.NEW_ROOMS_PER_MINUTE):
            self.host(now=1.0, sender=(HOST_AT[0], 50000 + port))
            self.send(mm.header(mm.LEAVE) + NO_TOKENS, (HOST_AT[0], 50000 + port), 1.0)
        self.assertEqual(self.send(hosting(), (HOST_AT[0], 60000), 2.0), [])
        self.host(now=2.0, sender=JOINER_AT)  # another address is not held back
        self.host(now=61.0, sender=(HOST_AT[0], 60000))

    def test_garbage_gets_no_answer(self):
        for packet in (b"", b"D2MM", mm.header(mm.HOST) + NO_TOKENS + b"short", mm.header(mm.LEAVE) + b"short",
                       b"XXXX" + bytes((mm.VERSION, mm.LIST)) + bytes(506), b"D2MM\x01\x03" + bytes(506)):
            self.assertEqual(self.send(packet, JOINER_AT, 0.0), [])

    def test_status_counts_without_addresses_and_restarts(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.send(joining(room_id), JOINER_AT, 1.5)  # a repeat is the same join
        for _ in range(mm.RELAY_PER_SECOND + 2):
            self.send(relaying(), JOINER_AT, 2.0)
        line = self.server.status(3.0)
        self.assertEqual(line, "matchmaker: rooms 1, open 1, pairs 1, joins 1, relayed 250 (4 KiB), over the relay cap 2")
        self.assertNotIn(HOST_AT[0], line)
        self.assertNotIn(JOINER_AT[0], line)
        self.assertTrue(self.server.status(4.0).endswith("joins 0, relayed 0 (0 KiB), over the relay cap 0"))

    def test_probe_hears_a_matchmaker_and_nothing_else(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))

            def answer():
                data, sender = server.recvfrom(2048)
                for packet, to in mm.Matchmaker().handle(data, sender, 0.0):
                    server.sendto(packet, to)

            threading.Thread(target=answer, daemon=True).start()
            self.assertIsNotNone(mm.probe("127.0.0.1", server.getsockname()[1]))
            silent = server.getsockname()[1]  # nothing answers a second ask
            self.assertIsNone(mm.probe("127.0.0.1", silent, timeout=0.3))


class LiveTest(unittest.TestCase):
    """The Live kinds: accounts, sessions, friends and stats."""

    def setUp(self):
        self.serial = 0
        self.request = 0
        self.server = mm.Matchmaker(lambda: 1, random_bytes=self.random)

    def random(self, count):
        """A deterministic stand-in for os.urandom, so kids and keys can be checked."""
        self.serial += 1
        return bytes((self.serial * 37 + i) & 0xff for i in range(count))

    def next_request(self):
        """A fresh request id, as the client gives each request."""
        self.request += 1
        return self.request

    def send(self, data, sender, now, cookie=None):
        """Sends a Live packet with its nonce and, unless one is given, the server's cookie."""
        if cookie is None:
            cookie = self.server.cookie(sender, int(now // mm.COOKIE_SECONDS))
        data = data[:6] + nonce(sender) + cookie + data[mm.ASKED:]
        answers = self.server.handle(data, sender, now)
        for packet, to in answers:
            self.assertLessEqual(len(packet), mm.MAX_PACKET)
            self.assertEqual(packet[6:6 + mm.TOKEN], nonce(to))
        return answers

    def one(self, data, sender, now, kind, cookie=None):
        """The single answer of a kind, from sender, after its header and nonce."""
        answers = self.send(data, sender, now, cookie)
        self.assertEqual(len(answers), 1)
        packet, to = answers[0]
        self.assertEqual((to, packet[5]), (sender, kind))
        return packet[mm.ASKED - mm.TOKEN:]

    def challenge(self, sender=A_AT, xuid=A, now=0.0, req=None):
        """Asks for a LOGON's nonce: the request id, whether the XUID has an account, and the nonce."""
        req = self.next_request() if req is None else req
        return mm.NONCE_BODY.unpack(self.one(live(mm.CHALLENGE, challenge_body(req, xuid)), sender, now, mm.NONCE))

    def logon(self, sender=A_AT, xuid=A, proof=None, tag=b"KASUMI", now=0.0, state=0, kid=NO_KID,
              digest=BUILD, title=TITLE, req=None):
        """A whole logon, as the client does it: CHALLENGE, then the LOGON that answers its nonce."""
        req, known, nonce = self.challenge(sender, xuid, now, req)
        body = self.one(live(mm.LOGON, logon_body(req, xuid, proof, digest, title, state, kid, tag,
                                                  nonce if known else None)), sender, now, mm.LOGGED)
        return mm.LOGGED_BODY.unpack(body)

    def put(self, sender=A_AT, now=0.0, req=None, kid=NO_KID, slots=(1, 7, 0, 0), attrs=b"\x01\x02"):
        body = self.one(live(mm.SESSION_PUT, put_body(self.next_request() if req is None else req, kid, slots, attrs)),
                        sender, now, mm.SESSION_OK)
        return mm.SESSION_OK_BODY.unpack(body)

    def search(self, sender=A_AT, now=0.0, req=None):
        req = self.next_request() if req is None else req
        return [self.row(packet[mm.ASKED - mm.TOKEN:]) for packet, _ in
                self.send(live(mm.SEARCH, search_body(req)), sender, now)]

    def row(self, body):
        """The request, hresult, index, total and the fields of one RESULT body."""
        req, hresult, index, total = mm.RESULT_BODY.unpack_from(body)
        if total == 0:
            return req, hresult, index, total, None
        return req, hresult, index, total, mm.RESULT_ROW.unpack_from(body, mm.RESULT_BODY.size)

    def friends(self, sender=A_AT, now=0.0, req=None):
        req = self.next_request() if req is None else req
        return [mm.FRIEND_BODY.unpack_from(packet, mm.ASKED - mm.TOKEN)
                for packet, _ in self.send(live(mm.FRIENDS, friends_body(req)), sender, now)]

    def done(self, body):
        return mm.DONE_BODY.unpack_from(body)

    def outcome(self, data, sender, now):
        """The hresult of a request's DONE, which comes before an answer's PUSH."""
        answers = self.send(data, sender, now)
        self.assertEqual(answers[0][0][5], mm.DONE)
        return mm.DONE_BODY.unpack_from(answers[0][0], mm.ASKED - mm.TOKEN)[1]

    def stats(self, sender=A_AT, now=0.0, req=None, board=1, xuids=(A,)):
        req = self.next_request() if req is None else req
        return [mm.STATROW_BODY.unpack_from(packet, mm.ASKED - mm.TOKEN) + (packet,) for packet, _ in
                self.send(live(mm.STAT_READ, read_body(req, board, xuids)), sender, now)]

    def leaders(self, sender=A_AT, now=0.0, req=None, board=1, pivot=0, first=1, count=10):
        req = self.next_request() if req is None else req
        return [mm.STATROW_BODY.unpack_from(packet, mm.ASKED - mm.TOKEN) for packet, _ in
                self.send(live(mm.LEADERS, leaders_body(req, board, pivot, first, count)), sender, now)]

    def online(self, now=0.0):
        self.logon(A_AT, A, tag=b"KASUMI", now=now)
        self.logon(B_AT, B, tag=b"AYANE", now=now)
        self.logon(C_AT, C, tag=b"HAYATE", now=now)

    def fill_accounts(self, count=mm.MAX_ACCOUNTS):
        """Fills the account table without logging on, as a long-running server would hold it."""
        for index in range(count):
            account = mm.Account(1 << 60 | index, hashlib.sha256(b"fill%d" % index).digest(),
                                 b"F%d" % index, TITLE)
            account.ina = self.server.next_ina()
            account.seen = 10.0
            self.server.accounts[account.xuid] = account
            self.server.by_ina[account.ina] = account

    def test_logon_registers_an_account_and_gives_a_virtual_address(self):
        req, hresult, ina, length, tag = self.logon()
        self.assertEqual((req, hresult, length, tag[:length]), (1, 0, 6, b"KASUMI"))
        self.assertEqual(ina, 0x0a010001)  # 10.1.0.1, never loopback and never 10.0.*
        self.assertEqual(self.logon(now=5.0, tag=b"OTHER")[2], ina)  # a heartbeat keeps the address
        self.assertEqual(self.logon(now=5.0, tag=b"OTHER")[4][:6], b"KASUMI")  # and the gamertag
        self.assertEqual(self.logon(B_AT, B, tag=b"AYANE", now=5.0)[2], 0x0a010002)

    def test_another_proof_is_refused_and_a_taken_gamertag_gets_a_digit(self):
        self.logon()
        # A new account asking for a taken gamertag gets a digit; the first keeps its own.
        self.assertEqual(self.logon(B_AT, B, proof=OTHER_PROOF, tag=b"KASUMI")[4][:7], b"KASUMI1")
        self.assertEqual(self.logon(A_AT, A, proof=OTHER_PROOF, now=1.0)[1], mm.E_ACCESSDENIED)
        self.assertEqual(self.logon(C_AT, C, tag=b"KASUMI", now=1.0)[4][:7], b"KASUMI2")
        self.assertEqual(self.logon(A_AT, A, now=2.0)[4][:6], b"KASUMI")  # its own proof still logs on

    def test_a_logon_must_carry_the_xuid_of_its_proof(self):
        self.logon()
        # A client cannot squat another player's XUID, which search results show.
        self.assertEqual(self.logon(A_AT, A, proof=OTHER_PROOF, now=1.0)[1], mm.E_ACCESSDENIED)
        self.assertEqual(self.logon(B_AT, B, proof=PROOF, now=1.0)[1], mm.E_ACCESSDENIED)
        self.assertEqual(self.server.accounts[A].key, hashlib.sha256(PROOF).digest())
        self.assertEqual(self.logon(B_AT, B, now=1.0)[1], 0)

    def test_a_logon_without_a_challenge_is_refused(self):
        # What a client from before the challenge sends: its proof, unasked. It registers nothing.
        body = self.one(live(mm.LOGON, logon_body(900)), A_AT, 0.0, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(body)[1], mm.E_ACCESSDENIED)
        self.assertEqual(self.server.accounts, {})
        self.assertEqual(self.logon(now=1.0)[1], 0)
        # Nor does the proof log an account on once it has a challenge: only the answer does.
        req, known, nonce = self.challenge(now=2.0)
        self.assertEqual(known, 1)
        body = self.one(live(mm.LOGON, logon_body(req)), A_AT, 2.0, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(body)[1], mm.E_ACCESSDENIED)

    def test_a_captured_logon_cannot_be_replayed(self):
        self.logon()
        req, known, nonce = self.challenge(now=5.0)
        captured = logon_body(req, nonce=nonce, state=0x13)
        self.assertEqual(mm.LOGGED_BODY.unpack(self.one(live(mm.LOGON, captured), A_AT, 5.0, mm.LOGGED))[1], 0)

        def replayed(sender, now):
            return mm.LOGGED_BODY.unpack(self.one(live(mm.LOGON, captured), sender, now, mm.LOGGED))[1]

        # From another socket there is no challenge, and one that socket asks for has another nonce.
        self.assertEqual(replayed(C_AT, 5.5), mm.E_ACCESSDENIED)
        self.assertEqual(self.challenge(C_AT, A, 6.0, req)[0], req)
        self.assertEqual(replayed(C_AT, 6.0), mm.E_ACCESSDENIED)
        self.assertIs(self.server.by_endpoint[A_AT], self.server.accounts[A])
        self.assertNotIn(C_AT, self.server.by_endpoint)
        # From its own socket a repeat gets the first answer again while that is cached, and
        # is refused after: the challenge went with its first answer.
        self.assertEqual(replayed(A_AT, 6.0), 0)
        self.assertEqual(self.server.accounts[A].seen, 5.0)
        self.assertEqual(replayed(A_AT, 5.0 + mm.CACHE_SECONDS + 1.0), mm.E_ACCESSDENIED)

    def test_a_challenge_is_answered_once_and_expires(self):
        self.logon()
        req, known, nonce = self.challenge(now=1.0)
        self.assertEqual(self.challenge(now=1.5, req=req)[2], nonce)  # a resent CHALLENGE: the same nonce
        self.server.live_cache.clear()
        self.assertEqual(self.challenge(now=1.5, req=req)[2], nonce)  # even when no answer was kept
        body = self.one(live(mm.LOGON, logon_body(req, nonce=nonce)), A_AT, 2.0, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(body)[1], 0)
        self.assertEqual(self.server.challenges, {})
        # The same nonce under another request id, or again once the cache forgot the first answer.
        other = self.one(live(mm.LOGON, logon_body(req + 1, nonce=nonce)), A_AT, 2.0, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(other)[1], mm.E_ACCESSDENIED)
        self.server.live_cache.clear()
        again = self.one(live(mm.LOGON, logon_body(req, nonce=nonce)), A_AT, 2.0, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(again)[1], mm.E_ACCESSDENIED)
        # One for another XUID does not serve this one, and one left unanswered runs out.
        req, known, nonce = self.challenge(A_AT, B, now=3.0)
        late = self.one(live(mm.LOGON, logon_body(req, nonce=nonce)), A_AT, 3.0, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(late)[1], mm.E_ACCESSDENIED)
        req, known, nonce = self.challenge(now=4.0)
        late = self.one(live(mm.LOGON, logon_body(req, nonce=nonce)), A_AT, 4.0 + mm.CHALLENGE_SECONDS, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(late)[1], mm.E_ACCESSDENIED)
        self.assertEqual(self.server.challenges, {})

    def test_challenges_are_capped_per_address_and_in_all(self):
        # An address past its share of unanswered challenges drops its own oldest.
        other = (A_AT[0], A_AT[1] + 1)  # another socket behind the same address
        first = self.challenge(other, B, now=1.5, req=99)
        for index in range(mm.CHALLENGES_PER_ADDRESS + 4):
            self.challenge(now=1.5, req=100 + index)
        self.assertEqual(len(self.server.challenges), mm.CHALLENGES_PER_ADDRESS)
        self.assertEqual(len(self.server.challenge_keys[A_AT[0]]), mm.CHALLENGES_PER_ADDRESS)
        body = self.one(live(mm.LOGON, logon_body(99, B, nonce=first[2])), other, 1.5, mm.LOGGED)
        self.assertEqual(mm.LOGGED_BODY.unpack(body)[1], mm.E_ACCESSDENIED)  # its oldest went
        # A table full of other addresses' drops the oldest of all, so a flood cannot keep players out.
        self.server.challenges = {((f"x{i}", 1), b"", b""): (A, bytes(16), 1e9) for i in range(mm.MAX_CHALLENGES)}
        self.server.challenge_keys = {}
        self.assertEqual(self.logon(B_AT, B, now=3.0)[1], 0)
        self.assertEqual(len(self.server.challenges), mm.MAX_CHALLENGES - 1)
        self.assertNotIn((("x0", 1), b"", b""), self.server.challenges)
        self.assertNotIn(B_AT[0], self.server.challenge_keys)  # answered, so nothing is left of it
        # What nobody answers runs out, and its address's note with it.
        self.challenge(C_AT, C, now=4.0)
        self.server.challenges = {key: value for key, value in self.server.challenges.items() if key[0] == C_AT}
        self.server.sweep(4.0 + mm.CHALLENGE_SECONDS)
        self.assertEqual((self.server.challenges, self.server.challenge_keys), ({}, {}))

    def test_heartbeats_keep_a_session_alive(self):
        self.logon()
        self.logon(B_AT, B, tag=b"AYANE")
        self.put(now=0.0)
        registered = self.server.registered
        for beat in range(1, 9):  # every 5 s, each a CHALLENGE and the LOGON that answers it
            self.assertEqual(self.logon(now=5.0 * beat)[1], 0)
        self.assertEqual(self.server.registered, registered)
        self.assertIsNotNone(self.server.online_at(A_AT, 40.0 + mm.LIVE_SECONDS))
        self.assertIsNone(self.server.online_at(B_AT, 40.0))  # B said nothing since its logon
        self.assertEqual(len(self.server.sessions), 1)  # A's, still there for a search

    def test_a_session_is_created_updated_found_and_deleted(self):
        self.logon()
        self.logon(B_AT, B, tag=b"AYANE")
        req, hresult, kid, key = self.put(attrs=b"\xaa" * 8)
        self.assertEqual(hresult, 0)
        self.assertNotEqual(kid, NO_KID)
        self.assertEqual(len(key), 16)
        self.assertNotEqual(self.put(slots=(8, 0, 0, 0))[2], kid)  # each create is its own session
        body = self.one(live(mm.FIND, find_body(self.next_request(), kid)), B_AT, 0.0, mm.RESULT)
        found = self.row(body)
        self.assertEqual((found[1], found[3]), (0, 1))
        self.assertEqual(found[4][0], kid)  # kid
        self.assertEqual(found[4][1], key)  # key
        self.assertEqual(found[4][2], A)  # host
        self.assertEqual(found[4][3][:6], b"KASUMI")
        self.assertEqual(found[4][4], 0x0a010001)  # the host's virtual address
        self.assertEqual(found[4][5:9], (1, 7, 0, 0))  # slots
        self.assertEqual(found[4][9], 8)  # attribute length
        self.assertEqual(body[mm.RESULT_BODY.size + mm.RESULT_ROW.size:], b"\xaa" * 8)  # attributes
        self.assertEqual(self.put(kid=kid, slots=(8, 0, 0, 0))[1], 0)  # update
        self.assertEqual(self.search(B_AT)[0][3], 0)  # no open slot, so not listed
        self.assertEqual(self.done(self.one(live(mm.SESSION_DEL, mm.SESSION_DEL_BODY.pack(8, kid)), A_AT, 0.0,
                                              mm.DONE)), (8, 0))
        self.assertEqual(self.row(self.one(live(mm.FIND, find_body(9, kid)), B_AT, 0.0, mm.RESULT))[1],
                         mm.E_INVALIDARG)
        self.assertEqual(self.put(sender=B_AT, kid=kid, req=10)[1], mm.E_INVALIDARG)  # not B's session
        self.assertEqual(self.done(self.one(live(mm.SESSION_DEL, mm.SESSION_DEL_BODY.pack(11, kid)), B_AT, 0.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)

    def test_search_takes_only_the_same_build_other_hosts_and_open_slots_newest_first(self):
        self.online()
        first = self.put(now=0.0)[2]
        second = self.put(sender=C_AT, now=1.0, slots=(0, 0, 2, 2))[2]
        # C logs on again with another build, as a patched runner would; its new session is not listed.
        self.logon(C_AT, C, now=2.0, digest=OTHER_BUILD)
        self.put(sender=C_AT, now=2.0, req=3)
        self.put(sender=B_AT, now=3.0, slots=(8, 0, 0, 0))  # full
        rows = self.search(B_AT, now=4.0, req=9)
        self.assertEqual([row[4][0] for row in rows], [second, first])
        self.assertEqual([row[2] for row in rows], [0, 1])  # index
        self.assertEqual({row[3] for row in rows}, {2})  # total
        own = self.search(A_AT, now=4.0, req=10)
        self.assertEqual([row[4][0] for row in own], [second])  # never its own session
        self.assertEqual(own[0][3], 1)

    def test_connect_pairs_a_host_and_a_joiner_and_feeds_the_live_relay(self):
        self.online()
        self.put(now=0.0)  # A hosts, so B may learn A's address
        ina = {xuid: struct.unpack(">I", self.server.accounts[xuid].ina)[0] for xuid in (A, B, C)}
        answers = dict((to, packet) for packet, to in
                       self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[A])), B_AT, 0.0))
        self.assertEqual(sorted(answers), sorted([A_AT, B_AT]))
        pair = self.server.live_pairs[frozenset((A, B))]
        tokens = set()
        for to, packet in answers.items():
            other = A if to == B_AT else B
            peer_ina, public, lan, token, relay = mm.LPEER_BODY.unpack_from(packet, mm.ASKED - mm.TOKEN)
            self.assertEqual(packet[5], mm.LPEER)
            self.assertEqual(peer_ina, ina[other])
            self.assertEqual(public, mm.endpoint(A_AT if other == A else B_AT))
            self.assertEqual(lan, bytes(6))  # LOGON carries no LAN endpoint
            # A pair token, not the socket nonce that only the server should see.
            self.assertNotIn(token, (nonce(A_AT), nonce(B_AT)))
            tokens.add(token)
        self.assertEqual(tokens, {pair.token})  # the same token to both players
        # One CONNECT is not both players meeting: only B connected, so neither
        # may write the other's stats yet.
        self.assertNotIn(frozenset((A, B)), self.server.met)
        # A LIVE_RELAY names where it goes; the server forwards it as a room
        # RELAY and replaces the sender's virtual address with the one it knows.
        claimed = guest(ina=0x0a0100ff, port=51234)
        expected = claimed[:5] + self.server.accounts[B].ina + claimed[9:]
        self.assertEqual(self.send(live_relaying(ina[A], ina=0x0a0100ff, port=51234), B_AT, 1.0),
                         [(mm.header(mm.RELAY) + nonce(A_AT) + expected, A_AT)])
        claimed = guest(ina=0x0a0100ff, port=1000)
        expected = claimed[:5] + self.server.accounts[A].ina + claimed[9:]
        self.assertEqual(self.send(live_relaying(ina[B], ina=0x0a0100ff), A_AT, 1.0),
                         [(mm.header(mm.RELAY) + nonce(B_AT) + expected, B_AT)])
        # The host connects back to the joiner, who hosts nothing of its own:
        # now both have connected, and each may write the other's stats.
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[B])), A_AT, 1.5)), 2)
        self.assertEqual(self.server.met[frozenset((A, B))], 1.5)
        # Nobody else: an unknown address, oneself, an idle player who hosts
        # nothing, and a player of another build.
        self.assertEqual(self.send(live(mm.CONNECT, connect_body(self.next_request(), 0x0a0100ff)), A_AT, 1.0), [])
        self.assertEqual(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[A])), A_AT, 1.0), [])
        self.assertEqual(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[C])), A_AT, 1.0), [])
        self.server.accounts[C].digest = OTHER_BUILD
        self.put(sender=C_AT, now=2.0)
        self.assertEqual(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[C])), A_AT, 1.0), [])
        self.server.accounts[C].digest = BUILD
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[C])), A_AT, 2.0)), 2)
        self.assertEqual(self.server.connects, 3)
        # A pair that expires is a new pair, with a new token.
        old = pair.token
        later = 2.0 + mm.PAIR_SECONDS + 1.0
        self.logon(A_AT, A, now=later)  # its packet sweeps the expired pair away
        self.logon(B_AT, B, now=later)
        self.put(now=later)  # A hosts again
        self.assertNotIn(frozenset((A, B)), self.server.live_pairs)
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[A])), B_AT, later)), 2)
        self.assertNotEqual(self.server.live_pairs[frozenset((A, B))].token, old)

    def test_a_repeated_connect_does_not_extend_a_meeting(self):
        self.online()
        self.put(now=0.0)  # A hosts
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        ina_b = struct.unpack(">I", self.server.accounts[B].ina)[0]
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 0.0)
        pair = self.server.live_pairs[frozenset((A, B))]
        self.assertIsNone(pair.met)
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 0.0)
        self.assertEqual(pair.met, 0.0)
        self.assertEqual(self.server.met[frozenset((A, B))], 0.0)
        # B connects again while A still hosts: the pair stays, the meeting does not move.
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 10.0)), 2)
        self.assertEqual(pair.used, 10.0)
        self.assertEqual(pair.met, 0.0)
        # Six hours on, with the pair kept alive by its own traffic, another
        # CONNECT does not make the meeting newer than the first one.
        later = mm.MET_SECONDS + 1.0
        self.server.accounts[A].seen = later
        self.server.accounts[B].seen = later
        pair.used = later - 1.0
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, later)), 2)
        self.assertEqual(pair.met, 0.0)
        self.assertEqual(self.server.met[frozenset((A, B))], 0.0)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(self.next_request(), 1, A,
                                                                           ((1, 1, 1),))), B_AT, later, mm.DONE))[1],
                         mm.E_ACCESSDENIED)

    def test_a_rematch_renews_the_meeting(self):
        self.online()
        self.put(now=0.0)  # A hosts
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        ina_b = struct.unpack(">I", self.server.accounts[B].ina)[0]
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 0.0)
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 0.0)
        self.assertEqual(self.server.met[frozenset((A, B))], 0.0)
        # The pair lapses; hours later the two meet again in a new one.
        later = mm.MET_SECONDS - 600.0
        self.logon(A_AT, A, now=later)
        self.logon(B_AT, B, now=later)
        self.put(now=later)
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, later)
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, later)
        self.assertEqual(self.server.met[frozenset((A, B))], later)
        # So past six hours from the first match, a stat write is still allowed.
        after = mm.MET_SECONDS + 300.0
        self.logon(A_AT, A, now=after)
        self.logon(B_AT, B, now=after)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(self.next_request(), 1, A,
                                                                           ((1, 1, 1),))), B_AT, after, mm.DONE))[1],
                         0)

    def test_the_pair_cap_counts_only_pairs_an_address_opened(self):
        self.online()
        self.put(now=0.0)  # A hosts
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        ina_b = struct.unpack(">I", self.server.accounts[B].ina)[0]
        # Others' pairs with A fill nothing of A's own allowance.
        for index in range(mm.PAIRS_PER_ADDRESS):
            self.server.live_pairs[frozenset((A, 1 << 61 | index))] = mm.LivePair(0.0, bytes(8), "192.0.2.200")
        self.assertEqual(self.server.live_pairs_at(A_AT[0]), 0)
        self.put(sender=B_AT, now=0.5)  # B hosts; A may still join it
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 1.0)), 2)
        self.assertEqual(self.server.live_pairs_at(A_AT[0]), 1)
        self.assertEqual(self.server.live_pairs_at(B_AT[0]), 0)
        self.assertIsNotNone(ina_a)

    def test_a_connect_keeps_a_pair_only_while_the_other_hosts(self):
        self.online()
        self.put(now=0.0)  # A hosts
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 0.0)
        pair = self.server.live_pairs[frozenset((A, B))]
        later = 30.0
        self.logon(A_AT, A, now=later)  # the sweep drops the old session
        self.logon(B_AT, B, now=later)
        kid = self.put(now=later)[2]  # A hosts again
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, later)
        self.assertEqual(pair.used, later)  # the other player still hosts
        self.assertEqual(self.done(self.one(live(mm.SESSION_DEL, mm.SESSION_DEL_BODY.pack(self.next_request(), kid)),
                                            A_AT, later, mm.DONE))[1], 0)
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, later + 1.0)
        self.assertEqual(pair.used, later)  # a one-sided CONNECT no longer keeps it
        # Relay traffic does.
        self.assertEqual(len(self.send(live_relaying(ina_a), B_AT, later + 2.0)), 1)
        self.assertEqual(pair.used, later + 2.0)

    def test_connect_relay_only_hides_the_endpoints(self):
        self.server.relay_only = True
        self.online()
        self.put(now=0.0)  # A hosts
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        answers = dict((to, packet) for packet, to in
                       self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 0.0))
        token = self.server.live_pairs[frozenset((A, B))].token
        for packet in answers.values():
            _, public, lan, peer_token, relay = mm.LPEER_BODY.unpack_from(packet, mm.ASKED - mm.TOKEN)
            self.assertEqual((public, lan, relay), (bytes(6), bytes(6), 1))
            self.assertEqual(peer_token, token)
            self.assertNotEqual(peer_token, bytes(8))
        claimed = guest(port=51234)
        expected = claimed[:5] + self.server.accounts[B].ina + claimed[9:]
        self.assertEqual(self.send(live_relaying(ina_a, port=51234), B_AT, 1.0),
                         [(mm.header(mm.RELAY) + nonce(A_AT) + expected, A_AT)])

    def test_connect_is_capped_per_address_like_a_join(self):
        self.online()
        self.put(sender=B_AT, now=0.0)  # B hosts
        ina_b = struct.unpack(">I", self.server.accounts[B].ina)[0]
        paired = sum(bool(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 1.0))
                     for _ in range(mm.JOINS_PER_SECOND + 3))
        self.assertEqual(paired, mm.JOINS_PER_SECOND)
        self.assertTrue(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 2.5))

    def test_a_live_relay_needs_a_pair_a_cookie_and_its_datagram(self):
        self.online()
        self.put(now=0.0)  # A hosts
        ina = {xuid: struct.unpack(">I", self.server.accounts[xuid].ina)[0] for xuid in (A, B, C)}
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina[A])), B_AT, 0.0)
        # No pair: C may not relay through A or B, nor write a pair into being.
        self.assertEqual(self.send(live_relaying(ina[A]), C_AT, 1.0), [])
        self.assertEqual(self.send(live_relaying(ina[B]), C_AT, 1.0), [])
        # A stale cookie gets COOKIE only and is forwarded nowhere.
        self.assertEqual(self.send(live_relaying(ina[A]), B_AT, 1.0, cookie=bytes(mm.TOKEN)),
                         [(mm.header(mm.COOKIE) + nonce(B_AT) + self.server.cookie(B_AT, 0), B_AT)])
        # Bad magic, an unknown kind and a body too short for a datagram are dropped.
        for datagram in (guest(magic=b"XXXX"), guest(version=3), guest()[:10]):
            self.assertEqual(self.send(live(mm.LIVE_RELAY, struct.pack(">I", ina[A]) + datagram), B_AT, 1.0), [])
        # A pair's own direction cap drops, and a repeated cookie still answers.
        forwarded = sum(len(self.send(live_relaying(ina[A]), B_AT, 1.5))
                        for _ in range(mm.RELAY_PER_SECOND + 3))
        self.assertEqual(forwarded, mm.RELAY_PER_SECOND)
        # A punch (kind 2) crosses the relay as a game datagram does, so a switch to it reaches the peer.
        punch = guest(version=2, payload=bytes(8))
        expected = punch[:5] + self.server.accounts[B].ina + punch[9:]
        self.assertEqual(self.send(live(mm.LIVE_RELAY, struct.pack(">I", ina[A]) + punch), B_AT, 2.6),
                         [(mm.header(mm.RELAY) + nonce(A_AT) + expected, A_AT)])

    def test_friends_requests_answers_removal_and_push(self):
        self.online()
        self.assertEqual(self.friends()[0][2:], (0, 0, bytes(16), 0, 0, bytes(8)))  # total, then an empty row
        pushed = self.send(live(mm.FRIEND_OP, friend_op_body(7, 1, A)), B_AT, 0.0)  # B asks A
        self.assertEqual([(packet[5], to) for packet, to in pushed], [(mm.DONE, B_AT), (mm.PUSH, A_AT)])
        self.assertEqual(mm.PUSH_BODY.unpack_from(pushed[1][0], mm.ASKED - mm.TOKEN), (1, B, NO_KID))
        row = self.friends()[0]
        self.assertEqual((row[3], row[5]), (B, mm.FRIEND_ONLINE | mm.FRIEND_RECEIVED_REQUEST))
        answered = self.send(live(mm.FRIEND_OP, friend_op_body(8, 2, B, 1)), A_AT, 0.0)
        self.assertEqual(mm.PUSH_BODY.unpack_from(answered[1][0], mm.ASKED - mm.TOKEN), (1, A, NO_KID))
        self.assertEqual(self.friends()[0][5], mm.FRIEND_ONLINE)
        self.assertEqual(self.friends(B_AT)[0][5], mm.FRIEND_ONLINE)
        self.send(live(mm.FRIEND_OP, friend_op_body(9, 3, B)), A_AT, 0.0)
        self.assertEqual(self.friends()[0][2:], (0, 0, bytes(16), 0, 0, bytes(8)))
        # A request to an unknown account, and an answer to one that is not there, are refused.
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(10, 1, 0x1234)), A_AT, 0.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(11, 2, C, 1)), A_AT, 0.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(12, 9, B)), A_AT, 0.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)

    def test_a_request_in_both_directions_makes_friends(self):
        self.online()
        self.send(live(mm.FRIEND_OP, friend_op_body(7, 1, B)), A_AT, 0.0)
        self.send(live(mm.FRIEND_OP, friend_op_body(8, 1, A)), B_AT, 0.0)  # the answer is implicit
        self.assertEqual(self.friends()[0][5], mm.FRIEND_ONLINE)
        self.assertEqual(self.server.requests, {})

    def test_game_invites_are_pushed_answered_and_show_on_the_friend(self):
        self.online()
        kid = self.put(slots=(1, 7, 0, 0))[2]
        self.logon(A_AT, A, now=0.5, state=0x13, kid=kid)  # presence: in that session
        self.send(live(mm.FRIEND_OP, friend_op_body(7, 1, A)), B_AT, 0.0)  # B asks A
        self.send(live(mm.FRIEND_OP, friend_op_body(8, 2, B, 1)), A_AT, 0.0)  # A accepts
        invited = self.send(live(mm.FRIEND_OP, friend_op_body(9, 4, B, kid=kid)), A_AT, 1.0)
        self.assertEqual(mm.PUSH_BODY.unpack_from(invited[1][0], mm.ASKED - mm.TOKEN), (2, A, kid))
        playing = mm.FRIEND_ONLINE | mm.FRIEND_PLAYING | mm.FRIEND_JOINABLE
        self.assertEqual(self.friends()[0][5], mm.FRIEND_ONLINE | mm.FRIEND_SENT_INVITE)  # B is idle
        self.assertEqual(self.friends(B_AT)[0][5], playing | mm.FRIEND_RECEIVED_INVITE)  # A plays
        self.assertEqual(self.friends(B_AT)[0][7], kid)  # the friend's session
        accepted = self.send(live(mm.FRIEND_OP, friend_op_body(10, 6, A, 1, kid)), B_AT, 2.0)
        self.assertEqual(mm.PUSH_BODY.unpack_from(accepted[1][0], mm.ASKED - mm.TOKEN), (3, B, kid))
        self.assertEqual(self.friends()[0][5], mm.FRIEND_ONLINE | mm.FRIEND_INVITE_ACCEPTED)
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(11, 6, A, 0, kid)), B_AT, 2.5,
                                              mm.DONE))[1], mm.E_INVALIDARG)  # answered once already
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(12, 5, B, kid=kid)), A_AT, 3.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)  # nothing pending to revoke
        # A new invite can be revoked while it is still pending.
        self.assertEqual(self.outcome(live(mm.FRIEND_OP, friend_op_body(13, 4, B, kid=kid)), A_AT, 4.0), 0)
        self.assertEqual(self.outcome(live(mm.FRIEND_OP, friend_op_body(14, 5, B, kid=kid)), A_AT, 5.0), 0)
        self.assertEqual(self.outcome(live(mm.FRIEND_OP, friend_op_body(15, 5, B, kid=kid)), A_AT, 5.5),
                         mm.E_INVALIDARG)
        self.assertEqual(self.friends()[0][5], mm.FRIEND_ONLINE)
        self.assertEqual(self.friends(B_AT)[0][5], playing)
        # An invite names this player's own session.
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(16, 4, B, kid=b"\x09" * 8)), A_AT, 6.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)
        # An invite names this player's own session.
        self.assertEqual(self.done(self.one(live(mm.FRIEND_OP, friend_op_body(15, 4, B, kid=b"\x09" * 8)), A_AT, 6.0,
                                              mm.DONE))[1], mm.E_INVALIDARG)

    def test_presence_shows_playing_and_joinable(self):
        self.online()
        kid = self.put(slots=(1, 7, 0, 0))[2]
        self.logon(A_AT, A, now=1.0, state=0x13, kid=kid)  # in a session
        self.server.add_friend(A, B)
        row = self.friends(B_AT, now=2.0)[0]
        self.assertEqual(row[3], A)
        self.assertEqual(row[5], mm.FRIEND_ONLINE | mm.FRIEND_PLAYING | mm.FRIEND_JOINABLE)
        self.assertEqual(row[7], kid)
        self.put(kid=kid, slots=(8, 0, 0, 0))  # full: still playing, not joinable
        self.assertEqual(self.friends(B_AT, now=2.0)[0][5], mm.FRIEND_ONLINE | mm.FRIEND_PLAYING)
        self.assertEqual(self.friends(A_AT, now=2.0)[0][7], NO_KID)  # B is idle

    def test_mutes_are_served_and_capped(self):
        self.online()
        self.server.mutes[A] = set(range(1, 10))
        packet = self.one(live(mm.MUTES, mutes_body(7)), A_AT, 0.0, mm.MUTELIST)
        req, hresult, count = mm.MUTELIST_BODY.unpack_from(packet)
        self.assertEqual((req, hresult, count), (7, 0, 9))
        self.assertEqual(struct.unpack_from(">Q", packet, mm.MUTELIST_BODY.size)[0], 1)
        self.server.mutes[A] = set(range(1, mm.MAX_MUTES + 20))
        self.assertEqual(mm.MUTELIST_BODY.unpack_from(self.one(live(mm.MUTES, mutes_body(8)), A_AT, 0.0,
                                                              mm.MUTELIST))[2], mm.MAX_MUTELIST)
        self.assertEqual(mm.MUTELIST_BODY.unpack_from(self.one(live(mm.MUTES, mutes_body(9)), B_AT, 0.0,
                                                              mm.MUTELIST))[2], 0)

    def test_feedback_is_counted_once_and_never_stored(self):
        self.online()
        self.assertEqual(self.done(self.one(live(mm.FEEDBACK, feedback_body(7, B, 2)), A_AT, 0.0, mm.DONE)),
                         (7, 0))
        self.assertEqual(self.server.feedback[(B, 2)], 1)
        self.assertEqual(self.done(self.one(live(mm.FEEDBACK, feedback_body(7, B, 2)), A_AT, 0.1, mm.DONE)),
                         (7, 0))  # the same request repeated is not counted twice
        self.assertEqual(self.server.feedback[(B, 2)], 1)
        self.assertNotIn("feedback", self.server.document())
        self.assertEqual(self.done(self.one(live(mm.FEEDBACK, feedback_body(8, 0x1234, 1)), A_AT, 0.2, mm.DONE))[1],
                         mm.E_INVALIDARG)

    def test_a_stat_read_holds_a_full_friends_list(self):
        def body(count):
            return mm.STAT_READ_BODY.pack(1, 2, count) + bytes(8 * count)
        self.assertTrue(mm.live_stat_read_ok(body(100)))  # the RANKINGS friends page asks for 100
        self.assertFalse(mm.live_stat_read_ok(body(101)))
        self.assertLessEqual(22 + len(body(100)), mm.MAX_PACKET)

    def test_stats_write_read_and_the_leaderboard(self):
        self.online()
        kid = self.put(slots=(1, 7, 0, 0))[2]
        self.logon(B_AT, B, now=1.0, state=0x13, kid=kid)
        # B joined A's session, and their clients CONNECTed, which is what
        # lets each write the other's row at the end of the fight.
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        ina_b = struct.unpack(">I", self.server.accounts[B].ina)[0]
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 1.0)), 2)
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 1.0)), 2)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(7, 1, A, ((1, 1, 100), (2, 1, 5)))),
                                            A_AT, 1.0, mm.DONE)), (7, 0))
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(8, 1, B, ((1, 1, 300),))), A_AT, 1.0,
                                            mm.DONE))[1], 0)  # the paired player's results, as the game writes both
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(9, 1, C, ((1, 1, 900),))), A_AT, 1.0,
                                            mm.DONE))[1], mm.E_ACCESSDENIED)
        rows = self.stats(req=10, now=2.0, xuids=(A, B))
        self.assertEqual([(row[0], row[2], row[3], row[4], row[5], row[7], row[8]) for row in rows],
                         [(10, 0, 2, 2, A, 2, 2), (10, 1, 2, 2, B, 1, 1)])
        self.assertEqual(mm.STAT_ITEM.unpack_from(rows[0][9], mm.ASKED - mm.TOKEN + mm.STATROW_BODY.size),
                         (1, 1, 100))
        leaders = self.leaders(req=11, now=2.0)
        self.assertEqual([(row[5], row[7]) for row in leaders], [(B, 1), (A, 2)])  # by rank, higher first
        self.assertEqual([row[4] for row in leaders], [2, 2])  # the board's total rows
        pivot = self.leaders(req=12, now=2.0, pivot=A)
        self.assertEqual([(row[5], row[7]) for row in pivot], [(A, 2)])
        self.assertEqual(self.leaders(req=13, now=2.0, board=9)[0][3], 0)  # an empty board says total 0
        self.assertEqual(self.leaders(req=14, now=2.0, board=9)[0][4], 0)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(15, 9, A, ())), A_AT, 2.0,
                                            mm.DONE))[1], 0)

    def test_a_stat_write_needs_a_pair_not_just_a_claimed_session(self):
        self.online()
        kid = self.put(now=0.0)[2]  # A hosts
        # B claims A's session but never CONNECTs: the server did not pair them,
        # so B cannot overwrite A's rankings.
        self.logon(B_AT, B, now=1.0, state=0x13, kid=kid)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(7, 1, A, ((1, 1, 1),))), B_AT, 1.0,
                                            mm.DONE))[1], mm.E_ACCESSDENIED)
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        ina_b = struct.unpack(">I", self.server.accounts[B].ina)[0]
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_a)), B_AT, 1.0)
        # One side's CONNECT introduces them but is not both meeting: the host
        # must CONNECT back before either may write the other's stats.
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(8, 1, A, ((1, 1, 1),))), B_AT, 1.0,
                                            mm.DONE))[1], mm.E_ACCESSDENIED)
        self.send(live(mm.CONNECT, connect_body(self.next_request(), ina_b)), A_AT, 1.0)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(9, 1, A, ((1, 1, 1),))), B_AT, 1.0,
                                            mm.DONE))[1], 0)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(10, 1, B, ((1, 1, 2),))), A_AT, 1.0,
                                            mm.DONE))[1], 0)
        # Six hours later the meeting is gone, even for two players who log on
        # again: only a new pair, both sides connecting, allows another write.
        later = mm.MET_SECONDS + mm.LIVE_SECONDS
        self.logon(A_AT, A, now=later)  # its packet sweeps the meeting away
        self.logon(B_AT, B, now=later)
        self.assertNotIn(frozenset((A, B)), self.server.met)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(11, 1, A, ((1, 1, 3),))), B_AT, later,
                                            mm.DONE))[1], mm.E_ACCESSDENIED)

    def test_an_account_keeps_only_so_many_stat_boards(self):
        self.online()
        for board in range(mm.MAX_BOARDS):
            self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(self.next_request(), board, A,
                                                                               ((1, 1, board),))), A_AT, 0.0,
                                                mm.DONE))[1], 0)
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(self.next_request(), mm.MAX_BOARDS, A,
                                                                           ((1, 1, 1),))), A_AT, 0.0,
                                            mm.DONE))[1], mm.E_INVALIDARG)
        # A board the account already has still takes a later write.
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(self.next_request(), 0, A,
                                                                           ((2, 1, 2),))), A_AT, 0.0,
                                            mm.DONE))[1], 0)
        self.assertEqual(sum(1 for (_, owner) in self.server.stats if owner == A), mm.MAX_BOARDS)

    def test_a_target_keeps_only_so_many_feedback_types(self):
        self.online()
        for kind in range(mm.MAX_FEEDBACK_KINDS):
            self.assertEqual(self.done(self.one(live(mm.FEEDBACK, feedback_body(self.next_request(), B, kind)),
                                                A_AT, 0.0, mm.DONE))[1], 0)
        self.assertEqual(self.done(self.one(live(mm.FEEDBACK, feedback_body(self.next_request(), B,
                                                                            mm.MAX_FEEDBACK_KINDS)),
                                            A_AT, 0.0, mm.DONE))[1], mm.E_INVALIDARG)
        self.assertEqual(self.done(self.one(live(mm.FEEDBACK, feedback_body(self.next_request(), B, 0)),
                                            A_AT, 0.0, mm.DONE))[1], 0)  # an existing type still counts
        self.assertEqual(sum(self.server.feedback.values()), mm.MAX_FEEDBACK_KINDS + 1)

    def test_a_board_cache_keeps_its_ranks_right_after_a_write(self):
        self.online()
        self.send(live(mm.STAT_WRITE, write_body(self.next_request(), 1, A, ((1, 1, 100),))), A_AT, 0.0)
        self.send(live(mm.STAT_WRITE, write_body(self.next_request(), 1, B, ((1, 1, 300),))), B_AT, 0.0)
        self.assertEqual([(row[5], row[7]) for row in self.leaders(req=8, now=1.0)], [(B, 1), (A, 2)])
        self.send(live(mm.STAT_WRITE, write_body(self.next_request(), 1, A, ((1, 1, 400),))), A_AT, 2.0)
        self.assertEqual([(row[5], row[7]) for row in self.leaders(req=9, now=3.0)], [(A, 1), (B, 2)])
        self.assertEqual([row[7] for row in self.stats(req=10, now=3.0, xuids=(A, B))], [1, 2])

    def test_the_ranking_cache_stays_bounded(self):
        for board in range(mm.MAX_RANKINGS + 20):
            self.server.board_ranking(board)
        self.assertEqual(len(self.server.rankings), mm.MAX_RANKINGS)
        self.assertNotIn(0, self.server.rankings)  # the oldest made room

    def test_a_group_7_board_keeps_and_returns_its_62_entries(self):
        self.online()
        stats = group_7_stats()
        self.assertEqual(len(stats), 61)
        packet = live(mm.STAT_WRITE, write_body(20, 7, A, stats))
        # 22 header, nonce and cookie, 17 request to count, 11 a stat.
        self.assertEqual(len(packet), mm.ASKED + 17 + 61 * mm.STAT_ITEM.size)
        self.assertLessEqual(len(packet), mm.MAX_PACKET)
        self.assertEqual(self.done(self.one(packet, A_AT, 1.0, mm.DONE)), (20, 0))
        self.assertEqual(len(self.server.stats[(7, A)]), 61)
        self.assertEqual(self.server.stats[(7, A)][0xFFFE], (2, 1234567890123))

        rows = self.stats(req=21, now=2.0, board=7)
        self.assertEqual(len(rows), 1)
        req, hresult, index, total, board_total, xuid, gamertag, rank, count = rows[0][:9]
        self.assertEqual((req, hresult, index, total, board_total, xuid, rank, count),
                         (21, 0, 0, 1, 1, A, 1, 61))
        self.assertEqual(gamertag, b"KASUMI".ljust(16, b"\0"))  # the -3 pseudo entry's text
        body = mm.ASKED - mm.TOKEN + mm.STATROW_BODY.size
        items = [mm.STAT_ITEM.unpack_from(rows[0][9], body + i * mm.STAT_ITEM.size) for i in range(count)]
        self.assertEqual(items, stats)  # sorted by id: 0..59, then the score
        self.assertEqual(len(rows[0][9]), 14 + mm.STATROW_BODY.size + 61 * mm.STAT_ITEM.size)
        self.assertLessEqual(len(rows[0][9]), mm.MAX_PACKET)

        leaders = self.leaders(req=22, now=2.0, board=7)
        self.assertEqual([(row[5], row[7], row[8]) for row in leaders], [(A, 1, 61)])  # rank and gamertag too

    def test_friend_requests_and_invites_expire(self):
        self.online()
        kid = self.put(slots=(1, 7, 0, 0))[2]
        self.send(live(mm.FRIEND_OP, friend_op_body(self.next_request(), 1, B)), A_AT, 0.0)
        self.send(live(mm.FRIEND_OP, friend_op_body(self.next_request(), 4, B, kid=kid)), A_AT, 0.0)
        self.assertEqual(sorted(self.server.requests), [(A, B)])
        self.assertIn((A, B), self.server.invites)
        self.send(live(mm.FRIENDS, friends_body(self.next_request())), A_AT, mm.INVITE_SECONDS + 1.0)
        self.assertEqual(self.server.invites, {})  # ten minutes, as before
        self.assertIn((A, B), self.server.requests)  # a request lasts longer
        self.send(live(mm.FRIENDS, friends_body(self.next_request())), A_AT, mm.REQUEST_SECONDS + 1.0)
        self.assertEqual(self.server.requests, {})

    def test_a_repeated_live_request_gets_its_first_answer_again(self):
        self.logon()
        first = self.put(req=2)
        self.assertEqual(self.put(req=2), first)  # a lost SESSION_OK is not a second session
        self.assertEqual(len(self.server.sessions), 1)
        search = self.send(live(mm.SEARCH, search_body(3)), A_AT, 0.0)
        self.assertEqual(self.send(live(mm.SEARCH, search_body(3)), A_AT, 0.0), search)

    def test_live_requests_need_a_cookie_and_are_capped_per_address(self):
        self.logon()
        answered = self.send(live(mm.FRIENDS, friends_body(6)), A_AT, 0.0, cookie=bytes(mm.TOKEN))
        self.assertEqual([(packet[5], to) for packet, to in answered], [(mm.COOKIE, A_AT)])
        self.assertEqual(self.server.friends, {})
        # 100 Live packets from one address between sweeps; the rest is dropped.
        answered = sum(bool(self.send(live(mm.FRIENDS, friends_body(1000 + i)), A_AT, 1.5))
                       for i in range(mm.LIVE_PER_SECOND + 5))
        self.assertEqual(answered, mm.LIVE_PER_SECOND)
        self.assertTrue(self.send(live(mm.FRIENDS, friends_body(1999)), A_AT, 2.5))
        # An oversized packet, a bad length and a too-short LIVE_RELAY get nothing.
        self.assertEqual(self.send(live(mm.FRIENDS, friends_body(7)) + bytes(mm.MAX_PACKET), A_AT, 3.0), [])
        self.assertEqual(self.send(live(mm.SEARCH, search_body(7) + b"x"), A_AT, 3.0), [])
        self.assertEqual(self.send(live(mm.LIVE_RELAY, b"\0\0\0\0"), A_AT, 3.0), [])

    def test_one_address_holds_only_so_many_cached_answers(self):
        self.logon()
        for index in range(mm.CACHE_PER_ADDRESS):
            self.send(live(mm.FRIENDS, friends_body(100 + index)), A_AT, 0.0)
        self.assertEqual(len(self.server.cache_keys[A_AT[0]]), mm.CACHE_PER_ADDRESS)
        self.assertEqual(len(self.server.live_cache), mm.CACHE_PER_ADDRESS)
        # Past its cap the address's oldest answer makes room, so a repeat of
        # its newest request is still answered from the cache, not applied twice.
        oldest = next(iter(self.server.cache_keys[A_AT[0]]))
        sessions = len(self.server.sessions)
        self.put(req=1000)
        self.put(req=1000)
        self.assertEqual(len(self.server.sessions), sessions + 1)
        self.assertEqual(len(self.server.live_cache), mm.CACHE_PER_ADDRESS)
        self.assertEqual(len(self.server.cache_keys[A_AT[0]]), mm.CACHE_PER_ADDRESS)
        self.assertNotIn(oldest, self.server.live_cache)
        # A sweep drops the expired answers and their keys. A bad cookie
        # changes nothing, so this request is not cached itself.
        self.send(live(mm.MUTES, mutes_body(1)), B_AT, mm.CACHE_SECONDS + 1.0, cookie=bytes(mm.TOKEN))
        self.assertEqual(self.server.live_cache, {})
        self.assertEqual(self.server.cache_keys, {})

    def test_a_connect_is_never_cached(self):
        self.online()
        self.put(now=0.0)  # A hosts
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        first = dict((to, packet) for packet, to in self.send(live(mm.CONNECT, connect_body(78, ina_a)), B_AT, 0.0))
        self.assertIn(A_AT, first)
        # A logs on from a new port inside B's repeats: the same request id names where A is now.
        moved = (A_AT[0], A_AT[1] + 1)
        self.logon(moved, A, now=1.0)
        again = dict((to, packet) for packet, to in self.send(live(mm.CONNECT, connect_body(78, ina_a)), B_AT, 1.5))
        self.assertIn(moved, again)
        _, public, _, _, _ = mm.LPEER_BODY.unpack_from(again[B_AT], mm.ASKED - mm.TOKEN)
        self.assertEqual(public, mm.endpoint(moved))
        self.assertNotIn((B_AT, nonce(B_AT), struct.pack(">I", 78), mm.CONNECT), self.server.live_cache)

    def test_answer_bytes_are_budgeted_per_address(self):
        self.online()
        stats = tuple((stat_id, 1, stat_id) for stat_id in range(mm.MAX_STATS))
        self.assertEqual(self.done(self.one(live(mm.STAT_WRITE, write_body(self.next_request(), 1, A, stats)), A_AT,
                                            0.5, mm.DONE))[1], 0)
        # One STAT_READ naming the same full row 100 times is a small request with a large answer.
        sent = 0
        for _ in range(20):
            sent += sum(len(packet) for packet, _ in self.send(live(mm.STAT_READ, read_body(self.next_request(), 1,
                                                                                          (A,) * 100)), B_AT, 1.0))
        self.assertLessEqual(sent, mm.LIVE_BYTES_PER_SECOND)
        self.assertGreater(sent, 0)
        # The next second has a new budget.
        self.assertTrue(self.send(live(mm.STAT_READ, read_body(self.next_request(), 1, (A,))), B_AT, 2.5))

    def test_a_full_cache_leaves_no_empty_address_entries(self):
        self.logon()
        self.server.cache_keys = {}
        self.server.live_cache = {("x", i): ([], 1e9) for i in range(mm.MAX_CACHE)}
        self.send(live(mm.FRIENDS, friends_body(500)), A_AT, 0.0)
        self.assertNotIn(A_AT[0], self.server.cache_keys)

    def test_every_player_with_the_default_name_gets_one(self):
        tags = set()
        for index in range(14):
            proof = proof_for(bytes((90 + index,)) * 32)
            sender = (f"192.0.2.{100 + index}", 4000)
            req, hresult, ina, length, tag = self.logon(sender, xuid_for(proof), proof, tag=b"PLAYER", now=1.0)
            self.assertEqual(hresult, 0)
            tags.add(tag[:length].lower())
        self.assertEqual(len(tags), 14)

    def test_an_empty_answer_is_not_cached(self):
        self.online()
        ina_a = struct.unpack(">I", self.server.accounts[A].ina)[0]
        # A hosts nothing yet, so B's CONNECT gets nothing; the same request
        # repeated once A hosts must pair them, not replay the empty answer.
        self.assertEqual(self.send(live(mm.CONNECT, connect_body(77, ina_a)), B_AT, 0.0), [])
        self.put(now=0.5)
        self.assertEqual(len(self.send(live(mm.CONNECT, connect_body(77, ina_a)), B_AT, 1.0)), 2)

    def test_an_answer_replaced_in_the_cache_is_counted_once(self):
        self.logon(now=0.0)
        for key, (answers, _) in list(self.server.live_cache.items()):
            self.server.live_cache[key] = (answers, 0.0)  # expired, but no sweep has run yet
        self.assertEqual(len(self.server.cache_keys[A_AT[0]]), 2)  # its NONCE and its LOGGED
        self.assertEqual(self.logon(now=0.5, req=1)[1], 0)  # the same requests, cached again
        self.assertEqual(len(self.server.cache_keys[A_AT[0]]), 2)

    def test_an_address_registers_only_a_few_accounts_a_minute(self):
        identities = [(xuid_for(proof_for(bytes((10 + index,)) * 32)), proof_for(bytes((10 + index,)) * 32))
                      for index in range(mm.ACCOUNTS_PER_ADDRESS + 1)]
        for index, (xuid, proof) in enumerate(identities[:mm.ACCOUNTS_PER_ADDRESS]):
            self.assertEqual(self.logon(now=1.0, xuid=xuid, proof=proof, tag=b"P%d" % index)[1], 0)
        late_xuid, late_proof = identities[-1]
        self.assertEqual(self.logon(now=1.0, xuid=late_xuid, proof=late_proof, tag=b"LATE")[1], mm.E_FAIL)
        self.assertEqual(self.logon(now=61.0, xuid=late_xuid, proof=late_proof, tag=b"LATE")[1], 0)  # next minute

    def test_an_address_registers_only_so_many_accounts_a_day(self):
        identities = [proof_for(bytes((10 + index,)) * 32) for index in range(mm.ACCOUNTS_PER_DAY + 1)]
        for index, proof in enumerate(identities[:mm.ACCOUNTS_PER_DAY]):
            now = 1.0 + 60.0 * (index // mm.ACCOUNTS_PER_ADDRESS)  # under the per-minute cap, after the first sweep
            self.assertEqual(self.logon(now=now, xuid=xuid_for(proof), proof=proof, tag=b"P%d" % index)[1], 0)
        late = identities[-1]
        self.assertEqual(self.logon(now=3600.0, xuid=xuid_for(late), proof=late, tag=b"LATE")[1], mm.E_FAIL)
        self.assertEqual(self.server.account_days[A_AT[0]], mm.ACCOUNTS_PER_DAY)  # a refusal is not an account
        self.assertEqual(self.logon(now=86400.0, xuid=xuid_for(late), proof=late, tag=b"LATE")[1], 0)  # next day

    def test_a_full_account_table_evicts_a_bare_account_first(self):
        self.fill_accounts()
        old, bare = 1 << 60, 1 << 60 | 1
        for xuid in self.server.accounts:
            self.server.boards_of[xuid] = {2}  # every account has a row but one
        del self.server.boards_of[bare]
        self.server.accounts[old].seen = 0.0  # offline longest, but it has stats
        proof = proof_for(bytes((200,)) * 32)
        self.assertEqual(self.logon(xuid=xuid_for(proof), proof=proof, tag=b"NEW", now=20.0)[1], 0)
        self.assertIn(old, self.server.accounts)
        self.assertNotIn(bare, self.server.accounts)

    def test_a_full_account_table_evicts_the_account_offline_longest(self):
        self.fill_accounts()
        victim, keeper, newest = 1 << 60, 1 << 60 | 1, 1 << 60 | 2
        for xuid in self.server.accounts:
            self.server.boards_of[xuid] = {2}  # all have stats, so age alone picks
        victim_ina = self.server.accounts[victim].ina
        self.server.accounts[victim].seen = 0.0
        self.server.accounts[keeper].seen = 5.0
        self.server.accounts[newest].seen = 15.0
        # References do not save the victim: everything that names it goes too.
        self.server.add_friend(keeper, victim)
        self.server.requests[(victim, newest)] = 0.0
        self.server.invites[(newest, victim)] = mm.Invite(bytes(8), None, 0.0)
        self.server.mutes[keeper] = {victim}
        self.server.mutes[victim] = {keeper}
        self.server.feedback[(victim, 1)] = 2
        self.server.feedback_kinds[victim] = {1}
        self.server.stats[(1, victim)] = {1: (1, 50)}
        self.server.stats_by_board[1] = {victim, keeper}
        self.server.boards_of[victim] = {1}
        self.server.board_ranking(1)  # its cached rank names it
        self.server.live_pairs[frozenset((victim, newest))] = mm.LivePair(0.0, bytes(8))
        self.server.met[frozenset((victim, newest))] = 0.0
        proof = proof_for(bytes((200,)) * 32)
        self.assertEqual(self.logon(xuid=xuid_for(proof), proof=proof, tag=b"NEW", now=20.0)[1], 0)
        self.assertNotIn(victim, self.server.accounts)
        self.assertNotIn(victim_ina, self.server.by_ina)
        self.assertIn(keeper, self.server.accounts)
        self.assertIn(newest, self.server.accounts)
        self.assertNotIn(victim, self.server.friends.get(keeper, set()))
        self.assertNotIn(keeper, self.server.friends.get(victim, set()))
        self.assertNotIn((victim, newest), self.server.requests)
        self.assertNotIn((newest, victim), self.server.invites)
        self.assertNotIn(victim, self.server.mutes.get(keeper, set()))
        self.assertNotIn(victim, self.server.mutes)
        self.assertNotIn((victim, 1), self.server.feedback)
        self.assertNotIn(victim, self.server.feedback_kinds)
        self.assertNotIn((1, victim), self.server.stats)
        self.assertNotIn(victim, self.server.stats_by_board[1])
        self.assertNotIn(victim, self.server.boards_of)
        self.assertNotIn(1, self.server.rankings)  # rebuilt without the victim
        self.assertEqual(self.server.board_ranking(1), [])
        self.assertNotIn(frozenset((victim, newest)), self.server.live_pairs)
        self.assertNotIn(frozenset((victim, newest)), self.server.met)

    def test_a_full_account_table_with_everyone_online_refuses_a_new_account(self):
        self.fill_accounts()
        for index, account in enumerate(self.server.accounts.values()):
            account.endpoint = ("192.0.2.5", 20000 + index)
            account.seen = 20.0
        proof = proof_for(bytes((201,)) * 32)
        self.assertEqual(self.logon(xuid=xuid_for(proof), proof=proof, tag=b"NEW", now=20.0)[1], mm.E_FAIL)
        self.assertEqual(len(self.server.accounts), mm.MAX_ACCOUNTS)

    def test_sessions_of_an_offline_host_leave_the_search(self):
        self.online()
        self.put(now=0.0)
        self.assertEqual(self.search(B_AT, now=5.0)[0][3], 1)
        self.assertEqual(self.search(B_AT, now=mm.LIVE_SECONDS + 1.0)[0][3], 0)  # the host went quiet
        self.assertEqual(self.server.sessions, {})

    def test_the_store_round_trips_accounts_friends_invites_mutes_and_stats(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.online()
            kid = self.put(slots=(1, 7, 0, 0))[2]
            self.send(live(mm.FRIEND_OP, friend_op_body(7, 1, A)), B_AT, 0.0)  # B asks A
            self.send(live(mm.FRIEND_OP, friend_op_body(8, 2, B, 1)), A_AT, 0.0)  # A accepts
            self.send(live(mm.FRIEND_OP, friend_op_body(9, 4, B, kid=kid)), A_AT, 1.0)
            self.server.mutes[A] = {C}
            self.send(live(mm.STAT_WRITE, write_body(10, 1, A, ((1, 1, 100), (2, 3, 0)))), A_AT, 1.0)
            self.send(live(mm.STAT_WRITE, write_body(11, 7, A, group_7_stats())), A_AT, 1.0)
            self.assertFalse(os.path.exists(path))  # at most every 30 s
            self.server.maybe_save(1.0)
            self.assertFalse(os.path.exists(path))
            self.server.maybe_save(31.0)
            self.assertTrue(os.path.exists(path))
            self.assertFalse(os.path.exists(path + ".tmp"))
            text = Path(path).read_text(encoding="utf-8")
            stored = json.loads(text)
            self.assertEqual(sorted(stored), ["accounts", "friends", "invites", "mutes", "requests", "stats", "version"])
            self.assertEqual([(entry["xuid"], entry["gamertag"]) for entry in stored["accounts"]],
                             [(A, "KASUMI"), (B, "AYANE"), (C, "HAYATE")])
            self.assertNotIn(PROOF.hex(), text)  # only the hash of the proof
            reloaded = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.assertEqual(sorted(reloaded.accounts), sorted([A, B, C]))
            self.assertEqual(reloaded.accounts[A].gamertag, b"KASUMI")
            # Its last logon is kept, in wall clock time: 31 s before the load.
            self.assertAlmostEqual(reloaded.accounts[A].seen, time.monotonic() - 31.0, delta=2.0)
            self.assertTrue(reloaded.are_friends(A, B))
            self.assertEqual(reloaded.invites[(A, B)].kid, kid)
            self.assertEqual(reloaded.mutes[A], {C})
            self.assertEqual(reloaded.stats[(1, A)], {1: (1, 100), 2: (3, 0)})
            self.assertEqual(reloaded.stats[(7, A)],
                             {stat_id: (kind, value) for stat_id, kind, value in group_7_stats()})
            # The reloaded account logs on with its proof and keeps its tag; sessions do not survive.
            req, hresult, ina, length, tag = self.logon(A_AT, A, now=40.0)
            self.assertEqual((hresult, tag[:length]), (0, b"KASUMI"))
            self.assertEqual(reloaded.sessions, {})
            self.assertEqual(self.server.stats[(1, A)], {1: (1, 100), 2: (3, 0)})

    def test_an_account_stored_before_the_challenge_keeps_its_key(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            # As a server from before the challenge wrote it: the hash of the proof it was sent.
            stored = {"version": 1, "accounts": [{"xuid": A, "proof": hashlib.sha256(PROOF).hexdigest(),
                                                  "gamertag": "KASUMI", "title": TITLE}]}
            Path(path).write_text(json.dumps(stored), encoding="utf-8")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            # That hash is the key: the account answers its first challenge, and is not registered again.
            self.assertEqual(self.challenge(now=1.0)[1], 1)
            req, hresult, ina, length, tag = self.logon(now=1.0, tag=b"OTHER")
            self.assertEqual((hresult, tag[:length], self.server.registered), (0, b"KASUMI", 0))
            # Nobody claims it with another secret, or with the proof alone.
            self.assertEqual(self.logon(B_AT, A, proof=OTHER_PROOF, now=2.0)[1], mm.E_ACCESSDENIED)
            req, known, nonce = self.challenge(B_AT, A, now=2.0)
            body = self.one(live(mm.LOGON, logon_body(req)), B_AT, 2.0, mm.LOGGED)
            self.assertEqual(mm.LOGGED_BODY.unpack(body)[1], mm.E_ACCESSDENIED)
            self.server.save(3.0)
            self.assertEqual(json.loads(Path(path).read_text(encoding="utf-8"))["accounts"][0]["proof"],
                             stored["accounts"][0]["proof"])

    def test_a_store_from_before_seen_loads_its_accounts_as_seen_now(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.logon(now=1.0)
            self.server.save(2.0)
            stored = json.loads(Path(path).read_text(encoding="utf-8"))
            for entry in stored["accounts"]:
                entry.pop("seen")
            Path(path).write_text(json.dumps(stored), encoding="utf-8")
            before = time.monotonic()
            reloaded = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            after = time.monotonic()
            self.assertGreaterEqual(reloaded.accounts[A].seen, before)
            self.assertLessEqual(reloaded.accounts[A].seen, after)

    def test_a_store_keeps_the_order_its_accounts_were_heard(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            clock, wall = (lambda: 1000.0), (lambda: 1_700_000_000.0)
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random, clock=clock, wall=wall)
            self.logon(A_AT, A, now=900.0)
            self.logon(B_AT, B, now=990.0)
            self.server.save(1000.0)
            # Five wall clock minutes pass while nothing runs.
            reloaded = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random, clock=clock,
                                     wall=lambda: 1_700_000_300.0)
            # The times are wall clock ones in the store and the loop clock's on
            # load, so the account heard first is still the one evicted first.
            self.assertLess(reloaded.accounts[A].seen, reloaded.accounts[B].seen)
            self.assertEqual((reloaded.accounts[A].seen, reloaded.accounts[B].seen), (600.0, 690.0))

    def test_a_monotonic_store_seen_is_loaded_as_now(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random,
                                        clock=lambda: 500.0, wall=lambda: 1_700_000_000.0)
            self.logon(now=1.0)
            self.server.save(2.0)
            stored = json.loads(Path(path).read_text(encoding="utf-8"))
            stored["accounts"][0]["seen"] = 12.0  # a store from before the wall clock
            Path(path).write_text(json.dumps(stored), encoding="utf-8")
            reloaded = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random,
                                     clock=lambda: 500.0, wall=lambda: 1_700_000_000.0)
            self.assertEqual(reloaded.accounts[A].seen, 500.0)

    def test_a_loaded_store_indexes_its_stats(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.online()
            self.send(live(mm.STAT_WRITE, write_body(self.next_request(), 1, A, ((1, 1, 100),))), A_AT, 0.0)
            self.send(live(mm.STAT_WRITE, write_body(self.next_request(), 1, B, ((1, 1, 300),))), B_AT, 0.0)
            self.server.save(1.0)
            reloaded = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.assertEqual(reloaded.stats_by_board, {1: {A, B}})
            self.assertEqual(reloaded.boards_of, {A: {1}, B: {1}})
            self.assertEqual(reloaded.board_ranking(1), [(1, B), (2, A)])  # only the board's rows were read

    def test_a_store_keeps_pending_requests_and_invites_with_their_ages(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            clock = lambda: 1000.0
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random, clock=clock,
                                        wall=lambda: 1_700_000_000.0)
            self.logon(A_AT, A, now=900.0)
            self.logon(B_AT, B, now=900.0)
            kid = self.put(now=900.0)[2]
            self.send(live(mm.FRIEND_OP, friend_op_body(self.next_request(), 1, B)), A_AT, 900.0)
            self.send(live(mm.FRIEND_OP, friend_op_body(self.next_request(), 4, B, kid=kid)), A_AT, 900.0)
            self.server.save(1000.0)
            # Both were made 400 s before the load, which is inside their lives.
            reloaded = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random, clock=clock,
                                     wall=lambda: 1_700_000_300.0)
            self.assertEqual(reloaded.requests[(A, B)], 600.0)
            self.assertEqual(reloaded.invites[(A, B)].seen, 600.0)
            self.assertEqual(reloaded.invites[(A, B)].kid, kid)

    def test_a_malformed_store_entry_is_skipped_with_one_log_line(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            data = {"version": 1, "accounts": [], "friends": [[A, B], [A], "x", 5, [A, B, C]],
                    "requests": [[A, B], ["bad"]], "invites": [], "mutes": [[A, B], 5],
                    "stats": [{"board": 1, "xuid": A, "stats": [[1, 1, 2], ["bad"]]},
                              {"board": 2, "xuid": A, "stats": [[1, 1, 2]]}]}
            Path(path).write_text(json.dumps(data), encoding="utf-8")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.assertTrue(server.are_friends(A, B))
            self.assertIn((A, B), server.requests)
            self.assertEqual(server.mutes[A], {B})
            self.assertNotIn((1, A), server.stats)
            self.assertEqual(server.stats[(2, A)], {1: (1, 2)})
            self.assertIn("skipped 7 malformed entries", errors.getvalue())
            self.assertEqual(len(errors.getvalue().splitlines()), 1)

    def test_a_store_number_too_large_for_float_is_skipped(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            huge = int("9" * 400)  # float() of it raises OverflowError, not ValueError
            data = {"version": 1, "accounts": [], "friends": [], "requests": [[A, B, huge]],
                    "invites": [{"kid": NO_KID.hex(), "from": A, "to": B, "answer": "", "seen": huge}],
                    "mutes": [], "stats": []}
            Path(path).write_text(json.dumps(data), encoding="utf-8")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.assertNotIn((A, B), server.requests)
            self.assertEqual(server.invites, {})
            self.assertIn("skipped 2 malformed entries", errors.getvalue())
            self.assertEqual(len(errors.getvalue().splitlines()), 1)

    def test_stored_stats_the_wire_cannot_carry_are_skipped(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            rows = [{"board": 1, "xuid": A, "stats": [[70000, 2, 1]]},   # stat id past 16 bits
                    {"board": 1, "xuid": A, "stats": [[1, 300, 1]]},     # type past 8 bits
                    {"board": 1, "xuid": A, "stats": [[1, 2, 2 ** 70]]},  # value past 64 bits
                    {"board": 2 ** 32, "xuid": A, "stats": [[1, 2, 1]]},
                    {"board": 1, "xuid": 2 ** 64, "stats": [[1, 2, 1]]},
                    {"board": 3, "xuid": B, "stats": [[1, 2, -5]]}]      # in range: kept
            data = {"version": 1, "accounts": [], "friends": [], "requests": [], "invites": [],
                    "mutes": [], "stats": rows}
            Path(path).write_text(json.dumps(data), encoding="utf-8")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.assertEqual(server.stats, {(3, B): {1: (2, -5)}})
            self.assertIn("skipped 5 malformed entries", errors.getvalue())

    def test_stored_xuids_and_titles_the_wire_cannot_carry_are_skipped(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            proof = (b"p" * 32).hex()
            data = {"version": 1,
                    "accounts": [{"xuid": A, "proof": proof, "gamertag": "AAA", "title": 2 ** 32},
                                 {"xuid": 2 ** 64, "proof": proof, "gamertag": "BIG", "title": 1}],
                    "friends": [[A, 2 ** 64], [A, -1]], "requests": [[A, 2 ** 64, 1]],
                    "invites": [{"kid": NO_KID.hex(), "from": A, "to": -1, "answer": ""}],
                    "mutes": [[A, 2 ** 64]], "stats": []}
            Path(path).write_text(json.dumps(data), encoding="utf-8")
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.assertEqual((server.accounts, server.requests, server.invites, server.mutes), ({}, {}, {}, {}))
            self.assertEqual(list(server.related(A)), [])
            self.assertIn("skipped 7 malformed entries", errors.getvalue())

    def test_a_failed_store_write_keeps_the_last_file_and_removes_its_temp(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.logon(now=1.0)
            self.server.save(2.0)
            good = Path(path).read_bytes()
            self.server.dirty = True  # something new to write
            errors = io.StringIO()
            with mock.patch.object(mm.os, "replace", side_effect=OSError("no space")), \
                    contextlib.redirect_stderr(errors):
                self.server.save(3.0)
            self.assertIn(f"matchmaker: cannot write {path}", errors.getvalue())
            self.assertEqual(Path(path).read_bytes(), good)  # the last good file
            self.assertEqual(os.listdir(folder), ["live.json"])  # and no temporary file beside it
            self.assertTrue(self.server.dirty)  # the next save tries again

    def test_a_store_write_does_not_write_through_a_stale_temp_path(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            os.mkdir(path + ".tmp")  # a leftover, or a trap, at the name an older save used
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.logon(now=1.0)
            self.server.save(2.0)
            self.assertEqual(json.loads(Path(path).read_text(encoding="utf-8"))["accounts"][0]["xuid"], A)
            self.assertTrue(os.path.isdir(path + ".tmp"))
            self.assertEqual(sorted(os.listdir(folder)), ["live.json", "live.json.tmp"])

    @unittest.skipUnless(os.name == "posix", "POSIX file modes")
    def test_the_store_is_written_owner_only(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "live.json")
            self.server = mm.Matchmaker(lambda: 1, store=path, random_bytes=self.random)
            self.logon(now=1.0)
            self.server.save(2.0)
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
            # A store made wider by an older server or by hand is narrowed by the next save.
            os.chmod(path, 0o644)
            self.server.dirty = True
            self.server.save(3.0)
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)

    def test_without_a_store_nothing_is_written(self):
        with tempfile.TemporaryDirectory() as folder:
            self.server = mm.Matchmaker(lambda: 1, store=None, random_bytes=self.random)
            self.logon()
            self.server.save(1.0)
            self.assertEqual(os.listdir(folder), [])
            self.assertIsNone(self.server.store)
            self.assertEqual(self.server.document()["accounts"][0]["xuid"], A)

    def test_live_status_holds_only_counts(self):
        self.online()
        self.put(now=0.0)
        line = self.server.live_status(1.0)
        self.assertEqual(line, "live: accounts 3, online 3, sessions 1, connects 0, pushes 0, friends 0, "
                               "requests 0, invites 0, mutes 0, stats 0, feedback 0")
        for secret in ("KASUMI", "AYANE", "HAYATE", A_AT[0], B_AT[0], str(A)):
            self.assertNotIn(secret, line)


# The shared byte vectors: one session that sends every Live kind, so the C
# client in recomp-runtime/live_client.c and this server cannot drift apart.
VECTOR_SECRET = bytes(range(32))
VECTOR_GAMERTAG = b"KASUMI"


def live_identity(secret=VECTOR_SECRET):
    """The XUID, proof and key a client derives from a secret."""
    proof = hashlib.sha256(b"doa2u-live" + secret).digest()
    return {"secret": secret.hex(), "xuid": xuid_for(proof), "proof": proof.hex(),
            "key": hashlib.sha256(proof).hexdigest()}


class VectorClient:
    """One player's side of the transcript: its nonce, its cookie and its request ids."""

    def __init__(self, server, sender, tag, digest=BUILD):
        self.server, self.sender, self.tag, self.digest = server, sender, tag, digest
        self.nonce = mm.endpoint(sender) + b"\0\0"  # 8 bytes, as a socket's nonce is
        self.cookie = bytes(8)
        self.request = 0

    def send(self, kind, body, now):
        """Sends one request; returns the packet and this player's answers."""
        self.request += 1
        packet = mm.header(kind) + self.nonce + self.cookie + struct.pack(">I", self.request) + body
        return packet, self.hear(packet, now)

    def retry(self, packet, now):
        """Sends the same request, id and all, with the cookie the server just gave."""
        packet = packet[:mm.ASKED - mm.TOKEN] + self.cookie + packet[mm.ASKED:]
        return packet, self.hear(packet, now)

    def logon(self, xuid, proof, now, state=0, kid=NO_KID):
        """A whole logon, as live_client.c does it: CHALLENGE, again with the cookie when the
        server asks, then LOGON under the same request id. Returns the packets and the answers."""
        packet, answers = self.send(mm.CHALLENGE, mm.CHALLENGE_BODY.pack(0, xuid)[4:], now)
        requests = [packet]
        if [answer[5] for answer in answers] == [mm.COOKIE]:  # the first ask, with no cookie
            packet, again = self.retry(packet, now)
            requests.append(packet)
            answers = answers + again
        assert answers[-1][5] == mm.NONCE
        _, known, nonce = mm.NONCE_BODY.unpack(answers[-1][mm.ASKED - mm.TOKEN:])
        body = logon_body(self.request, xuid, proof, self.digest, TITLE, state, kid, self.tag,
                          nonce if known else None)
        packet = mm.header(mm.LOGON) + self.nonce + self.cookie + body
        requests.append(packet)
        return requests, answers + self.hear(packet, now)

    def hear(self, packet, now):
        answers = []
        for answer, to in self.server.handle(packet, self.sender, now):
            if to != self.sender:
                continue
            if answer[5] == mm.COOKIE:
                self.cookie = answer[mm.ASKED - mm.TOKEN:][:mm.TOKEN]
            answers.append(answer)
        return answers


def live_vectors():
    """The transcript tools/live_vectors.json holds; every kind appears once."""
    identity = live_identity()
    me = identity["xuid"]
    serial = 0

    def random_bytes(count):
        nonlocal serial
        serial += 1
        return bytes((serial * 41 + i) & 0xff for i in range(count))

    server = mm.Matchmaker(lambda: 1, secret=VECTOR_SECRET, random_bytes=random_bytes)
    steps = []
    now = 0.0

    # Two other players, online and in sessions of their own, so search, find,
    # connect, friends and invites have someone to act on.
    b, c = VectorClient(server, B_AT, b"AYANE"), VectorClient(server, C_AT, b"HAYATE")
    b.logon(B, OTHER_PROOF, now)
    c.logon(C, THIRD_PROOF, now)
    b_kid = mm.SESSION_OK_BODY.unpack(b.send(mm.SESSION_PUT, mm.SESSION_PUT_BODY.pack(
        0, NO_KID, 1, 7, 0, 0, 0)[4:], now)[1][0][mm.ASKED - mm.TOKEN:])[2]
    c_kid = mm.SESSION_OK_BODY.unpack(c.send(mm.SESSION_PUT, mm.SESSION_PUT_BODY.pack(
        0, NO_KID, 1, 7, 0, 0, 0)[4:], now)[1][0][mm.ASKED - mm.TOKEN:])[2]
    b.logon(B, OTHER_PROOF, now, state=0x13, kid=b_kid)
    c.logon(C, THIRD_PROOF, now, state=0x13, kid=c_kid)
    ina_c = struct.unpack(">I", server.accounts[C].ina)[0]
    server.add_friend(me, C)
    server.requests[(B, me)] = now
    server.invites[(B, me)] = mm.Invite(b_kid, None, now)
    server.mutes[me] = {B, C}
    # C's own row on board 1, as C's client would write it after a match.
    c.send(mm.STAT_WRITE, mm.STAT_WRITE_BODY.pack(0, 1, C, 1)[4:] + mm.STAT_ITEM.pack(1, 1, 300), now)

    a = VectorClient(server, A_AT, VECTOR_GAMERTAG)

    def step(call, kind, body, now, **fields):
        packet, answers = a.send(kind, body, now)
        requests = [packet.hex()]
        if [answer[5] for answer in answers] == [mm.COOKIE]:  # the first ask, with no cookie
            first = answers
            packet, answers = a.retry(packet, now)
            requests.append(packet.hex())
            answers = first + answers
        entry = {"call": call, "kind": kind, "requests": requests,
                 "answers": [answer.hex() for answer in answers]}
        entry.update(fields)
        steps.append(entry)
        return answers

    def step_group(call, kind, bodies, now, **fields):
        requests, answers = [], []
        for body in bodies:
            packet, heard = a.send(kind, body, now)
            requests.append(packet.hex())
            answers.extend(heard)
        entry = {"call": call, "kind": kind, "requests": requests,
                 "answers": [answer.hex() for answer in answers]}
        entry.update(fields)
        steps.append(entry)
        return answers

    def step_logon(call, now, **presence):
        requests, answers = a.logon(me, bytes.fromhex(identity["proof"]), now, **presence)
        entry = {"call": call, "kind": mm.LOGON, "requests": [packet.hex() for packet in requests],
                 "answers": [answer.hex() for answer in answers]}
        if presence:
            entry.update(state=presence["state"], kid=presence["kid"].hex())
        steps.append(entry)
        return answers

    now += 0.5
    # The first logon of this XUID: the server has no account, so the LOGON carries the proof.
    answers = step_logon("logon", now)
    assert [answer[5] for answer in answers] == [mm.COOKIE, mm.NONCE, mm.LOGGED]
    assert answers[1][mm.ASKED - mm.TOKEN + 4] == 0
    now += 0.25
    answers = step("session_create", mm.SESSION_PUT,
                   mm.SESSION_PUT_BODY.pack(0, NO_KID, 1, 7, 0, 0, 2)[4:] + b"\x11\x22", now,
                   slots=[1, 7, 0, 0], attrs="1122")
    kid_a = mm.SESSION_OK_BODY.unpack(answers[0][mm.ASKED - mm.TOKEN:])[2]
    now += 0.25
    step("session_update", mm.SESSION_PUT,
         mm.SESSION_PUT_BODY.pack(0, kid_a, 2, 6, 0, 0, 1)[4:] + b"\x33", now,
         kid=kid_a.hex(), slots=[2, 6, 0, 0], attrs="33")
    now += 0.25
    # The heartbeat: the account is there now, so the LOGON answers the nonce with its key.
    answers = step_logon("presence", now, state=0x13, kid=kid_a)
    assert [answer[5] for answer in answers] == [mm.NONCE, mm.LOGGED]
    assert answers[0][mm.ASKED - mm.TOKEN + 4] == 1
    now += 0.25
    step("search", mm.SEARCH, mm.SEARCH_BODY.pack(0, 0, 0)[4:], now, procedure=0, filters="")
    now += 0.25
    step("find", mm.FIND, mm.FIND_BODY.pack(0, c_kid)[4:], now, kid=c_kid.hex())
    now += 0.25
    step("connect", mm.CONNECT, mm.CONNECT_BODY.pack(0, ina_c)[4:], now, ina=ina_c)
    now += 0.25
    # The pair's LIVE_RELAY carries the addressee's virtual address and one
    # guest datagram, with no request id. The server forwards it as a room RELAY
    # to the other player, stamping in the sender's real virtual address.
    datagram = guest(ina=0x0a0100ff, port=1000)
    packet = mm.header(mm.LIVE_RELAY) + a.nonce + a.cookie + struct.pack(">I", ina_c) + datagram
    forwarded = [answer for answer, to in server.handle(packet, A_AT, now) if to == C_AT]
    assert len(forwarded) == 1
    steps.append({"call": "live_relay", "kind": mm.LIVE_RELAY, "requests": [packet.hex()], "answers": [],
                  "destination": ina_c, "port": 1000})
    steps.append({"call": "live_relay_forward", "kind": mm.RELAY, "requests": [],
                  "answers": [forwarded[0].hex()], "destination": ina_c,
                  "ina": struct.unpack(">I", server.accounts[me].ina)[0], "port": 1000})
    now += 0.25
    step("friends", mm.FRIENDS, mm.FRIENDS_BODY.pack(0)[4:], now)
    now += 0.25
    step("friend_request", mm.FRIEND_OP, mm.FRIEND_OP_BODY.pack(0, 1, C, 0, NO_KID)[4:], now,
         op=1, xuid=C, answer=0, kid=NO_KID.hex())
    now += 0.25
    step("friend_answer", mm.FRIEND_OP, mm.FRIEND_OP_BODY.pack(0, 2, B, 1, NO_KID)[4:], now,
         op=2, xuid=B, answer=1, kid=NO_KID.hex())
    now += 0.25
    step("friend_remove", mm.FRIEND_OP, mm.FRIEND_OP_BODY.pack(0, 3, C, 0, NO_KID)[4:], now,
         op=3, xuid=C, answer=0, kid=NO_KID.hex())
    now += 0.25
    group = ((B, kid_a), (C, kid_a))
    step_group("game_invite", mm.FRIEND_OP,
               [mm.FRIEND_OP_BODY.pack(0, 4, xuid, 0, kid)[4:] for xuid, kid in group], now,
               op=4, xuids=[B, C], kid=kid_a.hex())
    now += 0.25
    step_group("revoke_invite", mm.FRIEND_OP,
               [mm.FRIEND_OP_BODY.pack(0, 5, xuid, 0, kid)[4:] for xuid, kid in group], now,
               op=5, xuids=[B, C], kid=kid_a.hex())
    now += 0.25
    step("answer_invite", mm.FRIEND_OP, mm.FRIEND_OP_BODY.pack(0, 6, B, 1, b_kid)[4:], now,
         op=6, xuid=B, answer=1, kid=b_kid.hex())
    now += 0.25
    step("mutes", mm.MUTES, mm.MUTES_BODY.pack(0)[4:], now)
    now += 0.25
    step("feedback", mm.FEEDBACK, mm.FEEDBACK_BODY.pack(0, C, 1)[4:], now, xuid=C, type=1)
    now += 0.25
    step("stat_write", mm.STAT_WRITE,
         mm.STAT_WRITE_BODY.pack(0, 1, me, 2)[4:] + mm.STAT_ITEM.pack(1, 1, 100) + mm.STAT_ITEM.pack(2, 1, 5),
         now, board=1, xuid=me, stats=[[1, 1, 100], [2, 1, 5]])
    now += 0.25
    step("stat_read", mm.STAT_READ,
         mm.STAT_READ_BODY.pack(0, 1, 2)[4:] + struct.pack(">QQ", me, C), now,
         board=1, xuids=[me, C], board_total=2)
    now += 0.25
    step("leaders", mm.LEADERS, mm.LEADERS_BODY.pack(0, 1, 0, 1, 10)[4:], now,
         board=1, pivot=0, first=1, count=10, board_total=2)
    now += 0.25
    # Group 7: a write of its 61 entries (ids 0..59 and the score -2), then a
    # read and a leaderboard row of all 61 stored stats.
    group7 = group_7_stats()
    step("stat_write_group7", mm.STAT_WRITE,
         mm.STAT_WRITE_BODY.pack(0, 7, me, len(group7))[4:] + b"".join(mm.STAT_ITEM.pack(*stat) for stat in group7),
         now, board=7, xuid=me, stats=[list(stat) for stat in group7])
    now += 0.25
    step("stat_read_group7", mm.STAT_READ,
         mm.STAT_READ_BODY.pack(0, 7, 1)[4:] + struct.pack(">Q", me), now,
         board=7, xuids=[me], board_total=1)
    now += 0.25
    step("leaders_group7", mm.LEADERS, mm.LEADERS_BODY.pack(0, 7, 0, 1, 4)[4:], now,
         board=7, pivot=0, first=1, count=4, board_total=1)
    now += 0.25
    step("session_delete", mm.SESSION_DEL, mm.SESSION_DEL_BODY.pack(0, kid_a)[4:], now, kid=kid_a.hex())
    now += 0.25
    pushed = server.push(me, 1, B, NO_KID, now)  # a friend changed: the client asks again
    steps.append({"call": "push", "kind": mm.PUSH, "requests": [],
                  "answers": [packet.hex() for packet, to in pushed if to == A_AT]})
    return {"protocol": 2, "identity": identity, "nonce": (mm.endpoint(A_AT) + b"\0\0").hex(),
            "digest": BUILD.decode(), "gamertag": VECTOR_GAMERTAG.decode(), "steps": steps}


class LiveVectorTest(unittest.TestCase):
    """The file both tool tests read: tools/live_vectors.json."""

    def test_the_shared_vectors_are_this_server_and_this_client(self):
        path = Path(__file__).with_name("live_vectors.json")
        committed = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(live_vectors(), committed)  # regenerate the file after a protocol change



if __name__ == "__main__":
    unittest.main()