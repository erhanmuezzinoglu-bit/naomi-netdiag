#include "netplay_netdimm.h"

#include <stddef.h>
#include <string.h>

#define NETPLAY_AF_INET       2u
#define NETPLAY_SOCK_DGRAM    2u
#define NETPLAY_IPPROTO_UDP   17u

#define NETPLAY_NETDIMM_DEFAULT_WAIT_FRAMES 900u
#define NETPLAY_NETDIMM_DEFAULT_TIMEOUT_MS  5000u

/* POSIX-compatible errno values used by Flycast's NetDIMM HLE. */
#define NETPLAY_ERRNO_EAGAIN      11u
#define NETPLAY_ERRNO_ETIMEDOUT  110u

static int set_socket_timeouts(netplay_netdimm_t *adapter,
                               uint32_t recv_timeout_ms)
{
    uint32_t result = 0xFFFFFFFFu;
    int rc;

    if (recv_timeout_ms == 0u) {
        recv_timeout_ms = NETPLAY_NETDIMM_DEFAULT_TIMEOUT_MS;
    }

    rc = netdimm_socket_set_timeout(adapter->net,
                                    adapter->socket_index,
                                    adapter->operation_timeout_ms,
                                    adapter->operation_timeout_ms,
                                    recv_timeout_ms,
                                    &result);
    adapter->last_driver_error = rc;
    if (rc != NETDIMM_NET_OK) {
        return NETPLAY_NETDIMM_ERR_SOCKET;
    }
    if (result != 0u) {
        return NETPLAY_NETDIMM_ERR_SOCKET;
    }

    adapter->receive_timeout_ms = recv_timeout_ms;
    return NETPLAY_NETDIMM_OK;
}

static int netdimm_send_datagram(void *opaque,
                                const uint8_t *data,
                                size_t length)
{
    netplay_netdimm_t *adapter = (netplay_netdimm_t *)opaque;
    uint32_t sent = 0u;
    int rc;

    if (adapter == NULL || adapter->is_open == 0u || data == NULL ||
        length == 0u || length > NETDIMM_NET_MAX_IO) {
        return -1;
    }

    rc = netdimm_socket_send(adapter->net, adapter->socket_index,
                             data, (uint32_t)length,
                             adapter->command_wait_frames, &sent);
    adapter->last_driver_error = rc;
    if (rc != NETDIMM_NET_OK || sent != (uint32_t)length) {
        adapter->last_socket_error = 0u;
        if (rc == NETDIMM_NET_ERR_DEVICE &&
            netdimm_socket_get_errno(adapter->net, adapter->socket_index,
                                     &adapter->last_socket_error) != NETDIMM_NET_OK) {
            adapter->last_socket_error = 0xFFFFFFFFu;
        }
        return -1;
    }

    adapter->last_socket_error = 0u;
    return 0;
}

/*
 * Generic transport receive contract:
 *   1 = one datagram received (including a zero-length UDP datagram)
 *   0 = receive timeout / no datagram
 *  -1 = hard transport or NetDIMM command error
 */
static int netdimm_receive_datagram(void *opaque,
                                    uint8_t *data,
                                    size_t capacity,
                                    size_t *received,
                                    uint32_t timeout_ms)
{
    netplay_netdimm_t *adapter = (netplay_netdimm_t *)opaque;
    uint32_t got = 0u;
    uint32_t socket_error = 0u;
    int rc;

    if (adapter == NULL || adapter->is_open == 0u || data == NULL ||
        received == NULL || capacity == 0u || capacity > NETDIMM_NET_MAX_IO) {
        return -1;
    }

    *received = 0u;

    if (timeout_ms == 0u) {
        timeout_ms = NETPLAY_NETDIMM_DEFAULT_TIMEOUT_MS;
    }
    if (timeout_ms != adapter->receive_timeout_ms) {
        if (set_socket_timeouts(adapter, timeout_ms) != NETPLAY_NETDIMM_OK) {
            return -1;
        }
    }

    rc = netdimm_socket_recv(adapter->net, adapter->socket_index,
                             data, (uint32_t)capacity,
                             adapter->command_wait_frames, &got);
    adapter->last_driver_error = rc;

    if (rc == NETDIMM_NET_OK) {
        if (got > capacity) {
            return -1;
        }
        *received = (size_t)got;
        adapter->last_socket_error = 0u;
        return 1;
    }

    if (rc == NETDIMM_NET_ERR_DEVICE) {
        int errno_rc;

        errno_rc = netdimm_socket_get_errno(adapter->net,
                                            adapter->socket_index,
                                            &socket_error);
        if (errno_rc != NETDIMM_NET_OK) {
            adapter->last_socket_error = 0xFFFFFFFFu;
            return -1;
        }

        adapter->last_socket_error = socket_error;
        if (socket_error == NETPLAY_ERRNO_EAGAIN ||
            socket_error == NETPLAY_ERRNO_ETIMEDOUT) {
            return 0;
        }
    }

    return -1;
}

int netplay_netdimm_open(netplay_netdimm_t *adapter,
                         netdimm_net_t *net,
                         const uint8_t relay_ipv4[4],
                         uint16_t relay_port,
                         uint32_t operation_timeout_ms,
                         uint32_t command_wait_frames,
                         netplay_transport_t *transport)
{
    uint32_t socket_index = 0xFFFFFFFFu;
    uint32_t command_result = 0xFFFFFFFFu;
    int rc;

    if (adapter == NULL || net == NULL || relay_ipv4 == NULL ||
        relay_port == 0u || transport == NULL || net->dimm_size == 0u) {
        return NETPLAY_NETDIMM_ERR_ARGUMENT;
    }

    memset(adapter, 0, sizeof(*adapter));
    memset(transport, 0, sizeof(*transport));

    adapter->net = net;
    adapter->operation_timeout_ms = operation_timeout_ms != 0u
        ? operation_timeout_ms : NETPLAY_NETDIMM_DEFAULT_TIMEOUT_MS;
    adapter->command_wait_frames = command_wait_frames != 0u
        ? command_wait_frames : NETPLAY_NETDIMM_DEFAULT_WAIT_FRAMES;

    rc = netdimm_socket_open(net, NETPLAY_AF_INET, NETPLAY_SOCK_DGRAM,
                             NETPLAY_IPPROTO_UDP, &socket_index);
    adapter->last_driver_error = rc;
    if (rc != NETDIMM_NET_OK || socket_index == 0u ||
        socket_index == 0xFFFFFFFFu) {
        return NETPLAY_NETDIMM_ERR_SOCKET;
    }

    adapter->socket_index = socket_index;
    adapter->is_open = 1u;

    rc = set_socket_timeouts(adapter, adapter->operation_timeout_ms);
    if (rc != NETPLAY_NETDIMM_OK) {
        (void)netplay_netdimm_close(adapter);
        return rc;
    }

    rc = netdimm_socket_connect_ipv4(net, adapter->socket_index,
                                     relay_ipv4, relay_port,
                                     adapter->command_wait_frames,
                                     &command_result);
    adapter->last_driver_error = rc;
    if (rc != NETDIMM_NET_OK || command_result != 0u) {
        (void)netplay_netdimm_close(adapter);
        return NETPLAY_NETDIMM_ERR_SOCKET;
    }

    transport->context = adapter;
    transport->send_datagram = netdimm_send_datagram;
    transport->receive_datagram = netdimm_receive_datagram;
    return NETPLAY_NETDIMM_OK;
}

int netplay_netdimm_close(netplay_netdimm_t *adapter)
{
    uint32_t result = 0xFFFFFFFFu;
    int rc;

    if (adapter == NULL) {
        return NETPLAY_NETDIMM_ERR_ARGUMENT;
    }
    if (adapter->is_open == 0u) {
        return NETPLAY_NETDIMM_OK;
    }

    rc = netdimm_socket_close(adapter->net, adapter->socket_index, &result);
    adapter->last_driver_error = rc;
    adapter->is_open = 0u;
    return (rc == NETDIMM_NET_OK && result == 0u)
        ? NETPLAY_NETDIMM_OK : NETPLAY_NETDIMM_ERR_SOCKET;
}

int netplay_netdimm_last_driver_error(const netplay_netdimm_t *adapter)
{
    return adapter != NULL ? adapter->last_driver_error
                           : NETDIMM_NET_ERR_ARGUMENT;
}

uint32_t netplay_netdimm_last_socket_error(const netplay_netdimm_t *adapter)
{
    return adapter != NULL ? adapter->last_socket_error : 0xFFFFFFFFu;
}
