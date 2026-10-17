/* 引擎路由 — 按引擎 key 分发到对应 adapter; 发音人枚举 + 流式合成分发.
 * scan (轻量, 文件系统) 与 init (重量, dlopen) 分离:
 * 系统查询 speechVoices 时必须秒回, 引擎库后台预热. */
#include "engine_bridge.h"
#include "adapters.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MTTS_MAX_ENGINES 4
#define MTTS_MAX_VOICES  16

typedef struct {
    mtts_engine_desc desc;
    int  loaded;               /* scan: 物料就绪; init 后: 会话建立 */
    void *session;             /* adapter 私有会话 (init 后) */
    mtts_voice_desc voices[MTTS_MAX_VOICES];   /* 发音人缓存 (字符串 heap) */
    int  voice_count;
} mtts_engine_state;

/* 流对象: 包住 adapter 的实现指针, 供 read/cancel/free 分发 */
struct mtts_stream {
    const mtts_adapter_if *a;
    void *impl;
};

static mtts_engine_state g_engines[MTTS_MAX_ENGINES];
static int g_engine_count = 0;
static int g_inited = 0;           /* mtts_global_init 已跑 */
static char g_engines_root[1024] = {0};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const mtts_engine_desc kEngineTable[] = {
    /* key             voice_name    identifier                   locale  引擎库                                采样率 */
    { "yongde",        "永德",       "com.magictts.yongde",       "zh-CN", "libyongde_tts.dylib",                16000 },
    { "naturalvoice",  "自然语音",   "com.magictts.naturalvoice", "zh-CN", "libMicrosoft.CognitiveServices.Speech.core.dylib", 24000 },
    { "onecore",       "OneCore",    "com.magictts.onecore",      "zh-CN", "libMicrosoft.CognitiveServices.Speech.core.dylib", 24000 },
};
#define TABLE_LEN ((int)(sizeof(kEngineTable) / sizeof(kEngineTable[0])))

/* —— 适配器入口 (各 adapter 文件) —— */
const mtts_adapter_if *mtts_yongde_adapter(void);   /* yongde_adapter.c (复刻, 无引擎库) */
const mtts_adapter_if *mtts_ms_adapter(void);       /* ms_embedded_adapter.c (naturalvoice + onecore) */

const mtts_adapter_if *mtts_adapter_for(const char *key) {
    if (!key) return NULL;
    if (strcmp(key, "yongde") == 0) return mtts_yongde_adapter();
    if (strcmp(key, "naturalvoice") == 0 || strcmp(key, "onecore") == 0)
        return mtts_ms_adapter();
    return NULL;
}

/* 发音人 identifier: com.magictts.<engine>.<voicekey> */
static char *voice_identifier(const char *engine_id, const char *vkey) {
    size_t n = strlen(engine_id) + strlen(vkey) + 16;
    char *s = malloc(n);
    snprintf(s, n, "%s.%s", engine_id, vkey);
    return s;
}

static void engine_free_voices(mtts_engine_state *st) {
    for (int i = 0; i < st->voice_count; i++) {
        free((void *)st->voices[i].key);
        free((void *)st->voices[i].name);
        free((void *)st->voices[i].identifier);
        free((void *)st->voices[i].locale);
    }
    st->voice_count = 0;
}

/* 轻量发音人扫描结果 → 发音人缓存 (identifier 一并生成;
 * locale 适配器未填时回退引擎级) */
static void engine_scan_voices(mtts_engine_state *st, const mtts_adapter_if *a) {
    engine_free_voices(st);
    mtts_voice_scan buf[MTTS_MAX_VOICES];
    memset(buf, 0, sizeof(buf));
    int n = a->scan_voices ? a->scan_voices(g_engines_root, &st->desc, buf, MTTS_MAX_VOICES) : 0;
    if (n > MTTS_MAX_VOICES) n = MTTS_MAX_VOICES;
    for (int i = 0; i < n; i++) {
        if (!buf[i].key[0]) continue;
        mtts_voice_desc *v = &st->voices[st->voice_count++];
        v->key = strdup(buf[i].key);
        v->name = strdup(buf[i].name);
        v->identifier = voice_identifier(st->desc.identifier, buf[i].key);
        v->locale = strdup(buf[i].locale[0] ? buf[i].locale : st->desc.locale);
    }
}

int mtts_global_scan(const char *engines_root) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_engines_root, sizeof(g_engines_root), "%s", engines_root ? engines_root : "");
    g_engine_count = 0;
    g_inited = 0;
    for (int i = 0; i < TABLE_LEN && g_engine_count < MTTS_MAX_ENGINES; i++) {
        mtts_engine_state *st = &g_engines[g_engine_count];
        memset(st, 0, sizeof(*st));
        st->desc = kEngineTable[i];
        const mtts_adapter_if *a = mtts_adapter_for(st->desc.key);
        st->loaded = (a && a->probe(g_engines_root, &st->desc)) ? 1 : 0;
        if (st->loaded) engine_scan_voices(st, a);
        g_engine_count++;
        fprintf(stderr, "[MagicTTS] scan  %-13s %-16s %s (%d 发音人)\n",
                st->desc.key, st->desc.voice_name,
                st->loaded ? "物料就绪" : "缺物料", st->voice_count);
    }
    int loaded = 0;
    for (int i = 0; i < g_engine_count; i++) loaded += g_engines[i].loaded;
    pthread_mutex_unlock(&g_lock);
    return loaded;
}

int mtts_global_init(const char *engines_root) {
    pthread_mutex_lock(&g_lock);
    if (g_inited && engines_root == NULL) {   /* 已初始化且不换根 → 幂等 */
        pthread_mutex_unlock(&g_lock);
        int loaded = 0;
        for (int i = 0; i < g_engine_count; i++) loaded += g_engines[i].loaded;
        return loaded;
    }
    if (g_engine_count == 0 || (engines_root && strcmp(engines_root, g_engines_root) != 0)) {
        pthread_mutex_unlock(&g_lock);
        mtts_global_scan(engines_root);
        pthread_mutex_lock(&g_lock);
    }
    for (int i = 0; i < g_engine_count; i++) {
        mtts_engine_state *st = &g_engines[i];
        if (!st->loaded || st->session) continue;
        const mtts_adapter_if *a = mtts_adapter_for(st->desc.key);
        if (!a) continue;
        st->session = a->create(g_engines_root, &st->desc);
        if (!st->session) {
            st->loaded = 0;
            engine_free_voices(st);
        }
    }
    g_inited = 1;
    int loaded = 0;
    for (int i = 0; i < g_engine_count; i++) loaded += g_engines[i].loaded;
    pthread_mutex_unlock(&g_lock);
    return loaded;
}

int mtts_engine_count(void) { return g_engine_count; }

const mtts_engine_desc *mtts_engine_at(int index) {
    if (index < 0 || index >= g_engine_count) return NULL;
    return &g_engines[index].desc;
}

const mtts_engine_desc *mtts_engine_find(const char *key) {
    for (int i = 0; i < g_engine_count; i++)
        if (strcmp(g_engines[i].desc.key, key) == 0) return &g_engines[i].desc;
    return NULL;
}

int mtts_engine_loaded(const char *key) {
    for (int i = 0; i < g_engine_count; i++)
        if (strcmp(g_engines[i].desc.key, key) == 0) return g_engines[i].loaded;
    return 0;
}

int mtts_voice_count(const char *engine_key) {
    for (int i = 0; i < g_engine_count; i++)
        if (strcmp(g_engines[i].desc.key, engine_key) == 0)
            return g_engines[i].loaded ? g_engines[i].voice_count : 0;
    return 0;
}

const mtts_voice_desc *mtts_voice_at(const char *engine_key, int index) {
    for (int i = 0; i < g_engine_count; i++) {
        if (strcmp(g_engines[i].desc.key, engine_key) != 0) continue;
        if (!g_engines[i].loaded) return NULL;
        if (index < 0 || index >= g_engines[i].voice_count) return NULL;
        return &g_engines[i].voices[index];
    }
    return NULL;
}

mtts_stream *mtts_stream_start(const char *engine_key, const char *voice_key,
                               const char *text, int rate, int pitch,
                               int32_t *out_sample_rate) {
    if (!engine_key || !text || !out_sample_rate) return NULL;

    pthread_mutex_lock(&g_lock);
    if (!g_inited) {
        /* 首次请求前未见预热 → 此处同步补 (调用方如 AU 渲染线程本就会等) */
        pthread_mutex_unlock(&g_lock);
        mtts_global_init(NULL);
        pthread_mutex_lock(&g_lock);
    }
    mtts_engine_state *st = NULL;
    for (int i = 0; i < g_engine_count; i++)
        if (strcmp(g_engines[i].desc.key, engine_key) == 0) { st = &g_engines[i]; break; }
    if (!st || !st->loaded || !st->session) { pthread_mutex_unlock(&g_lock); return NULL; }

    const mtts_adapter_if *a = mtts_adapter_for(st->desc.key);
    void *impl = NULL;
    if (a && a->stream_start)
        impl = a->stream_start(st->session, voice_key, text, rate, pitch, out_sample_rate);
    pthread_mutex_unlock(&g_lock);
    if (!impl) return NULL;

    mtts_stream *s = malloc(sizeof(*s));
    s->a = a;
    s->impl = impl;
    return s;
}

int32_t mtts_stream_read(mtts_stream *s, int16_t *buf, int32_t want, int *done) {
    if (!s || !s->a || !buf || want <= 0) { if (done) *done = 1; return 0; }
    return s->a->stream_read(s->impl, buf, want, done);
}

void mtts_stream_cancel(mtts_stream *s) {
    if (s && s->a && s->a->stream_cancel) s->a->stream_cancel(s->impl);
}

void mtts_stream_free(mtts_stream *s) {
    if (!s) return;
    if (s->a && s->a->stream_free) s->a->stream_free(s->impl);
    free(s);
}
