package com.iflytek.speech;

/** ISS 合成回调. 完整还原自 smali: com/iflytek/speech/ITtsListener.smali
 *  (n2/v0 引擎实现了它: onTtsMsgProc 处理结束/错误消息, onProgress 进度) */
public interface ITtsListener {
    void onProgress(int progress, int total);

    /** msgId: 消息类型; param: 参数; data: 附加数据. */
    void onTtsMsgProc(int msgId, int param, String data);
}
