#include "netplay_client.h"

#include <stdio.h>
#include <string.h>

#define NDNP_RX_BUFFER_SIZE NETPLAY_MAX_DATAGRAM
#define NDNP_TX_BUFFER_SIZE NETPLAY_MAX_DATAGRAM

struct client_context {
    const netplay_transport_t *transport;
    netplay_client_result_t *result;
    netplay_client_event_fn event_fn;
    void *event_opaque;
    uint32_t next_sequence;
    uint8_t peer_done_seen;
};

static void notify(struct client_context *ctx, const char *stage, const char *detail)
{
    if (ctx->event_fn != NULL) {
        ctx->event_fn(ctx->event_opaque, stage, detail != NULL ? detail : "");
    }
}

static int fail(struct client_context *ctx, int code, const char *message)
{
    ctx->result->error_code = code;
    if (message == NULL) {
        message = netplay_client_error_string(code);
    }
    (void)snprintf(ctx->result->error_message,
                   sizeof(ctx->result->error_message), "%s", message);
    notify(ctx, "FAILED", ctx->result->error_message);
    return code;
}

static int send_packet(struct client_context *ctx, uint8_t type,
                       uint32_t session_id, const void *payload,
                       uint16_t payload_length, uint32_t *sent_sequence)
{
    uint8_t datagram[NDNP_TX_BUFFER_SIZE];
    size_t datagram_length = 0u;
    uint32_t sequence = ctx->next_sequence++;
    int rc;

    rc = netplay_packet_encode(datagram, sizeof(datagram), type,
                               session_id, sequence, payload,
                               payload_length, &datagram_length);
    if (rc != NETPLAY_PROTOCOL_OK) {
        return fail(ctx, NETPLAY_CLIENT_ERR_PROTOCOL,
                    netplay_protocol_error_string(rc));
    }

    rc = ctx->transport->send_datagram(ctx->transport->context,
                                       datagram, datagram_length);
    if (rc != 0) {
        return fail(ctx, NETPLAY_CLIENT_ERR_TRANSPORT,
                    "UDP datagram send failed");
    }

    if (sent_sequence != NULL) {
        *sent_sequence = sequence;
    }
    return NETPLAY_CLIENT_OK;
}

/* Returns 1 for a decoded packet, 0 for timeout, or a negative client error. */
static int receive_packet(struct client_context *ctx,
                          uint32_t timeout_ms,
                          uint8_t *storage,
                          netplay_packet_t *packet)
{
    size_t received = 0u;
    int rc;

    rc = ctx->transport->receive_datagram(ctx->transport->context,
                                          storage, NDNP_RX_BUFFER_SIZE,
                                          &received, timeout_ms);
    if (rc == 0) {
        return 0;
    }
    if (rc < 0) {
        return fail(ctx, NETPLAY_CLIENT_ERR_TRANSPORT,
                    "UDP datagram receive failed");
    }

    rc = netplay_packet_decode(storage, received, packet);
    if (rc != NETPLAY_PROTOCOL_OK) {
        return fail(ctx, NETPLAY_CLIENT_ERR_PROTOCOL,
                    netplay_protocol_error_string(rc));
    }
    return 1;
}

static int relay_error(struct client_context *ctx, const netplay_packet_t *packet)
{
    char message[96];
    size_t length = packet->payload_length;

    if (length >= sizeof(message)) {
        length = sizeof(message) - 1u;
    }
    if (length != 0u) {
        memcpy(message, packet->payload, length);
    }
    message[length] = '\0';
    return fail(ctx, NETPLAY_CLIENT_ERR_RELAY, message[0] != '\0' ? message : "relay returned ERROR");
}

static int register_room(struct client_context *ctx,
                         uint8_t role,
                         const char *room,
                         size_t room_length)
{
    uint8_t message_type = role == NETPLAY_ROLE_HOST ? NETPLAY_MSG_HOST : NETPLAY_MSG_JOIN;
    return send_packet(ctx, message_type, 0u, room, (uint16_t)room_length, NULL);
}

static int wait_for_match(struct client_context *ctx,
                          const netplay_client_options_t *options,
                          const char *room,
                          size_t room_length,
                          uint32_t match_timeout_ms,
                          uint32_t receive_timeout_ms)
{
    uint8_t storage[NDNP_RX_BUFFER_SIZE];
    uint8_t role_message;
    uint32_t elapsed = 0u;
    uint8_t expected_role = options->role;
    int rc;

    rc = register_room(ctx, options->role, room, room_length);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }

    notify(ctx, "REGISTERED", options->role == NETPLAY_ROLE_HOST
           ? "Waiting for JOIN client" : "Waiting for HOST client");

    while (elapsed < match_timeout_ms) {
        netplay_packet_t packet;
        uint32_t remaining = match_timeout_ms - elapsed;
        uint32_t slice = receive_timeout_ms;
        int got;

        if (slice > remaining) {
            slice = remaining;
        }

        got = receive_packet(ctx, slice, storage, &packet);
        if (got < 0) {
            return got;
        }
        if (got == 0) {
            elapsed += slice;
            if (elapsed >= match_timeout_ms) {
                break;
            }
            /* Refresh the pending-room lease, as the Python reference does. */
            rc = register_room(ctx, options->role, room, room_length);
            if (rc != NETPLAY_CLIENT_OK) {
                return rc;
            }
            continue;
        }

        if (packet.type == NETPLAY_MSG_WAITING) {
            notify(ctx, "WAITING", "Room registered; waiting for the other player");
            continue;
        }
        if (packet.type == NETPLAY_MSG_ERROR) {
            return relay_error(ctx, &packet);
        }
        if (packet.type != NETPLAY_MSG_MATCH) {
            return fail(ctx, NETPLAY_CLIENT_ERR_PROTOCOL,
                        "unexpected relay message before MATCH");
        }
        if (packet.session_id == 0u || packet.payload_length != 1u) {
            return fail(ctx, NETPLAY_CLIENT_ERR_PROTOCOL,
                        "invalid MATCH session or role payload");
        }

        role_message = packet.payload[0];
        if (role_message != expected_role) {
            return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                        "MATCH role does not match this client");
        }

        ctx->result->session_id = packet.session_id;
        ctx->result->matched = 1u;
        notify(ctx, "MATCHED", "HOST and JOIN have been paired");
        return NETPLAY_CLIENT_OK;
    }

    return fail(ctx, NETPLAY_CLIENT_ERR_TIMEOUT,
                "timed out waiting for the other player");
}

static int send_test_data(struct client_context *ctx, uint8_t role,
                          uint32_t packet_count)
{
    uint32_t i;
    char payload[32];
    const char *role_name = role == NETPLAY_ROLE_HOST ? "HOST" : "JOIN";

    for (i = 1u; i <= packet_count; i++) {
        int length = snprintf(payload, sizeof(payload), "NDTEST|%s|%04u",
                              role_name, (unsigned int)i);
        int rc;

        if (length < 0 || (size_t)length >= sizeof(payload)) {
            return fail(ctx, NETPLAY_CLIENT_ERR_ARGUMENT,
                        "test payload formatting failed");
        }
        rc = send_packet(ctx, NETPLAY_MSG_DATA, ctx->result->session_id,
                         payload, (uint16_t)length, NULL);
        if (rc != NETPLAY_CLIENT_OK) {
            return rc;
        }
        ctx->result->packets_sent++;
        ctx->result->bytes_sent += (uint32_t)length;
    }
    return NETPLAY_CLIENT_OK;
}

static int receive_test_data(struct client_context *ctx, uint8_t role,
                             uint32_t packet_count, uint32_t receive_timeout_ms)
{
    uint8_t storage[NDNP_RX_BUFFER_SIZE];
    uint32_t received_mask = 0u;
    uint32_t received_count = 0u;
    char expected_prefix[20];
    int prefix_length;

    prefix_length = snprintf(expected_prefix, sizeof(expected_prefix),
                              "NDTEST|%s|",
                              role == NETPLAY_ROLE_HOST ? "JOIN" : "HOST");
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(expected_prefix)) {
        return fail(ctx, NETPLAY_CLIENT_ERR_ARGUMENT,
                    "peer prefix formatting failed");
    }

    notify(ctx, "DATA", "Waiting for peer datagrams");
    while (received_count < packet_count) {
        netplay_packet_t packet;
        int got = receive_packet(ctx, receive_timeout_ms, storage, &packet);

        if (got == 0) {
            return fail(ctx, NETPLAY_CLIENT_ERR_TIMEOUT,
                        "timed out waiting for peer DATA packets");
        }
        if (got < 0) {
            return got;
        }
        if (packet.type == NETPLAY_MSG_ERROR) {
            return relay_error(ctx, &packet);
        }
        if (packet.type != NETPLAY_MSG_DATA ||
            packet.session_id != ctx->result->session_id) {
            return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                        "unexpected message or session while receiving DATA");
        }
        if (packet.payload_length != (uint16_t)((size_t)prefix_length + 4u) ||
            memcmp(packet.payload, expected_prefix, (size_t)prefix_length) != 0) {
            return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                        "peer DATA payload does not match expected format");
        }

        {
            const uint8_t *digits = packet.payload + prefix_length;
            uint32_t index;
            if (digits[0] < '0' || digits[0] > '9' ||
                digits[1] < '0' || digits[1] > '9' ||
                digits[2] < '0' || digits[2] > '9' ||
                digits[3] < '0' || digits[3] > '9') {
                return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                            "peer DATA sequence is not numeric");
            }
            index = (uint32_t)(digits[0] - '0') * 1000u +
                    (uint32_t)(digits[1] - '0') * 100u +
                    (uint32_t)(digits[2] - '0') * 10u +
                    (uint32_t)(digits[3] - '0');
            if (index == 0u || index > packet_count) {
                return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                            "peer DATA index is outside expected range");
            }
            if ((received_mask & (1u << (index - 1u))) == 0u) {
                received_mask |= 1u << (index - 1u);
                received_count++;
                ctx->result->packets_received++;
                ctx->result->bytes_received += packet.payload_length;
            }
        }
    }

    notify(ctx, "DATA PASSED", "All peer datagrams received");
    return NETPLAY_CLIENT_OK;
}

static int is_peer_done(const netplay_packet_t *packet, uint8_t role,
                        uint32_t session_id)
{
    const char *peer_name = role == NETPLAY_ROLE_HOST ? "JOIN" : "HOST";
    char expected[16];
    int expected_length;

    if (packet == NULL || packet->type != NETPLAY_MSG_DATA ||
        packet->session_id != session_id) {
        return 0;
    }

    expected_length = snprintf(expected, sizeof(expected), "DONE|%s", peer_name);
    return expected_length > 0 &&
           (size_t)expected_length == packet->payload_length &&
           memcmp(packet->payload, expected, (size_t)expected_length) == 0;
}

static int ping_peer_service(struct client_context *ctx,
                             uint8_t role,
                             uint32_t receive_timeout_ms)
{
    uint8_t storage[NDNP_RX_BUFFER_SIZE];
    char payload[20];
    int payload_length;
    uint32_t sequence = 0u;
    int rc;

    payload_length = snprintf(payload, sizeof(payload), "PING|%s",
                              role == NETPLAY_ROLE_HOST ? "HOST" : "JOIN");
    if (payload_length < 0 || (size_t)payload_length >= sizeof(payload)) {
        return fail(ctx, NETPLAY_CLIENT_ERR_ARGUMENT,
                    "PING payload formatting failed");
    }

    rc = send_packet(ctx, NETPLAY_MSG_PING, ctx->result->session_id,
                     payload, (uint16_t)payload_length, &sequence);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }
    ctx->result->ping_sequence = sequence;

    for (;;) {
        netplay_packet_t packet;
        int got = receive_packet(ctx, receive_timeout_ms, storage, &packet);

        if (got == 0) {
            return fail(ctx, NETPLAY_CLIENT_ERR_TIMEOUT,
                        "timed out waiting for PONG");
        }
        if (got < 0) {
            return got;
        }
        if (packet.type == NETPLAY_MSG_ERROR) {
            return relay_error(ctx, &packet);
        }

        /*
         * The peer can finish its own PING first and send DONE while this
         * client is still waiting for its PONG. DATA and PONG share the same
         * UDP socket, so control/application datagrams may be interleaved.
         * Remember the peer barrier and continue waiting for our PONG.
         */
        if (is_peer_done(&packet, role, ctx->result->session_id)) {
            ctx->peer_done_seen = 1u;
            notify(ctx, "SYNC EARLY", "Received peer DONE while waiting for PONG");
            continue;
        }

        if (packet.type == NETPLAY_MSG_PONG &&
            packet.session_id == ctx->result->session_id &&
            packet.sequence == sequence &&
            packet.payload_length == (uint16_t)payload_length &&
            memcmp(packet.payload, payload, (size_t)payload_length) == 0) {
            ctx->result->ping_passed = 1u;
            notify(ctx, "PING/PONG PASSED", "Relay answered with the matching PONG");
            return NETPLAY_CLIENT_OK;
        }

        return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                    "unexpected message while waiting for PONG");
        notify(ctx, "PING/PONG PASSED", "Relay answered with the matching PONG");
        return NETPLAY_CLIENT_OK;
    }
}

static int done_barrier(struct client_context *ctx, uint8_t role,
                        uint32_t receive_timeout_ms)
{
    uint8_t storage[NDNP_RX_BUFFER_SIZE];
    char local_done[16];
    char peer_done[16];
    const char *local_name = role == NETPLAY_ROLE_HOST ? "HOST" : "JOIN";
    const char *peer_name = role == NETPLAY_ROLE_HOST ? "JOIN" : "HOST";
    int local_length;
    int peer_length;
    int rc;

    local_length = snprintf(local_done, sizeof(local_done), "DONE|%s", local_name);
    peer_length = snprintf(peer_done, sizeof(peer_done), "DONE|%s", peer_name);
    if (local_length < 0 || peer_length < 0 ||
        (size_t)local_length >= sizeof(local_done) ||
        (size_t)peer_length >= sizeof(peer_done)) {
        return fail(ctx, NETPLAY_CLIENT_ERR_ARGUMENT,
                    "DONE payload formatting failed");
    }

    rc = send_packet(ctx, NETPLAY_MSG_DATA, ctx->result->session_id,
                     local_done, (uint16_t)local_length, NULL);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }

    if (ctx->peer_done_seen != 0u) {
        notify(ctx, "SYNC PASSED", "Both clients reached DONE barrier");
        return NETPLAY_CLIENT_OK;
    }

    notify(ctx, "SYNC", "Waiting for peer DONE barrier");
    for (;;) {
        netplay_packet_t packet;
        int got = receive_packet(ctx, receive_timeout_ms, storage, &packet);

        if (got == 0) {
            return fail(ctx, NETPLAY_CLIENT_ERR_TIMEOUT,
                        "timed out waiting for peer DONE barrier");
        }
        if (got < 0) {
            return got;
        }
        if (packet.type == NETPLAY_MSG_ERROR) {
            return relay_error(ctx, &packet);
        }
        if (packet.type != NETPLAY_MSG_DATA ||
            packet.session_id != ctx->result->session_id) {
            return fail(ctx, NETPLAY_CLIENT_ERR_MISMATCH,
                        "unexpected message while waiting for DONE");
        }
        if (packet.payload_length == (uint16_t)peer_length &&
            memcmp(packet.payload, peer_done, (size_t)peer_length) == 0) {
            notify(ctx, "SYNC PASSED", "Both clients reached DONE barrier");
            return NETPLAY_CLIENT_OK;
        }
    }
}

static int leave_session(struct client_context *ctx, uint8_t role,
                         uint32_t receive_timeout_ms)
{
    uint8_t storage[NDNP_RX_BUFFER_SIZE];
    int rc;

    if (role == NETPLAY_ROLE_HOST) {
        rc = send_packet(ctx, NETPLAY_MSG_LEAVE, ctx->result->session_id,
                         NULL, 0u, NULL);
        if (rc != NETPLAY_CLIENT_OK) {
            return rc;
        }
        notify(ctx, "LEAVE SENT", "Relay session closed by HOST");
        return NETPLAY_CLIENT_OK;
    }

    notify(ctx, "WAIT PEER_LEFT", "Waiting for HOST to close the session");
    for (;;) {
        netplay_packet_t packet;
        int got = receive_packet(ctx, receive_timeout_ms, storage, &packet);

        if (got == 0) {
            return fail(ctx, NETPLAY_CLIENT_ERR_TIMEOUT,
                        "timed out waiting for PEER_LEFT");
        }
        if (got < 0) {
            return got;
        }
        if (packet.type == NETPLAY_MSG_ERROR) {
            return relay_error(ctx, &packet);
        }
        if (packet.type == NETPLAY_MSG_PEER_LEFT &&
            packet.session_id == ctx->result->session_id) {
            ctx->result->peer_left = 1u;
            notify(ctx, "PEER_LEFT PASSED", "HOST departure was received");
            return NETPLAY_CLIENT_OK;
        }
    }
}

int netplay_client_run(const netplay_transport_t *transport,
                       const netplay_client_options_t *options,
                       netplay_client_result_t *result,
                       netplay_client_event_fn event_fn,
                       void *event_opaque)
{
    struct client_context ctx;
    char room[NETPLAY_ROOM_MAX_LENGTH + 1u];
    size_t room_length = 0u;
    uint32_t match_timeout_ms;
    uint32_t receive_timeout_ms;
    uint32_t packet_count;
    size_t i;
    int rc;

    if (transport == NULL || options == NULL || result == NULL ||
        transport->send_datagram == NULL || transport->receive_datagram == NULL ||
        options->room_code == NULL ||
        (options->role != NETPLAY_ROLE_HOST && options->role != NETPLAY_ROLE_JOIN)) {
        return NETPLAY_CLIENT_ERR_ARGUMENT;
    }

    memset(result, 0, sizeof(*result));
    memset(&ctx, 0, sizeof(ctx));
    ctx.transport = transport;
    ctx.result = result;
    ctx.event_fn = event_fn;
    ctx.event_opaque = event_opaque;
    ctx.next_sequence = 1u;

    rc = netplay_room_code_length(options->room_code, &room_length);
    if (rc != NETPLAY_PROTOCOL_OK) {
        return fail(&ctx, NETPLAY_CLIENT_ERR_ARGUMENT,
                    netplay_protocol_error_string(rc));
    }
    for (i = 0u; i < room_length; i++) {
        char ch = options->room_code[i];
        room[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    room[room_length] = '\0';

    match_timeout_ms = options->match_timeout_ms != 0u
        ? options->match_timeout_ms : NETPLAY_CLIENT_DEFAULT_MATCH_TIMEOUT_MS;
    receive_timeout_ms = options->receive_timeout_ms != 0u
        ? options->receive_timeout_ms : NETPLAY_CLIENT_DEFAULT_RECV_TIMEOUT_MS;
    packet_count = options->data_packet_count != 0u
        ? options->data_packet_count : NETPLAY_CLIENT_DEFAULT_DATA_PACKETS;

    if (match_timeout_ms < receive_timeout_ms || packet_count == 0u ||
        packet_count > NETPLAY_CLIENT_MAX_DATA_PACKETS) {
        return fail(&ctx, NETPLAY_CLIENT_ERR_ARGUMENT,
                    "invalid match timeout or DATA packet count");
    }

    rc = wait_for_match(&ctx, options, room, room_length,
                        match_timeout_ms, receive_timeout_ms);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }

    rc = send_test_data(&ctx, options->role, packet_count);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }
    rc = receive_test_data(&ctx, options->role, packet_count, receive_timeout_ms);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }
    rc = ping_peer_service(&ctx, options->role, receive_timeout_ms);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }
    rc = done_barrier(&ctx, options->role, receive_timeout_ms);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }
    rc = leave_session(&ctx, options->role, receive_timeout_ms);
    if (rc != NETPLAY_CLIENT_OK) {
        return rc;
    }

    notify(&ctx, "NETPLAY CLIENT TEST PASS",
           options->role == NETPLAY_ROLE_HOST ? "HOST completed NDNP v1 test" : "JOIN completed NDNP v1 test");
    return NETPLAY_CLIENT_OK;
}

const char *netplay_client_error_string(int error)
{
    switch (error) {
        case NETPLAY_CLIENT_OK:
            return "success";
        case NETPLAY_CLIENT_ERR_ARGUMENT:
            return "invalid client argument";
        case NETPLAY_CLIENT_ERR_TRANSPORT:
            return "UDP transport operation failed";
        case NETPLAY_CLIENT_ERR_PROTOCOL:
            return "NDNP protocol error";
        case NETPLAY_CLIENT_ERR_RELAY:
            return "relay returned an ERROR message";
        case NETPLAY_CLIENT_ERR_TIMEOUT:
            return "relay operation timed out";
        case NETPLAY_CLIENT_ERR_MISMATCH:
            return "unexpected relay response or payload";
        default:
            return "unknown netplay client error";
    }
}
