#ifndef NETDIMM_DNS_H
#define NETDIMM_DNS_H

#include <stdint.h>
#include "netdimm_net.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NETDIMM_DNS_OK = 0,
    NETDIMM_DNS_ERR_ARGUMENT = -20,
    NETDIMM_DNS_ERR_NAME = -21,
    NETDIMM_DNS_ERR_DRIVER = -22,
    NETDIMM_DNS_ERR_RESPONSE = -23,
    NETDIMM_DNS_ERR_RCODE = -24,
    NETDIMM_DNS_ERR_NOT_FOUND = -25,
    NETDIMM_DNS_ERR_TRUNCATED = -26
};

typedef struct netdimm_dns_result {
    uint32_t socket_index;
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t answer_count;
    uint32_t socket_error;
    uint16_t transaction_id;
    uint16_t response_flags;
    uint8_t response_code;
    uint8_t reserved[3];
} netdimm_dns_result_t;

/*
 * Resolve an IPv4 A record using a connected UDP socket.
 *
 * dns_server: four IPv4 bytes in network order, e.g. {1,1,1,1}.
 * hostname: ASCII DNS name such as "example.com" (a trailing dot is allowed).
 * wait_frames: NetDIMM command wait; 0 selects the driver default.
 * ipv4_out: receives four network-order IPv4 bytes.
 * result: optional diagnostic counters and DNS response metadata.
 *
 * This deliberately does not use NetDIMM getIpByDns: Flycast's current HLE
 * returns its configured vfnet server address for that command rather than
 * resolving the supplied hostname. Instead this sends a normal DNS packet
 * through SOCK_DGRAM + connect + send + recv.
 */
int netdimm_dns_resolve_a(netdimm_net_t *net,
                          const uint8_t dns_server[4],
                          const char *hostname,
                          uint32_t wait_frames,
                          uint8_t ipv4_out[4],
                          netdimm_dns_result_t *result);

const char *netdimm_dns_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* NETDIMM_DNS_H */
