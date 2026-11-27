#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""UDP service for the port's Xbox Live, kinds 12 and up.

Accounts that log on by answering a challenge, session listings, friends, invites, mutes, feedback and
stats, and the relay two paired players use when punching fails. One UDP port,
no external service, everything in memory. docs/matchmaking.md describes the
packets. --store keeps accounts and what belongs to them in one JSON file;
without it they last as long as the process.
"""
import argparse
import asyncio
import hashlib
import hmac
import json
import os
import re
import socket
import struct
import sys
import tempfile
import time

MAGIC = b"D2MM"
VERSION = 2
# 1 to 8 and 11 were the Versus room protocol; their numbers stay unused.
RELAY, COOKIE = 9, 10  # the server forwards a paired player's packet, or hands out its cookie
TOKEN = 8  # a client's nonce and the server's cookie
ASKED = 6 + 2 * TOKEN  # where a request's body starts, after the nonce and cookie
COOKIE_SECONDS = 60  # a cookie lasts this epoch and the next
DIGEST = 64  # the build's hex SHA-256; Live sessions only meet the same build
PAIR_SECONDS = 600.0  # a pair's relay lasts this long after its last packet
CONNECTS_PER_SECOND = 10  # per address; a client repeats CONNECT until it hears
RELAY_PER_SECOND = 250  # per direction: 60 frames a second, plus repeats while waiting for the other player
RELAY_PER_ADDRESS = 2 * RELAY_PER_SECOND  # a second, so one address cannot relay through every pair it makes
PAIRS_PER_ADDRESS = 16  # each lasts PAIR_SECONDS; without a cap one address adds 10 a second
MAX_RELAY = 1200

# The Live kinds. Big endian, like the relay's.
(LOGON, LOGGED, SESSION_PUT, SESSION_OK, SESSION_DEL, SEARCH, RESULT, FIND, CONNECT, LPEER, FRIENDS,
 FRIEND, FRIEND_OP, DONE, PUSH, MUTES, MUTELIST, FEEDBACK, STAT_WRITE, STAT_READ, STATROW, LEADERS) = range(12, 34)
LIVE_RELAY = 34  # either player of a Live pair: destination virtual address, then one guest datagram
CHALLENGE, NONCE = 35, 36  # before every LOGON: the server's nonce, which the LOGON answers
MAX_PACKET = 1200  # every Live packet, both ways, except a LIVE_RELAY's larger body
XUID_TYPE = 0x0009  # the XUID's type field; the rest is the first six bytes of the proof's hash
LIVE_SECONDS = 20.0  # LOGON is the heartbeat every 5 s; this long silent is offline
CHALLENGE_BYTES = 16
CHALLENGE_SECONDS = 10.0  # a LOGON answers within a round trip, and its client gives up after 5 s
MAX_CHALLENGES = 3 * 4096  # nonces not yet ended, two a player from heartbeats; past it the oldest makes room
CHALLENGES_PER_ADDRESS = 32  # and from one address, whose own oldest makes room
CACHE_SECONDS = 60.0  # a repeated request gets its first answer again, for this long
MAX_CACHE = 2048  # cached answers in all
CACHE_PER_ADDRESS = 64  # and from one address, past which answers simply are not kept
MET_SECONDS = 6 * 3600.0  # both players CONNECTing lets each write the other's stats for this long
MAX_ACCOUNTS = 4096
MAX_SESSIONS = 4096
SESSIONS_PER_ACCOUNT = 4
ACCOUNTS_PER_ADDRESS = 4  # new XUIDs a minute; each keeps a gamertag
ACCOUNTS_PER_DAY = 16  # and a day, so one address takes 256 days to fill MAX_ACCOUNTS
MAX_ATTRIBUTES = 0x2a0  # what XOnlineMatchSearchParse writes into a search record
MAX_FILTERS = 512
MAX_FRIENDS = 100  # RECOMP_LIVE_MAX_FRIENDS
MAX_MUTES = 250  # RECOMP_LIVE_MAX_MUTES
MAX_STAT_WRITE = 64  # RECOMP_LIVE_MAX_STAT_ATTRS; group 7 writes 61
MAX_STATS = 64  # kept per account and board; group 7 reads back 62
MAX_BOARDS = 48  # distinct boards an account's stats may name; DOA2U uses 0..47
MAX_FEEDBACK_KINDS = 16  # distinct feedback types one target may receive
MAX_FEEDBACK_COUNT = 1 << 20  # a type's count saturates here, so a flood cannot grow the store without bound
MAX_RANKINGS = 256  # cached board rankings; the oldest makes room for a new board
MAX_READ_STATS = 100  # xuids in one STAT_READ: a full friends list (RECOMP_LIVE_MAX_FRIENDS), 831 bytes
MAX_LEADERS = 100  # RECOMP_LIVE_MAX_LEADER_ROWS
MAX_RESULTS = 25  # RECOMP_LIVE_MAX_RESULTS
INVITE_SECONDS = 600.0
REQUEST_SECONDS = 7 * 24 * 3600.0  # an unanswered friend request lasts a week
WALL_EPOCH = 1e9  # a wall clock time is past 2001; a store from before it held the monotonic clock
LIVE_PER_SECOND = 100  # Live packets per address between sweeps; resends every 500 ms are 2
LIVE_BYTES_PER_SECOND = 256 * 1024  # answer bytes per address between sweeps: three full 100-row group-7 reads
STORE_SECONDS = 30.0  # at most this often is --store written
BAD_ENTRY = (KeyError, TypeError, ValueError, AttributeError, IndexError,
             OverflowError, struct.error)  # an entry raising one of these is skipped, never the whole store
DATAGRAM_MAGIC = b"D2GD"  # the game's datagram, as live_transport.c wraps it
DATAGRAM_KINDS = (1, 2)  # a game datagram, or a punch the transports exchange
DATAGRAM_HEAD = 11  # magic, version, 4-byte sender virtual address, 2-byte guest port
LIVE_RELAY_DESTINATION = 4  # the virtual address a LIVE_RELAY body starts with
HRESULT_OK = 0
E_FAIL = 0x80004005
E_INVALIDARG = 0x80070057
E_ACCESSDENIED = 0x80070005
FRIEND_ONLINE = 0x00000001
FRIEND_PLAYING = 0x00000002
FRIEND_JOINABLE = 0x00000010
FRIEND_SENT_INVITE = 0x04000000
FRIEND_RECEIVED_INVITE = 0x08000000
FRIEND_INVITE_ACCEPTED = 0x10000000
FRIEND_INVITE_REJECTED = 0x20000000
FRIEND_SENT_REQUEST = 0x40000000
FRIEND_RECEIVED_REQUEST = 0x80000000
PRESENCE_SESSION = 0x10  # XONLINE_NOTIFICATION's in-a-session bit

# Live bodies. Requests carry the nonce and cookie, answers only the nonce, so
# every body below starts at ASKED or at ASKED - TOKEN.
CHALLENGE_BODY = struct.Struct(">IQ")  # request, XUID
NONCE_BODY = struct.Struct(">IB16s")  # request, whether the XUID has an account, nonce
LOGON_BODY = struct.Struct(">IQ32s64sII8sB15s")  # request, XUID, answer, digest, title, presence, kid, tag length, tag
LOGON_ANSWER = slice(12, 44)  # where the answer is in a LOGON body
LOGGED_BODY = struct.Struct(">IIIB15s")  # request, hresult, virtual address, tag length, tag
SESSION_PUT_BODY = struct.Struct(">I8s4IH")  # request, kid, four slot counts, attribute length
SESSION_OK_BODY = struct.Struct(">II8s16s")  # request, hresult, kid, key
SESSION_DEL_BODY = struct.Struct(">I8s")
SEARCH_BODY = struct.Struct(">IIH")  # request, procedure, filter length
FIND_BODY = struct.Struct(">I8s")
CONNECT_BODY = struct.Struct(">II")  # request, the other player's virtual address
LPEER_BODY = struct.Struct(">I6s6s8sB")  # virtual address, public endpoint, LAN endpoint, pair token, relay
RESULT_BODY = struct.Struct(">IIBB")  # request, hresult, index, total
RESULT_ROW = struct.Struct(">8s16sQ16sI4IH")  # kid, key, host XUID, host gamertag, host address, slots, attributes
FRIEND_BODY = struct.Struct(">IBBQ16sII8s")  # request, index, total, XUID, gamertag, flags, title, kid
FRIENDS_BODY = struct.Struct(">I")  # request
FRIEND_OP_BODY = struct.Struct(">IBQB8s")  # request, op, XUID, answer, kid
DONE_BODY = struct.Struct(">II")  # request, hresult
PUSH_BODY = struct.Struct(">BQ8s")  # type, XUID, kid
MUTELIST_BODY = struct.Struct(">IIB")  # request, hresult, count
MUTES_BODY = struct.Struct(">I")  # request
FEEDBACK_BODY = struct.Struct(">IQI")  # request, XUID, type
STAT_WRITE_BODY = struct.Struct(">IIQB")  # request, board, XUID, count
STAT_ITEM = struct.Struct(">HBq")  # stat id, type, value
# The offline record boards (ranking groups 2 and 3 in g_board_group 0x7bf940:
# Time Attack, the shortest victory and Survival), which the game reads its own
# row from before it uploads a record.
RECORD_BOARDS = frozenset({7, 8} | set(range(15, 31)))
STAT_RATING = 0xFFFE  # -2, XONLINE_STAT_RATING: the score a board ranks by (ranking_write_stats_board 0x297e60)
STAT_READ_BODY = struct.Struct(">IIB")  # request, board, count
STATROW_BODY = struct.Struct(">IIBBIQ16sIB")  # request, hresult, index, total, board total, XUID, gamertag, rank, count
LEADERS_BODY = struct.Struct(">IIQIB")  # request, board, pivot XUID, first rank, count
# One MUTELIST packet carries this many: 1,200 bytes less the header, nonce and count.
MAX_MUTELIST = (MAX_PACKET - (ASKED - TOKEN) - MUTELIST_BODY.size) // 8


def header(kind):
    return MAGIC + bytes((VERSION, kind))


def endpoint(address):
    """An IPv4 address and port as 6 bytes, in network order; the server listens on IPv4 only."""
    return socket.inet_aton(address[0]) + struct.pack(">H", address[1])


def over(counts, address, cap):
    """Counts one more for the address since the last sweep; true once that passes the cap."""
    counts[address] = counts.get(address, 0) + 1
    return counts[address] > cap


def sync_folder(folder):
    """Makes a rename in folder durable; Windows cannot open a folder, so it never can there."""
    try:
        handle = os.open(folder, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    except OSError:
        return
    try:
        os.fsync(handle)
    except OSError:
        pass
    finally:
        os.close(handle)


class Links(dict):
    """(source XUID, target XUID) -> value, also found by either XUID without a scan."""

    def __init__(self):
        super().__init__()
        self.of = {}  # XUID -> the keys that name it

    def __setitem__(self, key, value):
        if key not in self:
            for xuid in key:
                self.of.setdefault(xuid, set()).add(key)
        super().__setitem__(key, value)

    def __delitem__(self, key):
        super().__delitem__(key)
        for xuid in key:
            keys = self.of.get(xuid)
            if keys is not None:
                keys.discard(key)
                if not keys:
                    del self.of[xuid]

    def pop(self, key, default=None):
        if key not in self:
            return default
        value = self[key]
        del self[key]
        return value

    def naming(self, xuid):
        """The keys with xuid as source or target."""
        return list(self.of.get(xuid, ()))

    def sent_by(self, xuid):
        return sum(1 for source, _ in self.of.get(xuid, ()) if source == xuid)


def padded(text, size):
    """ASCII text in a fixed field, zeros after it."""
    return text[:size].ljust(size, b"\0")


def stat_number(stat):
    """A stat's (kind, value) as a number to compare: a type-3 value is a double's bits."""
    kind, value = stat
    return struct.unpack(">d", struct.pack(">q", value))[0] if kind == 3 else value


def printable(tag):
    """A requested gamertag with everything but printable ASCII dropped, at most 15 bytes."""
    return bytes(c for c in tag if 0x20 <= c <= 0x7e)[:15]


# The name check of netplay_names.c, kept in step with it by test_matchmaker.py: slurs and
# Nazi references; long ones anywhere in a name's letters, short ones as whole words.
NAMES_ANYWHERE = (
    "nigger", "nigga", "faggot", "tranny", "trannie", "shemale", "wetback", "raghead", "towelhead",
    "jigaboo", "porchmonkey", "zipperhead", "beaner", "spearchucker", "chingchong",
    "hitler", "neonazi", "natsoc", "siegheil", "fuhrer", "fuehrer", "thirdreich", "fourthreich",
    "swastika", "sonnenrad", "totenkopf", "zyklon", "holohoax", "gasthejews", "auschwitz",
    "goebbels", "himmler", "mengele", "eichmann", "mussolini", "fascis", "facist", "whitepower",
    "whitepride", "wpww", "rahowa", "bloodandhono", "fourteenwords", "kkk",
)
NAMES_WHOLE = (
    "nig", "niga", "fag", "fagot", "chink", "gook", "spic", "spick", "kike", "coon", "dyke", "paki",
    "homo", "troon", "nazi", "heil", "aryan",
)
NAMES_CODES = ("1488", "14words", "hh88", "88hh", "6mwe")
NAME_LEET = str.maketrans("0134579@$!", "oieastgasi")
MAX_NAME = 256
# A gamertag the server made from the default name, which the player's own name may replace.
FALLBACK_TAG = re.compile(rb"PLAYER[0-9A-F]{0,6}|PLA[0-9A-F]{12}")


def run_length(text, at):
    end = at
    while end < len(text) and text[end] == text[at]:
        end += 1
    return end - at


def runs_at(text, at, term):
    """Where term ends in text when its letter runs start at text[at], each at least as long, or None."""
    t = 0
    while t < len(term):
        if at >= len(text) or text[at] != term[t]:
            return None
        want, have = run_length(term, t), run_length(text, at)
        if have < want:
            return None
        t, at = t + want, at + have
    return at


def word_banned(word):
    for term in NAMES_WHOLE:
        end = runs_at(word, 0, term)
        if end is not None and (end == len(word) or (word[end] == "s" and end + run_length(word, end) == len(word))):
            return True
    return False


def name_banned(name):
    """recomp_netplay_name_banned for a name as bytes."""
    text = name.decode("latin-1")
    code = "".join(c.lower() for c in text if c.isascii() and c.isalnum())
    if len(text) > MAX_NAME or any(term in code for term in NAMES_CODES):
        return True
    for leet in (False, True):
        mapped = text.translate(NAME_LEET) if leet else text
        letters = "".join(c.lower() if c.isascii() and c.isalpha() else " " for c in mapped)
        if any(word_banned(word) for word in letters.split()):
            return True
        joined = letters.replace(" ", "")
        if word_banned(joined):  # letters spaced apart still spell a word
            return True
        at = 0
        while at < len(joined):
            if any(runs_at(joined, at, term) is not None for term in NAMES_ANYWHERE):
                return True
            at += run_length(joined, at)
    return False


def ina_bytes(ina):
    return struct.unpack(">I", ina)[0]


def xuid_of(proof):
    """The XUID a LOGON's 32-byte proof must belong to."""
    return (XUID_TYPE << 48) | int.from_bytes(hashlib.sha256(proof).digest()[:6], "big")


def logon_answer(key, nonce, body):
    """What an account's LOGON must carry: an HMAC, keyed by the account, of the server's nonce and every other field."""
    message = nonce + bytes((LOGON,)) + body[:LOGON_ANSWER.start] + body[LOGON_ANSWER.stop:]
    return hmac.new(key, message, hashlib.sha256).digest()


def online(account, now):
    return account.endpoint is not None and now - account.seen <= LIVE_SECONDS


class Account:
    """One logged-on XUID. key is the SHA-256 of the proof its first logon gave, which keys its later answers."""

    def __init__(self, xuid, key, gamertag, title):
        self.xuid, self.key, self.gamertag, self.title = xuid, key, gamertag, title
        self.ina = bytes(4)
        self.endpoint, self.nonce = None, bytes(TOKEN)
        self.seen = 0.0
        self.digest = bytes(DIGEST)
        self.state, self.kid = 0, bytes(8)


class LiveSession:
    def __init__(self, kid, key, host, digest, slots, attributes, now):
        self.kid, self.key, self.host, self.digest = kid, key, host, digest
        self.slots, self.attributes = list(slots), bytes(attributes)
        self.born = self.updated = now

    @property
    def open(self):
        # The counts are XOnlineMatchSessionCreate's: public filled, public
        # open, private filled, private open. Only an open slot is joinable.
        return self.slots[1] > 0 or self.slots[3] > 0


class Invite:
    def __init__(self, kid, answer, now):
        self.kid, self.answer, self.seen = kid, answer, now


class LivePair:
    """One CONNECT pairing, by XUID: its token, when it was last used and each side's packets this second."""

    def __init__(self, now, token, opener=None):
        self.token = token  # the same 8 random bytes to both players, which their punches carry
        self.opener = opener  # the address whose CONNECT made the pair, which PAIRS_PER_ADDRESS counts
        self.used, self.second, self.counts = now, int(now), {}
        self.connected = set()  # the XUIDs that have CONNECTed; both means they met
        self.met = None  # when the second of them connected, which allows writing each other's stats

    def tick(self, xuid, now):
        """Counts one packet from xuid in this second and returns the count."""
        if int(now) != self.second:
            self.second, self.counts = int(now), {}
        self.counts[xuid] = self.counts.get(xuid, 0) + 1
        return self.counts[xuid]


class Matchmaker:
    def __init__(self, relay_only=False, store=None, random_bytes=os.urandom, secret=None,
                 clock=time.monotonic, wall=time.time):
        self.relay_counts = {}  # RELAYs per address since the last sweep
        self.account_days, self.day = {}, None  # accounts each address registered today
        self.answer_bytes = {}  # Live answer bytes per address since the last sweep
        self.relay_only = relay_only  # players never learn each other's address
        self.secret = secret if secret is not None else os.urandom(32)  # cookie key; a test fixes it
        self.clock, self.wall = clock, wall  # the loop clock the packets count in, and the one the store keeps
        self.swept = 0.0
        self.relayed = self.relayed_bytes = self.over_cap = 0  # since the last status line
        self.random_bytes = random_bytes
        # Live, by XUID, virtual address, session id, (from, to), (board, XUID) and unordered XUID pairs.
        self.accounts, self.by_ina, self.by_endpoint = {}, {}, {}
        self.sessions = {}
        self.friends, self.mutes = {}, {}
        self.requests, self.invites = Links(), Links()  # by (source, target) XUIDs
        self.stats, self.feedback = {}, {}
        self.stats_by_board = {}  # board -> the XUIDs with a row on it, so a ranking reads only its board
        self.boards_of = {}  # XUID -> the boards it has a row on, for MAX_BOARDS without a scan
        self.feedback_kinds = {}  # target XUID -> its feedback types, for MAX_FEEDBACK_KINDS
        self.live_pairs = {}  # CONNECT pairs, by frozenset of two XUIDs, and when last used
        self.met = {}  # CONNECT pairs and when both first connected, which allows writing each other's stats
        self.rankings = {}  # each board's sorted ranking and XUID -> rank, until the board is written
        self.live_counts = {}  # Live packets per address since the last sweep
        self.connect_counts = {}  # CONNECTs per address since the last sweep
        self.account_counts, self.minute = {}, None  # new accounts each address registered this minute
        # (endpoint, socket nonce, request) -> (XUID, nonce, expires, its LOGON's hash and answer once it has one)
        self.challenges = {}  # oldest first
        self.challenge_keys = {}  # address -> its challenges' keys, oldest first, so a flood drops only its own
        self.live_cache = {}  # (endpoint, nonce, request, kind) -> (answers, expires)
        self.cache_keys = {}  # address -> its cached keys, oldest first, so one address cannot fill the cache
        self.ina_next = 0
        self.connects = self.pushes = self.registered = 0
        self.store = store
        self.dirty = False
        self.stored = 0.0
        if store:
            self.load()

    def handle(self, data, sender, now):
        """Returns the (packet, endpoint) answers to one packet from sender."""
        if len(data) < 6 + TOKEN or data[:4] != MAGIC or data[4] != VERSION:
            return []
        if now - self.swept >= 1.0:
            self.sweep(now)
        self.maybe_save(now)
        kind, nonce, cookie, body = data[5], data[6:6 + TOKEN], data[6 + TOKEN:ASKED], data[ASKED:]
        if kind == LIVE_RELAY:
            return self.live_relay(nonce, cookie, body, data, sender, now)
        if kind < LOGON:
            return []  # a kind the removed room protocol used
        return self.live(kind, nonce, cookie, body, data, sender, now)

    def check_cookie(self, sender, cookie, nonce, now):
        """(answers saying this minute's cookie, None) when the cookie is good, else (None, the answer to send)."""
        epoch = int(now // COOKIE_SECONDS)
        fresh = self.cookie(sender, epoch)
        if hmac.compare_digest(cookie, fresh):
            return [], None
        refresh = [(header(COOKIE) + nonce + fresh, sender)]
        if not hmac.compare_digest(cookie, self.cookie(sender, epoch - 1)):
            return None, refresh
        return refresh, None

    def cookie(self, sender, epoch):
        """The first bytes of an HMAC of the sender's endpoint and a minute's epoch, which only the server can make."""
        return hmac.new(self.secret, endpoint(sender) + struct.pack(">q", epoch), hashlib.sha256).digest()[:TOKEN]

    def sweep(self, now):
        self.swept = now
        self.relay_counts.clear()
        self.live_counts.clear()
        self.answer_bytes.clear()
        self.connect_counts.clear()
        for key in [key for key, pending in self.challenges.items() if pending[2] <= now]:
            self.take_challenge(key)
        if int(now // 60) != self.minute:
            self.minute = int(now // 60)
            self.account_counts.clear()
        if int(now // 86400) != self.day:
            if self.day is not None:  # the first sweep keeps what came before it
                self.account_days.clear()
            self.day = int(now // 86400)
        for key in [key for key, pair in self.live_pairs.items() if now - pair.used > PAIR_SECONDS]:
            del self.live_pairs[key]
        for key in [key for key, when in self.met.items() if now - when > MET_SECONDS]:
            del self.met[key]
        # A session is only joinable while its host is online.
        for kid in [kid for kid, session in self.sessions.items() if not online(session.host, now)]:
            del self.sessions[kid]
        expired = [key for key, when in self.requests.items() if now - when > REQUEST_SECONDS]
        for key in expired:
            del self.requests[key]
        for key in [key for key, invite in self.invites.items() if now - invite.seen > INVITE_SECONDS]:
            del self.invites[key]
            expired.append(key)
        self.dirty = self.dirty or bool(expired)  # the store drops them too
        for key in [key for key, (_, expires) in self.live_cache.items() if expires <= now]:
            del self.live_cache[key]
            keys = self.cache_keys.get(key[0][0])
            if keys is not None:
                keys.pop(key, None)
                if not keys:
                    del self.cache_keys[key[0][0]]

    # Live. Answers start with the header and the receiver's
    # nonce; a request that repeats, as a client resends until it hears, gets its
    # first answer again instead of a second account, invite or feedback.

    def live(self, kind, nonce, cookie, body, data, sender, now):
        checks = {CHALLENGE: live_challenge_ok, LOGON: live_logon_ok, SESSION_PUT: live_put_ok, SESSION_DEL: live_del_ok,
                  SEARCH: live_search_ok, FIND: live_find_ok, CONNECT: live_connect_ok,
                  FRIENDS: live_query_ok, FRIEND_OP: live_friend_op_ok, MUTES: live_query_ok,
                  FEEDBACK: live_feedback_ok, STAT_WRITE: live_stat_write_ok, STAT_READ: live_stat_read_ok,
                  LEADERS: live_leaders_ok}
        if len(data) > MAX_PACKET or not checks.get(kind, lambda body: False)(body):
            return []
        refresh, refused = self.check_cookie(sender, cookie, nonce, now)
        if refused is not None:
            return refused
        if over(self.live_counts, sender[0], LIVE_PER_SECOND):
            self.over_cap += 1
            return refresh
        key = (sender, nonce, body[:4], kind)
        cached = self.live_cache.get(key)
        if cached is not None and cached[1] > now:
            return self.budgeted(cached[0], sender) + refresh
        answers = self.live_request(kind, body, nonce, sender, now)
        # Nothing to repeat for an empty answer: a retry may yet succeed, so it is asked again.
        # CONNECT is not cached either: it changes nothing twice, its answer must name the
        # endpoints as they are now, and its repeats then meet the CONNECT cap. Nor are
        # CHALLENGE and LOGON: the challenges repeat a nonce and its LOGON's answer themselves,
        # so a logon never depends on room in this cache.
        if not answers or kind in (CONNECT, CHALLENGE, LOGON):
            return self.budgeted(answers, sender) + refresh
        keys = self.cache_keys.get(sender[0])
        if key not in self.live_cache:
            if keys is not None and len(keys) >= CACHE_PER_ADDRESS:
                oldest = next(iter(keys))  # the address's oldest makes room, so its newest retries stay safe
                del keys[oldest]
                self.live_cache.pop(oldest, None)
            elif len(self.live_cache) >= MAX_CACHE:
                return self.budgeted(answers, sender) + refresh
        self.cache_keys.setdefault(sender[0], {})[key] = None
        self.live_cache[key] = (answers, now + CACHE_SECONDS)
        return self.budgeted(answers, sender) + refresh

    def budgeted(self, answers, sender):
        """The answers, or none once they would take the address past its answer bytes this second.

        A request is small and its answer may not be (100 rows of stats), so counting requests
        alone lets one address pull megabytes a second out of the server."""
        size = sum(len(packet) for packet, _ in answers)
        spent = self.answer_bytes.get(sender[0], 0)
        if spent + size > LIVE_BYTES_PER_SECOND:
            self.over_cap += 1
            return []
        self.answer_bytes[sender[0]] = spent + size
        return answers

    def live_relay(self, nonce, cookie, body, data, sender, now):
        """One Live pair's guest datagram, forwarded to the other player as RELAY."""
        if len(body) < LIVE_RELAY_DESTINATION + DATAGRAM_HEAD or len(data) > ASKED + MAX_RELAY:
            return []
        refresh, refused = self.check_cookie(sender, cookie, nonce, now)
        if refused is not None:
            return refused
        me = self.online_at(sender, now)
        other = self.by_ina.get(body[:LIVE_RELAY_DESTINATION])
        if me is None or other is None or other is me or not online(other, now):
            return refresh
        datagram = body[LIVE_RELAY_DESTINATION:]
        pair = self.live_pairs.get(frozenset((me.xuid, other.xuid)))
        if (pair is None or now - pair.used > PAIR_SECONDS or datagram[:4] != DATAGRAM_MAGIC or
                datagram[4] not in DATAGRAM_KINDS):
            return refresh
        # As another pair's relay: the pair's own cap is checked first, so its
        # drops do not use up the address's share.
        if pair.tick(me.xuid, now) > RELAY_PER_SECOND or over(self.relay_counts, sender[0], RELAY_PER_ADDRESS):
            self.over_cap += 1
            return refresh
        pair.used = now
        datagram = datagram[:5] + me.ina + datagram[9:]  # the sender, not whatever the client claimed
        self.relayed += 1
        self.relayed_bytes += 6 + TOKEN + len(datagram)
        return [(header(RELAY) + other.nonce + datagram, other.endpoint)] + refresh

    def live_request(self, kind, body, nonce, sender, now):
        if kind == CHALLENGE:
            return self.live_challenge(body, nonce, sender, now)
        if kind == LOGON:
            return self.live_logon(body, nonce, sender, now)
        if kind == SESSION_PUT:
            return self.live_session_put(body, nonce, sender, now)
        if kind == SESSION_DEL:
            return self.live_session_del(body, nonce, sender, now)
        if kind == SEARCH:
            return self.live_search(body, nonce, sender, now)
        if kind == FIND:
            return self.live_find(body, nonce, sender, now)
        if kind == CONNECT:
            return self.live_connect(body, nonce, sender, now)
        if kind == FRIENDS:
            return self.live_friends(body, nonce, sender, now)
        if kind == FRIEND_OP:
            return self.live_friend_op(body, nonce, sender, now)
        if kind == MUTES:
            return self.live_mutes(body, nonce, sender, now)
        if kind == FEEDBACK:
            return self.live_feedback(body, nonce, sender, now)
        if kind == STAT_WRITE:
            return self.live_stat_write(body, nonce, sender, now)
        if kind == STAT_READ:
            return self.live_stat_read(body, nonce, sender, now)
        return self.live_leaders(body, nonce, sender, now)

    def online_at(self, sender, now):
        account = self.by_endpoint.get(sender)
        return account if account is not None and online(account, now) else None

    def next_ina(self):
        """10.x.y.z, unique among the accounts, never 10.0.* or 10.255.*, and never a .0 host part."""
        for _ in range(0xfe * 0x10000):
            self.ina_next = self.ina_next % (0xfe * 0x10000) + 1
            ina = bytes((10, (self.ina_next >> 16) & 0xff, (self.ina_next >> 8) & 0xff, self.ina_next & 0xff))
            if (self.ina_next >> 16) & 0xff not in (0, 0xff) and self.ina_next & 0xff != 0 and ina not in self.by_ina:
                return ina
        return bytes(4)

    def taken_tag(self, wanted, xuid):
        """wanted, or wanted with a digit; past nine of those, with the XUID's own hex digits,
        which no other account has, so every player who sends the default name gets one."""
        taken = {other.gamertag.lower() for other in self.accounts.values() if other.xuid != xuid}
        candidates = [wanted] + [wanted[:14] + bytes((digit,)) for digit in b"123456789"]
        candidates += [wanted[:9] + b"%06X" % (xuid & 0xffffff), wanted[:3] + b"%012X" % (xuid & 0xffffffffffff)]
        # A suffix can spell a banned name ("148" + "8"), so each candidate is checked again.
        return next((candidate for candidate in candidates
                     if candidate.lower() not in taken and not name_banned(candidate)), None)

    def evict_account(self, now):
        """Makes room in a full table: the account offline longest, and everything that names it.

        One with no stats or friends goes first, so throwaway accounts make room for each other."""
        offline = [account for account in self.accounts.values() if not online(account, now)]
        if not offline:
            return False
        account = min(offline, key=lambda account: (account.xuid in self.boards_of or
                                                    bool(self.friends.get(account.xuid)), account.seen))
        xuid = account.xuid
        del self.accounts[xuid]
        self.by_ina.pop(account.ina, None)
        if self.by_endpoint.get(account.endpoint) is account:
            del self.by_endpoint[account.endpoint]
        self.friends.pop(xuid, None)
        for others in self.friends.values():
            others.discard(xuid)  # both directions, as a removal does
        for key in self.requests.naming(xuid):
            del self.requests[key]
        for key in self.invites.naming(xuid):
            del self.invites[key]
        self.mutes.pop(xuid, None)
        for mutes in self.mutes.values():
            mutes.discard(xuid)
        self.feedback_kinds.pop(xuid, None)
        for key in [key for key in self.feedback if key[0] == xuid]:
            del self.feedback[key]  # a feedback row has no sender, only this target
        for board in self.boards_of.pop(xuid, ()):
            self.stats.pop((board, xuid), None)
            rows = self.stats_by_board.get(board)
            if rows is not None:
                rows.discard(xuid)
                if not rows:
                    del self.stats_by_board[board]
            self.rankings.pop(board, None)
        for key in [key for key in self.live_pairs if xuid in key]:
            del self.live_pairs[key]
        self.met = {key: when for key, when in self.met.items() if xuid not in key}
        self.dirty = True
        return True

    def live_challenge(self, body, nonce, sender, now):
        """A nonce for one LOGON: of this socket, this request id and this XUID, for CHALLENGE_SECONDS."""
        req, xuid = CHALLENGE_BODY.unpack(body)
        key = (sender, nonce, body[:4])
        pending = self.challenges.get(key)
        if pending is None or pending[0] != xuid or pending[2] <= now:
            self.take_challenge(key)  # a new one is the newest, wherever the old one was
            mine = self.challenge_keys.get(sender[0], ())
            # An address past its share drops its own oldest, so only a table full of
            # other addresses' costs anyone else a challenge: the oldest of all, whose LOGON is refused.
            if len(mine) >= CHALLENGES_PER_ADDRESS:
                self.take_challenge(next(iter(mine)))
            elif len(self.challenges) >= MAX_CHALLENGES:
                self.take_challenge(next(iter(self.challenges)))
            pending = self.challenges[key] = (xuid, self.random_bytes(CHALLENGE_BYTES), now + CHALLENGE_SECONDS, None)
            self.challenge_keys.setdefault(sender[0], {})[key] = None
        # A repeat says the same nonce while it lasts, so a resend never outdates an answer on its way.
        return [(header(NONCE) + nonce + NONCE_BODY.pack(req, xuid in self.accounts, pending[1]), sender)]

    def take_challenge(self, key):
        """Removes one challenge, with its address's note of it, and returns it or None."""
        keys = self.challenge_keys.get(key[0][0])
        if keys is not None:
            keys.pop(key, None)
            if not keys:
                del self.challenge_keys[key[0][0]]
        return self.challenges.pop(key, None)

    def live_logon(self, body, nonce, sender, now):
        req, xuid, answer, digest, title, state, kid, length, tag = LOGON_BODY.unpack(body)

        key, used = (sender, nonce, body[:4]), None

        def logged(hresult, ina=bytes(4), given=bytes(0)):
            answers = [(header(LOGGED) + nonce +
                        LOGGED_BODY.pack(req, hresult, ina_bytes(ina), len(given), padded(given, 15)), sender)]
            if used is not None:
                # The one LOGON this nonce answers, as a hash: a registration's body holds the proof.
                self.challenges[key] = used + ((hashlib.sha256(body).digest(), answers),)
            return answers

        if xuid == 0 or length > 15:
            return logged(E_INVALIDARG)
        asked, issued, expires, first = self.challenges.get(key) or (0, b"", 0.0, None)
        if asked != xuid or expires <= now:
            return logged(E_ACCESSDENIED)  # no challenge, as from a client that sends its proof unasked
        if first is not None:
            # A challenge is answered once, whatever came of its LOGON. That LOGON said again, as
            # a client does until it hears, gets the same answer while the nonce lasts; any other
            # is refused, so a captured LOGON is worth nothing from this socket or any other.
            return first[1] if first[0] == hashlib.sha256(body).digest() else logged(E_ACCESSDENIED)
        used = (asked, issued, expires)
        wanted = printable(tag[:length])
        if not wanted or name_banned(wanted):
            wanted = b"PLAYER"
        account = self.accounts.get(xuid)
        if account is None:
            # A new XUID answers with its proof, once: the XUID must be the proof's,
            # and the proof's hash becomes the key of every later answer.
            if xuid != xuid_of(answer):
                return logged(E_ACCESSDENIED)
            # The minute's cap counts attempts, to slow a flood; the day's counts accounts made,
            # so a refused attempt does not lock an address shared by real players out for the day.
            if (over(self.account_counts, sender[0], ACCOUNTS_PER_ADDRESS) or
                    self.account_days.get(sender[0], 0) >= ACCOUNTS_PER_DAY):
                return logged(E_FAIL)
            if len(self.accounts) >= MAX_ACCOUNTS and not self.evict_account(now):
                return logged(E_FAIL)
            given = self.taken_tag(wanted, xuid)  # a taken gamertag gets a digit
            if given is None:
                return logged(E_FAIL)
            account = self.accounts[xuid] = Account(xuid, hashlib.sha256(answer).digest(), given, title)
            account.ina = self.next_ina()  # the game's XNADDR uses it for both ina and inaOnline
            self.by_ina[account.ina] = account
            self.account_days[sender[0]] = self.account_days.get(sender[0], 0) + 1
            self.registered += 1
            self.dirty = True
        elif not hmac.compare_digest(answer, logon_answer(account.key, issued, body)):
            return logged(E_ACCESSDENIED)  # not this account's key: the game shows a logon error
        elif (wanted != b"PLAYER" and FALLBACK_TAG.fullmatch(account.gamertag)) or name_banned(account.gamertag):
            # A player who first logged on without a usable name takes the one they have now.
            given = self.taken_tag(wanted, xuid)
            if given is not None and given != account.gamertag:
                account.gamertag, self.dirty = given, True
        if account.endpoint is not None and self.by_endpoint.get(account.endpoint) is account:
            del self.by_endpoint[account.endpoint]
        account.endpoint, account.nonce, account.seen = sender, nonce, now
        account.digest, account.title = digest, title
        account.state, account.kid = state, kid
        self.by_endpoint[sender] = account
        return logged(HRESULT_OK, account.ina, account.gamertag)

    def live_session_put(self, body, nonce, sender, now):
        req, kid, s0, s1, s2, s3, length = SESSION_PUT_BODY.unpack_from(body)
        attributes = body[SESSION_PUT_BODY.size:]
        me = self.online_at(sender, now)

        def ok(hresult, given=bytes(8), key=bytes(16)):
            return [(header(SESSION_OK) + nonce + SESSION_OK_BODY.pack(req, hresult, given, key), sender)]

        if me is None:
            return ok(E_ACCESSDENIED)
        if kid == bytes(8):
            if (len(self.sessions) >= MAX_SESSIONS or
                    sum(1 for session in self.sessions.values() if session.host is me) >= SESSIONS_PER_ACCOUNT):
                return ok(E_FAIL)
            while True:
                kid = self.random_bytes(8)
                if kid != bytes(8) and kid not in self.sessions:
                    break
            key = self.random_bytes(16)
            session = self.sessions[kid] = LiveSession(kid, key, me, me.digest, (s0, s1, s2, s3), attributes, now)
            return ok(HRESULT_OK, session.kid, session.key)
        session = self.sessions.get(kid)
        if session is None or session.host is not me:
            return ok(E_INVALIDARG)
        session.slots, session.attributes, session.updated = [s0, s1, s2, s3], bytes(attributes), now
        return ok(HRESULT_OK, session.kid, session.key)

    def live_session_del(self, body, nonce, sender, now):
        req, kid = SESSION_DEL_BODY.unpack(body)
        me = self.online_at(sender, now)
        session = self.sessions.get(kid)
        if me is None:
            return self.done(req, E_ACCESSDENIED, sender, nonce)
        if kid == bytes(8):
            # A runner that quits logs off: its sessions go, and friends see it offline at once.
            mine = {kid for kid, session in self.sessions.items() if session.host is me}
            for kid in mine:
                del self.sessions[kid]
            for key in [key for key in self.invites.naming(me.xuid) if self.invites[key].kid in mine]:
                del self.invites[key]
            del self.by_endpoint[me.endpoint]
            me.endpoint = None
            return self.done(req, HRESULT_OK, sender, nonce)
        if session is None or session.host is not me:
            return self.done(req, E_INVALIDARG, sender, nonce)
        del self.sessions[kid]
        for key in [key for key in self.invites.naming(me.xuid) if self.invites[key].kid == kid]:
            del self.invites[key]
        return self.done(req, HRESULT_OK, sender, nonce)

    def done(self, req, hresult, sender, nonce):
        return [(header(DONE) + nonce + DONE_BODY.pack(req, hresult), sender)]

    def result(self, req, hresult, sender, nonce, rows=()):
        """One RESULT per row; a single one saying total 0 when there is none."""
        rows = list(rows)[:MAX_RESULTS]
        if not rows:
            return [(header(RESULT) + nonce + RESULT_BODY.pack(req, hresult, 0, 0), sender)]
        answers = []
        for index, session in enumerate(rows):
            attributes = session.attributes[:MAX_ATTRIBUTES]
            body = RESULT_BODY.pack(req, hresult, index, len(rows))
            body += RESULT_ROW.pack(session.kid, session.key, session.host.xuid,
                                    padded(session.host.gamertag, 16), ina_bytes(session.host.ina), *session.slots,
                                    len(attributes)) + attributes
            answers.append((header(RESULT) + nonce + body, sender))
        return answers

    def live_search(self, body, nonce, sender, now):
        req, procedure, length = SEARCH_BODY.unpack_from(body)
        me = self.online_at(sender, now)
        if me is None:
            return self.result(req, E_ACCESSDENIED, sender, nonce)
        rows = [session for session in self.sessions.values()
                if session.digest == me.digest and session.open and session.host is not me]
        rows.sort(key=lambda session: session.born, reverse=True)  # newest first
        return self.result(req, HRESULT_OK, sender, nonce, rows)

    def live_find(self, body, nonce, sender, now):
        req, kid = FIND_BODY.unpack(body)
        me = self.online_at(sender, now)
        session = self.sessions.get(kid)
        if me is None:
            return self.result(req, E_ACCESSDENIED, sender, nonce)
        if session is None:
            return self.result(req, E_INVALIDARG, sender, nonce)
        return self.result(req, HRESULT_OK, sender, nonce, [session])

    def hosts_session(self, account):
        """True when the account hosts a Live session from SESSION_PUT that is still alive."""
        return any(session.host is account for session in self.sessions.values())

    def live_pairs_at(self, address):
        """The Live pairs address opened; pairs others opened with its players do not count."""
        return sum(1 for pair in self.live_pairs.values() if pair.opener == address)

    def live_connect(self, body, nonce, sender, now):
        req, ina = CONNECT_BODY.unpack(body)
        me = self.online_at(sender, now)
        other = self.by_ina.get(struct.pack(">I", ina))
        if (me is None or other is None or other is me or not online(other, now) or other.digest != me.digest):
            return []
        if over(self.connect_counts, sender[0], CONNECTS_PER_SECOND):
            return []
        key = frozenset((me.xuid, other.xuid))
        pair = self.live_pairs.get(key)
        hosting = self.hosts_session(other)
        # An address is the other player's to learn only when they host a Live
        # session, or when they connected here first: a joiner may connect back.
        if (pair is None or now - pair.used > PAIR_SECONDS) and not hosting:
            return []
        if key not in self.live_pairs and self.live_pairs_at(sender[0]) >= PAIRS_PER_ADDRESS:
            return []
        if pair is None or now - pair.used > PAIR_SECONDS:
            pair = self.live_pairs[key] = LivePair(now, self.random_bytes(8), sender[0])
        if hosting:
            pair.used = now  # a CONNECT to a player who still hosts keeps the pair; one-sided repeats do not
        pair.connected.add(me.xuid)
        if len(pair.connected) == 2 and pair.met is None:
            pair.met = now  # the first time both connected, which a repeat must not push later
        if pair.met is not None:
            self.met[key] = pair.met  # fixed per pair, so a repeat cannot push it later, but a rematch can
        self.connects += 1
        relay = 1 if self.relay_only else 0
        empty = ("0.0.0.0", 0)

        def intro(target, peer_account):
            # LOGON carries no LAN endpoint, so Live players behind one router
            # punch through their public addresses; the field stays zero.
            public = empty if relay else peer_account.endpoint
            return (header(LPEER) + target.nonce + LPEER_BODY.pack(ina_bytes(peer_account.ina), endpoint(public),
                                                                   bytes(6), pair.token, relay),
                    target.endpoint)

        answers = [intro(me, other)]
        if other.endpoint != sender:
            answers.append(intro(other, me))
        return answers

    def live_friends(self, body, nonce, sender, now):
        req = FRIENDS_BODY.unpack(body)[0]
        me = self.online_at(sender, now)
        if me is None:
            return self.friend_rows(req, sender, nonce, [])
        rows = [self.friend_row(me, xuid, now) for xuid in self.related(me.xuid)]
        return self.friend_rows(req, sender, nonce, rows)

    def related(self, xuid):
        """Accepted friends and pending requests, in one stable order."""
        return sorted(self.friends.get(xuid, set()) |
                      {other for key in self.requests.naming(xuid) for other in key if other != xuid})

    def friend_rows(self, req, sender, nonce, rows):
        rows = rows[:MAX_FRIENDS]
        if not rows:
            return [(header(FRIEND) + nonce + FRIEND_BODY.pack(req, 0, 0, 0, bytes(16), 0, 0, bytes(8)), sender)]
        answers = []
        for index, row in enumerate(rows):
            answers.append((header(FRIEND) + nonce + FRIEND_BODY.pack(req, index, len(rows), *row), sender))
        return answers

    def friend_row(self, me, xuid, now):
        other = self.accounts.get(xuid)
        flags = 0
        if other is not None:
            if online(other, now):
                flags |= FRIEND_ONLINE
                if other.state & PRESENCE_SESSION:
                    flags |= FRIEND_PLAYING
                    session = self.sessions.get(other.kid)
                    if session is not None and session.open:
                        flags |= FRIEND_JOINABLE
            if (me.xuid, xuid) in self.requests:
                flags |= FRIEND_SENT_REQUEST
            if (xuid, me.xuid) in self.requests:
                flags |= FRIEND_RECEIVED_REQUEST
            invite = self.invites.get((me.xuid, xuid))
            if invite is not None and invite.answer is None:
                flags |= FRIEND_SENT_INVITE
            elif invite is not None:
                flags |= FRIEND_INVITE_ACCEPTED if invite.answer == 1 else FRIEND_INVITE_REJECTED
            invite = self.invites.get((xuid, me.xuid))
            if invite is not None and invite.answer is None:
                flags |= FRIEND_RECEIVED_INVITE
        kid = other.kid if other is not None and other.state & PRESENCE_SESSION else bytes(8)
        return (xuid, padded(other.gamertag if other is not None else b"", 16), flags,
                other.title if other is not None else 0, kid)

    def live_friend_op(self, body, nonce, sender, now):
        req, op, xuid, answer, kid = FRIEND_OP_BODY.unpack(body)
        me = self.online_at(sender, now)
        if me is None:
            return self.done(req, E_ACCESSDENIED, sender, nonce)
        other = self.accounts.get(xuid)
        if xuid == 0 or xuid == me.xuid or other is None:
            return self.done(req, E_INVALIDARG, sender, nonce)
        pushes = []
        if op == 1:  # request
            if self.are_friends(me.xuid, xuid):
                return self.done(req, HRESULT_OK, sender, nonce)
            if (xuid, me.xuid) in self.requests:
                del self.requests[(xuid, me.xuid)]
                self.add_friend(me.xuid, xuid)
            else:
                if self.requests.sent_by(me.xuid) >= MAX_FRIENDS:
                    return self.done(req, E_FAIL, sender, nonce)
                self.requests[(me.xuid, xuid)] = now
            self.dirty = True
            pushes = self.push(xuid, 1, me.xuid, bytes(8), now)
        elif op == 2:  # answer a request
            if (xuid, me.xuid) not in self.requests or answer > 2:
                return self.done(req, E_INVALIDARG, sender, nonce)
            del self.requests[(xuid, me.xuid)]
            if answer == 1:
                self.add_friend(me.xuid, xuid)
            self.dirty = True
            pushes = self.push(xuid, 1, me.xuid, bytes(8), now)
        elif op == 3:  # remove
            self.drop_friend(me.xuid, xuid)
            self.requests.pop((me.xuid, xuid), None)
            self.requests.pop((xuid, me.xuid), None)
            self.invites.pop((me.xuid, xuid), None)
            self.invites.pop((xuid, me.xuid), None)
            self.dirty = True
            pushes = self.push(xuid, 1, me.xuid, bytes(8), now)
        elif op == 4:  # game invite
            session = self.sessions.get(kid)
            if session is None or session.host is not me:
                return self.done(req, E_INVALIDARG, sender, nonce)
            if self.invites.sent_by(me.xuid) > MAX_FRIENDS:
                return self.done(req, E_FAIL, sender, nonce)
            self.invites[(me.xuid, xuid)] = Invite(kid, None, now)
            self.dirty = True
            pushes = self.push(xuid, 2, me.xuid, kid, now)
        elif op == 5:  # revoke an invite
            invite = self.invites.get((me.xuid, xuid))
            if invite is None or invite.answer is not None:
                return self.done(req, E_INVALIDARG, sender, nonce)
            del self.invites[(me.xuid, xuid)]
            self.dirty = True
            pushes = self.push(xuid, 1, me.xuid, bytes(8), now)
        elif op == 6:  # answer an invite
            invite = self.invites.get((xuid, me.xuid))
            if invite is None or invite.answer is not None or answer > 2 or (kid != bytes(8) and kid != invite.kid):
                return self.done(req, E_INVALIDARG, sender, nonce)
            invite.answer, invite.seen = answer, now
            self.dirty = True
            pushes = self.push(xuid, 3, me.xuid, invite.kid, now)
        else:
            return self.done(req, E_INVALIDARG, sender, nonce)
        return self.done(req, HRESULT_OK, sender, nonce) + pushes

    def are_friends(self, first, second):
        return second in self.friends.get(first, set())

    def add_friend(self, first, second):
        self.friends.setdefault(first, set()).add(second)
        self.friends.setdefault(second, set()).add(first)

    def drop_friend(self, first, second):
        self.friends.get(first, set()).discard(second)
        self.friends.get(second, set()).discard(first)

    def push(self, xuid, kind, source, kid, now):
        """An unsolicited PUSH to a player who is online, from another account."""
        target = self.accounts.get(xuid)
        if target is None or not online(target, now):
            return []
        self.pushes += 1
        return [(header(PUSH) + target.nonce + PUSH_BODY.pack(kind, source, kid), target.endpoint)]

    def live_mutes(self, body, nonce, sender, now):
        req = MUTES_BODY.unpack(body)[0]
        me = self.online_at(sender, now)
        if me is None:
            return [(header(MUTELIST) + nonce + MUTELIST_BODY.pack(req, E_ACCESSDENIED, 0), sender)]
        mutes = sorted(self.mutes.get(me.xuid, set()))[:MAX_MUTELIST]
        answer = header(MUTELIST) + nonce + MUTELIST_BODY.pack(req, HRESULT_OK, len(mutes))
        return [(answer + b"".join(struct.pack(">Q", xuid) for xuid in mutes), sender)]

    def live_feedback(self, body, nonce, sender, now):
        req, xuid, kind = FEEDBACK_BODY.unpack(body)
        if self.online_at(sender, now) is None:
            return self.done(req, E_ACCESSDENIED, sender, nonce)
        if xuid == 0 or xuid not in self.accounts:
            return self.done(req, E_INVALIDARG, sender, nonce)
        kinds = self.feedback_kinds.get(xuid, ())
        if kind not in kinds and len(kinds) >= MAX_FEEDBACK_KINDS:
            return self.done(req, E_INVALIDARG, sender, nonce)  # a new type past the target's cap
        self.feedback_kinds.setdefault(xuid, set()).add(kind)
        # Counted per target and type and kept in the store: the count is what a
        # later reader of the store has, so it saturates instead of growing freely.
        count = self.feedback.get((xuid, kind), 0) + 1
        self.feedback[(xuid, kind)] = min(count, MAX_FEEDBACK_COUNT)
        self.dirty = True
        return self.done(req, HRESULT_OK, sender, nonce)

    def live_stat_write(self, body, nonce, sender, now):
        req, board, xuid, count = STAT_WRITE_BODY.unpack_from(body)
        me = self.online_at(sender, now)
        if me is None:
            return self.done(req, E_ACCESSDENIED, sender, nonce)
        target = self.accounts.get(xuid)
        met = self.met.get(frozenset((me.xuid, xuid)))
        if target is None or (xuid != me.xuid and (met is None or now - met > MET_SECONDS)):
            return self.done(req, E_ACCESSDENIED, sender, nonce)  # one's own or a paired player's
        stats = self.stats.get((board, xuid), {})
        incoming = {}
        for index in range(count):
            stat_id, kind, value = STAT_ITEM.unpack_from(body, STAT_WRITE_BODY.size + index * STAT_ITEM.size)
            if kind not in (1, 2, 3):
                return self.done(req, E_INVALIDARG, sender, nonce)
            incoming[stat_id] = (kind, value)
        if not incoming:
            # The game sends an empty record for a board it has nothing on. It
            # changes nothing and must not take one of the account's MAX_BOARDS:
            # a row is a record, and a player with none reads as no data.
            return self.done(req, HRESULT_OK, sender, nonce)
        if len(set(stats) | set(incoming)) > MAX_STATS:
            return self.done(req, E_FAIL, sender, nonce)
        if (board, xuid) not in self.stats and len(self.boards_of.get(xuid, ())) >= MAX_BOARDS:
            return self.done(req, E_INVALIDARG, sender, nonce)  # a new board past the account's cap
        self.stats.setdefault((board, xuid), {}).update(incoming)
        self.stats_by_board.setdefault(board, set()).add(xuid)
        self.boards_of.setdefault(xuid, set()).add(board)
        self.rankings.pop(board, None)
        self.dirty = True
        return self.done(req, HRESULT_OK, sender, nonce)

    def board_ranking(self, board):
        """(rank, XUID) from the highest score down, cached."""
        return self.board_tally(board)[0]

    def board_tally(self, board):
        """The board's ranking and XUID to rank, cached until that board is written.

        A board ranks by its score stat, STAT_RATING (-2), which the game writes on
        every ranking board. The lowest id was ranked before and that is a profile
        byte, not a score. A row whose write carried no score still appears, after
        every scored row, by its lowest id as before."""
        cached = self.rankings.pop(board, None)
        if cached is not None:
            self.rankings[board] = cached  # most recently used last
            return cached
        rows = []
        for xuid in self.stats_by_board.get(board, ()):
            stats = self.stats.get((board, xuid))
            if not stats or xuid not in self.accounts:
                continue
            if STAT_RATING in stats:
                rows.append((0, -stat_number(stats[STAT_RATING]), xuid))
            else:
                rows.append((1, -stat_number(stats[min(stats)]), xuid))
        rows.sort()  # scored rows first, highest score first, then the lower XUID
        ranking = [(rank + 1, row[2]) for rank, row in enumerate(rows)]
        cached = (ranking, {xuid: rank for rank, xuid in ranking})
        if len(self.rankings) >= MAX_RANKINGS:
            del self.rankings[next(iter(self.rankings))]  # the least recently used
        self.rankings[board] = cached
        return cached

    def board_total(self, board):
        return len(self.board_ranking(board))

    def stat_row(self, req, board, xuid, tally, index, total, hresult=HRESULT_OK, rank=None):
        """One row, one packet: 43 body bytes plus 11 per stat, 747 at most.

        STAT_READ and LEADERS already answer one row per packet (index and
        total track them), so a 61-stat group-7 row needs no split. A row with
        no stats is the no-data shape: the game reads a stat entry left at
        type 0 as nothing (ranking_row_to_profile_counters 0x29b590)."""
        ranking, ranks = tally
        if rank is None:
            rank = ranks.get(xuid, 0)
        stats = self.stats.get((board, xuid), {})
        account = self.accounts.get(xuid)
        items = sorted(stats.items())[:MAX_STATS]
        body = STATROW_BODY.pack(req, hresult, index, total, len(ranking), xuid,
                                 padded(account.gamertag if account is not None else b"", 16), rank, len(items))
        for stat_id, (kind, value) in items:
            body += STAT_ITEM.pack(stat_id, kind, value)
        return body

    def stat_rows(self, req, board, xuids, sender, nonce, hresult=HRESULT_OK):
        tally = self.board_tally(board)
        if not xuids:
            return [(header(STATROW) + nonce + self.stat_row(req, board, 0, tally, 0, 0, hresult), sender)]
        return [(header(STATROW) + nonce + self.stat_row(req, board, xuid, tally, index, len(xuids), hresult),
                 sender) for index, xuid in enumerate(xuids)]

    def live_stat_read(self, body, nonce, sender, now):
        req, board, count = STAT_READ_BODY.unpack_from(body)
        xuids = [struct.unpack_from(">Q", body, STAT_READ_BODY.size + index * 8)[0] for index in range(count)]
        if self.online_at(sender, now) is None:
            return self.stat_rows(req, board, [], sender, nonce, E_ACCESSDENIED)
        return self.stat_rows(req, board, xuids, sender, nonce)

    def live_leaders(self, body, nonce, sender, now):
        req, board, pivot, first, count = LEADERS_BODY.unpack(body)
        if self.online_at(sender, now) is None:
            return self.stat_rows(req, board, [], sender, nonce, E_ACCESSDENIED)
        ranking, ranks = self.board_tally(board)
        if pivot != 0 and pivot not in ranks and board in RECORD_BOARDS:
            # A player with no row on a record board still gets their own empty
            # row, at the end of it. The game asks for its row this way, and with
            # no row live_ranking_record_is_new_best (0x2b4ca0) answers 4 and its
            # record is never written; with one it compares a zero score and
            # writes. The row has no stats, which the game reads as no data. The
            # other boards' pages would list it as a player with 0 points.
            return [(header(STATROW) + nonce + self.stat_row(req, board, pivot, (ranking, ranks), 0, 1,
                                                            rank=len(ranking) + 1), sender)]
        start = ranks.get(pivot, 0) if pivot != 0 else 0
        if start == 0:
            start = max(first, 1)
        rows = [xuid for _, xuid in ranking[start - 1:start - 1 + min(count, MAX_LEADERS)]]  # rank r is at r - 1
        return self.stat_rows(req, board, rows, sender, nonce)

    # --store: accounts and what outlives the process, in one JSON file.

    def load(self):
        try:
            with open(self.store, "r", encoding="utf-8") as file:
                data = json.load(file)
        except FileNotFoundError:
            return
        except (OSError, ValueError) as error:
            print(f"matchmaker: cannot read {self.store}: {error}", file=sys.stderr, flush=True)
            return
        if not isinstance(data, dict) or data.get("version") != 1:
            print(f"matchmaker: {self.store} is not version 1", file=sys.stderr, flush=True)
            return
        loaded = self.clock()  # a store from before seen existed counts as seen now
        wall = self.wall()
        skipped = 0

        def entries(name):
            value = data.get(name, [])
            return value if isinstance(value, list) else []

        def heard(value):
            """The loop clock's time from a store's wall clock one; an old store held monotonic seconds."""
            try:
                when = float(value)
            except (TypeError, ValueError):  # no time at all: heard now, as an absent seen is
                return loaded  # one too large for float() raises, and its section skips the entry
            if when < WALL_EPOCH:
                return loaded
            return loaded - max(0.0, wall - when)

        def xuids(*values):
            """XUIDs as ints, refusing one an answer could not carry (struct.error, a BAD_ENTRY)."""
            values = tuple(int(value) for value in values)
            struct.pack(f">{len(values)}Q", *values)
            return values

        def pair(entry):
            if len(entry) != 2:
                raise ValueError("not a pair")
            return xuids(entry[0], entry[1])

        for entry in entries("accounts"):
            try:
                account = Account(xuids(entry["xuid"])[0], bytes.fromhex(entry["proof"])[:32],
                                   printable(entry["gamertag"].encode("utf-8")) or b"PLAYER", int(entry["title"]))
                struct.pack(">I", account.title)  # a FRIEND row carries it
                account.seen = heard(entry.get("seen", loaded))
            except BAD_ENTRY:
                skipped += 1
                continue
            if account.xuid == 0 or account.xuid in self.accounts or len(account.key) != 32:
                continue
            account.ina = self.next_ina()
            self.accounts[account.xuid] = account
            self.by_ina[account.ina] = account
        for entry in entries("friends"):
            try:
                first, second = pair(entry)
            except BAD_ENTRY:
                skipped += 1
                continue
            self.add_friend(first, second)
        for entry in entries("requests"):
            try:
                source, target = xuids(entry[0], entry[1])
                when = heard(entry[2]) if len(entry) > 2 else loaded
            except BAD_ENTRY:
                skipped += 1
                continue
            self.requests[(source, target)] = when
        for entry in entries("invites"):
            try:
                invite = Invite(bytes.fromhex(entry["kid"]), entry["answer"],
                                heard(entry["seen"]) if "seen" in entry else loaded)
                from_xuid, to_xuid = xuids(entry["from"], entry["to"])
            except BAD_ENTRY:
                skipped += 1
                continue
            self.invites[(from_xuid, to_xuid)] = invite
        for entry in entries("mutes"):
            try:
                owner, muted = pair(entry)
            except BAD_ENTRY:
                skipped += 1
                continue
            self.mutes.setdefault(owner, set()).add(muted)
        for entry in entries("feedback"):
            try:
                xuid = xuids(entry["xuid"])[0]
                counts = {}
                for kind, count in entry["types"]:
                    kind, count = int(kind), int(count)
                    if not 0 <= kind <= 0xffffffff or count < 1:
                        raise ValueError("not a feedback count")
                    counts[kind] = min(count, MAX_FEEDBACK_COUNT)
            except BAD_ENTRY:
                skipped += 1
                continue
            for kind, count in sorted(counts.items())[:MAX_FEEDBACK_KINDS]:
                self.feedback[(xuid, kind)] = count
                self.feedback_kinds.setdefault(xuid, set()).add(kind)
        for entry in entries("stats"):
            try:
                stats = {(int(stat_id), int(kind), int(value)) for stat_id, kind, value in entry["stats"]}
                board, xuid = int(entry["board"]), int(entry["xuid"])
                # A number the wire cannot carry would fail every read of the row: skip it here.
                for item in stats:
                    STAT_ITEM.pack(*item)
                STAT_WRITE_BODY.pack(0, board, xuid, 0)
            except BAD_ENTRY:
                skipped += 1
                continue
            self.stats[(board, xuid)] = {stat_id: (kind, value) for stat_id, kind, value in stats}
            self.stats_by_board.setdefault(board, set()).add(xuid)
            self.boards_of.setdefault(xuid, set()).add(board)
        if skipped:
            print(f"matchmaker: skipped {skipped} malformed entries in {self.store}", file=sys.stderr, flush=True)

    def document(self, now=None):
        now = self.clock() if now is None else now
        wall = self.wall()

        def heard(seen):
            """The wall clock's time for a time the loop clock gave."""
            return wall - max(0.0, now - seen)

        def feedback_rows():
            """Counts per target and type; MAX_FEEDBACK_KINDS bounds each target's list."""
            by_target = {}
            for (xuid, kind), count in self.feedback.items():
                by_target.setdefault(xuid, []).append([kind, count])
            return [{"xuid": xuid, "types": sorted(types)} for xuid, types in sorted(by_target.items())]

        return {
            "version": 1,
            # "proof" is the name earlier stores gave this hash of the proof, so they still load.
            "accounts": [{"xuid": account.xuid, "proof": account.key.hex(),
                          "gamertag": account.gamertag.decode("ascii", "replace"), "title": account.title,
                          "seen": heard(account.seen)}
                         for account in self.accounts.values()],
            "feedback": feedback_rows(),
            "friends": sorted([first, second] for first, others in self.friends.items() for second in others
                              if first < second),
            "requests": sorted([source, target, heard(when)] for (source, target), when in self.requests.items()),
            "invites": [{"kid": invite.kid.hex(), "from": source, "to": target, "answer": invite.answer,
                         "seen": heard(invite.seen)}
                        for (source, target), invite in sorted(self.invites.items())],
            "mutes": sorted([owner, muted] for owner, mutes in self.mutes.items() for muted in mutes),
            "stats": [{"board": board, "xuid": xuid,
                       "stats": [[stat_id, kind, value] for stat_id, (kind, value) in sorted(stats.items())]}
                      for (board, xuid), stats in sorted(self.stats.items())],
        }

    def maybe_save(self, now):
        if self.store is None or not self.dirty or now - self.stored < STORE_SECONDS:
            return
        self.save(now)

    def save(self, now=None):
        """Writes the store beside itself, syncs it and renames it, so a crash leaves the last good file."""
        if self.store is None or not self.dirty:
            return
        folder = os.path.dirname(self.store) or "."
        temporary = None
        try:
            # A fresh name, created exclusively, so a leftover file or a symlink at
            # a fixed .tmp is never written through.
            handle, temporary = tempfile.mkstemp(prefix=os.path.basename(self.store) + ".", suffix=".tmp",
                                                 dir=folder)
            with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as file:
                json.dump(self.document(now), file, sort_keys=True, separators=(",", ":"))
                file.write("\n")
                file.flush()
                os.fsync(file.fileno())  # the data must be on the disk before the name changes
            os.replace(temporary, self.store)
            temporary = None
            sync_folder(folder)
        except OSError as error:
            print(f"matchmaker: cannot write {self.store}: {error}", file=sys.stderr, flush=True)
            return
        finally:
            if temporary is not None:
                try:
                    os.unlink(temporary)  # a failed write leaves the last good file and no temp
                except OSError:
                    pass
        self.dirty = False
        if now is not None:
            self.stored = now

    def live_status(self, now):
        """One log line of Live counts, without a name, XUID or address; the relay counts restart after it."""
        self.sweep(now)
        line = (f"live: accounts {len(self.accounts)}, online {sum(1 for account in self.accounts.values() if online(account, now))}, "
                f"sessions {len(self.sessions)}, connects {self.connects}, pushes {self.pushes}, "
                f"friends {sum(len(others) for others in self.friends.values()) // 2}, "
                f"requests {len(self.requests)}, invites {len(self.invites)}, "
                f"mutes {sum(len(mutes) for mutes in self.mutes.values())}, "
                f"stats {len(self.stats)}, feedback {sum(self.feedback.values())}, "
                f"relayed {self.relayed} ({self.relayed_bytes // 1024} KiB), over the cap {self.over_cap}")
        self.relayed = self.relayed_bytes = self.over_cap = 0
        return line


# Request sizes, before any state is read. A malformed packet gets no answer.
def live_challenge_ok(body):
    return len(body) == CHALLENGE_BODY.size


def live_logon_ok(body):
    return len(body) == LOGON_BODY.size and body[LOGON_BODY.size - 16] <= 15


def live_put_ok(body):
    if len(body) < SESSION_PUT_BODY.size:
        return False
    length = struct.unpack_from(">H", body, SESSION_PUT_BODY.size - 2)[0]
    return length <= MAX_ATTRIBUTES and SESSION_PUT_BODY.size + length == len(body)


def live_del_ok(body):
    return len(body) == SESSION_DEL_BODY.size


def live_search_ok(body):
    if len(body) < SEARCH_BODY.size:
        return False
    length = struct.unpack_from(">H", body, SEARCH_BODY.size - 2)[0]
    return length <= MAX_FILTERS and SEARCH_BODY.size + length == len(body)


def live_find_ok(body):
    return len(body) == FIND_BODY.size


def live_connect_ok(body):
    return len(body) == CONNECT_BODY.size


def live_query_ok(body):
    return len(body) == 4


def live_friend_op_ok(body):
    return len(body) == FRIEND_OP_BODY.size


def live_feedback_ok(body):
    return len(body) == FEEDBACK_BODY.size


def live_stat_write_ok(body):
    # A full write is 17 body bytes (request, board, XUID, count) plus 11 per
    # stat: 721 at 64 stats, 743 with the header, nonce and cookie, inside the
    # 1,200-byte packet the handle() check already enforces.
    if len(body) < STAT_WRITE_BODY.size:
        return False
    count = body[STAT_WRITE_BODY.size - 1]
    return count <= MAX_STAT_WRITE and STAT_WRITE_BODY.size + count * STAT_ITEM.size == len(body)


def live_stat_read_ok(body):
    if len(body) < STAT_READ_BODY.size:
        return False
    count = body[STAT_READ_BODY.size - 1]
    return count <= MAX_READ_STATS and STAT_READ_BODY.size + count * 8 == len(body)


def live_leaders_ok(body):
    return len(body) == LEADERS_BODY.size and body[LEADERS_BODY.size - 1] <= MAX_LEADERS


class Server(asyncio.DatagramProtocol):
    def __init__(self, matchmaker):
        self.matchmaker = matchmaker
        self.loop = asyncio.get_running_loop()

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, sender):
        for packet, target in self.matchmaker.handle(data, sender[:2], self.loop.time()):
            self.transport.sendto(packet, target)


async def serve(address, port, relay_only, status_seconds, store=None):
    loop = asyncio.get_running_loop()
    matchmaker = Matchmaker(relay_only=relay_only, store=store)
    # IPv4 only: endpoint() packs 4-byte addresses, so an IPv6 --address fails here, not on every packet.
    await loop.create_datagram_endpoint(lambda: Server(matchmaker), local_addr=(address, port), family=socket.AF_INET)
    print(f"matchmaker: UDP {address}:{port}{' relay only' if relay_only else ''}"
          f"{' storing ' + store if store else ''}", flush=True)
    try:
        if status_seconds <= 0:
            await asyncio.Event().wait()
        while True:
            await asyncio.sleep(status_seconds)
            print(matchmaker.live_status(loop.time()), flush=True)
    finally:
        matchmaker.save(loop.time())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--address", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=47700)
    parser.add_argument("--relay-only", action="store_true",
                        help="relay every Live pair, so no player learns another's address")
    parser.add_argument("--status", type=float, default=60.0, help="seconds between status lines, 0 for none")
    parser.add_argument("--store", metavar="PATH", help="keep Live accounts, friends and stats in this JSON file")
    arguments = parser.parse_args()
    asyncio.run(serve(arguments.address, arguments.port, arguments.relay_only, arguments.status,
                      arguments.store))


if __name__ == "__main__":
    main()
