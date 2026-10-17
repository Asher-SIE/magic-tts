package tts.core;

/** PCM 消费者. 还原自 b3/l (Function&lt;short[],Boolean&gt;). */
public interface PCMConsumer {
    Boolean accept(short[] pcm);
}
