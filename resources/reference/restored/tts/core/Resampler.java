package tts.core;

import com.sgr.grtts.GrTtsNative;

/** PCM 重采样流包装. 还原自 n2/j5. */
public final class Resampler implements AutoCloseable {
    public final long handle; // a

    public Resampler(int targetRate, int sourceRate) {
        handle = GrTtsNative.resampleStreamCreate(targetRate, sourceRate);
    }

    @Override
    public void close() {
        GrTtsNative.resampleStreamDestroy(handle);
    }
}
