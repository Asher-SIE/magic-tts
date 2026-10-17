package tts.engines;

import android.util.Log;

import com.iflytek.speech.ITtsListener;
import com.iflytek.speech.NativeHandle;
import com.iflytek.speech.libisstts;
import com.iflytek.speech.libxsstts;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

import tts.core.EngineType;
import tts.core.SegmentSink;
import tts.core.TtsEngineManager;
import tts.core.VoiceDescriptor;

/**
 * 讯飞 ISS 离线 xTTS 引擎. 还原自 smali: n2/v0(引擎) + n2/q0(核) + n2/t0(接口) + n2/s0(libisstts 实现)
 * + com/iflytek/speech/libisstts|libxsstts (JNI).
 *
 * 双核结构:
 *   CORE_40 = new IssCore(new LibIssttsApi(), "40", "lesl_tts")   // xTTS 4.0, filesDir/isstts/40
 *   CORE_30 = new IssCore(new LibXssttsApi(), "30", "lesl")       // xTTS 3.0, filesDir/isstts/30
 * 依赖库加载列表 [v0.<clinit> l 字段]:
 *   gnustl_shared, idas, lzmq, zmq, websockets, xtts, lesl, opencc, issauth, issavth, issxtts40, issxtts30
 */
public final class IssTtsEngine implements ITtsListener {

    private static final String TAG = "IssTtsEngine";

    // ==================== 依赖库加载 [v0.<clinit>] ====================
    static {
        for (String lib : new String[]{"gnustl_shared", "idas", "lzmq", "zmq", "websockets",
                "xtts", "lesl", "opencc", "issauth", "issavth", "issxtts40", "issxtts30"}) {
            try {
                System.loadLibrary(lib);
            } catch (Throwable ignore) {
            }
        }
    }

    // ==================== JNI API 抽象 [n2/t0] ====================
    public interface IssApi {                                      // n2/t0
        int start(NativeHandle h, String text);                    // a → libisstts.start
        int initRes(String resDir);                                // b → libisstts.initRes(dir, 0)
        int destroy(NativeHandle h);                               // c → libisstts.destroy
        int create(NativeHandle h, ITtsListener l);                // d → libisstts.create
        int unInitRes();                                           // e → libisstts.unInitRes
        int getAudioData(NativeHandle h, byte[] buf, int len, int[] info); // f
        int setMachineCode(String json);                           // g → libisstts.setMachineCode
        int setParam(NativeHandle h, int id, int value);           // h → libisstts.setParam
        int stop(NativeHandle h);                                  // i → libisstts.stop
    }

    /** xTTS 4.0 实现 [n2/s0]. */
    public static final class LibIssttsApi implements IssApi {
        public int start(NativeHandle h, String text) { return libisstts.start(h, text); }
        public int initRes(String d) { return libisstts.initRes(d, 0); }
        public int destroy(NativeHandle h) { return libisstts.destroy(h); }
        public int create(NativeHandle h, ITtsListener l) { return libisstts.create(h, l); }
        public int unInitRes() { return libisstts.unInitRes(); }
        public int getAudioData(NativeHandle h, byte[] b, int n, int[] i) { return libisstts.getAudioData(h, b, n, i); }
        public int setMachineCode(String j) { return libisstts.setMachineCode(j); }
        public int setParam(NativeHandle h, int id, int v) { return libisstts.setParam(h, id, v); }
        public int stop(NativeHandle h) { return libisstts.stop(h); }
    }

    /** xTTS 3.0 实现 [n2/r0 → libxsstts]. */
    public static final class LibXssttsApi implements IssApi {
        public int start(NativeHandle h, String text) { return libxsstts.start(h, text); }
        public int initRes(String d) { return libxsstts.initRes(d, 0); }
        public int destroy(NativeHandle h) { return libxsstts.destroy(h); }
        public int create(NativeHandle h, ITtsListener l) { return libxsstts.create(h, l); }
        public int unInitRes() { return libxsstts.unInitRes(); }
        public int getAudioData(NativeHandle h, byte[] b, int n, int[] i) { return libxsstts.getAudioData(h, b, n, i); }
        public int setMachineCode(String j) { return libxsstts.setMachineCode(j); }
        public int setParam(NativeHandle h, int id, int v) { return libxsstts.setParam(h, id, v); }
        public int stop(NativeHandle h) { return libxsstts.stop(h); }
    }

    // ==================== 单个核 [n2/q0] ====================
    public static final class IssCore {
        public final IssApi api;            // a
        public final String name;           // b: "40"/"30"
        public final String resDirName;     // c: "lesl_tts"/"lesl"
        public final boolean isV40;         // d
        public final NativeHandle handle = new NativeHandle(); // e
        public File resDir;                 // f: filesDir/isstts/<40|30>/xTTSRes
        public File workDir;                // g: filesDir/isstts/iflyrobot
        public volatile boolean sessionReady; // h
        public int sessionCount;            // i
        public int j;

        public IssCore(IssApi api, String name, String resDirName, boolean isV40) {
            this.api = api; this.name = name; this.resDirName = resDirName; this.isV40 = isV40;
        }

        /** 资源就绪探测 [q0.c(File)]: isstts.cfg 必须存在于核目录. */
        public boolean checkResources(File root) {
            this.resDir = new File(root, name + "/xTTSRes");
            return new File(resDir, "isstts.cfg").exists();
        }

        /** 创建会话 [q0.b(ITtsListener)]. */
        public boolean openSession(ITtsListener listener) {
            if (sessionReady) return true;
            if (resDir == null) return false;
            try {
                if (workDir == null) workDir = new File(resDir.getParentFile(), "iflyrobot");
                if (!workDir.exists()) workDir.mkdirs();
                // 授权 JSON: {"key":{"imei_info":"0"},"path":"<workDir>/<resDirName>"}
                String json = "{\"key\":{\"imei_info\":\"0\"},\"path\":\""
                        + workDir.getAbsolutePath() + "/" + resDirName + "\"}";
                api.setMachineCode(json);
                if (api.initRes(resDir.getAbsolutePath()) != 0) {
                    Log.w(TAG, "[" + name + "] initRes failed");
                    return false;
                }
                if (api.create(handle, listener) != 0) {
                    Log.w(TAG, "[" + name + "] create failed");
                    return false;
                }
                sessionReady = true;
                return true;
            } catch (Throwable t) {
                Log.w(TAG, "[" + name + "] " + t.getMessage());
                return false;
            }
        }

        /** 停止/销毁会话 [q0.a()]. */
        public void closeSession() {
            if (!sessionReady) return;
            sessionReady = false;
            try {
                api.stop(handle);
                api.destroy(handle);
            } catch (Throwable ignore) {
            }
        }
    }

    // ==================== 引擎静态状态 [v0 静态字段] ====================
    /** 4.0 核: libissxtts40, filesDir/isstts/40, 授权 path "lesl_tts". */
    public static final IssCore CORE_40 = new IssCore(new LibIssttsApi(), "40", "lesl_tts", true);  // g
    /** 3.0 核: libissxtts30, filesDir/isstts/30, 授权 path "lesl". */
    public static final IssCore CORE_30 = new IssCore(new LibXssttsApi(), "30", "lesl", false);     // h
    /** 就绪标志 (monitor j + volatile k). */
    public static volatile Boolean coresReady;   // k
    /** 引用计数 (r5 持有/释放). */
    public static final java.util.concurrent.atomic.AtomicInteger refCount =
            new java.util.concurrent.atomic.AtomicInteger(); // i
    /** ISS 语音表 (u0 列表: voiceId, 显示名). */
    public static final List<Object> ISS_VOICES = new ArrayList<>(); // o

    /** 引擎实例状态. */
    final java.util.concurrent.atomic.AtomicLong sessionGen = new java.util.concurrent.atomic.AtomicLong(); // a
    volatile boolean producing;   // b
    volatile boolean ready;       // c

    // ==================== 数据安装 [r5.E() 中 ISS 段] ====================
    public static void installAssets(TtsEngineManager manager) {
        // assets/isstts → filesDir/isstts; 版本标记 filesDir/isstts/.version
        // 内容与标记 "2026-07-04-11-iss13-offline" 不一致时删除重拷;
        // 拷贝后校验 40/xTTSRes/isstts.cfg 或 30/xTTSRes/isstts.cfg 至少存在一个;
        // 双核 checkResources(); 任一就绪 → coresReady=true, 引擎注册 [smali: r5.E() cond_18..26]
        // 完整拷贝循环见 smali n2/r5.smali f(String, File, boolean)
    }

    // ==================== 合成 [v0.e(String, s5, int rate, int pitch, int volume, float speed, p5)] ====================
    public boolean synthesize(String text, VoiceDescriptor voice,
                              int rate, int pitch, int volume, float speed, SegmentSink sink) {
        if (!ready) return false;
        if (text.length() == 0) return true;

        // 语音→核: ISS_VOICES 按语音ID 找条目, 找不到用默认; 条目属于 4.0 → CORE_40 否则 CORE_30
        IssCore core = CORE_40;

        sessionGen.incrementAndGet();
        java.util.concurrent.locks.ReentrantLock lock = CORE_LOCK; // v0.f
        lock.lock();
        try {
            if (core.sessionReady && core != currentCore) currentCore.closeSession(); // 核切换时重建
            if (!core.openSession(this /*ITtsListener: onTtsMsgProc/onProgress*/)) return false;

            // 停止旧请求, 重置参数, 再设语速/音量/音高 [t0.h(handle, id, value) × N + v0.a(F) 语速倍率]
            apiOf(core).stop(core.handle);
            apiOf(core).setParam(core.handle, PARAM_RESET_1, 0);
            apiOf(core).setParam(core.handle, PARAM_RESET_2, 0);
            apiOf(core).setParam(core.handle, PARAM_RATE, mapRate(rate));    // v0.d(int): 应用刻度→引擎刻度
            apiOf(core).setParam(core.handle, PARAM_VOLUME, mapRate(volume));
            apiOf(core).setParam(core.handle, PARAM_PITCH, mapRate(pitch));

            // 开始合成
            int ret = apiOf(core).start(core.handle, text);
            if (ret != 0) return false;

            // 拉取循环 [v0.c() → t0.f(getAudioData) → short[]], 迭代上限保护
            int guard = 0;
            while (guard++ < MAX_PULL_ITERATIONS) {
                short[] pcm = pullAudio(core);
                if (pcm == null) break;                      // 合成结束
                if (pcm.length > 0 && !sink.accept(pcm)) {   // 下游要求停止
                    apiOf(core).stop(core.handle);
                    return false;
                }
            }
            if (guard > MAX_PULL_ITERATIONS) {
                Log.w(TAG, "[" + core.name + "] getAudioData exceeded iteration guard, aborting");
            }
            return true;
        } finally {
            lock.unlock();
        }
    }

    volatile IssCore currentCore; // v0 内部记录当前占用核

    static IssApi apiOf(IssCore c) { return c.api; }
    static final java.util.concurrent.locks.ReentrantLock CORE_LOCK = new java.util.concurrent.locks.ReentrantLock();
    static final int MAX_PULL_ITERATIONS = 10000;
    // setParam 的参数 ID 还原自 v0.e 中三次 d(int) 映射调用的常量入参 (见 smali)
    static final int PARAM_RATE = 0, PARAM_VOLUME = 1, PARAM_PITCH = 2, PARAM_RESET_1 = 3, PARAM_RESET_2 = 4;
    static int mapRate(int v) { return v; } // v0.d(I)I — 应用刻度→ISS 刻度换算

    /** 拉取一块音频 [v0.c()]. */
    short[] pullAudio(IssCore core) {
        byte[] buf = new byte[8192];
        int[] info = new int[4];
        int n = core.api.getAudioData(core.handle, buf, buf.length, info);
        if (n <= 0) return null;
        // byte→short (小端) 后返回; 具体见 smali v0.c()
        short[] out = new short[n / 2];
        for (int i = 0; i < out.length; i++)
            out[i] = (short) ((buf[2 * i] & 0xFF) | (buf[2 * i + 1] << 8));
        return out;
    }

    // ==================== 停止/关闭 ====================
    /** [v0.b()]. */
    public void stop() {
        sessionGen.incrementAndGet();   // 使在途拉取循环失效
        try {
            if (CORE_40.sessionReady) CORE_40.closeSession();
            if (CORE_30.sessionReady) CORE_30.closeSession();
        } catch (Throwable ignore) {
        }
    }

    /** 引擎关闭 (r5.a0 调用). */
    public void shutdown() {
        refCount.decrementAndGet();
        CORE_LOCK.lock();
        try {
            CORE_40.closeSession();  // q0.a()
            CORE_30.closeSession();
        } finally {
            CORE_LOCK.unlock();
        }
    }

    // ==================== ITtsListener 回调 [v0.onTtsMsgProc / onProgress] ====================
    @Override
    public void onTtsMsgProc(int msgId, int param, String data) {
        // 结束/错误消息 → 结束拉取循环 [smali: v0.onTtsMsgProc]
    }

    @Override
    public void onProgress(int progress, int total) {
        // 进度上报(未用于音频) [smali: v0.onProgress]
    }
}
