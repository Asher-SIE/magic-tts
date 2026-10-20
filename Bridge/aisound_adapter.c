/* AiSound 引擎适配器 — 消费科大讯飞风格 Resource.irf 语音数据的 Rust 复刻引擎
 * (PSOLA 拼接合成, 16kHz/16bit/mono).
 *
 * 来源: AiSoundTts 0.7 Android 包 (com.xztom.aisound) 逆向重建源码包, 全部
 * 调用语义见 docs/NOTES-aisound.md. 原版形态:
 *   - libaisound_jni.so (8.6M, Android arm64 ELF, 仅导出 JNI_OnLoad,
 *     运行时 RegisterNatives) — Rust aisound-core + aisound-zd, 无 Darwin 版;
 *   - assets/Resource.irf (17.9M, AIRM v4 容器 + LZMA 压缩块) — 8 个基础音色.
 *
 * 本适配器按原版 JNI API 约定一份 C ABI (见下方 as_api), 引擎库就位后即可
 * 直驱; 引擎库缺失时 create 返回 NULL, 引擎显示"物料不全" (与 MS 缺 dylib
 * 同表现). 布局:
 *   Engines/aisound/
 *     Resource.irf        语音数据 (AIRM 魔数校验)
 *     libaisound.dylib    引擎库 (待 Rust 引擎出 Darwin 版; ABI 见下)
 *
 * 调用序列与原版 AiSoundService 一一对应:
 *   open(irf字节, 模式号) → setStyle(1) → speakStream(text, 语速, 音高, 49,
 *   hasFx, fx0..2, 分块回调); 失败时 close 后重开重试一次 (panic 自愈).
 * 语速/音高刻度换算 (AiSoundService.smali f() 实测提取):
 *   桥刻度 rate 20..500 (100=常速): ≥100 → 引擎恒 100 + Sonic ×(rate/100);
 *     <100 → 引擎 rate/2 (引擎原生慢速域), 不变速后处理.
 *   桥刻度 pitch 0..100 (50=常音高) → 引擎 pitch*49/50 (Android 100→49 同构),
 *     clamp [0,100]. */
#include "adapters.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sonic.h"

/* ---------- 引擎 C ABI 约定 (libaisound.dylib 需实现; JNI 层的 C 化镜像) ---------- */

/* 分块回调: 返回 0 请求中止合成 (对应 ChunkListener.onChunk → false). */
typedef int (*as_chunk_fn)(const int16_t *pcm, int64_t n, void *user);

typedef struct {
    long       (*open)(const void *irf_bytes, int64_t irf_len, int voice_mode);
    void       (*close)(long handle);
    int        (*set_style)(long handle, int style);      /* 1=连读(默认) 0=逐字 */
    const char *(*last_panic)(void);
    int        (*speak_stream)(long handle, const char *utf8_text,
                               int rate, int pitch, int flags,   /* flags 恒 49 */
                               int has_fx, double fx0, double fx1, double fx2,
                               as_chunk_fn on_chunk, void *user);
} as_api;

/* ---------- 发音人表 (MainActivity 选择器 + Catherine, 官方显示名) ----------
 * mode = 引擎 open() 的模式号 (Resource.irf 内基础音色);
 * fx 三元组 = speakStream 变声参数 (推测 序: 变速/音高/音量, 见 NOTES §4). */
typedef struct {
    const char *key, *name, *locale;
    int mode, has_fx;
    double fx0, fx1, fx2;
} as_voice;

static const as_voice kVoices[] = {
    {"xiaoyan",       "晓燕",              "zh-CN", 1, 0, 0,     0,       0    },
    {"xiaomei",       "晓美（粤语）",      "zh-HK", 2, 0, 0,     0,       0    },
    {"catherine",     "Catherine（英语）", "en-US", 3, 0, 0,     0,       0    },
    {"jiuxu",         "许久",              "zh-CN", 1, 1, 0.587, 0.790123, 1.025},
    {"duoxu",         "许多",              "zh-CN", 1, 1, 0.516, 0.739884, 1.03 },
    {"xiaoping",      "小平",              "zh-CN", 1, 1, 0.907, 0.895105, 1.012},
    {"donaldduck",    "唐老鸭",            "zh-CN", 1, 1, 0.738, 1.254902, 0.965},
    {"babyxu",        "许宝宝",            "zh-CN", 1, 1, 1.244, 1.254902, 0.963},
    {"xiaofeng",      "小风",              "zh-CN", 4, 0, 0,     0,       0    },
    {"dalong",        "大龙",              "zh-CN", 5, 0, 0,     0,       0    },
    {"yd-yanping",    "永德燕萍",          "zh-CN", 6, 0, 0,     0,       0    },
    {"yd-yufeng",     "永德雨峰",          "zh-CN", 7, 0, 0,     0,       0    },
    {"yd-xiaomei",    "永德小美（粤语）",  "zh-HK", 8, 0, 0,     0,       0    },
    {"yd-jiuxu",      "永德许久",          "zh-CN", 6, 1, 0.587, 0.790123, 1.025},
    {"yd-duoxu",      "永德许多",          "zh-CN", 6, 1, 0.516, 0.739884, 1.03 },
    {"yd-xiaoping",   "永德小平",          "zh-CN", 6, 1, 0.907, 0.895105, 1.012},
    {"yd-donaldduck", "永德唐老鸭",        "zh-CN", 6, 1, 0.738, 1.254902, 0.965},
    {"yd-babyxu",     "永德许宝宝",        "zh-CN", 6, 1, 1.244, 1.254902, 0.963},
};
#define VOICE_N ((int)(sizeof(kVoices) / sizeof(kVoices[0])))

static const as_voice *voice_lookup(const char *key) {
    if (!key || !key[0]) return &kVoices[0];   /* 默认 = 晓燕 (原版 x.g) */
    for (int i = 0; i < VOICE_N; i++)
        if (strcmp(kVoices[i].key, key) == 0) return &kVoices[i];
    return NULL;
}

/* ---------- 会话 ---------- */

#define AS_MODE_MAX 9   /* 模式号 1..8 (下标即模式) */

typedef struct as_session {
    void *dl;                       /* 进程存续 (与 ms 适配器同策略) */
    as_api api;
    unsigned char *irf;             /* Resource.irf 全量 (open 每次取字节) */
    int64_t irf_len;
    long handles[AS_MODE_MAX];      /* 0=未开; 原版池上限 4, 第 5 个触发全清 */
    int handle_count;
    pthread_mutex_t lock;           /* 串行化引擎调用 (会话串行前提) */
} as_session;

/* 取/建某模式号的引擎句柄 (原版 AiSoundService.b(): 池满全清再开). */
static long session_open_handle(as_session *s, int mode) {
    if (s->handles[mode]) return s->handles[mode];
    if (s->handle_count >= 4) {
        for (int m = 1; m < AS_MODE_MAX; m++)
            if (s->handles[m]) { s->api.close(s->handles[m]); s->handles[m] = 0; }
        s->handle_count = 0;
    }
    long h = s->api.open(s->irf, s->irf_len, mode);
    if (!h) {
        fprintf(stderr, "[MagicTTS][aisound] open(mode %d) 返回 0; panic=%s\n",
                mode, s->api.last_panic ? s->api.last_panic() : "(无)");
        return 0;
    }
    s->handles[mode] = h;
    s->handle_count++;
    return h;
}

static void session_drop_handle(as_session *s, long h) {
    for (int m = 1; m < AS_MODE_MAX; m++)
        if (s->handles[m] == h) {
            s->api.close(h);
            s->handles[m] = 0;
            s->handle_count--;
            return;
        }
}

/* ---------- 流 (worker 线程 + 队列缓冲) ---------- */

#define AS_BUF_INIT 32768   /* 样本 (~2s @16k) */

typedef struct as_stream {
    as_session *s;
    const as_voice *v;
    char *text;
    int engine_rate, engine_pitch;
    double speed;                  /* >1 时 Sonic 变速后处理 */
    sonicStream so;

    pthread_t thread;
    int started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int16_t *buf;
    size_t cap, len, pos;          /* 待读区间 [pos,len) */
    int done, failed, cancelled;
} as_stream;

/* 追加样本 (满则压缩队列/扩容/等消费者; 取消/结束 → 放弃并返回 0 中止引擎). */
static int stream_append(as_stream *st, const int16_t *pcm, size_t n) {
    pthread_mutex_lock(&st->lock);
    while (!st->cancelled) {
        if (st->len + n <= st->cap) {
            memcpy(st->buf + st->len, pcm, n * sizeof(int16_t));
            st->len += n;
            pthread_cond_broadcast(&st->cond);
            pthread_mutex_unlock(&st->lock);
            return 1;
        }
        if (st->pos > 0) {   /* 压缩: 已读区挪回队首 (消费端分次读时 pos 不归零) */
            memmove(st->buf, st->buf + st->pos,
                    (st->len - st->pos) * sizeof(int16_t));
            st->len -= st->pos;
            st->pos = 0;
            continue;
        }
        /* 真·满: 扩容有上限 (≈10s), 到顶后等消费者腾位 */
        if (st->cap < 16000 * 10) {
            size_t nc = st->cap * 2;
            while (nc < st->len + n) nc *= 2;
            int16_t *nb = realloc(st->buf, nc * sizeof(int16_t));
            if (nb) { st->buf = nb; st->cap = nc; continue; }
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&st->cond, &st->lock, &ts);
    }
    pthread_mutex_unlock(&st->lock);
    return 0;
}

/* 引擎分块回调 (speak_stream 同步调用; 返回 0 = 中止). */
static int stream_on_chunk(const int16_t *pcm, int64_t n, void *user) {
    as_stream *st = user;
    if (st->so) {
        sonicWriteShortToStream(st->so, pcm, (int)n);
        int16_t tmp[4096];
        int got;
        while ((got = sonicReadShortFromStream(st->so, tmp, 4096)) > 0)
            if (!stream_append(st, tmp, (size_t)got)) return 0;
        return 1;
    }
    return stream_append(st, pcm, (size_t)n);
}

/* 单次 speak (失败返回 0). 原版 g(): setStyle(1) + speakStream, 失败丢句柄. */
static int stream_speak_once(as_stream *st) {
    as_session *s = st->s;
    long h = session_open_handle(s, st->v->mode);
    if (!h) return 0;
    s->api.set_style(h, 1);
    if (s->api.speak_stream(h, st->text, st->engine_rate, st->engine_pitch, 49,
                            st->v->has_fx, st->v->fx0, st->v->fx1, st->v->fx2,
                            stream_on_chunk, st) != 0)
        return 1;
    fprintf(stderr, "[MagicTTS][aisound] speak_stream 失败 (mode %d); panic=%s\n",
            st->v->mode, s->api.last_panic ? s->api.last_panic() : "(无)");
    session_drop_handle(s, h);
    return 0;
}

static void *stream_worker(void *arg) {
    as_stream *st = arg;
    pthread_mutex_lock(&st->s->lock);
    int ok = stream_speak_once(st);
    if (!ok && !st->cancelled) ok = stream_speak_once(st);   /* panic 自愈重试一次 */
    pthread_mutex_unlock(&st->s->lock);
    if (st->so) {   /* 收尾: 排空变速器残余 */
        sonicFlushStream(st->so);
        int16_t tmp[4096];
        int got;
        while ((got = sonicReadShortFromStream(st->so, tmp, 4096)) > 0)
            if (!stream_append(st, tmp, (size_t)got)) break;
    }
    pthread_mutex_lock(&st->lock);
    st->done = 1;
    if (!ok && !st->cancelled) st->failed = 1;
    pthread_cond_broadcast(&st->cond);
    pthread_mutex_unlock(&st->lock);
    return NULL;
}

/* ---------- 适配器接口 ---------- */

static int aisound_probe(const char *root, const mtts_engine_desc *desc) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/Resource.irf", root, desc->key);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    size_t got = fread(magic, 1, 4, f);
    fclose(f);
    return got == 4 && memcmp(magic, "AIRM", 4) == 0;
}

static int aisound_scan_voices(const char *root, const mtts_engine_desc *desc,
                               mtts_voice_scan *out, int max) {
    (void)root; (void)desc;
    int n = VOICE_N < max ? VOICE_N : max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].key,   sizeof(out[i].key),   "%s", kVoices[i].key);
        snprintf(out[i].name,  sizeof(out[i].name),  "%s", kVoices[i].name);
        snprintf(out[i].locale, sizeof(out[i].locale), "%s", kVoices[i].locale);
    }
    return n;
}

static void *aisound_create(const char *root, const mtts_engine_desc *desc) {
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s/%s", root, desc->key, desc->dylib_name);
    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        fprintf(stderr, "[MagicTTS][aisound] 缺 macOS 版引擎库 %s (原包为 "
                "Android arm64 .so, 无法直接使用; ABI 见 docs/NOTES-aisound.md)"
                " (%s)\n", desc->dylib_name, dlerror());
        return NULL;
    }

    as_session *s = calloc(1, sizeof(*s));
    if (!s) { dlclose(dl); return NULL; }
    s->dl = dl;
    s->api.open = (long (*)(const void *, int64_t, int))dlsym(dl, "as_open");
    s->api.close = (void (*)(long))dlsym(dl, "as_close");
    s->api.set_style = (int (*)(long, int))dlsym(dl, "as_set_style");
    s->api.last_panic = (const char *(*)(void))dlsym(dl, "as_last_panic");
    s->api.speak_stream = (int (*)(long, const char *, int, int, int, int,
                                   double, double, double, as_chunk_fn, void *))
                          dlsym(dl, "as_speak_stream");
    if (!s->api.open || !s->api.close || !s->api.set_style ||
        !s->api.last_panic || !s->api.speak_stream) {
        fprintf(stderr, "[MagicTTS][aisound] 引擎库符号不全 (需 as_open/as_close/"
                "as_set_style/as_last_panic/as_speak_stream)\n");
        free(s);
        dlclose(dl);
        return NULL;
    }

    /* Resource.irf 全量载入 (原版缓存 f88b, 每次 open 传同一份字节) */
    snprintf(path, sizeof(path), "%s/%s/Resource.irf", root, desc->key);
    FILE *f = fopen(path, "rb");
    if (!f) { free(s); dlclose(dl); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->irf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!s->irf || (sz > 0 && fread(s->irf, 1, (size_t)sz, f) != (size_t)sz)) {
        fprintf(stderr, "[MagicTTS][aisound] Resource.irf 读取失败: %s\n", path);
        free(s->irf); free(s); fclose(f); dlclose(dl);
        return NULL;
    }
    fclose(f);
    s->irf_len = sz;

    pthread_mutex_init(&s->lock, NULL);
    return s;
}

static void *aisound_stream_start(void *session, const char *voice_key,
                                  const char *text, int rate, int pitch,
                                  int32_t *out_sample_rate) {
    as_session *s = session;
    const as_voice *v = voice_lookup(voice_key);
    if (!v) return NULL;

    as_stream *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->s = s;
    st->v = v;
    st->text = strdup(text);
    st->buf = malloc(AS_BUF_INIT * sizeof(int16_t));
    if (!st->text || !st->buf) {
        free(st->text); free(st->buf); free(st);
        return NULL;
    }
    st->cap = AS_BUF_INIT;

    /* 刻度换算 (AiSoundService.smali f(), 见文件头注释) */
    if (rate >= 100) {
        st->engine_rate = 100;
        st->speed = rate / 100.0;
        if (st->speed > 1.001) st->so = sonicCreateStream(16000, 1);
        if (st->so) sonicSetSpeed(st->so, (float)st->speed);
    } else {
        int er = rate / 2;                  /* 引擎原生慢速域 */
        st->engine_rate = er < 5 ? 5 : er;
        st->speed = 1.0;
    }
    int ep = (int)(pitch * 49.0 / 50.0 + 0.5);
    st->engine_pitch = ep < 0 ? 0 : (ep > 100 ? 100 : ep);

    pthread_mutex_init(&st->lock, NULL);
    pthread_cond_init(&st->cond, NULL);
    if (pthread_create(&st->thread, NULL, stream_worker, st) == 0) {
        st->started = 1;
    } else {
        st->done = 1; st->failed = 1;
    }
    *out_sample_rate = 16000;
    return st;
}

static int32_t aisound_stream_read(void *stream, int16_t *buf, int32_t want,
                                   int *done) {
    as_stream *st = stream;
    pthread_mutex_lock(&st->lock);
    while (st->len == st->pos && !st->done)
        pthread_cond_wait(&st->cond, &st->lock);
    size_t n = st->len - st->pos;
    if (n > (size_t)want) n = (size_t)want;
    if (n > 0) {
        memcpy(buf, st->buf + st->pos, n * sizeof(int16_t));
        st->pos += n;
        if (st->pos == st->len) { st->pos = st->len = 0; }   /* 队列复位 */
    }
    *done = (st->done && st->len == st->pos) ? 1 : 0;
    pthread_cond_broadcast(&st->cond);   /* 唤醒可能等空间的 worker */
    pthread_mutex_unlock(&st->lock);
    return (int32_t)n;
}

static void aisound_stream_cancel(void *stream) {
    as_stream *st = stream;
    pthread_mutex_lock(&st->lock);
    st->cancelled = 1;
    pthread_cond_broadcast(&st->cond);
    pthread_mutex_unlock(&st->lock);
}

static void aisound_stream_free(void *stream) {
    as_stream *st = stream;
    aisound_stream_cancel(st);
    if (st->started) pthread_join(st->thread, NULL);
    if (st->so) sonicDestroyStream(st->so);
    pthread_mutex_destroy(&st->lock);
    pthread_cond_destroy(&st->cond);
    free(st->text);
    free(st->buf);
    free(st);
}

static void aisound_destroy(void *session) {
    as_session *s = session;
    if (!s) return;
    for (int m = 1; m < AS_MODE_MAX; m++)
        if (s->handles[m]) s->api.close(s->handles[m]);
    pthread_mutex_destroy(&s->lock);
    free(s->irf);
    free(s);   /* dl 进程存续 (与 ms 适配器同策略) */
}

const mtts_adapter_if *mtts_aisound_adapter(void) {
    static const mtts_adapter_if ifc = {
        .probe         = aisound_probe,
        .scan_voices   = aisound_scan_voices,
        .create        = aisound_create,
        .voice_count   = NULL,   /* 无会话级枚举: scan 表即全量 */
        .voice_key     = NULL,
        .voice_name    = NULL,
        .stream_start  = aisound_stream_start,
        .stream_read   = aisound_stream_read,
        .stream_cancel = aisound_stream_cancel,
        .stream_free   = aisound_stream_free,
        .destroy       = aisound_destroy,
        .warm_voice    = NULL,
    };
    return &ifc;
}
