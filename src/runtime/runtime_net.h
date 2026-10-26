/* Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md. */
/* Gust runtime: the network behind stdlib/net.goose. TCP over IPv4, every
   socket non-blocking, waited on with a poller: WSAPoll on Windows, poll
   elsewhere. runtime_ext.h declares what a program calls, which this
   defines; it is part of the runtime object, after runtime_os.h.

   Handles. A listener, a poller, a connection and a detached connection are
   each an int64_t naming an entry of a table the runtime keeps:
       bits 60-62  the kind (GS_NET_K_*)
       bits 36-59  the owning thread program, for a poller and a connection
       bits 16-35  the entry's generation, which changes whenever the entry
                   is reused
       bits 0-15   the entry's index
   A handle that does not name a live entry of its kind, generation and,
   where it has one, thread (closed, reused, sent to another thread, made
   up) gets GS_NET_BAD_HANDLE and reaches nothing: no call acts on a socket
   through a stale handle. No handle is 0 or negative.

   Ownership. A poller owns the connections in it: closing the poller
   closes them. Listeners, pollers and detached connections belong to the
   thread program that made them, and when it ends, by returning or by a
   contained abort (gs_thread_main), the runtime closes what it still owns
   (gs_net_thread_end). A detached connection belongs to its poller's
   thread again once a poller adopts it.

   Threads. A poller and its connections live in their thread's own table,
   so only that thread can reach them, and nothing about them is shared or
   locked. Listeners and detached connections are shared, in tables under
   one mutex: any thread may accept from a listener or adopt a detached
   connection whose handle it holds. An accept or a poller watching a
   listener holds a reference to it, so closing the listener while they use
   it cannot let its socket number be reused under them; the socket closes
   when the last reference goes.

   Errors are negative results (GS_NET_AGAIN and the rest, below); the OS's
   own code for the last one on this thread is gs_net_os_error(). A recv of
   0 bytes means the peer closed its side.

   Windows: ws2_32.dll is loaded from the system directory the first time
   the network is used, so a program that never uses it neither links nor
   loads it. */

#define GS_NET_ERROR      (-1)   /* the OS failed it; the connection is no good */
#define GS_NET_AGAIN      (-2)   /* it would have to wait */
#define GS_NET_BAD_HANDLE (-3)   /* not a live handle of this thread */
#define GS_NET_LIMIT      (-4)   /* a table or memory ran out */
#define GS_NET_ADDRESS    (-5)   /* not an IPv4 address, or the port is taken */
#define GS_NET_TIMEOUT    (-6)   /* a connect did not finish in time */
#define GS_NET_UNAVAILABLE (-7)  /* the platform's sockets could not be started */
#define GS_NET_INVALID    (-8)   /* an argument the call does not take */

#define GS_NET_READ  1
#define GS_NET_WRITE 2
#define GS_NET_HUP   4

#define GS_NET_K_LISTENER 1
#define GS_NET_K_POLLER   2
#define GS_NET_K_CONN     3
#define GS_NET_K_DETACHED 4
#define GS_NET_MAX_INDEX  0xffff

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET gs_sock;
typedef WSAPOLLFD gs_pollfd;
#define GS_BAD_SOCK INVALID_SOCKET
#define GS_POLL_IN POLLRDNORM
#define GS_POLL_OUT POLLWRNORM
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
typedef int gs_sock;
typedef struct pollfd gs_pollfd;
#define GS_BAD_SOCK (-1)
#define GS_POLL_IN POLLIN
#define GS_POLL_OUT POLLOUT
#endif

/* The OS's code for this thread's last failure, for messages. */
static GS_TLS int64_t gs_net_oserr;
GS_API int64_t gs_net_os_error(void) { return gs_net_oserr; }

/* What a thread program has not closed, its end closes (gs_net_thread_end):
   once the network has been used, runtime_threads.h runs this hook as each
   ends. */
#if GS_NEED_THREADS
static void gs_net_thread_end(void);
#define GS_NET_HOOK() gs_set_thread_end_hook(gs_net_thread_end)
#else
#define GS_NET_HOOK() ((void)0)
#endif

/* ---- the platform ------------------------------------------------------- */

#ifdef _WIN32
/* The Winsock calls, taken from ws2_32.dll at first use. On clang and gcc
   each pointer is checked against winsock2.h's prototype at compile time. */
static struct {
    int (WSAAPI *WSAStartup)(WORD, LPWSADATA);
    int (WSAAPI *WSAGetLastError)(void);
    SOCKET (WSAAPI *socket)(int, int, int);
    int (WSAAPI *bind)(SOCKET, const struct sockaddr *, int);
    int (WSAAPI *listen)(SOCKET, int);
    SOCKET (WSAAPI *accept)(SOCKET, struct sockaddr *, int *);
    int (WSAAPI *connect)(SOCKET, const struct sockaddr *, int);
    int (WSAAPI *recv)(SOCKET, char *, int, int);
    int (WSAAPI *send)(SOCKET, const char *, int, int);
    int (WSAAPI *closesocket)(SOCKET);
    int (WSAAPI *shutdown)(SOCKET, int);
    int (WSAAPI *ioctlsocket)(SOCKET, long, u_long *);
    int (WSAAPI *setsockopt)(SOCKET, int, int, const char *, int);
    int (WSAAPI *getsockopt)(SOCKET, int, int, char *, int *);
    int (WSAAPI *getsockname)(SOCKET, struct sockaddr *, int *);
    int (WSAAPI *WSAPoll)(LPWSAPOLLFD, ULONG, INT);
} gs_ws;
#if defined(__clang__) || defined(__GNUC__)
#define GS_WS_CHECK(f) _Static_assert(__builtin_types_compatible_p(__typeof__(gs_ws.f), __typeof__(&f)), \
                                      "gs_ws." #f " does not match winsock2.h")
GS_WS_CHECK(WSAStartup); GS_WS_CHECK(WSAGetLastError); GS_WS_CHECK(socket); GS_WS_CHECK(bind);
GS_WS_CHECK(listen); GS_WS_CHECK(accept); GS_WS_CHECK(connect); GS_WS_CHECK(recv); GS_WS_CHECK(send);
GS_WS_CHECK(closesocket); GS_WS_CHECK(shutdown); GS_WS_CHECK(ioctlsocket); GS_WS_CHECK(setsockopt); GS_WS_CHECK(getsockopt);
GS_WS_CHECK(getsockname); GS_WS_CHECK(WSAPoll);
#endif
static volatile LONG gs_net_state;   /* 0 not tried, 1 ready, 2 failed */

static int gs_net_start(void) {
    GS_NET_HOOK();
    LONG s = InterlockedCompareExchange(&gs_net_state, 0, 0);
    if (s) return s == 1;
    /* Two threads may get here at once: each loads the same pointers. */
    HMODULE m = LoadLibraryExW(L"ws2_32.dll", NULL, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    int ok = m != NULL;
#define GS_WS_LOAD(f) if (ok && !(*(FARPROC *)&gs_ws.f = GetProcAddress(m, #f))) ok = 0
    GS_WS_LOAD(WSAStartup); GS_WS_LOAD(WSAGetLastError); GS_WS_LOAD(socket); GS_WS_LOAD(bind);
    GS_WS_LOAD(listen); GS_WS_LOAD(accept); GS_WS_LOAD(connect); GS_WS_LOAD(recv); GS_WS_LOAD(send);
    GS_WS_LOAD(closesocket); GS_WS_LOAD(shutdown); GS_WS_LOAD(ioctlsocket); GS_WS_LOAD(setsockopt); GS_WS_LOAD(getsockopt);
    GS_WS_LOAD(getsockname); GS_WS_LOAD(WSAPoll);
#undef GS_WS_LOAD
    WSADATA wsa;
    if (ok && gs_ws.WSAStartup(MAKEWORD(2, 2), &wsa) != 0) ok = 0;
    InterlockedExchange(&gs_net_state, ok ? 1 : 2);
    return ok;
}
static int gs_net_wouldblock(void) {
    int e = gs_ws.WSAGetLastError();
    gs_net_oserr = e;
    return e == WSAEWOULDBLOCK;
}
static void gs_net_noteerr(void) { gs_net_oserr = gs_ws.WSAGetLastError(); }
static void gs_net_closesock(gs_sock s) { gs_ws.closesocket(s); }
static int gs_net_nonblock(gs_sock s) { u_long on = 1; return gs_ws.ioctlsocket(s, FIONBIO, &on) == 0; }
static int gs_net_poll(gs_pollfd *fds, int64_t n, int64_t ms) {
    if (n == 0) { Sleep((DWORD)ms); return 0; }
    int r = gs_ws.WSAPoll(fds, (ULONG)n, (INT)ms);
    if (r < 0) gs_net_noteerr();
    return r;
}
#define GS_NET_CALL(f) gs_ws.f
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
static int gs_net_start(void) {
    GS_NET_HOOK();
    return 1;
}
static int gs_net_wouldblock(void) {
    gs_net_oserr = errno;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}
static void gs_net_noteerr(void) { gs_net_oserr = errno; }
static void gs_net_closesock(gs_sock s) { close(s); }
static int gs_net_nonblock(gs_sock s) {
    int f = fcntl(s, F_GETFL, 0);
    /* Not inherited by a child process either. */
    fcntl(s, F_SETFD, FD_CLOEXEC);
    return f >= 0 && fcntl(s, F_SETFL, f | O_NONBLOCK) == 0;
}
static int gs_net_poll(gs_pollfd *fds, int64_t n, int64_t ms) {
    int r = poll(n ? fds : NULL, (nfds_t)n, (int)ms);
    if (r < 0) {
        gs_net_noteerr();
        if (errno == EINTR) return 0;   /* a signal: the caller looks at its flags */
    }
    return r;
}
#define GS_NET_CALL(f) f
#endif

/* A connected socket's options: no Nagle delay, and (macOS, BSD) no SIGPIPE
   when the peer is gone; Linux asks per send instead (MSG_NOSIGNAL). */
static void gs_net_tune(gs_sock s) {
    int one = 1;
    GS_NET_CALL(setsockopt)(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    GS_NET_CALL(setsockopt)(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
}
#ifdef MSG_NOSIGNAL
#define GS_NET_SENDFLAGS MSG_NOSIGNAL
#else
#define GS_NET_SENDFLAGS 0
#endif

/* An IPv4 address in dotted form, or "localhost". */
static int gs_net_parse_ip(sl_u8 s, uint32_t *ip) {
    if (s.len == 9 && memcmp(s.data, "localhost", 9) == 0) { *ip = 0x7f000001u; return 1; }
    uint32_t v = 0;
    int64_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part && (i >= s.len || s.data[i++] != '.')) return 0;
        int64_t start = i;
        uint32_t n = 0;
        while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9' && i - start < 3) n = n * 10 + (s.data[i++] - '0');
        if (i == start || n > 255) return 0;
        v = v << 8 | n;
    }
    if (i != s.len) return 0;
    *ip = v;
    return 1;
}

/* ---- handles ------------------------------------------------------------ */

static int64_t gs_net_handle(int64_t kind, int64_t thread, uint32_t gen, int64_t index) {
    return kind << 60 | (thread & 0xffffff) << 36 | (int64_t)(gen & 0xfffff) << 16 | index;
}
static int64_t gs_net_kind(int64_t h) { return h >> 60 & 7; }
static int64_t gs_net_thread_of(int64_t h) { return h >> 36 & 0xffffff; }
static uint32_t gs_net_gen_of(int64_t h) { return (uint32_t)(h >> 16 & 0xfffff); }
static int64_t gs_net_index_of(int64_t h) { return h & 0xffff; }
/* The current thread program's number in a handle: main is 0. */
static int64_t gs_net_self(void) { return (gs_current_thread_id + 1) & 0xffffff; }
/* A generation for an entry's next use: never 0, so no handle is 0. */
static uint32_t gs_net_nextgen(uint32_t g) { g = (g + 1) & 0xfffff; return g ? g : 1; }

/* The shared tables' lock: a mutex where the program has threads. */
#if GS_NEED_THREADS
static gs_mutex gs_net_mutex = GS_MUTEX_INIT;
#define GS_NET_LOCK() gs_mutex_lock(&gs_net_mutex)
#define GS_NET_UNLOCK() gs_mutex_unlock(&gs_net_mutex)
#else
#define GS_NET_LOCK() ((void)0)
#define GS_NET_UNLOCK() ((void)0)
#endif

#ifndef _WIN32
/* One descriptor held in reserve, as libuv does: an accept refused for want
   of descriptors closes it, takes the waiting connection off the queue and
   closes that, and opens it again. The client is told at once, and the
   listener does not stay ready with nothing anyone can take. Opened with the
   first listener; under the shared lock. */
static int gs_net_reserve = -1;
static void gs_net_drop_one(gs_sock ls) {
    GS_NET_LOCK();
    if (gs_net_reserve >= 0) {
        close(gs_net_reserve);
        gs_net_reserve = -1;
        int s = accept(ls, NULL, NULL);
        if (s >= 0) close(s);
        gs_net_reserve = open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    GS_NET_UNLOCK();
}
#endif

/* Listeners: shared. refs counts the accepts under way and the pollers
   watching it; a close marks it closing and the last reference closes the
   socket. */
typedef struct {
    gs_sock sock;
    uint32_t gen;
    uint8_t live, closing;
    int64_t owner, refs, port;
} gs_net_lst;
static gs_net_lst *gs_net_lsts;
static int64_t gs_net_nlsts;

/* Detached connections: shared, between a detach and an adopt. */
typedef struct {
    gs_sock sock;
    uint32_t gen;
    uint8_t live;
    int64_t owner;
} gs_net_det;
static gs_net_det *gs_net_dets;
static int64_t gs_net_ndets;
/* Whether this process has used the network, so that a thread's end can
   skip the shared tables when it has not. */
static volatile int gs_net_used;

/* A free entry of a shared table, growing it; -1 when it is full. Under the lock. */
#define GS_NET_TAKE(arr, n, type, out) do {                                       \
        out = -1;                                                                 \
        for (int64_t i_ = 0; i_ < n; i_++) if (!arr[i_].live) { out = i_; break; } \
        if (out < 0 && n <= GS_NET_MAX_INDEX) {                                   \
            int64_t cap_ = n ? n * 2 : 16;                                        \
            if (cap_ > GS_NET_MAX_INDEX + 1) cap_ = GS_NET_MAX_INDEX + 1;         \
            if (cap_ > n) {                                                       \
                type *p_ = (type *)realloc(arr, (size_t)cap_ * sizeof(type));     \
                if (p_) {                                                         \
                    memset(p_ + n, 0, (size_t)(cap_ - n) * sizeof(type));         \
                    arr = p_; out = n; n = cap_;                                  \
                }                                                                 \
            }                                                                     \
        }                                                                         \
    } while (0)

static gs_net_lst *gs_net_lst_of(int64_t h) {
    if (gs_net_kind(h) != GS_NET_K_LISTENER) return NULL;
    int64_t i = gs_net_index_of(h);
    if (i >= gs_net_nlsts) return NULL;
    gs_net_lst *l = &gs_net_lsts[i];
    return l->live && !l->closing && l->gen == gs_net_gen_of(h) ? l : NULL;
}
/* Drops a reference, closing the socket with the last. Under the lock. */
static void gs_net_lst_unref(gs_net_lst *l) {
    if (--l->refs == 0 && l->closing) {
        gs_net_closesock(l->sock);
        l->live = 0;
        l->closing = 0;
        l->gen = gs_net_nextgen(l->gen);
    }
}

/* Pollers and their connections: in their thread's own table. */
typedef struct {
    gs_sock sock;
    uint32_t gen;
    uint8_t live;
    int32_t fd;            /* its entry in fds; -1 while it waits for nothing */
} gs_net_cn;
typedef struct {
    gs_sock sock;
    int64_t handle;        /* the listener */
    int32_t fd;
} gs_net_wt;
typedef struct {
    uint32_t gen;
    uint8_t live;
    gs_pollfd *fds;
    int64_t *fdref;        /* per fds entry: a connection's index, or -1 - a watch's */
    int64_t nfds, capfds;
    gs_net_cn *cns;
    int64_t ncns, capcns;
    int64_t *freecns;
    int64_t nfree;
    gs_net_wt watches[8];
    int64_t nwatches;
} gs_net_pl;
static GS_TLS gs_net_pl *gs_net_pls;
static GS_TLS int64_t gs_net_npls;

static gs_net_pl *gs_net_pl_of(int64_t h) {
    if (gs_net_kind(h) != GS_NET_K_POLLER || gs_net_thread_of(h) != gs_net_self()) return NULL;
    int64_t i = gs_net_index_of(h);
    if (i >= gs_net_npls) return NULL;
    gs_net_pl *p = &gs_net_pls[i];
    return p->live && p->gen == gs_net_gen_of(h) ? p : NULL;
}
static gs_net_cn *gs_net_cn_of(gs_net_pl *p, int64_t h) {
    if (gs_net_kind(h) != GS_NET_K_CONN || gs_net_thread_of(h) != gs_net_self()) return NULL;
    int64_t i = gs_net_index_of(h);
    if (i >= p->ncns) return NULL;
    gs_net_cn *c = &p->cns[i];
    return c->live && c->gen == gs_net_gen_of(h) ? c : NULL;
}

/* A pollfd entry for sock, refering back to ref; -1 when memory ran out. */
static int64_t gs_net_addfd(gs_net_pl *p, gs_sock sock, short events, int64_t ref) {
    if (p->nfds == p->capfds) {
        int64_t cap = p->capfds ? p->capfds * 2 : 64;
        gs_pollfd *fds = (gs_pollfd *)realloc(p->fds, (size_t)cap * sizeof *fds);
        if (!fds) return -1;
        p->fds = fds;
        int64_t *refs = (int64_t *)realloc(p->fdref, (size_t)cap * sizeof *refs);
        if (!refs) return -1;
        p->fdref = refs;
        p->capfds = cap;
    }
    int64_t f = p->nfds++;
    p->fds[f].fd = sock;
    p->fds[f].events = events;
    p->fds[f].revents = 0;
    p->fdref[f] = ref;
    return f;
}
/* Removes entry f, moving the last one into its place. */
static void gs_net_dropfd(gs_net_pl *p, int64_t f) {
    int64_t last = --p->nfds;
    if (f != last) {
        p->fds[f] = p->fds[last];
        p->fdref[f] = p->fdref[last];
        int64_t r = p->fdref[f];
        if (r >= 0) p->cns[r].fd = (int32_t)f;
        else p->watches[-1 - r].fd = (int32_t)f;
    }
}

/* A connection entry for sock, in the poller and waiting to read: its
   handle, or GS_NET_LIMIT (and sock closed). */
static int64_t gs_net_addcn(gs_net_pl *p, gs_sock sock) {
    int64_t i;
    if (p->nfree) i = p->freecns[--p->nfree];
    else {
        if (p->ncns > GS_NET_MAX_INDEX) { gs_net_closesock(sock); return GS_NET_LIMIT; }
        if (p->ncns == p->capcns) {
            int64_t cap = p->capcns ? p->capcns * 2 : 64;
            gs_net_cn *cns = (gs_net_cn *)realloc(p->cns, (size_t)cap * sizeof *cns);
            if (!cns) { gs_net_closesock(sock); return GS_NET_LIMIT; }
            memset(cns + p->capcns, 0, (size_t)(cap - p->capcns) * sizeof *cns);
            p->cns = cns;
            int64_t *fr = (int64_t *)realloc(p->freecns, (size_t)cap * sizeof *fr);
            if (!fr) { gs_net_closesock(sock); return GS_NET_LIMIT; }
            p->freecns = fr;
            p->capcns = cap;
        }
        i = p->ncns++;
    }
    gs_net_cn *c = &p->cns[i];
    int64_t f = gs_net_addfd(p, sock, GS_POLL_IN, i);
    if (f < 0) { p->freecns[p->nfree++] = i; gs_net_closesock(sock); return GS_NET_LIMIT; }
    c->sock = sock;
    c->gen = gs_net_nextgen(c->gen);
    c->live = 1;
    c->fd = (int32_t)f;
    return gs_net_handle(GS_NET_K_CONN, gs_net_self(), c->gen, i);
}
/* Takes connection i out of the poller: its socket closed, or not. */
static void gs_net_dropcn(gs_net_pl *p, int64_t i, int closesock) {
    gs_net_cn *c = &p->cns[i];
    if (closesock) gs_net_closesock(c->sock);
    if (c->fd >= 0) gs_net_dropfd(p, c->fd);   /* -1: waiting for nothing */
    c->live = 0;
    c->gen = gs_net_nextgen(c->gen);
    p->freecns[p->nfree++] = i;
}

/* ---- listeners ---------------------------------------------------------- */

GS_API int64_t gs_net_listen(int64_t port, int64_t backlog, uint8_t local_only) {
    if (!gs_net_start()) return GS_NET_UNAVAILABLE;
    if (port < 0 || port > 65535) return GS_NET_ADDRESS;
    gs_sock s = GS_NET_CALL(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == GS_BAD_SOCK) { gs_net_noteerr(); return GS_NET_ERROR; }
    int one = 1;
#ifdef _WIN32
    /* Windows' SO_REUSEADDR would let another process take the port over. */
    GS_NET_CALL(setsockopt)(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
#else
    GS_NET_CALL(setsockopt)(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    uint32_t ip = local_only ? 0x7f000001u : 0;
    a.sin_addr.s_addr = (uint32_t)((ip >> 24) | (ip >> 8 & 0xff00) | (ip << 8 & 0xff0000) | (ip << 24));
    a.sin_port = (uint16_t)((port >> 8 & 0xff) | (port << 8 & 0xff00));
    if (GS_NET_CALL(bind)(s, (struct sockaddr *)&a, sizeof a) != 0) {
        gs_net_noteerr();
        gs_net_closesock(s);
        return GS_NET_ADDRESS;
    }
    if (GS_NET_CALL(listen)(s, backlog < 1 ? 128 : backlog > 65535 ? 65535 : (int)backlog) != 0 ||
        !gs_net_nonblock(s)) {
        gs_net_noteerr();
        gs_net_closesock(s);
        return GS_NET_ERROR;
    }
    struct sockaddr_in b;
#ifdef _WIN32
    int blen = sizeof b;
#else
    socklen_t blen = sizeof b;
#endif
    int64_t bound = port;
    if (GS_NET_CALL(getsockname)(s, (struct sockaddr *)&b, &blen) == 0)
        bound = (int64_t)((b.sin_port >> 8 & 0xff) | (b.sin_port << 8 & 0xff00));
    GS_NET_LOCK();
    gs_net_used = 1;
#ifndef _WIN32
    if (gs_net_reserve < 0) gs_net_reserve = open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif
    int64_t i;
    GS_NET_TAKE(gs_net_lsts, gs_net_nlsts, gs_net_lst, i);
    int64_t h = GS_NET_LIMIT;
    if (i >= 0) {
        gs_net_lst *l = &gs_net_lsts[i];
        l->sock = s;
        l->gen = gs_net_nextgen(l->gen);
        l->live = 1;
        l->closing = 0;
        l->owner = gs_net_self();
        l->refs = 0;
        l->port = bound;
        h = gs_net_handle(GS_NET_K_LISTENER, 0, l->gen, i);
    }
    GS_NET_UNLOCK();
    if (h < 0) gs_net_closesock(s);
    return h;
}

GS_API int64_t gs_net_listener_port(int64_t lh) {
    GS_NET_LOCK();
    gs_net_lst *l = gs_net_lst_of(lh);
    int64_t port = l ? l->port : GS_NET_BAD_HANDLE;
    GS_NET_UNLOCK();
    return port;
}

/* Only its owner closes a listener. Closing waits for no one: the socket
   goes when the last accept or watch lets it go. */
GS_API int64_t gs_net_close_listener(int64_t lh) {
    GS_NET_LOCK();
    gs_net_lst *l = gs_net_lst_of(lh);
    int64_t r = 0;
    if (!l || l->owner != gs_net_self()) r = GS_NET_BAD_HANDLE;
    else {
        l->closing = 1;
        l->refs++;
        gs_net_lst_unref(l);
    }
    GS_NET_UNLOCK();
    return r;
}

/* ---- pollers ------------------------------------------------------------ */

GS_API int64_t gs_net_poller(void) {
    if (!gs_net_start()) return GS_NET_UNAVAILABLE;
    gs_net_used = 1;
    int64_t i = -1;
    for (int64_t k = 0; k < gs_net_npls; k++)
        if (!gs_net_pls[k].live) { i = k; break; }
    if (i < 0) {
        if (gs_net_npls > GS_NET_MAX_INDEX) return GS_NET_LIMIT;
        gs_net_pl *pls = (gs_net_pl *)realloc(gs_net_pls, (size_t)(gs_net_npls + 1) * sizeof *pls);
        if (!pls) return GS_NET_LIMIT;
        memset(&pls[gs_net_npls], 0, sizeof *pls);
        gs_net_pls = pls;
        i = gs_net_npls++;
    }
    gs_net_pl *p = &gs_net_pls[i];
    uint32_t gen = gs_net_nextgen(p->gen);
    memset(p, 0, sizeof *p);
    p->gen = gen;
    p->live = 1;
    return gs_net_handle(GS_NET_K_POLLER, gs_net_self(), gen, i);
}

static void gs_net_unwatch_at(gs_net_pl *p, int64_t w) {
    gs_net_dropfd(p, p->watches[w].fd);
    GS_NET_LOCK();
    int64_t li = gs_net_index_of(p->watches[w].handle);
    if (li < gs_net_nlsts) gs_net_lst_unref(&gs_net_lsts[li]);
    GS_NET_UNLOCK();
    /* The last watch moves into the hole, and its fds entry follows it. */
    int64_t last = --p->nwatches;
    if (w != last) {
        p->watches[w] = p->watches[last];
        p->fdref[p->watches[w].fd] = -1 - w;
    }
}

/* Closes the poller's connections and frees it. */
static void gs_net_free_pl(gs_net_pl *p) {
    while (p->nwatches) gs_net_unwatch_at(p, p->nwatches - 1);
    for (int64_t i = 0; i < p->ncns; i++)
        if (p->cns[i].live) gs_net_closesock(p->cns[i].sock);
    free(p->fds);
    free(p->fdref);
    free(p->cns);
    free(p->freecns);
    uint32_t gen = gs_net_nextgen(p->gen);
    memset(p, 0, sizeof *p);
    p->gen = gen;
}

GS_API int64_t gs_net_close_poller(int64_t ph) {
    gs_net_pl *p = gs_net_pl_of(ph);
    if (!p) return GS_NET_BAD_HANDLE;
    gs_net_free_pl(p);
    return 0;
}

GS_API int64_t gs_net_count(int64_t ph) {
    gs_net_pl *p = gs_net_pl_of(ph);
    return p ? p->ncns - p->nfree : GS_NET_BAD_HANDLE;
}

/* Waits on the listener too (on) or no longer (off). */
GS_API int64_t gs_net_watch(int64_t ph, int64_t lh, uint8_t on) {
    gs_net_pl *p = gs_net_pl_of(ph);
    if (!p) return GS_NET_BAD_HANDLE;
    for (int64_t w = 0; w < p->nwatches; w++)
        if (p->watches[w].handle == lh) {
            if (!on) gs_net_unwatch_at(p, w);
            return 0;
        }
    if (!on) return 0;
    if (p->nwatches == (int64_t)(sizeof p->watches / sizeof p->watches[0])) return GS_NET_LIMIT;
    GS_NET_LOCK();
    gs_net_lst *l = gs_net_lst_of(lh);
    gs_sock s = GS_BAD_SOCK;
    if (l) { l->refs++; s = l->sock; }
    GS_NET_UNLOCK();
    if (!l) return GS_NET_BAD_HANDLE;
    int64_t w = p->nwatches;
    int64_t f = gs_net_addfd(p, s, GS_POLL_IN, -1 - w);
    if (f < 0) {
        GS_NET_LOCK();
        gs_net_lst_unref(l);
        GS_NET_UNLOCK();
        return GS_NET_LIMIT;
    }
    p->watches[w].sock = s;
    p->watches[w].handle = lh;
    p->watches[w].fd = (int32_t)f;
    p->nwatches++;
    return 0;
}

/* A connection from the listener, into the poller and waiting to read:
   its handle, GS_NET_AGAIN when none is waiting, GS_NET_LIMIT when the
   process is out of descriptors (on POSIX the waiting connection has been
   dropped, gs_net_drop_one), or an error. */
GS_API int64_t gs_net_accept(int64_t ph, int64_t lh) {
    gs_net_pl *p = gs_net_pl_of(ph);
    if (!p) return GS_NET_BAD_HANDLE;
    GS_NET_LOCK();
    gs_net_lst *l = gs_net_lst_of(lh);
    gs_sock ls = GS_BAD_SOCK;
    if (l) { l->refs++; ls = l->sock; }
    GS_NET_UNLOCK();
    if (!l) return GS_NET_BAD_HANDLE;
    gs_sock s = GS_NET_CALL(accept)(ls, NULL, NULL);
    int again = s == GS_BAD_SOCK && gs_net_wouldblock();
    int64_t err = s == GS_BAD_SOCK ? gs_net_oserr : 0;
#ifndef _WIN32
    /* Out of descriptors: the waiting connection is taken off the queue and
       dropped, while the listener is still referenced. */
    if (err == EMFILE || err == ENFILE) gs_net_drop_one(ls);
#endif
    GS_NET_LOCK();
    gs_net_lst_unref(l);
    GS_NET_UNLOCK();
    if (s == GS_BAD_SOCK) {
        gs_net_oserr = err;
#ifndef _WIN32
        /* A connection reset before it was taken is no reason to stop. */
        if (err == ECONNABORTED || err == EINTR) again = 1;
        if (err == EMFILE || err == ENFILE) return GS_NET_LIMIT;
#else
        if (err == WSAECONNRESET) again = 1;
        if (err == WSAEMFILE || err == WSAENOBUFS) return GS_NET_LIMIT;
#endif
        return again ? GS_NET_AGAIN : GS_NET_ERROR;
    }
    if (!gs_net_nonblock(s)) { gs_net_noteerr(); gs_net_closesock(s); return GS_NET_ERROR; }
    gs_net_tune(s);
    return gs_net_addcn(p, s);
}

/* A connection to host:port (an IPv4 address, or localhost), into the
   poller: waits up to timeout_ms for it to be made. */
GS_API int64_t gs_net_connect(int64_t ph, sl_u8 host, int64_t port, int64_t timeout_ms) {
    gs_net_pl *p = gs_net_pl_of(ph);
    if (!p) return GS_NET_BAD_HANDLE;
    uint32_t ip;
    if (!gs_net_parse_ip(host, &ip) || port <= 0 || port > 65535) return GS_NET_ADDRESS;
    gs_sock s = GS_NET_CALL(socket)(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == GS_BAD_SOCK) { gs_net_noteerr(); return GS_NET_ERROR; }
    if (!gs_net_nonblock(s)) { gs_net_noteerr(); gs_net_closesock(s); return GS_NET_ERROR; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = (uint32_t)((ip >> 24) | (ip >> 8 & 0xff00) | (ip << 8 & 0xff0000) | (ip << 24));
    a.sin_port = (uint16_t)((port >> 8 & 0xff) | (port << 8 & 0xff00));
    if (GS_NET_CALL(connect)(s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (!gs_net_wouldblock()) { gs_net_closesock(s); return GS_NET_ERROR; }
        gs_pollfd pf;
        pf.fd = s;
        pf.events = GS_POLL_OUT;
        pf.revents = 0;
        int r = gs_net_poll(&pf, 1, timeout_ms < 0 ? 0 : timeout_ms);
        if (r == 0) { gs_net_closesock(s); return GS_NET_TIMEOUT; }
        int err = 0;
#ifdef _WIN32
        int elen = sizeof err;
#else
        socklen_t elen = sizeof err;
#endif
        if (r < 0 || GS_NET_CALL(getsockopt)(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 || err) {
            if (err) gs_net_oserr = err;
            gs_net_closesock(s);
            return GS_NET_ERROR;
        }
    }
    gs_net_tune(s);
    return gs_net_addcn(p, s);
}

/* Up to buf.len bytes: the count, 0 when the peer closed, GS_NET_AGAIN. */
GS_API int64_t gs_net_recv(int64_t ph, int64_t ch, sl_u8 buf) {
    gs_net_pl *p = gs_net_pl_of(ph);
    gs_net_cn *c = p ? gs_net_cn_of(p, ch) : NULL;
    if (!c) return GS_NET_BAD_HANDLE;
    if (buf.len <= 0) return 0;
    int64_t n;
#ifdef _WIN32
    n = gs_ws.recv(c->sock, (char *)buf.data, buf.len > 0x7fffffff ? 0x7fffffff : (int)buf.len, 0);
#else
    do n = recv(c->sock, buf.data, (size_t)buf.len, 0); while (n < 0 && errno == EINTR);
#endif
    if (n < 0) return gs_net_wouldblock() ? GS_NET_AGAIN : GS_NET_ERROR;
    return n;
}

/* What the socket takes of bytes now: the count (at least 1), GS_NET_AGAIN. */
GS_API int64_t gs_net_send(int64_t ph, int64_t ch, sl_u8 bytes) {
    gs_net_pl *p = gs_net_pl_of(ph);
    gs_net_cn *c = p ? gs_net_cn_of(p, ch) : NULL;
    if (!c) return GS_NET_BAD_HANDLE;
    if (bytes.len <= 0) return 0;
    int64_t n;
#ifdef _WIN32
    n = gs_ws.send(c->sock, (const char *)bytes.data, bytes.len > 0x7fffffff ? 0x7fffffff : (int)bytes.len, 0);
#else
    do n = send(c->sock, bytes.data, (size_t)bytes.len, GS_NET_SENDFLAGS); while (n < 0 && errno == EINTR);
#endif
    if (n < 0) return gs_net_wouldblock() ? GS_NET_AGAIN : GS_NET_ERROR;
    return n == 0 ? GS_NET_AGAIN : n;
}

/* What the poller waits for on the connection: GS_NET_READ, GS_NET_WRITE,
   both, or neither, which takes it out of the poll set (where WSAPoll might
   refuse an entry waiting for nothing) until it wants something again. */
GS_API int64_t gs_net_want(int64_t ph, int64_t ch, int64_t events) {
    gs_net_pl *p = gs_net_pl_of(ph);
    gs_net_cn *c = p ? gs_net_cn_of(p, ch) : NULL;
    if (!c) return GS_NET_BAD_HANDLE;
    if (events & ~(int64_t)(GS_NET_READ | GS_NET_WRITE)) return GS_NET_INVALID;
    short e = (short)(((events & GS_NET_READ) ? GS_POLL_IN : 0) | ((events & GS_NET_WRITE) ? GS_POLL_OUT : 0));
    if (!e) {
        if (c->fd >= 0) gs_net_dropfd(p, c->fd);
        c->fd = -1;
        return 0;
    }
    if (c->fd < 0) {
        int64_t f = gs_net_addfd(p, c->sock, e, gs_net_index_of(ch));
        if (f < 0) return GS_NET_LIMIT;
        c->fd = (int32_t)f;
    } else {
        p->fds[c->fd].events = e;
    }
    return 0;
}

/* Ends what the connection sends: the peer reads 0 once it has the rest,
   and the connection can still read. */
GS_API int64_t gs_net_shutdown(int64_t ph, int64_t ch) {
    gs_net_pl *p = gs_net_pl_of(ph);
    gs_net_cn *c = p ? gs_net_cn_of(p, ch) : NULL;
    if (!c) return GS_NET_BAD_HANDLE;
#ifdef _WIN32
    if (gs_ws.shutdown(c->sock, SD_SEND) != 0) { gs_net_noteerr(); return GS_NET_ERROR; }
#else
    if (shutdown(c->sock, SHUT_WR) != 0) { gs_net_noteerr(); return GS_NET_ERROR; }
#endif
    return 0;
}

GS_API int64_t gs_net_close(int64_t ph, int64_t ch) {
    gs_net_pl *p = gs_net_pl_of(ph);
    gs_net_cn *c = p ? gs_net_cn_of(p, ch) : NULL;
    if (!c) return GS_NET_BAD_HANDLE;
    gs_net_dropcn(p, gs_net_index_of(ch), 1);
    return 0;
}

/* Waits up to ms for the poller's sockets: how many are ready, as many as
   both slices hold at most, with each one's handle (a connection's, or a
   watched listener's) and its events in handles[k] and events[k]. A watched
   listener that has been closed is reported once with GS_NET_HUP and no
   longer watched. */
GS_API int64_t gs_net_wait(int64_t ph, int64_t ms, sl_i64 hs, sl_i64 evs) {
    gs_net_pl *p = gs_net_pl_of(ph);
    if (!p) return GS_NET_BAD_HANDLE;
    int64_t cap = hs.len < evs.len ? hs.len : evs.len;
    int64_t *handles = hs.data, *events = evs.data;
    if (cap <= 0) return 0;
    int64_t k = 0;
    /* Listeners closed under the watch first. */
    for (int64_t w = 0; w < p->nwatches && k < cap; ) {
        GS_NET_LOCK();
        int closed = gs_net_lst_of(p->watches[w].handle) == NULL;
        GS_NET_UNLOCK();
        if (closed) {
            handles[k] = p->watches[w].handle;
            events[k++] = GS_NET_HUP;
            gs_net_unwatch_at(p, w);
        } else w++;
    }
    if (k) return k;
    int r = gs_net_poll(p->fds, p->nfds, ms);
    if (r <= 0) return r < 0 ? GS_NET_ERROR : 0;
    for (int64_t f = 0; f < p->nfds && k < cap; f++) {
        short re = p->fds[f].revents;
        if (!re) continue;
        int64_t ev = 0;
        if (re & GS_POLL_IN) ev |= GS_NET_READ;
        if (re & GS_POLL_OUT) ev |= GS_NET_WRITE;
        if (re & (POLLERR | POLLHUP | POLLNVAL)) ev |= GS_NET_HUP;
        int64_t ref = p->fdref[f];
        if (ref >= 0) handles[k] = gs_net_handle(GS_NET_K_CONN, gs_net_self(), p->cns[ref].gen, ref);
        else handles[k] = p->watches[-1 - ref].handle;
        events[k++] = ev;
    }
    return k;
}

/* ---- handing a connection to another thread ----------------------------- */

/* Takes the connection out of the poller, its socket open: a token another
   thread's poller adopts. Until then it belongs to this thread program. */
GS_API int64_t gs_net_detach(int64_t ph, int64_t ch) {
    gs_net_pl *p = gs_net_pl_of(ph);
    gs_net_cn *c = p ? gs_net_cn_of(p, ch) : NULL;
    if (!c) return GS_NET_BAD_HANDLE;
    gs_sock s = c->sock;
    GS_NET_LOCK();
    int64_t i;
    GS_NET_TAKE(gs_net_dets, gs_net_ndets, gs_net_det, i);
    int64_t h = GS_NET_LIMIT;
    if (i >= 0) {
        gs_net_det *d = &gs_net_dets[i];
        d->sock = s;
        d->gen = gs_net_nextgen(d->gen);
        d->live = 1;
        d->owner = gs_net_self();
        h = gs_net_handle(GS_NET_K_DETACHED, 0, d->gen, i);
    }
    GS_NET_UNLOCK();
    if (h > 0) gs_net_dropcn(p, gs_net_index_of(ch), 0);
    return h;
}

/* The detached connection, into this thread's poller and waiting to read. */
GS_API int64_t gs_net_adopt(int64_t ph, int64_t dh) {
    gs_net_pl *p = gs_net_pl_of(ph);
    if (!p) return GS_NET_BAD_HANDLE;
    gs_sock s = GS_BAD_SOCK;
    GS_NET_LOCK();
    if (gs_net_kind(dh) == GS_NET_K_DETACHED && gs_net_index_of(dh) < gs_net_ndets) {
        gs_net_det *d = &gs_net_dets[gs_net_index_of(dh)];
        if (d->live && d->gen == gs_net_gen_of(dh)) {
            s = d->sock;
            d->live = 0;
            d->gen = gs_net_nextgen(d->gen);
        }
    }
    GS_NET_UNLOCK();
    if (s == GS_BAD_SOCK) return GS_NET_BAD_HANDLE;
    return gs_net_addcn(p, s);
}

/* ---- a thread program's end --------------------------------------------- */

/* Closes what the ending thread program still owns: its pollers with their
   connections, and its listeners and detached connections. */
#if GS_NEED_THREADS
static void gs_net_thread_end(void) {
    for (int64_t i = 0; i < gs_net_npls; i++)
        if (gs_net_pls[i].live) gs_net_free_pl(&gs_net_pls[i]);
    free(gs_net_pls);
    gs_net_pls = NULL;
    gs_net_npls = 0;
    if (!gs_net_used) return;
    int64_t self = gs_net_self();
    GS_NET_LOCK();
    for (int64_t i = 0; i < gs_net_nlsts; i++) {
        gs_net_lst *l = &gs_net_lsts[i];
        if (l->live && !l->closing && l->owner == self) {
            l->closing = 1;
            l->refs++;
            gs_net_lst_unref(l);
        }
    }
    for (int64_t i = 0; i < gs_net_ndets; i++) {
        gs_net_det *d = &gs_net_dets[i];
        if (d->live && d->owner == self) {
            gs_net_closesock(d->sock);
            d->live = 0;
            d->gen = gs_net_nextgen(d->gen);
        }
    }
    GS_NET_UNLOCK();
}
#endif

/* ---- stopping ----------------------------------------------------------- */

/* Written by a signal handler (POSIX) or the console's handler thread
   (Windows) and read by every thread: a lock-free atomic, since
   volatile sig_atomic_t is enough for a signal handler and its own thread
   but C11 makes it a data race between threads. */
#ifdef _WIN32
static volatile LONG gs_net_stop;
GS_API void gs_net_request_stop(void) { InterlockedExchange(&gs_net_stop, 1); }
GS_API uint8_t gs_net_stop_requested(void) { return InterlockedCompareExchange(&gs_net_stop, 0, 0) != 0; }
static BOOL WINAPI gs_net_on_console(DWORD type) {
    (void)type;
    gs_net_request_stop();
    return TRUE;
}
GS_API void gs_net_catch_signals(void) { SetConsoleCtrlHandler(gs_net_on_console, TRUE); }
#else
#include <signal.h>
static int gs_net_stop;
/* The generic builtins, which TinyCC has as gcc and clang do (its _n forms
   are macros of its stdatomic.h), in the order runtime_impl.h names for it. */
GS_API void gs_net_request_stop(void) {
    int one = 1;
    __atomic_store(&gs_net_stop, &one, __ATOMIC_SEQ_CST);
}
GS_API uint8_t gs_net_stop_requested(void) {
    int v;
    __atomic_load(&gs_net_stop, &v, __ATOMIC_SEQ_CST);
    return v != 0;
}
static void gs_net_on_signal(int sig) {
    (void)sig;
    gs_net_request_stop();
}
GS_API void gs_net_catch_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = gs_net_on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
#endif
