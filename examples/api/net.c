/* net.c -- see net.h. */
#include "net.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
typedef WSAPOLLFD pollfd_t;
#define BAD_SOCK INVALID_SOCKET
#define CLOSE_SOCK closesocket
#define POLL_READ POLLRDNORM
#define POLL_WRITE POLLWRNORM
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
typedef struct pollfd pollfd_t;
#define BAD_SOCK (-1)
#define CLOSE_SOCK close
#define POLL_READ POLLIN
#define POLL_WRITE POLLOUT
#define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

/* Written by a signal handler (POSIX) or the console's handler thread
   (Windows) and read by every connection thread. volatile sig_atomic_t is
   enough for a signal handler and its own thread, not for other threads:
   C11 makes that a data race, so the flag is a lock-free atomic. */
#ifdef _WIN32
static volatile LONG stop_requested = 0;
#define STOP_SET() InterlockedExchange(&stop_requested, 1)
#define STOP_GET() InterlockedCompareExchange(&stop_requested, 0, 0)
#else
static int stop_requested = 0;
#define STOP_SET() __atomic_store_n(&stop_requested, 1, __ATOMIC_RELEASE)
#define STOP_GET() __atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)
#endif

int64_t net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#else
    signal(SIGPIPE, SIG_IGN);   /* a peer that went away is an error return, not a signal */
#endif
    return 0;
}

int64_t net_listen(int64_t port, int64_t backlog) {
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCK) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(s, (int)backlog) != 0) {
        CLOSE_SOCK(s);
        return -2;
    }
    return (int64_t)s;
}

int64_t net_wait_accept(int64_t listener, int64_t ms) {
    pollfd_t p = { (sock_t)listener, POLL_READ, 0 };
#ifdef _WIN32
    int r = WSAPoll(&p, 1, (INT)ms);
#else
    int r = poll(&p, 1, (int)ms);
    if (r < 0 && errno == EINTR) return 0;   /* a signal: let the caller look at the flag */
#endif
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    return 1;
}

int64_t net_accept(int64_t listener) {
    sock_t c = accept((sock_t)listener, NULL, NULL);
    if (c == BAD_SOCK) {
#ifdef _WIN32
        int e = WSAGetLastError();
        return e == WSAEWOULDBLOCK || e == WSAECONNRESET ? NET_AGAIN : -1;
#else
        /* A connection reset before it was taken is no reason to stop. */
        return WOULD_BLOCK() || errno == EINTR || errno == ECONNABORTED ? NET_AGAIN : -1;
#endif
    }
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return (int64_t)c;
}

int64_t net_set_nonblocking(int64_t sock) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket((SOCKET)sock, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int flags = fcntl((int)sock, F_GETFL, 0);
    return flags >= 0 && fcntl((int)sock, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

int64_t net_wait_readable(int64_t sock, int64_t ms) {
    pollfd_t p = { (sock_t)sock, POLL_READ, 0 };
#ifdef _WIN32
    int r = WSAPoll(&p, 1, (INT)ms);
#else
    int r = poll(&p, 1, (int)ms);
    if (r < 0 && errno == EINTR) return 0;
#endif
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (p.revents & POLLNVAL) return -1;
    return 1;   /* readable, or hung up or failed: the recv that follows says which */
}

int64_t net_set_send_timeout(int64_t sock, int64_t ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    return setsockopt((SOCKET)sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t) == 0 ? 0 : -1;
#else
    struct timeval t = { (time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000) };
    return setsockopt((int)sock, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t) == 0 ? 0 : -1;
#endif
}

int64_t net_recv(int64_t sock, uint8_t *buf, int64_t cap) {
    if (cap <= 0) return -1;
#ifdef _WIN32
    int n = recv((SOCKET)sock, (char *)buf, cap > 0x7fffffff ? 0x7fffffff : (int)cap, 0);
#else
    ssize_t n;
    do n = recv((int)sock, buf, (size_t)cap, 0); while (n < 0 && errno == EINTR);
#endif
    if (n < 0) return WOULD_BLOCK() ? NET_AGAIN : -1;
    return (int64_t)n;
}

int64_t net_send(int64_t sock, const uint8_t *buf, int64_t len) {
    int64_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send((SOCKET)sock, (const char *)buf + sent,
                     len - sent > 0x7fffffff ? 0x7fffffff : (int)(len - sent), 0);
#else
        ssize_t n = send((int)sock, buf + sent, (size_t)(len - sent), 0);
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n <= 0) return -1;
        sent += n;
    }
    return len;
}

int64_t net_send_some(int64_t sock, const uint8_t *buf, int64_t len) {
    if (len <= 0) return -1;
#ifdef _WIN32
    int n = send((SOCKET)sock, (const char *)buf, len > 0x7fffffff ? 0x7fffffff : (int)len, 0);
#else
    ssize_t n;
    do n = send((int)sock, buf, (size_t)len, 0); while (n < 0 && errno == EINTR);
#endif
    if (n < 0) return WOULD_BLOCK() ? NET_AGAIN : -1;
    return n == 0 ? NET_AGAIN : (int64_t)n;
}

void net_close(int64_t sock) { CLOSE_SOCK((sock_t)sock); }

/* --- the poller ------------------------------------------------------------
   One pollfd array, the tag and ownership of each entry beside it, and for
   each tag its entry's index, so that changing or removing one is O(1); a
   removal moves the last entry into the hole. A wait is O(entries). */
typedef struct {
    pollfd_t *fds;
    int64_t *tags;
    uint8_t *owned;
    int64_t n, cap;
    int64_t *at;        /* tag -> index into fds, -1 for none */
    int64_t ntags;
} net_poller;

static short poll_events(int64_t events) {
    return (short)(((events & NET_READ) ? POLL_READ : 0) | ((events & NET_WRITE) ? POLL_WRITE : 0));
}

int64_t net_poller_new(void) {
    net_poller *p = (net_poller *)calloc(1, sizeof *p);
    return (int64_t)(intptr_t)p;
}

void net_poller_free(int64_t poller) {
    net_poller *p = (net_poller *)(intptr_t)poller;
    if (!p) return;
    for (int64_t i = 0; i < p->n; i++)
        if (p->owned[i]) CLOSE_SOCK(p->fds[i].fd);
    free(p->fds);
    free(p->tags);
    free(p->owned);
    free(p->at);
    free(p);
}

int64_t net_poller_add(int64_t poller, int64_t sock, int64_t tag, int64_t events, int64_t owned) {
    net_poller *p = (net_poller *)(intptr_t)poller;
    if (tag < 0) return -1;
    if (tag >= p->ntags) {
        int64_t nt = p->ntags ? p->ntags : 64;
        while (nt <= tag) nt *= 2;
        int64_t *at = (int64_t *)realloc(p->at, (size_t)nt * sizeof *at);
        if (!at) return -1;
        for (int64_t i = p->ntags; i < nt; i++) at[i] = -1;
        p->at = at;
        p->ntags = nt;
    }
    if (p->at[tag] >= 0) return -1;
    if (p->n == p->cap) {
        int64_t nc = p->cap ? p->cap * 2 : 64;
        pollfd_t *fds = (pollfd_t *)realloc(p->fds, (size_t)nc * sizeof *fds);
        if (!fds) return -1;
        p->fds = fds;
        int64_t *tags = (int64_t *)realloc(p->tags, (size_t)nc * sizeof *tags);
        if (!tags) return -1;
        p->tags = tags;
        uint8_t *own = (uint8_t *)realloc(p->owned, (size_t)nc);
        if (!own) return -1;
        p->owned = own;
        p->cap = nc;
    }
    int64_t i = p->n++;
    p->fds[i].fd = (sock_t)sock;
    p->fds[i].events = poll_events(events);
    p->fds[i].revents = 0;
    p->tags[i] = tag;
    p->owned[i] = owned != 0;
    p->at[tag] = i;
    return 0;
}

void net_poller_set(int64_t poller, int64_t tag, int64_t events) {
    net_poller *p = (net_poller *)(intptr_t)poller;
    if (tag >= 0 && tag < p->ntags && p->at[tag] >= 0) p->fds[p->at[tag]].events = poll_events(events);
}

void net_poller_remove(int64_t poller, int64_t tag) {
    net_poller *p = (net_poller *)(intptr_t)poller;
    if (tag < 0 || tag >= p->ntags || p->at[tag] < 0) return;
    int64_t i = p->at[tag];
    if (p->owned[i]) CLOSE_SOCK(p->fds[i].fd);
    int64_t last = --p->n;
    if (i != last) {
        p->fds[i] = p->fds[last];
        p->tags[i] = p->tags[last];
        p->owned[i] = p->owned[last];
        p->at[p->tags[i]] = i;
    }
    p->at[tag] = -1;
}

int64_t net_poller_count(int64_t poller) { return ((net_poller *)(intptr_t)poller)->n; }

int64_t net_poller_wait(int64_t poller, int64_t ms, int64_t *tags, int64_t *events, int64_t cap) {
    net_poller *p = (net_poller *)(intptr_t)poller;
    if (p->n == 0) {
        /* Nothing to wait on: wait out the time, as poll would. */
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        usleep((useconds_t)(ms * 1000));
#endif
        return 0;
    }
#ifdef _WIN32
    int r = WSAPoll(p->fds, (ULONG)p->n, (INT)ms);
#else
    int r = poll(p->fds, (nfds_t)p->n, (int)ms);
    if (r < 0 && errno == EINTR) return 0;
#endif
    if (r <= 0) return r < 0 ? -1 : 0;
    int64_t k = 0;
    for (int64_t i = 0; i < p->n && k < cap; i++) {
        short re = p->fds[i].revents;
        if (!re) continue;
        int64_t ev = 0;
        if (re & POLL_READ) ev |= NET_READ;
        if (re & POLL_WRITE) ev |= NET_WRITE;
        if (re & (POLLERR | POLLHUP | POLLNVAL)) ev |= NET_HUP;
        tags[k] = p->tags[i];
        events[k] = ev;
        k++;
    }
    return k;
}

#ifdef _WIN32
static BOOL WINAPI on_console_event(DWORD type) {
    (void)type;
    STOP_SET();
    return TRUE;
}
#else
static void on_signal(int sig) {
    (void)sig;
    STOP_SET();
}
#endif

void net_catch_signals(void) {
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif
}

void net_request_stop(void) { STOP_SET(); }

int64_t net_stop_requested(void) { return STOP_GET() != 0; }
