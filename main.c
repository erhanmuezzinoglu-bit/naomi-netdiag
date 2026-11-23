#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <naomi/video.h>
#include "netdimm_net.h"

/*
 * Fightcade Public API test.
 *
 * The Fightcade API is HTTPS. The current NetDIMM HLE socket driver has no
 * TLS layer, so this Naomi program talks plain HTTP to the small local
 * fightcade_api_bridge.py process. The bridge performs the HTTPS request.
 *
 * By default Flycast and the bridge run on the same PC, so loopback is used.
 */
#define PROXY_IP0                 127u
#define PROXY_IP1                 0u
#define PROXY_IP2                 0u
#define PROXY_IP3                 1u
#define PROXY_PORT                8765u
#define PROXY_PATH                "/cvs2"

#define HTTP_WAIT_FRAMES          NETDIMM_NET_DEFAULT_WAIT_FRAMES
#define HTTP_TIMEOUT_MS           15000u
#define HTTP_RX_CAPACITY          4096u
#define HTTP_CHUNK_CAPACITY       768u
#define MATCH_DISPLAY_COUNT       6u
#define MATCH_TEXT_CAPACITY       96u

static netdimm_net_t g_net;
static uint32_t g_socket;
static uint32_t g_socket_open;
static uint32_t g_connect_result = 0xFFFFFFFFu;
static uint32_t g_sent;
static uint32_t g_received;
static uint32_t g_close_result = 0xFFFFFFFFu;
static uint32_t g_pio_pass;
static uint32_t g_scanned;
static uint32_t g_match_count;
static uint32_t g_http_status;
static uint32_t g_api_ok;
static char g_status_line[64] = "(not received)";
static char g_stage[40] = "INITIALIZING";
static char g_detail[112] = "Reading BIOS NetDIMM configuration";
static char g_matches[MATCH_DISPLAY_COUNT][MATCH_TEXT_CAPACITY];

static void draw_page(void)
{
    unsigned int i;
    char proxy_text[64];

    snprintf(proxy_text, sizeof(proxy_text), "PROXY %u.%u.%u.%u:%u",
             (unsigned int)PROXY_IP0, (unsigned int)PROXY_IP1,
             (unsigned int)PROXY_IP2, (unsigned int)PROXY_IP3,
             (unsigned int)PROXY_PORT);

    video_fill_screen(rgb(12, 18, 28));
    video_draw_debug_text(18, 16, rgb(255, 255, 255), "FIGHTCADE PUBLIC API TEST");
    video_draw_debug_text(18, 40, rgb(170, 210, 255),
                          "BIOS:%08X DIMM:%u MiB BASE:%08X",
                          (unsigned int)g_net.bios_config,
                          (unsigned int)(g_net.dimm_size / (1024u * 1024u)),
                          (unsigned int)g_net.dimm_buffer_offset);
    video_draw_debug_text(18, 62, rgb(170, 210, 255), "%s", proxy_text);

    video_draw_debug_text(18, 92, rgb(255, 220, 120), "STAGE: %s", g_stage);
    video_draw_debug_text(18, 114, rgb(230, 230, 230), "%.82s", g_detail);
    video_draw_debug_text(18, 142, rgb(180, 220, 180), "PIO SELF-TEST: %s",
                          g_pio_pass ? "PASS" : "NOT RUN / FAILED");
    video_draw_debug_text(18, 164, rgb(180, 220, 180),
                          "HTTP:%u SENT:%u RECV:%u CLOSE:%08X",
                          (unsigned int)g_http_status,
                          (unsigned int)g_sent,
                          (unsigned int)g_received,
                          (unsigned int)g_close_result);
    video_draw_debug_text(18, 186, rgb(255, 220, 120), "%.60s", g_status_line);

    video_draw_debug_text(18, 218, rgb(180, 220, 180),
                          "API:%s SCANNED:%u CVS2 MATCHES:%u",
                          g_api_ok ? "OK" : "--",
                          (unsigned int)g_scanned,
                          (unsigned int)g_match_count);
    for (i = 0u; i < MATCH_DISPLAY_COUNT; i++) {
        if (g_matches[i][0] != '\0') {
            video_draw_debug_text(18, 246 + (int)(i * 27u),
                                  rgb(225, 230, 240), "%.90s", g_matches[i]);
        }
    }

    video_draw_debug_text(18, 430, rgb(150, 170, 190),
                          "Recent completed matches only - not live room presence");
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
        g_close_result = result;
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
                                        HTTP_TIMEOUT_MS,
                                        HTTP_TIMEOUT_MS,
                                        HTTP_TIMEOUT_MS,
                                        &result);
    if (rc != NETDIMM_NET_OK) {
        return rc;
    }
    return result == 0u ? NETDIMM_NET_OK : NETDIMM_NET_ERR_DEVICE;
}

static int find_bytes(const char *data, size_t data_length,
                      const char *needle, size_t needle_length,
                      size_t *position)
{
    size_t i;

    if (needle_length == 0u || data_length < needle_length) {
        return 0;
    }
    for (i = 0u; i <= data_length - needle_length; i++) {
        if (memcmp(data + i, needle, needle_length) == 0) {
            if (position != NULL) {
                *position = i;
            }
            return 1;
        }
    }
    return 0;
}

static int parse_content_length(const char *headers, size_t header_length,
                                uint32_t *content_length)
{
    static const char key[] = "Content-Length:";
    size_t pos;

    for (pos = 0u; pos + sizeof(key) - 1u <= header_length; pos++) {
        if (memcmp(headers + pos, key, sizeof(key) - 1u) == 0) {
            const char *p = headers + pos + sizeof(key) - 1u;
            uint32_t value = 0u;
            int digits = 0;

            while ((size_t)(p - headers) < header_length && (*p == ' ' || *p == '\t')) {
                p++;
            }
            while ((size_t)(p - headers) < header_length && *p >= '0' && *p <= '9') {
                value = value * 10u + (uint32_t)(*p - '0');
                digits = 1;
                p++;
            }
            if (!digits) {
                return 0;
            }
            *content_length = value;
            return 1;
        }
    }
    return 0;
}

static void copy_status_line(const char *response, size_t length)
{
    size_t i;
    size_t out = 0u;

    memset(g_status_line, 0, sizeof(g_status_line));
    for (i = 0u; i < length && out + 1u < sizeof(g_status_line); i++) {
        char ch = response[i];
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((unsigned char)ch < 32u || (unsigned char)ch > 126u) {
            ch = '.';
        }
        g_status_line[out++] = ch;
    }
    if (out == 0u) {
        strncpy(g_status_line, "(no HTTP status line)", sizeof(g_status_line) - 1u);
    }
    g_status_line[sizeof(g_status_line) - 1u] = '\0';
}

static int receive_http_response(char *response, size_t capacity,
                                 size_t *response_length, size_t *body_offset,
                                 size_t *body_length)
{
    char chunk[HTTP_CHUNK_CAPACITY];
    size_t total = 0u;
    size_t header_end = 0u;
    uint32_t content_length = 0u;
    int have_content_length = 0;

    if (response == NULL || response_length == NULL ||
        body_offset == NULL || body_length == NULL || capacity < 32u) {
        return -1;
    }

    while (total + 1u < capacity) {
        uint32_t got = 0u;
        int rc = netdimm_socket_recv(&g_net, g_socket, chunk,
                                     sizeof(chunk), HTTP_WAIT_FRAMES, &got);
        if (rc != NETDIMM_NET_OK) {
            return -2;
        }
        if (got == 0u) {
            break;
        }
        if ((size_t)got > capacity - total - 1u) {
            return -3;
        }
        memcpy(response + total, chunk, got);
        total += got;
        response[total] = '\0';

        if (header_end == 0u) {
            size_t marker = 0u;
            if (find_bytes(response, total, "\r\n\r\n", 4u, &marker)) {
                header_end = marker + 4u;
                have_content_length = parse_content_length(response, marker,
                                                           &content_length);
                if (!have_content_length || content_length > capacity - header_end - 1u) {
                    return -4;
                }
            }
        }

        if (header_end != 0u && have_content_length &&
            total >= header_end + (size_t)content_length) {
            break;
        }
    }

    response[total] = '\0';
    *response_length = total;
    if (header_end == 0u || !have_content_length ||
        total < header_end + (size_t)content_length) {
        return -5;
    }
    *body_offset = header_end;
    *body_length = (size_t)content_length;
    return 0;
}

static char *next_field(char **cursor)
{
    char *field = *cursor;
    char *separator = strchr(field, '|');

    if (separator != NULL) {
        *separator = '\0';
        *cursor = separator + 1;
    } else {
        *cursor = field + strlen(field);
    }
    return field;
}

static char *next_line(char **cursor)
{
    char *line;
    char *p;

    if (cursor == NULL || *cursor == NULL || **cursor == '\0') {
        return NULL;
    }

    line = *cursor;
    p = line;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
        while (*p == '\r' || *p == '\n') {
            p++;
        }
    }
    *cursor = (*p != '\0') ? p : NULL;
    return line;
}

static void parse_proxy_body(const char *body_data, size_t body_length)
{
    char body[HTTP_RX_CAPACITY];
    char *cursor;
    char *line;

    g_api_ok = 0u;
    g_scanned = 0u;
    g_match_count = 0u;
    memset(g_matches, 0, sizeof(g_matches));

    if (body_length >= sizeof(body)) {
        return;
    }
    memcpy(body, body_data, body_length);
    body[body_length] = '\0';
    cursor = body;

    while ((line = next_line(&cursor)) != NULL) {
        if (strncmp(line, "STATUS=OK", 9u) == 0) {
            g_api_ok = 1u;
        } else if (strncmp(line, "SCANNED=", 8u) == 0) {
            g_scanned = (uint32_t)strtoul(line + 8u, NULL, 10);
        } else if (strncmp(line, "M|", 2u) == 0 &&
                   g_match_count < MATCH_DISPLAY_COUNT) {
            char *field_cursor = line + 2u;
            char *date = next_field(&field_cursor);
            char *duration = next_field(&field_cursor);
            char *player1 = next_field(&field_cursor);
            char *player2 = field_cursor;

            snprintf(g_matches[g_match_count], MATCH_TEXT_CAPACITY,
                     "%s %s %s vs %s", date, duration, player1, player2);
            g_match_count++;
        }
    }
}

int main(void)
{
    static const uint8_t proxy_ip[4] = {
        PROXY_IP0, PROXY_IP1, PROXY_IP2, PROXY_IP3
    };
    static const char http_request[] =
        "GET " PROXY_PATH " HTTP/1.0\r\n"
        "Host: 127.0.0.1:8765\r\n"
        "Accept: text/plain\r\n"
        "Connection: close\r\n"
        "\r\n";

    char response[HTTP_RX_CAPACITY];
    size_t response_length = 0u;
    size_t body_offset = 0u;
    size_t body_length = 0u;
    int rc;

    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(12, 18, 28));

    rc = netdimm_net_init(&g_net);
    if (rc != NETDIMM_NET_OK) {
        fail_forever("NETDIMM INIT FAILED", netdimm_net_error_string(rc));
    }
    netdimm_net_set_progress_callback(&g_net, progress_callback, NULL);

    set_stage("PIO SELF-TEST", "Checking DIMM PIO read/write path");
    rc = netdimm_net_pio_self_test(&g_net, 0x46434150u);
    if (rc != NETDIMM_NET_OK) {
        fail_forever("PIO SELF-TEST FAILED", netdimm_net_error_string(rc));
    }
    g_pio_pass = 1u;
    draw_page();

    set_stage("TCP OPEN", "Opening a TCP connection to the local API bridge");
    rc = netdimm_socket_open(&g_net, 2u, 1u, 6u, &g_socket);
    if (rc != NETDIMM_NET_OK || g_socket == 0u || g_socket == 0xFFFFFFFFu) {
        fail_driver("TCP OPEN FAILED", "socket", rc);
    }
    g_socket_open = 1u;

    set_stage("TCP TIMEOUT", "Setting 15 second socket timeouts");
    rc = configure_socket();
    if (rc != NETDIMM_NET_OK) {
        fail_driver("TCP TIMEOUT FAILED", "settimeout", rc);
    }

    set_stage("TCP CONNECT", "Connecting to 127.0.0.1:8765");
    rc = netdimm_socket_connect_ipv4(&g_net, g_socket, proxy_ip,
                                    (uint16_t)PROXY_PORT,
                                    HTTP_WAIT_FRAMES, &g_connect_result);
    if (rc != NETDIMM_NET_OK || g_connect_result != 0u) {
        fail_driver("PROXY CONNECT FAILED", "connect", rc);
    }

    set_stage("HTTP REQUEST", "Requesting recent CvS2 matches from the local bridge");
    rc = netdimm_socket_send(&g_net, g_socket, http_request,
                             (uint32_t)(sizeof(http_request) - 1u),
                             HTTP_WAIT_FRAMES, &g_sent);
    if (rc != NETDIMM_NET_OK || g_sent != sizeof(http_request) - 1u) {
        fail_driver("HTTP SEND FAILED", "send", rc);
    }

    set_stage("HTTP RECEIVE", "Waiting for the Fightcade API bridge response");
    memset(response, 0, sizeof(response));
    rc = receive_http_response(response, sizeof(response),
                               &response_length, &body_offset, &body_length);
    if (rc != 0) {
        char detail[112];
        snprintf(detail, sizeof(detail), "HTTP receive/parser error %d; bytes=%u",
                 rc, (unsigned int)response_length);
        fail_forever("HTTP RESPONSE FAILED", detail);
    }
    g_received = (uint32_t)response_length;
    copy_status_line(response, response_length);

    if (strncmp(response, "HTTP/1.0 200", 12u) != 0 &&
        strncmp(response, "HTTP/1.1 200", 12u) != 0) {
        close_socket();
        fail_forever("PROXY HTTP ERROR", g_status_line);
    }
    g_http_status = 200u;

    parse_proxy_body(response + body_offset, body_length);
    if (g_api_ok == 0u) {
        close_socket();
        fail_forever("API RESPONSE INVALID", "Bridge did not return STATUS=OK");
    }

    set_stage("TCP CLOSE", "Closing the local HTTP connection");
    rc = netdimm_socket_close(&g_net, g_socket, &g_close_result);
    g_socket_open = 0u;
    if (rc != NETDIMM_NET_OK || g_close_result != 0u) {
        fail_forever("TCP CLOSE FAILED", "API response received, but socket close failed");
    }

    if (g_match_count != 0u) {
        char detail[112];
        snprintf(detail, sizeof(detail),
                 "Fightcade API OK; %u recent CvS2 replay(s) in %u global records",
                 (unsigned int)g_match_count, (unsigned int)g_scanned);
        set_stage("FIGHTCADE API TEST PASSED", detail);
    } else {
        char detail[112];
        snprintf(detail, sizeof(detail),
                 "API OK; no CvS2 replay in latest %u global records",
                 (unsigned int)g_scanned);
        set_stage("API OK / NO CVS2 MATCH", detail);
    }

    for (;;) {
        draw_page();
    }

    return 0;
}
