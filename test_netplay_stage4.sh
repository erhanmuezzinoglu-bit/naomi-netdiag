#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    netplay_protocol.c netplay_protocol_test.c \
    -o netplay_protocol_test
./netplay_protocol_test
./build_netplay_client_test.sh
python3 netplay_client_interleave_test.py
./build_netplay_netdimm_test.sh
