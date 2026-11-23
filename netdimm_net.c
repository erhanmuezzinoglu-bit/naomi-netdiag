#include "netdimm_net.h"

#include <string.h>

/* Naomi SH-4 cartridge PIO and NetDIMM registers. */
#define REG16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define REG32(address) (*(volatile uint32_t *)(uintptr_t)(address))

#define BIOS_DIMM_CONFIG_ADDR 0x0C01FC04u

#define ROM_PIO_OFFSET_H_ADDR 0xA05F7000u
#define ROM_PIO_OFFSET_L_ADDR 0xA05F7004u
#define ROM_PIO_DATA_ADDR     0xA05F7008u

#define DIMM_COMMAND_ADDR     0xA05F703Cu
#define DIMM_OFFSET_ADDR      0xA05F7040u
#define DIMM_PARAM_LO_ADDR    0xA05F7044u
#define DIMM_PARAM_HI_ADDR    0xA05F7048u
#define DIMM_STATUS_ADDR      0xA05F704Cu

#define NETDIMM_SLOT           1u
#define NETDIMM_GROUP_NETWORK  0x2000u
#define NETDIMM_RESPONSE_OK    0x2404u
#define NETDIMM_RESPONSE_ERR   0x24FFu

#define MIB                    (1024u * 1024u)
#define NETDIMM_WINDOW_SIZE    (16u * MIB)
#define NETDIMM_NET_AREA       (8u * MIB)
#define NETDIMM_COMMAND_STRIDE 0x1000u

/* Separate, non-overlapping work buffers in the reserved 16 MiB DIMM window. */
#define NETDIMM_ADDR_OFFSET    0x00900000u
#define NETDIMM_TX_OFFSET      0x00910000u
#define NETDIMM_RX_OFFSET      0x00920000u
#define NETDIMM_SCRATCH_OFFSET 0x00F00000u

#define NETDIMM_CONTROL_WAIT_FRAMES 180u
#define NETDIMM_COMMAND_WORDS       8u

static void pio_set_offset(uint32_t offset)
{
    /* Bit 15 enables cartridge ROM PIO auto-increment. */
    REG16(ROM_PIO_OFFSET_H_ADDR) =
        (uint16_t)(((offset >> 16) & 0x7FFFu) | 0x8000u);
    REG16(ROM_PIO_OFFSET_L_ADDR) = (uint16_t)(offset & 0xFFFFu);
}

static void pio_write_bytes(uint32_t offset, const uint8_t *source, size_t length)
{
    size_t i;

    pio_set_offset(offset);
    for (i = 0; i < length; i += 2u) {
        uint16_t value = source[i];
        if (i + 1u < length) {
            value |= (uint16_t)((uint16_t)source[i + 1u] << 8);
        }
        REG16(ROM_PIO_DATA_ADDR) = value;
    }
}

static void pio_read_bytes(uint32_t offset, uint8_t *destination, size_t length)
{
    size_t i;

    pio_set_offset(offset);
    for (i = 0; i < length; i += 2u) {
        uint16_t value = REG16(ROM_PIO_DATA_ADDR);
        destination[i] = (uint8_t)(value & 0xFFu);
        if (i + 1u < length) {
            destination[i + 1u] = (uint8_t)(value >> 8);
        }
    }
}

static void pio_write_words(uint32_t offset, const uint32_t *words, size_t count)
{
    size_t i;

    pio_set_offset(offset);
    for (i = 0; i < count; i++) {
        REG16(ROM_PIO_DATA_ADDR) = (uint16_t)(words[i] & 0xFFFFu);
        REG16(ROM_PIO_DATA_ADDR) = (uint16_t)(words[i] >> 16);
    }
}

static int netdimm_command(netdimm_net_t *net,
                           uint32_t opcode,
                           const uint32_t *arguments,
                           size_t argument_count,
                           uint32_t wait_frames,
                           uint32_t *result)
{
    uint32_t words[NETDIMM_COMMAND_WORDS];
    size_t i;

    if (net == NULL || argument_count > NETDIMM_COMMAND_WORDS - 1u ||
        (argument_count != 0u && arguments == NULL)) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }
    if (wait_frames == 0u) {
        wait_frames = NETDIMM_NET_DEFAULT_WAIT_FRAMES;
    }

    memset(words, 0, sizeof(words));
    words[0] = opcode;
    for (i = 0; i < argument_count; i++) {
        words[i + 1u] = arguments[i];
    }

    /* Flycast NetDimm::netCmd() reads this slot's command packet here. */
    pio_write_words(net->command_buffer_offset, words, argument_count + 1u);

    /* Clear a stale DIMM interrupt without launching another command. */
    REG16(DIMM_STATUS_ADDR) = 0x0101u;

    REG16(DIMM_COMMAND_ADDR) =
        (uint16_t)(NETDIMM_GROUP_NETWORK | NETDIMM_SLOT);
    REG16(DIMM_OFFSET_ADDR) = 0u;
    REG16(DIMM_PARAM_LO_ADDR) = 0u;
    REG16(DIMM_PARAM_HI_ADDR) = 0u;

    /* A zero in DIMM_STATUS bit 0 starts processing. */
    REG16(DIMM_STATUS_ADDR) = 0u;

    for (i = 0; i < wait_frames; i++) {
        net->last_command = REG16(DIMM_COMMAND_ADDR);
        net->last_status = REG16(DIMM_STATUS_ADDR);

        if (net->last_command == NETDIMM_RESPONSE_OK ||
            net->last_command == NETDIMM_RESPONSE_ERR) {
            net->last_result = ((uint32_t)REG16(DIMM_PARAM_HI_ADDR) << 16) |
                               (uint32_t)REG16(DIMM_PARAM_LO_ADDR);
            if (result != NULL) {
                *result = net->last_result;
            }
            return net->last_command == NETDIMM_RESPONSE_OK
                       ? NETDIMM_NET_OK
                       : NETDIMM_NET_ERR_DEVICE;
        }

        if (net->progress != NULL) {
            net->progress(net->progress_opaque);
        }
    }

    net->last_command = REG16(DIMM_COMMAND_ADDR);
    net->last_status = REG16(DIMM_STATUS_ADDR);
    net->last_result = 0xFFFFFFFFu;
    if (result != NULL) {
        *result = net->last_result;
    }
    return NETDIMM_NET_ERR_TIMEOUT;
}

int netdimm_net_init(netdimm_net_t *net)
{
    uint32_t size_code;

    if (net == NULL) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }

    memset(net, 0, sizeof(*net));
    net->bios_config = REG32(BIOS_DIMM_CONFIG_ADDR);

    /* NetDimm::systemCmd(0xf) publishes the configured DIMM size here. */
    if ((net->bios_config & 0x70000000u) != 0x70000000u) {
        return NETDIMM_NET_ERR_CONFIG;
    }

    size_code = (net->bios_config >> 16) & 0x3u;
    switch (size_code) {
        case 1u:
            net->dimm_size = 128u * MIB;
            break;
        case 2u:
            net->dimm_size = 256u * MIB;
            break;
        case 3u:
            net->dimm_size = 512u * MIB;
            break;
        default:
            return NETDIMM_NET_ERR_CONFIG;
    }

    /* The low BIOS bits encode dimmBufferOffset in MiB. */
    net->dimm_buffer_offset = (net->bios_config & 0x7FFu) << 20;
    if (net->dimm_buffer_offset != net->dimm_size - NETDIMM_WINDOW_SIZE) {
        return NETDIMM_NET_ERR_CONFIG;
    }

    net->command_buffer_offset = net->dimm_buffer_offset + NETDIMM_NET_AREA +
                                 NETDIMM_COMMAND_STRIDE * NETDIMM_SLOT;
    net->sockaddr_offset = net->dimm_buffer_offset + NETDIMM_ADDR_OFFSET;
    net->tx_offset = net->dimm_buffer_offset + NETDIMM_TX_OFFSET;
    net->rx_offset = net->dimm_buffer_offset + NETDIMM_RX_OFFSET;
    net->scratch_offset = net->dimm_buffer_offset + NETDIMM_SCRATCH_OFFSET;

    if (net->command_buffer_offset + 0x1000u > net->dimm_size ||
        net->sockaddr_offset + 16u > net->dimm_size ||
        net->tx_offset + NETDIMM_NET_MAX_IO > net->dimm_size ||
        net->rx_offset + NETDIMM_NET_MAX_IO > net->dimm_size ||
        net->scratch_offset + 4u > net->dimm_size) {
        memset(net, 0, sizeof(*net));
        return NETDIMM_NET_ERR_RANGE;
    }

    return NETDIMM_NET_OK;
}

int netdimm_net_pio_self_test(netdimm_net_t *net, uint32_t marker)
{
    uint8_t write_data[4];
    uint8_t read_data[4];

    if (net == NULL || net->dimm_size == 0u) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }

    write_data[0] = (uint8_t)(marker & 0xFFu);
    write_data[1] = (uint8_t)((marker >> 8) & 0xFFu);
    write_data[2] = (uint8_t)((marker >> 16) & 0xFFu);
    write_data[3] = (uint8_t)((marker >> 24) & 0xFFu);

    pio_write_bytes(net->scratch_offset, write_data, sizeof(write_data));
    pio_read_bytes(net->scratch_offset, read_data, sizeof(read_data));

    return memcmp(write_data, read_data, sizeof(write_data)) == 0
               ? NETDIMM_NET_OK
               : NETDIMM_NET_ERR_DEVICE;
}

void netdimm_net_set_progress_callback(netdimm_net_t *net,
                                       netdimm_net_progress_fn callback,
                                       void *opaque)
{
    if (net != NULL) {
        net->progress = callback;
        net->progress_opaque = opaque;
    }
}

int netdimm_socket_open(netdimm_net_t *net,
                        uint32_t domain,
                        uint32_t type,
                        uint32_t protocol,
                        uint32_t *socket_index)
{
    uint32_t args[3];
    uint32_t result = 0xFFFFFFFFu;
    int rc;

    if (net == NULL || socket_index == NULL) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }
    args[0] = domain;
    args[1] = type;
    args[2] = protocol;
    rc = netdimm_command(net, 11u, args, 3u,
                         NETDIMM_CONTROL_WAIT_FRAMES, &result);
    if (rc == NETDIMM_NET_OK) {
        *socket_index = result;
    }
    return rc;
}

int netdimm_socket_set_timeout(netdimm_net_t *net,
                               uint32_t socket_index,
                               uint32_t connect_timeout_ms,
                               uint32_t send_timeout_ms,
                               uint32_t recv_timeout_ms,
                               uint32_t *command_result)
{
    uint32_t args[4];
    args[0] = socket_index;
    args[1] = connect_timeout_ms;
    args[2] = send_timeout_ms;
    args[3] = recv_timeout_ms;
    return netdimm_command(net, 16u, args, 4u,
                           NETDIMM_CONTROL_WAIT_FRAMES, command_result);
}

int netdimm_socket_connect_ipv4(netdimm_net_t *net,
                                uint32_t socket_index,
                                const uint8_t ipv4[4],
                                uint16_t port,
                                uint32_t wait_frames,
                                uint32_t *connect_result)
{
    uint8_t sockaddr_data[16];
    uint32_t args[3];

    if (net == NULL || ipv4 == NULL || socket_index == 0u) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }

    memset(sockaddr_data, 0, sizeof(sockaddr_data));
    sockaddr_data[0] = 2u; /* AF_INET; Flycast currently ignores this field. */
    sockaddr_data[1] = 0u;
    sockaddr_data[2] = (uint8_t)(port >> 8);
    sockaddr_data[3] = (uint8_t)(port & 0xFFu);
    memcpy(&sockaddr_data[4], ipv4, 4u);
    pio_write_bytes(net->sockaddr_offset, sockaddr_data, sizeof(sockaddr_data));

    args[0] = socket_index;
    args[1] = net->sockaddr_offset;
    args[2] = sizeof(sockaddr_data);
    return netdimm_command(net, 4u, args, 3u, wait_frames, connect_result);
}

int netdimm_socket_send(netdimm_net_t *net,
                        uint32_t socket_index,
                        const void *data,
                        uint32_t length,
                        uint32_t wait_frames,
                        uint32_t *bytes_sent)
{
    uint32_t args[3];
    uint32_t result = 0xFFFFFFFFu;
    int rc;

    if (net == NULL || socket_index == 0u || data == NULL || length == 0u) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }
    if (length > NETDIMM_NET_MAX_IO) {
        return NETDIMM_NET_ERR_RANGE;
    }

    pio_write_bytes(net->tx_offset, (const uint8_t *)data, length);
    args[0] = socket_index;
    args[1] = net->tx_offset;
    args[2] = length;
    rc = netdimm_command(net, 10u, args, 3u, wait_frames, &result);

    if (rc == NETDIMM_NET_OK && result > length) {
        return NETDIMM_NET_ERR_DEVICE;
    }
    if (bytes_sent != NULL) {
        *bytes_sent = (rc == NETDIMM_NET_OK) ? result : 0u;
    }
    return rc;
}

int netdimm_socket_recv(netdimm_net_t *net,
                        uint32_t socket_index,
                        void *destination,
                        uint32_t capacity,
                        uint32_t wait_frames,
                        uint32_t *bytes_received)
{
    uint32_t args[3];
    uint32_t result = 0xFFFFFFFFu;
    int rc;

    if (net == NULL || socket_index == 0u || destination == NULL || capacity == 0u) {
        return NETDIMM_NET_ERR_ARGUMENT;
    }
    if (capacity > NETDIMM_NET_MAX_IO) {
        return NETDIMM_NET_ERR_RANGE;
    }

    args[0] = socket_index;
    args[1] = net->rx_offset;
    args[2] = capacity;
    rc = netdimm_command(net, 9u, args, 3u, wait_frames, &result);
    if (rc != NETDIMM_NET_OK) {
        if (bytes_received != NULL) {
            *bytes_received = 0u;
        }
        return rc;
    }
    if (result > capacity) {
        return NETDIMM_NET_ERR_DEVICE;
    }

    if (result != 0u) {
        pio_read_bytes(net->rx_offset, (uint8_t *)destination, result);
    }
    if (bytes_received != NULL) {
        *bytes_received = result;
    }
    return NETDIMM_NET_OK;
}

int netdimm_socket_get_errno(netdimm_net_t *net,
                             uint32_t socket_index,
                             uint32_t *socket_error)
{
    uint32_t args[1];
    args[0] = socket_index;
    return netdimm_command(net, 17u, args, 1u,
                           NETDIMM_CONTROL_WAIT_FRAMES, socket_error);
}

int netdimm_socket_close(netdimm_net_t *net,
                         uint32_t socket_index,
                         uint32_t *close_result)
{
    uint32_t args[1];
    args[0] = socket_index;
    return netdimm_command(net, 3u, args, 1u,
                           NETDIMM_CONTROL_WAIT_FRAMES, close_result);
}

const char *netdimm_net_error_string(int error)
{
    switch (error) {
        case NETDIMM_NET_OK:
            return "success";
        case NETDIMM_NET_ERR_ARGUMENT:
            return "invalid argument";
        case NETDIMM_NET_ERR_CONFIG:
            return "NetDIMM BIOS configuration is missing or unsupported";
        case NETDIMM_NET_ERR_RANGE:
            return "DIMM memory range is invalid or requested I/O is too large";
        case NETDIMM_NET_ERR_DEVICE:
            return "NetDIMM reported a command error or invalid result";
        case NETDIMM_NET_ERR_TIMEOUT:
            return "NetDIMM command timed out";
        default:
            return "unknown NetDIMM driver error";
    }
}
