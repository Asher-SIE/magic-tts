package tts.core;

/** 语音选择结果. 还原自 n2/k5: (主引擎,主语音,回退引擎,回退语音,允许回退). */
final class VoiceSelection {
    final EngineType engine;         // a
    final String voiceId;            // b
    final EngineType fallbackEngine; // c
    final String fallbackVoiceId;    // d
    final boolean allowFallback;     // e

    VoiceSelection(EngineType engine, String voiceId, EngineType fallbackEngine,
                   String fallbackVoiceId, boolean allowFallback) {
        this.engine = engine;
        this.voiceId = voiceId;
        this.fallbackEngine = fallbackEngine;
        this.fallbackVoiceId = fallbackVoiceId;
        this.allowFallback = allowFallback;
    }
}
