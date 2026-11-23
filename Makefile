# APP=echo (default), APP=host, or APP=join.
APP ?= echo

ifeq ($(APP),host)
SRCS += main_netplay_host.c
SRCS += netplay_protocol.c
SRCS += netplay_client.c
SRCS += netplay_netdimm.c
SRCS += netdimm_net.c
NAOMI_BIN_FILE = build/netplay_host.bin
ROM_TARGET = netplay_host.bin
ROM_TITLE = Naomi NetPlay HOST
else ifeq ($(APP),join)
SRCS += main_netplay_join.c
SRCS += netplay_protocol.c
SRCS += netplay_client.c
SRCS += netplay_netdimm.c
SRCS += netdimm_net.c
NAOMI_BIN_FILE = build/netplay_join.bin
ROM_TARGET = netplay_join.bin
ROM_TITLE = Naomi NetPlay JOIN
else ifeq ($(APP),echo)
SRCS += main.c
SRCS += netdimm_net.c
SRCS += netdimm_dns.c
NAOMI_BIN_FILE = build/naomi.bin
ROM_TARGET = netdimm.bin
ROM_TITLE = Naomi NetDiag UDP Echo Test
else
$(error APP must be echo, host, or join)
endif

# The top-level binary that you wish to produce.
all: $(ROM_TARGET)

# Pick up base makefile rules common to all examples.
include ../../Makefile.base

# Provide the top-level ROM creation target for the selected app.
$(ROM_TARGET): ${MAKEROM_FILE} ${NAOMI_BIN_FILE}
	${MAKEROM} $@ \
		--title "${ROM_TITLE}" \
		--publisher "Homebrew" \
		--serial "${SERIAL}" \
		--section ${NAOMI_BIN_FILE},${START_ADDR} \
		--entrypoint ${MAIN_ADDR} \
		--main-binary-includes-test-binary \
		--test-entrypoint ${TEST_ADDR}

.PHONY: clean
clean:
	rm -rf build
	rm -f netdimm.bin netplay_host.bin netplay_join.bin
