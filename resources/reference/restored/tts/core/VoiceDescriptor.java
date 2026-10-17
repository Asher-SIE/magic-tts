package tts.core;

/**
 * 语音描述符. 还原自 smali: n2/s5.smali (构造参数 14 个, 按构造调用顺序命名)
 */
public final class VoiceDescriptor {
    public final String displayName;   // a: 显示名, 如 "聆小琪", "中文普通话"
    public final String voiceId;       // b: 语音 ID, 如 "lingxiaoqizhuli", "eloquence-zh-cn", yongde 的 getVoiceId
    public final EngineType engine;    // c: 所属引擎
    public final int index;            // d: 引擎内索引
    public final String sdkVoice;      // e: SDK 内部语音名 (OneCore/NaturalVoice 用)
    public final Gender gender;        // f
    public final int sampleRate;       // g: 引擎原生采样率 (16000/24000 等)
    public final String locale;        // h: 如 "zh-CN", "en-US"
    public final String i;             // i: (空)
    public final String j;             // j: SDK voice 名 (OneCore)
    public final String k;             // k: (空)
    public final String l;             // l: (空)
    public final String dataPath;      // m: 语音数据路径 (MS embedded/ISS)

    public VoiceDescriptor(String displayName, String voiceId, EngineType engine, int index,
                           String sdkVoice, Gender gender, int sampleRate, String locale,
                           String i, String j, String k, String l, String m) {
        this.displayName = displayName;
        this.voiceId = voiceId;
        this.engine = engine;
        this.index = index;
        this.sdkVoice = sdkVoice;
        this.gender = gender;
        this.sampleRate = sampleRate;
        this.locale = locale;
        this.i = i;
        this.j = j;
        this.k = k;
        this.l = l;
        this.dataPath = m;
    }

    /** 便捷构造: 静态内置语音 (YONGDE/XUNFEI/ISS/ELOQUENCE 用). */
    public VoiceDescriptor(String displayName, String voiceId, EngineType engine, int index,
                           Gender gender, int sampleRate, String locale) {
        this(displayName, voiceId, engine, index, null, gender, sampleRate, locale,
                null, null, null, null, null);
    }
}
