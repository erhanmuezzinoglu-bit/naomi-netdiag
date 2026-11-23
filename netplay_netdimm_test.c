#include "netplay_netdimm.h"

#include <stdio.h>
#include <string.h>

static unsigned int g_checks;
static unsigned int g_failures;
static unsigned int g_socket_open_calls;
static unsigned int g_timeout_calls;
static unsigned int g_connect_calls;
static unsigned int g_send_calls;
static unsigned int g_recv_calls;
static unsigned int g_errno_calls;
static unsigned int g_close_calls;
static uint32_t g_last_recv_timeout;
static uint32_t g_socket_errno;
static int g_recv_rc;
static int g_send_rc;
static int g_settimeout_rc;
static int g_connect_rc;
static uint32_t g_send_result;
static uint32_t g_connect_result;
static uint32_t g_timeout_result;
static uint32_t g_recv_result;
static uint8_t g_recv_payload[64];
static size_t g_recv_payload_len;
static uint8_t g_sent_payload[64];
static size_t g_sent_payload_len;

static void check(int condition, const char *name)
{
    g_checks++;
    if (condition) {
        printf("PASS: %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        g_failures++;
    }
}

int netdimm_socket_open(netdimm_net_t *net, uint32_t domain,
                        uint32_t type, uint32_t protocol,
                        uint32_t *socket_index)
{
    (void)net;
    g_socket_open_calls++;
    if (domain != 2u || type != 2u || protocol != 17u) return NETDIMM_NET_ERR_ARGUMENT;
    *socket_index = 7u;
    return NETDIMM_NET_OK;
}

int netdimm_socket_set_timeout(netdimm_net_t *net, uint32_t socket_index,
                               uint32_t connect_timeout_ms,
                               uint32_t send_timeout_ms,
                               uint32_t recv_timeout_ms,
                               uint32_t *command_result)
{
    (void)net;
    g_timeout_calls++;
    if (socket_index != 7u || connect_timeout_ms == 0u || send_timeout_ms == 0u)
        return NETDIMM_NET_ERR_ARGUMENT;
    g_last_recv_timeout = recv_timeout_ms;
    *command_result = g_timeout_result;
    return g_settimeout_rc;
}

int netdimm_socket_connect_ipv4(netdimm_net_t *net, uint32_t socket_index,
                                const uint8_t ipv4[4], uint16_t port,
                                uint32_t wait_frames, uint32_t *connect_result)
{
    (void)net;
    (void)wait_frames;
    g_connect_calls++;
    if (socket_index != 7u || ipv4[0] != 127u || port != 19001u)
        return NETDIMM_NET_ERR_ARGUMENT;
    *connect_result = g_connect_result;
    return g_connect_rc;
}

int netdimm_socket_send(netdimm_net_t *net, uint32_t socket_index,
                        const void *data, uint32_t length,
                        uint32_t wait_frames, uint32_t *bytes_sent)
{
    (void)net;
    (void)wait_frames;
    g_send_calls++;
    if (socket_index != 7u || length > sizeof(g_sent_payload))
        return NETDIMM_NET_ERR_ARGUMENT;
    memcpy(g_sent_payload, data, length);
    g_sent_payload_len = length;
    *bytes_sent = g_send_result != 0u ? g_send_result : length;
    return g_send_rc;
}

int netdimm_socket_recv(netdimm_net_t *net, uint32_t socket_index,
                        void *destination, uint32_t capacity,
                        uint32_t wait_frames, uint32_t *bytes_received)
{
    (void)net;
    (void)wait_frames;
    g_recv_calls++;
    if (socket_index != 7u) return NETDIMM_NET_ERR_ARGUMENT;
    if (g_recv_rc != NETDIMM_NET_OK) {
        *bytes_received = 0u;
        return g_recv_rc;
    }
    if (g_recv_payload_len > capacity) return NETDIMM_NET_ERR_RANGE;
    memcpy(destination, g_recv_payload, g_recv_payload_len);
    *bytes_received = (uint32_t)g_recv_payload_len;
    return NETDIMM_NET_OK;
}

int netdimm_socket_get_errno(netdimm_net_t *net, uint32_t socket_index,
                             uint32_t *socket_error)
{
    (void)net;
    g_errno_calls++;
    if (socket_index != 7u) return NETDIMM_NET_ERR_ARGUMENT;
    *socket_error = g_socket_errno;
    return NETDIMM_NET_OK;
}

int netdimm_socket_close(netdimm_net_t *net, uint32_t socket_index,
                         uint32_t *close_result)
{
    (void)net;
    g_close_calls++;
    if (socket_index != 7u) return NETDIMM_NET_ERR_ARGUMENT;
    *close_result = 0u;
    return NETDIMM_NET_OK;
}

static void reset_fake(void)
{
    g_socket_open_calls = 0u;
    g_timeout_calls = 0u;
    g_connect_calls = 0u;
    g_send_calls = 0u;
    g_recv_calls = 0u;
    g_errno_calls = 0u;
    g_close_calls = 0u;
    g_last_recv_timeout = 0u;
    g_socket_errno = 0u;
    g_recv_rc = NETDIMM_NET_OK;
    g_send_rc = NETDIMM_NET_OK;
    g_settimeout_rc = NETDIMM_NET_OK;
    g_connect_rc = NETDIMM_NET_OK;
    g_send_result = 0u;
    g_connect_result = 0u;
    g_timeout_result = 0u;
    g_recv_result = 0u;
    g_recv_payload_len = 0u;
    g_sent_payload_len = 0u;
}

int main(void)
{
    netdimm_net_t net;
    netplay_netdimm_t adapter;
    netplay_transport_t transport;
    const uint8_t relay_ip[4] = {127u, 0u, 0u, 1u};
    const uint8_t tx[] = {0x4Eu, 0x44u, 0x4Eu, 0x50u, 1u, 2u, 3u};
    uint8_t rx[32];
    size_t received;
    int rc;

    reset_fake();
    memset(&net, 0, sizeof(net));
    net.dimm_size = 256u * 1024u * 1024u;

    rc = netplay_netdimm_open(&adapter, &net, relay_ip, 19001u,
                              8000u, 900u, &transport);
    check(rc == NETPLAY_NETDIMM_OK, "open UDP socket and connect relay");
    check(g_socket_open_calls == 1u && g_connect_calls == 1u,
          "socket open/connect called once");
    check(transport.context == &adapter && transport.send_datagram != NULL &&
          transport.receive_datagram != NULL, "transport callbacks installed");
    check(g_last_recv_timeout == 8000u, "initial receive timeout configured");

    rc = transport.send_datagram(transport.context, tx, sizeof(tx));
    check(rc == 0 && g_sent_payload_len == sizeof(tx) &&
          memcmp(g_sent_payload, tx, sizeof(tx)) == 0, "send callback preserves datagram");

    g_send_rc = NETDIMM_NET_ERR_DEVICE;
    g_socket_errno = 111u;
    rc = transport.send_datagram(transport.context, tx, sizeof(tx));
    check(rc == -1 && netplay_netdimm_last_socket_error(&adapter) == 111u,
          "send callback reports socket error");
    g_send_rc = NETDIMM_NET_OK;
    g_socket_errno = 0u;

    memcpy(g_recv_payload, tx, sizeof(tx));
    g_recv_payload_len = sizeof(tx);
    received = 0u;
    rc = transport.receive_datagram(transport.context, rx, sizeof(rx), &received, 5000u);
    check(rc == 1 && received == sizeof(tx) && memcmp(rx, tx, sizeof(tx)) == 0,
          "receive callback returns datagram unchanged");
    check(g_last_recv_timeout == 5000u, "receive callback applies requested timeout");

    g_recv_rc = NETDIMM_NET_ERR_DEVICE;
    g_socket_errno = 110u;
    received = 99u;
    rc = transport.receive_datagram(transport.context, rx, sizeof(rx), &received, 5000u);
    check(rc == 0 && received == 0u, "ETIMEDOUT maps to generic timeout");

    g_socket_errno = 11u;
    rc = transport.receive_datagram(transport.context, rx, sizeof(rx), &received, 5000u);
    check(rc == 0, "EAGAIN maps to generic timeout");

    g_socket_errno = 111u;
    rc = transport.receive_datagram(transport.context, rx, sizeof(rx), &received, 5000u);
    check(rc == -1, "non-timeout socket error remains hard failure");

    g_recv_rc = NETDIMM_NET_ERR_TIMEOUT;
    rc = transport.receive_datagram(transport.context, rx, sizeof(rx), &received, 5000u);
    check(rc == -1, "DIMM command timeout remains hard failure");

    rc = netplay_netdimm_close(&adapter);
    check(rc == NETPLAY_NETDIMM_OK && g_close_calls == 1u,
          "close underlying NetDIMM socket");

    printf("NETDIMM TRANSPORT ADAPTER TEST: %u checks, %u failures\n",
           g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
