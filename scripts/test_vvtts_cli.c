/* viaemu 冒烟测试: 加载 ViaVoice DLL → ECI 合成一句 → 写 WAV
 * 用法: test_vvtts <引擎目录(含 ibmeci.dll 等6文件)> [文本] [输出wav]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "viaemu.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <dir> [text] [out.wav]\n", argv[0]);
        return 1;
    }
    const char *dir = argv[1];
    const char *text = argc > 2 ? argv[2] : "你好，这是语音合成测试，一二三四五。";
    const char *out = argc > 3 ? argv[3] : "/tmp/vvtts_out.wav";

    int rc = viaemu_load(dir);
    if (rc) { fprintf(stderr, "加载失败 rc=%d\n", rc); return 2; }

    static int16_t pcm[11025 * 60];
    int sr = 11025;
    int n = viaemu_eci_synth(text, 0, 100, 50, pcm, 11025 * 60, &sr);
    if (n <= 0) { fprintf(stderr, "合成失败 n=%d\n", n); return 3; }

    /* 能量统计 */
    double sum = 0; int peak = 0;
    for (int i = 0; i < n; i++) {
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        sum += (double)a * a;
        if (a > peak) peak = a;
    }
    fprintf(stderr, "样本 %d (%.2fs @%dHz) RMS=%.1f 峰值=%d\n",
            n, n / (double)sr, sr, sqrt(sum / n), peak);

    FILE *f = fopen(out, "wb");
    if (!f) return 4;
    int32_t dataLen = n * 2;
    fwrite("RIFF", 1, 4, f); fwrite(&(int32_t){36 + dataLen}, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&(int32_t){16}, 4, 1, f);
    fwrite(&(int16_t){1}, 2, 1, f); fwrite(&(int16_t){1}, 2, 1, f);
    fwrite(&(int32_t){sr}, 4, 1, f); fwrite(&(int32_t){sr * 2}, 4, 1, f);
    fwrite(&(int16_t){2}, 2, 1, f); fwrite(&(int16_t){16}, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataLen, 4, 1, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
    fprintf(stderr, "已写 %s\n", out);
    return 0;
}
