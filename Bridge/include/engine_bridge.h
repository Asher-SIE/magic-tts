#ifndef MTTS_ENGINE_BRIDGE_H
#define MTTS_ENGINE_BRIDGE_H

/* Magic TTS — 引擎桥接公共接口.
 * 调用序列与原版实现一一对应:
 *   原版:  引擎管理器 synthesize → 各引擎包装类 → JNI
 *   macOS: mtts_synthesize → engine_adapter → 引擎库/数据
 *
 * 引擎二进制与数据放置约定:
 *   <App Group 容器>/Engines/<key>/
 *     引擎库 (如需要) + 数据目录
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MTTS_OK = 0,
    MTTS_ERR_NO_ENGINE = -1,     /* 未注册该引擎 */
    MTTS_ERR_NO_DYLIB = -2,      /* 引擎库不存在 */
    MTTS_ERR_INIT = -3,          /* 引擎初始化失败 */
    MTTS_ERR_SYNTH = -4,         /* 合成失败 */
    MTTS_ERR_PARAM = -5
} mtts_status;

/* 引擎描述 */
typedef struct {
    const char *key;           /* "yongde" | "naturalvoice" | "onecore" */
    const char *voice_name;    /* 系统语音列表显示名 */
    const char *identifier;    /* com.magictts.<key> */
    const char *locale;        /* "zh-CN" */
    const char *dylib_name;    /* 期望的引擎库文件名 (相对 Engines/<key>/) */
    int   native_sample_rate;  /* 引擎原生采样率 */
} mtts_engine_desc;

/* 初始化: 扫描 engines_root (Engines 目录), 加载各引擎.
 * 返回成功加载的引擎数. */
int mtts_global_init(const char *engines_root);

int  mtts_engine_count(void);
const mtts_engine_desc *mtts_engine_at(int index);
const mtts_engine_desc *mtts_engine_find(const char *key);

/* 引擎是否就绪 (库与数据均可用). */
int mtts_engine_loaded(const char *key);

/* 合成一段文本.
 * rate: 0..500, 100=常速; pitch: 0..100, 50=常音高.
 * 出参 *out_pcm 为 int16 PCM (调用方 mtts_free 释放), *out_sample_rate 为其采样率. */
mtts_status mtts_synthesize(const char *engine_key, const char *text,
                            int rate, int pitch,
                            int16_t **out_pcm, int32_t *out_samples,
                            int32_t *out_sample_rate);

/* 停止当前合成 (停止代际聚合). */
void mtts_cancel(const char *engine_key);

void mtts_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* MTTS_ENGINE_BRIDGE_H */
