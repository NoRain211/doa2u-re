#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""UDP matchmaker for netplay rooms.

Lists open rooms, pairs a joiner with a host so both can punch through their
routers, and relays a pair's packets when punching fails. One UDP port, no
accounts, everything in memory. docs/matchmaking.md describes the packets.
"""
import argparse
import asyncio
import os
import socket
import struct

MAGIC = b"D2MM"
VERSION = 1
HOST, HOSTED, LIST, ROOMS, JOIN, PEER, GONE, LEAVE, RELAY = range(1, 10)
DIGEST = 64  # the runner build's hex SHA-256; rooms only meet the same build
NAME = 32
LIST_BYTES = 512  # a list request is padded to this, so no answer outgrows its ask
ROOM_SECONDS = 20.0  # a host says HOST every 5 s
CLAIM_SECONDS = 10.0  # a joined room stays off the list while the two connect
PAIR_SECONDS = 600.0  # a pair's relay lasts this long after its last packet
MAX_ROOMS = 4096
ROOMS_PER_ADDRESS = 4
RELAY_PER_SECOND = 120  # per direction; a session sends 60
MAX_RELAY = 1200

HOST_BODY = struct.Struct(">64sBBB32s6s")  # digest, delay, open, name length, name, LAN endpoint
JOIN_BODY = struct.Struct(">I64sB32s6s")  # room id, digest, name length, name, LAN endpoint
ENTRY = struct.Struct(">IHBB32s")  # room id, age in seconds, delay, name length, name


def header(kind):
    return MAGIC + bytes((VERSION, kind))


def endpoint(address):
    """An IPv4 address and port as 6 bytes, in network order."""
    return socket.inet_aton(address[0]) + struct.pack(">H", address[1])


def peer(public, lan, name):
    return header(PEER) + endpoint(public) + lan + bytes((len(name),)) + name.ljust(NAME, b"\0")


class Room:
    def __init__(self, room_id, host, now):
        self.id, self.host, self.born = room_id, host, now
        self.claimed_until = 0.0


class Pair:
    def __init__(self, other, now):
        self.other, self.used = other, now
        self.second, self.count = int(now), 0


class Matchmaker:
    def __init__(self, random_id=lambda: int.from_bytes(os.urandom(4), "big")):
        self.rooms = {}  # by the host's public endpoint
        self.by_id = {}
        self.pairs = {}  # by endpoint, both ways
        self.random_id = random_id
        self.swept = 0.0

    def handle(self, data, sender, now):
        """Returns the (packet, endpoint) answers to one packet from sender."""
        if len(data) < 6 or data[:4] != MAGIC or data[4] != VERSION:
            return []
        if now - self.swept >= 1.0:
            self.sweep(now)
        kind, body = data[5], data[6:]
        if kind == HOST and len(body) == HOST_BODY.size:
            return self.host(body, sender, now)
        if kind == LIST and len(data) == LIST_BYTES:
            return self.list(body[:DIGEST], sender, now)
        if kind == JOIN and len(body) == JOIN_BODY.size:
            return self.join(body, sender, now)
        if kind == LEAVE:
            self.remove(self.rooms.get(sender))
            return []
        if kind == RELAY and len(body) <= MAX_RELAY:
            return self.relay(body, sender, now)
        return []

    def host(self, body, sender, now):
        digest, delay, is_open, length, name, lan = HOST_BODY.unpack(body)
        if length > NAME:
            return []
        room = self.rooms.get(sender)
        if room is None:
            same_address = sum(1 for host in self.rooms if host[0] == sender[0])
            if len(self.rooms) >= MAX_ROOMS or same_address >= ROOMS_PER_ADDRESS:
                return []
            room_id = self.random_id()
            while room_id == 0 or room_id in self.by_id:
                room_id = self.random_id()
            room = self.rooms[sender] = self.by_id[room_id] = Room(room_id, sender, now)
        room.digest, room.delay, room.open = digest, delay, is_open != 0
        room.name, room.lan, room.seen = name[:length], lan, now
        return [(header(HOSTED) + struct.pack(">I", room.id) + endpoint(sender), sender)]

    def list(self, digest, sender, now):
        rooms = sorted((room for room in self.rooms.values()
                        if room.digest == digest and room.open and room.claimed_until <= now and room.host != sender),
                       key=lambda room: room.born)
        rooms = rooms[:(LIST_BYTES - 7) // ENTRY.size]
        answer = header(ROOMS) + bytes((len(rooms),))
        for room in rooms:
            answer += ENTRY.pack(room.id, min(int(now - room.born), 65535), room.delay, len(room.name), room.name)
        return [(answer, sender)]

    def join(self, body, sender, now):
        room_id, digest, length, name, lan = JOIN_BODY.unpack(body)
        room = self.by_id.get(room_id)
        if length > NAME or room is None or room.digest != digest or not room.open or room.claimed_until > now:
            return [(header(GONE) + struct.pack(">I", room_id), sender)]
        room.claimed_until = now + CLAIM_SECONDS
        self.pairs[sender] = Pair(room.host, now)
        self.pairs[room.host] = Pair(sender, now)
        return [(peer(sender, lan, name[:length]), room.host), (peer(room.host, room.lan, room.name), sender)]

    def relay(self, body, sender, now):
        pair = self.pairs.get(sender)
        back = self.pairs.get(pair.other) if pair is not None else None
        if back is None or back.other != sender:
            return []
        if int(now) != pair.second:
            pair.second, pair.count = int(now), 0
        pair.count += 1
        if pair.count > RELAY_PER_SECOND:
            return []
        pair.used = back.used = now
        return [(header(RELAY) + body, pair.other)]

    def remove(self, room):
        if room is not None:
            del self.rooms[room.host]
            del self.by_id[room.id]

    def sweep(self, now):
        self.swept = now
        for room in [room for room in self.rooms.values() if now - room.seen > ROOM_SECONDS]:
            self.remove(room)
        for key in [key for key, pair in self.pairs.items() if now - pair.used > PAIR_SECONDS]:
            del self.pairs[key]


class Server(asyncio.DatagramProtocol):
    def __init__(self, matchmaker):
        self.matchmaker = matchmaker
        self.loop = asyncio.get_running_loop()

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, sender):
        for packet, target in self.matchmaker.handle(data, sender[:2], self.loop.time()):
            self.transport.sendto(packet, target)


async def serve(address, port):
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(lambda: Server(Matchmaker()), local_addr=(address, port))
    print(f"matchmaker: UDP {address}:{port}", flush=True)
    await asyncio.Event().wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--address", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=47700)
    arguments = parser.parse_args()
    asyncio.run(serve(arguments.address, arguments.port))


if __name__ == "__main__":
    main()
