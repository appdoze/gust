/* net.h -- the sockets the API example serves HTTP over: a few plain C
   functions, so that their prototypes are checked twice, against net.c here
   and against the extern fns in net.gust (its sidecar's c_headers).

   Every signature is integers and byte pointers with a length; a socket is
   an int64_t handle, negative on error. Windows (Winsock) and POSIX. */
#ifndef GUST_NET_H
#define GUST_NET_H

#include <stdint.h>

/* Once per process, before any other call: 0, or a negative error. */
int64_t net_init(void);

/* A listening TCP socket on 0.0.0.0:port (address reuse on), or < 0. */
int64_t net_listen(int64_t port, int64_t backlog);

/* Waits up to ms milliseconds for a connection on a listening socket:
   1 one is ready, 0 the wait ran out, < 0 the socket failed or was closed. */
int64_t net_wait_accept(int64_t listener, int64_t ms);

/* A connected socket from a listening one, or < 0. TCP_NODELAY is set. */
int64_t net_accept(int64_t listener);

/* Waits up to ms milliseconds for a connected socket to have bytes to read
   (or to be closed by its peer): 1 it has, 0 the wait ran out, < 0 an error. */
int64_t net_wait_readable(int64_t sock, int64_t ms);

/* Send timeout in milliseconds, so that a client that stops reading cannot
   hold a connection's thread forever. */
int64_t net_set_send_timeout(int64_t sock, int64_t ms);

/* Up to cap bytes into buf: the count, 0 when the peer closed, < 0 on an
   error. */
int64_t net_recv(int64_t sock, uint8_t *buf, int64_t cap);

/* All len bytes from buf: len, or < 0. */
int64_t net_send(int64_t sock, const uint8_t *buf, int64_t len);

void net_close(int64_t sock);

/* Ctrl-C, SIGINT and SIGTERM set a flag instead of ending the process, so
   the server can stop accepting, finish what it is doing and exit. The flag
   is the one value every thread of the process shares: an atomic, read
   with acquire and written with release ordering. net_request_stop sets it
   as a signal would. */
void net_catch_signals(void);
void net_request_stop(void);
int64_t net_stop_requested(void);

#endif
