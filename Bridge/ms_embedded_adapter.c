/* Microsoft 嵌入式语音适配器 (naturalvoice + onecore 共用).
 *
 * 对应 Windows 版 (原版 Windows 实现) 与 Android 版
 * (resources/reference/restored/MsEmbeddedEngines.java) 的 EmbeddedSpeechConfig.fromPath 模式.
 *
 * ★ 2026-10-17 实测打通的关键事实 (详见文件底部注释):
 *   1. SPX*HANDLE 为指针尺寸 (按 32 位截断会 SIGBUS);
 *   2. 语音名 = Tokens.xml 里的完整显示名 ("Microsoft Xiaoxiao (Natural HD) - ..."),
 *      短名 zh-CN-XiaoxiaoNeural 会报 VOICENAME_NOTEXIST;
 *   3. 必须调 set_speech_synthesis_voice(cfg, 完整名, license全文):
 *      license = 包根的 SynthModel.license (HD包) 或 SynthModel.key (Natural包),
 *      不带 → WRONG_DECRYPTION_KEY (模型是 license 门控加密的);
 *   4. 必须设输出格式属性 (id 3102, name 传 NULL) = "raw-24khz-16bit-mono-pcm",
 *      否则默认 16kHz → AUDIO_FORMAT_NOT_SUPPORTED (模型仅 24k);
 *   5. 音频要从 AudioConfig 目的地取: 一期用临时 wav 文件输出后读回
 *      (result 直取/pull 流均拿不到); SDK 写的文件头为零, 实为 raw PCM;
 *   6. 假 license 会被拒 (INVALID_LICENSE), 校验机制真实.
 *
 * 布局: Engines/<key>/
 *   libMicrosoft.CognitiveServices.Speech.core.dylib (+embedded.tts/+onnxruntime)
 *   model/<包>/            ← 语音包 (含 SynthModel.license/.key + data/…)
 *   voice.txt              ← 完整语音名 (如 "Microsoft Xiaoxiao (Natural HD) - Chinese (Simplified, China)")
 */
#include "adapters.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ★ 句柄为指针尺寸 (实测) */
typedef void *ms_handle;

typedef int   (*fn_cfg_create)(ms_handle *);
typedef int   (*fn_cfg_add_path)(ms_handle, const char *);
typedef int   (*fn_cfg_set_voice)(ms_handle, const char *, const char *);
typedef int   (*fn_cfg_propbag)(ms_handle, ms_handle *);
typedef int   (*fn_bag_set_string)(ms_handle, int, const char *, const char *);
typedef int   (*fn_acfg_wavfile)(ms_handle *, const char *);
typedef int   (*fn_synth_create)(ms_handle *, ms_handle, ms_handle);
typedef int   (*fn_speak_ssml)(ms_handle, const char *, unsigned, ms_handle *);
typedef int   (*fn_get_len)(ms_handle, unsigned *, unsigned *);
typedef int   (*fn_get_reason)(ms_handle, int *);
typedef int   (*fn_get_reason_detail)(ms_handle, int *);
typedef int   (*fn_stop)(ms_handle);
typedef int   (*fn_release_result)(ms_handle);
typedef int   (*fn_release_synth)(ms_handle);

typedef struct {
    void *dl;
    fn_cfg_create       cfg_create;
    fn_cfg_add_path     cfg_add_path;
    fn_cfg_set_voice    cfg_set_voice;
    fn_cfg_propbag      cfg_propbag;
    fn_bag_set_string   bag_set_string;
    fn_acfg_wavfile     acfg_wavfile;
    fn_synth_create     synth_create;
    fn_speak_ssml       speak_ssml;
    fn_get_len          get_len;
    fn_get_reason       get_reason;
    fn_get_reason_detail get_reason_detail;
    fn_stop             stop;
    fn_release_result   release_result;
    fn_release_synth    release_synth;
    ms_handle synth;
    char voice[256];      /* 完整语音名 (Tokens.xml 显示名) */
    char wavpath[512];    /* 每次合成复用的临时输出文件 */
} ms_ctx;

static int ms_bind(void *dl, void **slot, const char *name) {
    *slot = dlsym(dl, name);
    if (!*slot) fprintf(stderr, "[MagicTTS][ms] 缺符号 %s\n", name);
    return *slot != NULL;
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

/* model 目录非空 */
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

/* 读整个文件到 malloc 缓冲 */
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
    /* 去尾部换行 */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    return buf;
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

/* 选定语音包: model/ 下第一个含 SynthModel.license 的包 (实测 .key 过不了解密,
 * 仅 .license 有效); 从其 data 下的 Tokens.xml 提取完整语音名. 返回 0 成功. */
static int ms_auto_config(const char *root, const char *key,
                          char *voice, size_t vsz, char **lic_out) {
    char modeldir[1024], pkg[1152], found[768] = {0}, licpath[1280] = {0};
    snprintf(modeldir, sizeof(modeldir), "%s/%s/model", root, key);
    DIR *d = opendir(modeldir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[1280];
        snprintf(p, sizeof(p), "%s/%s/SynthModel.license", modeldir, e->d_name);
        if (access(p, R_OK) == 0) {
            snprintf(found, sizeof(found), "%s", e->d_name);
            snprintf(licpath, sizeof(licpath), "%s", p);
            break;
        }
    }
    closedir(d);
    if (!found[0]) {
        fprintf(stderr, "[MagicTTS][ms] model/ 下没有含 SynthModel.license 的包 (仅 .key 的 Natural 包被拒: set_voice 后合成 reason=1, 实测 2026-10-17)\n");
        return -1;
    }

    /* Tokens.xml: 第一个 <String name="" value="完整语音名" .../> */
    snprintf(pkg, sizeof(pkg), "%s/%s/data", modeldir, found);
    DIR *dd = opendir(pkg);
    if (!dd) return -1;
    char tokens[1536];
    while ((e = readdir(dd)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(tokens, sizeof(tokens), "%s/%s/Tokens.xml", pkg, e->d_name);
        if (access(tokens, R_OK) == 0) break;
        tokens[0] = 0;
    }
    closedir(dd);
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

    *lic_out = ms_read_file(licpath, NULL);
    return *lic_out ? 0 : -1;
}

static void *ms_create(const char *root, const mtts_engine_desc *desc) {
    ms_ctx *c = calloc(1, sizeof(ms_ctx));
    char path[1024];

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
             ms_bind(c->dl, (void **)&c->speak_ssml,      "synthesizer_speak_ssml") &&
             ms_bind(c->dl, (void **)&c->get_len,         "synth_result_get_audio_length_duration") &&
             ms_bind(c->dl, (void **)&c->get_reason,      "synth_result_get_reason") &&
             ms_bind(c->dl, (void **)&c->stop,            "synthesizer_stop_speaking") &&
             ms_bind(c->dl, (void **)&c->release_result,  "synthesizer_result_handle_release") &&
             ms_bind(c->dl, (void **)&c->release_synth,   "synthesizer_handle_release");
    if (!ok) { dlclose(c->dl); free(c); return NULL; }

    /* fromPath = create + add_path(包根的父目录) */
    ms_handle cfg = 0;
    char modeldir[1024];
    snprintf(modeldir, sizeof(modeldir), "%s/%s/model", root, desc->key);
    if (c->cfg_create(&cfg) != 0 || c->cfg_add_path(cfg, modeldir) != 0) {
        fprintf(stderr, "[MagicTTS][ms] fromPath 失败: %s\n", modeldir);
        dlclose(c->dl); free(c);
        return NULL;
    }

    /* 自动选定语音包 + 提取完整名 + license (voice.txt 可覆盖语音名) */
    char *lic = NULL;
    if (ms_auto_config(root, desc->key, c->voice, sizeof(c->voice), &lic) != 0) {
        fprintf(stderr, "[MagicTTS][ms] 语音包配置失败\n");
        dlclose(c->dl); free(c);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s/voice.txt", root, desc->key);
    char *vt = ms_read_file(path, NULL);
    if (vt) { snprintf(c->voice, sizeof(c->voice), "%s", vt); free(vt); }

    int rc = c->cfg_set_voice(cfg, c->voice, lic);
    fprintf(stderr, "[MagicTTS][ms] set_voice rc=0x%x (voice=%s lic=%zu字节)\n",
            rc, c->voice, strlen(lic));
    free(lic);

    /* 输出格式: 模型仅 24kHz (id 3102, name 必须传 NULL) */
    ms_handle bag = 0;
    if (c->cfg_propbag(cfg, &bag) == 0 && bag) {
        c->bag_set_string(bag, 3102 /*SpeechServiceConnectionSynthOutputFormat*/,
                          NULL, "raw-24khz-16bit-mono-pcm");
    }

    /* 临时输出文件 (appex 沙盒内 TMPDIR 可写) */
    const char *tmp = getenv("TMPDIR");
    snprintf(c->wavpath, sizeof(c->wavpath), "%s/mtts_ms_%s.wav",
             (tmp && tmp[0]) ? tmp : "/tmp", desc->key);
    unlink(c->wavpath);   /* 首次清场; 此后不再 unlink (SDK 持有文件句柄) */

    ms_handle acfg = 0;
    c->acfg_wavfile(&acfg, c->wavpath);
    if (c->synth_create(&c->synth, cfg, acfg) != 0) {
        fprintf(stderr, "[MagicTTS][ms] synthesizer 创建失败\n");
        dlclose(c->dl); free(c);
        return NULL;
    }
    fprintf(stderr, "[MagicTTS][ms] %s 就绪\n", desc->key);
    return c;
}

/* 读回 SDK 写的输出文件: 头部为零的 raw PCM (实测), 等待 flush 稳定 */
static int16_t *ms_read_output(const char *path, int32_t *samples) {
    long last = -1;
    for (int i = 0; i < 100; i++) {           /* 最多等 10s, 两轮大小一致即稳定 */
        FILE *f = fopen(path, "rb");
        if (!f) { usleep(100000); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz > 0 && sz == last) break;
        last = sz;
        usleep(100000);
    }
    if (last <= 44) return NULL;
    char *buf = ms_read_file(path, NULL);
    if (!buf) return NULL;
    int32_t n = (int32_t)last / 2;
    int16_t *pcm = malloc(n * 2);
    /* 头部 44 字节为零占位 (SDK 未写 RIFF 头), 对应 ~1ms 静音, 无碍 */
    memcpy(pcm, buf, n * 2);
    free(buf);
    *samples = n;
    return pcm;
}

static mtts_status ms_synthesize(void *session, const char *text,
                                  int rate, int pitch,
                                  int16_t **out_pcm, int32_t *out_samples,
                                  int32_t *out_sample_rate,
                                  const volatile unsigned *stop_gen, unsigned my_gen) {
    ms_ctx *c = (ms_ctx *)session;
    if (!c || !c->synth) return MTTS_ERR_INIT;

    char esc[4096];
    xml_escape(text, esc, sizeof(esc));

    int r = rate < 20 ? 20 : (rate > 500 ? 500 : rate);
    int pd = (pitch - 50) * 2;
    if (pd < -100) pd = -100;
    if (pd > 100) pd = 100;

    char ssml[8704];
    if (c->voice[0])
        snprintf(ssml, sizeof(ssml),
                 "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
                 "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
                 "<voice name=\"%s\"><prosody rate=\"%d%%\" pitch=\"%+d%%\">%s</prosody></voice>"
                 "</speak>", c->voice, r, pd, esc);
    else
        snprintf(ssml, sizeof(ssml),
                 "<speak version=\"1.0\" xml:lang=\"zh-CN\" "
                 "xmlns=\"http://www.w3.org/2001/10/synthesis\">"
                 "<prosody rate=\"%d%%\" pitch=\"%+d%%\">%s</prosody></speak>",
                 r, pd, esc);

    /* 不能 unlink: SDK 在 create 时已打开该文件. 读完后清空供下次复用. */
    ms_handle result = 0;
    int rc = c->speak_ssml(c->synth, ssml, (unsigned)strlen(ssml), &result);
    if (rc != 0 || !result) {
        fprintf(stderr, "[MagicTTS][ms] speak_ssml 失败 rc=0x%x\n", rc);
        return MTTS_ERR_SYNTH;
    }
    int reason = 0;
    c->get_reason(result, &reason);
    c->release_result(result);
    if (reason != 9 /*SynthesizingAudioCompleted*/) {
        fprintf(stderr, "[MagicTTS][ms] 合成未完成 reason=%d\n", reason);
        return MTTS_ERR_SYNTH;
    }

    int32_t n = 0;
    int16_t *pcm = ms_read_output(c->wavpath, &n);
    FILE *clr = fopen(c->wavpath, "wb");    /* 清空供下次合成 */
    if (clr) fclose(clr);
    if (!pcm || n == 0) {
        fprintf(stderr, "[MagicTTS][ms] 输出文件无音频\n");
        free(pcm);
        return MTTS_ERR_SYNTH;
    }

    (void)stop_gen; (void)my_gen;
    *out_pcm = pcm;
    *out_samples = n;
    *out_sample_rate = 24000;
    return MTTS_OK;
}

static void ms_stop(void *session) {
    ms_ctx *c = (ms_ctx *)session;
    if (c && c->synth && c->stop) c->stop(c->synth);
}

static void ms_destroy(void *session) {
    ms_ctx *c = (ms_ctx *)session;
    if (!c) return;
    if (c->synth) c->release_synth(c->synth);
    if (c->dl) dlclose(c->dl);
    free(c);
}

static const mtts_adapter_if kMsAdapter = {
    ms_probe, ms_create, ms_synthesize, ms_stop, ms_destroy
};
const mtts_adapter_if *mtts_ms_adapter(void) { return &kMsAdapter; }

/* ==== 10-17 排障记录 (供后来者) ====
 * SIGBUS: 句柄按 uint32 截断 → 改 void*。
 * VOICENAME_NOTEXIST: 短名 → 用 Tokens.xml 完整显示名。
 * WRONG_DECRYPTION_KEY: 未传 license → set_voice(cfg, 名, SynthModel.license 全文)。
 * AUDIO_FORMAT_NOT_SUPPORTED: 默认16k → 属性3102(name=NULL)="raw-24khz-16bit-mono-pcm"。
 * result/事件/pull流均 0 字节: 音频走 AudioConfig 目的地 → 临时 wav 文件输出读回
 * (SDK 写的文件头为零, 实为 raw PCM; 需轮询等 flush)。
 * 模型来源: tts2 (Windows NVDA 插件) 的 NeuralVoices 包, 跨平台数据。
 * Windows 侧 SDK=1.45.0, macOS 1.45/1.48 均验证可用。 */
