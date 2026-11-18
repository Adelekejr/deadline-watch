# DeadlineWatch — QNX 8.0 / QEMU build.
#
# Plain qcc wrapper (NOT the Momentics recursive-make system).
# Requires the SDP environment on the host:
#     source ~/qnx800/qnxsdp-env.sh
# then:
#     make            # builds ./deadline-watch for x86_64
#
# The binary runs inside the QEMU guest (QNX 8.0), not on the host.
# See README.md for how it gets into the guest.
#
# NOTE (Stage 1): this Makefile has NOT yet been run against an installed
# SDP — the myQNX licence is pending. Verify `qcc -V` output and the
# link step once ~/qnx800 exists.

QCC         ?= qcc
QCC_TARGET  ?= -Vgcc_ntox86_64
CFLAGS      ?= -Wall -Wextra -O2

SRC = src/supervisor.c src/threads.c src/config.c src/log.c
OBJ = $(SRC:.c=.o)
BIN = deadline-watch

all: $(BIN)

$(BIN): $(OBJ)
	$(QCC) $(QCC_TARGET) -o $@ $(OBJ)

%.o: %.c src/dw.h
	$(QCC) $(QCC_TARGET) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

# Host-side check: run the graph script against a synthetic log.
# Needs only python3 (matplotlib optional — without it, ASCII output).
render-test:
	python3 host/render.py --synthetic --out /tmp/dw-jitter-test.png

.PHONY: all clean render-test
