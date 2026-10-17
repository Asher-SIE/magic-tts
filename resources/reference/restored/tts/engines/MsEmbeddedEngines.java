package tts.engines;

import com.microsoft.cognitiveservices.speech.EmbeddedSpeechConfig;
import com.microsoft.cognitiveservices.speech.PropertyId;
import com.microsoft.cognitiveservices.speech.SpeechSynthesizer;

import java.util.List;

import tts.core.SegmentSink;
import tts.core.VoiceDescriptor;

/** Microsoft 嵌入式语音两个包装: NaturalVoice(流式, n2/f1) 与 OneCore(整段, n2/u1). */
public final class MsEmbeddedEngines {
    private MsEmbeddedEngines() {
    }

    /**
     * NaturalVoice — 流式引擎. 还原自 n2/f1.
     * 模型: filesDir 下的嵌入式语音模型目录 (SpeechSDK embedded TTS, 24kHz).
     */
    public static final class NaturalVoiceEngine {

        static final com.microsoft.cognitiveservices.speech.SpeechSynthesisOutputFormat FORMAT =
                com.microsoft.cognitiveservices.speech.SpeechSynthesisOutputFormat.Raw24Khz16BitMonoPcm; // f1.i
        SpeechSynthesizer synthesizer;      // 会话对象 n2/l.c
        volatile boolean loaded;

        /** [f1.m(path)]: EmbeddedSpeechConfig.fromPath(modelDir) → SpeechSynthesizer(cfg, null) + Synthesizing 事件挂接. */
        public boolean load(String modelDir) {
            try {
                EmbeddedSpeechConfig cfg = EmbeddedSpeechConfig.fromPath(modelDir);
                cfg.setProperty(PropertyId.SpeechServiceResponse_RequestSentenceBoundary, "true");
                cfg.setProperty(PropertyId.SpeechServiceResponse_RequestPunctuationBoundary, "true");
                synthesizer = new SpeechSynthesizer(cfg, null);
                // synthesizer.Synthesizing.addEventListener(...) → 流式 PCM (n2/l 会话)
                loaded = true;
                return true;
            } catch (Throwable t) {
                return false;
            }
        }

        public List<VoiceDescriptor> listVoices() { return null; }   // f1.c: 模型目录枚举语音

        /**
         * 合成 [f1.z(text, voice, rate, pitch, volume, speed, p5)]:
         * 1. 文本预处理: 切句(标点/长度), 丢空段
         * 2. 逐句构建 SSML [f1.b(text, voice, rate)] — 含 prosody 语速/音高标签
         * 3. synthesizer.SpeakSsmlAsync(ssml)
         * 4. Synthesizing 事件 → short[] → p5.accept
         * 5. 结束: ResultReason.SynthesizingAudioCompleted 校验 [f1.u → a1 结果]
         */
        public boolean synthesize(String text, VoiceDescriptor voice,
                                  int rate, int pitch, int volume, float speed, SegmentSink sink) {
            if (!loaded) return false;
            // 完整实现见 smali n2/f1.smali (z/u/m/b 方法族, 15160 行)
            return false;
        }

        public void stopStreaming() { if (synthesizer != null) synthesizer.StopSpeakingAsync(); }  // f1.x()
        public void close() { if (synthesizer != null) synthesizer.close(); }                      // f1.w()
    }

    /**
     * OneCore — 整段引擎. 还原自 n2/u1.
     * 与 NaturalVoice 同用 EmbeddedSpeechConfig.fromPath, 但一次 SpeakSsmlAsync 拿整段结果.
     */
    public static final class OneCoreEngine {

        SpeechSynthesizer synthesizer;
        volatile boolean loaded;

        /** [u1.h(path)]. */
        public boolean load(String modelDir) {
            try {
                EmbeddedSpeechConfig cfg = EmbeddedSpeechConfig.fromPath(modelDir);
                synthesizer = new SpeechSynthesizer(cfg, null);
                loaded = true;
                return true;
            } catch (Throwable t) {
                return false;
            }
        }

        public List<VoiceDescriptor> listVoices() { return null; }   // u1.a

        /**
         * 合成 [u1.n(text, voice, rate, pitch, volume, speed, p5)]:
         * SSML [u1.c(text, voice, rate)] → SpeakSsmlAsync → SpeechSynthesisResult
         * → 结果音频流分块 → p5.accept. 结果详情解析 [u1.j(result)].
         */
        public boolean synthesize(String text, VoiceDescriptor voice,
                                  int rate, int pitch, int volume, float speed, SegmentSink sink) {
            if (!loaded) return false;
            // 完整实现见 smali n2/u1.smali (n/m/c 方法族)
            return false;
        }

        public void close() { if (synthesizer != null) synthesizer.close(); }  // u1.l()
    }
}
