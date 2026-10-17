package com.microsoft.cognitiveservices.speech;

import com.microsoft.cognitiveservices.speech.util.EventHandlerImpl;

/** 编译验证桩. */
public final class SpeechSynthesizer {
    public final EventHandlerImpl Synthesizing = new EventHandlerImpl();
    public SpeechSynthesizer(EmbeddedSpeechConfig cfg, Object audioConfig) { }
    public java.util.concurrent.Future<SpeechSynthesisResult> SpeakSsmlAsync(String ssml) { return null; }
    public java.util.concurrent.Future<?> StopSpeakingAsync() { return null; }
    public void close() { }
}
