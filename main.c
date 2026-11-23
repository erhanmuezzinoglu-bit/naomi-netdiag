#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <naomi/video.h>
#include "netdimm_net.h"
#include "netdiag_config.h"

#define UDP_AF_INET       2u
#define UDP_SOCK_DGRAM    2u
#define UDP_IPPROTO_UDP   17u

#define ECHO_PACKET_SIZE  32u
#define ECHO_RX_CAPACITY  64u
#define ECHO_WAIT_FRAMES  NETDIAG_ECHO_WAIT_FRAMES

static netdimm_net_t g_net;
static uint32_t g_socket;
static uint32_t g_socket_open;
static uint32_t g_sent_packets;
static uint32_t g_received_packets;
static uint32_t g_sent_bytes;
static uint32_t g_received_bytes;
static uint32_t g_last_send_result = 0xFFFFFFFFu;
static uint32_t g_last_recv_result = 0xFFFFFFFFu;
static char g_stage[40] = "INITIALIZING";
static char g_detail[112] = "Reading BIOS NetDIMM configuration";

static void draw_page(void)
{
    video_fill_screen(rgb(12, 18, 28));
    video_draw_debug_text(18, 16, rgb(255, 255, 255), "NAOMI NETDIMM UDP ECHO TEST");
    video_draw_debug_text(18, 44, rgb(170, 210, 255),
                          "BIOS:%08X DIMM:%u MiB BASE:%08X",
                          (unsigned int)g_net.bios_config,
                          (unsigned int)(g_net.dimm_size / (1024u * 1024u)),
                          (unsigned int)g_net.dimm_buffer_offset);
    video_draw_debug_text(18, 72, rgb(170, 210, 255),
                          "SERVER %u.%u.%u.%u:%u UDP",
                          NETDIAG_SERVER_IP0, NETDIAG_SERVER_IP1,
                          NETDIAG_SERVER_IP2, NETDIAG_SERVER_IP3,
                          NETDIAG_UDP_PORT);
    video_draw_debug_text(18, 112, rgb(255, 220, 120), "STAGE: %s", g_stage);
    video_draw_debug_text(18, 138, rgb(230, 230, 230), "%.90s", g_detail);
    video_draw_debug_text(18, 184, rgb(180, 220, 180),
                          "PACKETS SENT:%u  RECEIVED:%u",
                          (unsigned int)g_sent_packets,
                          (unsigned int)g_received_packets);
    video_draw_debug_text(18, 210, rgb(180, 220, 180),
                          "BYTES SENT:%u  RECEIVED:%u",
                          (unsigned int)g_sent_bytes,
                          (unsigned int)g_received_bytes);
    video_draw_debug_text(18, 252, rgb(200, 210, 225),
                          "LAST SEND:%08X  LAST RECV:%08X",
                          (unsigned int)g_last_send_result,
                          (unsigned int)g_last_recv_result);
    video_draw_debug_text(18, 430, rgb(150, 170, 190),
                          "The server must be running on the configured LAN address.");
    video_draw_debug_text(18, 452, rgb(150, 170, 190),
                          "DIMM CMD:%04X STAT:%04X RESULT:%08X",
                          (unsigned int)g_net.last_command,
                          (unsigned int)g_net.last_status,
                          (unsigned int)g_net.last_result);
    video_display_on_vblank();
}

static void progress_callback(void *opaque)
{
    (void)opaque;
    draw_page();
}

static void set_stage(const char *stage, const char *detail)
{
    strncpy(g_stage, stage, sizeof(g_stage) - 1u);
    g_stage[sizeof(g_stage) - 1u] = '\0';
    strncpy(g_detail, detail, sizeof(g_detail) - 1u);
    g_detail[sizeof(g_detail) - 1u] = '\0';
    draw_page();
}

static void close_socket(void)
{
    if (g_socket_open != 0u) {
        uint32_t result = 0xFFFFFFFFu;
        (void)netdimm_socket_close(&g_net, g_socket, &result);
        g_socket_open = 0u;
    }
}

static void fail_forever(const char *stage, const char *detail)
{
    close_socket();
    set_stage(stage, detail);
    for (;;) {
        draw_page();
    }
}

static void fail_driver(const char *stage, const char *operation, int error)
{
    uint32_t socket_error = 0xFFFFFFFFu;
    char detail[112];

    if (g_socket_open != 0u) {
        if (netdimm_socket_get_errno(&g_net, g_socket, &socket_error) != NETDIMM_NET_OK) {
            socket_error = g_net.last_result;
        }
    }
    snprintf(detail, sizeof(detail), "%s: %s rc=%d errno=%u",
             operation, netdimm_net_error_string(error), error,
             (unsigned int)socket_error);
    fail_forever(stage, detail);
}

static int configure_socket(void)
{
    uint32_t result = 0xFFFFFFFFu;
    int rc = netdimm_socket_set_timeout(&g_net, g_socket,
                                        NETDIAG_SOCKET_TIMEOUT_MS,
                                        NETDIAG_SOCKET_TIMEOUT_MS,
                                        NETDIAG_SOCKET_TIMEOUT_MS,
                                        &result);
    if (rc != NETDIMM_NET_OK) {
        return rc;
    }
    return result == 0u ? NETDIMM_NET_OK : NETDIMM_NET_ERR_DEVICE;
}

static void build_echo_packet(uint32_t sequence, uint8_t packet[ECHO_PACKET_SIZE])
{
    size_t i;

    packet[0] = 'N';
    packet[1] = 'D';
    packet[2] = 'G';
    packet[3] = '1';
    packet[4] = (uint8_t)(sequence >> 24);
    packet[5] = (uint8_t)(sequence >> 16);
    packet[6] = (uint8_t)(sequence >> 8);
    packet[7] = (uint8_t)sequence;

    for (i = 8u; i < ECHO_PACKET_SIZE; i++) {
        packet[i] = (uint8_t)(0xA5u ^ sequence ^ (uint32_t)(i * 17u));
    }
}

int main(void)
{
    static const uint8_t server_ip[4] = {
        NETDIAG_SERVER_IP0, NETDIAG_SERVER_IP1,
        NETDIAG_SERVER_IP2, NETDIAG_SERVER_IP3
    };
    uint8_t tx[ECHO_PACKET_SIZE];
    uint8_t rx[ECHO_RX_CAPACITY];
    uint32_t command_result = 0xFFFFFFFFu;
    uint32_t received = 0u;
    uint32_t sequence;
    int rc;

    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(12, 18, 28));

    rc = netdimm_net_init(&g_net);
    if (rc != NETDIMM_NET_OK) {
        fail_forever("NETDIMM INIT FAILED", netdimm_net_error_string(rc));
    }
    netdimm_net_set_progress_callback(&g_net, progress_callback, NULL);

    set_stage("PIO SELF-TEST", "Checking DIMM PIO read/write path");
    rc = netdimm_net_pio_self_test(&g_net, 0x4E444731u);
    if (rc != NETDIMM_NET_OK) {
        fail_forever("PIO SELF-TEST FAILED", netdimm_net_error_string(rc));
    }

    set_stage("UDP SOCKET OPEN", "Creating an IPv4 datagram socket");
    rc = netdimm_socket_open(&g_net, UDP_AF_INET, UDP_SOCK_DGRAM,
                             UDP_IPPROTO_UDP, &g_socket);
    if (rc != NETDIMM_NET_OK || g_socket == 0u || g_socket == 0xFFFFFFFFu) {
        fail_driver("UDP SOCKET OPEN FAILED", "socket", rc);
    }
    g_socket_open = 1u;

    set_stage("UDP TIMEOUT", "Setting socket operation timeouts");
    rc = configure_socket();
    if (rc != NETDIMM_NET_OK) {
        fail_driver("UDP TIMEOUT FAILED", "settimeout", rc);
    }

    set_stage("UDP CONNECT", "Setting the remote UDP endpoint");
    rc = netdimm_socket_connect_ipv4(&g_net, g_socket, server_ip,
                                    (uint16_t)NETDIAG_UDP_PORT,
                                    ECHO_WAIT_FRAMES, &command_result);
    if (rc != NETDIMM_NET_OK || command_result != 0u) {
        fail_driver("UDP CONNECT FAILED", "connect", rc);
    }

    for (sequence = 1u; sequence <= NETDIAG_ECHO_ROUNDS; sequence++) {
        char detail[112];

        build_echo_packet(sequence, tx);
        snprintf(detail, sizeof(detail), "Sending test datagram %u of %u",
                 (unsigned int)sequence, (unsigned int)NETDIAG_ECHO_ROUNDS);
        set_stage("UDP SEND", detail);

        g_last_send_result = 0xFFFFFFFFu;
        rc = netdimm_socket_send(&g_net, g_socket, tx, sizeof(tx),
                                 ECHO_WAIT_FRAMES, &g_last_send_result);
        if (rc != NETDIMM_NET_OK || g_last_send_result != sizeof(tx)) {
            fail_driver("UDP SEND FAILED", "send", rc);
        }
        g_sent_packets++;
        g_sent_bytes += g_last_send_result;

        snprintf(detail, sizeof(detail), "Waiting for datagram %u echo",
                 (unsigned int)sequence);
        set_stage("UDP RECEIVE", detail);

        memset(rx, 0, sizeof(rx));
        received = 0u;
        g_last_recv_result = 0xFFFFFFFFu;
        rc = netdimm_socket_recv(&g_net, g_socket, rx, sizeof(rx),
                                 ECHO_WAIT_FRAMES, &received);
        g_last_recv_result = received;
        if (rc != NETDIMM_NET_OK) {
            fail_driver("UDP RECEIVE FAILED", "recv", rc);
        }
        if (received != sizeof(tx) || memcmp(tx, rx, sizeof(tx)) != 0) {
            snprintf(detail, sizeof(detail),
                     "Echo mismatch on packet %u: sent=%u received=%u",
                     (unsigned int)sequence, (unsigned int)sizeof(tx),
                     (unsigned int)received);
            fail_forever("UDP VERIFY FAILED", detail);
        }

        g_received_packets++;
        g_received_bytes += received;
    }

    set_stage("UDP CLOSE", "All echo packets matched; closing socket");
    rc = netdimm_socket_close(&g_net, g_socket, &command_result);
    g_socket_open = 0u;
    if (rc != NETDIMM_NET_OK || command_result != 0u) {
        fail_forever("UDP CLOSE FAILED", "Echo test passed but socket close failed");
    }

    set_stage("UDP ECHO PASSED", "All test datagrams returned byte-for-byte");
    for (;;) {
        draw_page();
    }

    return 0;
}
