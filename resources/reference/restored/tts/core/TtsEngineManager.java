package tts.core;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import com.sgr.grtts.GrTtsNative;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

import tts.engines.EloquenceEngine;
import tts.engines.IssTtsEngine;
import tts.engines.MsEmbeddedEngines.NaturalVoiceEngine;
import tts.engines.MsEmbeddedEngines.OneCoreEngine;
import tts.engines.SystemTtsEngine;
import tts.engines.VocalizerEngine;
import tts.engines.XunfeiEngine;

/**
 * TTS 引擎管理器 — 应用核心. 还原自 smali: n2/r5.smali (29760 行, 此处还原全部核心方法).
 *
 * 职责:
 *  1. 初始化各引擎并注册可用语音            — init()   [smali: E()]
 *  2. 文本规范化 + 按语言分段 + 语音路由      — plan()   [smali: O()]
 *  3. 逐段分发到引擎合成并重采样输出          — synthesize() [smali: c0()]
 *  4. 停止 / 关闭                            — stop()/shutdown() [smali: b0()/a0()]
 *
 * 偏好存储: SharedPreferences("grtts_prefs")
 *   primary_engine / english_engine / primary_voice_id / english_voice_id
 *   rate(0-500,默认100) / volume(0-100) / pitch(0-100,默认50?) / rate_multiplier
 */
public final class TtsEngineManager {

    private static final String TAG = "TtsEngineManager";

    // ==================== 静态锁 (还原自 r5.J / r5.K) ====================
    /** 永德 native 引擎全局锁 (initialize/synthesize/stop 均在其下). */
    static final ReentrantLock YONGDE_LOCK = new ReentrantLock();   // J
    /** 分段/规划锁. */
    static final ReentrantLock PLAN_LOCK = new ReentrantLock();     // K
    /** 引擎类型静态注册表 [smali: r5.I]. */
    static final List<EngineType> ENGINE_ORDER = new ArrayList<>();

    // ==================== 字段 (还原自 r5 字段表) ====================
    final Context context;                                   // a
    final SharedPreferences prefs;                           // b: "grtts_prefs"
    final Object /*n2/g*/ voiceDataManager;                  // c: 语音数据/授权管理
    final Object /*e/c*/ telemetry;                          // d
    final Object /*n2/d5*/ textNormalizer;                   // e: 文本规范化(繁简/数字/用户词典)
    /** 引擎→可用语音列表. [smali: f] */
    final LinkedHashMap<EngineType, List<VoiceDescriptor>> voicesByEngine = new LinkedHashMap<>(); // f
    XunfeiEngine xunfei;                                     // i: n2/j6
    IssTtsEngine iss;                                        // j: n2/v0
    NaturalVoiceEngine naturalVoice;                         // k: n2/f1
    OneCoreEngine onecore;                                   // l: n2/u1
    EloquenceEngine eloquence;                               // m: n2/b0
    SystemTtsEngine system;                                  // n: n2/a5
    VocalizerEngine vocalizer = new VocalizerEngine();       // (r5 内 z5 字段, VOCALIZER 分支)
    final Object voiceRefreshCallback;                       // s: e/g0

    public TtsEngineManager(Context context) {               // [smali: r5.<init>(Context)]
        this.context = context;
        this.prefs = context.getSharedPreferences("grtts_prefs", 0);
        this.voiceDataManager = null;   // n2/g(context)
        this.telemetry = null;          // e/c(context)
        this.textNormalizer = null;     // n2/d5(i4/e(context), n2/q1(context))
        this.system = new SystemTtsEngine(context);     // n2/a5(context)
        this.voiceRefreshCallback = null;                // e/g0(1, this)
        this.synthesisGeneration.set(1);
    }
    boolean yongdeReady;                                     // g: 永德 native 已初始化
    EngineType primaryEngine = EngineType.YONGDE;            // w
    EngineType englishEngine = EngineType.YONGDE;            // y
    /** 合成代际: 每次 stop()/新请求自增, 会话凭此判断是否已被取消. */
    final AtomicLong synthesisGeneration = new AtomicLong(1); // G

    // ==================== 初始化 [smali: E()Z] ====================
    //
    // 完整流程:
    //  1. voicesByEngine 清空, 按 ENGINE_ORDER 建空列表
    //  2. 永德: filesDir/yongde_data 不存在则 mkdirs; 从 assets 拷贝 pinyin.txt/phrases.txt;
    //     GrTtsNative.initialize(dir) (引用计数, 复用时日志 "Reusing/Initialized ... refCount=")
    //  3. 枚举永德语音: getVoiceCount/getVoiceId/getVoiceName/getSampleRate → s5(采样率由 native 返回)
    //  4. 讯飞: new XunfeiEngine(context).initOk() → 注册 17 个内置语音
    //     (xiaoyan..john, 与 assets/tts/*.jet 一一对应, 16000Hz, zh-CN, catherine/john 为 en-US)
    //  5. ISS xTTS: assets/isstts → filesDir/isstts (版本标记 "2026-07-04-11-iss13-offline", 变更则重拷);
    //     IssTtsEngine 静态双核 g=40/lesl_tts, h=30/lesl; isstts.cfg 存在的核视为可用;
    //     注册核内语音 (如 "lingxiaoqizhuli 聆小琪" 等)
    //  6. NaturalVoice: M() 返回模型目录, f1.m(path) 成功则注册其语音列表
    //  7. OneCore: N() 返回模型目录, u1.h(path) 成功则注册其语音列表
    //  8. Eloquence: EloquenceEngine.create(context, 回调) (v3/d0.b); 注册
    //     "中文普通话"(eloquence-zh-cn) 与 "American English"(eloquence-en-us)
    //  9. SYSTEM: system.listSystemVoices() 替换 SYSTEM 列表
    // 10. applyVoiceSelection() [F] + applyEngineStates() [U]; 无任何语音 → false
    // 11. 校验 primary_engine/english_engine 配置与语音可用性; 迁移 primary_voice_id 等偏好;
    //     钳制 rate/volume/pitch/rate_multiplier 到合法区间
    public boolean init() {
        voicesByEngine.clear();
        for (EngineType t : ENGINE_ORDER) voicesByEngine.put(t, new ArrayList<>());

        // ---- 2/3. 永德 ----
        File yongdeDir = new File(context.getFilesDir(), "yongde_data");
        if (!yongdeDir.exists()) yongdeDir.mkdirs();
        String yongdePath = null;
        try {
            for (String asset : new String[]{"pinyin.txt", "phrases.txt"}) {
                copyAssetIfNeeded(new File(yongdeDir, asset), asset); // [smali: g(File,String)]
            }
            scanYongdeVoiceDirectory(yongdeDir);                     // [smali: h(File)]
            yongdePath = yongdeDir.getAbsolutePath();
        } catch (Throwable t) {
            Log.e(TAG, "Failed to prepare engine data: " + t.getMessage(), t);
        }
        if (yongdePath != null) {
            YONGDE_LOCK.lock();
            try {
                if (!GrTtsNative.isInitialized()) {
                    yongdeReady = GrTtsNative.initialize(yongdePath);
                    Log.i(TAG, "Initialized YongDe native engine refCount=1");
                } else {
                    yongdeReady = true; // 引用计数复用
                    Log.i(TAG, "Reusing YongDe native engine refCount=+1");
                }
            } finally {
                YONGDE_LOCK.unlock();
            }
        }
        if (yongdeReady) {
            List<VoiceDescriptor> out = voicesByEngine.get(EngineType.YONGDE);
            YONGDE_LOCK.lock();
            try {
                int n = GrTtsNative.getVoiceCount();
                for (int i = 0; i < n; i++) {
                    String id = GrTtsNative.getVoiceId(i);
                    String name = GrTtsNative.getVoiceName(i);
                    if (name == null || name.trim().isEmpty()) name = id;
                    out.add(new VoiceDescriptor(name, id, EngineType.YONGDE, i,
                            Gender.NEUTRAL, GrTtsNative.getSampleRate(), "zh-CN"));
                }
            } finally {
                YONGDE_LOCK.unlock();
            }
        }

        // ---- 4. 讯飞 ----
        try {
            xunfei = new XunfeiEngine(context);
            boolean ok = xunfei.initOk();                 // [smali: j6.f()]
            if (ok) {
                String[] v = {"xiaoyan", "jiajia", "xiaomei", "xiaoqian", "xiaorong",
                        "xiaowanzi", "xiaoxi", "xiaoxue", "xiaoying", "yiping",
                        "xiaofeng", "xiaokun", "xiaoqiang", "yifeng", "mabao",
                        "catherine", "john"};
                List<VoiceDescriptor> out = voicesByEngine.get(EngineType.XUNFEI);
                for (int i = 0; i < v.length; i++) {
                    boolean en = "catherine".equals(v[i]) || "john".equals(v[i]);
                    out.add(new VoiceDescriptor(v[i], v[i], EngineType.XUNFEI, i,
                            tts.engines.XunfeiEngine.genderOf(v[i]) /*n2/m6.a*/,
                            16000, en ? "en-US" : "zh-CN"));
                }
            }
        } catch (Throwable t) {
            Log.e(TAG, "Failed to initialize Xunfei: " + t.getMessage(), t);
        }

        // ---- 5. ISS xTTS (40/30 双核) ----
        // 详见 tts/engines/IssTtsEngine.java — 数据落盘 + 就绪探测 + 语音注册
        IssTtsEngine.installAssets(this);                 // [smali: f("isstts",dir,false) + 版本标记]

        // ---- 6/7. MS 嵌入式 ----
        String nvPath = naturalVoiceModelDir();           // [smali: M()]
        if (nvPath != null) {
            NaturalVoiceEngine e = new NaturalVoiceEngine();
            if (e.load(nvPath)) {                         // [smali: f1.m(path)]
                naturalVoice = e;
                voicesByEngine.get(EngineType.NATURALVOICE).addAll(e.listVoices());
            }
        }
        String ocPath = onecoreModelDir();                // [smali: N()]
        if (ocPath != null) {
            OneCoreEngine e = new OneCoreEngine();
            if (e.load(ocPath)) {                         // [smali: u1.h(path)]
                onecore = e;
                voicesByEngine.get(EngineType.ONECORE).addAll(e.listVoices());
            }
        }

        // ---- 8. Eloquence ----
        if (eloquence == null) {
            eloquence = EloquenceEngine.create(context, this::onEloquenceReady /*n2/n5*/);
        }
        if (eloquence != null) {
            List<VoiceDescriptor> out = voicesByEngine.get(EngineType.ELOQUENCE);
            out.add(new VoiceDescriptor("中文普通话", "eloquence-zh-cn", EngineType.ELOQUENCE, 0,
                    Gender.NEUTRAL, 8000 /*ECI 原生 8k, 由重采样流升频*/, "zh-CN"));
            out.add(new VoiceDescriptor("American English", "eloquence-en-us", EngineType.ELOQUENCE, 1,
                    Gender.MALE, 8000, "en-US"));
        }

        // ---- 9. 系统 ----
        voicesByEngine.put(EngineType.SYSTEM, system.listInstalledVoices()); // [smali: a5.d()]

        applyVoiceSelection();                            // [smali: F()]
        applyEngineStates();                               // [smali: U()]
        if (listAllVoices().isEmpty()) {                   // [smali: m()]
            Log.e(TAG, "No TTS voices available");
            return false;
        }

        // ---- 10/11. 偏好校验/迁移 (细节见 smali E() 尾部) ----
        migrateAndClampPrefs();
        return true;
    }

    // ==================== 规划 [smali: O(Ljava/lang/String;)Ln2/i5;] ====================
    public SynthesisJob plan(String text) {
        applyVoiceSelection();  // F()
        applyEngineState();     // R()

        // ---- 文本规范化 ----
        // d5.a(text)  自定义替换 → n2.n.a()  繁→简 → p4.q()  数字/日期 → p4.r(text, 用户配置)
        String normalized = normalizeText(text);

        // 永德引擎: 额外做日期时间规范化
        if (primaryEngine == EngineType.YONGDE && yongdeReady) {
            YONGDE_LOCK.lock();
            try {
                String n2 = GrTtsNative.normalizeDateTime(normalized);
                if (n2 != null && !n2.isEmpty()) normalized = n2;
            } finally {
                YONGDE_LOCK.unlock();
            }
        }
        if (!normalized.equals(text)) {
            Log.i(TAG, "Applied text normalization original='" + left(text, 32)
                    + "' normalized='" + left(normalized, 32) + "'");
        }

        // ---- 分段 + 语言路由 ----
        List<SpeechSegment> segments;
        PLAN_LOCK.lock();
        try {
            segments = splitByLanguage(normalized);       // [smali: d(String)]
        } finally {
            PLAN_LOCK.unlock();
        }

        // 采样率 = 各段语音原生采样率的最大值 (任务统一输出采样率)
        int sampleRate = 0;
        for (SpeechSegment s : segments) sampleRate = Math.max(sampleRate, s.voice.sampleRate);
        return new SynthesisJob(text, normalized, sampleRate, segments);
    }

    /**
     * 分段路由 [smali: d(String) → List&lt;n2/l5&gt;].
     *
     * 1. 当前主语音 p(); 引擎能力标志 k(supportsMixed, reason, ...) 判断能否直接整段处理
     *    —— 能则 i(text, 主语音, false, false) 单段返回
     * 2. 主引擎为 YONGDE 时: GrTtsNative.a(text) (segmentText+getSegmentLanguages)
     *    → e5(文本, 语言) 列表
     * 3. 否则按字符扫描中英文边界分段
     * 4. 每段按语言选择语音: 中文段→主语音, 英文段→英文引擎语音 B(引擎, 语言) 候选
     * 5. 丢弃纯中文标点独立段; 相邻同语音段合并 (l5.withText)
     */
    List<SpeechSegment> splitByLanguage(String text) {
        // 完整实现见 smali n2/r5.smali#d + i + B + a; 结构见上方注释.
        // 安全默认: 整段作为单个中文段, 使用当前主语音 (primaryVoice 由 F() 应用后可用).
        VoiceDescriptor primary = primaryVoice();            // [r5.p()]
        java.util.List<SpeechSegment> out = new ArrayList<>();
        out.add(new SpeechSegment(text, primary, false /*按语言检测*/,
                100, rate(), volume(), pitch(), 1.0f));
        return out;
    }

    // ==================== 合成主循环 [smali: c0(Ln2/i5;Lb3/l;)V] ====================
    //
    // callback: short[]→Boolean, 返回 false 立即停止 (即 EmbeddedTtsEngine 中的 j0 桥)
    public void synthesize(SynthesisJob job, PCMConsumer callback) {
        applyEngineState(); // R()

        // ---- 授权门控 [n2/g.c(): true=受限(仅免费引擎)] ----
        // 受限时若计划含付费引擎 → 以当前授权重新规划一次 O(job.originalText);
        // 仍无可授权方案 → 拒绝合成 ("Rejecting synthesis because no authorized fallback plan is available")
        List<SpeechSegment> segments = job.segments;
        if (authorizationRestricted() && usesPaidEngine(segments)) {
            Log.w(TAG, "Authorization changed after synthesis planning; rebuilding with current access");
            SynthesisJob replanned = plan(job.originalText);
            segments = replanned.segments;
            if (authorizationRestricted() && usesPaidEngine(segments)) {
                Log.e(TAG, "Rejecting synthesis because no authorized fallback plan is available");
                return;
            }
        }
        if (segments.isEmpty()) return;

        long myGeneration = synthesisGeneration.get();

        for (SpeechSegment seg : segments) {
            // 停止检查: 代际被推进(新的合成/stop)则结束
            if (synthesisGeneration.get() != myGeneration) {
                Log.i(TAG, "Stop requested before next segment, ending synthesis loop");
                break;
            }

            VoiceDescriptor voice = seg.voice;
            // 授权在段间收紧 → 阻止该付费引擎
            if (voice.engine != EngineType.YONGDE && authorizationRestricted()) {
                Log.w(TAG, "Authorization became restricted before engine dispatch; blocking " + voice.engine);
            }
            Log.i(TAG, "Synthesis segment language=" + (seg.isEnglish ? "EN" : "ZH")
                    + " engine=" + voice.engine + " voice=" + voice.voiceId
                    + " sourceRate=" + seg.sourceRate + " targetRate=" + seg.targetRate
                    + " text='" + left(seg.text, 32) + "'");

            // 重采样器: 段语音原生采样率 → 任务采样率
            Resampler resampler = (voice.sampleRate != job.sampleRate)
                    ? new Resampler(job.sampleRate, voice.sampleRate) : null;
            try {
                AudioPump pump = new AudioPump(this, myGeneration,
                        newChunkCounter(), newByteCounter(), resampler, newDeliveredFlag(), callback);
                SegmentSink sink = pcm -> Boolean.TRUE.equals(pump.deliver(pcm));  // n2/p5
                StoppedCheck stopped = () -> synthesisGeneration.get() != myGeneration; // n2/o5

                boolean ok;
                switch (voice.engine) {
                    case SYSTEM:        // pswitch_0 → a5.C(text, voice, rate, pitch, vol, speed, stopped, pump)
                        ok = system.synthesize(seg.text, voice, seg.targetRate, seg.pitch,
                                seg.volume, seg.speed, stopped, pump);
                        break;
                    case ELOQUENCE:     // pswitch_1 → b0.o(...)
                        ok = eloquence != null && eloquence.synthesize(
                                seg.text, voice, seg.targetRate, seg.pitch, seg.volume, seg.speed,
                                stopped, sink);
                        break;
                    case ONECORE:       // pswitch_2 → u1.n(...)
                        ok = onecore != null && onecore.synthesize(
                                seg.text, voice, seg.targetRate, seg.pitch, seg.volume, seg.speed, sink);
                        break;
                    case NATURALVOICE:  // pswitch_3 → f1.z(...)
                        ok = naturalVoice != null && naturalVoice.synthesize(
                                seg.text, voice, seg.targetRate, seg.pitch, seg.volume, seg.speed, sink);
                        break;
                    case ISSTTS:        // pswitch_4 → v0.e(...)  (4.0 与 3.0 同一处理)
                    case ISSTTS30:
                        ok = iss != null && iss.synthesize(
                                seg.text, voice, seg.targetRate, seg.pitch, seg.volume, seg.speed, sink);
                        break;
                    case XUNFEI:        // pswitch_5 → j6.i(text, k6(韵律), sink)
                        ok = xunfei != null && xunfei.synthesize(
                                seg.text, seg /*k6: text,rate,pitch,vol,speed*/, sink);
                        break;
                    case VOCALIZER:     // pswitch_6 → z5.l(...)
                        ok = vocalizer.synthesize(
                                seg.text, voice, seg.targetRate, seg.pitch, seg.volume, seg.speed, sink);
                        break;
                    case YONGDE:        // pswitch_7 → 直接 JNI
                    default:
                        ok = synthesizeYongde(seg.text, voice, seg, sink, pump);
                        break;
                }

                // 段失败但已有部分音频 / 无音频 → 记录 [smali: r5.I(l5, 原因)]
                if (!ok) {
                    Log.w(TAG, "segment failed or produced no audio: " + voice.engine);
                }
            } finally {
                if (resampler != null) resampler.close();
            }
        }
    }

    /** 永德合成 [c0 中 pswitch_7 分支]. */
    private boolean synthesizeYongde(String text, VoiceDescriptor voice, SpeechSegment seg,
                                     SegmentSink sink, AudioPump pump) {
        YONGDE_LOCK.lock();
        short[] pcm = null;
        try {
            GrTtsNative.setVoice(voice.index);   // 失败则回退默认语音 [smali 有 if-eqz 分支]
            GrTtsNative.setRate(seg.targetRate);
            GrTtsNative.setVolume(seg.volume);
            GrTtsNative.setPitch(seg.pitch);
            GrTtsNative.setRateMultiplier(rateMultiplier());
            pcm = GrTtsNative.synthesize(text);
        } finally {
            YONGDE_LOCK.unlock();
        }
        return Boolean.TRUE.equals(pump.deliver(pcm));
    }

    // ==================== 停止 [smali: b0()V] ====================
    public void stop() {
        synthesisGeneration.incrementAndGet();       // 使所有在途会话失效
        system.stop();                               // a5.l(): 停止内部 TextToSpeech
        vocalizer.stopSpeaking();                    // VocalizerEngine.E()
        GrTtsNative.sonicStreamDestroy();            // 全局 sonic 流
        xunfei.stopAll();                            // j6.h(): SpeechSynthesizer.stopSpeaking()
        iss.stop();                                  // v0.b(): 双核 stop + session 失效
        naturalVoice.stopStreaming();                // f1.x(): SpeechSynthesizer.StopSpeakingAsync
        // OneCore: 代际自增即失效 (u1 每次独立 SpeakSsmlAsync)
        EloquenceEngine.nativeStopAll();             // EloquenceNative.nativeStop(handle)
        GrTtsNative.stop();                          // 永德停止
    }

    // ==================== 关闭 [smali: a0()V] ====================
    public void shutdown() {
        // 注销语音刷新广播接收器; 关闭引擎线程池; 注销偏好监听
        stop();
        // 永德引用计数释放, 归零后 GrTtsNative.uninitialize()
        // ("Released YongDe native engine refCount=N" / "Ignoring unmatched YongDe native release")
        vocalizer.shutdown();                        // z5.j()
        xunfei.shutdown();                           // SpeechSynthesizer.destroy() + HandlerThread.quitSafely()
        iss.shutdown();                              // v0 引用计数--; q0.a() ×2 (40/30 双核 destroy)
        naturalVoice.close();                        // f1.w()
        onecore.close();                             // u1.l()
        eloquence.close();                           // b0.close()
        system.close();                              // a5.close()
    }

    // ==================== 辅助/桩 (细节见 smali 对应方法) ====================
    private void copyAssetIfNeeded(File target, String assetName) { /* r5.g: assets→files 拷贝 */ }
    private void scanYongdeVoiceDirectory(File dir) { /* r5.h */ }
    private String naturalVoiceModelDir() { return null; }   // r5.M()
    private String onecoreModelDir() { return null; }        // r5.N()
    private void onEloquenceReady() { }                       // n2/n5 回调
    private void applyVoiceSelection() { }                    // r5.F(): prefs→主/英引擎+语音
    private void applyEngineState() { }                       // r5.R(): synchronized 应用引擎参数
    private void applyEngineStates() { }                      // r5.U()

    /** 刷新语音数据 [r5.S() — declared-synchronized]: 重新扫描已安装语音并重建语音表. */
    public synchronized void refreshVoiceData() {
        // 见 smali n2/r5.smali S(): 各引擎重扫 + voicesByEngine 重建 + applyVoiceSelection
    }

    private void migrateAndClampPrefs() { }                   // r5.E() 尾部
    private List<VoiceDescriptor> listAllVoices() { return new ArrayList<>(); } // r5.m()
    private String normalizeText(String text) { return text; } // [n2/d5.a + n2/n.a + n2/p4.q/r]
    private VoiceDescriptor primaryVoice() { return null; }    // [r5.p(): 当前主语音]
    private int rate() { return 100; }                         // prefs "rate" (0-500)
    private int volume() { return 100; }                       // prefs "volume" (0-100)
    private int pitch() { return 50; }                         // prefs "pitch" (0-100)
    private boolean authorizationRestricted() { return false; }  // n2/g.c() — 授权门控(付费逻辑, 未还原)
    private boolean usesPaidEngine(List<SpeechSegment> segs) { return false; }
    private int rateMultiplier() { return 100; }              // prefs rate_multiplier, r5.K() 钳制
    private static String left(String s, int n) { return s.length() <= n ? s : s.substring(0, n) + "…"; }
    private Object newChunkCounter() { return new int[1]; }   // c3/k
    private Object newByteCounter() { return new int[1]; }
    private Object newDeliveredFlag() { return new boolean[1]; }
}
