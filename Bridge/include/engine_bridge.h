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

/* 轻量扫描: 只读文件系统, 枚举引擎与发音人 (不加载任何引擎库, 毫秒级).
 * 系统查询 speechVoices 时必须秒回 — 先 scan 后异步 init.
 * 返回物料就绪的引擎数. */
int mtts_global_scan(const char *engines_root);

/* 重量初始化: dlopen 引擎库 + 加载数据 (秒级, 可后台预热).
 * 之前未 scan 过则先 scan. 返回成功加载的引擎数. */
int mtts_global_init(const char *engines_root);

int  mtts_engine_count(void);
const mtts_engine_desc *mtts_engine_at(int index);
const mtts_engine_desc *mtts_engine_find(const char *key);

/* 引擎是否就绪 (库与数据均可用). */
int mtts_engine_loaded(const char *key);

/* ---- 发音人 (一个引擎内的多个可选语音) ----
 * 永德: voices/ 下每个 .vl 文件一个发音人;
 * MS: model/ 下每个含 SynthModel.license 的包一个发音人.
 * 描述在 mtts_global_scan 后即有效 (存活到进程结束). */
typedef struct {
    const char *key;        /* 选择键: "ydvoice01" / 包目录名; 传 "" = 默认 */
    const char *name;       /* 显示名: "永德·语音1" / "自然语音·晓晓" */
    const char *identifier; /* 系统语音 id: com.magictts.<engine>.<key> */
} mtts_voice_desc;

int mtts_voice_count(const char *engine_key);
const mtts_voice_desc *mtts_voice_at(const char *engine_key, int index);

/* ---- 流式合成 (渲染线程拉取) ----
 * start 立即返回 (MS 引擎毫秒级启动, 首块音频 ~0.7s 到达);
 * read 阻塞至有数据/流结束 (单次调用内部有界等待), 返回样本数,
 * *done=1 表示流已结束且数据读尽; cancel 可随时终止; free 释放. */
typedef struct mtts_stream mtts_stream;

mtts_stream *mtts_stream_start(const char *engine_key, const char *voice_key,
                               const char *text, int rate, int pitch,
                               int32_t *out_sample_rate);   /* 失败返回 NULL */

int32_t mtts_stream_read(mtts_stream *s, int16_t *buf, int32_t want_samples,
                         int *done);

void mtts_stream_cancel(mtts_stream *s);
void mtts_stream_free(mtts_stream *s);

#ifdef __cplusplus
}
#endif

#endif /* MTTS_ENGINE_BRIDGE_H */
