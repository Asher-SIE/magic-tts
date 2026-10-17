/* viaemu 内存: guest 恒等映射区 + guest 堆 */
#include "viaemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

/* ---- 崩溃自诊断 (无调试器环境: lldb 需 Developer Tools 授权) ---- */
static void ve_fault_handler(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    extern VeCPU *ve_win_cpu(void);
    VeCPU *c = ve_win_cpu();
    if (c) {
        uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
        ve_log("宿主 fault sig=%d addr=0x%llx | guest_eip=0x%08x esp=0x%08x seg_fs=0x%08x seg_add=0x%08x guest_addr=0x%llx",
               sig, fa, c->eip, c->reg[4], c->seg_fs, c->seg_add,
               fa >= g_ve_off ? fa - g_ve_off : 0ull);
        /* 打印 guest 栈上疑似返回地址 (定位错位 ret) */
        for (int i = 0; i < 6; i++) {
            uint32_t v = *(uint32_t *)(c->reg[4] + i * 4 + g_ve_off);
            ve_log("  [esp+0x%x] = 0x%08x", i * 4, v);
        }
    } else {
        ve_log("宿主 fault sig=%d addr=%p (无 CPU 上下文)", sig, si->si_addr);
    }
    _exit(99);
}

int ve_debug = 0;

#define GUEST_BASE 0x00400000u
#define GUEST_SIZE 0x3F400000u   /* 4MB..0x3F800000: DLL/堆/栈 全在内 */

static uint8_t *g_region;
uintptr_t g_ve_off;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void ve_log(const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&g_log_lock);
    fprintf(stderr, "[viaemu] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&g_log_lock);
}

void *ve_guest_ptr(uint32_t addr) {
    if (!g_region) return NULL;
    if (addr < GUEST_BASE || addr >= GUEST_BASE + GUEST_SIZE) {
        ve_log("guest 指针越界 0x%08x", addr);
        return NULL;
    }
    return g_region + (addr - GUEST_BASE);
}

int ve_region_init(void) {
    if (g_region) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ve_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    void *p = mmap(NULL, GUEST_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE,
                   -1, 0);
    if (p == MAP_FAILED) return -1;
    g_region = p;
    g_ve_off = (uintptr_t)p - GUEST_BASE;
    memset(g_region, 0, 0x1000);
    return 0;
}

/* ---- guest 堆: 简单 first-fit + 空闲链 ---- */
#define HEAP_START 0x16000000u
#define HEAP_END   0x2F000000u
#define BLOCK_HDR  8u   /* size | flags(1=used) */

typedef struct { uint32_t next, prev; } _Freelist;

static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t ve_heap_alloc(uint32_t size) {
    if (!g_region) return 0;
    if (size == 0) size = 1;
    size = (size + BLOCK_HDR + 15) & ~15u;

    pthread_mutex_lock(&g_heap_lock);
    static int inited = 0;
    if (!inited) {
        uint32_t *h = ve_guest_ptr(HEAP_START);
        h[0] = HEAP_END - HEAP_START;   /* size */
        h[1] = 0;                       /* free */
        inited = 1;
    }
    /* first-fit 扫描 */
    uint32_t p = HEAP_START;
    while (p < HEAP_END - 64) {
        uint32_t *h = ve_guest_ptr(p);
        uint32_t sz = h[0];
        if (!(h[1] & 1) && sz >= size) {
            if (sz >= size + 32) {   /* 分裂 */
                uint32_t *n = ve_guest_ptr(p + size);
                n[0] = sz - size;
                n[1] = 0;
            } else {
                size = sz;
            }
            h[0] = size;
            h[1] = 1;
            pthread_mutex_unlock(&g_heap_lock);
            return p + BLOCK_HDR;
        }
        p += sz;
    }
    pthread_mutex_unlock(&g_heap_lock);
    ve_log("guest 堆耗尽 (%u 字节)", size);
    return 0;
}

void ve_heap_free(uint32_t p) {
    if (!p || !g_region) return;
    p -= BLOCK_HDR;
    if (p < HEAP_START || p >= HEAP_END) return;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t *h = ve_guest_ptr(p);
    if (!(h[1] & 1)) { pthread_mutex_unlock(&g_heap_lock); return; }
    h[1] = 0;
    /* 与后块合并 */
    for (;;) {
        uint32_t *h2 = ve_guest_ptr(p);
        uint32_t nxt = p + h2[0];
        if (nxt >= HEAP_END - 64) break;
        uint32_t *hn = ve_guest_ptr(nxt);
        if (hn[1] & 1) break;
        h2[0] += hn[0];
    }
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t ve_heap_realloc(uint32_t p, uint32_t size) {
    if (!p) return ve_heap_alloc(size);
    if (!size) { ve_heap_free(p); return 0; }
    uint32_t *h = ve_guest_ptr(p - BLOCK_HDR);
    uint32_t old = h[0] - BLOCK_HDR;
    if (size <= old) return p;
    uint32_t n = ve_heap_alloc(size);
    if (!n) return 0;
    memcpy(ve_guest_ptr(n), ve_guest_ptr(p), old);
    ve_heap_free(p);
    return n;
}
