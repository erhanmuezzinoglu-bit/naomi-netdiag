#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <naomi/video.h>

#include "netdimm_net.h"
#include "netplay_protocol.h"
#include "netplay_client.h"
#include "netplay_netdimm.h"
#include "netplay_config.h"

#ifndef NETPLAY_DEFAULT_ROLE
#error NETPLAY_DEFAULT_ROLE must be NETPLAY_ROLE_HOST or NETPLAY_ROLE_JOIN
#endif

#if NETPLAY_DEFAULT_ROLE != 1 && NETPLAY_DEFAULT_ROLE != 2
#error Invalid NETPLAY_DEFAULT_ROLE
#endif

static netdimm_net_t g_net;
static netplay_netdimm_t g_adapter;
static netplay_transport_t g_transport;
static char g_stage[40] = "INITIALIZING";
static char g_detail[112] = "Reading BIOS NetDIMM configuration";
static uint32_t g_session_id;
static uint32_t g_packets_sent;
static uint32_t g_packets_received;
static uint32_t g_bytes_sent;
static uint32_t g_bytes_received;
static uint32_t g_relay_open;

static void draw_page(void)
{
    video_fill_screen(rgb(12, 18, 28));

    video_draw_debug_text(18, 16, rgb(255, 255, 255),
                          "NAOMI NETPLAY NDNP V1 TEST");
    video_draw_debug_text(18, 44, rgb(170, 210, 255),
                          "ROLE: %s  ROOM: %s",
                          NETPLAY_DEFAULT_ROLE == NETPLAY_ROLE_HOST ? "HOST" : "JOIN",
                          NETPLAY_ROOM_CODE);
    video_draw_debug_text(18, 72, rgb(170, 210, 255),
                          "RELAY %u.%u.%u.%u:%u UDP",
                          NETPLAY_RELAY_IP0, NETPLAY_RELAY_IP1,
                          NETPLAY_RELAY_IP2, NETPLAY_RELAY_IP3,
                          NETPLAY_RELAY_PORT);
    video_draw_debug_text(18, 108, rgb(255, 220, 120), "STAGE: %s", g_stage);
    video_draw_debug_text(18, 136, rgb(230, 230, 230), "%.90s", g_detail);
    video_draw_debug_text(18, 188, rgb(180, 220, 180),
                          "SESSION: %08X  RELAY SOCKET: %s",
                          (unsigned int)g_session_id,
                          g_relay_open != 0u ? "OPEN" : "CLOSED");
    video_draw_debug_text(18, 218, rgb(180, 220, 180),
                          "DATA SENT:%u RECEIVED:%u",
                          (unsigned int)g_packets_sent,
                          (unsigned int)g_packets_received);
    video_draw_debug_text(18, 244, rgb(180, 220, 180),
                          "BYTES SENT:%u RECEIVED:%u",
                          (unsigned int)g_bytes_sent,
                          (unsigned int)g_bytes_received);
    video_draw_debug_text(18, 430, rgb(150, 170, 190),
                          "Run the NDNP relay server on the configured Linux host.");
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
    strncpy(g_detail, detail != NULL ? detail : "", sizeof(g_detail) - 1u);
    g_detail[sizeof(g_detail) - 1u] = '\0';
    draw_page();
}

static void client_event(void *opaque, const char *stage, const char *detail)
{
    (void)opaque;
    set_stage(stage, detail);
}

static void fail_forever(const char *stage, const char *detail)
{
    if (g_relay_open != 0u) {
        (void)netplay_netdimm_close(&g_adapter);
        g_relay_open = 0u;
    }
    set_stage(stage, detail);
    for (;;) {
        draw_page();
    }
}

int main(void)
{
    static const uint8_t relay_ip[4] = {
        NETPLAY_RELAY_IP0, NETPLAY_RELAY_IP1,
        NETPLAY_RELAY_IP2, NETPLAY_RELAY_IP3
    };
    netplay_client_options_t options;
    netplay_client_result_t result;
    int rc;
    int close_rc;

    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(12, 18, 28));
    draw_page();

    rc = netdimm_net_init(&g_net);
    if (rc != NETDIMM_NET_OK) {
        fail_forever("NETDIMM INIT FAILED", netdimm_net_error_string(rc));
    }
    netdimm_net_set_progress_callback(&g_net, progress_callback, NULL);

    set_stage("PIO SELF-TEST", "Checking DIMM PIO read/write path");
    rc = netdimm_net_pio_self_test(&g_net, 0x4E44504Eu);
    if (rc != NETDIMM_NET_OK) {
        fail_forever("PIO SELF-TEST FAILED", netdimm_net_error_string(rc));
    }

    set_stage("RELAY SOCKET", "Opening connected UDP socket to relay");
    rc = netplay_netdimm_open(&g_adapter, &g_net, relay_ip,
                              (uint16_t)NETPLAY_RELAY_PORT,
                              NETPLAY_SOCKET_OPERATION_TIMEOUT_MS,
                              NETPLAY_COMMAND_WAIT_FRAMES,
                              &g_transport);
    if (rc != NETPLAY_NETDIMM_OK) {
        char detail[112];
        snprintf(detail, sizeof(detail),
                 "adapter rc=%d driver=%d socket errno=%u",
                 rc,
                 netplay_netdimm_last_driver_error(&g_adapter),
                 (unsigned int)netplay_netdimm_last_socket_error(&g_adapter));
        fail_forever("RELAY CONNECT FAILED", detail);
    }
    g_relay_open = 1u;

    memset(&options, 0, sizeof(options));
    options.role = NETPLAY_DEFAULT_ROLE;
    options.room_code = NETPLAY_ROOM_CODE;
    options.match_timeout_ms = NETPLAY_MATCH_TIMEOUT_MS;
    options.receive_timeout_ms = NETPLAY_RECEIVE_TIMEOUT_MS;
    options.data_packet_count = NETPLAY_DATA_PACKET_COUNT;

    memset(&result, 0, sizeof(result));
    rc = netplay_client_run(&g_transport, &options, &result,
                            client_event, NULL);

    g_session_id = result.session_id;
    g_packets_sent = result.packets_sent;
    g_packets_received = result.packets_received;
    g_bytes_sent = result.bytes_sent;
    g_bytes_received = result.bytes_received;

    set_stage("RELAY SOCKET CLOSE", "Closing NetDIMM UDP socket");
    close_rc = netplay_netdimm_close(&g_adapter);
    g_relay_open = 0u;

    if (rc != NETPLAY_CLIENT_OK) {
        char detail[112];
        snprintf(detail, sizeof(detail), "%s (%d)",
                 result.error_message[0] != '\0'
                     ? result.error_message
                     : netplay_client_error_string(rc), rc);
        fail_forever("NETPLAY TEST FAILED", detail);
    }
    if (close_rc != NETPLAY_NETDIMM_OK) {
        char detail[112];
        snprintf(detail, sizeof(detail), "NetDIMM socket close failed: %d", close_rc);
        fail_forever("SOCKET CLOSE FAILED", detail);
    }

    set_stage("NETPLAY TEST PASSED",
              NETPLAY_DEFAULT_ROLE == NETPLAY_ROLE_HOST
                  ? "HOST completed NDNP v1 relay smoke test"
                  : "JOIN completed NDNP v1 relay smoke test");
    for (;;) {
        draw_page();
    }

    return 0;
}
