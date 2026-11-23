#define _POSIX_C_SOURCE 200112L

#include "netplay_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

struct posix_udp_transport {
    int socket_fd;
};

static int posix_send_datagram(void *opaque, const uint8_t *data, size_t length)
{
    struct posix_udp_transport *transport = (struct posix_udp_transport *)opaque;
    ssize_t sent = send(transport->socket_fd, data, length, 0);
    return sent == (ssize_t)length ? 0 : -1;
}

static int posix_receive_datagram(void *opaque, uint8_t *data, size_t capacity,
                                  size_t *received, uint32_t timeout_ms)
{
    struct posix_udp_transport *transport = (struct posix_udp_transport *)opaque;
    fd_set read_set;
    struct timeval timeout;
    int rc;
    ssize_t got;

    if (data == NULL || received == NULL || capacity == 0u) {
        return -1;
    }
    *received = 0u;
    for (;;) {
        FD_ZERO(&read_set);
        FD_SET(transport->socket_fd, &read_set);
        timeout.tv_sec = (time_t)(timeout_ms / 1000u);
        timeout.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
        rc = select(transport->socket_fd + 1, &read_set, NULL, NULL, &timeout);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return -1;
    }

    got = recv(transport->socket_fd, data, capacity, 0);
    if (got < 0) {
        return -1;
    }
    *received = (size_t)got;
    return 1;
}

static void print_event(void *opaque, const char *stage, const char *detail)
{
    (void)opaque;
    printf("%s: %s\n", stage, detail);
    fflush(stdout);
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct posix_udp_transport posix_transport;
    netplay_transport_t transport;
    netplay_client_options_t options;
    netplay_client_result_t result;
    struct sockaddr_in server;
    const char *role;
    const char *room;
    const char *server_ip;
    uint16_t port = 19001u;
    int rc;

    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: %s host|join ROOM SERVER_IPV4 [PORT]\n", argv[0]);
        return 2;
    }
    role = argv[1];
    room = argv[2];
    server_ip = argv[3];

    if (strcmp(role, "host") == 0) {
        options.role = NETPLAY_ROLE_HOST;
    } else if (strcmp(role, "join") == 0) {
        options.role = NETPLAY_ROLE_JOIN;
    } else {
        fprintf(stderr, "Role must be host or join.\n");
        return 2;
    }
    if (argc == 5 && parse_port(argv[4], &port) != 0) {
        fprintf(stderr, "Invalid UDP port.\n");
        return 2;
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IPv4 address.\n");
        return 2;
    }

    posix_transport.socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (posix_transport.socket_fd < 0) {
        perror("socket");
        return 1;
    }
    if (connect(posix_transport.socket_fd, (struct sockaddr *)&server,
                sizeof(server)) != 0) {
        perror("connect");
        close(posix_transport.socket_fd);
        return 1;
    }

    memset(&transport, 0, sizeof(transport));
    transport.context = &posix_transport;
    transport.send_datagram = posix_send_datagram;
    transport.receive_datagram = posix_receive_datagram;

    options.room_code = room;
    options.match_timeout_ms = 90000u;
    options.receive_timeout_ms = 5000u;
    options.data_packet_count = 5u;

    printf("%s registering room %s at %s:%u\n",
           role, room, server_ip, (unsigned int)port);
    fflush(stdout);

    rc = netplay_client_run(&transport, &options, &result, print_event, NULL);
    if (rc == NETPLAY_CLIENT_OK) {
        printf("DATA relay: sent=%u received=%u PASS\n",
               (unsigned int)result.packets_sent,
               (unsigned int)result.packets_received);
        printf("PING/PONG: %s\n", result.ping_passed ? "PASS" : "FAIL");
        printf("NDNP C CLIENT TEST PASS (%s) session=%08X\n",
               role, (unsigned int)result.session_id);
    } else {
        fprintf(stderr, "NDNP C CLIENT TEST FAIL: %s (%s)\n",
                netplay_client_error_string(rc), result.error_message);
    }

    close(posix_transport.socket_fd);
    return rc == NETPLAY_CLIENT_OK ? 0 : 1;
}
