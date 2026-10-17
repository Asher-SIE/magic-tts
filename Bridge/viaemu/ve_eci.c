/* viaemu ECI 宿主: 加载 ViaVoice 模块 → DllMain → eciNew/AddText/Synthesize 闭环.
 * 音频路径: eciSetOutputBuffer + 回调计数 (回调以 thunk 槽注册, 引擎调用时
 * 落入 ve_win 分发 → g_eci_cb_hook). 文本经 UTF-8→GBK 转换 (CoreFoundation).
 */
#include "viaemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>

char g_ve_dir[512];
uint32_t g_ve_errno, g_ve_iob, g_ve_ctype, g_ve_mbcurmax;
int ve_region_init(void);

/* ECI 回调钩子 (ve_win 的 __ve_eci_cb 槽转发到这里) */
int (*g_eci_cb_hook)(VeCPU *c, uint32_t msg, uint32_t lp, uint32_t wp) = NULL;

static VeCPU *g_cpu;                    /* 主 (合成) 线程 CPU */
static pthread_mutex_t g_eci_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_loaded;
static uint32_t f_eciNew, f_eciNewEx, f_eciDelete, f_eciAddText, f_eciSynthesize,
    f_eciSynchronize, f_eciSetOutputBuffer, f_eciRegisterCallback,
    f_eciStop, f_eciSpeaking, f_eciSetParam, f_eciGetParam,
    f_eciGetAvailableLanguages, f_eciGetVoiceName, f_eciCopyVoice, f_eciReset, f_eciSetVoiceParam;

/* 每线程 TEB 区: 主 0x0D000000, 辅线程向下排布 */
uint32_t ve_thread_setup(VeCPU *c, int is_main) {
    static uint32_t next_teb = 0x0D000000;
    uint32_t teb = next_teb;
    next_teb += 0x1000;
    (void)is_main;
    memset(ve_guest_ptr(teb), 0, 0x1000);
    uint32_t *t = ve_guest_ptr(teb);
    t[0] = 0xFFFFFFFF;         /* SEH 链头: 空 */
    *(uint32_t *)ve_guest_ptr(teb + 0x18) = teb;      /* self */
    *(uint32_t *)ve_guest_ptr(teb + 0x30) = 0x0D100000; /* PEB */
    c->seg_fs = teb;
    return teb;
}

/* ---- UTF-8 → GBK ---- */
static int utf8_to_gbk(const char *src, uint8_t *dst, int cap) {
    CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault,
                                            (const UInt8 *)src, (CFIndex)strlen(src),
                                            kCFStringEncodingUTF8, false);
    if (!s) return -1;
    CFIndex used = 0;
    CFIndex n = CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)),
                                 kCFStringEncodingGB_18030_2000, 0, false,
                                dst, cap - 1, &used);
    CFRelease(s);
    if (n == 0 && used == 0) { /* 转换失败 */ return -1; }
    dst[used] = 0;
    return (int)used;
}

/* ---- ECI 回调 (thunk 槽) ---- */
static uint32_t g_synth_samples;
static int eci_cb_host(VeCPU *c, uint32_t msg, uint32_t lp, uint32_t wp) {
    (void)c;
    if (msg == 0) {   /* eciWaveformBuffer = 0: lp = 本块样本数 */
        g_synth_samples += lp;
    }
    (void)wp;
    return 0;         /* ECIContinue */
}

int viaemu_load(const char *dir) {
    if (g_loaded) return 0;
    if (ve_region_init()) { ve_log("guest 区预留失败"); return -1; }
    snprintf(g_ve_dir, sizeof(g_ve_dir), "%s", dir);

    /* 静态数据区 */
    g_ve_errno = 0x0D200000;
    g_ve_iob = 0x0D210000;          /* 3×64B 伪 FILE */
    g_ve_ctype = 0x0D220000;        /* ctype 表 */
    g_ve_mbcurmax = 0x0D230000;
    memset(ve_guest_ptr(g_ve_errno), 0, 0x20000);
    strcpy((char *)ve_guest_ptr(g_ve_mbcurmax), "\x02");   /* mb_cur_max 值按数据放: 引擎按 int* 读 */
    *(uint32_t *)ve_guest_ptr(g_ve_mbcurmax) = 2;
    for (int i = 0; i < 256; i++) {
        uint16_t v = 0;
        if (isalpha(i)) v |= 0x0103;   /* _ALPHA|_LOWER|_UPPER 近似 */
        if (isdigit(i)) v |= 0x0204;
        if (isspace(i)) v |= 0x2040;
        if (ispunct(i)) v |= 0x0010;
        *(uint16_t *)ve_guest_ptr(g_ve_ctype + i * 2) = v;
    }
    strcpy((char *)ve_guest_ptr(0x20000004), "viaemu \"MagicTTS\"");

    ve_shim_init();
    /* 注册内部回调槽 */
    ve_shim_slot("__internal", "__ve_eci_cb");
    g_eci_cb_hook = eci_cb_host;

    /* 载入全部模块 (基址 64MB 步进) */
    struct { const char *file, *name; } mods[] = {
        { "ibmcmm.dll", "ibmcmm" },
        { "chs.uil", "chs.uil" },
        { "chsrom.dll", "chsrom" },
        { "chs50.syn", "chs50.syn" },
        { "torrent.dll", "torrent" },
        { "ibmeci.dll", "ibmeci" },
    };
    char path[1024];
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, mods[i].file);
        uint32_t b = ve_pe_load(path, 0x10000000u + (uint32_t)i * 0x04000000u, mods[i].name);
        if (!b) { ve_log("加载 %s 失败", mods[i].file); return -2; }
        ve_log("已加载 %-10s @0x%08x", mods[i].name, b);
    }

    /* 主线程 CPU + 栈 */
    g_cpu = calloc(1, sizeof(VeCPU));
    memset(g_cpu, 0, sizeof(VeCPU));
    ve_thread_setup(g_cpu, 1);
    g_cpu->reg[4] = 0x3EFF0000u;   /* esp */
    g_cpu->fctrl = 0x027F;
    ve_win_thread_attach(g_cpu);

    /* DllMain(base, DLL_PROCESS_ATTACH=1, NULL) — 依执行序: 工具→数据→主库.
     * ibmcmm/torrent (Cmm IPC 层) 的 DllMain 会等待跨进程服务 → 同进程模式跳过. */
    const VeModule *ms = ve_pe_modules(NULL);
    for (int i = 0; i < 6; i++) {
        if (!ms[i].entry) continue;
        if (!strcmp(ms[i].name, "ibmcmm") || !strcmp(ms[i].name, "torrent")) {
            ve_log("DllMain %s 跳过 (Cmm IPC 层, 同进程不用)", ms[i].name);
            continue;
        }
        uint32_t args[3] = { ms[i].base, 1, 0 };
        ve_log("DllMain %s ...", ms[i].name);
        uint32_t rc = ve_call_guest(g_cpu, ms[i].entry, 3, args);
        if (rc != 0) { ve_log("DllMain %s rc=%u eax=0x%08x (非零未必致命)", ms[i].name, rc, g_cpu->reg[0]); }
    }

    f_eciNew = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciNew");
    f_eciNewEx = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciNewEx");
    f_eciDelete = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciDelete");
    f_eciAddText = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciAddText");
    f_eciSynthesize = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciSynthesize");
    f_eciSynchronize = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciSynchronize");
    f_eciSetOutputBuffer = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciSetOutputBuffer");
    f_eciRegisterCallback = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciRegisterCallback");
    f_eciStop = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciStop");
    f_eciSpeaking = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciSpeaking");
    f_eciSetParam = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciSetParam");
    f_eciGetParam = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciGetParam");
    f_eciGetAvailableLanguages = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciGetAvailableLanguages");
    f_eciGetVoiceName = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciGetVoiceName");
    f_eciCopyVoice = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciCopyVoice");
    f_eciReset = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciReset");
    f_eciSetVoiceParam = ve_pe_export(0x10000000u + 5 * 0x04000000u, "eciSetVoiceParam");
    if (!f_eciNew || !f_eciAddText || !f_eciSynthesize || !f_eciSetOutputBuffer) {
        ve_log("ECI 导出解析失败");
        return -3;
    }
    g_loaded = 1;
    ve_pe_iat_check();
    ve_log("viaemu 就绪 (eciNew@0x%08x)", f_eciNew);
    return 0;
}

int viaemu_eci_synth(const char *text_utf8, int voice,
                     int rate, int pitch,
                     int16_t *out16, int out_cap,
                     int *out_sample_rate) {
    if (!g_loaded) return -1;
    (void)voice; (void)rate; (void)pitch;

    uint8_t gbk[8192];
    int gl = utf8_to_gbk(text_utf8, gbk, sizeof(gbk));
    if (gl < 0) return -2;
    uint32_t gtext = ve_heap_alloc((uint32_t)gl + 1);
    memcpy(ve_guest_ptr(gtext), gbk, gl + 1);

    static uint32_t pcm_buf = 0;
    if (!pcm_buf) pcm_buf = ve_heap_alloc(0x200000);   /* 2M 样本 */
    else memset(ve_guest_ptr(pcm_buf), 0, 0x400000);

    pthread_mutex_lock(&g_eci_lock);
    g_synth_samples = 0;

    uint32_t hand = 0;
    if (f_eciNewEx) {
        /* NVDA IBMTTS 驱动规范: MandarinChinese(GB) = 0x00060000 */
        uint32_t an[1] = { 0x00060000u };
        ve_call_guest(g_cpu, f_eciNewEx, 1, an);
        hand = g_cpu->reg[0];
    }
    if (!hand && f_eciNew) {
        uint32_t args[1] = { 0 };
        ve_call_guest(g_cpu, f_eciNew, 0, args);
        hand = g_cpu->reg[0];
    }
    if (!hand) { ve_log("eciNew(Ex) 返回 NULL"); pthread_mutex_unlock(&g_eci_lock); return -4; }

    /* 32 位签名: eciSetOutputBuffer(handle, numSamples, buffer) */
    uint32_t a2[3] = { hand, 0x100000, pcm_buf };
    ve_call_guest(g_cpu, f_eciSetOutputBuffer, 3, a2);

    if (f_eciSetVoiceParam) {
        /* NVDA 驱动刻度: speed 40..250 (def 100?), volume 0..100, pitch 0..100 */
        uint32_t av[4] = { hand, 0, 6, (uint32_t)(rate & 0xFF) };
        ve_call_guest(g_cpu, f_eciSetVoiceParam, 4, av);
        uint32_t ap[4] = { hand, 0, 2, (uint32_t)(pitch & 0xFF) };
        ve_call_guest(g_cpu, f_eciSetVoiceParam, 4, ap);
    }
    if (f_eciRegisterCallback) {
        void *cb = ve_shim_slot("__internal", "__ve_eci_cb");
        uint32_t a3[2] = { hand, (uint32_t)(uintptr_t)cb };
        ve_call_guest(g_cpu, f_eciRegisterCallback, 2, a3);
    }

    uint32_t a4[2] = { hand, gtext };
    ve_call_guest(g_cpu, f_eciAddText, 2, a4);

    uint32_t a5[1] = { hand };
    ve_call_guest(g_cpu, f_eciSynthesize, 1, a5);
    ve_call_guest(g_cpu, f_eciSynchronize, 1, a5);

    uint32_t got = g_synth_samples;
    if (got == 0) {
        /* 回调未触发 — 兜底: 扫描缓冲非零 (引擎可能直接填充) */
        int16_t *p = ve_guest_ptr(pcm_buf);
        while (got < 0x100000 && p[got] != 0) got++;
    }
    if (got > (uint32_t)out_cap) got = (uint32_t)out_cap;
    memcpy(out16, ve_guest_ptr(pcm_buf), got * 2);

    ve_call_guest(g_cpu, f_eciDelete, 1, a5);
    pthread_mutex_unlock(&g_eci_lock);

    if (out_sample_rate) *out_sample_rate = 11025;   /* ViaVoice 默认 11kHz mono */
    ve_log("eci synth: %u 样本 (%.2fs @11k)", got, got / 11025.0);
    return (int)got;
}

const char *viaemu_eci_version(void) { return "IBM ViaVoice TTS (viaemu)"; }
