/* net.h -- the sockets the API example serves HTTP over: a few plain C
   functions, so that their prototypes are checked twice, against net.c here
   and against the extern fns in net.gust (its sidecar's c_headers).

   Every signature is integers and byte pointers with a length; a socket is
   an int64_t handle, negative on error. Windows (Winsock) and POSIX. */
#ifndef GUST_NET_H
#define GUST_NET_H

#include <stdint.h>

/* What a call on a non-blocking socket returns when it would have to wait. */
#define NET_AGAIN (-2)

/* Once per process, before any other call: 0, or a negative error. */
int64_t net_init(void);

/* A listening TCP socket on 0.0.0.0:port (address reuse on), or < 0. */
int64_t net_listen(int64_t port, int64_t backlog);

/* Waits up to ms milliseconds for a connection on a listening socket:
   1 one is ready, 0 the wait ran out, < 0 the socket failed or was closed. */
int64_t net_wait_accept(int64_t listener, int64_t ms);

/* A connected socket from a listening one, or < 0: NET_AGAIN when the
   listener is non-blocking and no connection is waiting. TCP_NODELAY is set. */
int64_t net_accept(int64_t listener);

/* Makes a socket's reads, writes and accepts return NET_AGAIN rather than
   wait: 0, or < 0. */
int64_t net_set_nonblocking(int64_t sock);

/* Waits up to ms milliseconds for a connected socket to have bytes to read
   (or to be closed by its peer): 1 it has, 0 the wait ran out, < 0 an error. */
int64_t net_wait_readable(int64_t sock, int64_t ms);

/* Send timeout in milliseconds, so that a client that stops reading cannot
   hold a connection's thread forever. */
int64_t net_set_send_timeout(int64_t sock, int64_t ms);

/* Up to cap bytes into buf: the count, 0 when the peer closed, NET_AGAIN
   (non-blocking, nothing there yet), or -1 on an error. */
int64_t net_recv(int64_t sock, uint8_t *buf, int64_t cap);

/* All len bytes from buf: len, or < 0. For a blocking socket. */
int64_t net_send(int64_t sock, const uint8_t *buf, int64_t len);

/* What a non-blocking socket takes of len bytes now: the count (at least 1),
   NET_AGAIN when it takes none, or -1 on an error. */
int64_t net_send_some(int64_t sock, const uint8_t *buf, int64_t len);

void net_close(int64_t sock);

/* A poller: the sockets one thread waits on, each with a tag (a small
   number >= 0 its owner chooses, unique within the poller) and the events
   it waits for. The poller owns the sockets added with owned != 0: freeing
   the poller closes them, which is how a supervisor cleans up after a
   worker it could not wait on to do it. Only one thread uses a poller at a
   time. Windows: WSAPoll; POSIX: poll. */
#define NET_READ 1
#define NET_WRITE 2
#define NET_HUP 4     /* reported only: the peer hung up or the socket failed */

/* A poller handle, or 0 when memory ran out. */
int64_t net_poller_new(void);

/* Closes every socket the poller owns, and frees it. */
void net_poller_free(int64_t poller);

/* Starts waiting on sock for events under tag: 0, or < 0 (out of memory,
   or a tag already in use or negative). */
int64_t net_poller_add(int64_t poller, int64_t sock, int64_t tag, int64_t events, int64_t owned);

/* The events tag's socket waits for from now on: NET_READ, NET_WRITE or
   both, never none (WSAPoll may refuse an entry that waits for nothing). */
void net_poller_set(int64_t poller, int64_t tag, int64_t events);

/* Stops waiting on tag's socket, closing it if the poller owns it. */
void net_poller_remove(int64_t poller, int64_t tag);

/* How many sockets the poller holds. */
int64_t net_poller_count(int64_t poller);

/* Waits up to ms milliseconds for any socket's events: how many are ready,
   at most cap, with tags[i] and events[i] for each (NET_READ, NET_WRITE,
   NET_HUP bits); 0 when the wait ran out; < 0 on an error. */
int64_t net_poller_wait(int64_t poller, int64_t ms, int64_t *tags, int64_t *events, int64_t cap);

/* Ctrl-C, SIGINT and SIGTERM set a flag instead of ending the process, so
   the server can stop accepting, finish what it is doing and exit. The flag
   is the one value every thread of the process shares: an atomic, read
   with acquire and written with release ordering. net_request_stop sets it
   as a signal would. */
void net_catch_signals(void);
void net_request_stop(void);
int64_t net_stop_requested(void);

#endif
