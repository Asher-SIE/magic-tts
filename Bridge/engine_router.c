/* 引擎路由 — 按引擎 key 分发到对应 adapter.
 * 每个引擎的调用序列见各自 adapter 文件头部注释. */
#include "engine_bridge.h"
#include "adapters.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MTTS_MAX_ENGINES 4

typedef struct {
    mtts_engine_desc desc;
    int  loaded;               /* probe + create 成功 */
    void *session;             /* adapter 私有会话 */
    volatile unsigned stop_gen; /* 停止代际 (在途合成凭代际自毙) */
} mtts_engine_state;

static mtts_engine_state g_engines[MTTS_MAX_ENGINES];
static int g_engine_count = 0;
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

int mtts_global_init(const char *engines_root) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_engines_root, sizeof(g_engines_root), "%s", engines_root ? engines_root : "");
    g_engine_count = 0;
    for (int i = 0; i < TABLE_LEN && g_engine_count < MTTS_MAX_ENGINES; i++) {
        mtts_engine_state *st = &g_engines[g_engine_count];
        memset(st, 0, sizeof(*st));
        st->desc = kEngineTable[i];

        const mtts_adapter_if *a = mtts_adapter_for(st->desc.key);
        st->loaded = 0;
        if (a && a->probe(g_engines_root, &st->desc)) {
            st->session = a->create(g_engines_root, &st->desc);
            if (st->session) st->loaded = 1;
        }
        g_engine_count++;
        fprintf(stderr, "[MagicTTS] engine %-13s %-16s %s\n", st->desc.key, st->desc.voice_name,
                st->loaded ? "READY" : "未就绪 (缺物料)");
    }
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

mtts_status mtts_synthesize(const char *engine_key, const char *text,
                            int rate, int pitch,
                            int16_t **out_pcm, int32_t *out_samples,
                            int32_t *out_sample_rate) {
    if (!engine_key || !text || !out_pcm || !out_samples || !out_sample_rate)
        return MTTS_ERR_PARAM;

    pthread_mutex_lock(&g_lock);
    mtts_engine_state *st = NULL;
    for (int i = 0; i < g_engine_count; i++)
        if (strcmp(g_engines[i].desc.key, engine_key) == 0) { st = &g_engines[i]; break; }
    if (!st) { pthread_mutex_unlock(&g_lock); return MTTS_ERR_NO_ENGINE; }

    unsigned gen = ++st->stop_gen;   /* 本次合成代际; mtts_cancel 递增使其失效 */

    mtts_status rc = MTTS_ERR_INIT;
    const mtts_adapter_if *a = mtts_adapter_for(st->desc.key);
    if (st->loaded && a && a->synthesize) {
        rc = a->synthesize(st->session, text, rate, pitch,
                           out_pcm, out_samples, out_sample_rate,
                           &st->stop_gen, gen);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void mtts_cancel(const char *engine_key) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_engine_count; i++) {
        if (strcmp(g_engines[i].desc.key, engine_key) == 0) {
            g_engines[i].stop_gen++;
            const mtts_adapter_if *a = mtts_adapter_for(engine_key);
            if (g_engines[i].loaded && a && a->stop)
                a->stop(g_engines[i].session);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void mtts_free(void *p) { free(p); }
