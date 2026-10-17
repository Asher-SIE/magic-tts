package com.iflytek.cloud;

/** 编译验证桩. */
public interface SynthesizerListener {
    void onSpeakBegin();
    void onBufferProgress(int progress, int beginPos, int endPos, String info);
    void onSpeakPaused();
    void onSpeakResumed();
    void onSpeakProgress(int progress, int beginPos, int endPos);
    void onCompleted(SpeechError error);
    void onEvent(int eventType, int arg1, int arg2, android.os.Bundle obj);
}
