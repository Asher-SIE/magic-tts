package tts.core;

import com.sgr.grtts.GrTtsNative;

/**
 * 音频泵. 还原自 n2/q5.
 * 引擎产出的每块 PCM 先经过重采样流统一到任务采样率, 再交给下游回调(返回 false 表示停止).
 */
public final class AudioPump {
    final TtsEngineManager manager;      // b
    final long generation;               // c
    final Object /*c3/k*/ chunkCounter;  // d: 已交付块数(原子int)
    final Object /*c3/k*/ byteCounter;   // e: 已交付字节数(原子int)
    final Resampler resampler;           // f: 可为 null(采样率一致时)
    final Object /*c3/k*/ deliveredFlag; // g: 是否已产出音频
    final PCMConsumer callback;          // h: 下游 (见 EmbeddedTtsEngine 中的桥)

    AudioPump(TtsEngineManager manager, long generation, Object chunkCounter, Object byteCounter,
              Resampler resampler, Object deliveredFlag, PCMConsumer callback) {
        this.manager = manager;
        this.generation = generation;
        this.chunkCounter = chunkCounter;
        this.byteCounter = byteCounter;
        this.resampler = resampler;
        this.deliveredFlag = deliveredFlag;
        this.callback = callback;
    }

    /** 还原自 n2/q5.h(Object): 引擎输出入口. */
    public Object deliver(short[] pcm) {
        short[] out = pcm;
        if (resampler != null) {
            out = GrTtsNative.resampleStreamProcess(resampler.handle, pcm);
            if (out == null || out.length == 0) {
                return Boolean.TRUE; // 重采样器缓冲, 暂无输出
            }
        }
        return callback.accept(out);
    }

    /** 流结束时冲刷重采样器残余 (c0 中 resampleStreamFlush 的调用处). */
    public Object flushRemainder() {
        if (resampler == null) return Boolean.TRUE;
        short[] tail = GrTtsNative.resampleStreamFlush(resampler.handle);
        if (tail == null || tail.length == 0) return Boolean.TRUE;
        return callback.accept(tail);
    }
}
