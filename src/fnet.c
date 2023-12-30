#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/errno.h>
#include <sys/types.h>

#if defined(_WIN32) || defined(_WIN64)
#include <sys/timeb.h>
#include <winsock2.h>
#include <Ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "finwo/poll.h"
#include "tidwall/buf.h"

#include "fnet.h"

#if defined(_WIN32) || defined(_WIN64)
#define FNET_SOCKET unsigned int
#pragma comment(lib,"Ws2_32.lib")
#else
#define FNET_SOCKET int
#endif

struct fnet_internal_t {
  struct fnet_t ext; // KEEP AT TOP, allows casting between fnet_internal_t* and fnet_t*
  void          *prev;
  void          *next;
  FNET_SOCKET   *fds;
  int           nfds;
  FNET_FLAG     flags;
};

struct fnet_internal_t *connections = NULL;
struct fpoll           *fpfd        = NULL;
int                    runners      = 0;

FNET_RETURNCODE setkeepalive(FNET_SOCKET fd) {
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &(int){1}, sizeof(int))) {
        return -1;
    }
#if defined(__linux__)
    // tcp_keepalive_time
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &(int){600}, sizeof(int))) {
        return FNET_RETURNCODE_ERROR;
    }
    // tcp_keepalive_intvl
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &(int){60}, sizeof(int))) {
        return FNET_RETURNCODE_ERROR;
    }
    // tcp_keepalive_probes
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &(int){6}, sizeof(int))) {
        return FNET_RETURNCODE_ERROR;
    }
#endif
    return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE settcpnodelay(FNET_SOCKET fd) {
  if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int))) {
    return FNET_RETURNCODE_ERROR;
  }
  return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE setnonblock(FNET_SOCKET fd) {
  if (fd < 0) return FNET_RETURNCODE_ERROR;
#if defined(_WIN32) || defined(_WIN64)
  unsigned long mode = 1;
  if (ioctlsocket(fd, FIONBIO, &mode) == 0) {
    return FNET_RETURNCODE_OK;
  } else {
    return FNET_RETURNCODE_ERROR;
  }
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return flags;
  if(fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
    return FNET_RETURNCODE_ERROR;
  }
  return FNET_RETURNCODE_OK;
#endif
}

int64_t _fnet_now() {
#if defined(_WIN32) || defined(_WIN64)
  struct _timeb timebuffer;
  _ftime(&timebuffer);
  return (int64_t)(((timebuffer.time * 1000) + timebuffer.millitm));
#else
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * ((int64_t)1000)) + (tv.tv_usec / 1000);
#endif
}

// CAUTION: assumes options have been vetted
struct fnet_internal_t * _fnet_init(const struct fnet_options_t *options) {
  if (!fpfd) fpfd = fpoll_create();

  // 1-to-1 copy, don't touch the options
  struct fnet_internal_t *conn = malloc(sizeof(struct fnet_internal_t));
  conn->ext.proto     = options->proto;
  conn->ext.status    = FNET_STATUS_INITIALIZING;
  conn->ext.udata     = options->udata;
  conn->flags         = options->flags;
  conn->ext.onListen  = options->onListen;
  conn->ext.onConnect = options->onConnect;
  conn->ext.onData    = options->onData;
  conn->ext.onTick    = options->onTick;
  conn->ext.onClose   = options->onClose;
  conn->nfds          = 0;
  conn->fds           = NULL;

  // Aanndd add to the connection tracking list
  conn->next = connections;
  if (connections) connections->prev = conn;
  connections = conn;

  // Done
  return conn;
}


struct fnet_t * fnet_listen(const char *address, uint16_t port, const struct fnet_options_t *options) {
  struct fnet_internal_t *conn;

  // Checking arguments are given
  if (!address) {
    fprintf(stderr, "fnet_listen: address argument is required\n");
    return NULL;
  }
  if (!port) {
    fprintf(stderr, "fnet_listen: port argument is required\n");
    return NULL;
  }
  if (!options) {
    fprintf(stderr, "fnet_listen: options argument is required\n");
    return NULL;
  }

  // Check if we support the protocol
  switch(options->proto) {
    case FNET_PROTO_TCP:
      // Intentionally empty
      // TODO: tcp-specific arg validation
      break;
    default:
      fprintf(stderr, "fnet_listen: unknown protocol\n");
      return NULL;
  }

  // 1-to-1 copy, don't touch the options
  conn = _fnet_init(options);

  /* struct sockaddr_in servaddr; */
  struct addrinfo hints = {}, *addrs;
  char port_str[6] = {};

  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  // Get address info
  snprintf(port_str, sizeof(port_str), "%d", port);
  int ai_err = getaddrinfo(address, port_str, &hints, &addrs);
  if (ai_err != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ai_err));
    fnet_free((struct fnet_t *)conn);
    return NULL;
  }

  // Count the addresses to listen on
  // For example, "localhost" turned to "127.0.0.1" and "::1"
  int naddrs = 0;
  struct addrinfo *addrinfo = addrs;
  while(addrinfo) {
    naddrs++;
    addrinfo = addrinfo->ai_next;
  }

  conn->fds = malloc(naddrs * sizeof(FNET_SOCKET));
  if (!conn->fds) {
    fprintf(stderr, "%s\n", strerror(ENOMEM));
    fnet_free((struct fnet_t *)conn);
    freeaddrinfo(addrs);
    return NULL;
  }

  addrinfo = addrs;
  for (; addrinfo ; addrinfo = addrinfo->ai_next ) {

    FNET_SOCKET fd = socket(addrinfo->ai_family, addrinfo->ai_socktype, addrinfo->ai_protocol);
    if (fd < 0) {
      fprintf(stderr, "socket\n");
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
      fprintf(stderr, "setsockopt(SO_REUSEADDR)\n");
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    if (setnonblock(fd) < 0) {
      fprintf(stderr, "setnonblock\n");
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    if (bind(fd, addrinfo->ai_addr, addrinfo->ai_addrlen) < 0) {
      fprintf(stderr, "bind: %s\n", strerror(errno));
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    if (listen(fd, SOMAXCONN) < 0) {
      fprintf(stderr, "listen: %s\n", strerror(errno));
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    conn->fds[conn->nfds] = fd;
    conn->nfds++;

    if (fpfd) {
      fpoll_add(fpfd, FPOLL_IN | FPOLL_HUP, fd, conn);
    }
  }

  if (conn->ext.onListen) {
    conn->ext.onListen(&((struct fnet_ev){
      .connection = (struct fnet_t *)conn,
      .type       = FNET_EVENT_LISTEN,
      .buffer     = NULL,
      .udata      = conn->ext.udata,
    }));
  }

  freeaddrinfo(addrs);
  conn->ext.status = FNET_STATUS_LISTENING;
  return (struct fnet_t *)conn;
}

struct fnet_t * fnet_connect(const char *address, uint16_t port, const struct fnet_options_t *options) {
  struct fnet_internal_t *conn;

  // Checking arguments are given
  if (!address) {
    fprintf(stderr, "fnet_connect: address argument is required\n");
    return NULL;
  }
  if (!port) {
    fprintf(stderr, "fnet_connect: port argument is required\n");
    return NULL;
  }
  if (!options) {
    fprintf(stderr, "fnet_connect: options argument is required\n");
    return NULL;
  }

  // Check if we support the protocol
  switch(options->proto) {
    case FNET_PROTO_TCP:
      // Intentionally empty
      break;
    default:
      fprintf(stderr, "fnet_connect: unknown protocol\n");
      return NULL;
  }

  // 1-to-1 copy, don't touch the options
  conn = _fnet_init(options);

  /* struct sockaddr_in servaddr; */
  struct addrinfo hints = {}, *addrs;
  char port_str[6] = {};

  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  // Get address info
  snprintf(port_str, sizeof(port_str), "%d", port);
  int ai_err = getaddrinfo(address, port_str, &hints, &addrs);
  if (ai_err != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ai_err));
    fnet_free((struct fnet_t *)conn);
    return NULL;
  }

  // Count the addresses to listen on
  // For example, "localhost" turned to "127.0.0.1" and "::1"
  struct addrinfo *addrinfo = addrs;
  while(addrinfo) {
    addrinfo = addrinfo->ai_next;
  }

  conn->fds = malloc(sizeof(FNET_SOCKET));
  if (!conn->fds) {
    fprintf(stderr, "%s\n", strerror(ENOMEM));
    fnet_free((struct fnet_t *)conn);
    freeaddrinfo(addrs);
    return NULL;
  }

  addrinfo = addrs;
  for (; addrinfo ; addrinfo = addrinfo->ai_next ) {

    FNET_SOCKET fd = socket(addrinfo->ai_family, addrinfo->ai_socktype, addrinfo->ai_protocol);
    if (fd < 0) {
      fprintf(stderr, "socket\n");
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    // Skip found address on failure to connect
    if (connect(fd, addrinfo->ai_addr, sizeof(struct sockaddr))) {
#if defined(_WIN32) || defined(_WIN64)
      closesocket(fd);
#else
      close(fd);
#endif
      continue;
    }

    if (setnonblock(fd) < 0) {
#if defined(_WIN32) || defined(_WIN64)
      closesocket(fd);
#else
      close(fd);
#endif
      fprintf(stderr, "setnonblock\n");
      fnet_free((struct fnet_t *)conn);
      freeaddrinfo(addrs);
      return NULL;
    }

    conn->fds[conn->nfds] = fd;
    conn->nfds++;

    if (fpfd) {
      fpoll_add(fpfd, FPOLL_IN | FPOLL_HUP, fd, conn);
    }

    // Only need 1 connection
    break;
  }

  freeaddrinfo(addrs);

  // Could not connect, might be unreachable, might be something else
  // We're not checking for the WHY here
  if (conn->nfds != 1) {
    fnet_free((struct fnet_t *)conn);
    return NULL;
  }

  conn->ext.status = FNET_STATUS_CONNECTED;

  if (conn->ext.onConnect) {
    conn->ext.onConnect(&((struct fnet_ev){
      .connection = (struct fnet_t *)conn,
      .type       = FNET_EVENT_CONNECT,
      .buffer     = NULL,
      .udata      = conn->ext.udata,
    }));
  }

  return (struct fnet_t *)conn;
}

FNET_RETURNCODE fnet_process(const struct fnet_t *connection) {
  struct fnet_internal_t *conn = (struct fnet_internal_t *)connection;
  struct fnet_internal_t *nconn = NULL;
  int i;
  FNET_SOCKET nfd;
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  struct buf *rbuf = NULL;

  char *tmp_buf = NULL;
  int   tmp_n   = 0;

  // Checking arguments are given
  if (!conn) {
    fprintf(stderr, "fnet_process: connection argument is required\n");
    return FNET_RETURNCODE_MISSING_ARGUMENT;
  }

  // No processing to be done here
  /* printf("Status:"); */
  /* printf((conn->ext.status & FNET_STATUS_INITIALIZING) ? " INITIALIZING" : ""); */
  /* printf((conn->ext.status & FNET_STATUS_LISTENING   ) ? " LISTENING"    : ""); */
  /* printf((conn->ext.status & FNET_STATUS_ERROR       ) ? " ERROR"        : ""); */
  /* printf((conn->ext.status & FNET_STATUS_CONNECTING  ) ? " CONNECTING"   : ""); */
  /* printf((conn->ext.status & FNET_STATUS_CONNECTED   ) ? " CONNECTED"    : ""); */
  /* printf((conn->ext.status & FNET_STATUS_CLOSED      ) ? " CLOSED"       : ""); */
  /* printf("\n"); */
  if (conn->ext.status & FNET_STATUS_INITIALIZING) return FNET_RETURNCODE_OK;
  if (conn->ext.status & FNET_STATUS_ERROR       ) return FNET_RETURNCODE_OK;
  if (conn->ext.status & FNET_STATUS_CLOSED      ) return FNET_RETURNCODE_OK;

  /* // Handle client still connecting */
  /* if (conn->ext.status & FNET_STATUS_CONNECTING) { */
  /*   // TODO: handle client connecting */
  /*   return FNET_RETURNCODE_NOT_IMPLEMENTED; */
  /* } */

  if (conn->ext.status & FNET_STATUS_CONNECTED) {
    rbuf       = malloc(sizeof(struct buf));
    tmp_buf    = malloc(BUFSIZ);
    rbuf->data = malloc(BUFSIZ);
    rbuf->cap  = BUFSIZ;

    for ( i = 0 ; i < conn->nfds ; i++ ) {

      rbuf->len = 0;
      do {
        tmp_n = recv(conn->fds[i], tmp_buf, BUFSIZ, 0);
        buf_append(rbuf, tmp_buf, tmp_n);
      } while(tmp_n == BUFSIZ);

      /* printf("Received %d bytes\n", rbuf->len); */

      if (rbuf->len < 0) {
        if (errno == EAGAIN) continue;
        buf_clear(rbuf);
        free(rbuf);
        free(tmp_buf);
        return FNET_RETURNCODE_ERRNO;
      }

      if (rbuf->len == 0) {
        fnet_close((struct fnet_t *)conn);
        break;
      }
      if (conn->ext.onData) {
        conn->ext.onData(&((struct fnet_ev){
          .connection = (struct fnet_t *)conn,
          .type       = FNET_EVENT_DATA,
          .buffer     = rbuf,
          .udata      = conn->ext.udata,
        }));
      }
    }

    buf_clear(rbuf);
    free(rbuf);
    free(tmp_buf);
    return FNET_RETURNCODE_OK;
  }

  if (conn->ext.status & FNET_STATUS_LISTENING) {
    /* printf("Processing %d listening fds\n", conn->nfds); */
    for ( i = 0 ; i < conn->nfds ; i++ ) {
      nfd = accept(conn->fds[i], (struct sockaddr *)&addr, &addrlen);
      /* printf("New sock: %d\n", nfd); */

      if (nfd < 0) {
        /* if (errno == EAGAIN     ) printf("Errno: EAGAIN(%d)\n", EAGAIN); */
        /* if (errno == EWOULDBLOCK) printf("Errno: EWOULDBLOCK(%d)\n", EWOULDBLOCK); */
        if (
          (errno == EAGAIN) ||
          (errno == EWOULDBLOCK)
        ) {
          // Simply no connections to accept
          /* printf("No new connections\n"); */
          continue;
        }
        // Indicate the errno is set
        return FNET_RETURNCODE_ERRNO;
      }

      // Make this one non-blocking and stay alive
      if (setnonblock(nfd) < 0) return FNET_RETURNCODE_ERROR;
      if (setkeepalive(nfd) < 0) return FNET_RETURNCODE_ERROR;

      // Create new fnet_t instance
      // _init already tracks the connection
      nconn = _fnet_init(&((struct fnet_options_t){
        .proto     = conn->ext.proto,
        .flags     = conn->flags & (~FNET_FLAG_RECONNECT),
        .onListen  = NULL,
        .onConnect = NULL,
        .onData    = NULL,
        .onTick    = NULL,
        .onClose   = NULL,
        .udata     = NULL,
      }));

      nconn->fds        = malloc(sizeof(FNET_SOCKET));
      nconn->fds[0]     = nfd;
      nconn->nfds       = 1;
      nconn->ext.status = FNET_STATUS_CONNECTED | FNET_STATUS_ACCEPTED;

      if (conn->ext.onConnect) {
        conn->ext.onConnect(&((struct fnet_ev){
          .connection = (struct fnet_t *)nconn,
          .type       = FNET_EVENT_CONNECT,
          .buffer     = NULL,
          .udata      = conn->ext.udata,
        }));
      }

      if (fpfd) {
        fpoll_add(fpfd, FPOLL_IN | FPOLL_HUP, nfd, nconn);
      }
    }

    // TODO: handle client connection
    return FNET_RETURNCODE_OK;
  }

  return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE fnet_write(const struct fnet_t *connection, struct buf *buf) {
  struct fnet_internal_t *conn = (struct fnet_internal_t *)connection;

  // Checking arguments are given
  if (!conn) {
    fprintf(stderr, "fnet_write: connection argument is required\n");
    return FNET_RETURNCODE_MISSING_ARGUMENT;
  }
  if (!buf) {
    fprintf(stderr, "fnet_write: buf argument is required\n");
    return FNET_RETURNCODE_MISSING_ARGUMENT;
  }

  // A listening socket is not a connection
  if (conn->ext.status & FNET_STATUS_LISTENING) {
    fprintf(stderr, "fnet_write: Writing to a listening socket is not possible\n");
    return FNET_RETURNCODE_UNPROCESSABLE;
  }

  // How would I do this?? :S
  if (conn->nfds > 1) {
    fprintf(stderr, "fnet_write: Only connections with 1 file descriptor supported\n");
    return FNET_RETURNCODE_NOT_IMPLEMENTED;
  }
  if (conn->nfds < 1) {
    fprintf(stderr, "fnet_write: Only connections with 1 file descriptor supported\n");
    return FNET_RETURNCODE_NOT_IMPLEMENTED;
  }

  int n = 0;
  ssize_t r;
  while(n < buf->len) {
    r = send(conn->fds[0], &(buf->data[n]), buf->len - n, 0);
    // Handle errors
    if (r < 0) {
      if (errno == EAGAIN) {
        continue; // Try again
      }
      // We don't have a way to handle this error (yet)
      fprintf(stderr, "fnet_write: Unable to write to connection\n");
      return FNET_RETURNCODE_ERRNO;
    }
    // Increment counter with amount of bytes written
    // Allows for a partial write to not corrupt the data stream
    n += r;
  }

  return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE fnet_close(const struct fnet_t *connection) {
  /* printf("Internal fnet_close\n"); */
  struct fnet_internal_t *conn = (struct fnet_internal_t *)connection;
  int i;

  // Checking arguments are given
  if (!conn) {
    fprintf(stderr, "fnet_close: connection argument is required\n");
    return FNET_RETURNCODE_MISSING_ARGUMENT;
  }

  if (conn->nfds) {
    for ( i = 0 ; i < conn->nfds ; i++ ) {
      if (fpfd) {
        fpoll_del(fpfd, ~0, conn->fds[i]);
      }
#if defined(_WIN32) || defined(_WIN64)
      closesocket(conn->fds[i]);
#else
      close(conn->fds[i]);
#endif
    }
    conn->nfds = 0;
    free(conn->fds);
    conn->fds = NULL;
  }

  conn->ext.status = FNET_STATUS_CLOSED;

  if (conn->ext.onClose) {
    conn->ext.onClose(&((struct fnet_ev){
      .connection = (struct fnet_t *)conn,
      .type       = FNET_EVENT_CLOSE,
      .buffer     = NULL,
      .udata      = conn->ext.udata,
    }));

    conn->ext.onClose = NULL;
  }

  return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE fnet_free(struct fnet_t *connection) {
  struct fnet_internal_t *conn = (struct fnet_internal_t *)connection;

  // Checking arguments are given
  if (!conn) {
    fprintf(stderr, "fnet_free: connection argument is required\n");
    return FNET_RETURNCODE_MISSING_ARGUMENT;
  }

  // Remove ourselves from the linked list
  if (conn->next) ((struct fnet_internal_t *)(conn->next))->prev = conn->prev;
  if (conn->prev) ((struct fnet_internal_t *)(conn->prev))->next = conn->next;
  if (conn == connections) connections = conn->next;

  fnet_close((struct fnet_t *)conn);

  if (conn->fds) free(conn->fds);

  free(conn);

  return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE fnet_tick(int doProcess) {
  struct fnet_internal_t *conn = connections;
  FNET_RETURNCODE ret;
  while(conn) {
    if (doProcess) {
      ret = fnet_process((struct fnet_t *)conn);
      if (ret < 0) return ret;
    }
    if (conn->ext.onTick) {
      conn->ext.onTick(&((struct fnet_ev){
        .connection = (struct fnet_t *)conn,
        .type       = FNET_EVENT_TICK,
        .buffer     = NULL,
        .udata      = conn->ext.udata,
      }));
    }
    conn = conn->next;
  }
  return FNET_RETURNCODE_OK;
}

void fnet_thread() {
  fnet_main();
}

FNET_RETURNCODE fnet_main() {
  FNET_RETURNCODE ret;
  int64_t         ttime = _fnet_now();
  int64_t         tdiff = 0;
  int             ev_count;
  int             i;

  if (runners) {
    return FNET_RETURNCODE_ALREADY_ACTIVE;
  }

  runners++;

  struct fpoll_ev events[8];

  while(runners) {

    // Do the actual processing
    if (fpfd) {
      ev_count = fpoll_wait(fpfd, events, 8, tdiff);
      /* if (ev_count) { */
      /*   printf("New events: %d\n", ev_count); */
      /* } */
      for( i = 0 ; i < ev_count ; i++ ) {
        /* printf("EV:"); */
        /* printf((events[i].ev & FPOLL_IN ) ? " IN" : ""); */
        /* printf((events[i].ev & FPOLL_OUT) ? " OUT" : ""); */
        /* printf((events[i].ev & FPOLL_HUP) ? " HUP" : ""); */
        /* printf("\n"); */
        ret = fnet_process((struct fnet_t *)events[i].udata);
        if (ret) return ret;
      }
    } else {
      fnet_tick(1);
    }

    // Tick timing
    tdiff = ttime - _fnet_now();
    if (fpfd && (tdiff <= 0)) {
      ttime += 1000;
      tdiff += 1000;
      fnet_tick(0);
    }

    // Sleep if no epoll
    if (!fpfd) {
      /* printf("No poll, do tick\n"); */
      ttime += 1000;
#if defined(_WIN32) || defined(_WIN64)
      Sleep(tdiff);
#else
      usleep(tdiff * 1000);
#endif
    }
  }

  /* printf("fnet_main finished\n"); */

  return FNET_RETURNCODE_OK;
}

FNET_RETURNCODE fnet_shutdown() {
  runners = 0;
  while(connections) fnet_free((struct fnet_t *)connections);
  return FNET_RETURNCODE_OK;
}
