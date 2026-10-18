/* 流式合成 CLI 测试: 列出发音人 / 指定发音人合成, 计量首块延迟.
 *
 * 编译 (在 scripts/ 下; kona 为 ObjC, CLI 用桩替代):
 *   clang -O2 -I../Bridge/include -o /tmp/mtts_cli test_cli.c \
 *     ../Bridge/engine_router.c ../Bridge/yongde_adapter.c ../Bridge/ms_embedded_adapter.c \
 *     ../Bridge/sonic.c /tmp/kona_stub.c -lpthread -ldl
 * 用法:
 *   /tmp/mtts_cli                      # 列出全部引擎与发音人
 *   /tmp/mtts_cli <引擎key> [发音人key|序号] [文本] [语速20-500] [音高0-100]
 */
#include "engine_bridge.h"
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

static void crash_handler(int sig) {
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "\n=== SIG%d 崩溃, 回溯: ===\n", sig);
    backtrace_symbols_fd(bt, n, 2);
    _exit(88);
}

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static void write_wav(const char *path, const int16_t *pcm, int n, int sr) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int32_t data = n * 2;
    fwrite("RIFF", 1, 4, f); int32_t sz = 36 + data; fwrite(&sz, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); int32_t f16 = 16; fwrite(&f16, 4, 1, f);
    int16_t f1 = 1, ch = 1, bits = 16; int32_t br = sr * 2; int16_t ba = 2;
    fwrite(&f1, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv) {
    signal(SIGSEGV, crash_handler);
    signal(SIGBUS, crash_handler);
    setbuf(stdout, NULL);
    const char *root = getenv("MTTS_ROOT");
    char defroot[1024];
    if (!root) {
        snprintf(defroot, sizeof(defroot), "%s/Library/Containers/com.magictts.voices.synth/Data/Engines",
                 getenv("HOME"));
        root = defroot;
    }
    int loaded = mtts_global_init(root);
    printf("已加载引擎数: %d\n", loaded);

    if (argc < 2) {
        for (int i = 0; i < mtts_engine_count(); i++) {
            const mtts_engine_desc *d = mtts_engine_at(i);
            int vc = mtts_voice_count(d->key);
            printf("[%s] %s (%s) %s — %d 发音人:\n", d->key, d->voice_name, d->identifier,
                   mtts_engine_loaded(d->key) ? "READY" : "未就绪", vc);
            for (int v = 0; v < vc; v++) {
                const mtts_voice_desc *vd = mtts_voice_at(d->key, v);
                printf("    %2d  %-58s %-28s %s\n", v, vd->key, vd->name, vd->identifier);
            }
        }
        return 0;
    }

    const char *key = argv[1];
    const char *voice = argc > 2 ? argv[2] : "";
    const char *text = argc > 3 ? argv[3] : "语音测试，一二三。";
    /* 纯数字的发音人参数 → 按序号取 key */
    char vbuf[256] = "";
    if (voice[0] >= '0' && voice[0] <= '9') {
        const mtts_voice_desc *vd = mtts_voice_at(key, atoi(voice));
        if (!vd) { printf("序号越界\n"); return 2; }
        snprintf(vbuf, sizeof(vbuf), "%s", vd->key);
        voice = vbuf;
    }
    if (!mtts_engine_loaded(key)) { printf("引擎 %s 未加载\n", key); return 2; }

    double t0 = now_ms();
    int32_t sr = 0;
    int rate = argc > 4 ? atoi(argv[4]) : 100;
    int pitch = argc > 5 ? atoi(argv[5]) : 50;
    mtts_stream *s = mtts_stream_start(key, voice, text, rate, pitch, &sr);
    printf("stream_start(%s, voice=%s, rate=%d, pitch=%d) → %p 采样率=%d  启动耗时 %.1fms\n",
           key, voice[0] ? voice : "(默认)", rate, pitch, s, sr, now_ms() - t0);
    if (!s) return 1;

    /* 拉流 (模拟渲染: 每 512 样本一拉) */
    int cap = 24000 * 60;
    int16_t *pcm = malloc(cap * 2);
    int32_t total = 0, first_at = -1;
    double t_start = now_ms();
    while (total < cap) {
        int done = 0;
        int32_t got = mtts_stream_read(s, pcm + total, 512, &done);
        if (got > 0) {
            if (first_at < 0) first_at = (int)(now_ms() - t_start);
            total += got;
        }
        if (done) break;
    }
    double dur = now_ms() - t_start;
    printf("首块 %dms, 总 %d 样本 (%.2fs 音频 @%dHz), 全程 %.0fms%s\n",
           first_at, total, total / (double)sr, sr, dur, total >= cap ? " (截断60s)" : "");
    mtts_stream_free(s);

    if (total > 0) {
        char out[300];
        snprintf(out, sizeof(out), "/tmp/mtts_%s%s%s_r%d_p%d.wav",
                 key, voice[0] ? "_" : "", voice, rate, pitch);
        write_wav(out, pcm, total, sr);
        int peak = 0;
        for (int i = 0; i < total; i++) { int a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > peak) peak = a; }
        printf("峰值=%d (%.3f)  已写出 %s\n", peak, peak / 32768.0, out);
    }
    free(pcm);
    return 0;
}
