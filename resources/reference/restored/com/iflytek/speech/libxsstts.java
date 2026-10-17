package com.iflytek.speech;

/**
 * 讯飞 ISS 离线 xTTS 3.0 JNI 绑定. 完整还原自 smali: com/iflytek/speech/libxsstts.smali
 * native 库: libissxtts30.so + libissavth.so
 *
 * API 面与 libisstts 完全一致 (n2/t0 接口的两个实现),
 * 资源目录为 filesDir/isstts/30/xTTSRes, 授权 json path 为 "<dir>/lesl".
 */
public class libxsstts {

    static {
        System.loadLibrary("issxtts30");
        System.loadLibrary("issavth");
    }

    public static synchronized native int setMachineCode(String json);
    public static synchronized native int initRes(String resDir, int flag);
    public static synchronized native int create(NativeHandle handle, ITtsListener listener);
    public static synchronized native int setParam(NativeHandle handle, int paramId, int value);
    public static synchronized native int setParamEx(NativeHandle handle, int paramId, String value);
    public static synchronized native int start(NativeHandle handle, String text);
    public static native int getAudioData(NativeHandle handle, byte[] buf, int len, int[] info);
    public static synchronized native int stop(NativeHandle handle);
    public static synchronized native int destroy(NativeHandle handle);
    public static synchronized native int unInitRes();
    public static synchronized native String getSpeakerList(NativeHandle handle, int flag);
    public static synchronized native String getVersion();
    public static synchronized native int setLogCfgParam(int level, String path);
}
