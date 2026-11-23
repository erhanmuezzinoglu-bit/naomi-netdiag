# Naomi NetDiag

Native Sega Naomi / NetDIMM networking experiments using libnaomi.

The current milestone is a small UDP echo test. The Naomi application opens a
connected IPv4 UDP socket through the NetDIMM network command interface, sends
five 32-byte test datagrams, and verifies that the host returns each datagram
unchanged.

## Current test topology

```text
Naomi application (or Flycast NetDIMM HLE)
        |
        | UDP, port 19000
        v
Linux host running udp_echo_server.py
```

## Configure the host address

Find the Linux host's LAN IPv4 address:

```sh
ip -4 -brief address show scope global
```

Edit `netdiag_config.h` and set `NETDIAG_SERVER_IP0` through
`NETDIAG_SERVER_IP3` to that address. The initial values are only an example.
The server listens on UDP port `19000` by default.

## Start the UDP echo server

On the Linux computer:

```sh
python3 udp_echo_server.py --bind 0.0.0.0 --port 19000
```

Allow inbound UDP port 19000 from the Naomi/Flycast test network in the host
firewall. Do not expose this test service to the public internet.

## Build and run with Flycast

With the libnaomi toolchain and the Flycast source/build already available,
run:

```sh
./run.sh
```

`run.sh` builds the Naomi ROM, creates the local NetDIMM HLE package, and starts
Flycast. The UDP echo server must already be running in another terminal.

The default paths can be overridden with `FLYCAST_SRC`, `FLYCAST_BIN`,
`NAOMI_ENV`, `PACKER`, and `OUT_DIR` environment variables; see `run.sh`.

## Modules

- `netdimm_net.c/.h`: NetDIMM PIO command and socket operations.
- `netdimm_dns.c/.h`: standalone IPv4 DNS A-record resolver over UDP.
- `main.c`: Naomi-side UDP echo test.
- `udp_echo_server.py`: Linux-side echo endpoint for the test.
- `netdimm_hle_probe.c`: small read-only NetDIMM HLE register probe.
- `pack_netdimm_hle.py`: helper that builds the local Flycast NetDIMM HLE test package.

## Scope and limitations

This is a transport-layer diagnostic, not a matchmaking service or a finished
netplay implementation. A successful Flycast HLE test does not prove operation
on a physical Naomi; physical NetDIMM hardware, its network configuration, and
an actual LAN test must be validated separately.

The first milestone intentionally uses a connected UDP socket, so it does not
require `bind`, `listen`, or `accept` support in the HLE backend. Later work can
add discovery, matchmaking, relay transport, and game-specific input
synchronization after basic packet delivery is verified.
