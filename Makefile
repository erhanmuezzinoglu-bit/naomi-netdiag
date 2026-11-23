# Please see the README for setting up a valid build environment.

# The top-level binary that you wish to produce.
all: netdimm.bin

# Application plus the reusable NetDIMM socket and DNS modules.
SRCS += main.c
SRCS += netdimm_net.c
SRCS += netdimm_dns.c

# Pick up base makefile rules common to all examples.
include ../../Makefile.base

# Provide the top-level ROM creation target for this binary.
netdimm.bin: ${MAKEROM_FILE} ${NAOMI_BIN_FILE}
	${MAKEROM} $@ \
		--title "Fightcade API Bridge Test" \
		--publisher "Homebrew" \
		--serial "${SERIAL}" \
		--section ${NAOMI_BIN_FILE},${START_ADDR} \
		--entrypoint ${MAIN_ADDR} \
		--main-binary-includes-test-binary \
		--test-entrypoint ${TEST_ADDR}

.PHONY: clean
clean:
	rm -rf build
	rm -f netdimm.bin
