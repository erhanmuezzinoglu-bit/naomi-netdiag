#include "netplay_protocol.h"

#include <string.h>

static const uint8_t ndnp_magic[4] = { 'N', 'D', 'N', 'P' };

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int valid_message_type(uint8_t type)
{
    return type >= NETPLAY_MSG_HOST && type <= NETPLAY_MSG_ERROR;
}

int netplay_packet_encode(uint8_t *output,
                          size_t output_capacity,
                          uint8_t type,
                          uint32_t session_id,
                          uint32_t sequence,
                          const void *payload,
                          uint16_t payload_length,
                          size_t *output_length)
{
    size_t total_length;

    if (output == NULL || output_length == NULL ||
        (payload_length != 0u && payload == NULL)) {
        return NETPLAY_PROTOCOL_ERR_ARGUMENT;
    }
    if (!valid_message_type(type)) {
        return NETPLAY_PROTOCOL_ERR_TYPE;
    }
    if (payload_length > NETPLAY_MAX_PAYLOAD) {
        return NETPLAY_PROTOCOL_ERR_LENGTH;
    }

    total_length = NETPLAY_HEADER_SIZE + (size_t)payload_length;
    if (output_capacity < total_length) {
        return NETPLAY_PROTOCOL_ERR_CAPACITY;
    }

    memcpy(output, ndnp_magic, sizeof(ndnp_magic));
    output[4] = NETPLAY_PROTOCOL_VERSION;
    output[5] = type;
    write_be16(&output[6], payload_length);
    write_be32(&output[8], session_id);
    write_be32(&output[12], sequence);

    if (payload_length != 0u) {
        memcpy(&output[NETPLAY_HEADER_SIZE], payload, payload_length);
    }

    *output_length = total_length;
    return NETPLAY_PROTOCOL_OK;
}

int netplay_packet_decode(const uint8_t *datagram,
                          size_t datagram_length,
                          netplay_packet_t *packet)
{
    uint16_t payload_length;

    if (datagram == NULL || packet == NULL) {
        return NETPLAY_PROTOCOL_ERR_ARGUMENT;
    }
    if (datagram_length < NETPLAY_HEADER_SIZE ||
        datagram_length > NETPLAY_MAX_DATAGRAM) {
        return NETPLAY_PROTOCOL_ERR_LENGTH;
    }
    if (memcmp(datagram, ndnp_magic, sizeof(ndnp_magic)) != 0) {
        return NETPLAY_PROTOCOL_ERR_MAGIC;
    }
    if (datagram[4] != NETPLAY_PROTOCOL_VERSION) {
        return NETPLAY_PROTOCOL_ERR_VERSION;
    }
    if (!valid_message_type(datagram[5])) {
        return NETPLAY_PROTOCOL_ERR_TYPE;
    }

    payload_length = read_be16(&datagram[6]);
    if ((size_t)payload_length != datagram_length - NETPLAY_HEADER_SIZE ||
        payload_length > NETPLAY_MAX_PAYLOAD) {
        return NETPLAY_PROTOCOL_ERR_LENGTH;
    }

    packet->type = datagram[5];
    packet->payload_length = payload_length;
    packet->session_id = read_be32(&datagram[8]);
    packet->sequence = read_be32(&datagram[12]);
    packet->payload = &datagram[NETPLAY_HEADER_SIZE];
    return NETPLAY_PROTOCOL_OK;
}

int netplay_room_code_length(const char *room_code, size_t *length)
{
    size_t count = 0u;
    const unsigned char *p;

    if (room_code == NULL || length == NULL) {
        return NETPLAY_PROTOCOL_ERR_ARGUMENT;
    }

    p = (const unsigned char *)room_code;
    while (*p != '\0') {
        if (!((*p >= 'A' && *p <= 'Z') ||
              (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9'))) {
            return NETPLAY_PROTOCOL_ERR_ROOM;
        }
        count++;
        if (count > NETPLAY_ROOM_MAX_LENGTH) {
            return NETPLAY_PROTOCOL_ERR_ROOM;
        }
        p++;
    }

    if (count < NETPLAY_ROOM_MIN_LENGTH) {
        return NETPLAY_PROTOCOL_ERR_ROOM;
    }

    *length = count;
    return NETPLAY_PROTOCOL_OK;
}

const char *netplay_protocol_error_string(int error)
{
    switch (error) {
        case NETPLAY_PROTOCOL_OK:
            return "success";
        case NETPLAY_PROTOCOL_ERR_ARGUMENT:
            return "invalid protocol argument";
        case NETPLAY_PROTOCOL_ERR_CAPACITY:
            return "output buffer too small";
        case NETPLAY_PROTOCOL_ERR_LENGTH:
            return "invalid datagram or payload length";
        case NETPLAY_PROTOCOL_ERR_MAGIC:
            return "invalid NDNP magic";
        case NETPLAY_PROTOCOL_ERR_VERSION:
            return "unsupported NDNP version";
        case NETPLAY_PROTOCOL_ERR_TYPE:
            return "unknown NDNP message type";
        case NETPLAY_PROTOCOL_ERR_ROOM:
            return "room code must be 4-8 ASCII letters or digits";
        default:
            return "unknown NDNP protocol error";
    }
}
