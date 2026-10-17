package com.iflytek.speech;

/** native 会话句柄. 完整还原自 smali: com/iflytek/speech/NativeHandle.smali */
public class NativeHandle {
    public int err_ret;
    public long native_point;

    public NativeHandle() {
        err_ret = 0;
        native_point = 0L;
    }

    public int getErr_ret() { return err_ret; }
    public long getNative_point() { return native_point; }
    public void setErr_ret(int v) { err_ret = v; }
    public void reSet() { err_ret = 0; native_point = 0L; }
}
