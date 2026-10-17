/* 实验: 空 AudioConfig + Synthesizing 回调能否流式取到音频.
 * 结论决定 MS 适配器重构方向 (延迟治理).
 *
 * 编译:
 *   clang -O2 -o /tmp/ms_stream_probe ms_stream_probe.c \
 *     -L"$HOME/Library/Group Containers/com.magictts.shared/Engines/naturalvoice" \
 *     -lMicrosoft.CognitiveServices.Speech.core -Wl,-rpath,\
 *     "$HOME/Library/Group Containers/com.magictts.shared/Engines/naturalvoice"
 * 用法: /tmp/ms_stream_probe <引擎key> [文本]
 */
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <mach/mach_time.h>

typedef void *h;

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static char g_root[1024];

/* 拉包信息: model/ 下第一个 SynthModel.license 包 → 完整语音名 + license */
static char pkg_dir[512], voice_full[256];
static char *lic = NULL;

static char *read_all(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); b[n] = 0;
    while (n > 0 && (b[n-1] == '\n' || b[n-1] == '\r')) b[--n] = 0;
    return b;
}

static void pick_pkg(const char *key) {
    char modeldir[1024];
    snprintf(modeldir, sizeof(modeldir), "%s/%s/model", g_root, key);
    DIR *d = opendir(modeldir);
    if (!d) { fprintf(stderr, "no model dir\n"); exit(1); }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1280];
        snprintf(p, sizeof(p), "%s/%s/SynthModel.license", modeldir, e->d_name);
        if (access(p, R_OK) == 0) {
            snprintf(pkg_dir, sizeof(pkg_dir), "%s", e->d_name);
            lic = read_all(p);
            break;
        }
    }
    closedir(d);
    if (!pkg_dir[0]) { fprintf(stderr, "no license pkg\n"); exit(1); }
    /* Tokens.xml 完整名 */
    char dd[1280];
    snprintf(dd, sizeof(dd), "%s/%s/model/%s/data", g_root, key, pkg_dir);
    DIR *d2 = opendir(dd);
    char tokens[1536] = {0};
    while (d2 && (e = readdir(d2))) {
        if (e->d_name[0] == '.') continue;
        snprintf(tokens, sizeof(tokens), "%s/%s/Tokens.xml", dd, e->d_name);
        if (access(tokens, R_OK) == 0) break;
        tokens[0] = 0;
    }
    if (d2) closedir(d2);
    char *xml = tokens[0] ? read_all(tokens) : NULL;
    if (xml) {
        char *v = strstr(xml, "<String name=\"\" value=\"");
        if (v) {
            v += strlen("<String name=\"\" value=\"");
            char *end = strchr(v, '"');
            size_t n = end ? (size_t)(end - v) : 0;
            if (n < sizeof(voice_full)) { memcpy(voice_full, v, n); voice_full[n] = 0; }
        }
        free(xml);
    }
}

/* —— dylib 符号 —— */
static int (*cfg_create)(h *);
static int (*cfg_add_path)(h, const char *);
static int (*cfg_set_voice)(h, const char *, const char *);
static int (*cfg_propbag)(h, h *);
static int (*bag_set_string)(h, int, const char *, const char *);
static int (*acfg_wavfile)(h *, const char *);
static int (*synth_create)(h *, h, h);
static int (*speak_ssml)(h, const char *, unsigned, h *);
static int (*start_speaking_ssml)(h, const char *, unsigned, h *);
static int (*stop_speaking)(h);
static void (*synthesizing_set_cb)(h, void (*)(h, void *), void *);
static void (*completed_set_cb)(h, void (*)(h, void *), void *);
static void (*canceled_set_cb)(h, void (*)(h, void *), void *);
static int (*event_get_result)(h, h *);
static int (*result_get_audio_data)(h, uint8_t *, uint32_t, uint32_t *);
static int (*result_get_len)(h, unsigned *, unsigned *);
static int (*result_get_reason)(h, int *);
static int (*release_result)(h);
static int (*release_synth)(h);

/* —— 回调侧生产 —— */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static uint8_t *buf = NULL;
static size_t buflen = 0, bufcap = 0;
static int done = 0, canceled = 0, chunks = 0;
static double t_start, t_first = -1;

static void append(const uint8_t *d, size_t n) {
    pthread_mutex_lock(&mu);
    if (buflen + n > bufcap) {
        bufcap = (buflen + n) * 2 + (1 << 20);
        buf = realloc(buf, bufcap);
    }
    memcpy(buf + buflen, d, n);
    buflen += n;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mu);
}

static void on_event(h synth_or_a, h event, int kind) {
    static int n = 0;
    if (n++ < 4) fprintf(stderr, "[cb%d] arg1=%p arg2=%p\n", kind, synth_or_a, event);
    h result = 0;
    /* ★ 实测回调 ABI: arg1=恒定(synth 侧句柄), arg2=事件句柄 */
    int rc = event_get_result(event, &result);
    if (rc != 0 || !result) {
        fprintf(stderr, "[cb%d] event_get_result rc=0x%x (t=%.0fms)\n", kind, rc, now_ms() - t_start);
        goto fin;
    }
    unsigned bytes = 0, dur = 0;
    result_get_len(result, &bytes, &dur);
    if (bytes > 0) {
        uint8_t *tmp = malloc(bytes);
        uint32_t got = 0;
        /* ★ ABI: (result, buf非空, cap入, 实拷出) */
        if (result_get_audio_data(result, tmp, bytes, &got) == 0 && got > 0) {
            if (t_first < 0) t_first = now_ms() - t_start;
            chunks++;
            if (chunks <= 3 || chunks % 20 == 0)
                fprintf(stderr, "[cb%d] chunk#%d %u字节 (t=%.0fms)\n", kind, chunks, got, now_ms() - t_start);
            append(tmp, got);
        } else {
            fprintf(stderr, "[cb%d] get_audio_data 失败 bytes=%u got=%u (t=%.0fms)\n", kind, bytes, got, now_ms() - t_start);
        }
        free(tmp);
    }
    if (kind == 2) { /* completed */
        int reason = 0;
        result_get_reason(result, &reason);
        fprintf(stderr, "[completed] reason=%d bytes=%u (t=%.0fms)\n", reason, bytes, now_ms() - t_start);
    }
fin:
    if (result) release_result(result);
}

static void cb_synthesizing(h a, h b) { on_event(a, b, 1); }
static void cb_completed(h a, h b) {
    on_event(a, b, 2);
    pthread_mutex_lock(&mu); done = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&mu);
}
static void cb_canceled(h a, h b) {
    fprintf(stderr, "[canceled] (t=%.0fms)\n", now_ms() - t_start);
    pthread_mutex_lock(&mu); done = 1; canceled = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&mu);
}

static void *sym(void *dl, const char *n) {
    void *p = dlsym(dl, n);
    if (!p) fprintf(stderr, "缺符号 %s\n", n);
    return p;
}

int main(int argc, char **argv) {
    const char *key = argc > 1 ? argv[1] : "naturalvoice";
    const char *text = argc > 2 ? argv[2] : "这是流式合成实验，第一句测试延迟。第二句稍微长一点，用来观察分块到达的时间。";
    int mode = argc > 3 ? atoi(argv[3]) : 1;   /* 1=空acfg+start 2=wav+start+文件跟随 3=空acfg+speak同步+事件 */
    const char *root = getenv("MTTS_ROOT");
    if (root)
        snprintf(g_root, sizeof(g_root), "%s", root);
    else
        snprintf(g_root, sizeof(g_root), "%s/Library/Group Containers/com.magictts.shared/Engines", getenv("HOME"));

    pick_pkg(key);
    printf("包: %s\n语音: %s\nlicense: %zu字节\n", pkg_dir, voice_full, strlen(lic));

    char dy[1024];
    snprintf(dy, sizeof(dy), "%s/%s/libMicrosoft.CognitiveServices.Speech.core.dylib", g_root, key);
    void *dl = dlopen(dy, RTLD_NOW | RTLD_LOCAL);
    if (!dl) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int ok = (cfg_create = sym(dl, "embedded_speech_config_create")) &&
             (cfg_add_path = sym(dl, "embedded_speech_config_add_path")) &&
             (cfg_set_voice = sym(dl, "embedded_speech_config_set_speech_synthesis_voice")) &&
             (cfg_propbag = sym(dl, "speech_config_get_property_bag")) &&
             (bag_set_string = sym(dl, "property_bag_set_string")) &&
             (acfg_wavfile = sym(dl, "audio_config_create_audio_output_from_wav_file_name")) &&
             (synth_create = sym(dl, "synthesizer_create_speech_synthesizer_from_config")) &&
             (speak_ssml = sym(dl, "synthesizer_speak_ssml")) &&
             (start_speaking_ssml = sym(dl, "synthesizer_start_speaking_ssml")) &&
             (stop_speaking = sym(dl, "synthesizer_stop_speaking")) &&
             (synthesizing_set_cb = sym(dl, "synthesizer_synthesizing_set_callback")) &&
             (completed_set_cb = sym(dl, "synthesizer_completed_set_callback")) &&
             (canceled_set_cb = sym(dl, "synthesizer_canceled_set_callback")) &&
             (event_get_result = sym(dl, "synthesizer_synthesis_event_get_result")) &&
             (result_get_audio_data = sym(dl, "synth_result_get_audio_data")) &&
             (result_get_len = sym(dl, "synth_result_get_audio_length_duration")) &&
             (result_get_reason = sym(dl, "synth_result_get_reason")) &&
             (release_result = sym(dl, "synthesizer_result_handle_release")) &&
             (release_synth = sym(dl, "synthesizer_handle_release"));
    if (!ok) return 2;

    double t0 = now_ms();
    h cfg = 0;
    char modeldir[1024];
    snprintf(modeldir, sizeof(modeldir), "%s/%s/model", g_root, key);
    if (cfg_create(&cfg) || cfg_add_path(cfg, modeldir)) { fprintf(stderr, "fromPath失败\n"); return 3; }
    int rc = cfg_set_voice(cfg, voice_full, lic);
    printf("set_voice rc=0x%x (init %.0fms)\n", rc, now_ms() - t0);
    h bag = 0;
    if (cfg_propbag(cfg, &bag) == 0 && bag)
        bag_set_string(bag, 3102, NULL, "raw-24khz-16bit-mono-pcm");

    h synth = 0;
    /* mode 2: wav 文件目的地; 其余: 空 AudioConfig (音频只走事件回调) */
    h acfg = 0;
    char wavpath[512] = "";
    if (mode == 2 || mode == 4 || mode == 5) {
        snprintf(wavpath, sizeof(wavpath), "/tmp/ms_probe_%s.wav", key);
        unlink(wavpath);
        acfg_wavfile(&acfg, wavpath);
    }
    int rc2 = synth_create(&synth, cfg, acfg);
    printf("synth_create(%s acfg) rc=0x%x (累计 %.0fms)\n", mode == 2 ? "wav" : "NULL", rc2, now_ms() - t0);
    if (rc2 != 0 || !synth) return 4;

    synthesizing_set_cb(synth, cb_synthesizing, (void *)0xDEADBEEFULL);
    completed_set_cb(synth, cb_completed, NULL);
    canceled_set_cb(synth, cb_canceled, NULL);

    char ssml[8192];
    snprintf(ssml, sizeof(ssml),
             "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
             "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
             "<voice name=\"%s\"><prosody rate=\"100%%\" pitch=\"+0%%\">%s</prosody></voice></speak>",
             voice_full, text);

    if (mode == 4 || mode == 5) {
        /* 序列实验: 句A → 文件稳定 → [stop] → 句B → 文件稳定 */
        snprintf(wavpath, sizeof(wavpath), "/tmp/ms_probe_%s.wav", key);   /* ★ wavpath 原本只在 mode 2 赋值 */
        FILE *cw = fopen(wavpath, "wb"); if (cw) fclose(cw);   /* 清场 */
        for (int round = 0; round < 2; round++) {
            char s2[8192];
            snprintf(s2, sizeof(s2),
                     "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
                     "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
                     "<voice name=\"%s\"><prosody rate=\"100%%\" pitch=\"+0%%\">这是第%d句序列实验，观察连续合成的行为。</prosody></voice></speak>",
                     voice_full, round + 1);
            t_start = now_ms();
            h r2 = 0;
            int rc2 = start_speaking_ssml(synth, s2, (unsigned)strlen(s2), &r2);
            if (r2) release_result(r2);
            printf("第%d句 start rc=0x%x\n", round + 1, rc2);
            if (rc2 != 0) return 6;
            long last = -1;
            /* 等 completed 事件 (cb_completed 置 done), 再 3 轮确认尺寸收尾 */
            pthread_mutex_lock(&mu); done = 0; pthread_mutex_unlock(&mu);
            for (int i = 0; i < 6000; i++) {
                pthread_mutex_lock(&mu); int fin = done; pthread_mutex_unlock(&mu);
                if (fin) break;
                usleep(10000);
            }
            for (int i = 0; i < 300; i++) {
                FILE *f = fopen(wavpath, "rb");
                long sz = 0;
                if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
                if (sz > 0 && sz == last) break;
                last = sz;
                usleep(10000);
            }
            printf("第%d句 文件=%ld字节 (%.2fs音频) 完成于 %.0fms\n",
                   round + 1, last, last / 48000.0, now_ms() - t_start);
            FILE *c2 = fopen(wavpath, "wb"); if (c2) fclose(c2);
            if (mode == 5) { stop_speaking(synth); usleep(150000); }
        }
        return 0;
    }

    t_start = now_ms();
    fprintf(stderr, "synth handle = %p\n", synth);
    h result = 0;
    int rc3;
    if (mode == 3) {
        rc3 = speak_ssml(synth, ssml, (unsigned)strlen(ssml), &result);
        printf("speak_ssml(同步) rc=0x%x (耗时 %.0fms)\n", rc3, now_ms() - t_start);
        if (result) { int reason = 0; result_get_reason(result, &reason); printf("reason=%d\n", reason); release_result(result); result = 0; }
    } else {
        rc3 = start_speaking_ssml(synth, ssml, (unsigned)strlen(ssml), &result);
        printf("start_speaking_ssml rc=0x%x (启动耗时 %.0fms)\n", rc3, now_ms() - t_start);
        if (result) release_result(result);
    }
    if (rc3 != 0) return 5;

    /* 观察增量到达 (模拟渲染线程拉取): 事件缓冲 或 wav 文件增长 */
    size_t seen = 0;
    for (int i = 0; i < 3000; i++) {     /* 最多 60s */
        size_t cur; int fin;
        if (mode == 2) {
            FILE *f2 = fopen(wavpath, "rb");
            cur = 0;
            if (f2) { fseek(f2, 0, SEEK_END); cur = ftell(f2); fclose(f2); }
            pthread_mutex_lock(&mu); fin = done; pthread_mutex_unlock(&mu);
            usleep(10000);
        } else {
            pthread_mutex_lock(&mu);
            while (buflen == seen && !done) pthread_cond_wait(&cond, &mu);
            cur = buflen; fin = done;
            pthread_mutex_unlock(&mu);
        }
        if (cur != seen) {
            printf("t=%6.0fms  累计 %7zu 字节 (%.2fs 音频)  +%zu\n",
                   now_ms() - t_start, cur, cur / 48000.0, cur - seen);
            seen = cur;
        }
        if (fin && (mode != 2 || seen > 0)) {
            if (mode != 2) break;
            /* wav: done 后再等文件稳定一轮 */
            FILE *f3 = fopen(wavpath, "rb");
            size_t fin_sz = 0;
            if (f3) { fseek(f3, 0, SEEK_END); fin_sz = ftell(f3); fclose(f3); }
            if (fin_sz >= seen) { seen = fin_sz; break; }
        }
    }
    printf("首块 %.0fms, 总 %zu 字节 (%.2fs), %d 块, done=%d canceled=%d\n",
           t_first, seen, seen / 48000.0, chunks, done, canceled);

    if (mode == 2) {
        /* 从文件读回写标准 wav */
        char *raw = read_all(wavpath);
        if (raw) {
            long n = (long)strlen(raw);   /* read_all 截断了... 重新按二进制读 */
            (void)n;
            free(raw);
        }
        FILE *src = fopen(wavpath, "rb");
        if (src) {
            fseek(src, 0, SEEK_END); long sz = ftell(src); fseek(src, 0, SEEK_SET);
            char *raw2 = malloc(sz);
            if (fread(raw2, 1, sz, src) == (size_t)sz) {
                buf = (uint8_t *)raw2; buflen = sz;
                printf("文件实长 %zu 字节\n", buflen);
            } else { free(raw2); }
            fclose(src);
        }
    }

    /* 写 wav 供听感比对 */
    FILE *f = fopen("/tmp/ms_stream_probe.wav", "wb");
    if (f && buflen) {
        int32_t d32 = (int32_t)buflen; int16_t d16; int32_t sr = 24000;
        fwrite("RIFF", 1, 4, f); int32_t sz = 36 + d32; fwrite(&sz, 4, 1, f); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); int32_t f16 = 16; fwrite(&f16, 4, 1, f);
        d16 = 1; fwrite(&d16, 2, 1, f); d16 = 1; fwrite(&d16, 2, 1, f);
        fwrite(&sr, 4, 1, f); int32_t br = sr * 2; fwrite(&br, 4, 1, f);
        d16 = 2; fwrite(&d16, 2, 1, f); d16 = 16; fwrite(&d16, 2, 1, f);
        fwrite("data", 1, 4, f); fwrite(&d32, 4, 1, f);
        fwrite(buf, 1, buflen, f);
        printf("已写 /tmp/ms_stream_probe.wav\n");
    }
    if (f) fclose(f);
    release_synth(synth);
    return 0;
}
