package tts.engines;

import android.content.Context;
import android.speech.tts.TextToSpeech;
import android.speech.tts.Voice;

import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

import tts.core.AudioPump;
import tts.core.StoppedCheck;
import tts.core.VoiceDescriptor;

/**
 * 系统引擎转发. 还原自 smali: n2/a5.
 * 通过 android.speech.tts.TextToSpeech 调用设备上其他 TTS 引擎,
 * 请求参数携带 "com.sgr.grtts.system_tts_origin" 防止递归回落到本应用.
 */
public final class SystemTtsEngine {

    private static final String TAG = "SystemTtsEngine";
    /** 本应用语音的特征标记 — 系统语音枚举时跳过带此特征的 voice. */
    static final String OWN_VOICE_FEATURE = "com.sgr.grtts.voice";

    final Context context;
    /** 引擎包名 → TextToSpeech 会话 [a5 内 ConcurrentHashMap]. */
    final ConcurrentHashMap<String, Object /*n2/z4 会话*/> engines = new ConcurrentHashMap<>();
    final AtomicLong gen = new AtomicLong();
    TextToSpeech currentTts;

    public SystemTtsEngine(Context context) { this.context = context; }

    /** [a5.d()]: 枚举系统可用引擎与其语音(排除本应用特征) → SYSTEM 语音列表. */
    public List<VoiceDescriptor> listInstalledVoices() {
        // 见 smali n2/a5.smali d() — 枚举 ENGINE_INTENT 服务, 建会话 getVoices()
        return null;
    }

    /**
     * 合成 [a5.C(text, voice, rate, pitch, volume, speed, o5 停止检查, q5 泵)] — synchronized:
     * 1. 目标引擎的 TextToSpeech 会话 (带 system_tts_origin 参数)
     * 2. getVoices() 挑选匹配语音 (跳过含 OWN_VOICE_FEATURE 的)
     * 3. setVoice(v); setPitch / setSpeechRate (Math.pow 换算, 见 smali)
     * 4. 文本按 TextToSpeech.getMaxSpeechInputLength() 切块
     * 5. 每块 [a5.o(o4, text, rate, speed, z4, q5)]: synthesizeToFile → 回读 PCM → pump.deliver
     * 6. 引擎拒绝设置时告警 "Engine rejected settings engine=.. voice=.. pitch=.. rate=.."
     */
    public synchronized boolean synthesize(String text, VoiceDescriptor voice,
                                            int rate, int pitch, int volume, float speed,
                                            StoppedCheck stopped, AudioPump pump) {
        TextToSpeech tts = currentTts;
        if (tts == null) return false;

        Voice chosen = null;
        for (Voice v : tts.getVoices()) {
            if (v.getFeatures() != null && v.getFeatures().contains(OWN_VOICE_FEATURE)) continue;
            // locale/名称匹配 voice.locale / voice.sdkVoice
            chosen = v;
            break;
        }
        if (chosen != null && tts.setVoice(chosen) != TextToSpeech.SUCCESS) {
            android.util.Log.w(TAG, "Engine rejected settings engine=" + voice.engine
                    + " voice=" + voice.voiceId + " pitch=" + pitch + " rate=" + rate);
        }
        tts.setPitch((float) Math.pow(pitch / 50.0, 2));       // 实际换算见 smali
        tts.setSpeechRate((float) Math.pow(rate / 100.0, 2));  // 实际换算见 smali

        int maxLen = Math.min(text.length(), TextToSpeech.getMaxSpeechInputLength());
        // 逐块合成 + 回读 → pump.deliver — 完整见 smali a5.o
        return false;
    }

    /** [a5.l()]: 停止. */
    public void stop() { if (currentTts != null) currentTts.stop(); }

    public void close() { /* 关闭所有会话 (a5.close) */ }
}
