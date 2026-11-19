/* HOST STUB of <sys/dispatch.h> for compile checks only. Not QNX. */
#ifndef DW_STUB_DISPATCH_H
#define DW_STUB_DISPATCH_H
typedef struct _dispatch dispatch_t;
typedef struct _name_attach { dispatch_t *dpp; int chid; int mntid; int zero[2]; } name_attach_t;
#define DISPATCH_FLAG_NOLOCK 0x00000001
dispatch_t *dispatch_create_channel(int chid, unsigned flags);
name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags);
int name_detach(name_attach_t *attach, unsigned flags);
int name_open(const char *name, int flags);
int name_close(int coid);
#endif
