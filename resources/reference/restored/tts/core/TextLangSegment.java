package tts.core;

/** 文本分段+语言. 还原自 n2/e5 (a=文本, b=语言 0/1). */
final class TextLangSegment {
    final String text;
    final int lang; // 0=中文 1=英文 (对应 Lang)

    TextLangSegment(String text, int lang) {
        this.text = text;
        this.lang = lang;
    }
}
