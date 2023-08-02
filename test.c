#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fnet.h"

void onClose(struct fnet_ev *ev) {
  printf("Connection closed!\n");
}

void onData(struct fnet_ev *ev) {
  printf("Data(%d): %.*s\n", ev->buffer->len, (int)(ev->buffer->len), ev->buffer->data);

  // Simple echo if it was an accepted connection
  if (ev->connection->status & FNET_STATUS_ACCEPTED) {
    fnet_write(ev->connection, ev->buffer);
  }

}

void onConnect(struct fnet_ev *ev) {
  printf("Connection!!\n");
  ev->connection->onData  = onData;
  ev->connection->onClose = onClose;
}

void onTick(struct fnet_ev *ev) {
  printf("tick\n");
  fnet_write(ev->connection, &((struct buf){
    .len  = 12,
    .data = "Hello World\n",
  }));
}

int main(int argc, const char *argv[]) {
  int i, n;

  const char *addr = "127.0.0.1";
  uint16_t port    = 1337;
  int mode         = 1; // 1 = listen, 2 = connect

  for( i = 0 ; i < argc ; i++ ) {

    if (
      (!strcmp("--address", argv[i])) ||
      (!strcmp("-a", argv[i]))
    ) {
      i++;
      addr = argv[i];
      continue;
    }

    if (
      (!strcmp("--port", argv[i])) ||
      (!strcmp("-p", argv[i]))
    ) {
      i++;
      n = atoi(argv[i]);
      // Must be positive, 0 = not allowed, see TCP spec
      if (n <= 0) {
        fprintf(stderr, "%s: Port must be above 1, got %d\n", argv[0], n);
        return 1;
      }
      // Don't allow dynamic ports
      if (n >= 49152) {
        fprintf(stderr, "%s: Port must be below 49152, got %d\n", argv[0], n);
        return 1;
      }
      port = n;
      continue;
    }

    if (
      (!strcmp("--listen", argv[i])) ||
      (!strcmp("-l", argv[i]))
    ) {
      mode = 1;
      continue;
    }

    if (
      (!strcmp("--connect", argv[i])) ||
      (!strcmp("-c", argv[i]))
    ) {
      mode = 2;
      continue;
    }

    printf("Arg: %s\n", argv[i]);
  }

  printf("Address: %s\n", addr);
  printf("Port   : %d\n", port);
  printf("Mode   : %s\n", (mode == 1 ? "Listen" : (mode == 2 ? "Connect" : "Unknown")));


  if (mode == 1) {
    fnet_listen(addr, port, &((struct fnet_options_t){
      .proto     = FNET_PROTO_TCP,
      .flags     = 0,
      .onConnect = onConnect,
      .onData    = NULL,
      .onTick    = NULL,
      .onClose   = NULL,
      .udata     = NULL,
    }));
    fnet_main();
    return 0;
  }

  if (mode == 2) {
    fnet_connect(addr, port, &((struct fnet_options_t){
      .proto     = FNET_PROTO_TCP,
      .flags     = 0,
      .onConnect = onConnect,
      .onData    = onData,
      .onTick    = onTick,
      .onClose   = onClose,
      .udata     = NULL,
    }));
    fnet_main();
    return 0;
  }

  fprintf(stderr, "Mode not implemented\n");
  return 42;
}
