package com.iflytek.cloud;

/** 编译验证桩. */
public final class SpeechSynthesizer {
    public boolean setParameter(String key, String value) { return true; }
    public int synthesizeToUri(String text, String uri, SynthesizerListener l) { return 0; }
    public void stopSpeaking() { }
    public boolean destroy() { return true; }
}
