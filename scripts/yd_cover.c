/* yongde 诊断工具 — include 适配器本体以访问 static 函数 (不与其余 Bridge/*.c 同链).
 * 模式:
 *   yd_cover cover <root> < pinyin输入   数字/er组/全字覆盖
 *   yd_cover phrase <root>               词组值全量音节检查 (失败 token 统计)
 *   yd_cover walk  <root> <text...>      全文行走带日志 (词组/单字/槽位)
 *   yd_cover slotwav <root> <slot...>    导出槽位 PCM 为 /tmp/yd_slot_<n>.wav */
#include "../Bridge/yongde_adapter.c"

static void write_wav(const char *path, const int16_t *pcm, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = (uint32_t)(n * 2);
    uint32_t hdr[] = {0x46464952, 36 + sz, 0x45564157, 0x20746d66, 16,
                      0x00010001, 16000, 32000, 0x00100002, 0x61746164, sz};
    fwrite(hdr, 4, 11, f);
    fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "see header comment\n"); return 1; }
    mtts_engine_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.key = "yongde";
    yd_ctx *c = yd_create(argv[2], &desc);
    if (!c) { fprintf(stderr, "create failed\n"); return 1; }
    const char *mode = argv[1];

    if (strcmp(mode, "slotwav") == 0) {
        for (int i = 3; i < argc; i++) {
            int s = atoi(argv[i]);
            const int16_t *pcm; size_t n;
            if (vl_entry(c, s, &pcm, &n)) {
                char path[64];
                snprintf(path, sizeof(path), "/tmp/yd_slot_%d.wav", s);
                write_wav(path, pcm, n);
                printf("slot %d -> %s (%zu samples)\n", s, path, n);
            } else printf("slot %d: EMPTY\n", s);
        }
        return 0;
    }

    if (strcmp(mode, "phrase") == 0) {
        long vals = 0, toks = 0, fails = 0;
        char seen[64][40] = {0}; long cnt[64] = {0}; int nseen = 0;
        for (int b = 0; b < PH_BUCKETS; b++) {
            for (pnode *nd = c->phrs[b]; nd; nd = nd->next) {
                vals++;
                char tok[32];
                const char *val = nd->val;
                while (*val) {
                    while (*val == ' ' || *val == '\t') val++;
                    if (!*val) break;
                    int n = 0;
                    while (val[n] && val[n] != ' ' && val[n] != '\t' && n < 31) n++;
                    memcpy(tok, val, (size_t)n); tok[n] = 0;
                    val += n ? n : 1;
                    toks++;
                    char plain[32];
                    int tone = detone(tok, plain);
                    if (!plain[0] || syllable_slot(c, plain, tone) < 0) {
                        fails++;
                        int f = -1;
                        for (int k = 0; k < nseen; k++) if (strcmp(seen[k], tok) == 0) f = k;
                        if (f < 0 && nseen < 64) { strcpy(seen[nseen], tok); f = nseen++; }
                        if (f >= 0) cnt[f]++;
                    }
                }
            }
        }
        printf("词组=%ld 音节token=%ld 失败=%ld\n", vals, toks, fails);
        for (int k = 0; k < nseen; k++) printf("  FAIL %-8s ×%ld\n", seen[k], cnt[k]);
        return 0;
    }

    if (strcmp(mode, "walk") == 0) {
        for (int a = 3; a < argc; a++) {
            printf("=== 文本: %s\n", argv[a]);
            const char *p = argv[a];
            pcmbuf out = {0};
            while (*p) {
                if ((unsigned char)*p >= 0xE0) {
                    const char *ends[9]; int nch = 0;
                    const char *q = p;
                    while (nch < 8) { ends[nch] = q; if (!*q) break; utf8_next(&q); nch++; }
                    ends[nch] = q;
                    int hit = 0;
                    for (int k = nch; k >= 2 && !hit; k--) {
                        const char *val = phrase_lookup(c, p, (int)(ends[k] - p));
                        if (val) {
                            printf("  [词组%.*s] %s\n", (int)(ends[k] - p), p, val);
                            const char *v = val;
                            while (*v) {
                                while (*v == ' ' || *v == '\t') v++;
                                if (!*v) break;
                                char tok[32]; int n = 0;
                                while (v[n] && v[n] != ' ' && v[n] != '\t' && n < 31) n++;
                                memcpy(tok, v, (size_t)n); tok[n] = 0; v += n;
                                char plain[32];
                                int tone = detone(tok, plain);
                                int s = syllable_slot(c, plain, tone);
                                printf("    %-6s -> %-5s t%d slot=%d%s\n", tok, plain, tone, s,
                                       s < 0 ? "  ✗✗✗" : "");
                            }
                            emit_pinyin_string(c, &out, val);
                            p = ends[k]; hit = 1;
                        }
                    }
                    if (hit) continue;
                }
                uint32_t cp = utf8_next(&p);
                if (cp >= 0x4E00 && cp <= 0x9FFF || cp >= 0x3400 && cp <= 0x4DBF ||
                    cp == 0x3007 || cp >= 0x20000) {
                    const char *py = char_lookup(c, cp);
                    if (py) {
                        char plain[32];
                        int tone = detone(py, plain);
                        int s = syllable_slot(c, plain, tone);
                        printf("  [字 U+%04X] %-6s -> %-5s t%d slot=%d%s\n", cp, py, plain, tone, s,
                               s < 0 ? "  ✗✗✗" : "");
                        emit_syllable(c, &out, py);
                    } else printf("  [字 U+%04X] ✗✗✗ 字典无条目\n", cp);
                    continue;
                }
                if (cp >= '0' && cp <= '9') {
                    char plain[32];
                    int tone = detone(kDigits[cp - '0'], plain);
                    printf("  [数字 %c] %s -> slot=%d\n", (char)cp, plain,
                           syllable_slot(c, plain, tone));
                    emit_syllable(c, &out, kDigits[cp - '0']);
                    continue;
                }
                printf("  [其他 U+%04X]\n", cp);
            }
            printf("  => 输出 %zu 采样 (%.2f 秒)\n", out.len, out.len / 16000.0);
            free(out.p);
        }
        return 0;
    }

    /* 默认: cover 模式 */
    for (int i = 0; i < 10; i++) {
        char plain[32];
        int tone = detone(kDigits[i], plain);
        printf("DIGIT %d: %-6s -> %-4s t%d slot=%d\n", i, kDigits[i], plain, tone,
               syllable_slot(c, plain, tone));
    }
    for (int s = 1900; s <= 1915; s++) {
        const int16_t *pcm; size_t n;
        printf("IDX %d: %s\n", s, vl_entry(c, s, &pcm, &n) ? "YES" : "-");
    }
    char line[512];
    long tot = 0, miss = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char *sp = line;
        while (*sp && *sp != ' ') sp++;
        char py[64];
        if (sscanf(sp, "%63s", py) != 1) continue;
        char plain[32];
        int tone = detone(py, plain);
        tot++;
        if (syllable_slot(c, plain, tone) < 0) {
            miss++;
            printf("MISS %s -> %s t%d\n", py, plain, tone);
        }
    }
    fprintf(stderr, "TOTAL=%ld MISS=%ld\n", tot, miss);
    return 0;
}
