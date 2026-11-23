#!/usr/bin/env python3
"""Small LAN-only test helper for the Naomi NetDIMM UDP echo client."""

import argparse
import socket


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0", help="IPv4 bind address (default: all interfaces)")
    parser.add_argument("--port", type=int, default=19000, help="UDP port (default: 19000)")
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.bind, args.port))
        print(f"UDP echo server listening on {args.bind}:{args.port}", flush=True)
        print("Only N D G 1 test packets are echoed. Press Ctrl+C to stop.", flush=True)
        try:
            while True:
                data, peer = sock.recvfrom(2048)
                if not data.startswith(b"NDG1"):
                    print(f"ignored {len(data)} bytes from {peer[0]}:{peer[1]} (not an NDG1 packet)", flush=True)
                    continue
                sock.sendto(data, peer)
                print(f"echoed {len(data)} bytes to {peer[0]}:{peer[1]}: {data[:8].hex(' ')}", flush=True)
        except KeyboardInterrupt:
            print("\nUDP echo server stopped.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
