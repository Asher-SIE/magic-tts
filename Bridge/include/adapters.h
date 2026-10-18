#ifndef MTTS_ADAPTERS_H
#define MTTS_ADAPTERS_H

/* 各引擎适配器接口. 一个 adapter 服务一个或多个引擎 key;
 * 调用序列与原版实现一一对应 (注释标注出处). */

#include "engine_bridge.h"

/* 发音人轻量扫描结果 (纯文件系统枚举, 不加载引擎库) */
typedef struct {
    char key[256];    /* 选择键: "ydvoice01" / MS 包目录名 */
    char name[128];   /* 显示名 */
    char locale[32];  /* BCP-47; 适配器不清楚时留空 → router 回退引擎级 */
} mtts_voice_scan;

typedef struct mtts_adapter_if {
    /* 探测: Engines/<key>/ 下库与数据是否就绪 (只读检查, 不抛错). */
    int  (*probe)(const char *engines_root, const mtts_engine_desc *desc);

    /* 轻量发音人扫描 (不 dlopen/不加载 .vl, 毫秒级) —
     * 系统查询 speechVoices 时必须秒回, 故与引擎初始化分离. */
    int  (*scan_voices)(const char *engines_root, const mtts_engine_desc *desc,
                        mtts_voice_scan *out, int max);

    /* 建会话: 加载引擎 + 初始化. 失败返回 NULL → 引擎未就绪. */
    void *(*create)(const char *engines_root, const mtts_engine_desc *desc);

    /* 发音人枚举 (create 成功后有效; 与 scan_voices 结果一致). */
    int  (*voice_count)(void *session);
    const char *(*voice_key)(void *session, int idx);   /* idx 越界 → NULL */
    const char *(*voice_name)(void *session, int idx);

    /* 流式合成: start 毫秒级返回; read 阻塞拉取; cancel/free 见 engine_bridge.h.
     * voice_key 为 NULL/"" 时用引擎默认发音人. */
    void *(*stream_start)(void *session, const char *voice_key, const char *text,
                          int rate, int pitch, int32_t *out_sample_rate);
    int32_t (*stream_read)(void *stream, int16_t *buf, int32_t want, int *done);
    void (*stream_cancel)(void *stream);
    void (*stream_free)(void *stream);

    void (*destroy)(void *session);

    /* 可选: 后台预热某发音人 (建会话 + 一次微型合成, 读尽收尾, 绝不 stop).
     * 真实 stream_start 到来时等待预热完成 (等待 ≤ 冷启成本, 永不亏).
     * 适配器不支持时为 NULL. (2026-10-18: HD 模型冷启 ~650ms 的治理) */
    void (*warm_voice)(void *session, const char *voice_key);
} mtts_adapter_if;

/* 按 key 取 adapter; 未适配的 key 返回 NULL. */
const mtts_adapter_if *mtts_adapter_for(const char *key);

#endif
