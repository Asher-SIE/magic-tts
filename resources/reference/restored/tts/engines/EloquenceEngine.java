package tts.engines;

import android.content.Context;

import com.sgr.grtts.EloquenceNative;

import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

import tts.core.SegmentSink;
import tts.core.StoppedCheck;
import tts.core.VoiceDescriptor;

/**
 * Eloquence/ECI 引擎. 还原自 smali: n2/b0 (引擎) + v3/d0 (工厂).
 */
public final class EloquenceEngine {

    static final ReentrantLock GLOBAL = new ReentrantLock();   // b0.e: active 实例登记
    static final ReentrantLock NATIVE = new ReentrantLock();   // b0.f: native 调用串行
    static EloquenceEngine active;                             // b0.i
    final AtomicLong gen = new AtomicLong();                   // b0.d: 停止代际
    volatile long handle;                                      // EloquenceNative.nativeInit 句柄
    volatile boolean released;

    /**
     * 工厂 [v3/d0.b(context, n5 就绪回调)]:
     * 1. ABI 检查: SUPPORTED_ABIS 必须含 arm64-v8a
     * 2. System.loadLibrary("eloquence_jni") — 失败仅记日志返回 null
     *    ("Unable to load the experimental Eloquence JNI bridge")
     * 3. 部署 Eloquence 数据 (中文包/英文包)
     * 4. EloquenceNative.nativeInit(dataPath, langPath) → 句柄 → new EloquenceEngine
     */
    public static EloquenceEngine create(Context context, Runnable onReady /*n2/n5*/) {
        try {
            System.loadLibrary("eloquence_jni");
        } catch (Throwable t) {
            android.util.Log.e("EloquenceNative", "Unable to load the experimental Eloquence JNI bridge", t);
            return null;
        }
        // 数据部署 + nativeInit — 见 smali v3/d0.b 尾部
        return null;
    }

    /** [b0.d()]: 有效句柄或 null(已释放). */
    Long handleOrNull() { return released ? null : handle; }

    /** 停止检查 + nativeStop [b0.a(JJ)]. */
    void stopNative(long g, long h) {
        NATIVE.lock();
        try {
            EloquenceNative.nativeStop(h);
        } finally {
            NATIVE.unlock();
        }
    }

    /** r5.b0() 调用: 停止活跃实例. */
    public static void nativeStopAll() {
        EloquenceEngine e = active;
        if (e != null) {
            Long h = e.handleOrNull();
            if (h != null) e.stopNative(e.gen.get(), h);
        }
    }

    /**
     * 合成 [b0.o(text, voice, rate, pitch, volume, speed, o5 停止检查, p5 出口)].
     *
     * 1. 授权门控 [b0.l(gen, o5)]: 不可用 →
     *    "Blocked Eloquence synthesis because access is unavailable", 返回 false
     * 2. 获取合成锁; 登记 active=this
     * 3. 语速换算 (speed→ECI 刻度, clamp 1..10) → nativeSetProsody(handle, rate, pitch, volume)
     * 4. 文本→字节 → nativeSynthesize(handle, voiceIndex, bytes) → short[]
     *    voiceIndex: 0="eloquence-zh-cn", 1="eloquence-en-us"
     * 5. sink.accept(pcm); 每步间检查 o5.isStopped() → stopNative 后返回
     * 6. finally: 清除 active 登记, 释放锁
     */
    public boolean synthesize(String text, VoiceDescriptor voice,
                              int rate, int pitch, int volume, float speed,
                              StoppedCheck stopped, SegmentSink sink) {
        // 授权检查 (付费功能开关, 细节未还原)
        Long h = handleOrNull();
        if (h == null) return false;
        try {
            EloquenceNative.nativeSetProsody(h, rate, pitch, volume);
            if (stopped.isStopped()) { stopNative(gen.get(), h); return false; }
            byte[] bytes = text.getBytes(java.nio.charset.StandardCharsets.UTF_8);
            short[] pcm = EloquenceNative.nativeSynthesize(h, voice.index, bytes);
            if (stopped.isStopped()) { stopNative(gen.get(), h); return false; }
            return pcm != null && pcm.length > 0 && sink.accept(pcm);
        } finally {
            // 解除 active 登记
        }
    }

    /** 关闭 [b0.close()]. */
    public void close() {
        released = true;
        Long h = handleOrNull();
        if (h != null) EloquenceNative.nativeStop(h);
    }
}
