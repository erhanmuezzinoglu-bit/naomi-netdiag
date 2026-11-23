Naomi NetDiag - Stage 4: separate NetDIMM HOST/JOIN ROM targets
==============================================================

This stage does not replace main.c. The existing DNS + UDP Echo app remains
available as the default APP=echo build. It adds two separate ROMs:

  make APP=host  -> netplay_host.bin
  make APP=join  -> netplay_join.bin

Both use the same NETPLAY_ROOM_CODE and relay endpoint from netplay_config.h.
The role is compiled into a separate wrapper translation unit so two different
Flycast processes can run HOST and JOIN concurrently. The Makefile gives each
role a separate Naomi binary/ELF output path while sharing the regular object
folder; host and join entrypoint objects have different names.

Before running:
  1. Ensure netplay_relay_server.py is present in this directory.
  2. Start it in terminal 1:
       python3 netplay_relay_server.py --bind 0.0.0.0 --port 19001
  3. Check NETPLAY_RELAY_IP0..3 in netplay_config.h. It must be the Linux
     machine address reachable from Flycast (currently 192.168.52.131).
  4. Ensure HOST and JOIN use the same NETPLAY_ROOM_CODE.

Run HOST in terminal 2:
  ./run_netplay.sh host

Run JOIN in terminal 3:
  ./run_netplay.sh join

HOST should start first and display WAITING. JOIN then joins the same room.
Both applications run the current NDNP v1 smoke test: MATCH, DATA relay,
PING/PONG, DONE barrier, then HOST LEAVE / JOIN PEER_LEFT.

Keep the two old tests:
  ./run.sh             -> existing DNS + UDP Echo Naomi test
  python3 netplay_client_interleave_test.py -> C client race regression
  ./build_netplay_netdimm_test.sh            -> fake-NetDIMM adapter unit test

Important limitations:
  - This is a transport/matchmaking smoke test, not a game netplay implementation.
  - The Python relay is experimental and unauthenticated. Keep it on trusted LAN.
  - Current timeout errno mapping (EAGAIN 11, ETIMEDOUT 110) is Flycast HLE
    specific; verify against physical NetDIMM firmware when hardware is usable.
  - The fixed relay IPv4 is intentional for the first local HLE test; DNS is not
    yet used to discover the relay server.
