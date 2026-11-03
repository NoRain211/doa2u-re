# Matchmaking

Players find each other through a small UDP server,
[tools/matchmaker.py](../tools/matchmaker.py). It lists open rooms, pairs a
joiner with a host so both can punch through their routers, and relays a
pair's packets when punching fails. Joining by address stays as the
fallback.

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

`--relay-only` relays every pair and sends no endpoints, so no player
learns another's address.

`--status` sets the seconds between status lines, 60 by default and 0 for
none. Each line counts rooms, open rooms and pairs, and the joins, relayed
packets and KiB, and packets over the relay cap since the line before. It
holds no addresses or names.

It is not deployed yet; [Deploying](#deploying) has the plan. The relay
costs bandwidth: a session sends about 60 packets of about 100 bytes a
second each way, which is about 45 MB an hour for a relayed match. A relay
in one place adds the detour to players far from it.

## What it stores

Only in memory, and the log holds only the status counts:

- Each room: a random id, the host's public endpoint as the server sees it,
  the host's LAN endpoint, the build digest, name, input delay, whether
  its room is empty, and when it was first and last heard. A room that
  is not heard for 20 s is dropped. One address may hold 4 rooms, and the
  server holds 4,096.
- Each pair made by a join: the two public endpoints, kept 10 minutes past
  their last relayed packet.

The room list shows ids, names, ages and delays. Endpoints go only to the
two players of a join.

## Deploying

The plan is an Oracle Cloud Always Free VM in a US region, named through
Cloudflare DNS, in the normal mode: punching first, relay when it fails.

1. Sign up with a US home region. Always Free resources live only in the
   home region, which cannot change later.
2. Create an instance from the Canonical Ubuntu 24.04 image on an Always
   Free shape (VM.Standard.E2.1.Micro, or a small VM.Standard.A1.Flex),
   with a public IPv4 address and SSH keys only. Ubuntu 24.04 has Python
   3.12; Oracle Linux 9's Python 3.9 is too old.
3. In the subnet's security list, add an ingress rule: source 0.0.0.0/0,
   UDP, destination port 47700.
4. On the VM, from a copy of this repository's `tools` folder, once:

       sudo install -D -m 0644 matchmaker.py /opt/doa2u-matchmaker/matchmaker.py
       sudo install -m 0644 matchmaker.service /etc/systemd/system/doa2u-matchmaker.service
       sudo iptables -I INPUT 1 -p udp --dport 47700 -j ACCEPT
       sudo netfilter-persistent save
       sudo systemctl daemon-reload
       sudo systemctl enable --now doa2u-matchmaker

   Oracle's Ubuntu images end the INPUT chain with a REJECT, so the port
   is opened there as well as in the security list. To update the server,
   copy `matchmaker.py` again and run
   `sudo systemctl restart doa2u-matchmaker`.
5. `journalctl -u doa2u-matchmaker -f` shows a status line every minute.
6. In Cloudflare DNS, add an A record such as `mm` with the VM's public
   address and proxy status DNS only. Cloudflare's proxy carries HTTP, so a
   proxied name would not reach a UDP server.
7. Leave MATCHMAKER on DEFAULT on two machines on different home
   networks. HOST ONLINE should say LISTED ON FIND MATCH and the
   other should quick-match it, which is also the first test of punching
   through real routers.

Oracle stops Always Free VMs that look idle for 7 days: CPU and network,
and memory on A1 shapes, all under 20%. The matchmaker always looks idle.

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
| 9 RELAY | either player of a pair | up to 1,200 bytes | the same packet to the other player, up to 250 a second |

No answer is longer than the packet that asked for it, so a forged sender
address cannot turn the server into an amplifier. That is why LIST is
padded. A joined room leaves the list for 10 s while the two connect, and
returns if its host still says it is open. A joiner repeats JOIN every
0.5 s until it hears, and the server answers its own claim again.

## In the game

MATCHMAKER in NETPLAY SETTINGS names the server as host:port, edited like
a JOIN ONLINE address with host names allowed, and is kept in
`.recomp-storage/netplay.txt` as a line such as
`matchmaker 203.0.113.7:47700`. Left empty, it shows DEFAULT and the
runner uses its built-in server. HOST ONLINE is PUBLIC or PRIVATE, chosen
with left and right on the menu or in the room while nobody has joined,
and kept in `netplay.txt`: a public room is listed while the server
answers, and a private room never contacts it. Turning a listed room
private sends LEAVE; if that is lost, the room drops off the list after
20 s without HOST. The
server's name is looked up on a background thread, so a slow answer does
not hold the menu. All of a room's matchmaking runs on the
room's own socket, in `recomp-runtime/netplay.c`. Its receives record the server's
answers and unwrap relayed packets as if the peer sent them, and its sends
wrap packets for a relayed peer. So the room, the session lobby and the
session run the same way over either path.

- HOST ONLINE says HOST every 5 s while its room is up, open while nobody
  is in it, and LEAVE when the host leaves. Its room's status says LISTED
  ON FIND MATCH once the server answers.
- FIND MATCH, below JOIN ONLINE on the Versus menu, lists up to six open
  rooms of the same build every 2 s, hiding names that fail the name
  check. A joins the chosen room and Y quick-matches.
- A PEER answer starts punching: room packets go to both endpoints every
  100 ms, a joiner takes the first that answers, and a host takes any
  player as before. After 5 s without an answer, or with no endpoints,
  the runner relays. A runner that hears a relayed packet relays too, so a
  path that works one way only still ends with both sides on the relay.
  The plates show RELAY beside the ping.
- A relayed session repeats its pads every 16 ms while it waits, where a
  direct one repeats every 4 ms.

Two runners on one machine with a local server: the host was listed, the
joiner's quick match reached it directly, and both played to frame 1,500
with identical guest RAM. With `--relay-only`, the same run went through
the relay with identical guest RAM. A test in
`recomp-runtime/netplay_lobby_test.cpp` stands in for the server and
checks punching, a relay without endpoints, and the fallback after 5 s.

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

- The default server is not running yet; until it is, FIND MATCH says
  THE MATCHMAKER CANNOT BE FOUND and hosts are not listed.
- Check how to keep Oracle from reclaiming the idle VM, such as a Pay As
  You Go account, before relying on it.
- Punching has only been tested on one machine, not through real routers.
- The server does not check names; each player's runner checks the names
  it shows with the same check as profile names.
- Rooms carry no region, so quick match does not prefer nearby hosts.
- IPv4 only.
