# Matchmaking

Players find each other through a small UDP server,
[tools/matchmaker.py](../tools/matchmaker.py). It is the Xbox Live service of
the game's own Live menus: accounts, session listings, friends, invites,
mutes, feedback and rankings, and it relays two paired players' packets when
punching fails ([Xbox Live](#xbox-live)).

## Why one UDP server

The server must see each player's public UDP address and, for the relay,
forward UDP. Cloudflare Workers cannot take inbound UDP, so a Worker could
not relay, and the game would also need STUN and TURN clients. One UDP
server does both jobs, and the runner's Live transport
(`recomp-runtime/live_transport.c`) talks to it over one UDP socket.

## Where it runs

Any host with a public IPv4 address and one open UDP port (47700 by default)
and Python 3.10 or later. It needs no other packages:

    python3 tools/matchmaker.py --port 47700

Without `--store` everything is in memory, and a restart forgets it.

`--relay-only` relays every Live pair and sends no endpoints, so no player
learns another's address.

`--store PATH` keeps the Live accounts and what belongs to them in one JSON
file (see [What Live stores](#what-live-stores)).

`--status` sets the seconds between status lines, 60 by default and 0 for
none. Each line counts the accounts, sessions, friends, requests, invites,
mutes, stats and feedback, then the relayed packets and KiB and the packets
over a cap since the line before. It holds no addresses or names.

The runner's built-in server is a deployed one; [Deploying](#deploying) says
how it is set up. The relay costs bandwidth: a Live session sends about 60
packets of about 100 bytes a second each way, which is about 45 MB an hour
for a relayed match. A relay in one place adds the detour to players far from
it.

## Deploying

The default server is an Oracle Cloud Always Free VM, named through
Cloudflare DNS, in the normal mode: punching first, relay when it fails.
These steps set up one like it.

1. Sign up with a US home region. Always Free compute is created only in
   the home region, so treat the choice as permanent; the region nearest
   the middle of the players keeps relayed matches even.
2. Set a small budget alert. Usage inside the Always Free limits stays
   free, and the alert catches anything that is not.
3. Create an instance from the Canonical Ubuntu 24.04 image on the
   VM.Standard.E2.1.Micro shape (1/8 OCPU, 1 GB, Always Free, two per
   account), with a public IPv4 address and SSH keys only. It is far more
   than the server needs and avoids the A1 capacity shortages. Ubuntu
   24.04 has Python 3.12; Oracle Linux 9's Python 3.9 is too old.
4. In the subnet's security list, add an ingress rule: source 0.0.0.0/0,
   UDP, destination port 47700.
5. On the VM, from a copy of this repository's `tools` folder, once:

       sudo install -D -m 0644 matchmaker.py /opt/doa2u-matchmaker/matchmaker.py
       sudo install -m 0644 matchmaker.service /etc/systemd/system/doa2u-matchmaker.service
       sudo iptables -I INPUT 1 -p udp --dport 47700 -j ACCEPT
       sudo apt-get update
       sudo DEBIAN_FRONTEND=noninteractive apt-get install -y iptables-persistent
       sudo netfilter-persistent save
       sudo systemctl daemon-reload
       sudo systemctl enable --now doa2u-matchmaker

   Oracle's Ubuntu images end the INPUT chain with a REJECT, so the port
   is opened there as well as in the security list. To update the server,
   copy `matchmaker.py` again and run
   `sudo systemctl restart doa2u-matchmaker`.
   `journalctl -u doa2u-matchmaker -f` shows a status line every minute.
6. From home, run the game with `RECOMP_LIVE_SERVER=<VM address>:47700`
   and log on. The next status line
   counts the account, and `online 1` while it stays logged on, which proves
   both firewalls let the port through.
7. In Cloudflare DNS, add an A record such as `mm` with the VM's public
   address and proxy status DNS only. Cloudflare's proxy carries HTTP, so a
   proxied name would not reach a UDP server.
8. Start the game with `RECOMP_LIVE_SERVER=<name from step 7>:47700` on two
   machines on different home networks; without it they would test the
   built-in server instead. Both should log on and see the other's
   session in the Live pages, which is also the first test of punching
   through real routers.

## Packets

Every packet starts with `D2MM`, version 2 and a kind. A request then carries
the socket's 8-byte nonce, the server's 8-byte cookie, a 4-byte request id and
the body. LIVE_RELAY has no request id: its body starts with the receiver's
virtual address. The server's packets carry the header and the nonce of the
client they go to, so an answer's body starts 14 bytes in; RELAY, COOKIE and
PUSH have no request id. Numbers are big endian. An endpoint is an IPv4
address and port in 6 bytes. Digests are the runner build's 64 hex
characters.

| Kind | From | Body | Answer |
| --- | --- | --- | --- |
| 9 RELAY | server | a paired player's body, up to 1,200 bytes | — |
| 10 COOKIE | server | the cookie | the client stores it and sends its last request again with it |

Kinds 1 to 8 and 11 belonged to the Versus room protocol, which is gone;
their numbers stay unused.

UDP source addresses can be forged, so neither side takes one on trust. The
server answers a request without this minute's or the last minute's cookie
with COOKIE only and stores nothing. The cookie is the first 8 bytes of an
HMAC-SHA256 of the sender's endpoint and the minute, and the server takes
this minute's and the last one's; a request with the last one's is answered
and also gets this minute's. Only a client that receives at its address
learns its cookie, so a forged address cannot register an account, change
another player's friends or use a pair's relay. The runner drops a packet
from the server's address that does not carry its socket's nonce, so a
forged LPEER cannot send a player elsewhere and a forged RELAY cannot carry
pads.

Answers may be longer than their ask, since a listing carries attributes;
the cookie check is what stops a forged sender turning the server into an
amplifier.

## Xbox Live

The game's own Live menus talk to the same server, on the same socket and
port, in kinds 12 and up. Every packet is at most 1,200 bytes, except a
LIVE_RELAY's 1,200-byte body, and every request except LIVE_RELAY carries a
request id which its answer echoes, so a lost answer is asked for again
rather than applied twice.

Nothing about a player reaches the log: the status line says only
`live: accounts 3, online 1, sessions 1, connects 0, pushes 0, friends 2,
requests 1, invites 0, mutes 0, stats 4, feedback 0, relayed 0 (0 KiB), over
the cap 0`.

### Live packets

| Kind | From | Body | Answer |
| --- | --- | --- | --- |
| 12 LOGON | player, after each NONCE | the CHALLENGE's request, XUID, 32-byte answer, 64-hex build digest, title id, presence state, presence session id, gamertag length and 15 bytes | 13 LOGGED: request, result, the player's virtual address, the gamertag actually given |
| 14 SESSION_PUT | host | request, session id (zeros creates), 4 slot counts, attribute length and bytes | 15 SESSION_OK: request, result, session id, 16-byte key |
| 16 SESSION_DEL | host | request, session id; zero logs off | 25 DONE |
| 17 SEARCH | anyone | request, procedure, filter length and bytes | 18 RESULT, one per session |
| 19 FIND | anyone | request, session id | 18 RESULT |
| 20 CONNECT | either player | request, the other player's virtual address | 21 LPEER to both |
| 21 LPEER | server | the other's virtual address, public endpoint, LAN endpoint, the pair's 8-byte token, relay flag | — |
| 22 FRIENDS | anyone | request | 23 FRIEND, one per entry |
| 24 FRIEND_OP | anyone | request, op, XUID, answer, session id | 25 DONE |
| 25 DONE | server | request, result | — |
| 26 PUSH | server | type, XUID, session id | — |
| 27 MUTES | anyone | request | 28 MUTELIST: request, result, count, then the XUIDs |
| 29 FEEDBACK | anyone | request, XUID, type | 25 DONE |
| 30 STAT_WRITE | anyone | request, board, XUID, count (at most 64), then id, type and value per stat | 25 DONE |
| 31 STAT_READ | anyone | request, board, count, then the XUIDs | 32 STATROW, one per XUID |
| 32 STATROW | server | request, result, index, total, board total, XUID, gamertag, rank, count (at most 64), then the stats | — |
| 33 LEADERS | anyone | request, board, pivot XUID (0 = by rank), first rank, count | 32 STATROW, one per row |
| 34 LIVE_RELAY | either player of a Live pair | the other player's virtual address, then one guest datagram | 9 RELAY to that player: the datagram, with the sender's virtual address filled in |
| 35 CHALLENGE | player, before each LOGON, so every 5 s | request, XUID | 36 NONCE: request, 1 when the XUID has an account and 0 when it has none, 16-byte nonce |

A RESULT row is the session id, its key, the host's XUID, gamertag and
virtual address, the four slot counts and the attributes. A FRIEND row is the
XUID, gamertag, flags, title id and the friend's session id. PUSH types are 1
friends changed, 2 invite received and 3 invite answered; the client asks for
its friend list again and needs no acknowledgement. A list with no entries is
one row whose total is 0, so a reader always has a total.

A guest datagram is `D2GD`, a kind, the sender's four-byte virtual address,
its two-byte port and a body: kind 1 carries the game's payload, kind 2 is a
punch whose body is the pair's token. LIVE_RELAY names its receiver, so
the server forwards it as RELAY and fills in the sender's virtual
address; a datagram to anyone but a pair's other player is dropped. These
packets use the relay's per-pair and per-address caps instead of the Live
request rate, and they are never cached: a game repeats them legitimately.

### Accounts

A client makes three values from its 32-byte secret. The proof is SHA-256 of
`doa2u-live` and the secret. The key is SHA-256 of the proof. The XUID is the
type field 0x0009 followed by the first six bytes of the key. The secret and
the key never cross the wire, and the proof crosses it once for an account.

A logon is two requests under one request id:

1. CHALLENGE names the XUID. The server answers NONCE with 16 random bytes
   and whether it has an account for that XUID. The nonce belongs to that
   socket (its address and its nonce, as a cookie belongs to an address),
   that request id and that XUID, and lasts 10 s. A CHALLENGE said again
   while its nonce lasts gets the same nonce.
2. LOGON answers the nonce. The first LOGON that names a nonce uses it up,
   whatever the result, so a LOGON without a challenge, or with one that is
   used, expired or another socket's, is refused with access denied. The
   server keeps that first LOGON's answer with the nonce: the same LOGON
   said again, as a client does until it hears, gets the same answer while
   the nonce lasts, and changes nothing.

For an XUID with an account, the 32-byte answer is HMAC-SHA256, keyed by the
account's key, of the nonce, the kind byte (12) and every other field of the
LOGON body in order: the request id, the XUID, the build digest, the title
id, the presence state, the presence session id, the gamertag length and the
gamertag. The server makes the same HMAC from the key it holds, and any other
answer is refused with access denied, so the game shows a logon error.

For an XUID without an account, the answer is the proof itself. The server
checks that the XUID is that proof's, and registers the account with the
proof's SHA-256 as its key, its gamertag and the build digest. Search results
carry XUIDs, so this check keeps one player from claiming another's account
by guessing.

The client sends the proof to a server only until its first successful logon
there. After a LOGGED with success it adds the server, as `host:port` in
lowercase, to a list beside its identity file ([In the game](#in-the-game)).
When a NONCE from a listed server says the XUID has no account, the client
does not answer it: the logon fails with access denied, a heartbeat also
signs the player out, and one line on stderr names the server, the list and
the line to remove. A server that did lose the account is registered with
again only after the player removes its line from the list.

A heartbeat is a whole logon every 5 s, the CHALLENGE and the LOGON both;
20 s without one means the player is offline and their sessions leave the
search. The other design was a key agreed at the first logon that signs the
later requests. It only helps when every request is signed and counted
against replay, because every request after a logon is already tied to its
socket alone; a signed heartbeat among unsigned requests adds nothing.
Challenging each LOGON keeps one rule (no LOGON is taken without an answer to
a fresh nonce) and one path for the logon and the heartbeat on both sides.
The cost is a second round trip for each heartbeat. The client sends each
half again every 500 ms for up to 5 s, as it does any request, so a lost
packet is repeated inside the heartbeat instead of waiting for the next one.

What a reader of the wire, or of the store, can do:

- A CHALLENGE, a NONCE, and the LOGON of an account that exists give nothing
  that logs anyone on. The answer fits one nonce, which only its own socket
  can use and which is gone once it is answered. Sent again from its own
  socket, the LOGON gets its first answer again while the nonce lasts,
  which changes nothing, and access denied after that.
- The LOGON that registers an XUID carries the proof. Whoever reads that one
  packet can make the key and log on as that account from then on. The
  client sends it when a NONCE says the XUID has no account, and only to a
  server that is not in its list, so the first registration with a server
  stays exposed to a reader of that packet.
- Someone who can read the client's packets and send it a NONCE as the
  server can say "no account" to be sent the proof. That works until the
  client's first successful logon on that server and not after it: the
  client withholds the proof from a listed server. The list names a server
  as the runner is configured, so the same server under another name or
  port is a new one until its first logon there.
- A server that no longer has an account (after an eviction, a lost store,
  or a restart without `--store`) looks the same to the client as that
  forged NONCE, so the logon fails until the player removes the server's
  line. The registration that follows shows the proof once more.
- That same person can pass a logon on. They ask for a challenge from their
  own socket under the player's request id and XUID, send the player its
  nonce as the server before the server's own arrives, and send the
  player's answer from their own socket. The answer fits the nonce and
  names no socket, so the server logs them on as the player and refuses the
  player's own LOGON. It gives no key: each logon and heartbeat takes
  another answer from the player. The client has nothing to check a NONCE
  against, so only a NONCE the server signs would close it
  ([Not done yet](#not-done-yet)).
- The server holds each account's key, in memory and in the store. Whoever
  reads the store can log on as every account in it. The key does not give
  the proof or the secret.
- The key is the same on every matchmaker a player uses, so a key or a proof
  learned on one opens that account on the others. An operator learns the
  proof of every account that registers with their matchmaker.
- When a proof or key is known to someone else, the only remedy is a new
  identity: a new secret, so a new XUID without the old one's friends and
  stats. The XUID fixes its key, so neither the player nor an operator can
  change the key of an account.

Signatures made with a private key would close the registration and the
store: the server would hold only a public key. Neither side has them without
a new dependency, so they are in [Not done yet](#not-done-yet).

A store from before the challenge needs no change. It kept each account's
SHA-256 of the proof under the name `proof`, and that value is the key, so
such an account answers its first challenge and is not registered again; no
value that can be replayed is sent for it. What the earlier protocol showed
stays shown: it sent the proof in every LOGON, so anyone who read one of an
account's LOGONs then can make its key now. A reset would not help that
account. The server holds only the hash of the proof, which such a reader can
make too, so it cannot tell the owner from the reader, and a rule that let
the first of them set a new key would let the reader take the account from
its owner instead of sharing it. Such an account needs a new identity.

A client from before the challenge sends LOGON without CHALLENGE. It has no
nonce, so it gets LOGGED with access denied, registers nothing, and the game
shows a logon error; its packet still shows its proof to anyone who reads it.
A new client gets no answer to CHALLENGE from an earlier server, to which it
is an unknown kind, and its logon fails after 5 s. The version stays 2, so
the earlier client is answered with that error instead of with nothing.

A gamertag is unique among accounts: the server gives a taken one a digit, 1
to 9, and past those the XUID's own hex digits, so every player who sends the
default name gets one; LOGGED answers with the one it gave. A gamertag that
fails the profile-name check (`netplay_names.c`, mirrored in the server and
checked against it by `test_matchmaker.py`) becomes `PLAYER`. An account
whose gamertag came from the default name, or fails the check, takes the name
a later LOGON sends. A SESSION_DEL with a zero session id logs off at once:
the runner sends one when it quits, which takes its sessions out of the
search and shows it offline to friends.

Each account gets a virtual address `10.x.y.z`, unique among the accounts,
never 10.0.* or 10.255.*, which the game puts in the XNADDR's ina and
inaOnline. The matchmaker keeps it for the account, so friends and invites
still name it after a reconnect; a restart gives new ones. The game's MAC is
`00:50:F2` and the low three bytes of the XUID.

### Sessions, search and joins

Creating a session makes its id and 16-byte key; updating names the id. The
four counts are `XOnlineMatchSessionCreate`'s: public filled, public open,
private filled, private open. The game's search record keeps the Live
service's order instead, public open, private open, public filled, private
filled, and `recomp-runtime/live_xapi.c` writes them that way. A session is
listed while it has an open slot (public or private, slots 1 or 3 in the
create order), its host is online and its digest matches the searcher's,
newest first, at most 25. A host does not see its own session. FIND answers
any session still there, so an invite still works when the session has
filled.

CONNECT picks the other player's account by virtual address, and only when
that player hosts a Live session from SESSION_PUT that is still alive, when
the two already have a pair from an earlier CONNECT, or when both have a
live pair with the same session host. The last case is a lobby's members
reaching each other: the host pairs with each member as it joins, the game
has the two fighters exchange lockstep and a spectator subscribe to a
fighter's state stream, and the host may not be fighting. It reveals no
address the host could not pass on itself. Otherwise CONNECT is dropped:
virtual addresses are sequential, so without this any account could make any
online player reveal their public endpoint. Joining a friend means CONNECTing
to the host of the friend's session, which hosts one. Both get an LPEER with
the other's endpoint and the pair's token, whichever side asks first, and
then punch or relay; a punch carries that token instead of the socket's
nonce, which stays with the server. The side that receives an LPEER it did
not ask for CONNECTs back, so both sides have asked. The pairing is by the
two XUIDs, lasts 10 minutes past its last relayed packet or CONNECT to a
player who still hosts, and an address may open at most 16 (pairs others
opened with its players do not count); `--relay-only` zeroes both endpoints
and sets the relay flag. Two lobby members who answer each other are a full
pair, so a match between them can write its results. The LAN endpoint stays zero:
LOGON carries none, so Live players behind one router punch through their
public addresses. A token belongs to one pairing: once it expires, another
CONNECT makes a new pair and a new token.

### Friends, invites, mutes and rankings

`FRIEND_OP` ops are 1 request, 2 answer a request, 3 remove, 4 game invite, 5
revoke an invite and 6 answer an invite. A request in both directions makes
friends at once. The friend flags are the game's: online, playing, joinable,
sent or received request, sent or received invite, invite accepted or
rejected. An invite names the inviter's own session and stays until it is
answered or revoked; answering it tells the inviter which way it went.

A Live mutelist has no request that adds a name in the game's API, so the
server only serves what the store holds or a test sets: the game reads it at
logon and aborts the logon when it fails, so it always answers.

Feedback is counted per target and type in memory and never stored, and the
count is not in the log. A board's rank is its lowest-numbered stat id, high
value first, ties to the lower XUID; a double is compared as its value. Stats
may be written for oneself, or for the other player of a pairing in which
both sides CONNECTed, for six hours from when the second of them connected,
as the game writes both fighters' results at the end of a match. One side's
CONNECT is only an introduction, and a repeated CONNECT does not move those
six hours later; a rematch in a new pairing starts them again. A session id a
client chooses at LOGON does not authorize a
write, since search results show it.

A stat is 11 bytes on the wire: the id as two bytes (so the negative pseudo
ids are their two's complement, -2 = 0xfffe), the type in one and the value as
a signed 64-bit number. The game's largest RANKINGS group is group 7 with 62
entries: ids 0..59, the score -2 and the rank -1. A write carries the group
without -1, so 61 entries, and fits one STAT_WRITE packet: 22 bytes of header,
nonce and cookie, 17 of request, board, XUID and count, then 671 for the
stats, 710 in all. A read or leaderboard row is also one packet: 14 bytes of
header and nonce, 43 of body down to the count, then 11 per stat, 728 for
group 7's 61 stored stats and 761 at the 64-stat cap. Both are under 1,200, so
a write is never split and STATROW already sends one row per answer (`index`
and `total` track them); a resent write is the same request id and changes
nothing twice.
Of the negative ids only -2 is stored, as an ordinary int64, because the game
writes it like any other stat; -1 comes from the row's rank and -3 from its
gamertag, so the client fills those two from the row's fields.

### Caps

A Live packet over 1,200 bytes, a body whose length fields do not match and
an unknown kind get no answer; a LIVE_RELAY's body may be 1,200 bytes, as a
RELAY's is, so that packet is 1,222 at most. Per address the server
takes 100 Live packets between its once-a-second sweeps and sends 256 KiB of
answers (a request is small and its answer may be 100 rows of stats; past
that the answer is dropped and the client's resend gets it later), and
registers 4 new accounts a minute and 16 a day (the day counts accounts
made, not refused attempts); a player's requests are 2 a second at most, so
only a flood meets those. A nonce is kept for its 10 s, with its LOGON's
answer once it has one: 32 for an address, whose oldest makes room for a new
one, and 12,288 in all, where the oldest of all makes room. A heartbeat every
5 s keeps two of a player's, so a full account table of players online keeps
8,192 and leaves a third of the total free. A
LOGON whose nonce went that way is refused, and a heartbeat refused signs its
player out, so the share of an address keeps a flood's cost with the
addresses that flood: filling what players leave free takes 128 of them, and
the oldest it then drops are mostly nonces already answered. A LIVE_RELAY is not one of those
100: it has the relay's caps, 250 a second for a pair direction and 500 for
an address, and an address may open 16 pairs. The rest are
per entry: 4,096 accounts, 4 sessions per account and 4,096 sessions,
672-byte attributes, 512-byte filters, 25 search results, 100 friends and
pending requests, 250 mutes (147 fit one MUTELIST packet, so it is cut
there), 48 boards per account (the game's whole 0..47 board range), 256 board
rankings cached (the oldest makes room), 16 feedback types per target, 64 stats per write and 64 per account
and board (a full group-7 write is 61; the game reads back its
62 entries, the last a rank the client makes from the row), 100 XUIDs per
read, 100 leader rows. An invite lasts 10 minutes and an unanswered friend
request a week. When the account table is full, a new XUID drops the
account that has been offline longest, those without stats or friends first,
with everything that names it:
friendships in both directions, pending requests and invites, stats rows,
feedback, mutes, Live pairings and cached rankings. If every account is
online, the new account is refused. Those caps are per address, so filling
the table takes 256 addresses at the daily cap, or 256 days from one;
a flood that gets there costs the evicted players the data the file held for
them. A repeated request id from the same
socket and nonce gets its first answer again for 60 s, which is what makes a
resent SESSION_PUT one session and a resent FEEDBACK one count. At most
2,048 answers are cached, 64 of them from one address, whose oldest answer
makes room for its newest; past the total a request is answered but not
kept, so a resend past it is applied again. An empty answer is never cached,
so its resend is tried afresh, and neither is CONNECT: applying it twice
changes nothing, its LPEERs must name the endpoints as they are now, and its
resends then meet the CONNECT cap. Nor are CHALLENGE and LOGON: a nonce's
own entry repeats it and its LOGON's answer, so a logon never depends on
room in this cache.

### What Live stores

Without `--store` everything is in memory and a restart forgets it. With
`--store PATH` one JSON file holds:

- each account's XUID, its key (the hex SHA-256 of its proof, under the
  name `proof` as earlier stores have it), its gamertag, title id and when
  it was last heard, as wall clock time. The proof itself is never written,
  and neither is a client's address. The key is what answers a challenge, so
  whoever can read the file can log on as its accounts
  ([Accounts](#accounts)): keep it readable by the server's user alone. A
  store from before the last-heard field existed, or holding the old
  loop-clock seconds, loads its accounts as heard now;
- friends as pairs of XUIDs, and pending friend requests the same way plus
  when each was made;
- invites: session id, from, to, when it was made and the answer once there
  is one;
- each player's mute list, as pairs of XUIDs;
- stats: board, XUID and its entries.

Sessions, addresses, keys, presence, feedback, Live pairings and the CONNECT
times that allow writing another player's stats are not written. The file is
written to a fresh temporary file beside it at most every 30 s and at exit,
flushed and synced to the disk there, and renamed over `PATH`, so a crash
leaves the last good file; a write that fails is reported on stderr and
removes its temporary file, and the last good file stays. On POSIX systems
the new file is owner-only (`0600`), so a store the server did not create
that way is narrowed on its next save. A missing file is no error; a damaged
one is reported on stderr and left alone.

### In the game

The Live client in `recomp-runtime/live_client.c` sends those kinds through
the transport's socket: a request goes out again every 500 ms and fails after
5 s, a logon is a CHALLENGE and then the LOGON that answers its NONCE, a
logged-on player logs on again that way every 5 s with its presence, a PUSH
makes it ask for its friend list again, and an LPEER becomes the peer intro the
transport punches with. A game datagram to a paired player goes out as a
LIVE_RELAY that names that player's virtual address, and the server sends it
on as RELAY. The transport owns the request's nonce and cookie and
the one retry a COOKIE answer asks for; the client fills in only the magic,
version, kind and body. The LOGON carries DOA2U's title id, `0x54430006`,
from the XBE certificate (VA `0x10184` + 8), so a friend row names the title a
friend logged on with. It logs on with `recomp-runtime/live_identity.c`'s
secret, `<storage>/live-identity.bin` (or the file `RECOMP_LIVE_IDENTITY`
names), and the name of the newest game profile, cut to 15 bytes, or
`PLAYER` when that name fails the same check as profile names. The servers
that account has logged on to are listed in the same folder, in a text file
with the identity file's name and `.servers.txt` after it
(`live-identity.bin.servers.txt`), one `host:port` a line; a server in it is
never sent the proof again ([Accounts](#accounts)), and making a new identity
removes the list. The server is
the built-in one; `RECOMP_LIVE_SERVER=host:port` overrides it, with host
names allowed, and a bare `host` keeps the built-in server's port. The
server's name is looked up from the game's first request on, not at start,
so a player who never opens DOA ONLINE sends nothing. The lookup runs on a
thread of its own, because it can take seconds and requests come from the
frame thread: the first request starts it, and the socket opens and a
request goes out with the first one sent after the name is found, which is
the client's resend 500 ms later. A lookup that fails is tried again 5 s
after it began. An address in place of a name needs no lookup.

## Not done yet

- The default server is one small VM. While it is down, a client cannot log
  on or list sessions, and its requests fail after 5 s.
- Punching has only been tested on one machine, not through real routers.
- The server listens on IPv4 only, so a player who has IPv6 alone (NAT64)
  cannot reach it.
- The LOGON that registers an XUID carries its proof, and the server and
  its store hold every account's key ([Accounts](#accounts)). Whoever reads
  that one packet, the store, or a LOGON from before the challenge can log
  on as that account on every matchmaker that has it, and the remedy is a
  new identity. A signature made with a private key, with only the public
  key registered and stored, would close all three; it needs a signature
  scheme that neither side has without a new dependency.
- The client withholds the proof from a server it has logged on to, so a
  forged "no account" NONCE gets the proof only at a first registration.
  The client cannot tell that forgery from a server that lost its accounts:
  the player must remove the server's line from the list beside the identity
  file to register again, and that registration is exposed like the first.
- The client cannot tell the server's NONCE from a forged one. Someone who
  reads a player's packets and answers before the server can have the
  player answer a nonce the server gave them, and is logged on as the
  player for the 20 s a logon lasts ([Accounts](#accounts)). A NONCE
  signed by the server, with a key the client keeps from its first logon
  there, would close it; it needs the same signature scheme.
- Only the logon is authenticated. Each later request is tied to its socket
  by the address and the cookie, and nothing is encrypted. Someone who can
  read a logged-on player's packets and send from that player's address can
  act as the player while the session lasts, but cannot log on after it.
- A flood from many addresses can fill the 4,096-account table, and a new
  account then evicts the player offline longest with their friends, stats
  and mutes. The caps are per address, so it takes at least 256 of them,
  and eviction prefers accounts with no stats or friends; a global cap or
  keeping evicted rows would close it.
- The Live pages have not been run against this server in the game yet: the
  client is tested against the shared byte vectors
  ([tools/live_vectors.json](../tools/live_vectors.json)), not end to end.
  Mutes can only be set in the file, since the game's Live API has no page
  that adds one.
