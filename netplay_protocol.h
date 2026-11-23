#ifndef NETPLAY_PROTOCOL_H
#define NETPLAY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire format compatible with netplay_relay_server.py (NDNP v1). */
#define NETPLAY_PROTOCOL_VERSION 1u
#define NETPLAY_HEADER_SIZE      16u
#define NETPLAY_MAX_DATAGRAM     1200u
#define NETPLAY_MAX_PAYLOAD      (NETPLAY_MAX_DATAGRAM - NETPLAY_HEADER_SIZE)
#define NETPLAY_ROOM_MIN_LENGTH  4u
#define NETPLAY_ROOM_MAX_LENGTH  8u

enum {
    NETPLAY_MSG_HOST      = 1u,
    NETPLAY_MSG_JOIN      = 2u,
    NETPLAY_MSG_WAITING   = 3u,
    NETPLAY_MSG_MATCH     = 4u,
    NETPLAY_MSG_DATA      = 5u,
    NETPLAY_MSG_PING      = 6u,
    NETPLAY_MSG_PONG      = 7u,
    NETPLAY_MSG_LEAVE     = 8u,
    NETPLAY_MSG_PEER_LEFT = 9u,
    NETPLAY_MSG_ERROR     = 10u
};

enum {
    NETPLAY_ROLE_HOST = 1u,
    NETPLAY_ROLE_JOIN = 2u
};

enum {
    NETPLAY_PROTOCOL_OK = 0,
    NETPLAY_PROTOCOL_ERR_ARGUMENT = -1,
    NETPLAY_PROTOCOL_ERR_CAPACITY = -2,
    NETPLAY_PROTOCOL_ERR_LENGTH = -3,
    NETPLAY_PROTOCOL_ERR_MAGIC = -4,
    NETPLAY_PROTOCOL_ERR_VERSION = -5,
    NETPLAY_PROTOCOL_ERR_TYPE = -6,
    NETPLAY_PROTOCOL_ERR_ROOM = -7
};

typedef struct netplay_packet {
    uint8_t type;
    uint16_t payload_length;
    uint32_t session_id;
    uint32_t sequence;
    const uint8_t *payload; /* Points into the caller's input datagram. */
} netplay_packet_t;

/* Encode a complete NDNP datagram. Payload may be NULL only when length is 0. */
int netplay_packet_encode(uint8_t *output,
                          size_t output_capacity,
                          uint8_t type,
                          uint32_t session_id,
                          uint32_t sequence,
                          const void *payload,
                          uint16_t payload_length,
                          size_t *output_length);

/* Decode and validate a complete NDNP datagram. */
int netplay_packet_decode(const uint8_t *datagram,
                          size_t datagram_length,
                          netplay_packet_t *packet);

/* Validate ASCII [A-Za-z0-9] room code and return its length. */
int netplay_room_code_length(const char *room_code, size_t *length);

const char *netplay_protocol_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_PROTOCOL_H */
