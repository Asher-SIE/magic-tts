#include "engine_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 简易 WAV 写出 */
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
    const char *root = getenv("MT_ENGINES");
    const char *key = argc > 1 ? argv[1] : "naturalvoice";
    const char *text = argc > 2 ? argv[2] : "语音测试，真实人声。";
    int loaded = mtts_global_init(root);
    printf("已加载真实引擎数: %d\n", loaded);
    if (!mtts_engine_loaded(key)) { printf("引擎 %s 未加载真实 dylib\n", key); return 2; }
    int16_t *pcm = NULL; int32_t n = 0, sr = 0;
    int rc = mtts_synthesize(key, text, 100, 50, &pcm, &n, &sr);
    printf("synthesize rc=%d 采样=%d 采样率=%d 时长=%.2fs\n", rc, n, sr, n / (double)sr);
    if (rc == MTTS_OK && n > 0) {
        char out[256]; snprintf(out, sizeof(out), "/tmp/ms_%s.wav", key);
        write_wav(out, pcm, n, sr);
        int peak = 0;
        for (int i = 0; i < n; i++) { int a = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (a > peak) peak = a; }
        printf("峰值=%d (%.3f)  已写出 %s\n", peak, peak / 32768.0, out);
        mtts_free(pcm);
    }
    return 0;
}
