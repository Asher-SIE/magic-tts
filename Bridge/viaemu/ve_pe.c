/* viaemu PE32 加载器: 映射节 → 基址重定位 → 导入解析 (指向 thunk 槽) → 导出 */
#include "viaemu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_MODULES 8
static VeModule g_mods[MAX_MODULES];
static int g_nmods;

int ve_region_init(void);

/* IAT 槽位登记 (体检/诊断用) */
typedef struct { uint32_t addr; char dll[16]; char func[128]; } IatEnt;
static IatEnt g_iat[1600];
static int g_niat;

static uint32_t rva2off(uint32_t rva, uint32_t *secmap, int nsec) {
    /* secmap: 每节 [va, vsize, raw, rawsz]×4 */
    for (int i = 0; i < nsec; i++) {
        uint32_t va = secmap[i * 4], vsz = secmap[i * 4 + 1];
        uint32_t raw = secmap[i * 4 + 2], rsz = secmap[i * 4 + 3];
        uint32_t span = vsz > rsz ? vsz : rsz;
        if (rva >= va && rva < va + span) return raw + (rva - va);
    }
    return 0;
}

static uint32_t rd32(const uint8_t *d, uint32_t off) {
    uint32_t v;
    memcpy(&v, d + off, 4);
    return v;
}

/* 加载 PE: 返回基址, 0 失败. want_base 须 64K 对齐. */
uint32_t ve_pe_load(const char *path, uint32_t want_base, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) { ve_log("PE 打不开 %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(fsz);
    if (fread(d, 1, fsz, f) != (size_t)fsz) { fclose(f); free(d); return 0; }
    fclose(f);

    uint32_t pe = rd32(d, 0x3c);
    if (memcmp(d + pe, "PE\0\0", 4)) { ve_log("非 PE: %s", path); free(d); return 0; }
    int nsec = d[pe + 6] | (d[pe + 7] << 8);
    uint32_t optsz = d[pe + 20] | (d[pe + 21] << 8);
    uint32_t opt = pe + 24;
    uint16_t magic = d[opt] | (d[opt + 1] << 8);
    if (magic != 0x10b) { ve_log("非 PE32: %s", path); free(d); return 0; }
    uint32_t image_size = rd32(d, opt + 56);
    uint32_t hdrsz = rd32(d, opt + 60);

    /* 节表 */
    uint32_t secmap[16 * 4];
    if (nsec > 16) nsec = 16;
    uint32_t sec_off = opt + optsz;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = d + sec_off + i * 40;
        /* PE 节头: Name(0..8) VirtualSize(8) VirtualAddress(12) SizeOfRawData(16) PointerToRawData(20) */
        secmap[i * 4 + 0] = rd32(s, 12);   /* va  (VirtualAddress) */
        secmap[i * 4 + 1] = rd32(s, 8);    /* vsize */
        secmap[i * 4 + 2] = rd32(s, 20);   /* raw (PointerToRawData) */
        secmap[i * 4 + 3] = rd32(s, 16);   /* rawsz (SizeOfRawData) */
    }

    /* 映射: 头 + 各节 (未初始化节清零) */
    uint8_t *img = ve_guest_ptr(want_base);
    if (!img) { free(d); return 0; }
    memset(img, 0, (image_size + 0xFFFF) & ~0xFFFFu);
    memcpy(img, d, hdrsz);
    for (int i = 0; i < nsec; i++) {
        uint32_t va = secmap[i * 4], vsz = secmap[i * 4 + 1];
        uint32_t raw = secmap[i * 4 + 2], rsz = secmap[i * 4 + 3];
        if (rsz) memcpy(img + va, d + raw, rsz);
        (void)vsz;
    }

    /* 基址重定位 (IMAGE_REL_BASED_HIGHLOW=3) */
    uint32_t old_base = rd32(d, opt + 28);
    int32_t delta = (int32_t)(want_base - old_base);
    uint32_t reloc_rva = rd32(d, opt + 96 + 5 * 8);
    if (delta && reloc_rva) {
        uint32_t off = rva2off(reloc_rva, secmap, nsec);
        while (off && off < (uint32_t)fsz) {
            uint32_t page = rd32(d, off), bsz = rd32(d, off + 4);
            if (bsz < 8 || off + bsz > (uint32_t)fsz) break;
            for (uint32_t e = off + 8; e < off + bsz; e += 2) {
                uint16_t ent = d[e] | (d[e + 1] << 8);
                if ((ent >> 12) == 3) {
                    uint32_t rva = page + (ent & 0xFFF);
                    uint32_t p = rva2off(rva, secmap, nsec);
                    if (p) {
                        uint32_t v = rd32(d, p);
                        uint32_t *t = (uint32_t *)(img + rva);
                        *t = v + delta;
                    }
                }
            }
            off += bsz;
        }
    }

    /* 导入 → thunk 槽 */
    uint32_t imp_rva = rd32(d, opt + 96 + 1 * 8);
    if (imp_rva) {
        uint32_t off = rva2off(imp_rva, secmap, nsec);
        while (off && off + 20 <= (uint32_t)fsz) {
            uint32_t oft = rd32(d, off), name_rva = rd32(d, off + 12), iat = rd32(d, off + 16);
            if (!name_rva) break;
            uint32_t no = rva2off(name_rva, secmap, nsec);
            char dll[64] = {0};
            for (int i = 0; i < 63 && d[no + i]; i++) dll[i] = d[no + i];
            uint32_t thunk_rva = oft ? oft : iat;
            uint32_t to = rva2off(thunk_rva, secmap, nsec);
            uint32_t iat_p = rva2off(iat, secmap, nsec);
            if (!to || !iat_p) break;
            for (int idx = 0;; idx++) {
                uint32_t v = rd32(d, to + idx * 4);
                if (!v) break;
                char fname[128] = {0};
                if (v & 0x80000000u) {
                    snprintf(fname, sizeof(fname), "#%u", v & 0xFFFF);
                } else {
                    uint32_t ho = rva2off(v, secmap, nsec);
                    for (int i = 0; i < 127 && d[ho + 2 + i]; i++) fname[i] = d[ho + 2 + i];
                }
                void *slot = ve_shim_slot_mod(dll, fname, name);
                uint32_t *iat_entry = (uint32_t *)(img + iat + idx * 4);
                *iat_entry = (uint32_t)(uintptr_t)slot;
                if (g_niat < 1600) {
                    g_iat[g_niat].addr = want_base + iat + idx * 4;
                    snprintf(g_iat[g_niat].dll, 16, "%s", dll);
                    snprintf(g_iat[g_niat].func, 128, "%s", fname);
                    g_niat++;
                }
            }
            off += 20;
        }
    }

    /* 登记 (入口点供 DllMain) */
    for (int i = 0; i < MAX_MODULES; i++) {
        if (!g_mods[i].base) {
            g_mods[i].base = want_base;
            g_mods[i].size = image_size;
            g_mods[i].entry = want_base + rd32(d, opt + 16);
            snprintf(g_mods[i].name, sizeof(g_mods[i].name), "%s", name);
            g_nmods++;
            free(d);
            return want_base;
        }
    }
    free(d);
    return 0;
}

const VeModule *ve_pe_modules(int *count) {
    if (count) *count = g_nmods;
    return g_mods;
}

/* 导出地址 (RVA→映射后绝对地址) */
uint32_t ve_pe_export(uint32_t base, const char *func) {
    uint8_t *img = ve_guest_ptr(base);
    if (!img) return 0;
    uint32_t pe = rd32(img, 0x3c);
    uint32_t opt = pe + 24;
    uint32_t exp_rva = rd32(img, opt + 96);
    if (!exp_rva) return 0;
    uint8_t *e = img + exp_rva;
    uint32_t nnames = rd32(e, 24);
    uint32_t names_rva = rd32(e, 32), ords_rva = rd32(e, 36), funcs_rva = rd32(e, 28);
    uint32_t *names = (uint32_t *)(img + names_rva);
    uint16_t *ords = (uint16_t *)(img + ords_rva);
    uint32_t *funcs = (uint32_t *)(img + funcs_rva);
    for (uint32_t i = 0; i < nnames; i++) {
        const char *n = (const char *)(img + names[i]);
        if (strcmp(n, func) == 0) return base + funcs[ords[i]];
    }
    return 0;
}

/* IAT 体检: 报告不在 thunk 区的槽 (哨兵/未写/旧基址) */
void ve_pe_iat_check(void) {
    for (int i = 0; i < g_niat; i++) {
        uint32_t v = *(uint32_t *)GADDR(g_iat[i].addr);
        extern void *ve_thunk_end_pub(void);
        if (v < 0x0F000000u || v >= 0x0F000000u + 1200u * 8u) {
            ve_log("IAT 异常 @0x%08x %s!%s → 0x%08x", g_iat[i].addr, g_iat[i].dll, g_iat[i].func, v);
        }
    }
}
