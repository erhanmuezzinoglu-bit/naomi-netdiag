#ifndef NETDIAG_CONFIG_H
#define NETDIAG_CONFIG_H

/*
 * Bilgisayarda çalışacak UDP Echo sunucusunun LAN IPv4 adresi.
 * Bu adresi kendi bilgisayarının güncel LAN IP adresiyle değiştir.
 */
#define NETDIAG_SERVER_IP0 192u
#define NETDIAG_SERVER_IP1 168u
#define NETDIAG_SERVER_IP2 52u
#define NETDIAG_SERVER_IP3 131u

#define NETDIAG_UDP_PORT       19000u
#define NETDIAG_ECHO_ROUNDS    5u
#define NETDIAG_ECHO_WAIT_FRAMES 900u
#define NETDIAG_SOCKET_TIMEOUT_MS 8000u

#endif /* NETDIAG_CONFIG_H */
