#ifndef NETPLAY_NETDIMM_H
#define NETPLAY_NETDIMM_H

#include <stdint.h>

#include "netdimm_net.h"
#include "netplay_client.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NETPLAY_NETDIMM_OK = 0,
    NETPLAY_NETDIMM_ERR_ARGUMENT = -200,
    NETPLAY_NETDIMM_ERR_SOCKET = -201,
    NETPLAY_NETDIMM_ERR_CONFIG = -202
};

/*
 * Adapter from the generic NDNP datagram transport to the NetDIMM socket API.
 *
 * The UDP socket is connected to one relay IPv4 endpoint. The NetDIMM driver
 * performs each send/receive through the DIMM command interface.
 *
 * For Flycast NetDIMM HLE, a receive timeout is surfaced as a socket error
 * (normally ETIMEDOUT=110; some stacks may report EAGAIN=11). Those two values
 * are translated to the generic transport's timeout return (0). Other socket
 * errors remain hard failures. Verify errno values on physical NetDIMM firmware
 * when hardware is available.
 */
typedef struct netplay_netdimm {
    netdimm_net_t *net;
    uint32_t socket_index;
    uint32_t command_wait_frames;
    uint32_t operation_timeout_ms;
    uint32_t receive_timeout_ms;
    uint32_t last_socket_error;
    int last_driver_error;
    uint8_t is_open;
} netplay_netdimm_t;

/*
 * Open and connect a NetDIMM UDP socket to relay_ipv4:relay_port.
 * relay_ipv4 contains four network-order bytes; relay_port is host byte order.
 * On success, fills transport with callbacks backed by this adapter.
 */
int netplay_netdimm_open(netplay_netdimm_t *adapter,
                         netdimm_net_t *net,
                         const uint8_t relay_ipv4[4],
                         uint16_t relay_port,
                         uint32_t operation_timeout_ms,
                         uint32_t command_wait_frames,
                         netplay_transport_t *transport);

/* Close the underlying NetDIMM socket. Safe to call after a failed open. */
int netplay_netdimm_close(netplay_netdimm_t *adapter);

/* Diagnostic accessors for the last NetDIMM/socket error observed. */
int netplay_netdimm_last_driver_error(const netplay_netdimm_t *adapter);
uint32_t netplay_netdimm_last_socket_error(const netplay_netdimm_t *adapter);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_NETDIMM_H */
