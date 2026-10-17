package tts.core;

/** 每段的音频出口. 还原自 n2/p5: a(short[])→Boolean, 返回 false=停止. */
public interface SegmentSink {
    boolean accept(short[] pcm);
}
