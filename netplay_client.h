#ifndef NETPLAY_CLIENT_H
#define NETPLAY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "netplay_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Datagram transport contract:
 *   send_datagram: return 0 only when the entire datagram was accepted.
 *   receive_datagram: return 1 for a datagram, 0 for timeout, -1 for error.
 * The client owns neither the transport nor its context.
 */
typedef struct netplay_transport {
    void *context;
    int (*send_datagram)(void *context, const uint8_t *data, size_t length);
    int (*receive_datagram)(void *context, uint8_t *data, size_t capacity,
                            size_t *received, uint32_t timeout_ms);
} netplay_transport_t;

typedef struct netplay_client_options {
    uint8_t role;
    const char *room_code;
    uint32_t match_timeout_ms;
    uint32_t receive_timeout_ms;
    uint8_t data_packet_count;
} netplay_client_options_t;

typedef struct netplay_client_result {
    uint32_t session_id;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t ping_sequence;
    uint8_t matched;
    uint8_t ping_passed;
    uint8_t peer_left;
    uint8_t reserved;
    int error_code;
    char error_message[96];
} netplay_client_result_t;

typedef void (*netplay_client_event_fn)(void *opaque,
                                        const char *stage,
                                        const char *detail);

enum {
    NETPLAY_CLIENT_OK = 0,
    NETPLAY_CLIENT_ERR_ARGUMENT = -100,
    NETPLAY_CLIENT_ERR_TRANSPORT = -101,
    NETPLAY_CLIENT_ERR_PROTOCOL = -102,
    NETPLAY_CLIENT_ERR_RELAY = -103,
    NETPLAY_CLIENT_ERR_TIMEOUT = -104,
    NETPLAY_CLIENT_ERR_MISMATCH = -105
};

#define NETPLAY_CLIENT_DEFAULT_MATCH_TIMEOUT_MS 90000u
#define NETPLAY_CLIENT_DEFAULT_RECV_TIMEOUT_MS   5000u
#define NETPLAY_CLIENT_DEFAULT_DATA_PACKETS       5u
#define NETPLAY_CLIENT_MAX_DATA_PACKETS           32u

/*
 * Run the NDNP v1 smoke-test session over any connected UDP datagram
 * transport. HOST/JOIN registration, MATCH, DATA relay, PING/PONG, DONE
 * barrier and LEAVE/PEER_LEFT follow the Python reference client.
 */
int netplay_client_run(const netplay_transport_t *transport,
                       const netplay_client_options_t *options,
                       netplay_client_result_t *result,
                       netplay_client_event_fn event_fn,
                       void *event_opaque);

const char *netplay_client_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_CLIENT_H */
