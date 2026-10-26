/* Modified for Gust (https://github.com/appdoze/gust); see MODIFICATIONS.md. */
/* Gust runtime: the network behind stdlib/net.goose. TCP over IPv4, every
   socket non-blocking, waited on with a poller: epoll on Linux, kqueue on
   macOS and the BSDs, AFD polling through a completion port on Windows,
   and poll or WSAPoll where none of those is, or GUST_NET_POLL is set (the
   readiness backend, below). runtime_ext.h declares what a program calls,
   which this defines; it is part of the runtime object, after
   runtime_os.h.

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
   when the last reference goes. A listener's tally of its connections
   still open is an atomic count each of them holds a place in, wherever
   it is, so workers sharing the listener can keep to one limit.

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
#if defined(__linux__)
#include <sys/epoll.h>
#define GS_NET_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
      defined(__DragonFly__)
#include <sys/event.h>
#define GS_NET_KQUEUE 1
#endif
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
    int (WSAAPI *WSAIoctl)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE);
} gs_ws;
#if defined(__clang__) || defined(__GNUC__)
#define GS_WS_CHECK(f) _Static_assert(__builtin_types_compatible_p(__typeof__(gs_ws.f), __typeof__(&f)), \
                                      "gs_ws." #f " does not match winsock2.h")
GS_WS_CHECK(WSAStartup); GS_WS_CHECK(WSAGetLastError); GS_WS_CHECK(socket); GS_WS_CHECK(bind);
GS_WS_CHECK(listen); GS_WS_CHECK(accept); GS_WS_CHECK(connect); GS_WS_CHECK(recv); GS_WS_CHECK(send);
GS_WS_CHECK(closesocket); GS_WS_CHECK(shutdown); GS_WS_CHECK(ioctlsocket); GS_WS_CHECK(setsockopt); GS_WS_CHECK(getsockopt);
GS_WS_CHECK(getsockname); GS_WS_CHECK(WSAPoll); GS_WS_CHECK(WSAIoctl);
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
    GS_WS_LOAD(getsockname); GS_WS_LOAD(WSAPoll); GS_WS_LOAD(WSAIoctl);
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

/* What the connections accepted from a listener count themselves in, on
   whatever thread they are, so that workers sharing it can hold their
   total to a limit: refs is the connections still open, plus one while
   the listener's socket is. The last to let it go frees it. */
typedef struct { int64_t refs; } gs_net_tally;
static int64_t gs_net_tally_add(gs_net_tally *t, int64_t d) {
#if defined(_WIN32) && !defined(__TINYC__)
    return InterlockedExchangeAdd64((volatile LONG64 *)&t->refs, d) + d;
#else
    /* TinyCC has the fetch-first form only, and on Windows neither the
       intrinsic nor the order's name. */
#ifndef __ATOMIC_SEQ_CST
#define __ATOMIC_SEQ_CST 5
#endif
    return __atomic_fetch_add(&t->refs, d, __ATOMIC_SEQ_CST) + d;
#endif
}
static void gs_net_tally_drop(gs_net_tally *t) {
    if (t && gs_net_tally_add(t, -1) == 0) free(t);
}
/* After this thread's last accept: the connections from that listener
   open, the new one among them. */
static GS_TLS int64_t gs_net_accepted_open;
GS_API int64_t gs_net_listener_open(void) { return gs_net_accepted_open; }

/* Listeners: shared. refs counts the accepts under way and the pollers
   watching it; a close marks it closing and the last reference closes the
   socket. */
typedef struct {
    gs_sock sock;
    uint32_t gen;
    uint8_t live, closing;
    int64_t owner, refs, port;
    gs_net_tally *tally;
} gs_net_lst;
static gs_net_lst *gs_net_lsts;
static int64_t gs_net_nlsts;

/* Detached connections: shared, between a detach and an adopt. */
typedef struct {
    gs_sock sock;
    uint32_t gen;
    uint8_t live;
    int64_t owner;
    gs_net_tally *tally;
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
/* Advanced whenever a listener is closed, so that a poller looks for closed
   listeners it watches only when one has closed since it last did. Written
   under the lock, read without it. */
#ifdef _WIN32
static volatile LONG gs_net_lst_epoch;   /* wraps; only equality is asked of it */
static int64_t gs_net_epoch_now(void) { return InterlockedCompareExchange(&gs_net_lst_epoch, 0, 0); }
static void gs_net_epoch_bump(void) { InterlockedExchange(&gs_net_lst_epoch, gs_net_lst_epoch + 1); }
#else
static int64_t gs_net_lst_epoch;
static int64_t gs_net_epoch_now(void) {
    int64_t v;
    __atomic_load(&gs_net_lst_epoch, &v, __ATOMIC_SEQ_CST);
    return v;
}
static void gs_net_epoch_bump(void) {
    int64_t v = gs_net_epoch_now() + 1;
    __atomic_store(&gs_net_lst_epoch, &v, __ATOMIC_SEQ_CST);
}
#endif

/* Drops a reference, closing the socket with the last. Under the lock. */
static void gs_net_lst_unref(gs_net_lst *l) {
    if (--l->refs == 0 && l->closing) {
        gs_net_closesock(l->sock);
        gs_net_tally_drop(l->tally);
        l->tally = NULL;
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
    gs_net_tally *tally;   /* its listener's, when accepted */
} gs_net_cn;
typedef struct {
    gs_sock sock;
    int64_t handle;        /* the listener */
    int32_t fd;
} gs_net_wt;
typedef struct {
    uint32_t gen;
    uint8_t live;
    int kfd;               /* its epoll or kqueue descriptor (Windows: 1 for AFD); -1: poll over fds */
    int64_t epoch;         /* gs_net_lst_epoch when it last looked for closed listeners */
#ifdef _WIN32
    HANDLE iocp, afd;      /* AFD polling: the completion port, and the AFD device */
    struct gs_net_ks **kst;        /* per registry entry: its poll's state */
    struct gs_net_ks **kq;         /* the states to submit or resubmit at the next wait */
    int64_t nkq, capkq, kpending;  /* ... and how many polls are outstanding */
#endif
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

/* ---- the readiness backend ---------------------------------------------- */

/* What a poller waits with: epoll on Linux, kqueue on macOS and the BSDs,
   else (Windows, or with GUST_NET_POLL set, for testing the portable path)
   poll or WSAPoll over its array. The array is the registry either way, an
   entry per socket waited on with what it waits for; epoll and kqueue
   mirror it in the kernel, which then reports only what is ready, where
   poll scans every entry on every wait, so a worker's wait costs what is
   ready, not what is open. Both kernels' sets are level-triggered, as poll
   is. An entry's kernel payload is a connection's index, or a watched
   listener's handle with the top bit set. */
#define GS_NET_WATCHBIT ((uint64_t)1 << 63)

static uint64_t gs_net_payload(gs_net_pl *p, int64_t ref) {
    return ref >= 0 ? (uint64_t)ref : GS_NET_WATCHBIT | (uint64_t)p->watches[-1 - ref].handle;
}

static int gs_net_force_poll(void) {
    const char *e = getenv("GUST_NET_POLL");
    return e && *e && strcmp(e, "0") != 0;
}

#ifdef _WIN32
/* Windows: AFD polling, as wepoll and Rust's mio do it. Each socket waited
   on has an IOCTL_AFD_POLL request outstanding on the AFD device, which
   completes through the poller's completion port when the socket is ready
   (or the request is cancelled). A request is one-shot: one that completed
   is submitted again at the next wait, which reports a socket still ready
   again, as poll would. The request's buffers belong to the kernel from
   submission to completion, so a state removed or changed meanwhile is
   cancelled and freed only when its completion arrives. The ntdll and
   kernel32 calls are looked up at first use; where any is missing, or the
   device does not open, the poller uses WSAPoll. */
typedef LONG gs_ntstatus;
typedef struct { ULONG_PTR status; ULONG_PTR information; } gs_iosb;   /* IO_STATUS_BLOCK */
typedef struct { USHORT len, maxlen; PWSTR buf; } gs_ustr;             /* UNICODE_STRING */
typedef struct {
    ULONG len;
    HANDLE root;
    gs_ustr *name;
    ULONG attributes;
    PVOID sd, sqos;
} gs_objattr;                                                          /* OBJECT_ATTRIBUTES */
typedef struct { HANDLE handle; ULONG events; gs_ntstatus status; } gs_afd_handle;
typedef struct {
    int64_t timeout;
    ULONG nhandles;
    ULONG exclusive;
    gs_afd_handle handles[1];
} gs_afd_poll;
typedef struct { ULONG_PTR key; LPOVERLAPPED ovl; ULONG_PTR internal; DWORD bytes; } gs_ovl_entry;
typedef gs_ntstatus (WINAPI *gs_NtCreateFile)(HANDLE *, ULONG, gs_objattr *, gs_iosb *, void *, ULONG, ULONG,
                                              ULONG, ULONG, void *, ULONG);
typedef gs_ntstatus (WINAPI *gs_NtDeviceIoControlFile)(HANDLE, HANDLE, void *, void *, gs_iosb *, ULONG, void *,
                                                       ULONG, void *, ULONG);
typedef gs_ntstatus (WINAPI *gs_NtCancelIoFileEx)(HANDLE, gs_iosb *, gs_iosb *);
typedef HANDLE (WINAPI *gs_CreateIoCompletionPort)(HANDLE, HANDLE, ULONG_PTR, DWORD);
typedef BOOL (WINAPI *gs_GetQueuedCompletionStatusEx)(HANDLE, gs_ovl_entry *, ULONG, ULONG *, DWORD, BOOL);
typedef BOOL (WINAPI *gs_SetFileCompletionNotificationModes)(HANDLE, UCHAR);
static struct {
    gs_NtCreateFile NtCreateFile;
    gs_NtDeviceIoControlFile NtDeviceIoControlFile;
    gs_NtCancelIoFileEx NtCancelIoFileEx;
    gs_CreateIoCompletionPort CreateIoCompletionPort;
    gs_GetQueuedCompletionStatusEx GetQueuedCompletionStatusEx;
    gs_SetFileCompletionNotificationModes SetFileCompletionNotificationModes;
} gs_afd;
static volatile LONG gs_afd_state;   /* 0 not tried, 1 ready, 2 unavailable */

#define GS_AFD_POLL_RECEIVE      0x0001
#define GS_AFD_POLL_SEND         0x0004
#define GS_AFD_POLL_DISCONNECT   0x0008
#define GS_AFD_POLL_ABORT        0x0010
#define GS_AFD_POLL_LOCAL_CLOSE  0x0020
#define GS_AFD_POLL_ACCEPT       0x0080
#define GS_AFD_POLL_CONNECT_FAIL 0x0100
#define GS_IOCTL_AFD_POLL        0x00012024
#define GS_STATUS_PENDING        ((gs_ntstatus)0x00000103L)
#define GS_STATUS_CANCELLED      ((gs_ntstatus)0xC0000120L)
#define GS_SIO_BASE_HANDLE       0x48000022

static int gs_afd_start(void) {
    LONG st = InterlockedCompareExchange(&gs_afd_state, 0, 0);
    if (st) return st == 1;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll"), k32 = GetModuleHandleW(L"kernel32.dll");
    int ok = nt && k32;
#define GS_AFD_LOAD(m, f) if (ok && !(*(FARPROC *)&gs_afd.f = GetProcAddress(m, #f))) ok = 0
    GS_AFD_LOAD(nt, NtCreateFile); GS_AFD_LOAD(nt, NtDeviceIoControlFile); GS_AFD_LOAD(nt, NtCancelIoFileEx);
    GS_AFD_LOAD(k32, CreateIoCompletionPort); GS_AFD_LOAD(k32, GetQueuedCompletionStatusEx);
    GS_AFD_LOAD(k32, SetFileCompletionNotificationModes);
#undef GS_AFD_LOAD
    InterlockedExchange(&gs_afd_state, ok ? 1 : 2);
    return ok;
}

/* A poll's state; the status block first, so that a completion's
   overlapped pointer is the state's address. */
struct gs_net_ks {
    gs_iosb iosb;
    gs_afd_poll info;
    HANDLE base;           /* the socket the AFD driver knows (SIO_BASE_HANDLE) */
    uint64_t payload;
    short want;            /* what is waited for */
    short pending;         /* what the outstanding request waits for; 0: none */
    uint8_t queued, deleted;
};

/* The poller's completion port and AFD device: false where there are none. */
static int gs_afd_open(gs_net_pl *p) {
    if (!gs_afd_start()) return 0;
    static WCHAR name[] = L"\\Device\\Afd\\Gust";
    gs_ustr us = { (USHORT)(sizeof name - sizeof(WCHAR)), (USHORT)sizeof name, name };
    gs_objattr attr = { sizeof attr, NULL, &us, 0, NULL, NULL };
    gs_iosb io;
    HANDLE afd = INVALID_HANDLE_VALUE;
    HANDLE iocp = gs_afd.CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!iocp) return 0;
    gs_ntstatus st = gs_afd.NtCreateFile(&afd, 0x00100000 /* SYNCHRONIZE */, &attr, &io, NULL, 0,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, 1 /* FILE_OPEN */, 0, NULL, 0);
    if (st != 0 || !gs_afd.CreateIoCompletionPort(afd, iocp, 0, 0) ||
        !gs_afd.SetFileCompletionNotificationModes(afd, 2 /* FILE_SKIP_SET_EVENT_ON_HANDLE */)) {
        if (st == 0) CloseHandle(afd);
        CloseHandle(iocp);
        return 0;
    }
    p->iocp = iocp;
    p->afd = afd;
    return 1;
}

static void gs_afd_queue(gs_net_pl *p, struct gs_net_ks *k) {
    if (k->queued) return;
    if (p->nkq == p->capkq) {
        int64_t cap = p->capkq ? p->capkq * 2 : 64;
        struct gs_net_ks **q = (struct gs_net_ks **)realloc(p->kq, (size_t)cap * sizeof *q);
        if (!q) return;   /* Not resubmitted: the socket waits until it changes. */
        p->kq = q;
        p->capkq = cap;
    }
    p->kq[p->nkq++] = k;
    k->queued = 1;
}

static void gs_afd_cancel(gs_net_pl *p, struct gs_net_ks *k) {
    gs_iosb io;
    if (k->pending) gs_afd.NtCancelIoFileEx(p->afd, &k->iosb, &io);
}

static void gs_afd_submit(gs_net_pl *p, struct gs_net_ks *k) {
    ULONG ev = GS_AFD_POLL_ABORT | GS_AFD_POLL_CONNECT_FAIL | GS_AFD_POLL_LOCAL_CLOSE;
    if (k->want & GS_POLL_IN) ev |= GS_AFD_POLL_RECEIVE | GS_AFD_POLL_ACCEPT | GS_AFD_POLL_DISCONNECT;
    if (k->want & GS_POLL_OUT) ev |= GS_AFD_POLL_SEND;
    k->info.timeout = INT64_MAX;
    k->info.nhandles = 1;
    k->info.exclusive = 0;
    k->info.handles[0].handle = k->base;
    k->info.handles[0].events = ev;
    k->info.handles[0].status = 0;
    k->iosb.status = (ULONG_PTR)(ULONG)GS_STATUS_PENDING;
    gs_ntstatus st = gs_afd.NtDeviceIoControlFile(p->afd, NULL, NULL, &k->iosb, &k->iosb, GS_IOCTL_AFD_POLL,
                                                  &k->info, sizeof k->info, &k->info, sizeof k->info);
    /* Pending or done at once, its completion is queued either way. */
    if (st == GS_STATUS_PENDING || st == 0) {
        k->pending = k->want;
        p->kpending++;
    }
}

static void gs_afd_free_if_done(struct gs_net_ks *k) {
    if (k->deleted && !k->pending && !k->queued) free(k);
}
#endif

/* Changes what the kernel set waits for on registry entry f, from old to
   now (0: not in it): false when the kernel refused. */
#if GS_NET_EPOLL
static int gs_net_kreg(gs_net_pl *p, int64_t f, short old, short now) {
    gs_sock s = p->fds[f].fd;
    int64_t ref = p->fdref[f];
    struct epoll_event e;
    memset(&e, 0, sizeof e);
    e.events = (uint32_t)(((now & GS_POLL_IN) ? EPOLLIN : 0) | ((now & GS_POLL_OUT) ? EPOLLOUT : 0));
    e.data.u64 = gs_net_payload(p, ref);
    int op = !old ? EPOLL_CTL_ADD : !now ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
    return epoll_ctl(p->kfd, op, s, &e) == 0;
}
#elif GS_NET_KQUEUE
static int gs_net_kreg(gs_net_pl *p, int64_t f, short old, short now) {
    gs_sock s = p->fds[f].fd;
    int64_t ref = p->fdref[f];
    struct kevent ch[2];
    int n = 0;
    void *data = (void *)(uintptr_t)gs_net_payload(p, ref);
    if ((old & GS_POLL_IN) != (now & GS_POLL_IN)) {
        EV_SET(&ch[n], (uintptr_t)s, EVFILT_READ, (now & GS_POLL_IN) ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0,
               data);
        n++;
    }
    if ((old & GS_POLL_OUT) != (now & GS_POLL_OUT)) {
        EV_SET(&ch[n], (uintptr_t)s, EVFILT_WRITE, (now & GS_POLL_OUT) ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0,
               data);
        n++;
    }
    return n == 0 || kevent(p->kfd, ch, n, NULL, 0, NULL) == 0;
}
#elif defined(_WIN32)
static int gs_net_kreg(gs_net_pl *p, int64_t f, short old, short now) {
    struct gs_net_ks *k = p->kst[f];
    if (!old) {
        k = (struct gs_net_ks *)calloc(1, sizeof *k);
        if (!k) return 0;
        DWORD got = 0;
        SOCKET base = INVALID_SOCKET;
        if (gs_ws.WSAIoctl(p->fds[f].fd, GS_SIO_BASE_HANDLE, NULL, 0, &base, sizeof base, &got, NULL, NULL) != 0)
            base = p->fds[f].fd;
        k->base = (HANDLE)base;
        k->payload = gs_net_payload(p, p->fdref[f]);
        k->want = now;
        p->kst[f] = k;
        gs_afd_queue(p, k);
        return 1;
    }
    if (!k) return 1;
    if (!now) {
        p->kst[f] = NULL;
        k->deleted = 1;
        gs_afd_cancel(p, k);
        gs_afd_free_if_done(k);
        return 1;
    }
    k->want = now;
    if (k->pending && k->pending != now) gs_afd_cancel(p, k);
    else if (!k->pending) gs_afd_queue(p, k);
    return 1;
}
#else
static int gs_net_kreg(gs_net_pl *p, int64_t f, short old, short now) {
    (void)p; (void)f; (void)old; (void)now;
    return 1;
}
#endif

/* A pollfd entry for sock, refering back to ref; -1 when memory ran out or
   the kernel set refused it. */
static int64_t gs_net_addfd(gs_net_pl *p, gs_sock sock, short events, int64_t ref) {
    if (p->nfds == p->capfds) {
        int64_t cap = p->capfds ? p->capfds * 2 : 64;
        gs_pollfd *fds = (gs_pollfd *)realloc(p->fds, (size_t)cap * sizeof *fds);
        if (!fds) return -1;
        p->fds = fds;
        int64_t *refs = (int64_t *)realloc(p->fdref, (size_t)cap * sizeof *refs);
        if (!refs) return -1;
        p->fdref = refs;
#ifdef _WIN32
        struct gs_net_ks **ks = (struct gs_net_ks **)realloc(p->kst, (size_t)cap * sizeof *ks);
        if (!ks) return -1;
        p->kst = ks;
#endif
        p->capfds = cap;
    }
    int64_t f = p->nfds++;
    p->fds[f].fd = sock;
    p->fds[f].events = events;
    p->fds[f].revents = 0;
    p->fdref[f] = ref;
#ifdef _WIN32
    p->kst[f] = NULL;
#endif
    if (p->kfd >= 0 && !gs_net_kreg(p, f, 0, events)) {
        p->nfds--;
        return -1;
    }
    return f;
}
/* What entry f waits for, from now on: false when the kernel set refused. */
static int gs_net_setfd(gs_net_pl *p, int64_t f, short events) {
    /* The loop asks again for what it already waits for after most
       requests: no system call for that. */
    if (p->fds[f].events == events) return 1;
    if (p->kfd >= 0 && !gs_net_kreg(p, f, p->fds[f].events, events)) return 0;
    p->fds[f].events = events;
    return 1;
}
/* Removes entry f, moving the last one into its place. */
static void gs_net_dropfd(gs_net_pl *p, int64_t f) {
    if (p->kfd >= 0) gs_net_kreg(p, f, p->fds[f].events, 0);
    int64_t last = --p->nfds;
    if (f != last) {
        p->fds[f] = p->fds[last];
        p->fdref[f] = p->fdref[last];
#ifdef _WIN32
        p->kst[f] = p->kst[last];
#endif
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
    c->tally = NULL;
    return gs_net_handle(GS_NET_K_CONN, gs_net_self(), c->gen, i);
}
/* Takes connection i out of the poller: its socket closed, or not. */
static void gs_net_dropcn(gs_net_pl *p, int64_t i, int closesock) {
    gs_net_cn *c = &p->cns[i];
    /* Out of the kernel set first, while the descriptor is still this one. */
    if (c->fd >= 0) gs_net_dropfd(p, c->fd);   /* -1: waiting for nothing */
    if (closesock) gs_net_closesock(c->sock);
    gs_net_tally_drop(c->tally);
    c->tally = NULL;
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
    gs_net_tally *t = i >= 0 ? (gs_net_tally *)malloc(sizeof *t) : NULL;
    if (t) {
        t->refs = 1;
        gs_net_lst *l = &gs_net_lsts[i];
        l->tally = t;
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
        gs_net_epoch_bump();
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
    p->kfd = -1;
#if GS_NET_EPOLL
    if (!gs_net_force_poll()) p->kfd = epoll_create1(EPOLL_CLOEXEC);
#elif GS_NET_KQUEUE
    if (!gs_net_force_poll()) {
        p->kfd = kqueue();
        if (p->kfd >= 0) fcntl(p->kfd, F_SETFD, FD_CLOEXEC);
    }
#elif defined(_WIN32)
    if (!gs_net_force_poll() && gs_afd_open(p)) p->kfd = 1;
#endif
    /* A kernel that refuses a set leaves the poller on poll. */
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

#ifdef _WIN32
/* The poller's AFD polls ended: every outstanding one cancelled and its
   completion awaited, for the kernel writes into its state until then
   (a second at most; past that the states are left, not freed), then the
   states, the device and the port. */
static void gs_afd_close(gs_net_pl *p) {
    for (int64_t f = 0; f < p->nfds; f++) {
        struct gs_net_ks *k = p->kst[f];
        if (!k) continue;
        p->kst[f] = NULL;
        k->deleted = 1;
        gs_afd_cancel(p, k);
    }
    for (int64_t q = 0; q < p->nkq; q++) p->kq[q]->queued = 0;
    for (int64_t q = 0; q < p->nkq; q++) {
        struct gs_net_ks *k = p->kq[q];
        k->deleted = 1;
        gs_afd_free_if_done(k);
    }
    p->nkq = 0;
    DWORD start = GetTickCount();
    while (p->kpending > 0 && GetTickCount() - start < 1000) {
        gs_ovl_entry es[64];
        ULONG n = 0;
        if (!gs_afd.GetQueuedCompletionStatusEx(p->iocp, es, 64, &n, 100, FALSE)) continue;
        for (ULONG j = 0; j < n; j++) {
            struct gs_net_ks *k = (struct gs_net_ks *)es[j].ovl;
            k->pending = 0;
            p->kpending--;
            k->deleted = 1;
            gs_afd_free_if_done(k);
        }
    }
    free(p->kq);
    CloseHandle(p->afd);
    CloseHandle(p->iocp);
}
#endif

/* Closes the poller's connections and frees it. */
static void gs_net_free_pl(gs_net_pl *p) {
    while (p->nwatches) gs_net_unwatch_at(p, p->nwatches - 1);
    for (int64_t i = 0; i < p->ncns; i++)
        if (p->cns[i].live) {
            gs_net_closesock(p->cns[i].sock);
            gs_net_tally_drop(p->cns[i].tally);
        }
#ifndef _WIN32
    if (p->kfd >= 0) close(p->kfd);
#else
    if (p->kfd >= 0) gs_afd_close(p);
    free(p->kst);
#endif
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
    /* Its handle first: it is the kernel set's payload for the listener. */
    p->watches[w].sock = s;
    p->watches[w].handle = lh;
    int64_t f = gs_net_addfd(p, s, GS_POLL_IN, -1 - w);
    if (f < 0) {
        GS_NET_LOCK();
        gs_net_lst_unref(l);
        GS_NET_UNLOCK();
        return GS_NET_LIMIT;
    }
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
    gs_net_tally *t = NULL;
    if (l) {
        l->refs++;
        ls = l->sock;
        t = l->tally;
    }
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
    /* The connection's place in the tally, while the listener, and so the
       tally, is held; only a connection counts, so that another worker's
       accept that finds none never makes the total look larger. */
    if (s != GS_BAD_SOCK) gs_net_tally_add(t, 1);
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
    if (!gs_net_nonblock(s)) {
        gs_net_noteerr();
        gs_net_closesock(s);
        gs_net_tally_drop(t);
        return GS_NET_ERROR;
    }
    gs_net_tune(s);
    int64_t h = gs_net_addcn(p, s);
    if (h < 0) { gs_net_tally_drop(t); return h; }
    p->cns[gs_net_index_of(h)].tally = t;
    /* Another thread's connection may close in between: a count. */
    gs_net_accepted_open = gs_net_tally_add(t, 0) - 1;
    return h;
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
    } else if (!gs_net_setfd(p, c->fd, e)) {
        gs_net_noteerr();
        return GS_NET_ERROR;
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

/* The handle a kernel payload stands for (0: a connection no longer here). */
static int64_t gs_net_unpayload(gs_net_pl *p, uint64_t data) {
    if (data & GS_NET_WATCHBIT) return (int64_t)(data & ~GS_NET_WATCHBIT);
    int64_t i = (int64_t)data;
    if (i >= p->ncns || !p->cns[i].live) return 0;
    return gs_net_handle(GS_NET_K_CONN, gs_net_self(), p->cns[i].gen, i);
}

/* Up to cap ready sockets from the kernel set, as gs_net_wait reports them. */
static int64_t gs_net_kwait(gs_net_pl *p, int64_t ms, int64_t *handles, int64_t *events, int64_t cap) {
    int64_t k = 0;
#if GS_NET_EPOLL
    struct epoll_event evs[256];
    int n = epoll_wait(p->kfd, evs, (int)(cap < 256 ? cap : 256),
                       ms < 0 ? -1 : ms > 0x7fffffff ? 0x7fffffff : (int)ms);
    if (n < 0) {
        gs_net_noteerr();
        return errno == EINTR ? 0 : GS_NET_ERROR;
    }
    for (int j = 0; j < n; j++) {
        int64_t h = gs_net_unpayload(p, evs[j].data.u64);
        if (!h) continue;
        int64_t ev = 0;
        if (evs[j].events & EPOLLIN) ev |= GS_NET_READ;
        if (evs[j].events & EPOLLOUT) ev |= GS_NET_WRITE;
        if (evs[j].events & (EPOLLERR | EPOLLHUP)) ev |= GS_NET_HUP;
        handles[k] = h;
        events[k++] = ev;
    }
#elif GS_NET_KQUEUE
    struct kevent evs[256];
    struct timespec ts, *tsp = NULL;
    if (ms >= 0) {
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)(ms % 1000) * 1000000;
        tsp = &ts;
    }
    int n = kevent(p->kfd, NULL, 0, evs, (int)(cap < 256 ? cap : 256), tsp);
    if (n < 0) {
        gs_net_noteerr();
        return errno == EINTR ? 0 : GS_NET_ERROR;
    }
    for (int j = 0; j < n; j++) {
        int64_t h = gs_net_unpayload(p, (uint64_t)(uintptr_t)evs[j].udata);
        if (!h) continue;
        int64_t ev = evs[j].filter == EVFILT_WRITE ? GS_NET_WRITE : GS_NET_READ;
        if (evs[j].flags & (EV_EOF | EV_ERROR)) ev |= GS_NET_HUP;
        /* A socket ready both ways is two kernel events, and one report. */
        int64_t m = 0;
        while (m < k && handles[m] != h) m++;
        if (m < k) events[m] |= ev;
        else {
            handles[k] = h;
            events[k++] = ev;
        }
    }
#elif defined(_WIN32)
    /* A completion that reports nothing (a request cancelled for a change of
       what is waited for) is waited past, until ms are up, so that a wait
       returns 0 only at its timeout, as poll does. */
    DWORD start = GetTickCount();
    for (;;) {
    /* What completed or changed since the last wait goes back to the kernel. */
    int64_t nq = p->nkq;
    p->nkq = 0;
    for (int64_t q = 0; q < nq; q++) {
        struct gs_net_ks *s = p->kq[q];
        s->queued = 0;
        if (s->deleted) {
            gs_afd_free_if_done(s);
            continue;
        }
        if (!s->pending && s->want) gs_afd_submit(p, s);
    }
    gs_ovl_entry es[256];
    ULONG n = 0;
    DWORD spent = GetTickCount() - start;
    DWORD left = ms < 0 ? INFINITE : (int64_t)spent >= ms ? 0 : ms - spent > 0x7ffffffe ? 0x7ffffffe : (DWORD)(ms - spent);
    if (!gs_afd.GetQueuedCompletionStatusEx(p->iocp, es, (ULONG)(cap < 256 ? cap : 256), &n, left, FALSE)) {
        if (GetLastError() == WAIT_TIMEOUT) return 0;
        gs_net_oserr = (int64_t)GetLastError();
        return GS_NET_ERROR;
    }
    for (ULONG j = 0; j < n; j++) {
        struct gs_net_ks *s = (struct gs_net_ks *)es[j].ovl;
        s->pending = 0;
        p->kpending--;
        if (s->deleted) {
            gs_afd_free_if_done(s);
            continue;
        }
        /* Waited for again at the next wait, whatever this one says. */
        gs_afd_queue(p, s);
        gs_ntstatus st = (gs_ntstatus)(ULONG)s->iosb.status;
        if (st == GS_STATUS_CANCELLED) continue;
        ULONG got = st == 0 ? s->info.handles[0].events : GS_AFD_POLL_ABORT;
        if (got & GS_AFD_POLL_LOCAL_CLOSE) continue;
        int64_t want = ((s->want & GS_POLL_IN) ? GS_NET_READ : 0) | ((s->want & GS_POLL_OUT) ? GS_NET_WRITE : 0);
        int64_t ev = 0;
        if (got & (GS_AFD_POLL_RECEIVE | GS_AFD_POLL_ACCEPT | GS_AFD_POLL_DISCONNECT)) ev |= GS_NET_READ;
        if (got & GS_AFD_POLL_SEND) ev |= GS_NET_WRITE;
        if (got & (GS_AFD_POLL_ABORT | GS_AFD_POLL_CONNECT_FAIL)) ev |= GS_NET_HUP | want;
        /* A report for what is no longer waited for is dropped. */
        ev &= GS_NET_HUP | want;
        if (!ev) continue;
        int64_t h = gs_net_unpayload(p, s->payload);
        if (!h) continue;
        handles[k] = h;
        events[k++] = ev;
    }
    if (k || (ms >= 0 && (int64_t)(GetTickCount() - start) >= ms)) break;
    }
#else
    (void)p; (void)ms; (void)handles; (void)events; (void)cap;
#endif
    return k;
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
    /* Listeners closed under the watch first: looked for only when one has
       closed since, which the epoch says without the lock. */
    int64_t epoch = gs_net_epoch_now();
    if (p->nwatches && p->epoch != epoch) {
        int64_t w = 0;
        while (w < p->nwatches && k < cap) {
            GS_NET_LOCK();
            int closed = gs_net_lst_of(p->watches[w].handle) == NULL;
            GS_NET_UNLOCK();
            if (closed) {
                handles[k] = p->watches[w].handle;
                events[k++] = GS_NET_HUP;
                gs_net_unwatch_at(p, w);
            } else w++;
        }
        if (w >= p->nwatches) p->epoch = epoch;
    }
    if (k) return k;
    if (p->kfd >= 0) return gs_net_kwait(p, ms, handles, events, cap);
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
        d->tally = c->tally;   /* it goes with the connection */
        c->tally = NULL;
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
    gs_net_tally *t = NULL;
    GS_NET_LOCK();
    if (gs_net_kind(dh) == GS_NET_K_DETACHED && gs_net_index_of(dh) < gs_net_ndets) {
        gs_net_det *d = &gs_net_dets[gs_net_index_of(dh)];
        if (d->live && d->gen == gs_net_gen_of(dh)) {
            s = d->sock;
            t = d->tally;
            d->tally = NULL;
            d->live = 0;
            d->gen = gs_net_nextgen(d->gen);
        }
    }
    GS_NET_UNLOCK();
    if (s == GS_BAD_SOCK) return GS_NET_BAD_HANDLE;
    int64_t h = gs_net_addcn(p, s);
    if (h < 0) gs_net_tally_drop(t);
    else p->cns[gs_net_index_of(h)].tally = t;
    return h;
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
            gs_net_epoch_bump();
            l->closing = 1;
            l->refs++;
            gs_net_lst_unref(l);
        }
    }
    for (int64_t i = 0; i < gs_net_ndets; i++) {
        gs_net_det *d = &gs_net_dets[i];
        if (d->live && d->owner == self) {
            gs_net_closesock(d->sock);
            gs_net_tally_drop(d->tally);
            d->tally = NULL;
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
