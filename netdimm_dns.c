#include "netdimm_dns.h"

#include <stddef.h>
#include <string.h>

#define DNS_PORT                 53u
#define DNS_PACKET_CAPACITY      512u
#define DNS_TRANSACTION_ID       0x4E44u
#define DNS_DEFAULT_TIMEOUT_MS   8000u
#define DNS_AF_INET              2u
#define DNS_SOCK_DGRAM           2u
#define DNS_IPPROTO_UDP          17u

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static int build_dns_query(const char *hostname,
                           uint8_t *packet,
                           size_t capacity,
                           size_t *packet_length)
{
    size_t out = 12u;
    const char *label;
    size_t name_length;
    size_t encoded_name_length = 1u; /* Root label. */

    if (hostname == NULL || packet == NULL || packet_length == NULL) {
        return NETDIMM_DNS_ERR_ARGUMENT;
    }

    name_length = strlen(hostname);
    if (name_length == 0u || name_length > 254u) {
        return NETDIMM_DNS_ERR_NAME;
    }

    memset(packet, 0, capacity);
    write_be16(&packet[0], DNS_TRANSACTION_ID);
    write_be16(&packet[2], 0x0100u); /* Standard query, recursion desired. */
    write_be16(&packet[4], 1u);      /* QDCOUNT */

    label = hostname;
    while (*label != '\0') {
        const char *dot = label;
        size_t label_length;

        while (*dot != '\0' && *dot != '.') {
            dot++;
        }
        label_length = (size_t)(dot - label);
        if (label_length == 0u || label_length > 63u ||
            encoded_name_length + 1u + label_length > 255u ||
            out + 1u + label_length + 5u > capacity) {
            return NETDIMM_DNS_ERR_NAME;
        }
        encoded_name_length += 1u + label_length;

        packet[out++] = (uint8_t)label_length;
        memcpy(&packet[out], label, label_length);
        out += label_length;

        if (*dot == '\0') {
            label = dot;
        } else if (dot[1] == '\0') {
            /* Permit a fully-qualified name with one trailing dot. */
            label = dot + 1;
        } else {
            label = dot + 1;
        }
    }

    packet[out++] = 0u;
    write_be16(&packet[out], 1u); /* QTYPE=A */
    out += 2u;
    write_be16(&packet[out], 1u); /* QCLASS=IN */
    out += 2u;
    *packet_length = out;
    return NETDIMM_DNS_OK;
}

/* Skip an uncompressed name or a DNS compression pointer. */
static int skip_dns_name(const uint8_t *packet,
                         size_t packet_length,
                         size_t *offset)
{
    size_t pos;

    if (packet == NULL || offset == NULL || *offset >= packet_length) {
        return -1;
    }

    pos = *offset;
    for (;;) {
        uint8_t length;

        if (pos >= packet_length) {
            return -1;
        }
        length = packet[pos];

        if ((length & 0xC0u) == 0xC0u) {
            if (pos + 1u >= packet_length) {
                return -1;
            }
            *offset = pos + 2u;
            return 0;
        }
        if ((length & 0xC0u) != 0u || length > 63u) {
            return -1;
        }
        pos++;
        if (length == 0u) {
            *offset = pos;
            return 0;
        }
        if ((size_t)length > packet_length - pos) {
            return -1;
        }
        pos += length;
    }
}

static int parse_dns_a_response(const uint8_t *packet,
                                size_t packet_length,
                                uint16_t expected_id,
                                uint8_t ipv4_out[4],
                                netdimm_dns_result_t *result)
{
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t i;
    size_t offset = 12u;

    if (packet == NULL || ipv4_out == NULL || packet_length < 12u) {
        return NETDIMM_DNS_ERR_RESPONSE;
    }
    if (read_be16(&packet[0]) != expected_id) {
        return NETDIMM_DNS_ERR_RESPONSE;
    }

    flags = read_be16(&packet[2]);
    question_count = read_be16(&packet[4]);
    answer_count = read_be16(&packet[6]);

    if ((flags & 0x8000u) == 0u) { /* QR must identify a response. */
        return NETDIMM_DNS_ERR_RESPONSE;
    }
    if ((flags & 0x0200u) != 0u) { /* TC: response was truncated. */
        return NETDIMM_DNS_ERR_TRUNCATED;
    }

    if (result != NULL) {
        result->response_flags = flags;
        result->response_code = (uint8_t)(flags & 0x000Fu);
        result->answer_count = answer_count;
    }

    if ((flags & 0x000Fu) != 0u) {
        return ((flags & 0x000Fu) == 3u)
                   ? NETDIMM_DNS_ERR_NOT_FOUND
                   : NETDIMM_DNS_ERR_RCODE;
    }

    for (i = 0u; i < question_count; i++) {
        if (skip_dns_name(packet, packet_length, &offset) != 0 ||
            packet_length - offset < 4u) {
            return NETDIMM_DNS_ERR_RESPONSE;
        }
        offset += 4u; /* QTYPE + QCLASS */
    }

    for (i = 0u; i < answer_count; i++) {
        uint16_t type;
        uint16_t class_code;
        uint16_t data_length;

        if (skip_dns_name(packet, packet_length, &offset) != 0 ||
            packet_length - offset < 10u) {
            return NETDIMM_DNS_ERR_RESPONSE;
        }

        type = read_be16(&packet[offset]);
        class_code = read_be16(&packet[offset + 2u]);
        data_length = read_be16(&packet[offset + 8u]);
        offset += 10u;

        if ((size_t)data_length > packet_length - offset) {
            return NETDIMM_DNS_ERR_RESPONSE;
        }

        if (type == 1u && class_code == 1u && data_length == 4u) {
            memcpy(ipv4_out, &packet[offset], 4u);
            return NETDIMM_DNS_OK;
        }
        offset += data_length;
    }

    return NETDIMM_DNS_ERR_NOT_FOUND;
}

int netdimm_dns_resolve_a(netdimm_net_t *net,
                          const uint8_t dns_server[4],
                          const char *hostname,
                          uint32_t wait_frames,
                          uint8_t ipv4_out[4],
                          netdimm_dns_result_t *result)
{
    uint8_t query[DNS_PACKET_CAPACITY];
    uint8_t response[DNS_PACKET_CAPACITY];
    size_t query_length = 0u;
    uint32_t socket_index = 0u;
    uint32_t command_result = 0xFFFFFFFFu;
    uint32_t sent = 0u;
    uint32_t received = 0u;
    uint32_t socket_error = 0u;
    uint32_t close_result = 0xFFFFFFFFu;
    int rc;
    int close_rc;

    if (net == NULL || dns_server == NULL || hostname == NULL || ipv4_out == NULL) {
        return NETDIMM_DNS_ERR_ARGUMENT;
    }
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->transaction_id = DNS_TRANSACTION_ID;
    }
    memset(ipv4_out, 0, 4u);

    rc = build_dns_query(hostname, query, sizeof(query), &query_length);
    if (rc != NETDIMM_DNS_OK) {
        return rc;
    }

    rc = netdimm_socket_open(net, DNS_AF_INET, DNS_SOCK_DGRAM,
                             DNS_IPPROTO_UDP, &socket_index);
    if (rc != NETDIMM_NET_OK || socket_index == 0u || socket_index == 0xFFFFFFFFu) {
        return NETDIMM_DNS_ERR_DRIVER;
    }
    if (result != NULL) {
        result->socket_index = socket_index;
    }

    rc = netdimm_socket_set_timeout(net, socket_index,
                                    DNS_DEFAULT_TIMEOUT_MS,
                                    DNS_DEFAULT_TIMEOUT_MS,
                                    DNS_DEFAULT_TIMEOUT_MS,
                                    &command_result);
    if (rc != NETDIMM_NET_OK || command_result != 0u) {
        rc = NETDIMM_DNS_ERR_DRIVER;
        goto cleanup;
    }

    /* UDP connect sets the default peer; it does not perform a TCP handshake. */
    rc = netdimm_socket_connect_ipv4(net, socket_index, dns_server, DNS_PORT,
                                    wait_frames, &command_result);
    if (rc != NETDIMM_NET_OK || command_result != 0u) {
        rc = NETDIMM_DNS_ERR_DRIVER;
        goto cleanup;
    }

    rc = netdimm_socket_send(net, socket_index, query, (uint32_t)query_length,
                             wait_frames, &sent);
    if (rc != NETDIMM_NET_OK || sent != query_length) {
        rc = NETDIMM_DNS_ERR_DRIVER;
        goto cleanup;
    }
    if (result != NULL) {
        result->bytes_sent = sent;
    }

    rc = netdimm_socket_recv(net, socket_index, response, sizeof(response),
                             wait_frames, &received);
    if (rc != NETDIMM_NET_OK || received == 0u) {
        (void)netdimm_socket_get_errno(net, socket_index, &socket_error);
        if (result != NULL) {
            result->socket_error = socket_error;
        }
        rc = NETDIMM_DNS_ERR_DRIVER;
        goto cleanup;
    }
    if (result != NULL) {
        result->bytes_received = received;
    }

    rc = parse_dns_a_response(response, received, DNS_TRANSACTION_ID,
                              ipv4_out, result);

cleanup:
    close_rc = netdimm_socket_close(net, socket_index, &close_result);
    if (close_rc != NETDIMM_NET_OK || close_result != 0u) {
        if (rc == NETDIMM_DNS_OK) {
            rc = NETDIMM_DNS_ERR_DRIVER;
        }
    }
    return rc;
}

const char *netdimm_dns_error_string(int error)
{
    switch (error) {
        case NETDIMM_DNS_OK:
            return "success";
        case NETDIMM_DNS_ERR_ARGUMENT:
            return "invalid DNS argument";
        case NETDIMM_DNS_ERR_NAME:
            return "invalid DNS hostname";
        case NETDIMM_DNS_ERR_DRIVER:
            return "NetDIMM UDP operation failed";
        case NETDIMM_DNS_ERR_RESPONSE:
            return "invalid or mismatched DNS response";
        case NETDIMM_DNS_ERR_RCODE:
            return "DNS server returned an error response";
        case NETDIMM_DNS_ERR_NOT_FOUND:
            return "DNS name has no IPv4 A record";
        case NETDIMM_DNS_ERR_TRUNCATED:
            return "DNS response was truncated";
        default:
            return "unknown DNS error";
    }
}
