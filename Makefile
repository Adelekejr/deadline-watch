# DeadlineWatch — QNX 8.0 build (plain qcc wrapper, not the Momentics
# recursive-make system). On the host, with the SDP environment:
#     source ~/qnx800/qnxsdp-env.sh
#     make                # ./deadline-watch, ./dw-pump, ./dw-rogue for x86_64
# Or self-hosted inside the QNX 8.0 guest (it ships cc):
#     make QCC=cc QCC_TARGET=
#
# The binaries run inside the QNX 8.0 guest (QEMU), not on the host.
#
#     make host-check     # no QNX needed: compiles every C file with gcc
#                         # against stub QNX headers (-Werror), runs the
#                         # policy unit tests and the render.py self-test

QCC         ?= qcc
QCC_TARGET  ?= -Vgcc_ntox86_64
CFLAGS      ?= -Wall -Wextra -O2

HDRS = src/dw.h src/dw_core.h src/dw_proto.h src/dw_client.h

SUP_SRC  = src/supervisor.c src/threads.c src/config.c src/log.c src/dw_core.c
PUMP_SRC = examples/pump.c src/dw_client.c src/dw_core.c
ROG_SRC  = examples/rogue.c src/dw_client.c src/dw_core.c

BINS = deadline-watch dw-pump dw-rogue

all: $(BINS)

deadline-watch: $(SUP_SRC) $(HDRS)
	$(QCC) $(QCC_TARGET) $(CFLAGS) -o $@ $(SUP_SRC)

dw-pump: $(PUMP_SRC) $(HDRS)
	$(QCC) $(QCC_TARGET) $(CFLAGS) -o $@ $(PUMP_SRC)

dw-rogue: $(ROG_SRC) $(HDRS)
	$(QCC) $(QCC_TARGET) $(CFLAGS) -o $@ $(ROG_SRC)

clean:
	rm -f $(BINS) src/*.o examples/*.o tests/test_core

# ---- host checks (no QNX SDP required) -------------------------------
HOST_CC     ?= cc
HOST_CFLAGS  = -std=c11 -Wall -Wextra -Werror -O2 -Isrc -Itests/qnx-stubs

host-check: host-compile host-test render-test

host-compile:
	@for f in $(sort $(SUP_SRC) $(PUMP_SRC) $(ROG_SRC)); do \
	    echo "  CC (stub QNX headers) $$f"; \
	    $(HOST_CC) $(HOST_CFLAGS) -c $$f -o /tmp/dw-host-check.o || exit 1; \
	done
	@rm -f /tmp/dw-host-check.o

host-test: tests/test_core
	./tests/test_core

tests/test_core: tests/test_core.c src/dw_core.c src/dw_core.h
	$(HOST_CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 \
	    -Isrc -o $@ tests/test_core.c src/dw_core.c

render-test:
	python3 host/render.py --self-test

.PHONY: all clean host-check host-compile host-test render-test
