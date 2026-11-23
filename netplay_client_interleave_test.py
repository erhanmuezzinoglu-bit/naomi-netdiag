#!/usr/bin/env python3
"""Force NDNP peer-DONE to arrive before this client's delayed PONG."""

import argparse
import importlib
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--client", default="./netplay_client_posix_test",
        help="path to the compiled POSIX C test client",
    )
    parser.add_argument(
        "--pong-delay", type=float, default=0.75,
        help="artificial delay for the JOIN PONG in seconds",
    )
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    sys.path.insert(0, str(script_dir))
    client = Path(args.client).resolve()
    if not client.is_file():
        parser.error("C client executable not found: {}".format(client))

    relay = importlib.import_module("netplay_relay_server")
    original_handle = relay.RelayServer._handle

    def delayed_join_pong(self, address, msg_type, session_id, sequence, payload):
        if msg_type == relay.MSG_PING and session_id:
            session = self.sessions.get(session_id)
            if session is not None and address == session.players.get(relay.ROLE_JOIN):
                self.log("TEST: delaying JOIN PONG to force peer DONE before PONG")
                session.last_activity = time.monotonic()
                timer = threading.Timer(
                    args.pong_delay,
                    self._send,
                    args=(address, relay.MSG_PONG, payload, session_id, sequence),
                )
                timer.daemon = True
                timer.start()
                return
        return original_handle(self, address, msg_type, session_id, sequence, payload)

    # Ask the OS for an unused UDP port, then bind the relay to it.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    relay.RelayServer._handle = delayed_join_pong
    server = relay.RelayServer("127.0.0.1", port, logger=print)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    host = join = None

    try:
        thread.start()
        time.sleep(0.15)
        room = "RACE01"
        host = subprocess.Popen(
            [str(client), "host", room, "127.0.0.1", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        time.sleep(0.15)
        join = subprocess.Popen(
            [str(client), "join", room, "127.0.0.1", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        host_output, _ = host.communicate(timeout=15)
        join_output, _ = join.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        for process in (host, join):
            if process is not None and process.poll() is None:
                process.kill()
        raise
    finally:
        server.close()
        thread.join(timeout=2)
        relay.RelayServer._handle = original_handle

    print("--- HOST ---")
    print(host_output, end="" if host_output.endswith("\n") else "\n")
    print("--- JOIN ---")
    print(join_output, end="" if join_output.endswith("\n") else "\n")

    passed = (
        host.returncode == 0
        and join.returncode == 0
        and "SYNC EARLY: Received peer DONE while waiting for PONG" in join_output
        and "NDNP C CLIENT TEST PASS" in host_output
        and "NDNP C CLIENT TEST PASS" in join_output
    )
    print("INTERLEAVED DONE/PONG REGRESSION:", "PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
