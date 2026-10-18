/* Kona 适配器 — 进程内直驱系统内置 Eloquence ("Kona") 引擎.
 *
 * 背景 (NOTES-vvtts.md 第十三/十四节): macOS 26 的 Kona 合成栈在
 * ExtensionKit 扩展 (KonaSynthesizer.appex) 内经 TextToSpeechKonaSupport
 * 的 eci.dylib (ETIThread/SynthThread) 驱动 chs.dylib 等语言引擎; 宿主侧
 * 高层封装 = ObjC 类 AXKonaSpeechEngine (同样编译在 KonaSupport, dyld
 * 缓存内, appex 沙盒可直接 dlopen). 本适配器直接使用该类:
 *   +allVoices → AXKonaVoice (8 发音人 × 14 语言, 含 zh-CN/en-US)
 *   -initWithVoice: / -synthesizeText: / -nextBuffer / -cancelSynthesis
 * 音频 = AXKonaBuffer.buffer (AVAudioPCMBuffer, 引擎原生 16k, Int16/Float).
 *
 * 已验证: AVSpeechSynthesizer.write 在语音提供者 appex 内被系统拒绝
 * (XPC 不通, 双路径实测回调永不达); 本路径纯进程内, 不受影响.
 *
 * 参数映射 (桥刻度 → AXKonaParameters, 参照 KonaVoicePresets.plist:
 * speed 50=常速, pitchBase 65≈男声/81≈女声):
 *   rate   20..500 (100=常速) → speed = rate/2
 *   pitch  0..100  (50=常音高) → pitchBase = 40 + pitch*3/5
 */
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include "adapters.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* —— 静态发音人表 (与 allVoices 的 name+languageCode 对应; 系统组件恒在) —— */
static const struct { const char *key; const char *name; const char *locale; const char *langCode; } kKonaVoices[] = {
    {"eddy-zh",     "Eddy·中文",       "zh-CN", "zh-CN"},
    {"flo-zh",      "Flo·中文",        "zh-CN", "zh-CN"},
    {"grandma-zh",  "奶奶·中文",       "zh-CN", "zh-CN"},
    {"grandpa-zh",  "爷爷·中文",       "zh-CN", "zh-CN"},
    {"reed-zh",     "Reed·中文",       "zh-CN", "zh-CN"},
    {"rocko-zh",    "Rocko·中文",      "zh-CN", "zh-CN"},
    {"sandy-zh",    "Sandy·中文",      "zh-CN", "zh-CN"},
    {"shelley-zh",  "Shelley·中文",    "zh-CN", "zh-CN"},
    {"eddy-en",     "Eddy·English",    "en-US", "en-US"},
    {"flo-en",      "Flo·English",     "en-US", "en-US"},
    {"grandma-en",  "Grandma·English", "en-US", "en-US"},
    {"grandpa-en",  "Grandpa·English", "en-US", "en-US"},
    {"reed-en",     "Reed·English",    "en-US", "en-US"},
    {"rocko-en",    "Rocko·English",   "en-US", "en-US"},
    {"sandy-en",    "Sandy·English",   "en-US", "en-US"},
    {"shelley-en",  "Shelley·English", "en-US", "en-US"},
};
#define KONA_NVOICES ((int)(sizeof(kKonaVoices) / sizeof(kKonaVoices[0])))

static const char *g_kona_root;   /* engines root (scan/create 记下, 供 trace) */

static id KonaEngineCls(void) {
    return NSClassFromString(@"AXKonaSpeechEngine");
}

/* ---- 会话: 类 + 全量发音人缓存 (key → AXKonaVoice) ---- */
typedef struct {
    Class eng;
    NSDictionary *byKey;     /* NSString key → AXKonaVoice */
} kona_session;

/* ---- 流: 一次性合成 (共振峰引擎 ~50-100x 实时) + 内存顺序读 ---- */
typedef struct {
    id engine;               /* AXKonaSpeechEngine (ARC 桥接持有) */
    CFTypeRef retain;        /* 平衡手写 release */
    int16_t *pcm;
    size_t n, pos, cap;
    int rate;
} kona_stream;

static void kona_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void kona_trace(const char *fmt, ...) {
    const char *path = getenv("MT_KONA_TRACE");
    char buf[1024];
    if (!path) {
        if (!g_kona_root) return;
        snprintf(buf, sizeof(buf), "%s/.debug/kona_trace.txt", g_kona_root);
        path = buf;
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* 把 AXKonaBuffer 队列拉平成 int16 PCM */
static int kona_drain(id engine, kona_stream *st) {
    SEL next = NSSelectorFromString(@"nextBuffer");
    SEL getbuf = NSSelectorFromString(@"buffer");
    int pulls = 0;
    for (;;) {
        id kb = [engine performSelector:next];
        if (!kb) break;
        pulls++;
        if ([kb respondsToSelector:getbuf]) {
            id b = [kb performSelector:getbuf];
            if ([b isKindOfClass:[AVAudioPCMBuffer class]]) {
                AVAudioPCMBuffer *pb = (AVAudioPCMBuffer *)b;
                if (pb.frameLength == 0) continue;
                st->rate = (int)pb.format.sampleRate;
                if (st->n + pb.frameLength > st->cap) {
                    st->cap = (st->n + pb.frameLength) * 2;
                    st->pcm = realloc(st->pcm, st->cap * sizeof(int16_t));
                }
                if (pb.floatChannelData && pb.floatChannelData[0]) {
                    const float *p = pb.floatChannelData[0];
                    for (UInt32 i = 0; i < pb.frameLength; i++) {
                        float v = p[i];
                        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
                        st->pcm[st->n++] = (int16_t)(v * 32767.0f);
                    }
                } else if (pb.int16ChannelData && pb.int16ChannelData[0]) {
                    memcpy(st->pcm + st->n, pb.int16ChannelData[0], pb.frameLength * 2);
                    st->n += pb.frameLength;
                }
            }
        }
    }
    return pulls;
}

/* ---- mtts_adapter_if 实现 ---- */
static int kona_probe(const char *root, const mtts_engine_desc *desc) {
    (void)desc;
    if (root) g_kona_root = root;
    /* 类在 dyld 缓存镜像里, dlopen 按安装名即可 (appex 沙盒同样可达) */
    dlopen("/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/TextToSpeechKonaSupport", RTLD_LAZY);
    return KonaEngineCls() != nil;
}

static int kona_scan_voices(const char *root, const mtts_engine_desc *desc,
                            mtts_voice_scan *out, int max) {
    (void)desc;
    if (root) g_kona_root = root;
    int n = KONA_NVOICES < max ? KONA_NVOICES : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].key, sizeof(out[i].key), "%s", kKonaVoices[i].key);
        snprintf(out[i].name, sizeof(out[i].name), "%s (Kona)", kKonaVoices[i].name);
        snprintf(out[i].locale, sizeof(out[i].locale), "%s", kKonaVoices[i].locale);
    }
    return n;
}

static void *kona_create(const char *root, const mtts_engine_desc *desc) {
    (void)desc;
    if (root) g_kona_root = root;
    void *h = dlopen("/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/TextToSpeechKonaSupport", RTLD_LAZY);
    if (!h) { kona_trace("create: dlopen 失败 %s", dlerror()); return NULL; }
    Class eng = KonaEngineCls();
    if (!eng) { kona_trace("create: AXKonaSpeechEngine 类不可用"); return NULL; }
    NSArray *all = [eng performSelector:NSSelectorFromString(@"allVoices")];
    if (![all count]) { kona_trace("create: allVoices 空"); return NULL; }
    NSMutableDictionary *m = [NSMutableDictionary dictionary];
    for (id v in all) {
        NSString *nm = [v performSelector:NSSelectorFromString(@"name")];
        NSString *lc = [v performSelector:NSSelectorFromString(@"languageCode")];
        /* key = "<name 小写>-<语言前缀>" 与静态表对齐 (eddy-zh / rocko-en …);
         * 同名多方言只留首个 (zh-CN 先于 zh-TW 出现, 英文同理) */
        NSString *first = [lc length] >= 2 ? [lc substringToIndex:2] : lc;
        NSString *key = [[NSString stringWithFormat:@"%@-%@", [nm lowercaseString], first] lowercaseString];
        if (![m objectForKey:key]) [m setObject:v forKey:key];
    }
    kona_session *s = malloc(sizeof(*s));
    s->eng = eng;
    s->byKey = [m copy];
    kona_trace("create: %zu 个发音人 (%@)", (size_t)[all count],
               [NSString stringWithFormat:@"%d keys", (int)[m count]]);
    return s;
}

static void kona_destroy(void *session) {
    kona_session *s = session;
    CFRelease((__bridge CFTypeRef)s->byKey);
    free(s);
}

static void *kona_stream_start(void *session, const char *voice_key, const char *text,
                               int rate, int pitch, int32_t *out_sample_rate) {
    kona_session *s = session;
    NSString *key = [NSString stringWithUTF8String:voice_key && voice_key[0] ? voice_key : "eddy-zh"];
    id voice = [s->byKey objectForKey:key];
    if (!voice) voice = [s->byKey objectForKey:@"eddy-zh"];
    if (!voice) { kona_trace("stream_start: 无发音人 %s", key.UTF8String); return NULL; }

    id engine = [[s->eng alloc] performSelector:NSSelectorFromString(@"initWithVoice:")
                                     withObject:voice];
    if (!engine) { kona_trace("stream_start: initWithVoice 失败"); return NULL; }

    /* 参数: KonaVoicePresets.plist 的调音 (AXKonaParameters 属性全为 NSInteger,
     * 必须整数直传 — performSelector withObject 会把 NSNumber 指针当值传);
     * 桥刻度覆盖: rate 20..500(100=常速, 预设 50=常速) → speed=rate/2;
     *             pitch 0..100(50=常音高) → pitchBase 按预设比例缩放 */
    static const struct { const char *name; int eci, pitch, fluct, breath, rough, head, tract, vol; } kPresets[] = {
        {"eddy",     9, 69, 34,  0,  0, 50, 0, 92},
        {"flo",      6, 89, 35, 40,  0, 56, 1, 95},
        {"reed",     1, 65, 30,  0,  0, 50, 0, 92},
        {"rocko",    4, 56, 47,  0,  0, 86, 0, 93},
        {"sandy",    3, 93, 30, 50,  0, 22, 1, 95},
        {"shelley",  2, 81, 30, 50,  0, 50, 1, 95},
        {"grandma",  7, 68, 30, 40,  3, 45, 1, 90},
        {"grandpa",  8, 61, 44, 20, 18, 30, 0, 90},
    };
    const char *vk = voice_key && voice_key[0] ? voice_key : "eddy-zh";
    char base[32]; snprintf(base, sizeof(base), "%s", vk);
    char *dash = strchr(base, '-'); if (dash) *dash = 0;
    const void *ps = &kPresets[0];
    for (unsigned i = 0; i < sizeof(kPresets) / sizeof(kPresets[0]); i++)
        if (strcmp(kPresets[i].name, base) == 0) { ps = &kPresets[i]; break; }
    const struct { const char *name; int eci, pitch, fluct, breath, rough, head, tract, vol; } *P = ps;

    Class PCls = NSClassFromString(@"AXKonaParameters");
    if (PCls) {
        id p = [[PCls alloc] init];
        void (*sl)(id, SEL, long) = (void (*)(id, SEL, long))objc_msgSend;
        int speed = (rate > 0 ? rate : 100) / 2;
        if (speed < 5) speed = 5; else if (speed > 250) speed = 250;
        int pb = P->pitch * (pitch > 0 ? pitch : 50) / 50;
        if (pb < 20) pb = 20; else if (pb > 140) pb = 140;
        sl(p, sel_registerName("setSpeed:"), speed);
        sl(p, sel_registerName("setPitchBase:"), pb);
        sl(p, sel_registerName("setPitchFluctuation:"), P->fluct);
        sl(p, sel_registerName("setBreathiness:"), P->breath);
        sl(p, sel_registerName("setRoughness:"), P->rough);
        sl(p, sel_registerName("setHeadSize:"), P->head);
        sl(p, sel_registerName("setVocalTract:"), P->tract);
        sl(p, sel_registerName("setVolume:"), P->vol);
        [engine performSelector:NSSelectorFromString(@"setParameters:") withObject:p];
    }

    [engine performSelector:NSSelectorFromString(@"synthesizeText:")
                 withObject:[NSString stringWithUTF8String:text]];

    kona_stream *st = calloc(1, sizeof(*st));
    st->engine = engine;
    st->retain = CFBridgingRetain(engine);
    st->rate = 16000;
    int pulls = kona_drain(engine, st);
    if (out_sample_rate) *out_sample_rate = st->rate;
    kona_trace("synth %s: %d 次拉取 → %zu 样本 @%d", key.UTF8String, pulls, st->n, st->rate);
    if (st->n == 0) {
        CFBridgingRelease(st->retain);
        free(st->pcm);
        free(st);
        return NULL;
    }
    return st;
}

static int32_t kona_stream_read(void *stream, int16_t *buf, int32_t want, int *done) {
    kona_stream *st = stream;
    int32_t can = (int32_t)(st->n - st->pos);
    int32_t got = can < want ? can : want;
    if (got > 0) { memcpy(buf, st->pcm + st->pos, got * 2); st->pos += got; }
    if (done) *done = (st->pos >= st->n) ? 1 : 0;
    return got;
}

static void kona_stream_cancel(void *stream) {
    kona_stream *st = stream;
    id e = st ? st->engine : nil;
    [e performSelector:NSSelectorFromString(@"cancelSynthesis")];
}

static void kona_stream_free(void *stream) {
    kona_stream *st = stream;
    if (!st) return;
    CFBridgingRelease(st->retain);
    free(st->pcm);
    free(st);
}

const mtts_adapter_if *mtts_kona_adapter(void) {
    static mtts_adapter_if iface = {
        .probe = kona_probe,
        .scan_voices = kona_scan_voices,
        .create = kona_create,
        .stream_start = kona_stream_start,
        .stream_read = kona_stream_read,
        .stream_cancel = kona_stream_cancel,
        .stream_free = kona_stream_free,
        .destroy = kona_destroy,
    };
    return &iface;
}
