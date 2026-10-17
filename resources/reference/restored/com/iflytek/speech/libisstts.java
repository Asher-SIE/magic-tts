package com.iflytek.speech;

/**
 * 讯飞 ISS 离线 xTTS 4.0 JNI 绑定. 完整还原自 smali: com/iflytek/speech/libisstts.smali
 * native 库: libissxtts40.so + libissauth.so
 *
 * 调用顺序 (还原自 n2/q0.b + n2/v0.e):
 *   setMachineCode(json) → initRes(resDir) → create(handle, listener)
 *   → setParam(handle, id, value)... → start(handle, text)
 *   → 循环 getAudioData(...) → stop/destroy
 */
public class libisstts {

    static {
        System.loadLibrary("issxtts40");
        System.loadLibrary("issauth");
    }

    /** 设置机器码/授权 JSON: {"key":{"imei_info":"0"},"path":"<dir>/lesl_tts"}. */
    public static synchronized native int setMachineCode(String json);

    /** 初始化资源目录 (filesDir/isstts/40/xTTSRes). */
    public static synchronized native int initRes(String resDir, int flag);

    /** 创建会话. handle.native_point 由 native 层填充. */
    public static synchronized native int create(NativeHandle handle, ITtsListener listener);

    /** 设置整数参数 (语速/音量/音高等, 参数 id 见 n2/v0.d(int) 映射). */
    public static synchronized native int setParam(NativeHandle handle, int paramId, int value);

    public static synchronized native int setParamEx(NativeHandle handle, int paramId, String value);

    /** 开始合成文本. */
    public static synchronized native int start(NativeHandle handle, String text);

    /** 拉取音频数据(阻塞直至有数据/结束). 返回值<=0 表示结束. */
    public static native int getAudioData(NativeHandle handle, byte[] buf, int len, int[] info);

    public static synchronized native int stop(NativeHandle handle);

    public static synchronized native int destroy(NativeHandle handle);

    public static synchronized native int unInitRes();

    public static synchronized native String getSpeakerList(NativeHandle handle, int flag);

    public static synchronized native String getVersion();

    public static synchronized native int setLogCfgParam(int level, String path);
}
