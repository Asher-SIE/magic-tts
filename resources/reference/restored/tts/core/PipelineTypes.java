package tts.core;

/** 引擎+语音对 [n2/l0] / 引擎结果标志 [n2/k] / ISS 语音条目 [n2/u0] — 包内辅助类型. */

/** 引擎+语音对. 还原自 n2/l0: (a=引擎, b=语音ID). */
final class EngineVoice {
    final EngineType engine;  // a
    final String voiceId;     // b

    EngineVoice(EngineType engine, String voiceId) {
        this.engine = engine;
        this.voiceId = voiceId;
    }
}

/** 引擎能力/结果标志. 还原自 n2/k: (a, b=失败原因n0, c=已产出音频, d). */
final class SegmentResult {
    final boolean a;
    final Object /*n2/n0 失败原因*/ reason; // b
    final boolean producedAudio;            // c
    final boolean d;

    SegmentResult(boolean a, Object reason, boolean producedAudio, boolean d) {
        this.a = a;
        this.reason = reason;
        this.producedAudio = producedAudio;
        this.d = d;
    }
}

/** ISS 语音条目. 还原自 n2/u0: (a=语音ID, b=显示名, c/d=参数). */
final class IssVoiceEntry {
    final String voiceId;     // a
    final String displayName; // b
    final int c;
    final int d;

    IssVoiceEntry(String voiceId, String displayName, int c, int d) {
        this.voiceId = voiceId;
        this.displayName = displayName;
        this.c = c;
        this.d = d;
    }
}
