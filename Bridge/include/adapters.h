#ifndef MTTS_ADAPTERS_H
#define MTTS_ADAPTERS_H

/* 各引擎适配器接口. 一个 adapter 服务一个或多个引擎 key;
 * 调用序列与原版实现一一对应 (注释标注出处). */

#include "engine_bridge.h"

typedef struct mtts_adapter_if {
    /* 探测: Engines/<key>/ 下库与数据是否就绪 (只读检查, 不抛错). */
    int  (*probe)(const char *engines_root, const mtts_engine_desc *desc);

    /* 建会话: 加载引擎 + 初始化. 失败返回 NULL → 引擎未就绪. */
    void *(*create)(const char *engines_root, const mtts_engine_desc *desc);

    /* 合成: 输出 int16 PCM. stop_gen/my_gen 为停止代际. */
    mtts_status (*synthesize)(void *session, const char *text,
                              int rate, int pitch,
                              int16_t **out_pcm, int32_t *out_samples,
                              int32_t *out_sample_rate,
                              const volatile unsigned *stop_gen, unsigned my_gen);

    void (*stop)(void *session);

    void (*destroy)(void *session);
} mtts_adapter_if;

/* 按 key 取 adapter; 未适配的 key 返回 NULL. */
const mtts_adapter_if *mtts_adapter_for(const char *key);

#endif
