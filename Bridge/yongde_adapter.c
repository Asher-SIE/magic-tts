/* 永德引擎复刻适配器 — 不再 dlopen 任何 dylib.
 *
 * 背景: Android 版引擎本体编译在 libyongde_tts.so (无 Darwin 版, iOS 亦无自家
 * 实现). 复刻路线 = 破解其编码 + 直接消费其语音数据:
 *   1) 编码公式 (从 .so 0x1c8b4 map-builder 指令流全量提取, 见
 *      scripts/yongde-formula.json / NOTES-yongde-re.md §4):
 *        slot = ZC[声母] + F[韵母] + 声调号   (轻声=0, 一~四声=1..4)
 *      零声母按韵母首字母分类: a/i/u/v类 ZC=0, e类 ZC=1665, o类 ZC=5825.
 *      切分: strip 最长声母(含 y/w), 韵母=剩余原样 (wang=w+ang, yong=y+ong);
 *      归一化仅两条: ü→v, ue→ve. 字母A-Z走特殊槽 8+416k, 静音槽 24.
 *   2) .vl 库 (ydvoice00.vl): 24B 头 + 24576×{u32 offset, u32 length(字节)}
 *      稀疏索引 + 16kHz/16bit/mono PCM 区.
 *   3) 汉字→拼音: mozillazg pinyin.txt (单字, 首读音) + phrases.txt (词组
 *      最长匹配, 多音字纠音) — 与 Android 版随包数据同源.
 *
 * 布局: Engines/yongde/
 *   voices/ydvoice00.vl  pinyin.txt  phrases.txt
 * (scripts/install-engines.sh 自动分发.)
 *
 * 二期: 直接拼接 + 标点停顿 + Sonic 后处理变速变调 (对齐原版
 *   YongDeEngine::processSpeed(pcm, rate) → sonic 的做法):
 *   桥刻度 rate 20..500 (100=常速) → sonicSetSpeed = rate/100 (变速不变调),
 *   pitch 0..100 (50=常音高) → sonicSetPitch = 2^((pitch-50)/50) (±1 八度,
 *   变调不变速). 常速常调时位直通, 不过 Sonic. */
#include "adapters.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sonic.h"

/* ---------- 编码表 (二进制提取, 权威) ---------- */
typedef struct { const char *k; int v; } kv_t;
static const kv_t kI[] = {
    {"b",416},{"c",832},{"d",1248},{"f",2080},{"g",2496},{"h",2912},
    {"j",3744},{"k",4160},{"l",4576},{"m",4992},{"n",5408},{"p",6240},
    {"q",6656},{"r",7072},{"s",7488},{"t",7904},{"zh",8320},{"ch",8736},
    {"sh",3328},{"w",9152},{"x",9568},{"y",9984},{"z",10400},
};
static const kv_t kF[] = {
    {"a",0},{"ai",368},{"an",48},{"ang",288},{"ao",352},
    {"e",64},{"ei",272},{"en",96},{"eng",80},{"er",239},
    {"o",224},{"ou",240},
    {"i",128},{"ia",256},{"ian",32},{"iang",192},{"iao",176},{"ie",304},
    {"iu",384},{"in",144},{"ing",112},{"iong",160},{"ong",160},
    {"u",320},{"ua",256},{"uo",224},{"uai",336},{"ui",16},{"uan",208},
    {"un",400},{"uang",192},
    {"v",336},{"ve",16},{"van",208},{"vn",400},
    {"ng",96},{"n",96},
};
#define ZC_A 0      /* 零声母 a/i/u/v 类 */
#define ZC_E 1665   /* 零声母 e 类 */
#define ZC_O 5825   /* 零声母 o 类 */

/* er 勘误 (2026-10-17): F[er]=239 而非 240 — 240 是 ou 的值, 当初网格解码串读.
 * 佐证: er 组基 1904, 成员 1906/1907/1908 = ér/ěr/èr (+2/+3/+4), +0/+1 空
 * (普通话无 ēr); 1908 为强降调 (F0 实测 213→148Hz) = èr; 用户耳审 1906=ér. */

/* 引擎未录音的音节 → 就近替代 (整组缺失时才触发; 覆盖 pinyin.txt 全部 31 miss) */
static const struct { const char *k, *v; } kFallback[] = {
    {"shei","shui"},{"chua","chuo"},{"biang","bian"},{"den","deng"},
    {"rua","ruo"},{"fiao","biao"},{"cei","ce"},{"din","ding"},{"bong","beng"},
    {"wong","weng"},{"nun","nong"},{"n","en"},{"ng","en"},{"m","mu"},
    {"hm","heng"},{"eng","en"},
};

#define SLOT_SILENCE 24
#define LETTER_SLOT(c) (8 + 416 * ((c) - 'A'))

static int lookup(const kv_t *tab, int n, const char *key, int keylen) {
    for (int i = 0; i < n; i++)
        if ((int)strlen(tab[i].k) == keylen && strncmp(tab[i].k, key, keylen) == 0)
            return tab[i].v;
    return -1;
}

/* ---------- 声调字母 (NFC 预组 + 组合附标) ---------- */
static const struct { uint32_t cp; char base; int tone; } kTone[] = {
    {0x0101,'a',1},{0x00E1,'a',2},{0x01CE,'a',3},{0x00E0,'a',4},
    {0x0113,'e',1},{0x00E9,'e',2},{0x011B,'e',3},{0x00E8,'e',4},
    {0x012B,'i',1},{0x00ED,'i',2},{0x01D0,'i',3},{0x00EC,'i',4},
    {0x014D,'o',1},{0x00F3,'o',2},{0x01D2,'o',3},{0x00F2,'o',4},
    {0x016B,'u',1},{0x00FA,'u',2},{0x01D4,'u',3},{0x00F9,'u',4},
    {0x01D6,'v',1},{0x01D8,'v',2},{0x01DA,'v',3},{0x01DC,'v',4},
    {0x00FC,'v',0},                     /* ü 无调 → v 轻声 */
    {0x0144,'n',2},{0x0148,'n',3},{0x01F9,'n',4},
    {0x1E3F,'m',2},
    {0x00EA,'e',0},                     /* ê → e */
};

/* 拼音 token (如 "zhōng"/"de") → 纯 ASCII 音节 + 声调 (无标记=0 轻声) */
static int detone(const char *tok, char *plain) {
    int tone = 0, o = 0;
    for (const unsigned char *p = (const unsigned char *)tok; *p; ) {
        uint32_t cp = *p;
        if (cp < 0x80) { p++; if (cp >= 'a' && cp <= 'z') plain[o++] = (char)cp; continue; }
        /* UTF-8 解码 */
        int extra = (cp >> 5) == 6 ? 1 : (cp >> 4) == 14 ? 2 : (cp >> 3) == 30 ? 3 : 0;
        if (!extra) { p++; continue; }
        cp &= (uint32_t)(0xFF >> (extra + 1));
        p++;
        while (extra-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
        if (cp >= 0x300 && cp <= 0x304) {     /* 组合附标: ̀ ́ ̄ ̌ */
            int t = cp == 0x300 ? 4 : cp == 0x301 ? 2 : cp == 0x304 ? 1 : 3;
            if (!tone) tone = t;
            continue;
        }
        for (size_t i = 0; i < sizeof(kTone) / sizeof(kTone[0]); i++)
            if (kTone[i].cp == cp) {
                if (o < 30) plain[o++] = kTone[i].base;
                if (!tone) tone = kTone[i].tone;
                break;
            }
        /* 未知多字节 → 丢弃 */
    }
    plain[o] = 0;
    return tone;
}

/* ---------- 字典 (链式哈希) ---------- */
#define CH_BUCKETS (1 << 17)
#define PH_BUCKETS (1 << 16)
#define PH_MAX_BYTES 33

typedef struct cnode { uint32_t cp; char *py; struct cnode *next; } cnode;
typedef struct pnode { char *key; char *val; struct pnode *next; } pnode;

/* ---------- .vl 上下文 ---------- */
typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t nslots;
    const uint32_t *idx;      /* 2*nslots: {offset, length(字节)} */
} vl_file;

/* 发音人条目: voices/ 下每个 .vl 一个 (原版 scanVoices 即扫描 ydvoice%d) */
typedef struct {
    char file[64];     /* "ydvoice01.vl" */
    char key[48];      /* "ydvoice01" (去扩展名, 作选择键) */
    char name[48];     /* "永德·语音1" */
} yd_voice;

#define YD_MAX_VOICES 16

/* 内存流: 整段合成后顺序供样 (永德合成毫秒级, 无需真流式) */
typedef struct {
    int16_t *pcm;
    int32_t n;
    int32_t pos;
    int canceled;
} yd_stream;

typedef struct {
    vl_file vl;
    char vdir[1024];           /* voices 目录绝对路径 */
    char vl_file[64];          /* 当前加载的 .vl 文件名 */
    yd_voice voices[YD_MAX_VOICES];
    int nvoices;
    cnode *chars[CH_BUCKETS];
    pnode *phrs[PH_BUCKETS];
    pthread_mutex_t lock;
} yd_ctx;

/* voices 目录 → 发音人表 (按文件名序; create 与 scan_voices 共用, 纯文件系统) */
static int yd_scan_dir(const char *vdir, yd_voice *out, int max) {
    DIR *d = opendir(vdir);
    if (!d) return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len < 4 || len >= sizeof(out[0].file) || strcmp(e->d_name + len - 3, ".vl") != 0)
            continue;
        snprintf(out[n].file, sizeof(out[n].file), "%s", e->d_name);
        snprintf(out[n].key, sizeof(out[n].key), "%.*s", (int)(len - 3), e->d_name);
        /* 显示名: 文件名里的编号 (ydvoice03 → 永德·语音3), 无数字则用序号 */
        int num = -1;
        for (const char *p = e->d_name; *p; p++)
            if (*p >= '0' && *p <= '9') { num = (int)strtol(p, NULL, 10); break; }
        snprintf(out[n].name, sizeof(out[n].name), "永德·语音%d", num >= 0 ? num : n);
        n++;
    }
    closedir(d);
    for (int i = 1; i < n; i++) {          /* 文件名排序 (个位数级) */
        yd_voice t = out[i];
        int j = i - 1;
        while (j >= 0 && strcmp(out[j].file, t.file) > 0) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return n;
}

static uint8_t *read_all(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

static uint32_t fnv1a(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static int load_pinyin_dict(yd_ctx *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    long n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "U+", 2) != 0) continue;
        uint32_t cp = (uint32_t)strtoul(line + 2, NULL, 16);
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char *p = colon + 1;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p;
        while (*e && *e != ',' && *e != ' ' && *e != '#' && *e != '\n' && *e != '\r') e++;
        if (e == p) continue;
        size_t len = (size_t)(e - p);
        char *py = malloc(len + 1);
        memcpy(py, p, len); py[len] = 0;
        uint32_t b = (cp * 2654435761u) & (CH_BUCKETS - 1);
        cnode *nd = malloc(sizeof(cnode));
        nd->cp = cp; nd->py = py; nd->next = c->chars[b];
        c->chars[b] = nd;
        n++;
    }
    fclose(f);
    fprintf(stderr, "[MagicTTS][yongde] pinyin.txt: %ld 字\n", n);
    return 0;
}

static int load_phrase_dict(yd_ctx *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    long n = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        char *key = line, *ke = colon;
        while (ke > key && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        size_t klen = (size_t)(ke - key);
        if (klen < 3 || klen >= PH_MAX_BYTES) continue;   /* 最短 1 个汉字 */
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        char *ve = val + strlen(val);
        while (ve > val && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ')) ve--;
        if (ve == val) continue;
        char *kcopy = malloc(klen + 1);
        memcpy(kcopy, key, klen); kcopy[klen] = 0;
        size_t vlen = (size_t)(ve - val);
        char *vcopy = malloc(vlen + 1);
        memcpy(vcopy, val, vlen); vcopy[vlen] = 0;
        uint32_t b = fnv1a(kcopy, (int)klen) & (PH_BUCKETS - 1);
        pnode *nd = malloc(sizeof(pnode));
        nd->key = kcopy; nd->val = vcopy; nd->next = c->phrs[b];
        c->phrs[b] = nd;
        n++;
    }
    fclose(f);
    fprintf(stderr, "[MagicTTS][yongde] phrases.txt: %ld 词组\n", n);
    return 0;
}

static const char *phrase_lookup(yd_ctx *c, const char *s, int len) {
    if (len <= 0 || len >= PH_MAX_BYTES) return NULL;
    for (pnode *nd = c->phrs[fnv1a(s, len) & (PH_BUCKETS - 1)]; nd; nd = nd->next)
        if ((int)strlen(nd->key) == len && strncmp(nd->key, s, len) == 0)
            return nd->val;
    return NULL;
}

static const char *char_lookup(yd_ctx *c, uint32_t cp) {
    for (cnode *nd = c->chars[(cp * 2654435761u) & (CH_BUCKETS - 1)]; nd; nd = nd->next)
        if (nd->cp == cp) return nd->py;
    return NULL;
}

/* ---------- PCM 动态缓冲 ---------- */
typedef struct { int16_t *p; size_t len, cap; } pcmbuf;

static void pcm_append(pcmbuf *b, const int16_t *src, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 4096;
        b->p = realloc(b->p, b->cap * sizeof(int16_t));
    }
    memcpy(b->p + b->len, src, n * sizeof(int16_t));
    b->len += n;
}

static int vl_entry(yd_ctx *c, int slot, const int16_t **pcm, size_t *samples) {
    if (slot < 0 || (uint32_t)slot >= c->vl.nslots) return 0;
    uint32_t off = c->vl.idx[2 * slot], bytes = c->vl.idx[2 * slot + 1];
    if (!bytes) return 0;
    if ((size_t)off + bytes > c->vl.size) return 0;
    *pcm = (const int16_t *)(c->vl.data + off);
    *samples = bytes / 2;
    return 1;
}

/* 音节 → 槽; 声调缺失时回退组内任一存在成员 */
static int syllable_slot(yd_ctx *c, const char *plain, int tone) {
    /* strip 最长声母 */
    int ini = -1, ini_len = 0, flen = (int)strlen(plain);
    for (int L = 2; L >= 1; L--) {          /* zh/ch/sh 优先 */
        if (flen > L) {
            int v = lookup(kI, (int)(sizeof(kI) / sizeof(kI[0])), plain, L);
            if (v >= 0) { ini = v; ini_len = L; break; }
        }
    }
    char fin[16];
    int fn = flen - ini_len;
    if (fn <= 0 || fn >= (int)sizeof(fin)) return -1;
    memcpy(fin, plain + ini_len, fn); fin[fn] = 0;
    /* 归一化: ue→ve (jue/que/xue/yue), 拼写变体 */
    if (strcmp(fin, "ue") == 0) strcpy(fin, "ve");
    else if (strcmp(fin, "iou") == 0) strcpy(fin, "iu");
    else if (strcmp(fin, "uei") == 0) strcpy(fin, "ui");
    else if (strcmp(fin, "uen") == 0) strcpy(fin, "un");

    int fv = lookup(kF, (int)(sizeof(kF) / sizeof(kF[0])), fin, (int)strlen(fin));
    if (fv < 0) return -1;
    int base = (ini_len > 0) ? ini + fv
                             : (fin[0] == 'e' ? ZC_E : fin[0] == 'o' ? ZC_O : ZC_A) + fv;
    /* 声调回退: 原调 → 轻声 → 1..4 */
    int order[6] = { tone, 0, 1, 2, 3, 4 };
    const int16_t *pcm; size_t n;
    for (int i = 0; i < 6; i++) {
        int s = base + order[i];
        if (s >= 0 && vl_entry(c, s, &pcm, &n)) return s;
    }
    return -1;
}

static int emit_syllable(yd_ctx *c, pcmbuf *out, const char *tok) {
    char plain[32];
    int tone = detone(tok, plain);
    if (!plain[0]) return 0;
    int s = syllable_slot(c, plain, tone);
    if (s < 0) {
        /* 整组缺失 → 就近音节替代 (shei→shui, 嗯类→en, …) */
        for (int i = 0; i < (int)(sizeof(kFallback) / sizeof(kFallback[0])); i++) {
            if (strcmp(kFallback[i].k, plain) == 0) {
                char alt[32];
                strncpy(alt, kFallback[i].v, sizeof(alt) - 1);
                alt[sizeof(alt) - 1] = 0;
                s = syllable_slot(c, alt, tone);
                break;
            }
        }
    }
    if (s < 0) return 0;
    const int16_t *pcm; size_t n;
    if (!vl_entry(c, s, &pcm, &n)) return 0;
    pcm_append(out, pcm, n);
    return 1;
}

static void emit_silence(yd_ctx *c, pcmbuf *out, int repeats) {
    const int16_t *pcm; size_t n;
    if (!vl_entry(c, SLOT_SILENCE, &pcm, &n)) return;
    for (int i = 0; i < repeats; i++) pcm_append(out, pcm, n);
}

static void emit_pinyin_string(yd_ctx *c, pcmbuf *out, const char *val) {
    char tok[32];
    while (*val) {
        while (*val == ' ' || *val == '\t') val++;
        if (!*val) break;
        int n = 0;
        while (val[n] && val[n] != ' ' && val[n] != '\t' && n < 31) n++;
        if (n) { memcpy(tok, val, n); tok[n] = 0; emit_syllable(c, out, tok); }
        val += n ? n : 1;
    }
}

/* ---------- 文本行走 ---------- */
static const char *kDigits[] = {
    "líng", "yī", "èr", "sān", "sì", "wǔ", "liù", "qī", "bā", "jiǔ"
};

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp = *s++;
    if (cp >= 0xC0) {
        int extra = (cp >> 5) == 6 ? 1 : (cp >> 4) == 14 ? 2 : (cp >> 3) == 30 ? 3 : 0;
        cp &= (uint32_t)(0xFF >> (extra + 1));
        while (extra-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
    }
    *p = (const char *)s;
    return cp;
}

/* 文本 → PCM (锁内执行; 发音人须已加载). 返回 0 成功, out 交由调用方接管. */
static int yd_walk_locked(yd_ctx *c, const char *text, pcmbuf *out) {
    const char *p = text;
    while (*p) {
        /* 词组最长匹配 (最多 8 字) */
        if ((unsigned char)*p >= 0xE0) {
            const char *ends[9]; int nch = 0;
            const char *q = p;
            while (nch < 8) {
                ends[nch] = q;
                if (!*q) break;
                utf8_next(&q);
                nch++;
            }
            ends[nch] = q;
            int hit = 0;
            for (int k = nch; k >= 2 && !hit; k--) {
                int blen = (int)(ends[k] - p);
                const char *val = phrase_lookup(c, p, blen);
                if (val) { emit_pinyin_string(c, out, val); p = ends[k]; hit = 1; }
            }
            if (hit) continue;
        }
        uint32_t cp = utf8_next(&p);
        if (cp >= 0x4E00 && cp <= 0x9FFF || cp >= 0x3400 && cp <= 0x4DBF ||
            cp == 0x3007 || cp >= 0x20000) {
            const char *py = char_lookup(c, cp);
            if (py) emit_syllable(c, out, py);
            continue;
        }
        if (cp >= 'A' && cp <= 'Z' || cp >= 'a' && cp <= 'z' ||
            cp >= 0xFF21 && cp <= 0xFF3A || cp >= 0xFF41 && cp <= 0xFF5A) {
            int up = (cp >= 'a') ? (int)cp - 32
                   : (cp >= 0xFF41) ? (int)cp - 0xFF20
                   : (cp >= 0xFF21) ? (int)cp - 0xFF00 : (int)cp;
            const int16_t *pcm; size_t n;
            if (vl_entry(c, LETTER_SLOT(up), &pcm, &n)) pcm_append(out, pcm, n);
            continue;
        }
        if (cp >= '0' && cp <= '9' || cp >= 0xFF10 && cp <= 0xFF19) {
            int d = (cp >= 0xFF10) ? (int)cp - 0xFF10 : (int)cp - '0';
            emit_syllable(c, out, kDigits[d]);
            continue;
        }
        switch (cp) {
            case 0x3002: case '!': case 0xFF01: case '?': case 0xFF1F: case 0x2026:
                emit_silence(c, out, 2); break;                 /* 。!?… */
            case ',': case 0xFF0C: case 0x3001: case ';': case 0xFF1B:
            case ':': case 0xFF1A: case 0x201C: case 0x201D:
            case 0xFF08: case 0xFF09: case 0x300A: case 0x300B: case 0x2014:
                emit_silence(c, out, 1); break;                 /* ，、；:“”()《》— */
            default: break;                                       /* 其余跳过 */
        }
    }
    return out->p && out->len > 0 ? 0 : -1;
}

/* ---------- 生命周期 ---------- */
static int yd_probe(const char *root, const mtts_engine_desc *desc) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/voices", root, desc->key);
    yd_voice tmp[2];
    if (yd_scan_dir(path, tmp, 2) == 0) return 0;
    snprintf(path, sizeof(path), "%s/%s/pinyin.txt", root, desc->key);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f); else return 0;
    return 1;
}

/* 轻量发音人扫描 (纯文件系统) */
static int yd_scan_voices(const char *root, const mtts_engine_desc *desc,
                          mtts_voice_scan *out, int max) {
    char vdir[1024];
    snprintf(vdir, sizeof(vdir), "%s/%s/voices", root, desc->key);
    yd_voice tmp[YD_MAX_VOICES];
    int n = yd_scan_dir(vdir, tmp, YD_MAX_VOICES);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].key, sizeof(out[i].key), "%s", tmp[i].key);
        snprintf(out[i].name, sizeof(out[i].name), "%s", tmp[i].name);
    }
    return n;
}

/* 加载指定 .vl 到 c->vl (调用方持锁) */
static int yd_load_vl(yd_ctx *c, const char *path) {
    size_t sz = 0;
    uint8_t *data = read_all(path, &sz);
    if (!data || sz < 24 + 8 * 24576) {
        fprintf(stderr, "[MagicTTS][yongde] .vl 缺失或过小: %s\n", path);
        free(data);
        return -1;
    }
    uint32_t hdr[6];
    memcpy(hdr, data, 24);
    if (hdr[0] != 24576 || hdr[2] != 16000) {
        fprintf(stderr, "[MagicTTS][yongde] .vl 头异常 (slots=%u sr=%u): %s\n",
                hdr[0], hdr[2], path);
        free(data);
        return -1;
    }
    free(c->vl.data);
    c->vl.data = data;
    c->vl.size = sz;
    c->vl.nslots = hdr[0];
    c->vl.idx = (const uint32_t *)(c->vl.data + 24);
    if (c->vdir[0]) {
        const char *slash = strrchr(path, '/');
        snprintf(c->vl_file, sizeof(c->vl_file), "%s", slash ? slash + 1 : path);
    }
    return 0;
}

static void *yd_create(const char *root, const mtts_engine_desc *desc) {
    yd_ctx *c = calloc(1, sizeof(yd_ctx));
    char path[1024];

    snprintf(c->vdir, sizeof(c->vdir), "%s/%s/voices", root, desc->key);
    c->nvoices = yd_scan_dir(c->vdir, c->voices, YD_MAX_VOICES);
    if (c->nvoices == 0) {
        fprintf(stderr, "[MagicTTS][yongde] voices/ 下没有 .vl\n");
        free(c);
        return NULL;
    }

    snprintf(path, sizeof(path), "%s/%s", c->vdir, c->voices[0].file);
    if (yd_load_vl(c, path) != 0) {
        free(c->vl.data); free(c);
        return NULL;
    }

    snprintf(path, sizeof(path), "%s/%s/pinyin.txt", root, desc->key);
    if (load_pinyin_dict(c, path) != 0) {
        fprintf(stderr, "[MagicTTS][yongde] pinyin.txt 加载失败\n");
        free(c->vl.data); free(c);
        return NULL;
    }
    snprintf(path, sizeof(path), "%s/%s/phrases.txt", root, desc->key);
    if (load_phrase_dict(c, path) != 0) {
        fprintf(stderr, "[MagicTTS][yongde] phrases.txt 加载失败\n");
        free(c->vl.data); free(c);
        return NULL;
    }
    pthread_mutex_init(&c->lock, NULL);
    size_t entries = 0;
    for (uint32_t k = 0; k < c->vl.nslots; k++) if (c->vl.idx[2 * k + 1]) entries++;
    fprintf(stderr, "[MagicTTS][yongde] 复刻引擎就绪 (%d 发音人, 当前 %s: slots=%u entries=%zu sr=16000)\n",
            c->nvoices, c->voices[0].file, c->vl.nslots, entries);
    return c;
}

/* ---------- 发音人枚举 ---------- */
static int yd_voice_count(void *session) { return ((yd_ctx *)session)->nvoices; }
static const char *yd_voice_key(void *session, int idx) {
    yd_ctx *c = session;
    if (idx < 0 || idx >= c->nvoices) return NULL;
    return c->voices[idx].key;
}
static const char *yd_voice_name(void *session, int idx) {
    yd_ctx *c = session;
    if (idx < 0 || idx >= c->nvoices) return NULL;
    return c->voices[idx].name;
}

/* ---------- 流式合成 ---------- */

/* Sonic 后处理 (对齐原版 processSpeed): 整段 PCM 变速/变调, 原地替换.
 * speed = rate/100 (变速不变调, 0.2..5.0 在 Sonic 钳位 0.05..20 内);
 * pitch 标量 = 2^((pitch-50)/50) → 0.5..2.0 (±1 八度, 变调不变速).
 * 常速常调直接返回 (位直通); Sonic 失败保底输出原始音频. */
static void yd_apply_prosody(pcmbuf *out, int rate, int pitch) {
    if (rate <= 0) rate = 100;
    if (pitch < 0) pitch = 0;
    if (pitch > 100) pitch = 100;
    float speed = rate / 100.0f;
    float pscal = exp2f((pitch - 50) / 50.0f);
    if (speed == 1.0f && pscal == 1.0f) return;

    sonicStream st = sonicCreateStream(16000, 1);
    if (!st) {
        fprintf(stderr, "[MagicTTS][yongde] Sonic 流创建失败, 原样输出\n");
        return;
    }
    sonicSetSpeed(st, speed);
    sonicSetPitch(st, pscal);
    sonicWriteShortToStream(st, out->p, (int)out->len);
    sonicFlushStream(st);

    pcmbuf res = {0};
    short tmp[8192];
    int n;
    while ((n = sonicReadShortFromStream(st, tmp, (int)(sizeof(tmp) / sizeof(tmp[0])))) > 0)
        pcm_append(&res, tmp, (size_t)n);
    sonicDestroyStream(st);

    if (res.p && res.len > 0) {
        free(out->p);
        *out = res;
    } else {
        free(res.p);   /* 空输出 (异常) → 保留原始音频 */
    }
}

static void *yd_stream_start(void *session, const char *voice_key, const char *text,
                             int rate, int pitch, int32_t *out_sample_rate) {
    yd_ctx *c = session;

    pthread_mutex_lock(&c->lock);

    /* 切换发音人: 选择键匹配 (ydvoice01 / ydvoice01.vl 均可) */
    if (voice_key && voice_key[0]) {
        char want[64];
        const char *dot = strrchr(voice_key, '.');
        if (dot && strcmp(dot, ".vl") == 0)
            snprintf(want, sizeof(want), "%.*s", (int)(dot - voice_key), voice_key);
        else
            snprintf(want, sizeof(want), "%s", voice_key);
        for (int i = 0; i < c->nvoices; i++) {
            if (strcmp(c->voices[i].key, want) != 0) continue;
            if (strcmp(c->voices[i].file, c->vl_file) != 0) {
                char path[1100];
                snprintf(path, sizeof(path), "%s/%s", c->vdir, c->voices[i].file);
                if (yd_load_vl(c, path) != 0) {
                    pthread_mutex_unlock(&c->lock);
                    return NULL;
                }
                fprintf(stderr, "[MagicTTS][yongde] 切换发音人 → %s\n", c->voices[i].file);
            }
            break;
        }
    }

    pcmbuf out = {0};
    int rc = yd_walk_locked(c, text, &out);
    pthread_mutex_unlock(&c->lock);
    if (rc != 0) return NULL;
    yd_apply_prosody(&out, rate, pitch);

    yd_stream *s = malloc(sizeof(*s));
    s->pcm = out.p;
    s->n = (int32_t)out.len;
    s->pos = 0;
    s->canceled = 0;
    *out_sample_rate = 16000;
    return s;
}

static int32_t yd_stream_read(void *stream, int16_t *buf, int32_t want, int *done) {
    yd_stream *s = stream;
    if (!s || s->canceled || s->pos >= s->n) {
        if (done) *done = 1;
        return 0;
    }
    int32_t n = s->n - s->pos;
    if (n > want) n = want;
    memcpy(buf, s->pcm + s->pos, (size_t)n * sizeof(int16_t));
    s->pos += n;
    if (done) *done = (s->pos >= s->n) ? 1 : 0;
    return n;
}

static void yd_stream_cancel(void *stream) { ((yd_stream *)stream)->canceled = 1; }

static void yd_stream_free(void *stream) {
    yd_stream *s = stream;
    if (!s) return;
    free(s->pcm);
    free(s);
}

static void yd_destroy(void *session) {
    yd_ctx *c = (yd_ctx *)session;
    if (!c) return;
    for (int b = 0; b < CH_BUCKETS; b++)
        for (cnode *nd = c->chars[b], *nx; nd; nd = nx) { nx = nd->next; free(nd->py); free(nd); }
    for (int b = 0; b < PH_BUCKETS; b++)
        for (pnode *nd = c->phrs[b], *nx; nd; nd = nx) { nx = nd->next; free(nd->key); free(nd->val); free(nd); }
    free(c->vl.data);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

static const mtts_adapter_if kYdAdapter = {
    yd_probe, yd_scan_voices, yd_create,
    yd_voice_count, yd_voice_key, yd_voice_name,
    yd_stream_start, yd_stream_read, yd_stream_cancel, yd_stream_free,
    yd_destroy,
};
const mtts_adapter_if *mtts_yongde_adapter(void) { return &kYdAdapter; }
