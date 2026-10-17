package tts.engines;

import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;

import com.iflytek.cloud.SpeechError;
import com.iflytek.cloud.SpeechSynthesizer;
import com.iflytek.cloud.SynthesizerListener;
import com.iflytek.cloud.util.ResourceUtil;
import com.sgr.grtts.GrTtsNative;

import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

import tts.core.Gender;
import tts.core.SegmentSink;

/**
 * 讯飞 MSC 引擎. 还原自 smali: n2/j6 (实现 SynthesizerListener).
 * 依赖: libmsc.so (com.iflytek.cloud SDK), 语音资源 assets/tts/*.jet (17 个)
 *
 * 音频通路: SpeechSynthesizer.synthesizeToUri(文件) → 监听回调读 PCM
 *           → GrTtsNative.sonicStream* (变速) → SegmentSink
 */
public final class XunfeiEngine implements SynthesizerListener {

    private static final String TAG = "XunfeiEngine";

    SpeechSynthesizer synthesizer;                    // j6.h
    HandlerThread thread;                             // 回调线程
    Handler handler;
    final AtomicLong sessionGen = new AtomicLong();   // 会话代际
    final ConcurrentHashMap<Long, Object /*n2/j4 会话状态*/> sessions = new ConcurrentHashMap<>();
    long sonicHandle;                                 // sonic 变速流句柄
    volatile boolean inited;

    public XunfeiEngine(Context context) {
        // [j6.<init>(Context)]:
        //   离线资源路径 = ResourceUtil.generateResourcePath(context, RESOURCE_TYPE.assets, "tts/common.jet")
        //                 + ... 各 .jet
        //   SpeechSynthesizer.getInstance(context); setParameter(ENGINE_TYPE=local 等)
    }

    public boolean initOk() { return inited; }        // [j6.f()]

    /** 性别映射 [n2/m6.a(voiceId) → e6]. */
    public static Gender genderOf(String voiceId) {
        switch (voiceId) {
            case "john": case "xiaokun": case "xiaoqiang": case "yifeng": return Gender.MALE;
            default: return Gender.FEMALE; // catherine/xiaoyan/... 为女声(完整表见 n2/m6)
        }
    }

    /**
     * 合成 [j6.i(text, k6 韵律, p5 出口)].
     * k6 = n2/k6(text, rate, pitch, volume, speed), 由调用方从 SpeechSegment 构造.
     */
    public boolean synthesize(String text, tts.core.SpeechSegment prosody, SegmentSink sink) {
        if (!inited) {
            android.util.Log.e(TAG, "Not initialized");
            return false;
        }
        ReentrantLock lock = new ReentrantLock();
        lock.lock();
        try {
            long sid = sessionGen.incrementAndGet();
            AtomicBoolean stopped = new AtomicBoolean(false);
            if (handler != null) handler.removeCallbacksAndMessages(null);

            applySonic(prosody.speed, sid);        // [j6.a(F,J)] sonicStreamCreateHandle(16000, speed)
            applyProsody(prosody);                 // [j6.g(k6)] setParameter 语速/音量/音高
            boolean started = startSynthesis(text, sid);  // [j6.c(String,J)] synthesizeToUri(text, file, this)
            if (!started) {
                cleanupSession(sid);               // [j6.b(Long)]
                return false;
            }
            Object done = awaitCompletion(sid);    // [j6.d(J) → j4] 等 onCompleted
            boolean ok = drainAudio(sid, sink);    // [j6.e(J, bytes)]: 会话 PCM → sonic → sink
            stopped.set(false);
            cleanupSession(sid);
            return ok;
        } catch (Throwable t) {
            android.util.Log.w(TAG, String.valueOf(t.getMessage()));
            return false;
        } finally {
            lock.unlock();
        }
    }

    private void applySonic(float speed, long sid) { /* sonicStreamCreateHandle(16000, speed) */ }
    private void applyProsody(tts.core.SpeechSegment p) { /* setParameter(SpeechConstant.*, ...) */ }
    private boolean startSynthesis(String text, long sid) { return false; /* synthesizeToUri == 0 */ }
    private Object awaitCompletion(long sid) { return null; }
    private boolean drainAudio(long sid, SegmentSink sink) { return false; }
    private void cleanupSession(long sid) {
        GrTtsNative.sonicStreamDestroyHandle(sonicHandle);
    }

    /** [j6.h()] 停止全部会话. */
    public void stopAll() { if (synthesizer != null) synthesizer.stopSpeaking(); }

    /** 关闭 (r5.a0 调用): destroy + 回调线程退出. */
    public void shutdown() {
        if (synthesizer != null) synthesizer.destroy();
        if (thread != null) thread.quitSafely();
    }

    // ===== SynthesizerListener [j6]: PCM 与完成信号都从回调进入 =====
    @Override public void onSpeakBegin() { }

    @Override public void onBufferProgress(int progress, int beginPos, int endPos, String info) {
        // 读取合成产物 PCM → sonicStreamProcessHandle(sonicHandle, pcm) → 会话队列
    }

    @Override public void onSpeakPaused() { }
    @Override public void onSpeakResumed() { }
    @Override public void onSpeakProgress(int progress, int beginPos, int endPos) { }

    @Override public void onCompleted(SpeechError error) {
        // error==null → 成功解锁等待; 否则 Log error.getErrorDescription()
    }

    @Override public void onEvent(int eventType, int arg1, int arg2, android.os.Bundle obj) { }
}
