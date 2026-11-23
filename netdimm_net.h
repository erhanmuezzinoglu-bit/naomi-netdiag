#ifndef NETDIMM_NET_H
#define NETDIMM_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small NetDIMM 3.7 network-client driver for Naomi homebrew.
 *
 * This driver talks to the Naomi cartridge DIMM PIO registers. It is not a
 * Linux driver and does not use libnaomi's dimm_comms PEEK/POKE callbacks.
 */

enum {
    NETDIMM_NET_OK = 0,
    NETDIMM_NET_ERR_ARGUMENT = -1,
    NETDIMM_NET_ERR_CONFIG = -2,
    NETDIMM_NET_ERR_RANGE = -3,
    NETDIMM_NET_ERR_DEVICE = -4,
    NETDIMM_NET_ERR_TIMEOUT = -5
};

#define NETDIMM_NET_DEFAULT_WAIT_FRAMES 900u
#define NETDIMM_NET_DEFAULT_TIMEOUT_MS  8000u
#define NETDIMM_NET_MAX_IO              65536u

typedef void (*netdimm_net_progress_fn)(void *opaque);

typedef struct netdimm_net {
    uint32_t bios_config;
    uint32_t dimm_size;
    uint32_t dimm_buffer_offset;
    uint32_t command_buffer_offset;
    uint32_t sockaddr_offset;
    uint32_t tx_offset;
    uint32_t rx_offset;
    uint32_t scratch_offset;

    uint16_t last_command;
    uint16_t last_status;
    uint32_t last_result;

    netdimm_net_progress_fn progress;
    void *progress_opaque;
} netdimm_net_t;

/* Read BIOS DIMM configuration and calculate all DIMM buffer addresses. */
int netdimm_net_init(netdimm_net_t *net);

/* Optional write/read test through the Naomi cartridge PIO data port. */
int netdimm_net_pio_self_test(netdimm_net_t *net, uint32_t marker);

/* Called while the driver waits for a DIMM command to complete. */
void netdimm_net_set_progress_callback(netdimm_net_t *net,
                                       netdimm_net_progress_fn callback,
                                       void *opaque);

/* Common NetDIMM socket operations. All functions return NETDIMM_NET_OK
 * when the NetDIMM command completed successfully. The host result (socket
 * index, byte count, errno, etc.) is returned through the output argument. */
int netdimm_socket_open(netdimm_net_t *net,
                        uint32_t domain,
                        uint32_t type,
                        uint32_t protocol,
                        uint32_t *socket_index);

int netdimm_socket_set_timeout(netdimm_net_t *net,
                               uint32_t socket_index,
                               uint32_t connect_timeout_ms,
                               uint32_t send_timeout_ms,
                               uint32_t recv_timeout_ms,
                               uint32_t *command_result);

/* ipv4 is four network-order bytes; port is supplied in host byte order. */
int netdimm_socket_connect_ipv4(netdimm_net_t *net,
                                uint32_t socket_index,
                                const uint8_t ipv4[4],
                                uint16_t port,
                                uint32_t wait_frames,
                                uint32_t *connect_result);

int netdimm_socket_send(netdimm_net_t *net,
                        uint32_t socket_index,
                        const void *data,
                        uint32_t length,
                        uint32_t wait_frames,
                        uint32_t *bytes_sent);

/* Receives at most capacity bytes and copies them into destination. */
int netdimm_socket_recv(netdimm_net_t *net,
                        uint32_t socket_index,
                        void *destination,
                        uint32_t capacity,
                        uint32_t wait_frames,
                        uint32_t *bytes_received);

int netdimm_socket_get_errno(netdimm_net_t *net,
                             uint32_t socket_index,
                             uint32_t *socket_error);

int netdimm_socket_close(netdimm_net_t *net,
                         uint32_t socket_index,
                         uint32_t *close_result);

const char *netdimm_net_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* NETDIMM_NET_H */
