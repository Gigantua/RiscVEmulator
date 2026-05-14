// slirp_bridge.cpp — C++ shim that owns a libslirp instance and exposes a
// 5-function C ABI to the .NET host. Built as slirp_bridge.dll alongside
// rv32i_core.dll. At runtime it LoadLibrary's slirp.dll (the mingw build
// from msys2's mingw-w64-libslirp package) and resolves its API by name —
// no .lib import file needed, and no .NET marshaling pain.
//
// API (called from C# via simple [DllImport]s):
//   int  sb_init(void);                            // -> 0 on success
//   void sb_tx(const uint8_t* buf, size_t len);    // guest -> slirp
//   int  sb_rx(uint8_t* buf, size_t max_len);      // slirp -> guest (returns frame len, 0 if none)
//   void sb_get_mac(uint8_t mac[6]);
//   void sb_cleanup(void);
//
// Topology: guest 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3. libslirp's
// internal DHCP, DNS proxy, and TCP/UDP/ICMP NAT are all in play — same
// model QEMU uses with "-net user".
//
// Critical correctness notes (paid for in segfaults):
//   1. libslirp keeps a POINTER to the SlirpCb struct, not a copy. Storage
//      must outlive the Slirp instance — see the static `cb` in sb_init.
//   2. SlirpConfig field order must match libslirp.h exactly. `restricted`
//      comes BEFORE `in_enabled` — swapping silently disables IPv4.
//   3. libslirp's poll bits are NOT GIOCondition: IN=1, OUT=2, PRI=4
//      (glib has OUT=4, PRI=2 — easy to get wrong; TCP breaks).
//   4. libslirp 4.9.1 crashes on slirp_pollfds_poll with zero fds. We skip
//      the call when count==0.
//   5. libslirp is single-threaded. All slirp_* calls go through slirp_lock.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

// winsock2.h defines s_addr/s6_addr as macros (-> S_un.S_addr etc), which
// poison any struct field by that name. Use ip4 / ip6 in our local mirror
// of libslirp's SlirpInAddr / SlirpIn6Addr — same memory layout, no
// preprocessor conflict.

// ── libslirp types (subset; matches libslirp.h SlirpConfig version 1) ──

typedef struct Slirp Slirp;

typedef int64_t       ssize_t_compat;
typedef ssize_t_compat (*send_packet_cb)(const void* buf, size_t len, void* opaque);
typedef void          (*guest_error_cb)(const char* msg, void* opaque);
typedef int64_t       (*clock_get_ns_cb)(void* opaque);
typedef void          (*timer_cb)(void* opaque);
typedef void*         (*timer_new_cb)(timer_cb cb, void* cb_opaque, void* opaque);
typedef void          (*timer_free_cb)(void* timer, void* opaque);
typedef void          (*timer_mod_cb)(void* timer, int64_t expire_time_ms, void* opaque);
typedef void          (*register_poll_fd_cb)(int fd, void* opaque);
typedef void          (*unregister_poll_fd_cb)(int fd, void* opaque);
typedef void          (*notify_cb)(void* opaque);
typedef int           (*add_poll_cb)(int fd, int events, void* opaque);
typedef int           (*get_revents_cb)(int idx, void* opaque);

struct SlirpCb {
    send_packet_cb       send_packet;
    guest_error_cb       guest_error;
    clock_get_ns_cb      clock_get_ns;
    timer_new_cb         timer_new;
    timer_free_cb        timer_free;
    timer_mod_cb         timer_mod;
    register_poll_fd_cb  register_poll_fd;
    unregister_poll_fd_cb unregister_poll_fd;
    notify_cb            notify;
    // v2+ slots — leave NULL for v1.
    void* init_completed;
    void* timer_new_opaque;
};

struct SlirpInAddr  { uint32_t ip4; };
// Windows in6_addr is a union { UCHAR[16]; USHORT[8]; } so it has 2-byte
// alignment. Match exactly — otherwise vprefix_len, vhost6, vhostname and
// every field after the first in6_addr in SlirpConfig are off by 1 byte
// (libslirp ends up reading garbage as vhostname → crash).
struct SlirpIn6Addr { union { uint8_t b[16]; uint16_t w[8]; } u; };

struct SlirpConfig {
    // Field order MUST match libslirp.h SlirpConfig — restricted comes BEFORE
    // in_enabled. Reversing these silently disables IPv4 (DHCP DISCOVERs go
    // unanswered) since our true/false get applied to the wrong fields.
    uint32_t        version;
    int             restricted;
    bool            in_enabled;
    SlirpInAddr     vnetwork;
    SlirpInAddr     vnetmask;
    SlirpInAddr     vhost;
    bool            in6_enabled;
    SlirpIn6Addr    vprefix_addr6;
    uint8_t         vprefix_len;
    SlirpIn6Addr    vhost6;
    const char*     vhostname;
    const char*     tftp_server_name;
    const char*     tftp_path;
    const char*     bootfile;
    SlirpInAddr     vdhcp_start;
    SlirpInAddr     vnameserver;
    SlirpIn6Addr    vnameserver6;
    const char**    vdnssearch;
    const char*     vdomainname;
    size_t          if_mtu;
    size_t          if_mru;
    bool            disable_host_loopback;
    bool            enable_emu;
};

typedef Slirp* (__cdecl *slirp_new_fn)(const SlirpConfig*, const SlirpCb*, void*);
typedef Slirp* (__cdecl *slirp_init_fn)(int, bool,
    SlirpInAddr, SlirpInAddr, SlirpInAddr,
    bool, SlirpIn6Addr, uint8_t, SlirpIn6Addr,
    const char*, const char*, const char*, const char*,
    SlirpInAddr, SlirpInAddr, SlirpIn6Addr,
    const char**, const char*,
    const SlirpCb*, void*);
typedef void   (__cdecl *slirp_cleanup_fn)(Slirp*);
typedef void   (__cdecl *slirp_input_fn)(Slirp*, const uint8_t*, int);
typedef void   (__cdecl *slirp_pollfds_fill_fn)(Slirp*, uint32_t*, add_poll_cb, void*);
typedef void   (__cdecl *slirp_pollfds_poll_fn)(Slirp*, int, get_revents_cb, void*);

static slirp_new_fn          p_slirp_new;
static slirp_init_fn         p_slirp_init;
static slirp_cleanup_fn      p_slirp_cleanup;
static slirp_input_fn        p_slirp_input;
static slirp_pollfds_fill_fn p_slirp_pollfds_fill;
static slirp_pollfds_poll_fn p_slirp_pollfds_poll;

// ── RX queue (slirp → guest) ────────────────────────────────────────────

#define RX_QUEUE_CAP 256
struct RxEntry { uint8_t* data; int len; };
static RxEntry         rx_queue[RX_QUEUE_CAP];
static int             rx_head = 0, rx_tail = 0;
static CRITICAL_SECTION rx_lock;
static HANDLE          rx_event;

// ── Timer list ──────────────────────────────────────────────────────────

#define MAX_TIMERS 64
struct TimerEntry { timer_cb cb; void* opaque; int64_t expire_ms; bool used; };
static TimerEntry      timers[MAX_TIMERS];
static CRITICAL_SECTION timer_lock;

// ── Slirp instance + poll thread ────────────────────────────────────────

static Slirp*  g_slirp;
static HANDLE  g_thread;
static volatile LONG g_running;
static CRITICAL_SECTION slirp_lock;  // libslirp is NOT thread-safe — serialize ALL slirp_* calls.
static const uint8_t g_guest_mac[6] = { 0x02, 0x54, 0x00, 0x12, 0x34, 0x56 };
static volatile LONG s_tx_count, s_rx_count;

// ── Helpers ─────────────────────────────────────────────────────────────

static int64_t now_ns() {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (int64_t)((ctr.QuadPart * 1000000000LL) / freq.QuadPart);
}

// ── libslirp callbacks ──────────────────────────────────────────────────

static int64_t cb_send_packet(const void* buf, size_t len, void* opaque) {
    InterlockedIncrement(&s_rx_count);
    EnterCriticalSection(&rx_lock);
    int next = (rx_tail + 1) % RX_QUEUE_CAP;
    if (next != rx_head) {
        uint8_t* p = (uint8_t*)malloc(len);
        if (p) {
            memcpy(p, buf, len);
            rx_queue[rx_tail].data = p;
            rx_queue[rx_tail].len  = (int)len;
            rx_tail = next;
            SetEvent(rx_event);
        }
    }
    LeaveCriticalSection(&rx_lock);
    return (int64_t)len;
}

static void cb_guest_error(const char* msg, void* opaque) {
    fprintf(stderr, "[slirp_bridge] %s\n", msg ? msg : "(null)");
}

static int64_t cb_clock_get_ns(void* opaque) { return now_ns(); }

static void* cb_timer_new(timer_cb cb, void* cb_opaque, void* opaque) {
    EnterCriticalSection(&timer_lock);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].used) {
            timers[i].cb        = cb;
            timers[i].opaque    = cb_opaque;
            timers[i].expire_ms = INT64_MAX;
            timers[i].used      = true;
            LeaveCriticalSection(&timer_lock);
            return &timers[i];
        }
    }
    LeaveCriticalSection(&timer_lock);
    return NULL;
}

static void cb_timer_free(void* timer, void* opaque) {
    TimerEntry* t = (TimerEntry*)timer;
    EnterCriticalSection(&timer_lock);
    t->used = false;
    LeaveCriticalSection(&timer_lock);
}

static void cb_timer_mod(void* timer, int64_t expire_time_ms, void* opaque) {
    TimerEntry* t = (TimerEntry*)timer;
    EnterCriticalSection(&timer_lock);
    t->expire_ms = expire_time_ms;
    LeaveCriticalSection(&timer_lock);
}

static void cb_register_poll_fd(int fd, void* opaque)   {}
static void cb_unregister_poll_fd(int fd, void* opaque) {}
static void cb_notify(void* opaque)                     {}

// ── Poll loop: collects libslirp's host fds, select()s, drains ──────────

#define MAX_POLL_FDS 64
struct PollFd { int fd; int events; int revents; };
struct PollState {
    PollFd fds[MAX_POLL_FDS];
    int    count;
};

static int cb_add_poll(int fd, int events, void* opaque) {
    PollState* st = (PollState*)opaque;
    if (st->count >= MAX_POLL_FDS) return -1;
    st->fds[st->count].fd      = fd;
    st->fds[st->count].events  = events;
    st->fds[st->count].revents = 0;
    return st->count++;
}

static int cb_get_revents(int idx, void* opaque) {
    PollState* st = (PollState*)opaque;
    if (idx < 0 || idx >= st->count) return 0;
    return st->fds[idx].revents;
}

// libslirp poll flags (from libslirp.h, NOT GIOCondition):
//   SLIRP_POLL_IN  = 1 << 0 = 1
//   SLIRP_POLL_OUT = 1 << 1 = 2
//   SLIRP_POLL_PRI = 1 << 2 = 4
//   SLIRP_POLL_ERR = 1 << 3 = 8
//   SLIRP_POLL_HUP = 1 << 4 = 16
// (GIOCondition has OUT=4 and PRI=2 — DIFFERENT. Earlier code copied the
// glib values and broke TCP because writable revents got reported as PRI.)
static const int SLIRP_IN  = 1;
static const int SLIRP_OUT = 2;
static const int SLIRP_PRI = 4;

static int run_select(PollState* st, int timeout_ms) {
    if (st->count == 0) return 0;
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
    for (int i = 0; i < st->count; i++) {
        SOCKET s = (SOCKET)st->fds[i].fd;
        int ev = st->fds[i].events;
        if (ev & SLIRP_IN)  FD_SET(s, &rd);
        if (ev & SLIRP_OUT) FD_SET(s, &wr);
        if (ev & SLIRP_PRI) FD_SET(s, &ex);
    }
    TIMEVAL tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int rc = select(0, &rd, &wr, &ex, &tv);
    if (rc < 0) return rc;
    for (int i = 0; i < st->count; i++) {
        SOCKET s = (SOCKET)st->fds[i].fd;
        int re = 0;
        if (FD_ISSET(s, &rd)) re |= SLIRP_IN;
        if (FD_ISSET(s, &wr)) re |= SLIRP_OUT;
        if (FD_ISSET(s, &ex)) re |= SLIRP_PRI;
        st->fds[i].revents = re;
    }
    return 0;
}

static DWORD WINAPI poll_thread_func(LPVOID arg) {
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        PollState st = { 0 };
        uint32_t timeout = 10;

        EnterCriticalSection(&slirp_lock);
        p_slirp_pollfds_fill(g_slirp, &timeout, cb_add_poll, &st);
        LeaveCriticalSection(&slirp_lock);

        int err = run_select(&st, (int)timeout);
        if (st.count == 0) Sleep((DWORD)timeout);

        // Skip pollfds_poll when there are no fds — some libslirp builds
        // crash on the empty case (and there's nothing to do anyway).
        if (st.count > 0) {
            EnterCriticalSection(&slirp_lock);
            p_slirp_pollfds_poll(g_slirp, err < 0 ? 1 : 0, cb_get_revents, &st);
            LeaveCriticalSection(&slirp_lock);
        }

        // Fire any expired timers. Timer callbacks themselves call into slirp,
        // so the timer fire path needs slirp_lock — but timer_lock must be
        // released first to avoid deadlock with cb_timer_mod (which holds
        // timer_lock and can be called from inside slirp_input under slirp_lock).
        int64_t now_ms = now_ns() / 1000000;
        for (int i = 0; i < MAX_TIMERS; i++) {
            timer_cb cb = NULL;
            void* op    = NULL;
            EnterCriticalSection(&timer_lock);
            if (timers[i].used && timers[i].expire_ms <= now_ms) {
                cb = timers[i].cb;
                op = timers[i].opaque;
                timers[i].expire_ms = INT64_MAX;
            }
            LeaveCriticalSection(&timer_lock);
            if (cb) {
                EnterCriticalSection(&slirp_lock);
                cb(op);
                LeaveCriticalSection(&slirp_lock);
            }
        }
    }
    return 0;
}

// ── Public C ABI ────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) int sb_init() {
    // Bind libslirp at runtime so MSVC doesn't need a mingw .lib import file.
    // Use LoadLibraryEx + LOAD_WITH_ALTERED_SEARCH_PATH so the loader looks for
    // slirp.dll's own deps (libglib, libintl, etc) in slirp.dll's own folder.
    char self_dir[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&sb_init, &self);
    GetModuleFileNameA(self, self_dir, MAX_PATH);
    char* lastSep = strrchr(self_dir, '\\');
    if (lastSep) *lastSep = '\0';
    char slirp_path[MAX_PATH];
    snprintf(slirp_path, MAX_PATH, "%s\\slirp.dll", self_dir);

    HMODULE slirp_dll = LoadLibraryExA(slirp_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!slirp_dll) {
        fprintf(stderr, "[slirp_bridge] LoadLibraryEx '%s' failed: %lu\n",
                slirp_path, GetLastError());
        return -1;
    }
    typedef const char* (__cdecl *slirp_version_fn)(void);
    slirp_version_fn p_ver = (slirp_version_fn)GetProcAddress(slirp_dll, "slirp_version_string");
    if (p_ver) fprintf(stderr, "[slirp_bridge] libslirp %s\n", p_ver());

    p_slirp_new          = (slirp_new_fn)         GetProcAddress(slirp_dll, "slirp_new");
    p_slirp_init         = (slirp_init_fn)        GetProcAddress(slirp_dll, "slirp_init");
    p_slirp_cleanup      = (slirp_cleanup_fn)     GetProcAddress(slirp_dll, "slirp_cleanup");
    p_slirp_input        = (slirp_input_fn)       GetProcAddress(slirp_dll, "slirp_input");
    p_slirp_pollfds_fill = (slirp_pollfds_fill_fn)GetProcAddress(slirp_dll, "slirp_pollfds_fill");
    p_slirp_pollfds_poll = (slirp_pollfds_poll_fn)GetProcAddress(slirp_dll, "slirp_pollfds_poll");
    if (!p_slirp_new || !p_slirp_cleanup || !p_slirp_input || !p_slirp_pollfds_fill || !p_slirp_pollfds_poll) {
        fprintf(stderr, "[slirp_bridge] Missing libslirp symbol\n");
        return -2;
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    InitializeCriticalSection(&rx_lock);
    InitializeCriticalSection(&timer_lock);
    InitializeCriticalSection(&slirp_lock);
    rx_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    SlirpConfig cfg = {};
    cfg.version    = 1;
    cfg.restricted = 0;
    cfg.in_enabled = true;
    cfg.vnetwork.ip4    = htonl(0x0A000200);   // 10.0.2.0
    cfg.vnetmask.ip4    = htonl(0xFFFFFF00);   // 255.255.255.0
    cfg.vhost.ip4       = htonl(0x0A000202);   // 10.0.2.2
    cfg.in6_enabled     = false;
    cfg.vhostname       = NULL;                 // omit DHCP hostname option
    cfg.vdhcp_start.ip4 = htonl(0x0A00020F);   // 10.0.2.15
    cfg.vnameserver.ip4 = htonl(0x0A000203);   // 10.0.2.3
    cfg.if_mtu = 0;  // 0 = use IF_MTU_DEFAULT
    cfg.if_mru = 0;

    // libslirp keeps a pointer to the SlirpCb (doesn't copy it). The struct
    // MUST outlive g_slirp, so make it static — a stack-local cb would be
    // freed when sb_init returns and the first DHCP request would crash.
    static SlirpCb cb = {};
    cb.send_packet        = cb_send_packet;
    cb.guest_error        = cb_guest_error;
    cb.clock_get_ns       = cb_clock_get_ns;
    cb.timer_new          = cb_timer_new;
    cb.timer_free         = cb_timer_free;
    cb.timer_mod          = cb_timer_mod;
    cb.register_poll_fd   = cb_register_poll_fd;
    cb.unregister_poll_fd = cb_unregister_poll_fd;
    cb.notify             = cb_notify;

    g_slirp = p_slirp_new(&cfg, &cb, NULL);
    if (!g_slirp) {
        fprintf(stderr, "[slirp_bridge] slirp_new returned NULL\n");
        return -3;
    }
    (void)p_slirp_init;  // kept as fallback only; not used.

    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, poll_thread_func, NULL, 0, NULL);
    if (!g_thread) return -4;
    return 0;
}

extern "C" __declspec(dllexport) void sb_tx(const uint8_t* buf, size_t len) {
    if (!g_slirp || !p_slirp_input) return;
    InterlockedIncrement(&s_tx_count);
    EnterCriticalSection(&slirp_lock);
    p_slirp_input(g_slirp, buf, (int)len);
    LeaveCriticalSection(&slirp_lock);
}

extern "C" __declspec(dllexport) int sb_rx(uint8_t* buf, size_t max_len) {
    EnterCriticalSection(&rx_lock);
    if (rx_head == rx_tail) { LeaveCriticalSection(&rx_lock); return 0; }
    RxEntry e = rx_queue[rx_head];
    rx_head = (rx_head + 1) % RX_QUEUE_CAP;
    LeaveCriticalSection(&rx_lock);
    int n = e.len > (int)max_len ? (int)max_len : e.len;
    memcpy(buf, e.data, n);
    free(e.data);
    return n;
}

extern "C" __declspec(dllexport) void sb_get_mac(uint8_t mac[6]) {
    memcpy(mac, g_guest_mac, 6);
}

extern "C" __declspec(dllexport) void sb_cleanup(void) {
    InterlockedExchange(&g_running, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_slirp && p_slirp_cleanup) { p_slirp_cleanup(g_slirp); g_slirp = NULL; }
}

extern "C" int __stdcall DllMain(HINSTANCE, DWORD, LPVOID) { return 1; }
