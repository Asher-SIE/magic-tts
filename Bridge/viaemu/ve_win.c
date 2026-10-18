/* viaemu Win32/CRT 垫片: 266 个导入面按需实现.
 * 线程 = 宿主 pthread (各持 guest 栈 + 独立 VeCPU); 事件/互斥/临界区 → pthread 原语.
 * guest→垫片→guest 重入 (qsort 比较器/窗口过程/ECI 回调) 走 ve_call_guest.
 */
#include "viaemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

void SYSTEMTIME_FILL(void *p);

/* ---------------- guest 伪 FILE ↔ 宿主 FILE* ---------------- */
static FILE *g_fhost[128];
static const uint32_t GFILE_BASE = 0x30000000;
int ve_fhost(uint32_t gfile, FILE **out) {
    if (gfile < GFILE_BASE || gfile >= GFILE_BASE + 128 * 64) return 0;
    if (!g_fhost[(gfile - GFILE_BASE) / 64]) return 0;
    *out = g_fhost[(gfile - GFILE_BASE) / 64];
    return 1;
}
void ve_fmark_free(uint32_t gfile) {
    if (gfile >= GFILE_BASE && gfile < GFILE_BASE + 128 * 64)
        g_fhost[(gfile - GFILE_BASE) / 64] = NULL;
}

/* ---------------- thunk 表 ---------------- */
#define MAX_SHIMS 1200
typedef struct ShimEnt { char dll[16]; char func[140]; char mod[16]; int borp; } ShimEnt;
/* borp: 导入方为 Borland 约定 (ibmeci/torrent) 的 new/delete 被调方弹栈 */
static ShimEnt g_shims[MAX_SHIMS];
static int g_nshims;
int ve_shim_burst_req = 0;   /* >0: 接下来 N 个垫片调用强制打印 (诊断) */
int ve_sscanf_trace = 0;     /* >0: 接下来 N 个 sscanf 打印输入/格式 */

/* ---- Cmm IPC 命名共享块 (pgmin/pgmout/cmdin/cmdout/prompt): 单进程内 ---- */
typedef struct { char name[64]; uint32_t ptr; uint32_t size; int used; } VeShm;
static VeShm g_shm[32];
int ve_shm_find(const char *nm) {
    if (!nm) return -1;
    for (int i = 0; i < 32; i++)
        if (g_shm[i].used && strcmp(g_shm[i].name, nm) == 0) return i;
    return -1;
}
int ve_shm_create(const char *nm, uint32_t sz) {
    for (int i = 0; i < 32; i++) {
        if (!g_shm[i].used) {
            snprintf(g_shm[i].name, 64, "%s", nm);
            g_shm[i].size = sz;
            g_shm[i].ptr = ve_heap_alloc(sz);
            if (g_shm[i].ptr) memset(ve_guest_ptr(g_shm[i].ptr), 0, sz);
            g_shm[i].used = 1;
            ve_log("SHM-创建 %s size=0x%x @0x%08x", nm, sz, g_shm[i].ptr);
            return g_shm[i].ptr ? i : -1;
        }
    }
    return -1;
}
uint32_t ve_shm_ptr_of(uint32_t h) {
    uint32_t i = h - 0x2100;
    return (h >= 0x2100 && h < 0x2120 && g_shm[i].used) ? g_shm[i].ptr : 0;
}

void *ve_shim_slot(const char *dll, const char *func) {
    return ve_shim_slot_mod(dll, func, "");
}

void *ve_shim_slot_mod(const char *dll, const char *func, const char *mod) {
    if (strcmp(func, "__ve_missing") == 0) {
        return (void *)(uintptr_t)0x0EF00000u;   /* 缺失哨兵槽 */
    }
    int borp = (!strcmp(mod, "ibmeci") || !strcmp(mod, "torrent"));
    for (int i = 0; i < g_nshims; i++) {
        if (strcmp(g_shims[i].func, func) == 0 && strcasecmp(g_shims[i].dll, dll) == 0
            && strcmp(g_shims[i].mod, mod) == 0) {
            return (void *)(uintptr_t)(0x0F000000u + (uint32_t)i * 8);
        }
    }
    if (g_nshims >= MAX_SHIMS) {
        ve_log("!! thunk 表溢出 (%s!%s) → 哨兵", dll, func);
        return (void *)(uintptr_t)0x0EF00000u;
    }
    snprintf(g_shims[g_nshims].dll, 16, "%s", dll);
    snprintf(g_shims[g_nshims].func, 140, "%s", func);
    snprintf(g_shims[g_nshims].mod, 16, "%s", mod);
    g_shims[g_nshims].borp = borp;
    return (void *)(uintptr_t)(0x0F000000u + (uint32_t)g_nshims++ * 8);
}

void ve_shim_init(void) { g_nshims = 0; }

/* ---------------- 上下文 ---------------- */
static pthread_key_t g_cpu_key;
static pthread_once_t g_cpu_once = PTHREAD_ONCE_INIT;
static void make_key(void) { pthread_key_create(&g_cpu_key, NULL); }

VeCPU *ve_win_cpu(void) {
    pthread_once(&g_cpu_once, make_key);
    return (VeCPU *)pthread_getspecific(g_cpu_key);
}
void ve_win_set_cpu(VeCPU *c) {
    pthread_once(&g_cpu_once, make_key);
    pthread_setspecific(g_cpu_key, c);
}
void ve_win_thread_attach(VeCPU *c) { ve_win_set_cpu(c); }

static inline char *cp(uint32_t a) { return (char *)GADDR(a); }
static uint32_t M32H(uint32_t a) { return *(uint32_t *)GADDR(a); }

/* ---------------- 句柄表 ---------------- */
typedef enum { H_EVENT, H_MUTEX, H_THREAD, H_FILE, H_KEY, H_CRIT } HType;
typedef struct Handle {
    HType type;
    int used;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int state;
    int autoreset;
    pthread_t th;
    int exitcode;
    FILE *fp;
    char path[256];
    pthread_mutex_t crit;
} Handle;
#define MAX_H 256
static Handle g_h[MAX_H];
static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER;

static int h_alloc(HType t) {
    pthread_mutex_lock(&g_hmu);
    for (int i = 1; i < MAX_H; i++) {
        if (!g_h[i].used) {
            memset(&g_h[i], 0, sizeof(Handle));
            g_h[i].used = 1;
            g_h[i].type = t;
            g_h[i].exitcode = 0;
            pthread_mutex_init(&g_h[i].mu, NULL);
            pthread_cond_init(&g_h[i].cv, NULL);
            pthread_mutex_unlock(&g_hmu);
            return i;
        }
    }
    pthread_mutex_unlock(&g_hmu);
    return 0;
}
static Handle *h_get(int h) { return (h > 0 && h < MAX_H && g_h[h].used) ? &g_h[h] : NULL; }

/* ---------------- guest 线程 ---------------- */
static uint32_t g_stack_top = 0x3A000000u;

typedef struct { uint32_t start, arg; VeCPU *cpu; int hid; } ThrArg;
uint32_t ve_thread_setup(VeCPU *c, int is_main);

static void *thread_main(void *p) {
    ThrArg *t = p;
    ve_win_thread_attach(t->cpu);
    uint32_t args[1] = { t->arg };
    uint32_t rc = ve_call_guest(t->cpu, t->start, 1, args);
    Handle *h = h_get(t->hid);
    if (h) {
        pthread_mutex_lock(&h->mu);
        h->exitcode = rc ? -1 : (int)t->cpu->reg[0];
        pthread_cond_broadcast(&h->cv);
        pthread_mutex_unlock(&h->mu);
    }
    free(t);
    return NULL;
}

/* ---------------- SEH (最小实现) ---------------- */
void ve_raise_fault(VeCPU *c, int code, uint32_t addr) {
    ve_log("fault code=%d eip=0x%08x addr=0x%08x → 终止当前 guest 调用", code, c->eip, addr);
    c->eip = 0x0EFFFFFFu;   /* STOP 哨兵 */
}

/* ---------------- printf 家族 (guest 变参) ---------------- */
static int guest_vformat(char *out, int cap, const char *fmt, uint32_t sp) {
    int n = 0;
    for (const char *p = fmt; *p && n < cap - 1; p++) {
        if (*p != '%') { out[n++] = *p; continue; }
        p++;
        char spec[32]; int si = 0;
        while (*p == '-' || *p == '+' || *p == '0' || *p == ' ' || *p == '#' ||
               (*p >= '0' && *p <= '9') || *p == '.') {
            if (si < 30) spec[si++] = *p;
            p++;
        }
        spec[si] = 0;
        int lng = 0;
        while (*p == 'l' || *p == 'I') {
            if (*p == 'l') lng = 1;
            p++;
            if (*p == '3' || *p == '6') p++;
            while (*p >= '0' && *p <= '9') p++;
        }
        char conv = *p;
        char piece[160];
        switch (conv) {
        case 'd': case 'i': {
            int32_t v = *(int32_t *)GADDR(sp); sp += 4;
            snprintf(piece, sizeof(piece), "%%%s%sd", spec, lng ? "l" : "");
            if (lng) { long lv = v; n += snprintf(out + n, (size_t)(cap - n), piece, lv); }
            else n += snprintf(out + n, (size_t)(cap - n), piece, v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint32_t v = *(uint32_t *)GADDR(sp); sp += 4;
            char cs[2] = { conv, 0 };
            snprintf(piece, sizeof(piece), "%%%s%s%s", spec, lng ? "l" : "", cs);
            unsigned long lv = v;
            n += snprintf(out + n, (size_t)(cap - n), piece, lv);
            break;
        }
        case 'c': {
            int v = *(int32_t *)GADDR(sp); sp += 4;
            out[n++] = (char)v;
            break;
        }
        case 's': case 'S': {
            uint32_t s = *(uint32_t *)GADDR(sp); sp += 4;
            n += snprintf(out + n, (size_t)(cap - n), "%s", s ? cp(s) : "(null)");
            break;
        }
        case 'f': case 'g': case 'e': case 'F': case 'G': case 'E': {
            double v;
            memcpy(&v, GADDR(sp), 8); sp += 8;
            char cs[2] = { conv, 0 };
            snprintf(piece, sizeof(piece), "%%%s%s", spec, cs);
            n += snprintf(out + n, (size_t)(cap - n), piece, v);
            break;
        }
        case 'p': {
            uint32_t v = *(uint32_t *)GADDR(sp); sp += 4;
            n += snprintf(out + n, (size_t)(cap - n), "%08x", v);
            break;
        }
        case '%': out[n++] = '%'; break;
        default: if (*p) { out[n++] = '%'; out[n++] = *p; } break;
        }
    }
    out[n] = 0;
    return n;
}

/* ---------------- 垫片分发 ---------------- */
int ve_shim_dispatch(int idx, VeCPU *c) {
    static int s_last = -1; static uint32_t s_count = 0; static int s_reported = 0;
    if (idx < 0 || idx >= g_nshims) { c->reg[0] = 0; return 0; }
    const char *dll = g_shims[idx].dll;
    const char *fn = g_shims[idx].func;
    int pop = 0;
    if (idx == s_last) {
        if (++s_count > 500 && !s_reported) {
            ve_log("垫片自旋: %s!%s 已连调 %u 次", dll, fn, s_count);
            s_reported = 1;
        }
    } else { s_last = idx; s_count = 0; s_reported = 0; }

#define A(n) (*(uint32_t *)GADDR(c->reg[4] + (n) * 4))   /* FF/2 已弹返回地址: [esp]=arg1 */
#define STD(nbytes) (pop = (nbytes))

    /* 内部槽: ECI 波形回调 (cdecl 5 参: data, event, msg, lparam, wparam) */
    if (strcmp(fn, "__ve_eci_cb") == 0) {
        extern int (*g_eci_cb_hook)(VeCPU *c, uint32_t msg, uint32_t lp, uint32_t wp);
        c->reg[0] = g_eci_cb_hook ? (uint32_t)g_eci_cb_hook(c, A(2), A(3), A(4)) : 0;
        return 0;
    }

    static int trace = -1;
    static int burst = 0;                 /* 诊断: 强制打印接下来 N 个垫片调用 */
    if (trace < 0) trace = getenv("VE_TRACE") ? 1 : 0;
    if (burst > 0) { burst--; ve_log("SHIM %s!%s esp=0x%08x", dll, fn, c->reg[4]); }
    else if (trace) ve_log("SHIM %s!%s esp=0x%08x", dll, fn, c->reg[4]);
    extern int ve_shim_burst_req;
    if (ve_shim_burst_req > 0) { burst = ve_shim_burst_req; ve_shim_burst_req = 0; }

    if (strcasecmp(dll, "KERNEL32.dll") == 0) {
        if (!strcmp(fn, "Sleep")) { usleep((useconds_t)A(0) * 1000); STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "SleepEx")) { usleep((useconds_t)A(0) * 1000); STD(8); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "GetCurrentThreadId")) { c->reg[0] = (uint32_t)(uintptr_t)pthread_self(); return 0; }
        if (!strcmp(fn, "GetCurrentProcess")) { c->reg[0] = 0xFFFFFFFF; return 0; }
        if (!strcmp(fn, "GetCurrentProcessId")) { c->reg[0] = (uint32_t)getpid(); return 0; }
        if (!strcmp(fn, "GetTickCount")) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); c->reg[0] = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); return 0; }
        if (!strcmp(fn, "GetVersion")) { c->reg[0] = 0x0A280105; return 0; }
        if (!strcmp(fn, "GetVersionExA")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "GetCommandLineA")) { c->reg[0] = 0x20000004; return 0; }
        if (!strcmp(fn, "GetStartupInfoA")) { memset(ve_guest_ptr(A(0)), 0, 68); STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "GetStdHandle")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "GetFileType")) { STD(4); c->reg[0] = 2; goto done; }
        if (!strcmp(fn, "SetHandleCount")) { STD(4); c->reg[0] = A(0); goto done; }
        if (!strcmp(fn, "GetLastError")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "SetLastError")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "SetUnhandledExceptionFilter")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "InterlockedIncrement")) { int32_t *p = (int32_t *)ve_guest_ptr(A(0)); c->reg[0] = (uint32_t)(++(*p)); STD(4); return pop; }
        if (!strcmp(fn, "InterlockedDecrement")) { int32_t *p = (int32_t *)ve_guest_ptr(A(0)); c->reg[0] = (uint32_t)(--(*p)); STD(4); return pop; }
        if (!strcmp(fn, "IsBadReadPtr") || !strcmp(fn, "IsBadWritePtr")) { STD(8); c->reg[0] = ve_guest_ptr(A(0)) ? 0 : 1; goto done; }
        if (!strcmp(fn, "IsBadCodePtr")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "HeapAlloc")) {
            if (A(2) > 0x10000000) ve_log("HeapAlloc(0x%08x) 可疑 ret=0x%08x", A(2), M32H(c->reg[4]));
            STD(12); c->reg[0] = ve_heap_alloc(A(2)); goto done;
        }
        if (!strcmp(fn, "HeapReAlloc")) { STD(16); c->reg[0] = ve_heap_realloc(A(1), A(3)); goto done; }
        if (!strcmp(fn, "HeapFree")) { STD(12); ve_heap_free(A(1)); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "HeapSize")) { STD(12); uint32_t *hh = ve_guest_ptr(A(1) - 8); c->reg[0] = hh ? hh[0] - 8 : 0; goto done; }
        if (!strcmp(fn, "HeapCreate")) { STD(12); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "HeapDestroy")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "VirtualAlloc")) {
            if (A(1) > 0x10000000) ve_log("VirtualAlloc(0x%08x) 可疑 ret=0x%08x", A(1), M32H(c->reg[4]));
            STD(16); c->reg[0] = ve_heap_alloc(A(1)); goto done;
        }
        if (!strcmp(fn, "VirtualFree")) { STD(12); ve_heap_free(A(0)); c->reg[0] = 1; goto done; }
        static __thread uint32_t tls_slots[64];
        if (!strcmp(fn, "TlsAlloc")) { c->reg[0] = 1; return 0; }
        if (!strcmp(fn, "TlsFree")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "TlsGetValue")) { STD(4); c->reg[0] = tls_slots[A(0) & 63]; return pop; }
        if (!strcmp(fn, "TlsSetValue")) { tls_slots[A(0) & 63] = A(1); STD(8); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "CreateEventA")) {
            int h = h_alloc(H_EVENT);
            g_h[h].autoreset = !A(1);
            g_h[h].state = A(3) ? 1 : 0;
            STD(16); c->reg[0] = (uint32_t)h; goto done;
        }
        if (!strcmp(fn, "SetEvent")) {
            Handle *h = h_get((int)A(0));
            if (h) { pthread_mutex_lock(&h->mu); h->state = 1; pthread_cond_broadcast(&h->cv); pthread_mutex_unlock(&h->mu); }
            STD(4); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "ResetEvent")) {
            Handle *h = h_get((int)A(0));
            if (h) { pthread_mutex_lock(&h->mu); h->state = 0; pthread_mutex_unlock(&h->mu); }
            STD(4); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "CreateMutexA")) {
            int h = h_alloc(H_MUTEX);
            pthread_mutexattr_t a; pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_h[h].crit, &a);
            if (A(2)) pthread_mutex_lock(&g_h[h].crit);
            STD(12); c->reg[0] = (uint32_t)h; goto done;
        }
        if (!strcmp(fn, "ReleaseMutex")) {
            Handle *h = h_get((int)A(0));
            if (h) pthread_mutex_unlock(&h->crit);
            STD(4); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "OpenEventA")) { STD(12); c->reg[0] = (uint32_t)h_alloc(H_EVENT); goto done; }
        if (!strcmp(fn, "InitializeCriticalSection")) {
            int h = h_alloc(H_CRIT);
            pthread_mutexattr_t a; pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_h[h].crit, &a);
            *(uint32_t *)ve_guest_ptr(A(0)) = (uint32_t)h;
            STD(4); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "EnterCriticalSection")) {
            Handle *h = h_get((int)M32H(A(0)));
            if (h) pthread_mutex_lock(&h->crit);
            STD(4); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "LeaveCriticalSection")) {
            Handle *h = h_get((int)M32H(A(0)));
            if (h) pthread_mutex_unlock(&h->crit);
            STD(4); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "DeleteCriticalSection")) {
            Handle *h = h_get((int)M32H(A(0)));
            if (h) { pthread_mutex_destroy(&h->crit); h->used = 0; }
            STD(4); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "WaitForSingleObject")) {
            int hidx = (int)A(0);
            uint32_t ms = A(1);
            Handle *h = h_get(hidx);
            if (!h) { STD(8); c->reg[0] = 0xFFFFFFFF; goto done; }
            if (h->type == H_MUTEX) { pthread_mutex_lock(&h->crit); STD(8); c->reg[0] = 0; goto done; }
            if (h->type == H_THREAD) {
                pthread_mutex_lock(&h->mu);
                int waited = 0;
                while (h->exitcode == -1 && waited < 5000) {
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_sec += 1;
                    pthread_cond_timedwait(&h->cv, &h->mu, &ts);
                    waited += 1000;
                }
                pthread_mutex_unlock(&h->mu);
                STD(8); c->reg[0] = h->exitcode == -1 ? 258u : 0u; goto done;   /* 封顶 5s */
            }
            pthread_mutex_lock(&h->mu);
            {
                int waited = 0;
                for (;;) {
                    if (h->state) { if (h->autoreset) h->state = 0; break; }
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_nsec += 50 * 1000000; ts.tv_sec += ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
                    if (pthread_cond_timedwait(&h->cv, &h->mu, &ts) != 0) {
                        if (ms != 0xFFFFFFFF) break;
                        waited += 50;
                        if (waited >= 5000) {   /* INFINITE 等待封顶 5s → WAIT_TIMEOUT */
                            pthread_mutex_unlock(&h->mu);
                            STD(8); c->reg[0] = 258u; goto done;
                        }
                    }
                }
            }
            pthread_mutex_unlock(&h->mu);
            STD(8); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "GetExitCodeThread")) {
            Handle *h = h_get((int)A(0));
            *(uint32_t *)ve_guest_ptr(A(1)) = h ? (h->exitcode == -1 ? 259u : (uint32_t)h->exitcode) : 0;
            STD(8); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "CloseHandle")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "DuplicateHandle")) { STD(28); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "ExitProcess") || !strcmp(fn, "TerminateProcess")) {
            ve_log("guest 请求 %s(%u) — 忽略并终止调用链", fn, A(0));
            c->eip = 0x0EFFFFFFu;
            c->reg[0] = 0;
            return 0;
        }
        if (!strcmp(fn, "RaiseException")) {
            ve_raise_fault(c, (int)(0xE0000000u | A(0)), 0);
            return 0;
        }
        if (!strcmp(fn, "RtlUnwind")) { STD(16); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "CreateFileMappingA") || !strcmp(fn, "OpenFileMappingA")) {
            /* Cmm IPC 通道 (pgmin/pgmout/cmdin/cmdout/prompt): 单进程命名共享块 */
            const char *nm = NULL;
            uint64_t sz = 0;
            int nargs;
            if (fn[0] == 'C') {   /* (hFile, sa, prot, maxHi, maxLo, lpName) 6 参 */
                nm = A(5) ? cp(A(5)) : NULL;
                sz = ((uint64_t)A(3) << 32) | A(4);
                nargs = 24;
            } else {              /* (access, inherit, lpName) 3 参 */
                nm = A(2) ? cp(A(2)) : NULL;
                nargs = 12;
            }
            int hit = ve_shm_find(nm);
            if (hit < 0 && fn[0] == 'C' && nm) {
                uint32_t rsz = (uint32_t)(sz ? sz : 0x40000);   /* 默认 256KB */
                hit = ve_shm_create(nm, rsz);
            }
            c->reg[0] = hit >= 0 ? (uint32_t)(0x2100 + hit) : 0;
            if (hit < 0) ve_log("SHM-%s %s 失败", fn[0] == 'C' ? "创建" : "打开", nm ? nm : "(null)");
            STD(nargs); goto done;
        }
        if (!strcmp(fn, "MapViewOfFile")) {
            uint32_t h = A(0);
            uint32_t r = ve_shm_ptr_of(h);
            if (!r) ve_log("MapViewOfFile 未知句柄 0x%08x", h);
            c->reg[0] = r;
            STD(20); goto done;
        }
        if (!strcmp(fn, "UnmapViewOfFile")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "GetModuleHandleA")) { STD(4); c->reg[0] = 0x10000000; return pop; }
        if (!strcmp(fn, "GetProcAddress")) {
            STD(8);
            const char *nm = cp(A(1));
            int n; const VeModule *ms = ve_pe_modules(&n);
            for (int i = 0; i < n; i++) {
                uint32_t a = ve_pe_export(ms[i].base, nm);
                if (a) { c->reg[0] = a; return 0; }
            }
            c->reg[0] = 0;
            return 0;
        }
        if (!strcmp(fn, "LoadLibraryA")) {
            STD(4);
            const char *nm = cp(A(0));
            int n; const VeModule *ms = ve_pe_modules(&n);
            for (int i = 0; i < n; i++) {
                if (strstr(nm, ms[i].name)) { c->reg[0] = ms[i].base; return 0; }
            }
            ve_log("LoadLibraryA(%s) 未知 → NULL", nm);
            c->reg[0] = 0;
            return 0;
        }
        if (!strcmp(fn, "FreeLibrary")) { STD(4); c->reg[0] = 1; return pop; }
        if (!strcmp(fn, "GetModuleFileNameA")) {
            STD(12);
            int n; const VeModule *ms = ve_pe_modules(&n);
            extern char g_ve_dir[512];
            const char *pick = ms[0].name;
            const char *want = cp(A(0));
            for (int i = 0; i < n; i++) if (strstr(want, ms[i].name)) { pick = ms[i].name; break; }
            snprintf(cp(A(1)), A(2), "%s/%s", g_ve_dir, pick);
            c->reg[0] = (uint32_t)strlen(cp(A(1)));
            return 0;
        }
        if (!strcmp(fn, "SearchPathA")) { STD(32); c->reg[0] = 0; return pop; }
        if (!strcmp(fn, "CreateFileA")) {
            const char *path = cp(A(0));
            uint32_t disp = A(3);
            int h = h_alloc(H_FILE);
            snprintf(g_h[h].path, sizeof(g_h[h].path), "%s", path);
            char full[1024];
            extern char g_ve_dir[512];
            if (path[0] != '/') snprintf(full, sizeof(full), "%s/%s", g_ve_dir, path);
            else snprintf(full, sizeof(full), "%s", path);
            const char *mode = "rb";
            if (disp == 2 || disp == 4) mode = "r+b";
            if (disp == 1) mode = "wb";
            if (disp == 5) mode = "ab";
            if (disp == 3) mode = "wb+";
            g_h[h].fp = fopen(full, mode);
            if (!g_h[h].fp) { g_h[h].used = 0; STD(28); c->reg[0] = 0xFFFFFFFF; goto done; }
            STD(28); c->reg[0] = (uint32_t)h; goto done;
        }
        if (!strcmp(fn, "ReadFile")) {
            Handle *h = h_get((int)A(0));
            uint32_t got = 0;
            if (h && h->fp) got = (uint32_t)fread(ve_guest_ptr(A(1)), 1, A(2), h->fp);
            if (A(3)) *(uint32_t *)ve_guest_ptr(A(3)) = got;
            STD(20); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "WriteFile")) {
            Handle *h = h_get((int)A(0));
            uint32_t put = 0;
            if (h && h->fp) put = (uint32_t)fwrite(ve_guest_ptr(A(1)), 1, A(2), h->fp);
            if (A(3)) *(uint32_t *)ve_guest_ptr(A(3)) = put;
            STD(20); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "SetFilePointer")) {
            Handle *h = h_get((int)A(0));
            if (h && h->fp) fseek(h->fp, (int32_t)A(1), (int)A(3));
            STD(16); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "FlushFileBuffers") || !strcmp(fn, "SetEndOfFile")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "SetStdHandle")) { STD(8); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "GetEnvironmentVariableA")) { STD(12); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "SetEnvironmentVariableA")) { STD(8); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "GetEnvironmentStringsW") || !strcmp(fn, "GetEnvironmentStrings")) { c->reg[0] = 0x20001000; return 0; }
        if (!strcmp(fn, "FreeEnvironmentStringsA") || !strcmp(fn, "FreeEnvironmentStringsW")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "GetSystemTime") || !strcmp(fn, "GetLocalTime")) {
            SYSTEMTIME_FILL(ve_guest_ptr(A(0)));
            STD(4); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "GetTimeZoneInformation")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "GetACP")) { c->reg[0] = 936; return 0; }
        if (!strcmp(fn, "GetOEMCP")) { c->reg[0] = 936; return 0; }
        if (!strcmp(fn, "GetCPInfo")) { STD(8); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "MultiByteToWideChar")) {
            uint32_t src = A(2), srclen = A(3), dst = A(4), dstcap = A(5);
            const uint8_t *sp = (const uint8_t *)GADDR(src);
            int n = 0, i = 0;
            while (srclen == 0xFFFFFFFF ? sp[i] != 0 : i < (int)srclen) {
                if (dst && (i + 1) >= (int)dstcap) break;
                if (dst) *(uint16_t *)GADDR(dst + n) = sp[i];
                n += 2; i++;
            }
            if (dst && srclen == 0xFFFFFFFF) { *(uint16_t *)GADDR(dst + n) = 0; n += 2; }
            c->reg[0] = (uint32_t)(n / 2);
            return 0;
        }
        if (!strcmp(fn, "WideCharToMultiByte")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "CompareStringA") || !strcmp(fn, "CompareStringW")) {
            int r = strcmp(cp(A(2)), cp(A(4)));
            STD(24); c->reg[0] = r < 0 ? 1u : (r ? 3u : 2u); goto done;
        }
        if (!strcmp(fn, "LCMapStringA") || !strcmp(fn, "LCMapStringW")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "GetStringTypeA") || !strcmp(fn, "GetStringTypeW")) { STD(20); c->reg[0] = 0; goto done; }
    }
    else if (strcasecmp(dll, "ADVAPI32.dll") == 0) {
        /* ---- ViaVoice ECIINI 伪注册表 (数据源自 libq4 内嵌表 eciini_vals, 全量) ---- */
        static const struct { const char *sub, *name, *val; } k_ini[] = {
            {"6.0", "Path", "{D}/chs50.syn"},
            {"6.0", "Path_Rom", "{D}/chsrom.dll"},
            {"6.0", "Path_Cmm", "{D}/ibmcmm.dll"},
            {"6.0", "Path_Cat", "{D}/torrent.dll"},
            {"6.0", "Version", "5.0"},
            {"6.0", "Concatenative", "1.0"},
            {"6.0", "CallbackFlag", "0x33f"},
            {"6.0", "Voice1", "0 50 69 30 0 0 50 92"},
            {"6.0", "Voice2", "1 50 81 30 0 50 50 100"},
            {"6.0", "Voice3", "1 22 93 35 0 0 50 90"},
            {"6.0", "Voice4", "0 86 56 47 0 0 50 93"},
            {"6.0", "Voice5", "0 50 69 34 0 0 70 92"},
            {"6.0", "Voice6", "1 56 89 35 0 40 70 95"},
            {"6.0", "Voice7", "1 45 68 30 3 40 50 90"},
            {"6.0", "Voice8", "0 30 61 44 18 20 50 90"},
            {"6.0", "Phoneme0", "65 0 0 0 0 0 0 75 150 128 100 130 175 200 125"},
            {"6.0", "Phoneme1", "66 0 0 0 0 0 0 100 125 128 200 150 130 150 150"},
            {"6.0", "Phoneme2", "67 0 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme3", "68 0 0 0 0 0 0 50 100 128 25 130 130 180 125"},
            {"6.0", "Phoneme4", "69 0 0 0 0 0 0 50 100 128 75 130 175 0 125"},
            {"6.0", "Phoneme5", "70 0 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme6", "71 0 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme7", "72 0 0 0 0 0 0 50 75 128 100 150 130 0 100"},
            {"6.0", "Phoneme8", "73 0 0 0 0 0 0 50 100 128 50 130 175 0 125"},
            {"6.0", "Phoneme9", "74 0 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme10", "79 0 0 0 0 0 0 25 0 130 150 60 60 0 255"},
            {"6.0", "Phoneme11", "82 0 0 0 0 0 0 75 100 128 75 130 0 0 150"},
            {"6.0", "Phoneme12", "83 0 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme13", "84 0 0 0 0 0 0 50 100 128 25 130 130 180 125"},
            {"6.0", "Phoneme14", "85 0 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme15", "89 0 0 0 0 0 0 100 125 128 200 150 130 150 150"},
            {"6.0", "Phoneme16", "90 0 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme17", "97 0 0 0 0 0 0 100 125 128 200 150 130 150 150"},
            {"6.0", "Phoneme18", "98 0 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme19", "99 0 0 0 0 0 0 50 160 0 50 128 128 160 0"},
            {"6.0", "Phoneme20", "100 0 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme21", "101 0 0 0 0 0 0 50 100 128 75 130 175 0 255"},
            {"6.0", "Phoneme22", "102 0 0 0 0 0 0 10 100 128 25 130 0 0 200"},
            {"6.0", "Phoneme23", "103 0 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme24", "104 0 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme25", "105 0 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme26", "107 0 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme27", "108 0 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme28", "109 0 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme29", "110 0 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme30", "111 0 0 0 0 0 0 25 0 130 130 60 60 0 255"},
            {"6.0", "Phoneme31", "112 0 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme32", "114 0 0 0 0 0 0 75 50 128 75 130 0 0 175"},
            {"6.0", "Phoneme33", "115 0 0 0 0 0 0 50 100 128 75 130 130 150 0"},
            {"6.0", "Phoneme34", "116 0 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme35", "117 0 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme36", "118 0 0 0 0 0 0 10 100 128 25 130 0 0 200"},
            {"6.0", "Phoneme37", "119 0 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme38", "120 0 0 0 0 0 0 10 50 128 75 130 0 0 0"},
            {"6.0", "Phoneme39", "121 0 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme40", "122 0 0 0 0 0 0 50 100 128 75 130 130 150 0"},
            {"6.0", "Phoneme41", "95 65 0 0 0 0 0 75 150 128 100 130 175 200 125"},
            {"6.0", "Phoneme42", "95 66 0 0 0 0 0 100 125 128 200 150 130 150 150"},
            {"6.0", "Phoneme43", "95 68 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme44", "95 69 0 0 0 0 0 50 100 128 75 130 175 0 125"},
            {"6.0", "Phoneme45", "95 71 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme46", "95 72 0 0 0 0 0 50 100 128 75 130 175 0 255"},
            {"6.0", "Phoneme47", "95 73 0 0 0 0 0 50 100 128 50 130 175 0 125"},
            {"6.0", "Phoneme48", "95 79 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme49", "95 82 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme50", "95 83 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme51", "95 84 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme52", "95 85 0 0 0 0 0 50 160 0 50 128 128 160 0"},
            {"6.0", "Phoneme53", "95 86 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme54", "95 87 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme55", "95 88 0 0 0 0 0 50 160 0 50 128 128 160 0"},
            {"6.0", "Phoneme56", "95 89 0 0 0 0 0 75 125 128 100 130 130 0 200"},
            {"6.0", "Phoneme57", "95 97 0 0 0 0 0 100 125 128 200 150 130 150 150"},
            {"6.0", "Phoneme58", "95 98 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme59", "95 99 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme60", "95 100 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme61", "95 101 0 0 0 0 0 50 100 128 75 130 175 0 255"},
            {"6.0", "Phoneme62", "95 102 0 0 0 0 0 10 100 128 25 130 0 0 200"},
            {"6.0", "Phoneme63", "95 103 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme64", "95 104 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme65", "95 105 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme66", "95 106 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme67", "95 107 0 0 0 0 0 50 100 128 75 130 130 0 0"},
            {"6.0", "Phoneme68", "95 108 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme69", "95 109 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme70", "95 110 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme71", "95 111 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme72", "95 112 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme73", "95 113 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme74", "95 114 0 0 0 0 0 75 50 128 75 130 0 0 175"},
            {"6.0", "Phoneme75", "95 115 0 0 0 0 0 50 100 128 75 130 130 150 0"},
            {"6.0", "Phoneme76", "95 116 0 0 0 0 0 75 150 128 50 130 130 255 0"},
            {"6.0", "Phoneme77", "95 117 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme78", "95 118 0 0 0 0 0 10 100 128 25 130 0 0 200"},
            {"6.0", "Phoneme79", "95 118 0 0 0 0 0 25 0 130 90 60 60 0 255"},
            {"6.0", "Phoneme80", "95 119 0 0 0 0 0 0 100 128 0 0 0 0 200"},
            {"6.0", "Phoneme81", "95 120 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme82", "95 121 0 0 0 0 0 25 150 128 50 130 130 0 255"},
            {"6.0", "Phoneme83", "95 64 0 0 0 0 0 75 50 128 75 130 0 0 175"},
            {"6.0", "Phoneme84", "164 0 0 0 0 0 0 0 170 128 0 0 0 0 0"},
            {"LanguageIndependent", "Path_Cmm", "{D}/ibmcmm.dll"},
            {"LanguageIndependent", "Path_Cat", "{D}/torrent.dll"},
        };
        /* 句柄编码: 0x100=ECIINI 根, 0x101="6.0", 0x102="LanguageIndependent" */
        if (!strcmp(fn, "RegOpenKeyExA")) {
            const char *sub = cp(A(1));
            uint32_t parent = A(0);
            uint32_t hk = 0x1FF;   /* 通用句柄 (一律成功, 保持 DllMain 原走向) */
            if (strstr(sub, "ECIINI")) {
                hk = 0x100;
                if (strstr(sub, "6.0")) hk = 0x101;
                else if (strstr(sub, "LanguageIndependent")) hk = 0x102;
            } else if (parent == 0x100) {
                if (strstr(sub, "6.0")) hk = 0x101;
                else if (strstr(sub, "LanguageIndependent")) hk = 0x102;
                else hk = 0x100;
            }
            if (A(4)) *(uint32_t *)ve_guest_ptr(A(4)) = hk;
            extern int ve_reg_trace;
            if (ve_reg_trace) ve_log("REGOPEN parent=0x%x sub=%s → 0x%x", parent, sub, hk);
            STD(20); c->reg[0] = 0; goto done;   /* ERROR_SUCCESS */
        }
        if (!strcmp(fn, "RegCreateKeyExA")) {
            if (A(6)) *(uint32_t *)ve_guest_ptr(A(6)) = 0x1FF;
            STD(32); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "RegQueryValueExA")) {
            uint32_t hk = A(0);
            const char *name = cp(A(1));
            extern int ve_reg_trace;
            if (ve_reg_trace) ve_log("REGQ hk=0x%x name=%s", hk, name);
            {   /* 断路器: Phoneme 查询满 200 次 → 打印接下来 80 个垫片调用 + sscanf 细节 */
                static int s_phn = 0;
                extern int ve_sscanf_trace;
                if (!strncmp(name, "Phoneme", 7) && ++s_phn == 200) {
                    ve_shim_burst_req = 80;
                    ve_sscanf_trace = 20;
                }
                if (!strcmp(name, "CallbackFlag")) ve_sscanf_trace = 4;
            }
            const char *subname = hk == 0x101 ? "6.0" : (hk == 0x102 ? "LanguageIndependent" : NULL);
            const char *val = NULL;
            if (subname) {
                for (unsigned i = 0; i < sizeof(k_ini) / sizeof(k_ini[0]); i++) {
                    if (!strcmp(k_ini[i].sub, subname) && !strcmp(k_ini[i].name, name)) {
                        val = k_ini[i].val; break;
                    }
                }
            } else {
                /* 根/通用句柄: 全表搜索 */
                for (unsigned i = 0; i < sizeof(k_ini) / sizeof(k_ini[0]); i++) {
                    if (!strcmp(k_ini[i].name, name)) { val = k_ini[i].val; break; }
                }
            }
            if (val) {
                char expanded[600];
                extern char g_ve_dir[512];
                const char *p = val; int j = 0;
                for (const char *q = strstr(p, "{D}"); q; q = strstr(p, "{D}")) {
                    while (p < q && j < 550) expanded[j++] = *p++;
                    for (const char *dd = g_ve_dir; *dd && j < 550; dd++) expanded[j++] = *dd;
                    p += 3;
                }
                while (*p && j < 550) expanded[j++] = *p++;
                expanded[j] = 0;
                uint32_t len = (uint32_t)strlen(expanded) + 1;
                if (A(3)) *(uint32_t *)ve_guest_ptr(A(3)) = 1;   /* REG_SZ */
                if (A(5)) *(uint32_t *)ve_guest_ptr(A(5)) = len;
                /* A(5)=lpcbData 指针, 读入方缓冲尺寸; A(2) 是 lpReserved (恒 NULL) */
                uint32_t cb = 0xFFFFFFFFu;
                if (A(5)) { uint32_t *pv = (uint32_t *)ve_guest_ptr(A(5)); if (pv) cb = *pv; }
                extern int ve_reg_trace;
                if (ve_reg_trace && !strncmp(name, "Phoneme", 7) && name[7] == '0' && !name[8])
                    ve_log("REGQ-DATA %s lpData=0x%08x cb_in=%u len=%u → %s",
                           name, A(4), cb, len, (A(4) && cb >= len) ? "拷贝" : "未拷贝");
                if (A(4) && cb >= len) memcpy(ve_guest_ptr(A(4)), expanded, len);
                STD(24); c->reg[0] = 0; goto done;
            }
            if (A(3)) *(uint32_t *)ve_guest_ptr(A(3)) = 4;
            if (A(5)) *(uint32_t *)ve_guest_ptr(A(5)) = 4;
            if (A(4)) *(uint32_t *)ve_guest_ptr(A(4)) = 0;
            STD(24); c->reg[0] = 2; goto done;
        }
        if (!strcmp(fn, "RegCloseKey")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "RegSetValueExA")) { STD(24); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "RegQueryInfoKeyA")) {
            /* 12 参; 计数出参必须写 (调用方据 lpcMax*Len 分配缓冲!) */
            uint32_t outs[8] = { A(4), A(5), A(6), A(7), A(8), A(9), A(10), 0 };
            uint32_t vals[8] = { 0, 260, 260, 0, 260, 0x400, 4, 0 };
            /* 根键有 2 个子键: "6.0" / "LanguageIndependent" (max len=19) */
            if (A(0) == 0x100 || A(0) == 0x1FF) { vals[0] = 2; vals[1] = 19; }
            for (int i = 0; i < 7; i++)
                if (outs[i]) *(uint32_t *)ve_guest_ptr(outs[i]) = vals[i];
            if (A(11)) memset(ve_guest_ptr(A(11)), 0, 8);   /* lpftLastWriteTime */
            STD(48); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "RegEnumKeyExA")) {
            static const char *root_keys[] = { "6.0", "LanguageIndependent" };
            uint32_t idx = A(1);
            if ((A(0) == 0x100 || A(0) == 0x1FF) && idx < 2) {
                const char *nm = root_keys[idx];
                uint32_t n = (uint32_t)strlen(nm);
                uint32_t cap = 0xFFFFFFFFu;
                if (A(3)) { uint32_t *pc = (uint32_t *)ve_guest_ptr(A(3)); if (pc) { cap = *pc; *pc = n; } }
                if (A(2) && cap >= n) memcpy(ve_guest_ptr(A(2)), nm, n + 1);
                ve_log("REGENUM idx=%u → %s", idx, nm);
                STD(32); c->reg[0] = 0; goto done;
            }
            STD(32); c->reg[0] = 259; goto done;   /* ERROR_NO_MORE_ITEMS */
        }
        if (!strcmp(fn, "RegDeleteKeyA") || !strcmp(fn, "RegDeleteValueA")) { STD(8); c->reg[0] = 2; goto done; }
        if (!strcmp(fn, "InitializeSecurityDescriptor") || !strcmp(fn, "SetSecurityDescriptorDacl")) { STD(8); c->reg[0] = 1; goto done; }
    }
    else if (strcasecmp(dll, "USER32.dll") == 0) {
        static uint32_t s_wndproc;
        if (!strcmp(fn, "RegisterClassExA") || !strcmp(fn, "RegisterClassA")) {
            s_wndproc = M32H(A(0) + (strstr(fn, "Ex") ? 8 : 4));
            c->reg[0] = 0x1000;
            return 0;
        }
        if (!strcmp(fn, "CreateWindowExA")) {
            static uint32_t next_hwnd = 0x20010;
            c->reg[0] = ++next_hwnd;
            STD(48);
            goto done;
        }
        if (!strcmp(fn, "DestroyWindow") || !strcmp(fn, "UnregisterClassA")) { STD(4); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "GetDesktopWindow")) { c->reg[0] = 0x20001; return 0; }
        if (!strcmp(fn, "PostMessageA")) { STD(16); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "PostQuitMessage")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "SendMessageA")) {
            if (s_wndproc) {
                uint32_t args[4] = { A(0), A(1), A(2), A(3) };
                ve_call_guest(c, s_wndproc, 4, args);
            }
            STD(16); c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "GetMessageA")) {
            /* 真实语义: 无消息时阻塞 (绝不能返回 0=WM_QUIT, 会杀死引擎窗口线程) */
            extern int ve_pop_msg(uint32_t *m);
            int got = 0;
            for (int waited = 0; waited < 60000 && !got; waited += 20) {
                uint32_t m;
                if (ve_pop_msg(&m)) {
                    memset(ve_guest_ptr(A(0)), 0, 28);
                    *(uint32_t *)ve_guest_ptr(A(0) + 4) = m;
                    got = 1;
                } else usleep(20000);
            }
            if (!got) { memset(ve_guest_ptr(A(0)), 0, 28); }   /* WM_NULL 空转 */
            STD(16); c->reg[0] = 1; goto done;   /* 非 0/‑1 = 有消息 */
        }
        if (!strcmp(fn, "PostMessageA")) {
            extern void ve_post_msg(uint32_t m);
            ve_post_msg(A(1));
            STD(16); c->reg[0] = 1; goto done;
        }
        if (!strcmp(fn, "PeekMessageA")) { memset(ve_guest_ptr(A(0)), 0, 28); STD(20); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "TranslateMessage") || !strcmp(fn, "DispatchMessageA")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "DefWindowProcA")) { STD(16); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "SetTimer")) { STD(16); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "KillTimer")) { STD(8); c->reg[0] = 1; goto done; }
    }
    else if (strcasecmp(dll, "WINMM.dll") == 0) {
        ve_log("WINMM %s 被调用 (期望走 eciSetOutputBuffer 旁路)", fn);
        if (!strcmp(fn, "waveOutGetNumDevs")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "waveOutOpen")) { if (A(0)) *(uint32_t *)ve_guest_ptr(A(0)) = 0x666; STD(32); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "waveOutWrite")) { STD(12); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "waveOutPrepareHeader") || !strcmp(fn, "waveOutUnprepareHeader")) { STD(12); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "waveOutReset") || !strcmp(fn, "waveOutClose")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "waveOutGetPosition")) { STD(12); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "timeSetEvent")) { STD(20); c->reg[0] = 1; goto done; }
        if (!strcmp(fn, "timeKillEvent")) { STD(4); c->reg[0] = 0; goto done; }
        if (!strcmp(fn, "timeEndPeriod") || !strcmp(fn, "timeBeginPeriod")) { STD(4); c->reg[0] = 0; goto done; }
    }
    else if (strcasecmp(dll, "MSVCRT.dll") == 0) {
        if (!strcmp(fn, "memcpy")) {
            if (A(2) > 0x40000) ve_log("memcpy 大块: dst=0x%08x src=0x%08x n=0x%x (返回地址 0x%08x)", A(0), A(1), A(2), M32H(c->reg[4] + 0x1000));
            c->reg[0] = A(0); memcpy(ve_guest_ptr(A(0)), ve_guest_ptr(A(1)), A(2)); return 0;
        }
        if (!strcmp(fn, "memmove")) { c->reg[0] = A(0); memmove(ve_guest_ptr(A(0)), ve_guest_ptr(A(1)), A(2)); return 0; }
        if (!strcmp(fn, "memset")) {
            if (A(3) > 0x10000 || ((A(2) & 0xFF) == 0x80 && A(3) > 256)) {
                ve_log("memset 可疑: [esp-4]=0x%08x [esp]=0x%08x [esp+4]=0x%08x [esp+8]=0x%08x [esp+12]=0x%08x [esp+16]=0x%08x",
                       M32H(c->reg[4] - 4), M32H(c->reg[4]), M32H(c->reg[4] + 4),
                       M32H(c->reg[4] + 8), M32H(c->reg[4] + 12), M32H(c->reg[4] + 16));
                ve_log("  → 解释为 dst=0x%08x val=0x%02x n=0x%x", A(0), A(2) & 0xFF, A(3));
            }
            c->reg[0] = A(0); memset(ve_guest_ptr(A(0)), A(1) & 0xFF, A(2)); return 0;
        }
        if (!strcmp(fn, "malloc")) {
            if (A(0) > 0x10000000) {
                /* 超大分配 = 疑似指针当大小 (0x3EFExxxx=栈地址) — 打印调用现场 */
                extern const VeModule *ve_pe_modules(int *);
                int n; const VeModule *ms = ve_pe_modules(&n);
                ve_log("malloc(0x%08x) 可疑! ret=0x%08x", A(0), M32H(c->reg[4]));
                for (int i = 1; i < 40; i++) {
                    uint32_t v = M32H(c->reg[4] + i * 4);
                    for (int k = 0; k < n; k++)
                        if (v >= ms[k].base + 0x1000 && v < ms[k].base + ms[k].size) {
                            ve_log("   栈+0x%02x: %s+0x%x", i * 4, ms[k].name, v - ms[k].base);
                            k = n;
                        }
                }
            }
            c->reg[0] = ve_heap_alloc(A(0));
            return 0;
        }
        if (!strcmp(fn, "calloc")) {
            uint32_t sz = A(0) * A(1);
            if (sz > 0x10000000) ve_log("calloc(0x%x*0x%x) 可疑 ret=0x%08x", A(0), A(1), M32H(c->reg[4]));
            uint32_t p = ve_heap_alloc(sz); if (p) memset(ve_guest_ptr(p), 0, sz); c->reg[0] = p; return 0;
        }
        if (!strcmp(fn, "realloc")) { c->reg[0] = ve_heap_realloc(A(0), A(1)); return 0; }
        if (!strcmp(fn, "free")) { ve_heap_free(A(0)); return 0; }
        if (!strcmp(fn, "??2@YAPAXI@Z")) {
            if (A(0) > 0x10000000) {
                /* 真实调用者返回地址在 [esp-4] (FF/2 压过返回地址又弹掉, 值仍在) */
                ve_log("new(0x%08x) 可疑 caller=0x%08x [esp+4]=0x%08x [esp+8]=0x%08x",
                       A(0), M32H(c->reg[4] - 4), M32H(c->reg[4] + 4), M32H(c->reg[4] + 8));
            }
            c->reg[0] = ve_heap_alloc(A(0));
            return 0;   /* operator new (cdecl — 曾误判 Borland 弹参, 0x1001f7c5 add esp,4 铁证撤销) */
        }
        if (!strcmp(fn, "??3@YAXPAX@Z")) { ve_heap_free(A(0)); return 0; }
        if (!strcmp(fn, "strlen")) { c->reg[0] = (uint32_t)strlen(cp(A(0))); return 0; }
        if (!strcmp(fn, "strcpy")) { c->reg[0] = A(0); strcpy(cp(A(0)), cp(A(1))); return 0; }
        if (!strcmp(fn, "strncpy")) { c->reg[0] = A(0); strncpy(cp(A(0)), cp(A(1)), A(2)); return 0; }
        if (!strcmp(fn, "strcat")) { c->reg[0] = A(0); strcat(cp(A(0)), cp(A(1))); return 0; }
        if (!strcmp(fn, "strncat")) { c->reg[0] = A(0); strncat(cp(A(0)), cp(A(1)), A(2)); return 0; }
        if (!strcmp(fn, "strcmp")) { c->reg[0] = (uint32_t)strcmp(cp(A(0)), cp(A(1))); return 0; }
        if (!strcmp(fn, "strncmp")) { c->reg[0] = (uint32_t)strncmp(cp(A(0)), cp(A(1)), A(2)); return 0; }
        if (!strcmp(fn, "_stricmp")) { c->reg[0] = (uint32_t)strcasecmp(cp(A(0)), cp(A(1))); return 0; }
        if (!strcmp(fn, "strchr")) { char *r = strchr(cp(A(0)), (int)A(1)); c->reg[0] = r ? A(0) + (uint32_t)(r - cp(A(0))) : 0; return 0; }
        if (!strcmp(fn, "strrchr")) { char *r = strrchr(cp(A(0)), (int)A(1)); c->reg[0] = r ? A(0) + (uint32_t)(r - cp(A(0))) : 0; return 0; }
        if (!strcmp(fn, "strstr")) { char *r = strstr(cp(A(0)), cp(A(1))); c->reg[0] = r ? A(0) + (uint32_t)(r - cp(A(0))) : 0; return 0; }
        if (!strcmp(fn, "strtok")) {
            /* guest NULL(0) 必须映射为宿主 NULL; strtok(NULL,..) 是续读状态 */
            char *in = A(0) ? cp(A(0)) : NULL;
            char *r = strtok(in, cp(A(1)));
            extern uintptr_t g_ve_off;
            c->reg[0] = r ? (uint32_t)((uintptr_t)r - g_ve_off) : 0;
            {   static int s_n = 0;
                extern int ve_sscanf_trace;
                if (ve_sscanf_trace > 0 || s_n < 6) {
                    if (s_n < 6) s_n++;
                    ve_log("STRTOK in=0x%08x delim=0x%08x → 0x%08x %s",
                           A(0), A(1), c->reg[0], r ? r : "(null)");
                }
            }
            return 0;
        }
        if (!strcmp(fn, "_strdup")) { const char *s = cp(A(0)); uint32_t p = ve_heap_alloc((uint32_t)strlen(s) + 1); strcpy(cp(p), s); c->reg[0] = p; return 0; }
        if (!strcmp(fn, "isdigit")) { c->reg[0] = (uint32_t)isdigit((int)A(0)); return 0; }
        if (!strcmp(fn, "isspace")) { c->reg[0] = (uint32_t)isspace((int)A(0)); return 0; }
        if (!strcmp(fn, "isalpha")) { c->reg[0] = (uint32_t)isalpha((int)A(0)); return 0; }
        if (!strcmp(fn, "islower")) { c->reg[0] = (uint32_t)islower((int)A(0)); return 0; }
        if (!strcmp(fn, "ispunct")) { c->reg[0] = (uint32_t)ispunct((int)A(0)); return 0; }
        if (!strcmp(fn, "tolower")) { c->reg[0] = (uint32_t)tolower((int)A(0)); return 0; }
        if (!strcmp(fn, "atoi")) { c->reg[0] = (uint32_t)atoi(cp(A(0))); return 0; }
        if (!strcmp(fn, "atol")) { c->reg[0] = (uint32_t)atol(cp(A(0))); return 0; }
        if (!strcmp(fn, "atof")) { double v = atof(cp(A(0))); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = v; return 0; }
        if (!strcmp(fn, "strtol")) { c->reg[0] = (uint32_t)strtol(cp(A(0)), A(1) ? (char **)ve_guest_ptr(A(1)) : NULL, (int)A(2)); return 0; }
        if (!strcmp(fn, "strtod")) {
            char *end;
            double v = strtod(cp(A(0)), A(1) ? (char **)ve_guest_ptr(A(1)) : NULL);
            c->ftop = (c->ftop + 7) & 7;      /* double 返回约定: x87 st0 */
            c->st[c->ftop] = (long double)v;
            c->reg[0] = 0;
            return 0;
        }
        if (!strcmp(fn, "div") || !strcmp(fn, "ldiv")) {
            uint32_t p = ve_heap_alloc(8);
            *(int32_t *)ve_guest_ptr(p) = (int32_t)A(0) / (int32_t)A(1);
            *(int32_t *)ve_guest_ptr(p + 4) = (int32_t)A(0) % (int32_t)A(1);
            c->reg[0] = p;
            return 0;
        }
        if (!strcmp(fn, "rand")) { c->reg[0] = (uint32_t)rand(); return 0; }
        if (!strcmp(fn, "srand")) { srand(A(0)); return 0; }
        if (!strcmp(fn, "time")) { c->reg[0] = (uint32_t)time(NULL); return 0; }
        if (!strcmp(fn, "clock")) { c->reg[0] = (uint32_t)clock(); return 0; }
        if (!strcmp(fn, "_ftime")) { memset(ve_guest_ptr(A(0)), 0, 8); return 0; }
        /* cdecl double 按值传参: 第 n 个 double = 栈上 {A(2n), A(2n+1)} 两个 dword */
#define VD(n) ({ uint64_t _b = (uint64_t)A(2 * (n)) | ((uint64_t)A(2 * (n) + 1) << 32); \
                double _d; memcpy(&_d, &_b, 8); _d; })
        if (!strcmp(fn, "pow")) { double r = pow(VD(0), VD(1)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "exp")) { double r = exp(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "log")) { double r = log(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "log10")) { double r = log10(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "sqrt")) { double r = sqrt(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "fabs")) { double r = fabs(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "floor")) { double r = floor(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "ceil")) { double r = ceil(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "sin")) { double r = sin(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "cos")) { double r = cos(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "atan")) { double r = atan(VD(0)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "atan2")) { double r = atan2(VD(0), VD(1)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "fmod")) { double r = fmod(VD(0), VD(1)); c->ftop = (c->ftop + 7) & 7; c->st[c->ftop] = r; return 0; }
        if (!strcmp(fn, "_ftol")) {
            long double v = c->st[c->ftop];
            c->ftop = (c->ftop + 1) & 7;
            int64_t iv = (int64_t)v;
            c->reg[0] = (uint32_t)iv;
            c->reg[2] = (uint32_t)((uint64_t)iv >> 32);
            return 0;
        }
        if (!strcmp(fn, "_errno")) { extern uint32_t g_ve_errno; c->reg[0] = g_ve_errno; return 0; }
        if (!strcmp(fn, "_iob")) { extern uint32_t g_ve_iob; c->reg[0] = g_ve_iob; return 0; }
        if (!strcmp(fn, "_pctype")) { extern uint32_t g_ve_ctype; c->reg[0] = g_ve_ctype; return 0; }
        if (!strcmp(fn, "__mb_cur_max")) { extern uint32_t g_ve_mbcurmax; c->reg[0] = g_ve_mbcurmax; return 0; }
        if (!strcmp(fn, "_isctype")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "_adjust_fdiv")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "_purecall")) { ve_log("purecall!"); c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "exit")) { ve_log("guest exit(%u)", A(0)); c->eip = 0x0EFFFFFFu; return 0; }
        if (!strcmp(fn, "_ftol") || !strcmp(fn, "__ftol") || !strcmp(fn, "_ftol2") ||
            !strcmp(fn, "_ftol2_sse") || !strcmp(fn, "_CIftol")) {
            /* st0 → 截断 → edx:eax (MSVC x87→int helper) */
            long double v = c->st[c->ftop];
            c->st[c->ftop] = 0;
            c->ftop = (c->ftop + 1) & 7;
            int64_t t = (int64_t)v;
            c->reg[0] = (uint32_t)t;
            c->reg[2] = (uint32_t)((uint64_t)t >> 32);
            return 0;
        }
        if (!strcmp(fn, "?terminate@@YAXXZ")) { ve_log("terminate"); c->eip = 0x0EFFFFFFu; return 0; }
        if (!strcmp(fn, "_initterm")) {
            uint32_t p = A(0);
            while (p < A(1)) {
                uint32_t f = M32H(p);
                if (f) { uint32_t na[1] = { 0 }; ve_call_guest(c, f, 0, na); }
                p += 4;
            }
            c->reg[0] = 0; goto done;
        }
        if (!strcmp(fn, "_onexit") || !strcmp(fn, "__dllonexit")) { c->reg[0] = A(0); return 0; }
        if (!strcmp(fn, "_beginthreadex")) {
            int h = h_alloc(H_THREAD);
            g_stack_top -= 0x100000;
            ThrArg *t = calloc(1, sizeof(ThrArg));
            t->start = A(2); t->arg = A(3);
            t->cpu = calloc(1, sizeof(VeCPU));
            t->cpu->fctrl = 0x027F;
            t->cpu->seg_fs = ve_thread_setup(t->cpu, 0);
            t->cpu->reg[4] = (g_stack_top + 0x100000 - 64) & ~15u;
            t->hid = h;
            g_h[h].exitcode = -1;
            pthread_create(&g_h[h].th, NULL, thread_main, t);
            if (A(5)) *(uint32_t *)ve_guest_ptr(A(5)) = (uint32_t)(uintptr_t)g_h[h].th;
            /* MSVCRT 全 cdecl: 调用方自清 24 字节 (0x1fc40 add esp,0x18 铁证), 垫片不弹 */
            c->reg[0] = (uint32_t)h; goto done;
        }
        if (!strcmp(fn, "_setjmp3")) {
            uint32_t *b = ve_guest_ptr(A(0));
            b[0] = c->reg[3]; b[1] = c->reg[6]; b[2] = c->reg[7];
            b[3] = c->reg[5]; b[4] = c->reg[4];
            b[5] = M32H(c->reg[4]);
            c->reg[0] = 0;
            return 0;
        }
        if (!strcmp(fn, "longjmp")) {
            uint32_t *b = ve_guest_ptr(A(0));
            c->reg[3] = b[0]; c->reg[6] = b[1]; c->reg[7] = b[2];
            c->reg[5] = b[3]; c->reg[4] = b[4] + 4;
            c->eip = b[5];
            c->reg[0] = A(1) ? A(1) : 1;
            return 0;
        }
        if (!strcmp(fn, "_except_handler3") || !strcmp(fn, "__CxxFrameHandler")) {
            c->reg[0] = 1;
            return 0;
        }
        if (!strcmp(fn, "_stat") || !strcmp(fn, "_chmod")) { c->reg[0] = (uint32_t)-1; return 0; }
        if (!strcmp(fn, "_splitpath")) {
            const char *p = cp(A(0));
            const char *b = strrchr(p, '/');
            b = b ? b + 1 : p;
            if (A(3)) strcpy(cp(A(3)), b);
            if (A(4)) strcpy(cp(A(4)), "");
            return 0;
        }
        if (!strcmp(fn, "sprintf") || !strcmp(fn, "vsprintf") || !strcmp(fn, "_snprintf")) {
            char tmp[4096];
            /* FF/2 已弹返回地址: arg1@[esp], arg2@[esp+4], 首变参 arg3@[esp+8] */
            uint32_t varg = !strcmp(fn, "vsprintf") ? A(2) : c->reg[4] + 8;
            int n = guest_vformat(tmp, sizeof(tmp), cp(A(1)), varg);
            {   /* 定点诊断: 音素名生成 */
                if (strstr(cp(A(1)), "Phoneme")) ve_log("SPRINTF → \"%s\"", tmp);
            }
            strcpy(cp(A(0)), tmp);
            c->reg[0] = (uint32_t)n;
            return 0;
        }
        if (!strcmp(fn, "vfprintf") || !strcmp(fn, "fprintf")) {
            char tmp[4096];
            uint32_t varg = !strcmp(fn, "vfprintf") ? A(2) : c->reg[4] + 8;
            guest_vformat(tmp, sizeof(tmp), cp(A(1)), varg);
            ve_log("CRT: %s", tmp);
            return 0;
        }
        if (!strcmp(fn, "sscanf")) {
            /* 通用子集: %[*][width][l/h] d u x X o f e g s c n —
             * 引擎用 "%d %d ..." 解析注册表音素/语音参数串 (15 个数) */
            char *ip = cp(A(0));
            const char *f = cp(A(1));
            int next_arg = 2, matched = 0;
            int traced = 0;
            {   /* 定点诊断: 前 40 条 + 显式请求的都记 */
                static int s_always = 0;
                extern int ve_sscanf_trace;
                if (ve_sscanf_trace > 0) {
                    ve_sscanf_trace--;
                    ve_log("SSCANF @0x%08x in=\"%.60s\" fmt=\"%.60s\"", c->eip, ip, f);
                    traced = 1;
                } else if (s_always < 40 && !strstr(f, "%hd")) {   /* 跳过音素串 */
                    s_always++;
                    ve_log("SSCANF @0x%08x in=\"%.60s\" fmt=\"%.60s\"", c->eip, ip, f);
                    traced = 1;
                }
            }
            const char *p = f;
            while (*p) {
                if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                    while (*ip == ' ' || *ip == '\t' || *ip == '\n' || *ip == '\r') ip++;
                    p++;
                    continue;
                }
                if (*p != '%') {
                    if (*ip != *p) break;
                    ip++; p++;
                    continue;
                }
                p++;
                int suppress = 0, width = 0, lng = 0;
                if (*p == '*') { suppress = 1; p++; }
                while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
                while (*p == 'l' || *p == 'h' || *p == 'I') {
                    if (*p == 'l' && *(p + 1) == 'l') { lng = 2; p++; }
                    else if (*p == 'l') lng = 1;   /* Win32: long=4字节 */
                    p++;
                }
                char conv = *p;
                if (!conv) break;
                p++;
                if (conv == 'n') {
                    if (!suppress) {
                        uint32_t ap = A(next_arg++);
                        if (ap) *(int32_t *)ve_guest_ptr(ap) = (int32_t)(ip - cp(A(0)));
                    }
                    continue;
                }
                if (conv == 'c') {
                    int n = width ? width : 1;
                    if (!*ip && n > 0) break;
                    if (!suppress) {
                        uint32_t ap = A(next_arg++);
                        char *d = ap ? (char *)ve_guest_ptr(ap) : NULL;
                        for (int i = 0; i < n; i++) { if (d) *d++ = *ip; ip++; }
                    } else ip += n;
                    matched++;
                    continue;
                }
                /* 其余转换先跳输入空白 */
                while (*ip == ' ' || *ip == '\t' || *ip == '\n' || *ip == '\r') ip++;
                if (conv == 's') {
                    if (!*ip) break;
                    uint32_t ap = suppress ? 0 : A(next_arg++);
                    char *d = ap ? (char *)ve_guest_ptr(ap) : NULL;
                    int n = 0;
                    while (*ip && *ip != ' ' && *ip != '\t' && *ip != '\n' &&
                           (!width || n < width)) {
                        if (d) *d++ = *ip;
                        ip++; n++;
                    }
                    if (d) *d = 0;
                    matched++;
                    continue;
                }
                if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                    char *end;
                    int base = (conv == 'd' || conv == 'u' || conv == 'i') ? 10 : (conv == 'o' ? 8 : 16);
                    long v = strtol(ip, &end, base);
                    if (end == ip) break;
                    ip = end;
                    if (!suppress) {
                        uint32_t ap = A(next_arg++);
                        if (ap) {
                            /* Win32: long=4字节, 只有 %lld 才写 8 */
                            if (lng >= 2) *(int64_t *)ve_guest_ptr(ap) = (int64_t)v;
                            else *(int32_t *)ve_guest_ptr(ap) = (int32_t)v;
                        }
                    }
                    matched++;
                    continue;
                }
                if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G') {
                    char *end;
                    double v = strtod(ip, &end);
                    if (end == ip) break;
                    ip = end;
                    if (!suppress) {
                        uint32_t ap = A(next_arg++);
                        if (ap) {
                            if (lng >= 2) *(double *)ve_guest_ptr(ap) = v;
                            else *(float *)ve_guest_ptr(ap) = (float)v;
                        }
                    }
                    matched++;
                    continue;
                }
                break;   /* 未知转换 (如 %[) */
            }
            if (traced) ve_log("SSCANF → matched=%d eax=%u", matched, (uint32_t)matched);
            c->reg[0] = (uint32_t)matched;
            return 0;
        }
        if (!strcmp(fn, "fopen") || !strcmp(fn, "_wfopen")) {
            /* 返回 guest 空间伪 FILE (宿主 FILE* 是 48 位地址, 进 guest 即炸弹) */
            const char *path = cp(A(0)), *mode = cp(A(1));
            char full[1024];
            extern char g_ve_dir[512];
            const char *bs = strrchr(path, 92);   /* backslash */
            const char *fs2 = strrchr(path, '/');
            const char *base = bs > fs2 ? bs + 1 : (fs2 ? fs2 + 1 : path);
            if (path[0] == '/') snprintf(full, sizeof(full), "%s", path);
            else snprintf(full, sizeof(full), "%s/%s", g_ve_dir, base);
            FILE *fp = fopen(full, mode);
            if (!fp) { c->reg[0] = 0; return 0; }
            int slot = -1;
            for (int i = 0; i < 128; i++) if (!g_fhost[i]) { slot = i; break; }
            if (slot < 0) { fclose(fp); c->reg[0] = 0; return 0; }
            g_fhost[slot] = fp;
            uint32_t gfile = GFILE_BASE + (uint32_t)slot * 64;
            memset(ve_guest_ptr(gfile), 0, 64);
            c->reg[0] = gfile;
            return 0;
        }
        if (!strcmp(fn, "fclose")) {
            FILE *fp = NULL;
            if (ve_fhost(A(0), &fp) && fp) { fclose(fp); ve_fmark_free(A(0)); }
            return 0;
        }
        if (!strcmp(fn, "fread")) { FILE *fp; if (!ve_fhost(A(3), &fp)) { c->reg[0] = 0; return 0; } c->reg[0] = (uint32_t)fread(ve_guest_ptr(A(0)), A(1), A(2), fp); return 0; }
        if (!strcmp(fn, "fwrite")) { FILE *fp; if (!ve_fhost(A(3), &fp)) { c->reg[0] = 0; return 0; } c->reg[0] = (uint32_t)fwrite(ve_guest_ptr(A(0)), A(1), A(2), fp); return 0; }
        if (!strcmp(fn, "fseek")) { FILE *fp; if (!ve_fhost(A(0), &fp)) { c->reg[0] = 0; return 0; } c->reg[0] = (uint32_t)fseek(fp, (long)A(1), (int)A(2)); return 0; }
        if (!strcmp(fn, "ftell")) { FILE *fp; if (!ve_fhost(A(0), &fp)) { c->reg[0] = 0; return 0; } c->reg[0] = (uint32_t)ftell(fp); return 0; }
        if (!strcmp(fn, "rewind")) { FILE *fp; if (ve_fhost(A(0), &fp)) rewind(fp); return 0; }
        if (!strcmp(fn, "fflush")) { FILE *fp; if (ve_fhost(A(0), &fp) && fp) fflush(fp); return 0; }
        if (!strcmp(fn, "fgets")) { FILE *fp; if (!ve_fhost(A(2), &fp)) { c->reg[0] = 0; return 0; } char *r = fgets(cp(A(0)), (int)A(1), fp); c->reg[0] = r ? A(0) : 0; return 0; }
        if (!strcmp(fn, "fgetc")) { FILE *fp; if (!ve_fhost(A(0), &fp)) { c->reg[0] = (uint32_t)-1; return 0; } c->reg[0] = (uint32_t)fgetc(fp); return 0; }
        if (!strcmp(fn, "fputs")) { FILE *fp; if (!ve_fhost(A(1), &fp)) { c->reg[0] = 0; return 0; } c->reg[0] = (uint32_t)fputs(cp(A(0)), fp); return 0; }
        if (!strcmp(fn, "fclose")) { fclose((FILE *)(uintptr_t)A(0)); return 0; }
        if (!strcmp(fn, "fgetws")) { c->reg[0] = 0; return 0; }
        if (!strcmp(fn, "getchar")) { c->reg[0] = (uint32_t)-1; return 0; }
        if (!strcmp(fn, "perror")) { return 0; }
        if (!strcmp(fn, "qsort")) {
            /* 插入排序: 暂存元素放 guest 堆临时块 (比较器只认 guest 地址) */
            uint32_t base = A(0), n = A(1), sz = A(2), cmp = A(3);
            uint8_t *tmp = malloc(sz);
            uint32_t scratch = ve_heap_alloc(sz);
            for (uint32_t i = 1; i < n; i++) {
                memcpy(tmp, ve_guest_ptr(base + i * sz), sz);
                if (scratch) memcpy(ve_guest_ptr(scratch), tmp, sz);
                int32_t j = (int32_t)i - 1;
                while (j >= 0) {
                    uint32_t args[2] = { base + (uint32_t)j * sz,
                                         scratch ? scratch : base + i * sz };
                    ve_call_guest(c, cmp, 2, args);
                    if ((int32_t)c->reg[0] <= 0) break;
                    memcpy(ve_guest_ptr(base + (uint32_t)(j + 1) * sz),
                           ve_guest_ptr(base + (uint32_t)j * sz), sz);
                    j--;
                }
                memcpy(ve_guest_ptr(base + (uint32_t)(j + 1) * sz), tmp, sz);
            }
            free(tmp);
            if (scratch) ve_heap_free(scratch);
            return 0;
        }
        if (!strcmp(fn, "bsearch")) {
            uint32_t key = A(0), base = A(1), n = A(2), sz = A(3), cmp = A(4);
            uint32_t lo = 0, hi = n;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                uint32_t args[2] = { key, base + mid * sz };
                ve_call_guest(c, cmp, 2, args);
                int32_t r = (int32_t)c->reg[0];
                if (r == 0) { c->reg[0] = base + mid * sz; return 0; }
                if (r < 0) hi = mid; else lo = mid + 1;
            }
            c->reg[0] = 0;
            return 0;
        }
    }
    else if (strcasecmp(dll, "MSVCIRT.dll") == 0) {
        static int warned = 0;
        if (!warned) { ve_log("MSVCIRT %s (iostream 垫片 this 透传)", fn); warned = 1; }
        c->reg[0] = A(0);
        return 0;
    }
    else if (strcasecmp(dll, "ibmcmm.dll") == 0) {
        ve_log("ibmcmm %s 被调用", fn);
        c->reg[0] = 0;
        return 0;
    }

    ve_log("垫片未实现 %s!%s → 返回 0", dll, fn);
    c->reg[0] = 0;
done:
    return pop;
}

void SYSTEMTIME_FILL(void *p) {
    uint16_t *t = p;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    t[0] = (uint16_t)(tm.tm_year + 1900); t[1] = (uint16_t)(tm.tm_mon + 1); t[2] = (uint16_t)tm.tm_wday;
    t[3] = (uint16_t)tm.tm_mday; t[4] = (uint16_t)tm.tm_hour; t[5] = (uint16_t)tm.tm_min;
    t[6] = (uint16_t)tm.tm_sec; t[7] = 0;
}

/* ---- 简单跨线程消息队列 (PostMessage → GetMessage) ---- */
static uint32_t g_msgq[16];
static int g_msgq_n = 0;
static pthread_mutex_t g_msgq_mu = PTHREAD_MUTEX_INITIALIZER;
void ve_post_msg(uint32_t m) {
    pthread_mutex_lock(&g_msgq_mu);
    if (g_msgq_n < 16) g_msgq[g_msgq_n++] = m;
    pthread_mutex_unlock(&g_msgq_mu);
}
int ve_pop_msg(uint32_t *m) {
    pthread_mutex_lock(&g_msgq_mu);
    if (g_msgq_n == 0) { pthread_mutex_unlock(&g_msgq_mu); return 0; }
    *m = g_msgq[0];
    g_msgq_n--;
    memmove(g_msgq, g_msgq + 1, g_msgq_n * 4);
    pthread_mutex_unlock(&g_msgq_mu);
    return 1;
}

int ve_ctor_trace = 1;

uint32_t g_watch_addr = 0;

int ve_shim_name(int idx, char *buf, int cap) {
    if (idx < 0 || idx >= g_nshims) { snprintf(buf, cap, "?%d", idx); return 0; }
    snprintf(buf, cap, "%s!%s", g_shims[idx].dll, g_shims[idx].func);
    return 1;
}

int ve_reg_trace = 1;

int ve_call_probe = 1;
