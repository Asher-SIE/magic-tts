package tts.core;

/**
 * 合成分段. 还原自 n2/l5.
 * a=文本, b=目标语音(VoiceDescriptor), c=是否英文段,
 * d/e=源/目标语速, f=音量, g=音高, h=语速倍率(float)
 */
public final class SpeechSegment {
    public final String text;               // a
    public final VoiceDescriptor voice;     // b
    public final boolean isEnglish;         // c
    public final int sourceRate;            // d
    public final int targetRate;            // e
    public final int volume;                // f
    public final int pitch;                 // g
    public final float speed;               // h

    SpeechSegment(String text, VoiceDescriptor voice, boolean isEnglish,
                  int sourceRate, int targetRate, int volume, int pitch, float speed) {
        this.text = text;
        this.voice = voice;
        this.isEnglish = isEnglish;
        this.sourceRate = sourceRate;
        this.targetRate = targetRate;
        this.volume = volume;
        this.pitch = pitch;
        this.speed = speed;
    }

    /** 还原自 n2/l5.a(l5, String): 复制并替换文本 (相邻同语音段合并时用). */
    public SpeechSegment withText(String newText) {
        return new SpeechSegment(newText, voice, isEnglish, sourceRate, targetRate, volume, pitch, speed);
    }
}
