# Matchmaking

Players find each other through a small UDP server,
[tools/matchmaker.py](../tools/matchmaker.py). It lists open rooms, pairs a
joiner with a host so both can punch through their routers, and relays a
pair's packets when punching fails. Joining by address stays as the
fallback. The game does not talk to it yet.

## Why one UDP server

The server must see each player's public UDP address and, for the relay,
forward UDP. Cloudflare Workers cannot take inbound UDP, so a Worker could
only list rooms and the game would also need STUN and TURN clients. One UDP
server does all three jobs, and the game reuses the UDP code in
`recomp-runtime/netplay.c` to talk to it.

## Where it runs

Any host with a public IPv4 address and one open UDP port (47700 by
default) and Python 3.10 or later. It needs no other packages:

    python3 tools/matchmaker.py --port 47700

It is not deployed. The relay costs bandwidth: a session sends about 60
packets of about 100 bytes a second each way, which is about 45 MB an hour
for a relayed match. A relay in one place adds the detour to players far
from it.

## What it stores

Only in memory, and nothing is logged:

- Each room: a random id, the host's public endpoint as the server sees it,
  the host's LAN endpoint, the build digest, name, input delay, whether
  its room is empty, and when it was first and last heard. A room that
  is not heard for 20 s is dropped. One address may hold 4 rooms, and the
  server holds 4,096.
- Each pair made by a join: the two public endpoints, kept 10 minutes past
  their last relayed packet.

The room list shows ids, names, ages and delays. Endpoints go only to the
two players of a join.

## Packets

Every packet starts with `D2MM`, version 1 and a kind. Numbers are big
endian. An endpoint is an IPv4 address and port in 6 bytes. Names are up to
32 bytes, zero padded after a length byte. Digests are the runner build's
64 hex characters.

| Kind | From | Body | Answer |
| --- | --- | --- | --- |
| 1 HOST | host, every 5 s | digest, delay, open, name, LAN endpoint | 2 HOSTED: room id, the host's public endpoint |
| 3 LIST | anyone | digest, padded to 512 bytes in all | 4 ROOMS: count, then room id, age in seconds, delay and name per room, oldest first |
| 5 JOIN | joiner | room id, digest, name, LAN endpoint | 6 PEER to each side: the other's public and LAN endpoints and name; or 7 GONE |
| 8 LEAVE | host | none | none |
| 9 RELAY | either player of a pair | up to 1,200 bytes | the same packet to the other player, up to 120 a second |

No answer is longer than the packet that asked for it, so a forged sender
address cannot turn the server into an amplifier. That is why LIST is
padded. A joined room leaves the list for 10 s while the two connect, and
returns if its host still says it is open.

## Connecting

1. The host opens its room, asks the router over UPnP as now, and says
   HOST from its room socket. The server records the address the packet
   came from, which is the host's public endpoint.
2. Quick match sends LIST, which returns only rooms on the same build, and
   joins the oldest. With none open, the player hosts instead.
3. JOIN gives both players the other's public and LAN endpoints. Both then
   send room packets to both endpoints for about 5 s. The first that
   answers becomes the peer. Sending first opens each router for the
   other's packets, which works behind most home routers.
4. Behind carrier NAT and some mobile networks punching fails. Both
   players then wrap their packets in RELAY to the server.
5. Each runner keeps its room's socket through the session and back to the
   room, so the path through the routers stays open for the session and a
   rematch.

## Not done yet

- The game's side: HOST heartbeats, a room list screen with quick match,
  punching to two endpoints, and wrapping packets in RELAY.
- The server does not check names; each player's runner checks the names
  it shows with the same check as profile names.
- Rooms carry no region, so quick match does not prefer nearby hosts.
- IPv4 only.
