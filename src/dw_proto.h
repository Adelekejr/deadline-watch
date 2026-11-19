/*
 * DeadlineWatch — system-mode wire protocol (QNX message passing).
 *
 * In system mode the supervisor registers the name DW_SERVICE_NAME with
 * name_attach(). Any process on the same node can then name_open() it and
 * talk to it with MsgSend(): a synchronous send/receive/reply exchange,
 * the native QNX IPC. Each thread that wants to be watched (or that may
 * be demoted) uses its own connection; see dw_client.h.
 *
 *   REGISTER   -> reply: slot, epoch (CLOCK_MONOTONIC, shared system-wide)
 *   BEAT       -> "cycle k done"; reply carries a command
 *                 (DW_CMD_RUN, or DW_CMD_SAFE_STATE once the supervisor
 *                 has decided the critical loop must go to its safe state)
 *   PROGRESS   -> best-effort work report (for the throughput graph)
 *
 * The supervisor learns the sender's pid/tid from the kernel
 * (struct _msg_info), not from the message, so a client cannot claim to
 * be another thread. Messages are checked against the connection
 * (scoid) that registered the slot.
 */
#ifndef DW_PROTO_H
#define DW_PROTO_H

#include <stdint.h>

#define DW_SERVICE_NAME "deadline-watch"

/* Message type: well above _IO_MAX so it can never be mistaken for a
 * resource-manager I/O message arriving on the same channel. */
#define DW_MSG_TYPE 0x4457u   /* "DW" */

#define DW_REQ_REGISTER  1
#define DW_REQ_BEAT      2
#define DW_REQ_PROGRESS  3

/* Registration kinds. */
#define DW_KIND_CRITICAL  1   /* deadline-tracked, drives enforcement, gets SAFE_STATE */
#define DW_KIND_TRACKED   2   /* deadline-tracked only */
#define DW_KIND_DEMOTABLE 3   /* best-effort thread the supervisor may demote */

/* Commands in a reply. */
#define DW_CMD_RUN        0
#define DW_CMD_SAFE_STATE 1

#define DW_NAME_LEN 24

typedef struct {
    uint16_t type;            /* DW_MSG_TYPE */
    uint16_t req;             /* DW_REQ_* */
    int32_t  slot;            /* from the REGISTER reply (BEAT/PROGRESS) */
    int32_t  kind;            /* DW_KIND_* (REGISTER) */
    int32_t  period_ms;       /* REGISTER, tracked kinds */
    int64_t  value;           /* BEAT: cycle index; PROGRESS: work units */
    char     name[DW_NAME_LEN];
} dw_msg_t;

typedef struct {
    int32_t  status;          /* 0 = ok, else an errno value */
    int32_t  slot;
    int32_t  command;         /* DW_CMD_* */
    int32_t  reserved;
    int64_t  epoch_sec;       /* REGISTER: cycle 0 starts at this */
    int64_t  epoch_nsec;      /* CLOCK_MONOTONIC instant */
} dw_reply_t;

#endif /* DW_PROTO_H */
