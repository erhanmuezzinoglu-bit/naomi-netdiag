#!/usr/bin/env python3
"""Experimental UDP rendezvous/relay server for Naomi NetDiag.

Protocol v1 is intentionally small and suitable for connected UDP sockets:
  HOST/JOIN  payload: 4-8 uppercase ASCII letters/digits (room code)
  WAITING    host/join is registered, peer has not joined yet
  MATCH      payload: one byte role (1=host, 2=join)
  DATA       payload: opaque application bytes, relayed unchanged
  PING/PONG  payload: echoed unchanged
  LEAVE      closes a matched session
  PEER_LEFT  notifies the remaining peer
  ERROR      payload: short ASCII error code

This prototype has no authentication, encryption, persistence, or abuse
protection. Keep it on a trusted LAN during development; do not expose it to
the public Internet as-is.
"""

from __future__ import annotations

import argparse
import secrets
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, Optional, Tuple

MAGIC = b"NDNP"
VERSION = 1
HEADER = struct.Struct("!4sBBHII")  # magic, version, type, payload_len, session_id, sequence
HEADER_SIZE = HEADER.size
MAX_DATAGRAM = 1200
MAX_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE

MSG_HOST = 1
MSG_JOIN = 2
MSG_WAITING = 3
MSG_MATCH = 4
MSG_DATA = 5
MSG_PING = 6
MSG_PONG = 7
MSG_LEAVE = 8
MSG_PEER_LEFT = 9
MSG_ERROR = 10

ROLE_HOST = 1
ROLE_JOIN = 2

MESSAGE_NAMES = {
    MSG_HOST: "HOST",
    MSG_JOIN: "JOIN",
    MSG_WAITING: "WAITING",
    MSG_MATCH: "MATCH",
    MSG_DATA: "DATA",
    MSG_PING: "PING",
    MSG_PONG: "PONG",
    MSG_LEAVE: "LEAVE",
    MSG_PEER_LEFT: "PEER_LEFT",
    MSG_ERROR: "ERROR",
}

Address = Tuple[str, int]


def encode_packet(msg_type: int, payload: bytes = b"", session_id: int = 0,
                  sequence: int = 0) -> bytes:
    if not 0 <= msg_type <= 255:
        raise ValueError("message type out of range")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = HEADER.pack(MAGIC, VERSION, msg_type, len(payload),
                         session_id & 0xFFFFFFFF, sequence & 0xFFFFFFFF)
    return header + payload


def decode_packet(data: bytes):
    if len(data) < HEADER_SIZE or len(data) > MAX_DATAGRAM:
        return None
    magic, version, msg_type, payload_len, session_id, sequence = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        return None
    if payload_len != len(data) - HEADER_SIZE:
        return None
    return msg_type, session_id, sequence, data[HEADER_SIZE:]


@dataclass
class PendingRoom:
    room: str
    role: int
    address: Address
    created_at: float
    last_seen: float


@dataclass
class Session:
    room: str
    session_id: int
    players: Dict[int, Address] = field(default_factory=dict)
    last_activity: float = field(default_factory=time.monotonic)
    forwarded_packets: int = 0
    forwarded_bytes: int = 0


class RelayServer:
    def __init__(self, bind: str, port: int,
                 pending_ttl: float = 90.0,
                 session_idle_ttl: float = 180.0,
                 logger=print):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((bind, port))
        self.sock.settimeout(0.25)
        self.address = self.sock.getsockname()
        self.pending_ttl = pending_ttl
        self.session_idle_ttl = session_idle_ttl
        self.log = logger
        self.pending: Dict[str, PendingRoom] = {}
        self.sessions: Dict[int, Session] = {}
        self.endpoint_sessions: Dict[Address, int] = {}
        self._stop = threading.Event()

    def close(self):
        self._stop.set()
        self.sock.close()

    def serve_forever(self):
        self.log(f"Naomi NetDiag relay listening on {self.address[0]}:{self.address[1]} UDP")
        self.log("Protocol NDNP v1; experimental trusted-network use only")
        while not self._stop.is_set():
            try:
                data, address = self.sock.recvfrom(MAX_DATAGRAM + 1)
            except socket.timeout:
                self._expire()
                continue
            except OSError:
                break

            address = (address[0], address[1])
            decoded = decode_packet(data)
            if decoded is None:
                self.log(f"ignored malformed datagram from {address[0]}:{address[1]} ({len(data)} bytes)")
                continue
            msg_type, session_id, sequence, payload = decoded
            if len(data) > MAX_DATAGRAM:
                continue
            self._handle(address, msg_type, session_id, sequence, payload)
            self._expire()

        self.log("Naomi NetDiag relay stopped")

    def _send(self, address: Address, msg_type: int, payload: bytes = b"",
              session_id: int = 0, sequence: int = 0):
        try:
            self.sock.sendto(encode_packet(msg_type, payload, session_id, sequence), address)
        except OSError:
            pass

    def _error(self, address: Address, code: str, sequence: int = 0):
        self._send(address, MSG_ERROR, code.encode("ascii", "replace")[:64], sequence=sequence)

    @staticmethod
    def _room_from_payload(payload: bytes) -> Optional[str]:
        if not 4 <= len(payload) <= 8:
            return None
        try:
            room = payload.decode("ascii").upper()
        except UnicodeDecodeError:
            return None
        if not room.isalnum():
            return None
        return room

    def _register(self, address: Address, role: int, sequence: int, payload: bytes):
        room = self._room_from_payload(payload)
        if room is None:
            self._error(address, "BAD_ROOM_CODE", sequence)
            return

        # An endpoint must not hold multiple active sessions.
        if address in self.endpoint_sessions:
            self._error(address, "ALREADY_IN_SESSION", sequence)
            return

        pending = self.pending.get(room)
        now = time.monotonic()
        role_name = "HOST" if role == ROLE_HOST else "JOIN"

        if pending is None:
            self.pending[room] = PendingRoom(room, role, address, now, now)
            self._send(address, MSG_WAITING, room.encode("ascii"), sequence=sequence)
            self.log(f"{role_name} waiting room={room} peer={address[0]}:{address[1]}")
            return

        if pending.address == address:
            if pending.role != role:
                self._error(address, "ROLE_CONFLICT", sequence)
                return
            pending.last_seen = now
            self._send(address, MSG_WAITING, room.encode("ascii"), sequence=sequence)
            return

        if pending.role == role:
            self._error(address, "ROOM_ROLE_TAKEN", sequence)
            return

        # Pair the two outbound-connected UDP clients. The relay never asks a
        # Naomi client to bind/listen/accept, which keeps this HLE-compatible.
        session_id = self._new_session_id()
        session = Session(room=room, session_id=session_id)
        session.players[pending.role] = pending.address
        session.players[role] = address
        session.last_activity = now
        self.sessions[session_id] = session
        self.endpoint_sessions[pending.address] = session_id
        self.endpoint_sessions[address] = session_id
        del self.pending[room]

        host = session.players[ROLE_HOST]
        join = session.players[ROLE_JOIN]
        self._send(host, MSG_MATCH, bytes([ROLE_HOST]), session_id, sequence)
        self._send(join, MSG_MATCH, bytes([ROLE_JOIN]), session_id, sequence)
        self.log(f"MATCH room={room} session={session_id:08X} host={host[0]}:{host[1]} join={join[0]}:{join[1]}")

    def _new_session_id(self) -> int:
        while True:
            value = secrets.randbits(32)
            if value != 0 and value not in self.sessions:
                return value

    def _handle(self, address: Address, msg_type: int, session_id: int,
                sequence: int, payload: bytes):
        if msg_type in (MSG_HOST, MSG_JOIN):
            self._register(address, ROLE_HOST if msg_type == MSG_HOST else ROLE_JOIN,
                           sequence, payload)
            return

        if msg_type == MSG_PING:
            session = self.sessions.get(session_id) if session_id else None
            if session_id and (session is None or address not in session.players.values()):
                self._error(address, "UNKNOWN_SESSION", sequence)
                return
            if session is not None:
                session.last_activity = time.monotonic()
            self._send(address, MSG_PONG, payload, session_id, sequence)
            return

        if msg_type == MSG_DATA:
            session = self.sessions.get(session_id)
            if session is None or address not in session.players.values():
                self._error(address, "UNKNOWN_SESSION", sequence)
                return
            peer = next((endpoint for endpoint in session.players.values() if endpoint != address), None)
            if peer is None:
                self._error(address, "PEER_NOT_READY", sequence)
                return
            session.last_activity = time.monotonic()
            session.forwarded_packets += 1
            session.forwarded_bytes += len(payload)
            self._send(peer, MSG_DATA, payload, session_id, sequence)
            return

        if msg_type == MSG_LEAVE:
            session = self.sessions.get(session_id)
            if session is None or address not in session.players.values():
                self._error(address, "UNKNOWN_SESSION", sequence)
                return
            peer = next((endpoint for endpoint in session.players.values() if endpoint != address), None)
            if peer is not None:
                self._send(peer, MSG_PEER_LEFT, b"", session_id, sequence)
            self._remove_session(session_id, "client left")
            return

        self._error(address, "UNKNOWN_MESSAGE", sequence)

    def _remove_session(self, session_id: int, reason: str):
        session = self.sessions.pop(session_id, None)
        if session is None:
            return
        for endpoint in session.players.values():
            self.endpoint_sessions.pop(endpoint, None)
        self.log(f"CLOSE room={session.room} session={session_id:08X} reason={reason} "
                 f"relayed={session.forwarded_packets} packets/{session.forwarded_bytes} bytes")

    def _expire(self):
        now = time.monotonic()
        for room, pending in list(self.pending.items()):
            if now - pending.last_seen > self.pending_ttl:
                self._error(pending.address, "WAIT_EXPIRED")
                del self.pending[room]
                self.log(f"EXPIRE waiting room={room}")
        for session_id, session in list(self.sessions.items()):
            if now - session.last_activity > self.session_idle_ttl:
                for endpoint in session.players.values():
                    self._send(endpoint, MSG_PEER_LEFT, b"IDLE_TIMEOUT", session_id)
                self._remove_session(session_id, "idle timeout")


def _recv_decoded(sock: socket.socket, expected_type: int, timeout: float = 2.0):
    sock.settimeout(timeout)
    data, _ = sock.recvfrom(MAX_DATAGRAM)
    packet = decode_packet(data)
    if packet is None:
        raise AssertionError("received malformed packet")
    msg_type, session_id, sequence, payload = packet
    if msg_type != expected_type:
        name = MESSAGE_NAMES.get(msg_type, str(msg_type))
        raise AssertionError(f"expected {MESSAGE_NAMES.get(expected_type)}, received {name}: {payload!r}")
    return session_id, sequence, payload


def run_self_test():
    server = RelayServer("127.0.0.1", 0, logger=lambda _message: None)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    join = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    host.bind(("127.0.0.1", 0))
    join.bind(("127.0.0.1", 0))
    try:
        room = b"TST123"
        host.sendto(encode_packet(MSG_HOST, room, sequence=1), server.address)
        host_session, host_seq, host_payload = _recv_decoded(host, MSG_WAITING)
        assert host_session == 0 and host_seq == 1 and host_payload == room

        join.sendto(encode_packet(MSG_JOIN, room, sequence=2), server.address)
        host_session, _, host_role = _recv_decoded(host, MSG_MATCH)
        join_session, _, join_role = _recv_decoded(join, MSG_MATCH)
        assert host_session != 0 and host_session == join_session
        assert host_role == bytes([ROLE_HOST])
        assert join_role == bytes([ROLE_JOIN])

        payload_a = b"Naomi client A -> client B"
        host.sendto(encode_packet(MSG_DATA, payload_a, host_session, 10), server.address)
        sid, seq, received = _recv_decoded(join, MSG_DATA)
        assert (sid, seq, received) == (host_session, 10, payload_a)

        payload_b = b"Naomi client B -> client A"
        join.sendto(encode_packet(MSG_DATA, payload_b, join_session, 11), server.address)
        sid, seq, received = _recv_decoded(host, MSG_DATA)
        assert (sid, seq, received) == (join_session, 11, payload_b)

        ping_payload = b"ping-test"
        host.sendto(encode_packet(MSG_PING, ping_payload, host_session, 12), server.address)
        sid, seq, received = _recv_decoded(host, MSG_PONG)
        assert (sid, seq, received) == (host_session, 12, ping_payload)

        join.sendto(encode_packet(MSG_LEAVE, b"", join_session, 13), server.address)
        _recv_decoded(host, MSG_PEER_LEFT)

        print("RELAY SELF-TEST PASS")
        print("  host/join room pairing: PASS")
        print("  host -> join relay:     PASS")
        print("  join -> host relay:     PASS")
        print("  session ping/pong:      PASS")
        print("  peer leave notice:      PASS")
        print("  payloads preserved:     PASS")
    finally:
        host.close()
        join.close()
        server.close()
        worker.join(timeout=1.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="IPv4 bind address")
    parser.add_argument("--port", type=int, default=19001, help="UDP port (default: 19001)")
    parser.add_argument("--self-test", action="store_true", help="run an in-process two-client relay test")
    parser.add_argument("--pending-ttl", type=float, default=90.0, help="waiting-room expiry in seconds")
    parser.add_argument("--session-idle-ttl", type=float, default=180.0, help="idle session expiry in seconds")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return 0
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if args.pending_ttl <= 0 or args.session_idle_ttl <= 0:
        parser.error("timeouts must be positive")

    server = RelayServer(args.bind, args.port, args.pending_ttl, args.session_idle_ttl)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
