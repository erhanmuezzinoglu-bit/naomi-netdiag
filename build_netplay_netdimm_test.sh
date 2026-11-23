#!/usr/bin/env bash
set -Eeuo pipefail

cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -I. \
    netplay_netdimm.c \
    netplay_netdimm_test.c \
    -o netplay_netdimm_test

./netplay_netdimm_test
