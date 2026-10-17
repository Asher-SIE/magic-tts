package com.sgr.grtts;

import java.util.List;

/**
 * 永德(yongde) TTS 引擎 JNI 桥。完整还原自 smali: com/sgr/grtts/GrTtsNative.smali
 * native 库: libyongde_tts.so (assets/voices/ydvoice*.vl 为语音数据, initialize 时传入目录)
 */
public abstract class GrTtsNative {

    static {
        System.loadLibrary("yongde_tts");
    }

    // ==================== 核心 TTS 生命周期 ====================

    /** 初始化引擎. voiceDataPath = filesDir/yongde_data 绝对路径(内含语音数据). */
    public static native boolean initialize(String voiceDataPath);

    public static native boolean isInitialized();

    public static native void uninitialize();

    // ==================== 合成（核心调用） ====================

    /** 同步合成整段文本, 返回 16bit PCM(short[]), 采样率见 getSampleRate(). */
    public static native short[] synthesize(String text);

    /** 停止当前合成. */
    public static native void stop();

    // ==================== 参数 ====================

    public static native boolean setVoice(int index);
    public static native void setRate(int rate);
    public static native void setRateMultiplier(int multiplier);
    public static native void setPitch(int pitch);
    public static native void setVolume(int volume);

    // ==================== 语音枚举 ====================

    public static native int getVoiceCount();
    public static native String getVoiceId(int index);
    public static native String getVoiceName(int index);
    public static native int getSampleRate();

    // ==================== 文本处理 ====================

    /** 文本分段. */
    public static native String[] segmentText(String text);

    /** 每段语言: 与 segmentText() 结果一一对应 (0=中文, 1=英文). */
    public static native int[] getSegmentLanguages(String text);

    /** 日期/时间等文本规范化 (仅在主引擎为 YONGDE 时调用). */
    public static native String normalizeDateTime(String text);

    // ==================== 重采样流 (所有引擎统一输出采样率用) ====================

    public static native long resampleStreamCreate(int targetRate, int sourceRate);
    public static native void resampleStreamDestroy(long handle);
    public static native short[] resampleStreamProcess(long handle, short[] pcm);
    public static native short[] resampleStreamFlush(long handle);

    // ==================== Sonic 变速流 (Vocalizer/讯飞 等不支持任意语速的引擎用) ====================

    public static native boolean sonicStreamCreate(int sampleRate, float speed);
    public static native void sonicStreamDestroy();
    public static native short[] sonicStreamProcess(short[] pcm);
    public static native short[] sonicStreamFlush();

    /** 带句柄版本 (并发会话用, 如讯飞引擎的按会话变速). */
    public static native long sonicStreamCreateHandle(int sampleRate, float speed);
    public static native void sonicStreamDestroyHandle(long handle);
    public static native short[] sonicStreamProcessHandle(long handle, short[] pcm);
    public static native short[] sonicStreamFlushHandle(long handle);

    /** ISS xTTS 4.0 停止 (代理到 libissxtts40). */
    public static native boolean issStop40(long sessionPtr);

    // ==================== 静态辅助: 分段+语言 合并为 TextSegment 列表 ====================
    // 还原自 GrTtsNative.a(String). 原 Kotlin 编译产物, 逻辑:
    //   segments = segmentText(text); langs = getSegmentLanguages(text)
    //   return segments.zip(langs).map { (seg, lang) -> TextSegment(seg, lang) }

    /**
     * 分段并标注语言.
     * @return List&lt;TextSegment&gt; — 还原自 smali 中对 n2/e5(String,I) 的构造
     */
    public static List<Object /* tts.core.TextSegment */> a(String text) {
        // 见 tts/core/TextSegment.java; 完整实现见 smali GrTtsNative.smali#a
        throw new UnsupportedOperationException("见 tts.core.TextSegment — 由 TtsEngineManager.splitByLanguage 调用");
    }
}
