package tts.core;

/** 停止检查(流式引擎用). 还原自 n2/o5: a()→Boolean, 返回 true=已被停止. */
public interface StoppedCheck {
    boolean isStopped();
}
