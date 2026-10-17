package tts.core;

import java.util.List;

/**
 * 合成任务. 还原自 n2/i5.
 * plan(text) 规划后的产物: 原文 + 目标采样率(各段最大) + 分段列表.
 */
public final class SynthesisJob {
    public final String originalText;            // a
    public final int sampleRate;                 // b
    public final List<SpeechSegment> segments;   // c

    SynthesisJob(String originalText, String normalizedText /*原实现保留但未用*/,
                 int sampleRate, List<SpeechSegment> segments) {
        this.originalText = originalText;
        this.sampleRate = sampleRate;
        this.segments = segments;
    }
}
