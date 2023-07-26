#ifndef __INCLUDE_FINWO_FNET_H__
#define __INCLUDE_FINWO_FNET_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "tidwall/buf.h"

#define FNET_FLAG            uint8_t
#define FNET_FLAG_RECONNECT  1

#define FNET_PROTOCOL  uint8_t
#define FNET_PROTO_TCP 0

#define FNET_RETURNCODE                  int
#define FNET_RETURNCODE_OK               0
#define FNET_RETURNCODE_MISSING_ARGUMENT 1

#define FNET_STATUS               uint8_t
#define FNET_STATUS_INITIALIZING  0
#define FNET_STATUS_CONNECTING    1   // Client-only status
#define FNET_STATUS_READY         2   // Client = connected, server = listening
#define FNET_STATUS_ERROR         4
#define FNET_STATUS_CLOSED        8

#define FNET_CALLBACK(NAME) void (*(NAME))(struct fnet_t *connection, void *udata)
#define FNET_CALLBACK_VA(NAME, ...) void (*(NAME))(struct fnet_t *connection, __VA_ARGS__, void *udata)

struct fnet_t {
  FNET_PROTOCOL proto;
  FNET_STATUS   status;
  void *udata;
};

struct fnet_options_t {
  FNET_PROTOCOL proto;
  FNET_FLAG     flags;
  FNET_CALLBACK(onConnect);
  FNET_CALLBACK_VA(onData, struct buf *data);
  FNET_CALLBACK(onClose);
  void *udata;
};

struct fnet_t * fnet_listen(const char *address, uint16_t port, const struct fnet_options_t *options);
struct fnet_t * fnet_connect(const char *address, uint16_t port, const struct fnet_options_t *options);

FNET_RETURNCODE fnet_process(const struct fnet_t *connection);
FNET_RETURNCODE fnet_write(const struct fnet_t *connection, struct buf *buf);
FNET_RETURNCODE fnet_close(const struct fnet_t *connection);
FNET_RETURNCODE fnet_free(struct fnet_t *connection);

FNET_RETURNCODE fnet_step();
FNET_RETURNCODE fnet_main();

#ifdef __cplusplus
}
#endif

#endif // __INCLUDE_FINWO_FNET_H__
