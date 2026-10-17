package tts.core;

/** 引擎类型. 还原自 smali: n2/m0.smali (枚举名完整保留). */
public enum EngineType {
    /** 永德: libyongde_tts.so (本地, 通过 com.sgr.grtts.GrTtsNative). */
    YONGDE,
    /** Nuance Vocalizer: es.codefactory.vocalizertts (libNuanceVocalizer.so). */
    VOCALIZER,
    /** 讯飞 MSC 在线/离线 SDK: com.iflytek.cloud.SpeechSynthesizer (libmsc.so). */
    XUNFEI,
    /** 讯飞 ISS 离线 xTTS 4.0: libissxtts40.so (assets/isstts/40). */
    ISSTTS,
    /** 讯飞 ISS 离线 xTTS 3.0: libissxtts30.so (assets/isstts/30). */
    ISSTTS30,
    /** Microsoft 嵌入式语音 (NaturalVoice): EmbeddedSpeechConfig.fromPath, 流式. */
    NATURALVOICE,
    /** Microsoft 嵌入式语音 (OneCore): EmbeddedSpeechConfig.fromPath, 整段. */
    ONECORE,
    /** Eloquence/ECI: libeloquence_jni.so. */
    ELOQUENCE,
    /** 系统其他 TTS 引擎 (通过 android.speech.tts.TextToSpeech 转发). */
    SYSTEM
}
