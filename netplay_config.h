#ifndef NETPLAY_CONFIG_H
#define NETPLAY_CONFIG_H

/* LAN address of the Linux machine running netplay_relay_server.py. */
#define NETPLAY_RELAY_IP0 192u
#define NETPLAY_RELAY_IP1 168u
#define NETPLAY_RELAY_IP2 52u
#define NETPLAY_RELAY_IP3 131u
#define NETPLAY_RELAY_PORT 19001u

/* HOST and JOIN builds must use exactly the same room code. */
#define NETPLAY_ROOM_CODE "CTEST2"

#define NETPLAY_MATCH_TIMEOUT_MS       120000u
#define NETPLAY_RECEIVE_TIMEOUT_MS       1000u
#define NETPLAY_DATA_PACKET_COUNT           5u
#define NETPLAY_SOCKET_OPERATION_TIMEOUT_MS 5000u
#define NETPLAY_COMMAND_WAIT_FRAMES          900u

#endif /* NETPLAY_CONFIG_H */
