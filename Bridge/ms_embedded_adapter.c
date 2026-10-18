/* Microsoft 嵌入式语音适配器 (naturalvoice + onecore 共用).
 *
 * 对应 Windows 版 (原版 Windows 实现) 与 Android 版
 * (resources/reference/restored/MsEmbeddedEngines.java) 的 EmbeddedSpeechConfig.fromPath 模式.
 *
 * ★ 2026-10-17 实测打通的关键事实 (详见文件底部注释):
 *   1. SPX*HANDLE 为指针尺寸 (按 32 位截断会 SIGBUS);
 *   2. 语音名 = Tokens.xml 里的完整显示名 ("Microsoft Xiaoxiao (Natural HD) - ..."),
 *      短名 zh-CN-XiaoxiaoNeural 会报 VOICENAME_NOTEXIST;
 *   3. 必须调 set_speech_synthesis_voice(cfg, 完整名, 凭证):
 *      license 包 = SynthModel.license 全文, 不带 → WRONG_DECRYPTION_KEY;
 *      .key 包 = "Key:"+SynthModel.key 内容 (Windows 逆向 2026-10-19, 见
 *      docs/NOTES-win-re-ms-voices.md; 之前裸传 key 内容 → reason=1);
 *   4. 必须设输出格式属性 (id 3102, name 传 NULL) = "raw-24khz-16bit-mono-pcm",
 *      否则默认 16kHz → AUDIO_FORMAT_NOT_SUPPORTED (模型仅 24k);
 *   5. .key 的 Natural 包: Windows 侧以 "Key:"+90字符 传入可用 (全部 7 包共用
 *      同一把 key); 本适配器已改为同样传法, 待 macOS 实测;
 *   6. 音频只走 AudioConfig 目的地 (result/事件/pull 流均 0 字节, 实测);
 *      → 流式 = start_speaking_ssml 立即返回 + 跟随 wav 文件增长读回,
 *        completed/canceled 回调 (arg1=synth 句柄, 不传 ctx!) 作结束信号.
 *      实测: 首块音频 ~0.75s 到达, 之后 ~1.7× 实时速率持续写入.
 *   7. ★ 嵌入式 SDK 的 prosody rate 封顶 200% (2026-10-18 实测: r250/300/
 *      400 与 r200 输出逐字节相同) → 桥 rate 20..500 = 引擎 min(rate,200)
 *      原生跑 + 超出部分 Sonic 流式补速 (rate/200, 上限 2.5×), read 路径内
 *      逐块过 Sonic, 不破坏流式;
 *      SDK 写的 "wav" 实为无头裸 PCM (size = 样本×2), 直接喂 Sonic 无需去头.
 *
 * 布局: Engines/<key>/
 *   libMicrosoft.CognitiveServices.Speech.core.dylib (+embedded.tts/+onnxruntime)
 *   model/<包>/            ← 语音包, 含 SynthModel.license 或 .key 的包 = 一个发音人
 *   voice.txt              ← 可选: 完整语音名, 命中则该包为默认发音人
 */
#include "adapters.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sonic.h"

/* ★ 句柄为指针尺寸 (实测) */
typedef void *ms_handle;

static double now_ms_f(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef int   (*fn_cfg_create)(ms_handle *);
typedef int   (*fn_cfg_add_path)(ms_handle, const char *);
typedef int   (*fn_cfg_set_voice)(ms_handle, const char *, const char *);
typedef int   (*fn_cfg_propbag)(ms_handle, ms_handle *);
typedef int   (*fn_bag_set_string)(ms_handle, int, const char *, const char *);
typedef int   (*fn_acfg_wavfile)(ms_handle *, const char *);
typedef int   (*fn_synth_create)(ms_handle *, ms_handle, ms_handle);
typedef int   (*fn_start_speak)(ms_handle, const char *, unsigned, ms_handle *);
typedef int   (*fn_speak_ssml)(ms_handle, const char *, unsigned, ms_handle *);
typedef int   (*fn_get_reason)(ms_handle, int *);
typedef int   (*fn_stop)(ms_handle);
typedef void  (*fn_event_cb)(ms_handle, void (*)(ms_handle, ms_handle), void *);
typedef int   (*fn_release_result)(ms_handle);
typedef int   (*fn_release_synth)(ms_handle);

/* 回调 ABI (实测): cb(arg1=synth句柄, arg2=事件句柄), set_callback 的 ctx 不传 */

#define MS_MAX_PKG 16

/* 语音包 (一个发音人) */
typedef struct {
    char dir[256];        /* 包目录名 (发音人选择键) */
    char voice[256];      /* Tokens.xml 完整语音名 */
    char vdir[256];       /* data/ 下的模型叶子目录名 (如 zh-CN-XiaoxiaoNaturalHD) */
    char credpath[1024];  /* SynthModel.license 或 SynthModel.key 路径 */
    int keypkg;           /* 凭证为 .key: 传入 SDK 时需 "Key:" 前缀 */
    void *sess;           /* 懒建的合成会话 */
} ms_pkg;

/* 每发音人合成会话 */
typedef struct ms_ctx ms_ctx;
typedef struct ms_vsession {
    ms_pkg *pkg;
    ms_ctx *owner;
    ms_handle cfg, synth;
    char wavpath[768];
    fn_stop stop;              /* dl 绑定的 stop_speaking (cancel 用) */
    int warmed;                /* 预热已跑/在跑 */
    int got_real;               /* 真实请求已到过: 预热线程让位 (launch 竞争仲裁) */
    struct ms_stream *grave;   /* 待回收流 (回调可能仍持指针, 延迟到下次 start 销毁) */
    pthread_mutex_t lock;      /* 保护 active + 串行化该会话的 SDK 调用 */
    struct ms_stream *active;  /* 当前流 (回调目标) */
    struct timespec settle_until;  /* stop 后的安全重启时刻 (CLOCK_MONOTONIC) */
    int warming;               /* 后台预热进行中: stream_start 等待而非 stop */
    pthread_cond_t warm_cond;  /* 预热完成通知 */
} ms_vsession;

struct ms_ctx {
    void *dl;
    fn_cfg_create       cfg_create;
    fn_cfg_add_path     cfg_add_path;
    fn_cfg_set_voice    cfg_set_voice;
    fn_cfg_propbag      cfg_propbag;
    fn_bag_set_string   bag_set_string;
    fn_acfg_wavfile     acfg_wavfile;
    fn_synth_create     synth_create;
    fn_start_speak      start_speak;
    fn_speak_ssml       speak_ssml;
    fn_get_reason       get_reason;
    fn_stop             stop;
    fn_event_cb         completed_set_cb;
    fn_event_cb         canceled_set_cb;
    fn_release_result   release_result;
    fn_release_synth    release_synth;
    ms_pkg pkgs[MS_MAX_PKG];
    int npkg;
    char key[32];
    char ename[64];            /* 引擎显示名 (发音人名前缀) */
    char edir[1024];           /* Engines/<key> 绝对路径 */
    char vnames[MS_MAX_PKG][128];  /* 发音人显示名 */
    char remembered[320];      /* 已记入 .lastvoice 的 "engine voice" (去重写盘) */
};

/* 流: 跟随 SDK 写的 wav 文件增长; rate>200% 时逐块过 Sonic 补速 */
#define MS_ENGINE_RATE_MAX 200   /* 嵌入式 SDK prosody 封顶 (实测, 见头部注释 7) */
#define MS_RAW_CHUNK 8192        /* 单轮文件读取的原始样本数 */
struct ms_stream {
    ms_vsession *vs;
    struct ms_stream *next;    /* 墓园链 */
    int fd;
    off_t off;                 /* 已读字节偏移 */
    pthread_mutex_t lock;
    int completed;             /* completed 回调已到 */
    int canceled;              /* cancel 或 canceled 回调 */
    int stable;                /* 完成后文件尺寸已稳定 */
    off_t stable_size;
    int stable_rounds;
    struct timespec last_growth;
    sonicStream so;            /* 超出引擎上限部分的补速 (NULL = 不需要) */
    int flushed;               /* 终局时已 sonicFlushStream */
    int16_t raw[MS_RAW_CHUNK]; /* 喂 Sonic 的原始样本块 */
};

/* ---- synth 句柄 → 会话 注册表 (回调不传 ctx, 按 arg1 路由) ---- */
#define MS_REG_MAX 24
static struct { ms_handle synth; ms_vsession *vs; } g_reg[MS_REG_MAX];
static int g_reg_n = 0;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static void ms_reg_add(ms_handle synth, ms_vsession *vs) {
    pthread_mutex_lock(&g_reg_lock);
    if (g_reg_n < MS_REG_MAX) { g_reg[g_reg_n].synth = synth; g_reg[g_reg_n].vs = vs; g_reg_n++; }
    pthread_mutex_unlock(&g_reg_lock);
}

static ms_vsession *ms_reg_find(ms_handle synth) {
    ms_vsession *vs = NULL;
    pthread_mutex_lock(&g_reg_lock);
    for (int i = 0; i < g_reg_n; i++)
        if (g_reg[i].synth == synth) { vs = g_reg[i].vs; break; }
    pthread_mutex_unlock(&g_reg_lock);
    return vs;
}

static int ms_bind(void *dl, void **slot, const char *name) {
    *slot = dlsym(dl, name);
    if (!*slot) fprintf(stderr, "[MagicTTS][ms] 缺符号 %s\n", name);
    return *slot != NULL;
}

/* 读整个文件到 malloc 缓冲 (去尾部换行) */
static char *ms_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    return buf;
}

static int ms_dir_nonempty(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') { found = 1; break; }
    }
    closedir(d);
    return found;
}

static int ms_probe(const char *root, const mtts_engine_desc *desc) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", root, desc->key, desc->dylib_name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    snprintf(path, sizeof(path), "%s/%s/model", root, desc->key);
    return ms_dir_nonempty(path);
}

/* 包目录名 → 中文短名: NeuralVoices-voice-Xiaoxiao-NaturalHD-... → 晓晓 HD */
static const struct { const char *en, *zh; } kVoiceZh[] = {
    {"Xiaoxiao", "晓晓"}, {"Xiaoyi", "晓伊"}, {"Xiaohan", "晓涵"},
    {"Xiaomeng", "晓梦"}, {"Xiaoqiu", "晓秋"}, {"Xiaorui", "晓睿"},
    {"Xiaoshuang", "晓双"}, {"Xiaoxuan", "晓萱"}, {"Xiaoyan", "晓颜"},
    {"Xiaoyou", "晓悠"}, {"Xiaozhen", "晓珍"}, {"Xiaochen", "晓辰"},
    {"HsiaoChen", "晓臻"}, {"HsiaoYu", "晓雨"},
    {"Yunxi", "云希"}, {"Yunyang", "云扬"}, {"Yunjian", "云健"},
    {"Yunfeng", "云丰"}, {"Yunhao", "云皓"}, {"Yunye", "云野"},
    {"YunJhe", "云哲"},
};

/* 包目录名尾段 → BCP-47 (英文包装进中文引擎槽位时, 系统语音列表的语言
 * 分类必须正确; 未知尾段返回空, 由 router 回退引擎级 locale) */
static void pkg_locale(const char *dir, char *out, size_t cap) {
    static const struct { const char *seg, *loc; } kMap[] = {
        {"Chinese-Simplified-China", "zh-CN"},
        {"Chinese-China",            "zh-CN"},
        {"Chinese-Taiwan",           "zh-TW"},
        {"English-UnitedStates",     "en-US"},
        {"English-UnitedKingdom",    "en-GB"},
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++)
        if (strstr(dir, kMap[i].seg)) {
            snprintf(out, cap, "%s", kMap[i].loc);
            return;
        }
}

static void pkg_display_name(const char *dir, const char *engine_name,
                             char *out, size_t cap) {
    char tok[64] = "";
    const char *v = strstr(dir, "voice-");
    if (v) {
        v += 6;
        const char *e = strchr(v, '-');
        size_t n = e ? (size_t)(e - v) : strlen(v);
        if (n >= sizeof(tok)) n = sizeof(tok) - 1;
        memcpy(tok, v, n); tok[n] = 0;
    }
    const char *zh = NULL;
    for (size_t i = 0; i < sizeof(kVoiceZh) / sizeof(kVoiceZh[0]); i++)
        if (strcmp(kVoiceZh[i].en, tok) == 0) { zh = kVoiceZh[i].zh; break; }
    snprintf(out, cap, "%s·%s%s", engine_name, zh ? zh : (tok[0] ? tok : dir),
             strstr(dir, "NaturalHD") ? " HD" : "");
}

/* 包目录 → Tokens.xml 提取完整语音名 (第一个 <String name="" value="..."/>);
 * vdir 输出 Tokens.xml 所在的 data/ 叶子目录名 (Windows 侧 add_path 传的就是它) */
static int pkg_voice_fullname(const char *modeldir, const char *dir,
                              char *voice, size_t vsz,
                              char *vdir, size_t dsz) {
    char dd[1280];
    snprintf(dd, sizeof(dd), "%s/%s/data", modeldir, dir);
    DIR *d = opendir(dd);
    if (!d) return -1;
    struct dirent *e;
    char tokens[1536] = {0};
    char leaf[256] = {0};
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(tokens, sizeof(tokens), "%s/%s/Tokens.xml", dd, e->d_name);
        if (access(tokens, R_OK) == 0) { snprintf(leaf, sizeof(leaf), "%s", e->d_name); break; }
        tokens[0] = 0;
    }
    closedir(d);
    if (!tokens[0]) return -1;
    char *xml = ms_read_file(tokens, NULL);
    if (!xml) return -1;
    char *v = strstr(xml, "<String name=\"\" value=\"");
    if (!v) { free(xml); return -1; }
    v += strlen("<String name=\"\" value=\"");
    char *end = strchr(v, '"');
    if (!end || (size_t)(end - v) >= vsz) { free(xml); return -1; }
    memcpy(voice, v, end - v);
    voice[end - v] = 0;
    free(xml);
    if (vdir) snprintf(vdir, dsz, "%s", leaf);
    return 0;
}

/* model/ 下 .license 包扫描 (create 与 scan_voices 共用; Tokens.xml 为小文件,
 * 纯文件系统毫秒级). pkgs 可为 NULL (仅枚举发音人时). 返回包数. */
static int ms_scan_pkgs(const char *edir, const char *ename,
                        ms_pkg *pkgs, int pkgs_max,
                        mtts_voice_scan *out, int out_max) {
    char modeldir[1024];
    snprintf(modeldir, sizeof(modeldir), "%s/model", edir);
    DIR *d = opendir(modeldir);
    if (!d) return 0;
    struct dirent *e;
    const int limit = pkgs ? pkgs_max : out_max;
    int n = 0;
    while ((e = readdir(d)) != NULL && n < limit) {
        if (e->d_name[0] == '.') continue;
        char p[1280], pkey[1280];
        snprintf(p, sizeof(p), "%s/%s/SynthModel.license", modeldir, e->d_name);
        snprintf(pkey, sizeof(pkey), "%s/%s/SynthModel.key", modeldir, e->d_name);
        int keypkg = 0;
        if (access(p, R_OK) == 0) keypkg = 0;
        else if (access(pkey, R_OK) == 0) { keypkg = 1; snprintf(p, sizeof(p), "%s", pkey); }
        else continue;   /* 两种凭证都没有: 非发音人目录 */
        char voice[256], vdir[256];
        if (pkg_voice_fullname(modeldir, e->d_name, voice, sizeof(voice),
                               vdir, sizeof(vdir)) != 0) {
            fprintf(stderr, "[MagicTTS][ms] %s: 提取语音名失败, 跳过\n", e->d_name);
            continue;
        }
        if (pkgs) {
            snprintf(pkgs[n].dir, sizeof(pkgs[n].dir), "%s", e->d_name);
            snprintf(pkgs[n].credpath, sizeof(pkgs[n].credpath), "%s", p);
            snprintf(pkgs[n].voice, sizeof(pkgs[n].voice), "%s", voice);
            snprintf(pkgs[n].vdir, sizeof(pkgs[n].vdir), "%s", vdir);
            pkgs[n].keypkg = keypkg;
            pkgs[n].sess = NULL;
        }
        snprintf(out[n].key, sizeof(out[n].key), "%s", e->d_name);
        pkg_display_name(e->d_name, ename, out[n].name, sizeof(out[n].name));
        pkg_locale(e->d_name, out[n].locale, sizeof(out[n].locale));
        n++;
    }
    closedir(d);

    /* voice.txt: 完整语音名命中某包 → 该包置首 (默认发音人) */
    if (n > 1) {
        char vtpath[1024];
        snprintf(vtpath, sizeof(vtpath), "%s/voice.txt", edir);
        char *vt = ms_read_file(vtpath, NULL);
        if (vt) {
            char tmpname[128];
            char tmpkey[256];
            for (int i = 1; i < n; i++) {
                int match;
                if (pkgs) match = strcmp(pkgs[i].voice, vt) == 0;
                else {
                    char voice[256];
                    match = pkg_voice_fullname(modeldir, out[i].key, voice, sizeof(voice),
                                               NULL, 0) == 0
                            && strcmp(voice, vt) == 0;
                }
                if (match) {
                    if (pkgs) { ms_pkg t = pkgs[i]; pkgs[i] = pkgs[0]; pkgs[0] = t; }
                    memcpy(tmpname, out[i].name, sizeof(tmpname));
                    memcpy(tmpkey, out[i].key, sizeof(tmpkey));
                    char tmploc[sizeof(out[i].locale)];
                    memcpy(tmploc, out[i].locale, sizeof(tmploc));
                    out[i] = out[0];
                    snprintf(out[0].key, sizeof(out[0].key), "%s", tmpkey);
                    memcpy(out[0].name, tmpname, sizeof(out[0].name));
                    memcpy(out[0].locale, tmploc, sizeof(out[0].locale));
                    break;
                }
            }
            free(vt);
        }
    }
    return n;
}

/* 轻量发音人扫描 (纯文件系统) */
static int ms_scan_voices(const char *root, const mtts_engine_desc *desc,
                          mtts_voice_scan *out, int max) {
    char edir[1024];
    snprintf(edir, sizeof(edir), "%s/%s", root, desc->key);
    return ms_scan_pkgs(edir, desc->voice_name, NULL, 0, out, max);
}

static ms_vsession *ms_get_session(ms_ctx *c, ms_pkg *pk);
static void *ms_warm_thread(void *arg);
static void ms_reap_graves(ms_vsession *vs);
static struct ms_stream *ms_launch_locked(ms_ctx *c, ms_vsession *vs, const char *ssml);
static int32_t ms_stream_read(void *stream, int16_t *buf, int32_t want, int *done);
static void ms_stream_free(void *stream);

static void *ms_create(const char *root, const mtts_engine_desc *desc) {
    ms_ctx *c = calloc(1, sizeof(ms_ctx));
    char path[1024];
    snprintf(c->key, sizeof(c->key), "%s", desc->key);
    snprintf(c->ename, sizeof(c->ename), "%s", desc->voice_name);
    snprintf(c->edir, sizeof(c->edir), "%s/%s", root, desc->key);

    snprintf(path, sizeof(path), "%s/%s/%s", root, desc->key, desc->dylib_name);
    c->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!c->dl) {
        fprintf(stderr, "[MagicTTS][ms] dlopen 失败: %s (%s)\n", path, dlerror());
        free(c);
        return NULL;
    }
    int ok = ms_bind(c->dl, (void **)&c->cfg_create,      "embedded_speech_config_create") &&
             ms_bind(c->dl, (void **)&c->cfg_add_path,    "embedded_speech_config_add_path") &&
             ms_bind(c->dl, (void **)&c->cfg_set_voice,   "embedded_speech_config_set_speech_synthesis_voice") &&
             ms_bind(c->dl, (void **)&c->cfg_propbag,     "speech_config_get_property_bag") &&
             ms_bind(c->dl, (void **)&c->bag_set_string,  "property_bag_set_string") &&
             ms_bind(c->dl, (void **)&c->acfg_wavfile,    "audio_config_create_audio_output_from_wav_file_name") &&
             ms_bind(c->dl, (void **)&c->synth_create,    "synthesizer_create_speech_synthesizer_from_config") &&
             ms_bind(c->dl, (void **)&c->start_speak,     "synthesizer_start_speaking_ssml") &&
             ms_bind(c->dl, (void **)&c->speak_ssml,      "synthesizer_speak_ssml") &&
             ms_bind(c->dl, (void **)&c->get_reason,      "synth_result_get_reason") &&
             ms_bind(c->dl, (void **)&c->stop,            "synthesizer_stop_speaking") &&
             ms_bind(c->dl, (void **)&c->completed_set_cb, "synthesizer_completed_set_callback") &&
             ms_bind(c->dl, (void **)&c->canceled_set_cb, "synthesizer_canceled_set_callback") &&
             ms_bind(c->dl, (void **)&c->release_result,  "synthesizer_result_handle_release") &&
             ms_bind(c->dl, (void **)&c->release_synth,   "synthesizer_handle_release");
    if (!ok) { dlclose(c->dl); free(c); return NULL; }

    /* 扫描 model/ 下的发音人包 (SynthModel.license 或 .key, 各为一个发音人) */
    mtts_voice_scan scan[MS_MAX_PKG];
    c->npkg = ms_scan_pkgs(c->edir, c->ename, c->pkgs, MS_MAX_PKG, scan, MS_MAX_PKG);
    if (c->npkg == 0) {
        fprintf(stderr, "[MagicTTS][ms] model/ 下没有含 SynthModel.license/.key 的包\n");
        dlclose(c->dl); free(c);
        return NULL;
    }
    for (int i = 0; i < c->npkg; i++)
        pkg_display_name(c->pkgs[i].dir, c->ename, c->vnames[i], sizeof(c->vnames[i]));

    fprintf(stderr, "[MagicTTS][ms] %s 就绪 (%d 个可用语音包)\n", desc->key, c->npkg);

    /* 预热已移除 (2026-10-17): 后台空跑与真实请求在 SDK/ORT 状态机上竞争,
     * stop 打断推理会让共享的 onnxruntime 全局僵死 (两引擎 dylib 链接同一
     * libonnxruntime, 连坐). 会话跨句复用 ⇒ 首句 ~0.9s 冷推理后即热.
     * ms_warm_thread 保留备查. */
    return c;
}

/* 预热: 走与真实合成完全一致的流式管线 (launch → 读尽 → 入墓),
 * 避免 speak_ssml 直调与后续合成在 SDK/ORT 状态机上交叠 (实测推理线程段错误). */
__attribute__((unused))
static void *ms_warm_thread(void *arg) {
    ms_vsession *vs = arg;
    ms_ctx *c = vs->owner;

    pthread_mutex_lock(&vs->lock);
    if (vs->warmed || vs->got_real) { pthread_mutex_unlock(&vs->lock); return NULL; }
    vs->warmed = 1;

    char ssml[512];
    snprintf(ssml, sizeof(ssml),
             "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
             "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
             "<voice name=\"%s\">嗯。</voice></speak>", vs->pkg->voice);
    double t0 = now_ms_f();
    struct ms_stream *st = ms_launch_locked(c, vs, ssml);
    pthread_mutex_unlock(&vs->lock);
    if (!st) {
        fprintf(stderr, "[MagicTTS][ms] 预热启动失败 %s\n", vs->pkg->dir);
        return NULL;
    }
    int16_t sink[512];
    int done = 0;
    while (!done) {
        if (ms_stream_read(st, sink, 512, &done) == 0 && !done) usleep(10000);
    }
    ms_stream_free(st);          /* 入墓园, 下次 stream_start 统一销毁 */
    fprintf(stderr, "[MagicTTS][ms] 预热完成 %s (%.0fms)\n", vs->pkg->dir, now_ms_f() - t0);
    return NULL;
}

/* ---------- 发音人枚举 ---------- */
static int ms_voice_count(void *session) { return ((ms_ctx *)session)->npkg; }

static const char *ms_voice_key(void *session, int idx) {
    ms_ctx *c = session;
    if (idx < 0 || idx >= c->npkg) return NULL;
    return c->pkgs[idx].dir;
}

static const char *ms_voice_name(void *session, int idx) {
    ms_ctx *c = session;
    if (idx < 0 || idx >= c->npkg) return NULL;
    return c->vnames[idx];
}

/* ---------- 流式合成 ---------- */

/* 结束回调: 置 completed (arg1=synth 句柄 → 查会话 → active 流) */
static void ms_on_completed(ms_handle synth, ms_handle event) {
    ms_vsession *vs = ms_reg_find(synth);
    if (!vs) return;
    pthread_mutex_lock(&vs->lock);
    struct ms_stream *st = vs->active;
    if (st) {
        pthread_mutex_lock(&st->lock);
        st->completed = 1;
        pthread_mutex_unlock(&st->lock);
    }
    pthread_mutex_unlock(&vs->lock);
    (void)event;
}

static void ms_on_canceled(ms_handle synth, ms_handle event) {
    ms_vsession *vs = ms_reg_find(synth);
    if (!vs) return;
    pthread_mutex_lock(&vs->lock);
    struct ms_stream *st = vs->active;
    if (st) {
        pthread_mutex_lock(&st->lock);
        st->canceled = 1;
        pthread_mutex_unlock(&st->lock);
    }
    pthread_mutex_unlock(&vs->lock);
    (void)event;
}

/* 懒建发音人会话 (首次使用该发音人时); g_sess_lock 防预热线程与请求线程
 * 并发双建 (pk->sess 检查-赋值非原子) */
static pthread_mutex_t g_sess_lock = PTHREAD_MUTEX_INITIALIZER;

static ms_vsession *ms_get_session(ms_ctx *c, ms_pkg *pk) {
    pthread_mutex_lock(&g_sess_lock);
    if (pk->sess) { pthread_mutex_unlock(&g_sess_lock); return pk->sess; }

    ms_vsession *vs = calloc(1, sizeof(ms_vsession));
    vs->pkg = pk;
    vs->owner = c;
    pthread_mutex_init(&vs->lock, NULL);
    pthread_cond_init(&vs->warm_cond, NULL);

    char *cred = ms_read_file(pk->credpath, NULL);
    if (!cred) { fprintf(stderr, "[MagicTTS][ms] 凭证读失败: %s\n", pk->credpath); goto fail; }

    /* .key 包须加 "Key:" 前缀 (Windows grtts_x64.dll 逆向结论: 前缀决定引擎走
     * AES-GCM 解密路径而非 license 校验; 主 key 90 字符, 7 个 OneCore 包通用) */
    char keybuf[160];
    const char *cred_arg = cred;
    if (pk->keypkg) {
        snprintf(keybuf, sizeof(keybuf), "Key:%s", cred);
        cred_arg = keybuf;
    }

    ms_handle cfg = 0;
    /* add_path 传模型叶子目录 (<LCID>.INI 所在处, Windows 同款): 引擎按 INI 取
     * HDVoiceSeqLen=5 / 关输出缓冲与节流; 传父目录时 INI 可能不加载, SeqLen
     * 退化为默认 24 → HD 语音首块台阶延迟 (详见 docs/NOTES-win-re-ms-voices.md) */
    char leafdir[2048];
    snprintf(leafdir, sizeof(leafdir), "%s/model/%s/data/%s", c->edir, pk->dir, pk->vdir);
    if (c->cfg_create(&cfg) != 0 || c->cfg_add_path(cfg, leafdir) != 0) {
        fprintf(stderr, "[MagicTTS][ms] fromPath 失败: %s\n", leafdir);
        free(cred); goto fail;
    }
    int rc = c->cfg_set_voice(cfg, pk->voice, cred_arg);
    free(cred);
    if (rc != 0) { fprintf(stderr, "[MagicTTS][ms] set_voice rc=0x%x (%s)\n", rc, pk->voice); goto fail; }

    /* 输出格式: 模型仅 24kHz (id 3102, name 必须传 NULL) */
    ms_handle bag = 0;
    if (c->cfg_propbag(cfg, &bag) == 0 && bag)
        c->bag_set_string(bag, 3102 /*SpeechServiceConnectionSynthOutputFormat*/,
                          NULL, "raw-24khz-16bit-mono-pcm");

    /* 目的地: 每发音人独立临时 wav (appex 沙盒 TMPDIR 可写; SDK 持句柄, 不 unlink) */
    const char *tmp = getenv("TMPDIR");
    snprintf(vs->wavpath, sizeof(vs->wavpath), "%s/mtts_ms_%s_%s.wav",
             (tmp && tmp[0]) ? tmp : "/tmp", c->key, pk->dir);

    ms_handle acfg = 0;
    c->acfg_wavfile(&acfg, vs->wavpath);
    if (c->synth_create(&vs->synth, cfg, acfg) != 0) {
        fprintf(stderr, "[MagicTTS][ms] synthesizer 创建失败 (%s)\n", pk->dir);
        goto fail;
    }
    c->completed_set_cb(vs->synth, ms_on_completed, vs->synth);
    c->canceled_set_cb(vs->synth, ms_on_canceled, vs->synth);
    vs->stop = c->stop;
    ms_reg_add(vs->synth, vs);
    fprintf(stderr, "[MagicTTS][ms] 发音人会话就绪: %s (%s)\n", pk->dir, pk->voice);
    pk->sess = vs;
    pthread_mutex_unlock(&g_sess_lock);
    return vs;
fail:
    pthread_cond_destroy(&vs->warm_cond);
    pthread_mutex_destroy(&vs->lock);
    free(vs);
    pthread_mutex_unlock(&g_sess_lock);
    return NULL;
}

static void xml_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 8 < cap; p++) {
        switch (*p) {
        case '&': memcpy(out + o, "&amp;", 5);  o += 5; break;
        case '<': memcpy(out + o, "&lt;", 4);   o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4);   o += 4; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(out + o, "&apos;", 6); o += 6; break;
        default: out[o++] = *p;
        }
    }
    out[o] = 0;
}

/* 延迟探针: stream_start 各相位耗时 → $TMPDIR/mtts_ms_lat.log */
static void ms_lat_log(double t0, double t_sess, double t_reap, double t_settle) {
    const char *tmp = getenv("TMPDIR");
    char p[1100];
    snprintf(p, sizeof(p), "%s/mtts_ms_lat.log", (tmp && tmp[0]) ? tmp : "/tmp");
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "start: total=%.0f (sess=%.0f reap=%.0f settle=%.0f launch=%.0f)\n",
            now_ms_f() - t0, t_sess - t0, t_reap - t_sess, t_settle - t_reap,
            now_ms_f() - t_settle);
    fclose(f);
}

static void *ms_stream_start(void *session, const char *voice_key, const char *text,
                             int rate, int pitch, int32_t *out_sample_rate) {
    double t0 = now_ms_f(), t_sess = t0, t_reap = t0, t_settle = t0;
    ms_ctx *c = session;
    ms_pkg *pk = &c->pkgs[0];
    if (voice_key && voice_key[0]) {
        int hit = 0;
        for (int i = 0; i < c->npkg; i++)
            if (strcmp(c->pkgs[i].dir, voice_key) == 0) { pk = &c->pkgs[i]; hit = 1; break; }
        if (!hit) {
            fprintf(stderr, "[MagicTTS][ms] 未知发音人 %s, 用默认\n", voice_key);
            pk = &c->pkgs[0];
        }
    }

    ms_vsession *vs = ms_get_session(c, pk);
    t_sess = now_ms_f();
    if (!vs) return NULL;

    /* SSML (prosody 语速/音高). 引擎 rate 封顶 200 (实测), 超出部分 Sonic 补速 */
    char esc[4096];
    xml_escape(text, esc, sizeof(esc));
    int r = rate < 20 ? 20 : (rate > 500 ? 500 : rate);
    int er = r < MS_ENGINE_RATE_MAX ? r : MS_ENGINE_RATE_MAX;
    float extra = er > 0 ? r / (float)er : 1.0f;
    int pd = (pitch - 50) * 2;
    if (pd < -100) pd = -100;
    if (pd > 100) pd = 100;
    char ssml[8704];
    snprintf(ssml, sizeof(ssml),
             "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
             "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
             "<voice name=\"%s\"><prosody rate=\"%d%%\" pitch=\"%+d%%\">%s</prosody></voice>"
             "</speak>", pk->voice, er, pd, esc);

    pthread_mutex_lock(&vs->lock);
    vs->got_real = 1;
    while (vs->warming)                 /* 预热在途: 等它读完, 绝不打断 (ORT 红线) */
        pthread_cond_wait(&vs->warm_cond, &vs->lock);
    ms_reap_graves(vs);
    t_reap = now_ms_f();
    /* 在途流先取消 (其 reader 因 canceled 收尾); 打断推理属 ORT 红线路径,
     * 保留 stop + 短拍 — 但 old 已 completed (推理结束) 时免 stop 免拍.
     * 空闲 (active==NULL) 直接 start — 旧版无条件 stop+150ms 拍的"空完成"
     * 实为陈旧回调误判, 已由 read 的 off==0 守卫根治 (裸探针 3 轮验证). */
    if (vs->active) {
        struct ms_stream *old = vs->active;
        pthread_mutex_lock(&old->lock);
        old->canceled = 1;
        int old_done = old->completed;
        pthread_mutex_unlock(&old->lock);
        if (!old_done) {
            c->stop(vs->synth);
            clock_gettime(CLOCK_MONOTONIC, &vs->settle_until);
            vs->settle_until.tv_nsec += 30 * 1000000L;
            if (vs->settle_until.tv_nsec >= 1000000000L) {
                vs->settle_until.tv_sec += 1;
                vs->settle_until.tv_nsec -= 1000000000L;
            }
        }
    }
    /* stop (本函数或 cancel 路径) 之后须过结算窗口才重启 */
    struct timespec nowts;
    clock_gettime(CLOCK_MONOTONIC, &nowts);
    if (nowts.tv_sec < vs->settle_until.tv_sec ||
        (nowts.tv_sec == vs->settle_until.tv_sec &&
         nowts.tv_nsec < vs->settle_until.tv_nsec)) {
        long wait_us = (vs->settle_until.tv_sec - nowts.tv_sec) * 1000000L
                     + (vs->settle_until.tv_nsec - nowts.tv_nsec) / 1000L;
        pthread_mutex_unlock(&vs->lock);
        if (wait_us > 0) usleep((useconds_t)wait_us);
        pthread_mutex_lock(&vs->lock);
    }
    struct ms_stream *st = ms_launch_locked(c, vs, ssml);
    t_settle = now_ms_f();
    if (st && extra > 1.001f) {
        /* 流尚未返回给渲染线程 (vs->lock 仍持), 在此挂 Sonic 补速 */
        st->so = sonicCreateStream(24000, 1);
        if (st->so) sonicSetSpeed(st->so, extra);
    }
    pthread_mutex_unlock(&vs->lock);
    ms_lat_log(t0, t_sess, t_reap, t_settle);
    if (!st) return NULL;
    *out_sample_rate = 24000;

    /* 记忆本语音 → Engines/.lastvoice (进程启动时据此后台预热, 见
     * mtts_warm_last_voice); 仅变化时写盘 */
    {
        char combo[320];
        snprintf(combo, sizeof(combo), "%s %s", c->key, pk->dir);
        if (strcmp(c->remembered, combo) != 0) {
            char p[1100];
            const char *slash = strrchr(c->edir, '/');
            if (slash) {
                snprintf(p, sizeof(p), "%.*s/.lastvoice", (int)(slash - c->edir), c->edir);
                FILE *f = fopen(p, "w");
                if (f) { fprintf(f, "%s\n", combo); fclose(f); }
                snprintf(c->remembered, sizeof(c->remembered), "%s", combo);
            }
        }
    }
    return st;
}

/* 锁内启动: 清场 → 开跟随 fd → 挂 active → start_speaking (立即返回).
 * probe 实验 (M4/M5): 同一 synth 连续 start_speaking 无需句间 stop. */
static struct ms_stream *ms_launch_locked(ms_ctx *c, ms_vsession *vs, const char *ssml) {
    struct ms_stream *st = calloc(1, sizeof(*st));
    st->vs = vs;
    pthread_mutex_init(&st->lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &st->last_growth);

    /* 清场: 截断 wav (SDK 句柄仍在, 下次写从 0 覆盖, 实测) */
    FILE *clr = fopen(vs->wavpath, "wb");
    if (clr) fclose(clr);
    st->fd = open(vs->wavpath, O_RDONLY);
    if (st->fd < 0) {
        pthread_mutex_destroy(&st->lock);
        free(st);
        return NULL;
    }
    vs->active = st;

    ms_handle result = 0;
    int rc = c->start_speak(vs->synth, ssml, (unsigned)strlen(ssml), &result);
    if (result) c->release_result(result);
    if (rc != 0) {
        fprintf(stderr, "[MagicTTS][ms] start_speaking 失败 rc=0x%x\n", rc);
        vs->active = NULL;
        close(st->fd);
        pthread_mutex_destroy(&st->lock);
        free(st);
        return NULL;
    }
    return st;
}

static int32_t ms_stream_read(void *stream, int16_t *buf, int32_t want, int *done) {
    struct ms_stream *st = stream;
    int32_t got_total = 0;

    for (;;) {
        /* 先排干 Sonic 已处理输出 (无 Sonic 时此步为空, 行为同旧版) */
        if (st->so && got_total < want) {
            int n = sonicReadShortFromStream(st->so, buf + got_total, want - got_total);
            if (n > 0) got_total += n;
        }
        if (got_total >= want) {
            if (done) *done = 0;   /* 本次读满; 是否结束下次读时判断 */
            return got_total;
        }

        struct stat sb;
        off_t size = 0;
        if (fstat(st->fd, &sb) == 0) size = sb.st_size;
        off_t avail = size - st->off;

        pthread_mutex_lock(&st->lock);
        int completed = st->completed, canceled = st->canceled;
        pthread_mutex_unlock(&st->lock);

        if (avail >= 2) {
            /* 原始样本: 无 Sonic 直拷; 有 Sonic 过流 (循环顶再取回处理后的) */
            if (!st->so) {
                ssize_t want_bytes = (off_t)want * 2 - got_total * 2;
                if (want_bytes > avail) want_bytes = avail;
                want_bytes &= ~((ssize_t)1);
                ssize_t n = pread(st->fd, (uint8_t *)buf + got_total * 2,
                                  want_bytes, st->off);
                if (n > 0) {
                    st->off += n;
                    got_total += (int32_t)(n / 2);
                    clock_gettime(CLOCK_MONOTONIC, &st->last_growth);
                    st->stable_rounds = 0;
                }
                continue;
            }
            ssize_t want_bytes = (off_t)sizeof(st->raw);
            if (want_bytes > avail) want_bytes = avail;
            want_bytes &= ~((ssize_t)1);
            ssize_t n = pread(st->fd, st->raw, want_bytes, st->off);
            if (n > 0) {
                st->off += n;
                clock_gettime(CLOCK_MONOTONIC, &st->last_growth);
                st->stable_rounds = 0;
                sonicWriteShortToStream(st->so, st->raw, (int)(n / 2));
            }
            continue;
        }

        /* 无新数据: 判定是否结束 */
        if (canceled) { if (done) *done = 1; return got_total; }
        if (completed && st->off > 0) {
            /* 已有数据: 文件尺寸稳定 (3 轮 10ms 一致) → 终尺寸确定.
             * off==0 时 completed 可能是上一次合成(预热)的陈旧回调, 不判结束. */
            if (!st->stable) {
                if (size == st->stable_size) {
                    if (++st->stable_rounds >= 3) st->stable = 1;
                } else {
                    st->stable_size = size;
                    st->stable_rounds = 0;
                }
            }
            if (st->stable && st->off >= st->stable_size) {
                if (st->so) {
                    if (!st->flushed) {
                        sonicFlushStream(st->so);   /* 终局: 冲出残余 */
                        st->flushed = 1;
                        continue;
                    }
                    if (sonicSamplesAvailable(st->so) > 0) continue;
                }
                if (done) *done = 1;
                return got_total;
            }
            usleep(10000);
            continue;
        }

        /* 合成中: 无进展超时 (20s) 兜底, 防渲染线程永挂 */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - st->last_growth.tv_sec) * 1000
                + (now.tv_nsec - st->last_growth.tv_nsec) / 1000000;
        if (ms > 20000) {
            fprintf(stderr, "[MagicTTS][ms] 流无进展超 %.1fs, 放弃 (completed=%d)\n",
                    ms / 1000.0, completed);
            if (st->so && !st->flushed) {
                sonicFlushStream(st->so);
                st->flushed = 1;
                continue;
            }
            if (done) *done = 1;
            return got_total;
        }
        usleep(10000);
    }
}

static void ms_stream_cancel(void *stream) {
    struct ms_stream *st = stream;
    if (!st) return;
    pthread_mutex_lock(&st->lock);
    st->canceled = 1;
    int was_completed = st->completed;
    pthread_mutex_unlock(&st->lock);

    ms_vsession *vs = st->vs;
    pthread_mutex_lock(&vs->lock);
    if (vs->active == st) {
        vs->active = NULL;          /* 摘除: 回调不再触达该流 */
        /* completed=1 → 推理早已结束, 无需 stop (系统播完照例 cancel 一次,
         * 无脑 stop 会设 50ms 结算窗, 下一句白付 ~54ms — 2026-10-18 实测);
         * 仅真正打断在途推理 (completed=0) 才 stop + 结算窗 (ORT 红线). */
        if (!was_completed) {
            if (vs->stop) vs->stop(vs->synth);   /* 终止后台合成 */
            clock_gettime(CLOCK_MONOTONIC, &vs->settle_until);
            vs->settle_until.tv_nsec += 50 * 1000000L;
            if (vs->settle_until.tv_nsec >= 1000000000L) {
                vs->settle_until.tv_sec += 1;
                vs->settle_until.tv_nsec -= 1000000000L;
            }
        }
    }
    pthread_mutex_unlock(&vs->lock);
}

static void ms_stream_free(void *stream) {
    struct ms_stream *st = stream;
    if (!st) return;
    ms_vsession *vs = st->vs;
    pthread_mutex_lock(&vs->lock);
    pthread_mutex_lock(&st->lock);
    if (vs->active == st) vs->active = NULL;
    st->next = vs->grave;      /* 入墓园: 在途回调可能仍持 st 指针, */
    vs->grave = st;            /* 延迟到下次 stream_start 真正销毁 */
    pthread_mutex_unlock(&st->lock);
    pthread_mutex_unlock(&vs->lock);
    if (st->so) sonicDestroyStream(st->so);
    st->so = NULL;             /* 墓园期 read 不再触碰 (free 后本就该无 read) */
    close(st->fd);
}

/* 墓园回收 (stream_start 持 vs->lock 时调用): 锁握手即内存栅栏 — 回调须持
 * st->lock 才触碰墓园流, 握手后必已离开, 无需睡眠等待 (2026-10-18 延迟治理:
 * 原每具 20/100ms 串行睡眠是稳态延迟主源之一, 积压时叠加至上百 ms). */
static void ms_reap_graves(ms_vsession *vs) {
    while (vs->grave) {
        struct ms_stream *g = vs->grave;
        vs->grave = g->next;
        pthread_mutex_lock(&g->lock);
        pthread_mutex_unlock(&g->lock);
        pthread_mutex_destroy(&g->lock);
        free(g);
    }
}

/* ---------- 安全后台预热 (2026-10-18) ----------
 * HD 模型冷启 ~650ms (加载+首推理, 每会话私有, 跨会话不共享 — 实测).
 * 预热 = 一次微型合成跑完整 launch→读尽→入墓 循环, 全程绝不 stop
 * (ORT 红线: stop 打断推理会僵死全局). 真实 stream_start 到来时在
 * vs->lock 上等 warming 结束 — 等待 ≤ 冷启成本, 任何时序都不亏. */
static void ms_warm_session(ms_ctx *c, ms_vsession *vs) {
    pthread_mutex_lock(&vs->lock);
    if (vs->got_real || vs->active || vs->warming || vs->warmed) {
        pthread_mutex_unlock(&vs->lock);
        return;
    }
    vs->warming = 1;
    char ssml[512];
    snprintf(ssml, sizeof(ssml),
             "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
             "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
             "<voice name=\"%s\">嗯。</voice></speak>", vs->pkg->voice);
    struct ms_stream *st = ms_launch_locked(c, vs, ssml);
    pthread_mutex_unlock(&vs->lock);
    if (st) {
        int16_t sink[512];
        int done = 0;
        while (!done) {
            if (ms_stream_read(st, sink, 512, &done) == 0 && !done) usleep(10000);
        }
        ms_stream_free(st);          /* 入墓园, 下次 stream_start 统一销毁 */
        fprintf(stderr, "[MagicTTS][ms] 预热完成 %s\n", vs->pkg->dir);
    }
    pthread_mutex_lock(&vs->lock);
    vs->warmed = st != NULL;
    vs->warming = 0;
    pthread_cond_broadcast(&vs->warm_cond);
    pthread_mutex_unlock(&vs->lock);
}

struct ms_warm_arg { ms_ctx *c; ms_vsession *vs; };

static void *ms_warm_voice_thread(void *p) {
    struct ms_warm_arg *a = p;
    ms_warm_session(a->c, a->vs);
    free(a);
    return NULL;
}

/* adapter 钩子: 后台预热某发音人 (voice_key 空 = 引擎默认) */
static void ms_warm_voice(void *session, const char *voice_key) {
    ms_ctx *c = session;
    ms_pkg *pk = &c->pkgs[0];
    if (voice_key && voice_key[0]) {
        int hit = 0;
        for (int i = 0; i < c->npkg; i++)
            if (strcmp(c->pkgs[i].dir, voice_key) == 0) { pk = &c->pkgs[i]; hit = 1; break; }
        if (!hit) return;
    }
    ms_vsession *vs = ms_get_session(c, pk);   /* 沙盒/物料异常 → NULL 自然放弃 */
    if (!vs) return;
    struct ms_warm_arg *a = malloc(sizeof(*a));
    if (!a) return;
    a->c = c; a->vs = vs;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, ms_warm_voice_thread, a) != 0) free(a);
    pthread_attr_destroy(&at);
}

static void ms_destroy(void *session) {
    ms_ctx *c = session;
    if (!c) return;
    for (int i = 0; i < c->npkg; i++) {
        ms_vsession *vs = c->pkgs[i].sess;
        if (vs) {
            if (vs->synth) c->release_synth(vs->synth);
            pthread_cond_destroy(&vs->warm_cond);
            pthread_mutex_destroy(&vs->lock);
            free(vs);
        }
    }
    if (c->dl) dlclose(c->dl);
    free(c);
}

static const mtts_adapter_if kMsAdapter = {
    ms_probe, ms_scan_voices, ms_create,
    ms_voice_count, ms_voice_key, ms_voice_name,
    ms_stream_start, ms_stream_read, ms_stream_cancel, ms_stream_free,
    ms_destroy,
    ms_warm_voice,
};
const mtts_adapter_if *mtts_ms_adapter(void) { return &kMsAdapter; }

/* ==== 10-17 排障记录 (供后来者) ====
 * SIGBUS: 句柄按 uint32 截断 → 改 void*。
 * VOICENAME_NOTEXIST: 短名 → 用 Tokens.xml 完整显示名。
 * WRONG_DECRYPTION_KEY: 未传凭证 → set_voice(cfg, 名, license全文或"Key:"+key内容)。
 * AUDIO_FORMAT_NOT_SUPPORTED: 默认16k → 属性3102(name=NULL)="raw-24khz-16bit-mono-pcm"。
 * 流式关键实测 (ms_stream_probe.c, 2026-10-17):
 *   - result/事件直取音频均 0 字节 (空 AudioConfig 下 Synthesizing 事件也空),
 *     音频只走 AudioConfig 目的地 → wav 文件;
 *   - synthesizer_start_speaking_ssml(h, ssml, len, &res) 立即返回 (0ms),
 *     文件随合成渐进写入, 首块 ~0.75s;
 *   - completed/canceled 回调 ABI: cb(synth句柄, 事件句柄) — ctx 不回传,
 *     按 arg1=synth 查注册表路由到当前流;
 *   - synth_result_get_audio_data ABI: (res, buf非空, cap入, 实拷出) — 不支持空探。
 * 模型来源: tts2 (Windows NVDA 插件) 的 NeuralVoices 包, 跨平台数据。
 * Windows 侧 SDK=1.45.0, macOS 1.45/1.48 均验证可用。 */
