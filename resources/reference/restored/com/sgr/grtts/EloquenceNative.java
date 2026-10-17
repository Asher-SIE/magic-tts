package com.sgr.grtts;

/**
 * Eloquence(ECI) TTS 引擎 JNI 桥. 完整还原自 smali: com/sgr/grtts/EloquenceNative.smali
 * native 库: libeloquence_jni.so (依赖 libeci.so/libenu.so/libchs.so/libchsrom.so)
 *
 * 会话式 API: nativeInit → nativeSetProsody → nativeSynthesize → nativeStop
 */
public final class EloquenceNative {

    public static final EloquenceNative INSTANCE = new EloquenceNative();

    // volatile 标志位在原实现中由引擎包装类(n2/b0)维护, 见 tts/engines/EloquenceEngine.java
    public static volatile boolean available;
    public static volatile boolean initialized;

    private EloquenceNative() {
    }

    /**
     * 初始化引擎, 返回会话句柄.
     * @param dataPath  Eloquence 数据目录
     * @param langPath  语言数据路径 (中文包/英文包)
     */
    public static native long nativeInit(String dataPath, String langPath);

    /** 设置韵律: rate/pitch/volume 均为引擎刻度(还原自 n2/b0.o 中的换算). */
    public static native void nativeSetProsody(long handle, int rate, int pitch, int volume);

    /**
     * 同步合成.
     * @param voiceIndex 语音索引 (0=中文, 1=英文; 由语音描述符 "eloquence-zh-cn"/"eloquence-en-us" 映射)
     * @param textBytes  文本字节 (按引擎要求编码)
     * @return 16bit PCM
     */
    public static native short[] nativeSynthesize(long handle, int voiceIndex, byte[] textBytes);

    public static native void nativeStop(long handle);
}
