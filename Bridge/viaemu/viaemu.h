/* viaemu — 进程内 i386 PE 模拟层: 让 IBM ViaVoice TTS 的 Windows DLL
 * 在 macOS/ARM64 进程内直接运行 (解释执行, 无 JIT, 无 exec 权限需求).
 *
 * 布局 (guest 虚拟地址 == 宿主指针, 恒等映射):
 *   0x0F000000..0x0FF00000  thunk 槽 (导入函数编号)
 *   0x10000000..            DLL 映像 (每 64MB 一个)
 *   0x16000000..0x30000000  guest 堆 (HeapAlloc/malloc)
 *   0x3F000000 向下          主线程栈
 *   区域整体 1.5GB RW MAP_NORESERVE 预留
 */
#ifndef VIAEMU_H
#define VIAEMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* guest 地址 → 宿主指针: 加法偏移 (macOS arm64 无低 4GB 固定映射, 恒等不可行) */
extern uintptr_t g_ve_off;
#define GADDR(a) ((void *)(uintptr_t)((uint32_t)(a) + g_ve_off))

/* ---- 初始化 ---- */
/* 加载 <dir> 下的 ViaVoice DLL (ibmeci/chs50/chsrom/chs.uil[, torrent/ibmcmm]),
 * 跑 DllMain, 解析 ECI 导出. 失败返回 <0. 进程内一次. */
int viaemu_load(const char *dir);

/* ---- ECI 宿主接口 (线程安全: 内部全局锁, ViaVoice 引擎本身按单会话设计) ---- */
/* 合成一次: text 为 GB2312/GBK 编码字节串. 输出 16bit PCM mono.
 * out16 容量 out_cap 样本; 返回实际样本数, <0 失败.
 * voice/rate/pitch: ECI 参数刻度 (voice=0 默认; rate 0..100? 以 ECI_PARAM 为准). */
int viaemu_eci_synth(const char *text_gbk, int voice,
                     int rate, int pitch,
                     int16_t *out16, int out_cap,
                     int *out_sample_rate);

/* 引擎版本串 (eciVersion), 调试用 */
const char *viaemu_eci_version(void);

/* ---- 内部 (供 ve_*.c 互调, 外部勿用) ---- */
typedef struct VeCPU VeCPU;
struct VeCPU {
    uint32_t reg[8];        /* eax ecx edx ebx esp ebp esi edi */
    uint32_t eip;
    uint32_t flags;         /* 实际只用低 12 位 + ID */
    long double st[8];      /* x87 栈 (host long double = IEEE quad, 精度≥80bit) */
    uint32_t ftop;
    uint16_t fstat, fctrl;  /* x87 状态/控制字 (条件码 C0..C3 维护于 fstat) */
    uint32_t seg_fs;        /* FS 基 (TEB) */
    uint32_t seg_add;       /* 段前缀附加基址 (fs→TEB), modrm 消费后清零 */
    int depth;              /* 重入深度 (shim→guest 回调) */
};

void *ve_guest_ptr(uint32_t addr);
/* 测试便捷别名 */
static inline void *viaemu_guest(uint32_t a) { return (void *)(uintptr_t)((uint32_t)a + g_ve_off); }          /* guest 地址 → 宿主指针 (边界检查) */
uint32_t ve_cpu_run(VeCPU *c, uint32_t stop_esp);  /* 执行直到 ret 到 stop_esp 层 */

/* PE 加载 */
typedef struct VeModule {
    uint32_t base;
    uint32_t size;
    uint32_t entry;         /* DllMain 绝对地址 (0=无) */
    char name[16];
} VeModule;
const VeModule *ve_pe_modules(int *count);
void ve_pe_iat_check(void);   /* IAT 体检: 报告异常槽 */
uint32_t ve_pe_load(const char *path, uint32_t want_base, const char *name);
uint32_t ve_pe_export(uint32_t base, const char *func);

/* 垫片 */
void ve_shim_init(void);
void *ve_shim_slot(const char *dll, const char *func);  /* 返回 thunk 地址 */
/* 带导入方模块的槽位 (同名导入按模块区分约定, 如 Borland ibmeci 的 new 弹栈) */
void *ve_shim_slot_mod(const char *dll, const char *func, const char *mod);
void ve_win_set_cpu(VeCPU *c);                           /* 当前线程 CPU 上下文 */
VeCPU *ve_win_cpu(void);
void ve_win_thread_attach(VeCPU *c);
uint32_t ve_call_guest(VeCPU *c, uint32_t func, int nargs, const uint32_t *args);

/* guest 堆 */
uint32_t ve_heap_alloc(uint32_t size);
void ve_heap_free(uint32_t p);
uint32_t ve_heap_realloc(uint32_t p, uint32_t size);

/* 日志 */
void ve_log(const char *fmt, ...);
extern int ve_debug;

#ifdef __cplusplus
}
#endif
#endif
