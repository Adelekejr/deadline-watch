# Stub QNX headers (host compile check only)

These headers declare just enough of the QNX 8.0 API (`<sys/neutrino.h>`,
`<sys/dispatch.h>`, `<sys/iomsg.h>`) for `make host-check` to compile every
C file on a Linux host with `-Wall -Wextra -Werror`. They are written from
the public QNX 8.0 C Library Reference signatures. They contain **no
implementation**: nothing here runs, and a clean host compile is not a
substitute for building with `qcc` against the real SDP 8.0 headers.
