package com.sgr.grtts;

import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.speech.tts.SynthesisCallback;
import android.speech.tts.SynthesisRequest;
import android.speech.tts.TextToSpeechService;
import android.speech.tts.Voice;
import android.util.Log;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

import tts.core.PCMConsumer;
import tts.core.SynthesisJob;
import tts.core.TtsEngineManager;

/**
 * Android TTS 服务入口. 完整还原自 smali: com/sgr/grtts/EmbeddedTtsEngine.smali (1850 行).
 *
 * 注册 (AndroidManifest.xml):
 *   service android.intent.action.TTS_SERVICE, meta-data android.speech.tts → @xml/tts_engine
 * 配套组件: CheckVoiceData (CHECK_TTS_DATA), GetSampleText (GET_SAMPLE_TEXT), SettingsActivity
 */
public final class EmbeddedTtsEngine extends TextToSpeechService {

    private static final String TAG = "EmbeddedTtsEngine";
    private static final int SAMPLE_RATE = 16000;  // 0x3e80
    private static final int AUDIO_FORMAT = 2;     // ENCODING_PCM_16BIT

    // ==================== 字段 ====================
    private final Object initLock = new Object();
    private final AtomicBoolean shutdownRequested = new AtomicBoolean(false);
    private final AtomicBoolean shutdownDone = new AtomicBoolean(false);
    private String[] currentLanguage = {Locale.SIMPLIFIED_CHINESE.getLanguage(),
            Locale.SIMPLIFIED_CHINESE.getCountry(), ""};
    private final AtomicLong synthesisGeneration = new AtomicLong(0);
    private final AtomicLong activeSynthesisGeneration = new AtomicLong(0);
    private final AtomicBoolean pendingVoiceDataRefresh = new AtomicBoolean(false);
    private final AtomicBoolean voiceDataRefreshInFlight = new AtomicBoolean(false);
    private TtsEngineManager engineManager;
    private volatile boolean initialized;
    private volatile boolean initFailed;
    /** 语音数据刷新广播 [n2/k0]: SettingsActivity 安装语音后通知服务. */
    private final BroadcastReceiver voiceDataRefreshReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            scheduleVoiceDataRefresh();
        }
    };

    // ==================== 生命周期 ====================
    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "EmbeddedTtsEngine onCreate");

        // 防篡改自检 [i4/h.M(Context)]: 包身份被修改 → 提示并拒绝启动
        // (反编译重打包会触发: "检测到应用程序被修改，TTS服务已停止运行")
        if (tamperDetected()) {
            Log.e(TAG, "Detected modified app identity, stopping TTS service");
            showTamperingMessage();
            stopSelf();
            return;
        }

        engineManager = new TtsEngineManager(this);
        registerVoiceDataRefreshReceiver();

        // 后台线程 "grtts_init" 执行 engineManager.init() [n2/i0 case 0]
        boolean ok = engineManager.init();
        synchronized (initLock) {
            initialized = ok;
            initFailed = !ok;
            initLock.notifyAll();
        }
        if (ok) {
            Log.i(TAG, "TTS engine initialized successfully");
            if (pendingVoiceDataRefresh.get()) scheduleVoiceDataRefresh();
        } else {
            Log.e(TAG, "Failed to initialize TTS engine");
        }
        // 若关闭先于初始化完成 → 立即关闭
        if (shutdownRequested.get() && shutdownDone.compareAndSet(false, true)) {
            engineManager.shutdown();
        }
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "EmbeddedTtsEngine onDestroy");
        unregisterReceiverSafely(voiceDataRefreshReceiver);
        requestShutdown();
        super.onDestroy();
    }

    private void requestShutdown() {
        shutdownRequested.set(true);
        if (initialized || initFailed) {
            if (shutdownDone.compareAndSet(false, true)) engineManager.shutdown();
        }
    }

    // ==================== 语言/语音查询 ====================

    /** 只支持 zh-CN / en-US, 其余 LANG_NOT_SUPPORTED(-2) 或 LANG_AVAILABLE_DATA_MISSING(0).
     *  返回值即 android.speech.tts.TextToSpeech 的常量: 0=LANG_AVAILABLE, 1=LANG_COUNTRY_AVAILABLE, -2=LANG_NOT_SUPPORTED. */
    @Override
    public int onIsLanguageAvailable(String language, String country, String variant) {
        boolean zh = equalsAny(language, Locale.CHINESE.getLanguage(), "zh",
                Locale.SIMPLIFIED_CHINESE.getISO3Language(), "zho");
        if (zh) {
            return equalsAny(country, Locale.SIMPLIFIED_CHINESE.getISO3Country(), "CHN", "CN")
                    ? 1 /*LANG_COUNTRY_AVAILABLE*/ : 0 /*LANG_AVAILABLE_DATA_MISSING*/;
        }
        boolean en = equalsAny(language, Locale.ENGLISH.getLanguage(), "en",
                Locale.US.getISO3Language(), "eng");
        if (en) {
            return equalsAny(country, Locale.US.getISO3Country(), "USA", "US")
                    ? 1 /*LANG_COUNTRY_AVAILABLE*/ : 0 /*LANG_AVAILABLE_DATA_MISSING*/;
        }
        return -2; // LANG_NOT_SUPPORTED
    }

    @Override
    public String[] onGetLanguage() {
        return currentLanguage;
    }

    @Override
    public int onLoadLanguage(String language, String country, String variant) {
        int result = onIsLanguageAvailable(language, country, variant);
        if (result == 0 || result == 1 || result == 2 /*LANG_COUNTRY_VAR_AVAILABLE*/) {
            if (language == null) language = Locale.SIMPLIFIED_CHINESE.getLanguage();
            if (country == null || country.trim().isEmpty()) {
                boolean en = equalsAny(language, "en", Locale.US.getISO3Language(), "eng");
                country = en ? Locale.US.getCountry() : Locale.SIMPLIFIED_CHINESE.getCountry();
            }
            currentLanguage = new String[]{language, country, variant == null ? "" : variant};
        }
        return result;
    }

    /** 语音列表 = engineManager 注册的语音, 附加特征 "com.sgr.grtts.voice". */
    @Override
    public List<Voice> onGetVoices() {
        List<Voice> out = new ArrayList<>();
        // [smali: 遍历 engineManager 语音 → new Voice(name, locale, QUALITY, LATENCY, false, features)]
        // features 始终包含 "com.sgr.grtts.voice" (SYSTEM 引擎据此排除本应用语音)
        return out;
    }

    // ==================== 合成入口 ====================

    /**
     * 核心: onSynthesizeText(request, callback) [smali 完整还原].
     *
     * 1. 请求参数含 "com.sgr.grtts.system_tts_origin" → 递归系统请求, 直接 error()
     * 2. 文本 = getCharSequenceText() ?: getText() ?: ""; 空 → start(16000,16bit,1) + done()
     * 3. awaitInitialized(5000ms) 失败 → error()
     * 4. 代际登记; job = engineManager.plan(text)
     * 5. callback.start(job.sampleRate, ENCODING_PCM_16BIT, 1)
     * 6. engineManager.synthesize(job, j0 桥) — 桥: short[]→byte[] 小端,
     *    按 getMaxBufferSize 分块 audioAvailable, 返回代际是否仍有效
     * 7. 正常完成 → callback.done(); 异常 → 代际仍有效时 callback.error()
     */
    @Override
    public void onSynthesizeText(SynthesisRequest request, SynthesisCallback callback) {
        if (request.getParams().containsKey("com.sgr.grtts.system_tts_origin")) {
            Log.w(TAG, "Rejecting recursive system TTS request");
            callback.error();
            return;
        }

        String text = request.getCharSequenceText() != null
                ? request.getCharSequenceText().toString() : request.getText();
        if (text == null) text = "";
        if (text.length() == 0) {
            callback.start(16000, AUDIO_FORMAT, 1);
            callback.done();
            return;
        }

        if (!awaitInitialized(5000)) {
            Log.e(TAG, "Engine not initialized");
            callback.error();
            return;
        }

        long generation = synthesisGeneration.incrementAndGet();
        activeSynthesisGeneration.set(generation);

        SynthesisJob job = engineManager.plan(text);
        Log.i(TAG, "Starting synthesis generation=" + generation
                + " sampleRate=" + job.sampleRate
                + " maxBufferSize=" + callback.getMaxBufferSize()
                + " textLength=" + text.length());

        callback.start(job.sampleRate, AUDIO_FORMAT, 1);

        final int[] chunks = {0};
        final int[] totalBytes = {0};
        try {
            PCMConsumer bridge = pcm -> {
                if (activeSynthesisGeneration.get() != generation) return Boolean.FALSE;
                if (pcm == null || pcm.length == 0) return Boolean.TRUE;
                byte[] bytes = shortArrayToByteArray(pcm);
                chunks[0]++;
                totalBytes[0] += bytes.length;
                if (chunks[0] == 1) {
                    Log.i(TAG, "First callback PCM generation=" + generation
                            + " samples=" + pcm.length + " bytes=" + bytes.length);
                }
                int max = callback.getMaxBufferSize();
                for (int off = 0; off < bytes.length; ) {
                    if (activeSynthesisGeneration.get() != generation) return Boolean.FALSE;
                    int len = Math.min(max, bytes.length - off);
                    callback.audioAvailable(bytes, off, len);
                    off += len;
                }
                return activeSynthesisGeneration.get() == generation;
            };
            engineManager.synthesize(job, bridge);

            if (isGenerationCurrent(generation)) {
                Log.i(TAG, "Finishing synthesis generation=" + generation
                        + " chunks=" + chunks[0] + " totalBytes=" + totalBytes[0]);
                callback.done();
            }
        } catch (Exception e) {
            Log.e(TAG, "Synthesis error: " + e.getMessage(), e);
            if (isGenerationCurrent(generation)) callback.error();
        } finally {
            clearActiveGeneration(generation);
        }
    }

    @Override
    public void onStop() {
        long generation = synthesisGeneration.incrementAndGet();
        activeSynthesisGeneration.set(generation);
        Log.d(TAG, "onStop called generation=" + generation);
        engineManager.stop();
    }

    // ==================== 辅助 ====================

    private boolean awaitInitialized(long timeoutMs) {
        if (initialized) return true;
        if (initFailed) return false;
        long deadline = System.currentTimeMillis() + timeoutMs;
        synchronized (initLock) {
            while (!initialized && !initFailed && System.currentTimeMillis() < deadline) {
                try {
                    initLock.wait(deadline - System.currentTimeMillis());
                } catch (InterruptedException e) {
                    return false;
                }
            }
        }
        return initialized;
    }

    private boolean isGenerationCurrent(long generation) {
        return activeSynthesisGeneration.get() == generation;
    }

    private void clearActiveGeneration(long generation) {
        activeSynthesisGeneration.compareAndSet(generation, 0L);
    }

    /** short[] → 小端 byte[] [smali shortArrayToByteArray]. */
    static byte[] shortArrayToByteArray(short[] pcm) {
        byte[] out = new byte[pcm.length * 2];
        for (int i = 0; i < pcm.length; i++) {
            out[2 * i] = (byte) (pcm[i] & 0xFF);
            out[2 * i + 1] = (byte) ((pcm[i] >> 8) & 0xFF);
        }
        return out;
    }

    private void registerVoiceDataRefreshReceiver() {
        IntentFilter filter = new IntentFilter("com.sgr.grtts.action.REFRESH_VOICE_DATA");
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(voiceDataRefreshReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(voiceDataRefreshReceiver, filter);
        }
    }

    /** 语音数据刷新: 线程 "grtts_voice_refresh" → 等初始化完 → engineManager.refreshVoiceData() [r5.S()]. */
    private void scheduleVoiceDataRefresh() {
        pendingVoiceDataRefresh.set(true);
        if (!voiceDataRefreshInFlight.compareAndSet(false, true)) return;
        // 单线程执行: awaitInitialized → pendingVoiceDataRefresh CAS(true→false)
        // → Log "Refreshing installed voice data in active TTS service" → engineManager.S()
        voiceDataRefreshInFlight.set(false);
    }

    private void unregisterReceiverSafely(BroadcastReceiver receiver) {
        try {
            unregisterReceiver(receiver);
        } catch (Throwable ignore) {
        }
    }

    private boolean tamperDetected() {
        return false; // [i4/h.M(Context)] — 防篡改校验, 未还原 (重打包会触发)
    }

    private void showTamperingMessage() {
        new Handler(Looper.getMainLooper()).post(() ->
                Toast.makeText(getApplicationContext(), "检测到应用程序被修改，TTS服务已停止运行",
                        Toast.LENGTH_LONG).show());
    }

    private static boolean equalsAny(Object v, Object... candidates) {
        for (Object c : candidates) if (v != null && v.equals(c)) return true;
        return false;
    }
}
