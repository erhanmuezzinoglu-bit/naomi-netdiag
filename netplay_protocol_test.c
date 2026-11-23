#include "netplay_protocol.h"

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 0;
    }
    printf("PASS: %s\n", name);
    return 1;
}

int main(void)
{
    static const uint8_t expected_host[] = {
        0x4e, 0x44, 0x4e, 0x50, 0x01, 0x01, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x54, 0x45, 0x53, 0x54, 0x30, 0x31
    };
    static const char room[] = "TEST01";
    static const char data_payload[] = "NDTEST|HOST|0001";
    uint8_t wire[NETPLAY_MAX_DATAGRAM];
    uint8_t bad[NETPLAY_MAX_DATAGRAM];
    size_t wire_length = 0u;
    size_t room_length = 0u;
    netplay_packet_t packet;
    int ok = 1;
    int rc;

    rc = netplay_packet_encode(wire, sizeof(wire), NETPLAY_MSG_HOST,
                               0u, 1u, room, 6u, &wire_length);
    ok &= check(rc == NETPLAY_PROTOCOL_OK, "encode HOST");
    ok &= check(wire_length == sizeof(expected_host), "HOST datagram length");
    ok &= check(memcmp(wire, expected_host, sizeof(expected_host)) == 0,
                "HOST wire bytes match Python NDNP v1");

    rc = netplay_packet_decode(wire, wire_length, &packet);
    ok &= check(rc == NETPLAY_PROTOCOL_OK, "decode HOST");
    ok &= check(packet.type == NETPLAY_MSG_HOST &&
                packet.session_id == 0u && packet.sequence == 1u &&
                packet.payload_length == 6u &&
                memcmp(packet.payload, room, 6u) == 0,
                "HOST fields round-trip");

    rc = netplay_packet_encode(wire, sizeof(wire), NETPLAY_MSG_DATA,
                               0x56E9B34Bu, 0x01020304u,
                               data_payload, (uint16_t)(sizeof(data_payload) - 1u),
                               &wire_length);
    ok &= check(rc == NETPLAY_PROTOCOL_OK, "encode DATA");
    rc = netplay_packet_decode(wire, wire_length, &packet);
    ok &= check(rc == NETPLAY_PROTOCOL_OK &&
                packet.type == NETPLAY_MSG_DATA &&
                packet.session_id == 0x56E9B34Bu &&
                packet.sequence == 0x01020304u &&
                packet.payload_length == sizeof(data_payload) - 1u &&
                memcmp(packet.payload, data_payload, sizeof(data_payload) - 1u) == 0,
                "DATA fields round-trip");

    rc = netplay_room_code_length("test01", &room_length);
    ok &= check(rc == NETPLAY_PROTOCOL_OK && room_length == 6u,
                "room code accepts ASCII letters/digits");
    ok &= check(netplay_room_code_length("A-123", &room_length) == NETPLAY_PROTOCOL_ERR_ROOM,
                "room code rejects punctuation");
    ok &= check(netplay_room_code_length("ABC", &room_length) == NETPLAY_PROTOCOL_ERR_ROOM,
                "room code rejects short code");
    ok &= check(netplay_room_code_length("ABCDEFGHI", &room_length) == NETPLAY_PROTOCOL_ERR_ROOM,
                "room code rejects long code");

    memcpy(bad, wire, wire_length);
    bad[0] = 'X';
    ok &= check(netplay_packet_decode(bad, wire_length, &packet) == NETPLAY_PROTOCOL_ERR_MAGIC,
                "decoder rejects bad magic");

    memcpy(bad, wire, wire_length);
    bad[4] = 2u;
    ok &= check(netplay_packet_decode(bad, wire_length, &packet) == NETPLAY_PROTOCOL_ERR_VERSION,
                "decoder rejects unsupported version");

    memcpy(bad, wire, wire_length);
    bad[7]++;
    ok &= check(netplay_packet_decode(bad, wire_length, &packet) == NETPLAY_PROTOCOL_ERR_LENGTH,
                "decoder rejects payload length mismatch");

    ok &= check(netplay_packet_encode(wire, 8u, NETPLAY_MSG_HOST,
                                      0u, 1u, room, 6u, &wire_length) ==
                NETPLAY_PROTOCOL_ERR_CAPACITY,
                "encoder checks output capacity");

    ok &= check(netplay_packet_encode(wire, sizeof(wire), 0u,
                                      0u, 1u, NULL, 0u, &wire_length) ==
                NETPLAY_PROTOCOL_ERR_TYPE,
                "encoder rejects unknown message type");

    if (!ok) {
        return 1;
    }
    puts("NDNP C PROTOCOL TEST PASS");
    return 0;
}
