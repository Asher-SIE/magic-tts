package tts.engines;

import com.sgr.grtts.GrTtsNative;

import java.util.concurrent.atomic.AtomicBoolean;

import tts.core.SegmentSink;
import tts.core.VoiceDescriptor;

/**
 * Nuance Vocalizer 引擎. 还原自 smali: n2/z5.
 * 依赖: es.codefactory.vocalizertts (VocalizerEngine, libNuanceVocalizer.so)
 *
 * Vocalizer 不支持任意语速 → 统一经 GrTtsNative 的 sonic 流变速.
 */
public final class VocalizerEngine {

    private static final String TAG = "VocalizerEngine";

    es.codefactory.vocalizertts.voices.VocalizerEngine engine;   // z5 实际引擎字段
    final AtomicBoolean speaking = new AtomicBoolean();
    SegmentSink currentSink;

    /** [z5.i(s5)]: 数据/引擎就绪检查(必要时初始化 Vocalizer 数据目录). */
    public synchronized boolean prepare(VoiceDescriptor voice) {
        return engine != null;
    }

    /**
     * 合成 [z5.l(text, voice, rate, pitch, volume, speed, p5)] — synchronized:
     * 1. prepare(voice)
     * 2. GrTtsNative.sonicStreamCreate(voice.sampleRate, speed)   ← 变速准备
     * 3. 文本规范化 [z5.d(text)]
     * 4. engine.setVolumeGain(volume) / setSpeechVolume(volume)
     *    engine.setSpeechRate(rate) / setSpeechPitch(pitch)
     *    engine.setSpeechReadMode(..) / setSpeechTextMode(..)
     * 5. engine.D(text) 朗读 → onAudioSamples(short[]) 回调
     *    → sonicStreamProcess → sink.accept (false 则停止)
     * 6. 读完: sonicStreamFlush → sink.accept; finally sonicStreamDestroy
     */
    public synchronized boolean synthesize(String text, VoiceDescriptor voice,
                                            int rate, int pitch, int volume, float speed,
                                            SegmentSink sink) {
        if (!prepare(voice)) return false;
        speaking.set(true);
        currentSink = sink;
        try {
            if (!GrTtsNative.sonicStreamCreate(voice.sampleRate, speed)) return false;
            engine.setVolumeGain(volume);
            engine.setSpeechVolume(volume);
            engine.setSpeechRate(rate);
            engine.setSpeechPitch(pitch);
            engine.setSpeechReadMode(0);
            engine.setSpeechTextMode(0);
            engine.D(normalize(text));            // 异步朗读, 音频从 onAudioSamples 进入
            // 等待 speaking 变为 false (onAudioSamples 返回 false 或朗读完成)
            short[] tail = GrTtsNative.sonicStreamFlush();
            if (tail != null && tail.length > 0 && !sink.accept(tail)) return false;
            return true;
        } finally {
            GrTtsNative.sonicStreamDestroy();
        }
    }

    /** 音频回调 [z5.onAudioSamples(short[]) → Boolean]: sonic 变速后送当前 sink. */
    public boolean onAudioSamples(short[] pcm) {
        short[] out = GrTtsNative.sonicStreamProcess(pcm);
        if (out == null || out.length == 0) return true;
        SegmentSink sink = currentSink;
        boolean cont = sink == null || sink.accept(out);
        if (!cont) speaking.set(false);
        return cont;
    }

    private String normalize(String text) { return text; }   // z5.d: Vocalizer 文本规范化

    /** 停止 [z5 内 VocalizerEngine.E()]. */
    public void stopSpeaking() {
        speaking.set(false);
        if (engine != null) engine.E();
    }

    /** 关闭 [z5.j()] (r5.a0 调用). */
    public void shutdown() { stopSpeaking(); }
}
