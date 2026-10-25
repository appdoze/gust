/* net.c -- see net.h. */
#include "net.h"

#include <signal.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define CLOSE_SOCK closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define CLOSE_SOCK close
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
#ifdef _WIN32
    WSAPOLLFD p = { (SOCKET)listener, POLLRDNORM, 0 };
    int r = WSAPoll(&p, 1, (INT)ms);
#else
    struct pollfd p = { (int)listener, POLLIN, 0 };
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
    if (c == BAD_SOCK) return -1;
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return (int64_t)c;
}

int64_t net_wait_readable(int64_t sock, int64_t ms) {
#ifdef _WIN32
    WSAPOLLFD p = { (SOCKET)sock, POLLRDNORM, 0 };
    int r = WSAPoll(&p, 1, (INT)ms);
#else
    struct pollfd p = { (int)sock, POLLIN, 0 };
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
    return n < 0 ? -1 : (int64_t)n;
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

void net_close(int64_t sock) { CLOSE_SOCK((sock_t)sock); }

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
