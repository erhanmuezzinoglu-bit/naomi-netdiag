#!/usr/bin/env python3
"""Local HTTP bridge: Naomi NetDIMM HTTP -> Fightcade Public API HTTPS.

Run this on the same PC that runs Flycast. It listens only on 127.0.0.1.
The public Fightcade endpoint is queried without account credentials.
"""

import json
import os
import sys
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

API_URL = "https://www.fightcade.com/api/"
LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = 8765
QUERY_LIMIT = 100
DISPLAY_LIMIT = 6

# Fightcade may present a Cloudflare challenge to non-browser HTTP clients.
# A browser-like UA is used by default; an optional cf_clearance cookie can be
# supplied through the environment without writing it into this source file.
USER_AGENT = os.environ.get(
    "FIGHTCADE_USER_AGENT",
    "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0",
).strip()
CF_CLEARANCE = os.environ.get("FIGHTCADE_CF_CLEARANCE", "").strip()


def safe_ascii(value, limit=24):
    value = str(value or "")
    value = value.encode("ascii", "replace").decode("ascii")
    value = "".join(ch if 32 <= ord(ch) <= 126 else " " for ch in value)
    value = value.replace("|", "/").replace("\r", " ").replace("\n", " ")
    return " ".join(value.split())[:limit] or "?"


def is_cvs2_replay(item):
    gameid = str(item.get("gameid", "")).lower()
    channel = str(item.get("channelname", "")).lower()
    return (
        "cvs2" in gameid
        or "capcom vs snk 2" in channel
        or "capcom vs. snk 2" in channel
        or "capcom vs snk ii" in channel
    )


def request_replays(gameid=None):
    payload = {"req": "searchquarks", "limit": QUERY_LIMIT, "offset": 0}
    if gameid:
        payload["gameid"] = gameid

    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    request = Request(
        API_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": USER_AGENT,
        },
        method="POST",
    )
    if CF_CLEARANCE:
        request.add_header("Cookie", "cf_clearance=" + CF_CLEARANCE)
    with urlopen(request, timeout=15) as response:
        data = json.loads(response.read().decode("utf-8"))

    if data.get("res") != "OK":
        raise RuntimeError("Fightcade API response did not contain res=OK")

    result_block = data.get("results", {})
    if not isinstance(result_block, dict):
        raise RuntimeError("Unexpected Fightcade API results object")
    entries = result_block.get("results", [])
    if not isinstance(entries, list):
        raise RuntimeError("Fightcade API results.results is not an array")
    return entries


def make_result_body():
    entries = request_replays()
    matches = [entry for entry in entries if isinstance(entry, dict) and is_cvs2_replay(entry)]

    # If CvS2 is not among the newest global records, also test the expected
    # Fightcade ROM id. This is a filtered replay query, not a live-user query.
    if not matches:
        try:
            filtered = request_replays("cvs2")
        except Exception as exc:
            # The unfiltered query already proved the API endpoint works.
            # Keep that result even if the candidate game-id filter is rejected.
            print("[bridge] gameid=cvs2 fallback query failed: {}".format(exc), file=sys.stderr, flush=True)
            filtered = []
        if filtered:
            entries = filtered
            matches = [entry for entry in filtered if isinstance(entry, dict) and is_cvs2_replay(entry)]
            if not matches:
                # Some API responses may omit a matching channel label even
                # when the requested gameid filter was accepted.
                matches = [entry for entry in filtered if isinstance(entry, dict)]

    lines = ["STATUS=OK", "SCANNED={}".format(len(entries)), "COUNT={}".format(len(matches))]
    for item in matches[:DISPLAY_LIMIT]:
        timestamp = item.get("date", 0)
        try:
            date_text = datetime.fromtimestamp(float(timestamp) / 1000.0).strftime("%m-%d %H:%M")
        except (TypeError, ValueError, OSError, OverflowError):
            date_text = "date?"

        try:
            duration = "{}s".format(max(0, int(item.get("duration", 0))))
        except (TypeError, ValueError, OverflowError):
            duration = "?s"

        players = item.get("players", [])
        if not isinstance(players, list):
            players = []
        player1 = safe_ascii(players[0].get("name", "?"), 18) if len(players) > 0 and isinstance(players[0], dict) else "?"
        player2 = safe_ascii(players[1].get("name", "?"), 18) if len(players) > 1 and isinstance(players[1], dict) else "?"
        lines.append("M|{}|{}|{}|{}".format(date_text, duration, player1, player2))

    return ("\n".join(lines) + "\n").encode("ascii", "replace")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def send_text(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=us-ascii")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path != "/cvs2":
            self.send_text(404, b"NOT_FOUND\n")
            return

        try:
            body = make_result_body()
            self.send_text(200, body)
            print("[bridge] Fightcade API OK; response {} bytes".format(len(body)), flush=True)
        except HTTPError as exc:
            # Surface safe diagnostics (status, server, Cloudflare ray and a
            # short body excerpt); never print request headers or cookie values.
            raw = exc.read(1200).decode("utf-8", "replace")
            headers = exc.headers or {}
            server = headers.get("Server", "unknown")
            cf_ray = headers.get("CF-Ray", "none")
            safe_body = " ".join(raw.replace("\r", " ").replace("\n", " ").split())[:500]
            message = (
                "FIGHTCADE_HTTP_ERROR={}\nSERVER={}\nCF_RAY={}\nBODY={}\n"
                .format(exc.code, server, cf_ray, safe_body or "(empty)")
            )
            self.send_text(502, message.encode("ascii", "replace"))
            print("[bridge] Fightcade HTTP error {}; server={}; cf-ray={}; body={}".format(
                exc.code, server, cf_ray, safe_body or "(empty)"
            ), file=sys.stderr, flush=True)
        except URLError as exc:
            self.send_text(502, b"FIGHTCADE_NETWORK_ERROR\n")
            print("[bridge] Fightcade network error: {}".format(exc), file=sys.stderr, flush=True)
        except Exception as exc:
            self.send_text(502, b"BRIDGE_ERROR\n")
            print("[bridge] Error: {}".format(exc), file=sys.stderr, flush=True)

    def log_message(self, fmt, *args):
        print("[bridge] " + fmt % args, flush=True)


def main():
    server = HTTPServer((LISTEN_HOST, LISTEN_PORT), Handler)
    print("Fightcade API bridge listening on http://{}:{}/cvs2".format(
        LISTEN_HOST, LISTEN_PORT
    ), flush=True)
    print("User-Agent: {}".format(USER_AGENT), flush=True)
    print("Cloudflare clearance cookie: {}".format("configured" if CF_CLEARANCE else "not configured"), flush=True)
    print("Start Flycast only after this bridge is running. Ctrl+C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        print("Fightcade API bridge stopped.", flush=True)


if __name__ == "__main__":
    main()
