#!/usr/bin/env bash
set -Eeuo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    netplay_protocol.c \
    netplay_client.c \
    netplay_client_posix_test.c \
    -o netplay_client_posix_test

echo "Built ./netplay_client_posix_test"
