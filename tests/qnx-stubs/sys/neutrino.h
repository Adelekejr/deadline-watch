/* HOST STUB of <sys/neutrino.h> for compile checks only. Not QNX. */
#ifndef DW_STUB_NEUTRINO_H
#define DW_STUB_NEUTRINO_H
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sched.h>

#define EOK 0
#define _NTO_CHF_FIXED_PRIORITY 0x0001u
#define _NTO_CHF_UNBLOCK        0x0002u
#define _NTO_CHF_DISCONNECT     0x0008u
#define _NTO_CHF_PRIVATE        0x0400u
#define _NTO_SIDE_CHANNEL       0x40000000
#define _PULSE_CODE_UNBLOCK     (-32)
#define _PULSE_CODE_DISCONNECT  (-33)

struct _pulse {
    uint16_t type;
    uint16_t subtype;
    int8_t code;
    uint8_t zero[3];
    union sigval value;
    int32_t scoid;
};

struct _msg_info {
    uint32_t nd, srcnd;
    pid_t pid;
    int32_t tid, chid, scoid, coid;
    int16_t priority, flags;
    size_t msglen, srcmsglen, dstmsglen;
};

int ChannelCreate(unsigned flags);
int ChannelDestroy(int chid);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);
long MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int MsgReply(long rcvid, long status, const void *msg, size_t bytes);
int MsgError(long rcvid, int err);
long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
int MsgSendPulse(int coid, int priority, int code, int value);
int SchedSet(pid_t pid, int tid, int policy, const struct sched_param *param);
int SchedGet(pid_t pid, int tid, struct sched_param *param);

#define SIGEV_PULSE_INIT(e, c, p, cd, v) \
    do { (void)(c); (void)(p); (void)(cd); (void)(v); \
         (e)->sigev_notify = SIGEV_SIGNAL; } while (0)
#endif
