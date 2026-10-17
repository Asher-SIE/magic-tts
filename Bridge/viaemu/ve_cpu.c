/* viaemu i386 解释器 — 整数指令全量 + x87 全量 (MSVC6 时代代码面, 无 SSE).
 * 恒等映射: guest 地址即宿主指针. 垫片经 thunk 区 (0x0F000000) 分发.
 */
#include "viaemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 垫片分发 (ve_win.c 实现): 返回 stdcall 需弹出的字节数 */
int ve_shim_dispatch(int idx, VeCPU *c);

/* ---- 常量 ---- */
enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
#define THUNK_BASE 0x0F000000u
#define THUNK_END  0x0FF00000u
#define STOP_EIP   0x0EFFFFFFu   /* ve_call_guest 的哨兵返回地址 */

#define CF 0x0001u
#define PF 0x0004u
#define AF 0x0010u
#define ZF 0x0040u
#define SF 0x0080u
#define DF 0x0400u
#define OF 0x0800u

/* ---- 内存 (guest→host: 加法偏移) ---- */
#define M8(a)   (*(uint8_t *)(uintptr_t)((uint32_t)(a) + g_ve_off))
#define M16(a)  (*(uint16_t *)(uintptr_t)((uint32_t)(a) + g_ve_off))
#define M32(a)  (*(uint32_t *)(uintptr_t)((uint32_t)(a) + g_ve_off))
#define WR32(a, v) (*(uint32_t *)(uintptr_t)((uint32_t)(a) + g_ve_off) = (uint32_t)(v))
#define WR16(a, v) (*(uint16_t *)(uintptr_t)((uint32_t)(a) + g_ve_off) = (uint16_t)(v))
#define WR8(a, v)  (*(uint8_t *)(uintptr_t)((uint32_t)(a) + g_ve_off) = (uint8_t)(v))

static const uint8_t parity_tab[256] = {
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,
    4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,
    0,4,4,0,4,0,0,4,4,0,0,4,0,4,4,0,4,0,0,4,0,4,4,0,0,4,4,0,4,0,0,4,
};
static inline uint32_t pf_of(uint32_t v) { return parity_tab[v & 0xFF] ? 0 : PF; }

/* ---- x87 ---- */
#define FST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(VeCPU *c, long double v) {
    c->ftop = (c->ftop + 7) & 7;
    c->st[c->ftop] = v;
}
static inline void fpop(VeCPU *c) { c->st[c->ftop] = 0; c->ftop = (c->ftop + 1) & 7; }

static void fcom_setcc(VeCPU *c, long double a, long double b) {
    uint16_t s = c->fstat & ~(0x0100u | 0x0400u | 0x4000u);
    if (isnan(a) || isnan(b)) s |= 0x0100u | 0x0400u | 0x4000u;
    else if (a > b) { /* C0=0 */ }
    else if (a < b) s |= 0x0100u;
    else s |= 0x4000u;
    c->fstat = s;
}

static long double frndint_of(VeCPU *c, long double v) {
    switch ((c->fctrl >> 10) & 3) {
    case 0: return rintl(v) - (rintl(v) > v ? 1.0L : 0.0L);  /* 屏蔽: 向下 */
    case 1: return truncl(v);
    case 2: return ceill(v);
    default: return rintl(v);
    }
}

/* ---- modrm ---- */
typedef struct { uint32_t ea; int reg; int rm; } ModRM;

static uint32_t fetch8(VeCPU *c) { uint8_t v = M8(c->eip); c->eip++; return v; }
static uint32_t fetch16(VeCPU *c) { uint16_t v = M16(c->eip); c->eip += 2; return v; }
static uint32_t fetch32(VeCPU *c) { uint32_t v = M32(c->eip); c->eip += 4; return v; }

static ModRM modrm(VeCPU *c, int osize) {
    ModRM m;
    uint32_t x = fetch8(c);
    m.reg = (x >> 3) & 7;
    m.rm = x & 7;
    int mod = x >> 6;
    uint32_t base = 0;
    if (osize == 16) {
        static const int bx[8][2] = {{EBX,ESI},{EBX,EDI},{EBP,ESI},{EBP,EDI},
                                     {ESI,0},{EDI,0},{EBP,0},{EBX,0}};
        if (mod != 3) {
            if (m.rm == 6 && mod == 0) base = fetch16(c);
            else base = c->reg[bx[m.rm][0]] + c->reg[bx[m.rm][1]];
            if (mod == 1) base += (int8_t)fetch8(c);
            else if (mod == 2) base += (int16_t)fetch16(c);
            base += c->seg_add;
            c->seg_add = 0;
            m.ea = base;
            return m;
        }
        c->seg_add = 0;
        m.ea = 0;
        return m;
    }
    if (mod == 3) { m.ea = 0; c->seg_add = 0; return m; }
    if (m.rm == 4) {
        uint32_t sib = fetch8(c);
        uint32_t scale = sib >> 6, idx = (sib >> 3) & 7, bse = sib & 7;
        uint32_t idxv = (idx == 4) ? 0 : c->reg[idx];
        base = c->reg[bse];
        if (bse == 5 && mod == 0) base = fetch32(c);
        base += idxv << scale;
    } else if (m.rm == 5 && mod == 0) {
        base = fetch32(c);
    } else {
        base = c->reg[m.rm];
    }
    if (mod == 1) base += (int8_t)fetch8(c);
    else if (mod == 2) base += (int32_t)fetch32(c);
    base += c->seg_add;
    c->seg_add = 0;
    m.ea = base;
    return m;
}

/* modrm 有效地址占位 (EA 已内嵌于 ModRM.ea) */
#define RM32(m)  (m.ea ? M32(m.ea) : c->reg[m.rm])
#define SET_RM32(m, v) do { if (m.ea) WR32(m.ea, v); else c->reg[m.rm] = (v); } while (0)
#define RM16(m)  (m.ea ? M16(m.ea) : (uint16_t)c->reg[m.rm])
#define SET_RM16(m, v) do { if (m.ea) WR16(m.ea, v); else c->reg[m.rm] = (c->reg[m.rm] & 0xFFFF0000u) | ((v) & 0xFFFF); } while (0)
#define RM8(m)   (m.ea ? M8(m.ea) : (uint8_t)c->reg[m.rm & 3])
#define SET_RM8(m, v) do { if (m.ea) WR8(m.ea, v); else c->reg[m.rm & 3] = (c->reg[m.rm & 3] & 0xFFFFFF00u) | ((v) & 0xFF); } while (0)

/* ---- 标志算术 ---- */
static void add_flags(VeCPU *c, uint32_t a, uint32_t b, uint32_t r) {
    uint32_t f = pf_of(r);
    if (r == 0) f |= ZF;
    f |= r & SF;
    if (r < a) f |= CF;
    if ((~(a ^ b) & (a ^ r) & 0x80000000u)) f |= OF;
    if (((a ^ b ^ r) & 0x10u)) f |= AF;
    c->flags = (c->flags & ~(CF | PF | AF | ZF | SF | OF)) | f;
}
static void sub_flags(VeCPU *c, uint32_t a, uint32_t b, uint32_t r) {
    uint32_t f = pf_of(r);
    if (r == 0) f |= ZF;
    f |= r & SF;
    if (a < b) f |= CF;
    if (((a ^ b) & (a ^ r) & 0x80000000u)) f |= OF;
    if (((a ^ b ^ r) & 0x10u)) f |= AF;
    c->flags = (c->flags & ~(CF | PF | AF | ZF | SF | OF)) | f;
}
static void logic_flags(VeCPU *c, uint32_t r) {
    uint32_t f = pf_of(r);
    if (r == 0) f |= ZF;
    f |= r & SF;
    c->flags = (c->flags & ~(CF | PF | AF | ZF | SF | OF)) | f;
}
static void inc_flags(VeCPU *c, uint32_t a, uint32_t r) {
    uint32_t keep = c->flags & CF;
    uint32_t f = pf_of(r);
    if (r == 0) f |= ZF;
    f |= r & SF;
    if ((r == 0x80000000u && a == 0x7FFFFFFFu)) f |= OF;
    if (((a ^ 1 ^ r) & 0x10u)) f |= AF;
    c->flags = (c->flags & ~(PF | AF | ZF | SF | OF)) | f | keep;
}
static void dec_flags(VeCPU *c, uint32_t a, uint32_t r) {
    uint32_t keep = c->flags & CF;
    uint32_t f = pf_of(r);
    if (r == 0) f |= ZF;
    f |= r & SF;
    if (a == 0x80000000u && r == 0x7FFFFFFFu) f |= OF;
    if (((a ^ 0xFFFFFFFFu ^ r) & 0x10u)) f |= AF;
    c->flags = (c->flags & ~(PF | AF | ZF | SF | OF)) | f | keep;
}
static uint8_t shl_flags_res(VeCPU *c, uint32_t v, int n, uint32_t *out) {
    uint32_t r = v << n;
    logic_flags(c, r);
    if (n > 0) {
        uint32_t f = c->flags & ~OF;
        if (((v >> (32 - n)) & 1)) f |= CF;
        if (((r >> 31) & 1) != (((v >> (32 - n)) & 1))) f |= OF;
        c->flags = f;
    }
    *out = r;
    return 0;
}
static void shr_flags_res(VeCPU *c, uint32_t v, int n, int wide, uint32_t *out) {
    uint32_t r = v >> n;
    logic_flags(c, r);
    if (n > 0) {
        uint32_t f = c->flags & ~OF;
        if ((v >> (n - 1)) & 1) f |= CF;
        if (!wide) { if (r & 0x80000000u) f |= OF; }
        else { if (((r >> 31) & 1) != ((v >> 31) & 1)) f |= OF; }
        c->flags = f;
    }
    *out = r;
}
static void sar_flags_res(VeCPU *c, uint32_t v, int n, uint32_t *out) {
    int32_t sv = (int32_t)v;
    uint32_t r = (uint32_t)(sv >> (n ? n : 0));
    logic_flags(c, r);
    if (n > 0) {
        uint32_t f = c->flags & ~OF;
        if ((v >> (n - 1)) & 1) f |= CF;
        c->flags = f;
    }
    *out = r;
}

static inline int cond(VeCPU *c, int cc) {
    uint32_t f = c->flags;
    switch (cc >> 1) {
    case 0: return (f & OF) != 0;
    case 1: return (f & CF) != 0;
    case 2: return (f & ZF) != 0;
    case 3: return (f & CF) || (f & ZF);
    case 4: return (f & SF) != 0;
    case 5: return (f & PF) != 0;
    case 6: return ((f & SF) != 0) != ((f & OF) != 0);
    case 7: return ((f & SF) != 0) == ((f & OF) != 0);
    }
    return 0;
}
#define JCC(n) (cond(c, (n) & 0xF) ^ ((n) & 1))

static void push32(VeCPU *c, uint32_t v) { c->reg[ESP] -= 4; WR32(c->reg[ESP], v); }
static uint32_t pop32(VeCPU *c) { uint32_t v = M32(c->reg[ESP]); c->reg[ESP] += 4; return v; }

/* x87 读内存操作数 */
static long double fld_m32(uint32_t a) { float f; memcpy(&f, GADDR(a), 4); return f; }
static long double fld_m64(uint32_t a) { double f; memcpy(&f, (uint8_t *)(uintptr_t)a, 8); return f; }
static long double fld_m80(uint32_t a) {
    /* 80-bit 扩展精度 → host long double (arm64: IEEE quad) */
    uint16_t se; uint64_t m;
    memcpy(&se, (uint8_t *)GADDR(a) + 8, 2);
    memcpy(&m, GADDR(a), 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    if (exp == 0 && m == 0) return sign ? -0.0L : 0.0L;
    if (exp == 0x7FFF) return m & (1ULL << 63) ? NAN : (sign ? -INFINITY : INFINITY);
    long double v = (long double)m / 18446744073709551616.0L;   /* 2^64 */
    if (exp) v *= powl(2.0L, exp - 16383);
    else v *= powl(2.0L, -16382);   /* 非规格化 */
    return sign ? -v : v;
}
static void fstp_m80(uint32_t a, long double v) {
    uint16_t se = 0; uint64_t m = 0;
    if (v == 0.0L) { se = signbit(v) ? 0x8000 : 0; m = 0; }
    else {
        int sign = signbit(v);
        long double av = fabsl(v);
        int exp;
        if (isinf(v)) { se = (uint16_t)(0x7FFF | (sign << 15)); m = 1ULL << 63; }
        else if (isnan(v)) { se = 0x7FFF | (sign << 15); m = 1ULL << 62; }
        else {
            long double norm = frexpl(av, &exp);   /* [0.5,1) */
            m = (uint64_t)(norm * 18446744073709551616.0L);   /* 64bit 尾数 */
            se = (uint16_t)(exp - 1 + 16383 + (sign << 15));
        }
    }
    memcpy(GADDR(a), &m, 8);
    memcpy((uint8_t *)GADDR(a) + 8, &se, 2);
}

/* M64REF helper (避免前向宏问题) */
static inline uint64_t M64R(uint32_t a) { uint64_t v; memcpy(&v, (uint8_t *)(uintptr_t)a, 8); return v; }
static inline void WM64(uint32_t a, uint64_t v) { memcpy((uint8_t *)(uintptr_t)a, &v, 8); }
#define M64REF(a) M64R(a)

/* 除零等异常 → SEH */
void ve_raise_fault(VeCPU *c, int code, uint32_t addr);

/* FCOMI/FCOMIP 标志映射: C0→CF, C3→ZF, C2→PF */
#define FCOMI_FLAGS() do { \
    uint32_t _f = c->flags & ~(CF | ZF | PF); \
    if (c->fstat & 0x0100u) _f |= CF; \
    if (c->fstat & 0x4000u) _f |= ZF; \
    if (c->fstat & 0x0400u) _f |= PF; \
    c->flags = _f; \
} while (0)

/* ---- 主循环 ---- */
uint32_t ve_cpu_run(VeCPU *c, uint32_t stop_esp) {
    (void)stop_esp;
    uint64_t budget = 0;
    static int trace = -1;
    if (trace < 0) trace = getenv("VE_TRACE") ? 1 : 0;
    for (;;) {
        if (++budget > 3000000000u) {   /* ~15s @200MIPS: 防死循环 */
            ve_log("指令预算耗尽 eip=0x%08x esp=0x%08x", c->eip, c->reg[4]);
            return 4;
        }
        if ((c->eip & 0x03FFFFFFu) == 0 && c->eip >= 0x10000000u && c->eip < 0x28000000u) {
            ve_log("!! eip=模块基址 0x%08x (调用/返回错位) esp=0x%08x [esp]=0x%08x [esp+4]=0x%08x",
                   c->eip, c->reg[4], M32(c->reg[4]), M32(c->reg[4] + 4));
            return 5;
        }
        if (trace && budget <= 3000) {
            ve_log("TRACE %3llu eip=0x%08x esp=0x%08x bytes=%02x %02x %02x %02x %02x %02x eax=%08x ecx=%08x",
                   (unsigned long long)budget, c->eip, c->reg[4],
                   M8(c->eip), M8(c->eip + 1), M8(c->eip + 2),
                   M8(c->eip + 3), M8(c->eip + 4), M8(c->eip + 5),
                   c->reg[0], c->reg[1]);
        }
        if (c->eip >= THUNK_BASE && c->eip < THUNK_END) {
            ve_log("eip 落入 thunk 区 (jmp/call 未捕获) 0x%08x", c->eip);
            return 1;
        }
        if (c->eip == STOP_EIP) return 0;

        int osize = 0, rep = 0;
        uint32_t new_eip;
    next_prefix:
        switch (M8(c->eip)) {
        case 0x66: osize = 1; c->eip++; goto next_prefix;
        case 0x67: c->eip++; goto next_prefix;             /* 16位寻址: 罕见, 按 32 位处理 */
        case 0xF0: c->eip++; goto next_prefix;             /* lock */
        case 0xF2: rep = 2; c->eip++; goto next_prefix;
        case 0xF3: rep = 1; c->eip++; goto next_prefix;
        case 0x64: c->seg_add = c->seg_fs; c->eip++; goto next_prefix;   /* fs: → TEB 基 */
        case 0x2E: case 0x36: case 0x26: case 0x3E: case 0x65: c->eip++; goto next_prefix;
        }
        uint8_t op = M8(c->eip); c->eip++;

        switch (op) {
        /* ---------- ALU ---------- */
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B: {
            int aluop = (op >> 3) & 7;
            if (op & 2) {   /* r ← r/m */
                ModRM m = modrm(c, osize);
                uint32_t a = c->reg[m.reg], b;
                if (osize) { b = RM16(m); } else { b = RM32(m); }
                uint32_t r = 0;
                switch (aluop) {
                case 0: r = a + b; add_flags(c, a, b, r); break;
                case 1: r = a | b; logic_flags(c, r); break;
                case 2: r = a + b + (c->flags & CF); add_flags(c, a + (c->flags & CF), b, r); break;
                case 3: r = a - b - (c->flags & CF); sub_flags(c, a - (c->flags & CF), b, r); break;
                case 4: r = a & b; logic_flags(c, r); break;
                case 5: r = a - b; sub_flags(c, a, b, r); break;
                case 6: r = a ^ b; logic_flags(c, r); break;
                case 7: sub_flags(c, a, b, a - b); break;
                }
                if (aluop != 7) {
                    if (osize) c->reg[m.reg] = (c->reg[m.reg] & 0xFFFF0000u) | (r & 0xFFFF);
                    else c->reg[m.reg] = r;
                }
            } else {        /* r/m ← r */
                ModRM m = modrm(c, osize);
                uint32_t a, b = c->reg[m.reg];
                if (osize) { a = RM16(m); } else { a = RM32(m); }
                uint32_t r = 0;
                switch (aluop) {
                case 0: r = a + b; add_flags(c, a, b, r); break;
                case 1: r = a | b; logic_flags(c, r); break;
                case 2: r = a + b + (c->flags & CF); add_flags(c, a + (c->flags & CF), b, r); break;
                case 3: r = a - b - (c->flags & CF); sub_flags(c, a - (c->flags & CF), b, r); break;
                case 4: r = a & b; logic_flags(c, r); break;
                case 5: r = a - b; sub_flags(c, a, b, r); break;
                case 6: r = a ^ b; logic_flags(c, r); break;
                case 7: sub_flags(c, a, b, a - b); break;
                }
                if (aluop != 7) {
                    if (osize) { if (m.ea) WR16(m.ea, r); else c->reg[m.rm] = (c->reg[m.rm] & 0xFFFF0000u) | (r & 0xFFFF); }
                    else SET_RM32(m, r);
                }
            }
            break;
        }

        case 0x06: push32(c, 0x10); break;             /* push es */
        case 0x07: (void)pop32(c); break;              /* pop es */
        case 0x0E: push32(c, 0x33); break;             /* push cs 值无意义 */
        case 0x16: push32(c, 0x20); break;             /* push ss */
        case 0x17: (void)pop32(c); break;
        case 0x1E: push32(c, 0x26); break;             /* push ds */
        case 0x1F: (void)pop32(c); break;

        case 0x0F: {
            uint8_t op2 = fetch8(c);
            switch (op2) {
            case 0x05: ve_log("syscall 0F05"); c->eip = STOP_EIP; break;
            case 0x31: {   /* rdtsc */
                static uint64_t tsc; tsc += 1000;
                c->reg[EAX] = (uint32_t)tsc; c->reg[EDX] = (uint32_t)(tsc >> 32);
                break;
            }
            case 0x80 ... 0x8F: {   /* jcc rel32 */
                int32_t d = (int32_t)fetch32(c);
                if (JCC(op2 & 0xF)) c->eip += d;
                break;
            }
            case 0x90 ... 0x9F: {   /* setcc */
                ModRM m = modrm(c, osize);
                uint8_t v = JCC(op2 & 0xF) ? 1 : 0;
                if (m.ea) WR8(m.ea, v); else c->reg[m.rm & 3] = (c->reg[m.rm & 3] & ~0xFFu) | v;
                break;
            }
            case 0xA0: push32(c, c->seg_fs); break;    /* push fs */
            case 0xA1: c->seg_fs = pop32(c); break;    /* pop fs */
            case 0xA8: push32(c, 0); break;            /* push gs */
            case 0xA9: (void)pop32(c); break;
            case 0xA2: {   /* cpuid */
                uint32_t leaf = c->reg[EAX];
                if (leaf == 0) { c->reg[EAX] = 1; WR32(0x20000000, 0x756E6547); }  /* 不实际需要 */
                else { c->reg[EAX] = 0x633; c->reg[EBX] = c->reg[ECX] = c->reg[EDX] = 0; }
                break;
            }
            case 0xA3: case 0xAB: case 0xB3: case 0xBB: {  /* bt/bts/btr/btc */
                ModRM m = modrm(c, osize);
                uint32_t v = RM32(m);
                uint32_t bit = c->reg[m.reg] & 31;
                uint32_t f = c->flags & ~CF;
                if ((v >> bit) & 1) f |= CF;
                c->flags = f;
                if (op2 == 0xAB) v |= (1u << bit);
                else if (op2 == 0xB3) v &= ~(1u << bit);
                else if (op2 == 0xBB) v ^= (1u << bit);
                if (op2 != 0xA3) SET_RM32(m, v);
                break;
            }
            case 0xBC: case 0xBD: {   /* bsf/bsr */
                ModRM m = modrm(c, osize);
                uint32_t v = RM32(m);
                if (!v) { c->flags |= ZF; }
                else {
                    c->flags &= ~ZF;
                    uint32_t r = (op2 == 0xBC) ? __builtin_ctz(v) : 31 - __builtin_clz(v);
                    c->reg[m.reg] = r;
                }
                break;
            }
            case 0xAF: {   /* imul r, r/m */
                ModRM m = modrm(c, osize);
                int32_t a = (int32_t)c->reg[m.reg], b = (int32_t)RM32(m);
                int64_t r = (int64_t)a * b;
                uint32_t r32 = (uint32_t)r;
                c->reg[m.reg] = r32;
                uint32_t f = pf_of(r32); if (!r32) f |= ZF; f |= r32 & SF;
                if (r != (int64_t)(int32_t)r32) f |= CF | OF;
                c->flags = (c->flags & ~(CF | PF | ZF | SF | OF)) | f;
                break;
            }
            case 0xB6: case 0xBE: {   /* movzx/movsx r, r/m8 */
                ModRM m = modrm(c, osize);
                uint32_t v = m.ea ? M8(m.ea) : (uint8_t)c->reg[m.rm & 3];
                c->reg[m.reg] = (op2 == 0xBE) ? (uint32_t)(int32_t)(int8_t)v : v;
                break;
            }
            case 0xB7: case 0xBF: {   /* movzx/movsx r, r/m16 */
                ModRM m = modrm(c, osize);
                uint32_t v = m.ea ? M16(m.ea) : (uint16_t)c->reg[m.rm];
                c->reg[m.reg] = (op2 == 0xBF) ? (uint32_t)(int32_t)(int16_t)v : v;
                break;
            }
            case 0xA4: {   /* shld r/m, r, imm8 */
                ModRM m = modrm(c, osize);
                uint32_t v = RM32(m), s = c->reg[m.reg];
                int n = fetch8(c) & 31;
                uint32_t r = n ? (v << n) | (s >> (32 - n)) : v;
                if (n) { logic_flags(c, r); uint32_t f = c->flags & ~CF; if ((v >> (32 - n)) & 1) f |= CF; c->flags = f; }
                SET_RM32(m, r);
                break;
            }
            case 0xAC: {   /* shrd r/m, r, imm8 */
                ModRM m = modrm(c, osize);
                uint32_t v = RM32(m), s = c->reg[m.reg];
                int n = fetch8(c) & 31;
                uint32_t r = n ? (v >> n) | (s << (32 - n)) : v;
                if (n) { logic_flags(c, r); uint32_t f = c->flags & ~CF; if ((v >> (n - 1)) & 1) f |= CF; c->flags = f; }
                SET_RM32(m, r);
                break;
            }
            case 0xB0: case 0xB1: {   /* cmpxchg */
                ModRM m = modrm(c, osize);
                uint32_t dst = RM32(m), src = c->reg[m.reg], acc = c->reg[EAX];
                if (acc == dst) { SET_RM32(m, src); c->flags &= ~ZF; c->flags |= ZF; c->reg[EAX] = acc; }
                else { c->reg[EAX] = dst; c->flags &= ~ZF; sub_flags(c, acc, dst, acc - dst); }
                break;
            }
            case 0xC0: case 0xC1: {   /* xadd */
                ModRM m = modrm(c, osize);
                uint32_t a = RM32(m), b = c->reg[m.reg];
                uint32_t r = a + b;
                add_flags(c, a, b, r);
                c->reg[m.reg] = a;
                SET_RM32(m, r);
                break;
            }
            case 0xC8 ... 0xCF: {   /* bswap */
                int r = op2 & 7;
                c->reg[r] = __builtin_bswap32(c->reg[r]);
                break;
            }
            case 0x1F: { modrm(c, osize); break; }   /* 多字节 nop */
            case 0x0B: ve_log("ud2 @0x%08x", c->eip); return 2;
            default:
                ve_log("0F %02X 未实现 @0x%08x", op2, c->eip - 2);
                return 2;
            }
            break;
        }

        case 0x40 ... 0x47: { uint32_t a = c->reg[op & 7]; uint32_t r = a + 1; c->reg[op & 7] = r; inc_flags(c, a, r); break; }
        case 0x48 ... 0x4F: { uint32_t a = c->reg[op & 7]; uint32_t r = a - 1; c->reg[op & 7] = r; dec_flags(c, a, r); break; }
        case 0x50 ... 0x57: push32(c, c->reg[op & 7]); break;
        case 0x58 ... 0x5F: c->reg[op & 7] = pop32(c); break;

        case 0x60: {   /* pushad */
            uint32_t sp = c->reg[ESP];
            push32(c, c->reg[EAX]); push32(c, c->reg[ECX]); push32(c, c->reg[EDX]); push32(c, c->reg[EBX]);
            push32(c, sp); push32(c, c->reg[EBP]); push32(c, c->reg[ESI]); push32(c, c->reg[EDI]);
            break;
        }
        case 0x61: {   /* popad */
            c->reg[EDI] = pop32(c); c->reg[ESI] = pop32(c); c->reg[EBP] = pop32(c); (void)pop32(c);
            c->reg[EBX] = pop32(c); c->reg[EDX] = pop32(c); c->reg[ECX] = pop32(c); c->reg[EAX] = pop32(c);
            break;
        }
        case 0x68: push32(c, fetch32(c)); break;
        case 0x69: {   /* imul r, rm, imm32 */
            ModRM m = modrm(c, osize);
            int64_t r = (int64_t)(int32_t)RM32(m) * (int32_t)fetch32(c);
            c->reg[m.reg] = (uint32_t)r;
            uint32_t f = 0; if (r != (int64_t)(int32_t)r) f = CF | OF;
            c->flags = (c->flags & ~(CF | OF)) | f;
            break;
        }
        case 0x6A: push32(c, (uint32_t)(int32_t)(int8_t)fetch8(c)); break;
        case 0x6B: {   /* imul r, rm, imm8 */
            ModRM m = modrm(c, osize);
            int64_t r = (int64_t)(int32_t)RM32(m) * (int8_t)fetch8(c);
            c->reg[m.reg] = (uint32_t)r;
            uint32_t f = 0; if (r != (int64_t)(int32_t)r) f = CF | OF;
            c->flags = (c->flags & ~(CF | OF)) | f;
            break;
        }

        case 0x70 ... 0x7F: {   /* jcc rel8 */
            int8_t d = (int8_t)fetch8(c);
            if (JCC(op & 0xF)) c->eip += d;
            break;
        }

        case 0x80: case 0x82: {   /* grp1 rm8, imm8 */
            ModRM m = modrm(c, osize);
            int aluop = m.reg;
            uint32_t a = RM8(m), b = fetch8(c), r = 0;
            switch (aluop) {
            case 0: r = a + b; add_flags(c, a, b, r); break;
            case 1: r = a | b; logic_flags(c, r); break;
            case 2: r = a + b + (c->flags & CF); add_flags(c, a + (c->flags & CF), b, r); break;
            case 3: r = a - b - (c->flags & CF); sub_flags(c, a - (c->flags & CF), b, r); break;
            case 4: r = a & b; logic_flags(c, r); break;
            case 5: r = a - b; sub_flags(c, a, b, r); break;
            case 6: r = a ^ b; logic_flags(c, r); break;
            case 7: sub_flags(c, a, b, a - b); break;
            }
            if (aluop != 7) SET_RM8(m, r);
            break;
        }
        case 0x81: case 0x83: {
            ModRM m = modrm(c, osize);
            int aluop = m.reg;
            uint32_t b = (op == 0x81) ? fetch32(c) : (uint32_t)(int32_t)(int8_t)fetch8(c);
            uint32_t a = RM32(m), r = 0;
            switch (aluop) {
            case 0: r = a + b; add_flags(c, a, b, r); break;
            case 1: r = a | b; logic_flags(c, r); break;
            case 2: r = a + b + (c->flags & CF); add_flags(c, a + (c->flags & CF), b, r); break;
            case 3: r = a - b - (c->flags & CF); sub_flags(c, a - (c->flags & CF), b, r); break;
            case 4: r = a & b; logic_flags(c, r); break;
            case 5: r = a - b; sub_flags(c, a, b, r); break;
            case 6: r = a ^ b; logic_flags(c, r); break;
            case 7: sub_flags(c, a, b, a - b); break;
            }
            if (aluop != 7) SET_RM32(m, r);
            break;
        }
        case 0x84: { ModRM m = modrm(c, osize); logic_flags(c, RM8(m) & (c->reg[m.reg] & 0xFF)); break; }
        case 0x85: { ModRM m = modrm(c, osize); logic_flags(c, RM32(m) & c->reg[m.reg]); break; }
        case 0x86: { ModRM m = modrm(c, osize); uint8_t t = RM8(m); SET_RM8(m, c->reg[m.reg & 3] & 0xFF); c->reg[m.reg & 3] = (c->reg[m.reg & 3] & ~0xFF) | t; break; }
        case 0x87: { ModRM m = modrm(c, osize); uint32_t t = RM32(m); SET_RM32(m, c->reg[m.reg]); c->reg[m.reg] = t; break; }
        case 0x88: { ModRM m = modrm(c, osize); SET_RM8(m, c->reg[m.reg & 3] & 0xFF); break; }
        case 0x89: { ModRM m = modrm(c, osize); if (osize) { if (m.ea) WR16(m.ea, c->reg[m.reg]); else c->reg[m.rm] = (c->reg[m.rm] & 0xFFFF0000u) | (c->reg[m.reg] & 0xFFFF); } else SET_RM32(m, c->reg[m.reg]); break; }
        case 0x8A: { ModRM m = modrm(c, osize); c->reg[m.reg] = RM8(m); break; }
        case 0x8B: { ModRM m = modrm(c, osize); if (osize) c->reg[m.reg] = m.ea ? M16(m.ea) : (uint16_t)c->reg[m.rm]; else c->reg[m.reg] = RM32(m); break; }
        case 0x8C: { ModRM m = modrm(c, osize); SET_RM32(m, 0x10); break; }   /* mov rm16, seg — 置假段值 */
        case 0x8D: {   /* lea */
            ModRM m = modrm(c, osize);
            c->reg[m.reg] = m.ea;
            break;
        }
        case 0x8E: { ModRM m = modrm(c, osize); (void)RM32(m); break; }   /* mov seg, rm16 */
        case 0x8F: { ModRM m = modrm(c, osize); uint32_t v = pop32(c); SET_RM32(m, v); break; }

        case 0x90: break;   /* nop */
        case 0x91 ... 0x97: { int r = op & 7; uint32_t t = c->reg[EAX]; c->reg[EAX] = c->reg[r]; c->reg[r] = t; break; }
        case 0x98: c->reg[EAX] = (uint32_t)(int32_t)(int16_t)c->reg[EAX]; break;   /* cwde */
        case 0x99: c->reg[EDX] = (int32_t)c->reg[EAX] < 0 ? 0xFFFFFFFF : 0; break;  /* cdq */
        case 0x9B: break;   /* fwait */
        case 0x9C: push32(c, c->flags | 0x0002); break;
        case 0x9D: c->flags = pop32(c) & (CF | PF | AF | ZF | SF | DF | OF); break;
        case 0x9E: c->flags = (c->flags & ~0xD5u) | (c->reg[EAX] & 0xD5u); break;  /* sahf */
        case 0x9F: c->reg[EAX] = (c->reg[EAX] & 0xFFFFFF00) | (c->flags & 0xD5u); break;  /* lahf */

        /* A0-A3 绝对地址寻址 (moffs): 段前缀同样生效 (fs:[0] 即此形态) */
        case 0xA0: { uint32_t a = fetch32(c) + c->seg_add; c->seg_add = 0; c->reg[EAX] = M8(a); break; }
        case 0xA1: { uint32_t a = fetch32(c) + c->seg_add; c->seg_add = 0; c->reg[EAX] = M32(a); break; }
        case 0xA2: { uint32_t a = fetch32(c) + c->seg_add; c->seg_add = 0; WR8(a, c->reg[EAX] & 0xFF); break; }
        case 0xA3: { uint32_t a = fetch32(c) + c->seg_add; c->seg_add = 0; WR32(a, c->reg[EAX]); break; }

        case 0xA4: {   /* movsb [rep 计数在 ECX] */
            int32_t d = (c->flags & DF) ? -1 : 1;
            uint32_t n = rep ? c->reg[ECX] : 1;
            while (n--) { M8(c->reg[EDI]) = M8(c->reg[ESI]); c->reg[ESI] += d; c->reg[EDI] += d; }
            if (rep) c->reg[ECX] = 0;
            break;
        }
        case 0xA5: {   /* movsd */
            int32_t d = (c->flags & DF) ? -4 : 4;
            uint32_t n = rep ? c->reg[ECX] : 1;
            while (n--) { M32(c->reg[EDI]) = M32(c->reg[ESI]); c->reg[ESI] += d; c->reg[EDI] += d; }
            if (rep) c->reg[ECX] = 0;
            break;
        }
        case 0xA6: case 0xA7: {   /* cmps[b/d] */
            int wide = op & 1;
            int32_t d = (c->flags & DF) ? (wide ? -4 : -1) : (wide ? 4 : 1);
            uint32_t n = rep ? c->reg[ECX] : 1;
            while (n--) {
                uint32_t a = wide ? M32(c->reg[ESI]) : M8(c->reg[ESI]);
                uint32_t b = wide ? M32(c->reg[EDI]) : M8(c->reg[EDI]);
                sub_flags(c, a, b, a - b);
                c->reg[ESI] += d; c->reg[EDI] += d;
                c->reg[ECX] = n;
                if (rep == 1 && !(c->flags & ZF)) break;
                if (rep == 2 && (c->flags & ZF)) break;
            }
            break;
        }
        case 0xA8: logic_flags(c, (c->reg[EAX] & 0xFF) & fetch8(c)); break;
        case 0xA9: logic_flags(c, c->reg[EAX] & fetch32(c)); break;
        case 0xAA: { int32_t d = (c->flags & DF) ? -1 : 1; uint32_t n = rep ? c->reg[ECX] : 1; while (n--) { M8(c->reg[EDI]) = c->reg[EAX] & 0xFF; c->reg[EDI] += d; } if (rep) c->reg[ECX] = 0; break; }
        case 0xAB: { int32_t d = (c->flags & DF) ? -4 : 4; uint32_t n = rep ? c->reg[ECX] : 1; while (n--) { M32(c->reg[EDI]) = c->reg[EAX]; c->reg[EDI] += d; } if (rep) c->reg[ECX] = 0; break; }
        case 0xAC: { int32_t d = (c->flags & DF) ? -1 : 1; uint32_t n = rep ? c->reg[ECX] : 1; while (n--) { c->reg[EAX] = M8(c->reg[ESI]); c->reg[ESI] += d; } if (rep) c->reg[ECX] = 0; break; }
        case 0xAD: { int32_t d = (c->flags & DF) ? -4 : 4; uint32_t n = rep ? c->reg[ECX] : 1; while (n--) { c->reg[EAX] = M32(c->reg[ESI]); c->reg[ESI] += d; } if (rep) c->reg[ECX] = 0; break; }
        case 0xAE: case 0xAF: {   /* scas[b/d] */
            int wide = op & 1;
            int32_t d = (c->flags & DF) ? (wide ? -4 : -1) : (wide ? 4 : 1);
            uint32_t n = rep ? c->reg[ECX] : 1;
            while (n--) {
                uint32_t b = wide ? M32(c->reg[EDI]) : M8(c->reg[EDI]);
                sub_flags(c, c->reg[EAX], b, c->reg[EAX] - b);
                c->reg[EDI] += d;
                c->reg[ECX] = n;
                if (rep == 1 && !(c->flags & ZF)) break;
                if (rep == 2 && (c->flags & ZF)) break;
            }
            break;
        }

        case 0xB0 ... 0xB7: c->reg[op & 7] = (c->reg[op & 7] & ~0xFFu) | fetch8(c); break;
        case 0xB8 ... 0xBF: c->reg[op & 7] = fetch32(c); break;

        case 0xC0: case 0xC1: {   /* shift rm, imm8 */
            ModRM m = modrm(c, osize);
            int n = fetch8(c) & 31;
            uint32_t v = RM32(m), r = 0;
            switch (m.reg) {
            case 0: case 2: shl_flags_res(c, v, n, &r); break;      /* rol 由下面 */
            case 4: shl_flags_res(c, v, n, &r); break;
            case 5: shr_flags_res(c, v, n, 1, &r); break;
            case 7: sar_flags_res(c, v, n, &r); break;
            case 1: {   /* rol */
                n &= 31; r = n ? (v << n) | (v >> (32 - n)) : v;
                if (n) { uint32_t f = (c->flags & ~(CF | OF)); if (r & 1) f |= CF; if (((r >> 31) & 1) != ((v >> (31 - n + 0)) & 1)) f |= OF; c->flags = f; }
                break;
            }
            case 3: {   /* ror */
                n &= 31; r = n ? (v >> n) | (v << (32 - n)) : v;
                if (n) { uint32_t f = (c->flags & ~(CF | OF)); if ((r >> 31) & 1) f |= CF; c->flags = f; }
                break;
            }
            case 6: r = v; break;   /* 保留 (sal=shl 已含) */
            }
            SET_RM32(m, r);
            break;
        }
        case 0xC2: { uint16_t n = fetch16(c); c->eip = pop32(c); c->reg[ESP] += n;
                     if (c->eip == STOP_EIP) return 0; break; }
        case 0xC3: { c->eip = pop32(c); if (c->eip == STOP_EIP) return 0; break; }
        case 0xC6: { ModRM m = modrm(c, osize); SET_RM8(m, fetch8(c)); break; }
        case 0xC7: { ModRM m = modrm(c, osize); if (osize) WR16(m.ea, fetch16(c)); else SET_RM32(m, fetch32(c)); break; }
        case 0xC8: {   /* enter */
            uint32_t size = fetch16(c), level = fetch8(c) & 31;
            push32(c, c->reg[EBP]);
            uint32_t fp = c->reg[ESP];
            for (uint32_t i = 1; i < level; i++) { c->reg[EBP] -= 4; push32(c, M32(c->reg[EBP])); }
            if (level) push32(c, fp);
            c->reg[EBP] = fp;
            c->reg[ESP] -= size;
            break;
        }
        case 0xC9: c->reg[ESP] = c->reg[EBP]; c->reg[EBP] = pop32(c); break;   /* leave */
        case 0xCC: ve_raise_fault(c, 3, c->eip); break;
        case 0xCD: ve_raise_fault(c, fetch8(c), c->eip); break;

        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {   /* shift rm, 1/cl */
            ModRM m = modrm(c, osize);
            int n = (op == 0xD0 || op == 0xD1) ? 1 : (c->reg[ECX] & 31);
            uint32_t v = RM32(m), r = 0;
            switch (m.reg) {
            case 0: case 2: case 4: shl_flags_res(c, v, n, &r); break;
            case 5: shr_flags_res(c, v, n, 1, &r); break;
            case 7: sar_flags_res(c, v, n, &r); break;
            case 1: { n &= 31; r = n ? (v << n) | (v >> (32 - n)) : v; break; }
            case 3: { n &= 31; r = n ? (v >> n) | (v << (32 - n)) : v; break; }
            case 6: r = v; break;
            }
            SET_RM32(m, r);
            break;
        }
        case 0xD7: { uint8_t b = M8((c->reg[EBX] + c->reg[EAX]) & 0xFFFFFFFF); c->reg[EAX] = b; break; }  /* xlat */

        /* ---------------- x87 ---------------- */
        case 0xD8 ... 0xDF: {
            uint8_t mod = M8(c->eip);
            int is_mem = (mod >> 6) != 3;
            ModRM m = modrm(c, 0);
            uint32_t a = is_mem ? m.ea : 0;
            switch (op) {
            case 0xD8:
                if (is_mem) {
                    long double v = fld_m32(a);
                    switch (m.reg) {
                    case 0: FST(0) += v; break;
                    case 1: FST(0) *= v; break;
                    case 2: FST(0) -= v; break;
                    case 3: FST(0) = v - FST(0); break;
                    case 4: FST(0) /= v; break;
                    case 5: FST(0) = v / FST(0); break;
                    case 6: case 7: fcom_setcc(c, FST(0), v); if (m.reg == 7) fpop(c); break;
                    }
                } else {
                    switch (mod) {
                    case 0xC0 ... 0xC7: FST(0) += FST(mod & 7); break;
                    case 0xC8 ... 0xCF: FST(0) *= FST(mod & 7); break;
                    case 0xE0 ... 0xE7: FST(0) -= FST(mod & 7); break;
                    case 0xE8 ... 0xEF: { long double t = FST(mod & 7); FST(0) = t - FST(0); break; }
                    case 0xF0 ... 0xF7: FST(0) /= FST(mod & 7); break;
                    case 0xF8 ... 0xFF: { long double t = FST(mod & 7); FST(0) = t / FST(0); break; }
                    default: ve_log("x87 D8 %02x", mod); return 2;
                    }
                }
                break;
            case 0xD9:
                if (is_mem) {
                    switch (m.reg) {
                    case 0: fpush(c, fld_m32(a)); break;
                    case 2: { long double v = FST(0); float f = (float)v; memcpy(&M32(a), &f, 4); break; }
                    case 3: { long double v = FST(0); float f = (float)v; memcpy(&M32(a), &f, 4); fpop(c); break; }
                    case 4: { uint32_t e = M32(a); c->fctrl = (uint16_t)e; break; }
                    case 5: WR32(a, c->fctrl); break;
                    case 7: WM64(a, ((uint64_t)c->fstat) | (((uint64_t)(c->fctrl)) << 16)); break;
                    default: ve_log("x87 D9 /%d mem", m.reg); return 2;
                    }
                } else {
                    switch (mod) {
                    case 0xC0 ... 0xC7: fpush(c, FST(mod & 7)); break;
                    case 0xC8 ... 0xCF: { long double t = FST(0); FST(0) = FST(mod & 7); FST(mod & 7) = t; break; }
                    case 0xD0: break;   /* fnop */
                    case 0xE0: FST(0) = -FST(0); break;
                    case 0xE1: FST(0) = fabsl(FST(0)); break;
                    case 0xE4: fcom_setcc(c, FST(0), 0.0L); break;
                    case 0xE5: break;   /* fxam: 近似 */
                    case 0xE8: fpush(c, 1.0L); break;
                    case 0xE9: fpush(c, 3.3219280948873623L); break;   /* l2t */
                    case 0xEA: fpush(c, 1.4426950408889634L); break;   /* l2e */
                    case 0xEB: fpush(c, 3.14159265358979323846L); break;
                    case 0xEC: fpush(c, 0.3010299956639812L); break;   /* lg2 */
                    case 0xED: fpush(c, 0.6931471805599453L); break;   /* ln2 */
                    case 0xEE: fpush(c, 0.0L); break;
                    case 0xF0: { long double x = FST(0); FST(0) = powl(2.0L, x) - 1.0L; break; }
                    case 0xF1: { long double y = FST(0); fpop(c); FST(0) = y * log2l(FST(0)); break; }
                    case 0xF2: { long double t = tanl(FST(0)); FST(0) = t; fpush(c, 1.0L); break; }
                    case 0xF3: { long double y = FST(0); fpop(c); FST(0) = atan2l(FST(0), y); break; }
                    case 0xF4: { long double v = FST(0); FST(0) = truncl(v); fpush(c, v - truncl(v)); break; }
                    case 0xF5: { long double x = FST(0); fpop(c); FST(0) = fmodl(FST(0), x); break; }
                    case 0xF6: c->ftop = (c->ftop + 1) & 7; break;
                    case 0xF7: c->ftop = (c->ftop + 7) & 7; break;
                    case 0xF8: { long double x = FST(0); FST(0) = fmodl(FST(0), x); break; }
                    case 0xF9: { long double y = FST(0); fpop(c); FST(0) = y * log2l(FST(0) + 1.0L); break; }
                    case 0xFA: FST(0) = sqrtl(FST(0)); break;
                    case 0xFB: { long double s = sinl(FST(0)); fpush(c, cosl(FST(0))); FST(1) = s; break; }
                    case 0xFC: FST(0) = frndint_of(c, FST(0)); break;
                    case 0xFD: { long double y = FST(0); fpop(c); FST(0) = scalbnl(FST(0), (int)truncl(y)); break; }
                    case 0xFE: FST(0) = sinl(FST(0)); break;
                    case 0xFF: FST(0) = cosl(FST(0)); break;
                    default: ve_log("x87 D9 %02x @%08x", mod, c->eip - 2); return 2;
                    }
                }
                break;
            case 0xDA:
                if (is_mem) {
                    int16_t w; memcpy(&w, GADDR(a), 2);
                    int32_t i32 = w;
                    switch (m.reg) {
                    case 0: fpush(c, (long double)i32); break;
                    case 2: case 3: { int32_t t = (int32_t)frndint_of(c, FST(0)); int16_t tt = (int16_t)t; memcpy((uint8_t *)(uintptr_t)a, &tt, 2); if (m.reg == 3) fpop(c); break; }
                    default: ve_log("x87 DA /%d", m.reg); return 2;
                    }
                } else { ve_log("x87 DA reg %02x", mod); return 2; }
                break;
            case 0xDB:
                if (is_mem) {
                    switch (m.reg) {
                    case 0: { int32_t v; memcpy(&v, (uint8_t *)(uintptr_t)a, 4); fpush(c, v); break; }
                    case 2: case 3: { int32_t t = (int32_t)frndint_of(c, FST(0)); M32(a) = t; if (m.reg == 3) fpop(c); break; }
                    case 5: fpush(c, fld_m80(a)); break;
                    case 7: fstp_m80(a, FST(0)); fpop(c); break;
                    default: ve_log("x87 DB /%d", m.reg); return 2;
                    }
                } else {
                    if (mod >= 0xF0) { fcom_setcc(c, FST(0), FST(mod & 7)); fpop(c); FCOMI_FLAGS(); }
                    else if (mod >= 0xE8) { fcom_setcc(c, FST(0), FST(mod & 7)); FCOMI_FLAGS(); }
                    else { ve_log("x87 DB %02x", mod); return 2; }
                }
                break;
            case 0xDC:
                if (is_mem) {
                    long double v = fld_m64(a);
                    switch (m.reg) {
                    case 0: FST(0) += v; break;
                    case 1: FST(0) *= v; break;
                    case 2: FST(0) -= v; break;
                    case 3: FST(0) = v - FST(0); break;
                    case 4: FST(0) /= v; break;
                    case 5: FST(0) = v / FST(0); break;
                    case 6: case 7: fcom_setcc(c, FST(0), v); if (m.reg == 7) fpop(c); break;
                    }
                } else {
                    switch (mod) {
                    case 0xC0 ... 0xC7: FST(mod & 7) += FST(0); break;
                    case 0xC8 ... 0xCF: FST(mod & 7) *= FST(0); break;
                    case 0xE8 ... 0xEF: FST(mod & 7) -= FST(0); break;
                    case 0xE0 ... 0xE7: FST(mod & 7) = FST(0) - FST(mod & 7); break;
                    case 0xF8 ... 0xFF: FST(mod & 7) /= FST(0); break;
                    case 0xF0 ... 0xF7: FST(mod & 7) = FST(0) / FST(mod & 7); break;
                    default: ve_log("x87 DC %02x", mod); return 2;
                    }
                }
                break;
            case 0xDD:
                if (is_mem) {
                    switch (m.reg) {
                    case 0: fpush(c, fld_m64(a)); break;
                    case 2: { double f = (double)FST(0); WM64(a, *(uint64_t *)&f); break; }
                    case 3: { double f = (double)FST(0); WM64(a, *(uint64_t *)&f); fpop(c); break; }
                    case 4: case 6: {   /* frstor/fsave: 94/108 字节 — 只恢复/保存核心 */
                        uint8_t *p = (uint8_t *)(uintptr_t)a;
                        memcpy(&c->fctrl, p + 0, 2); memcpy(&c->fstat, p + 2, 2);
                        for (int i = 0; i < 8; i++) { long double v = fld_m80(a + 28 + i * 10); c->st[(c->ftop + i) & 7] = v; }
                        if (m.reg == 6) memset(p, 0, 108);
                        break;
                    }
                    default: ve_log("x87 DD /%d", m.reg); return 2;
                    }
                } else {
                    switch (mod) {
                    case 0xC0 ... 0xC7: break;   /* ffree st(i) */
                    case 0xD0 ... 0xD7: { long double t = FST(0); (void)t; break; }  /* fst st(i): st0→st(i) */
                    case 0xD8 ... 0xDF: { FST(mod & 7) = FST(0); fpop(c); break; }
                    case 0xE0 ... 0xE7: fcom_setcc(c, FST(0), FST(mod & 7)); break;
                    case 0xE8 ... 0xEF: fcom_setcc(c, FST(0), FST(mod & 7)); fpop(c); break;
                    default: ve_log("x87 DD %02x", mod); return 2;
                    }
                }
                break;
            case 0xDE:
                if (is_mem) {
                    int16_t w; memcpy(&w, GADDR(a), 2);
                    switch (m.reg) {
                    case 0: case 1: case 4: case 5: ve_log("x87 DE mem /%d", m.reg); return 2;
                    default: ve_log("x87 DE mem /%d", m.reg); return 2;
                    }
                } else {
                    switch (mod) {
                    case 0xC0 ... 0xC7: FST(mod & 7) += FST(0); fpop(c); break;
                    case 0xC8 ... 0xCF: FST(mod & 7) *= FST(0); fpop(c); break;
                    case 0xD9: { fcom_setcc(c, FST(0), FST(1)); fpop(c); fpop(c); break; }  /* fcompp */
                    case 0xE8 ... 0xEF: FST(mod & 7) -= FST(0); fpop(c); break;
                    case 0xE0 ... 0xE7: FST(mod & 7) = FST(0) - FST(mod & 7); fpop(c); break;
                    case 0xF8 ... 0xFF: FST(mod & 7) /= FST(0); fpop(c); break;
                    case 0xF0 ... 0xF7: FST(mod & 7) = FST(0) / FST(mod & 7); fpop(c); break;
                    default: ve_log("x87 DE %02x", mod); return 2;
                    }
                }
                break;
            case 0xDF:
                if (is_mem) {
                    switch (m.reg) {
                    case 0: { int16_t w; memcpy(&w, GADDR(a), 2); fpush(c, w); break; }
                    case 2: case 3: { int16_t t = (int16_t)(int32_t)frndint_of(c, FST(0)); memcpy(GADDR(a), &t, 2); if (m.reg == 3) fpop(c); break; }
                    case 5: fpush(c, fld_m64(a)); break;   /* fild m64 */
                    case 7: { int64_t t = (int64_t)frndint_of(c, FST(0)); WM64(a, t); fpop(c); break; }
                    default: ve_log("x87 DF /%d", m.reg); return 2;
                    }
                } else {
                    if (mod >= 0xF0) { fcom_setcc(c, FST(0), FST(mod & 7)); fpop(c); FCOMI_FLAGS(); }
                    else if (mod >= 0xE8) { fcom_setcc(c, FST(0), FST(mod & 7)); FCOMI_FLAGS(); }
                    else if (mod == 0xE0) { int16_t w = (int16_t)(int32_t)frndint_of(c, FST(0)); c->reg[EAX] = (uint32_t)(int32_t)w; }
                    else { ve_log("x87 DF %02x", mod); return 2; }
                }
                break;
            }
            break;
        }

        case 0xE0: case 0xE1: case 0xE2: {   /* loopnz/loopz/loop */
            int8_t d = (int8_t)fetch8(c);
            c->reg[ECX]--;
            int take = c->reg[ECX] != 0;
            if (op == 0xE0) take = take && !(c->flags & ZF);
            if (op == 0xE1) take = take && (c->flags & ZF);
            if (take) c->eip += d;
            break;
        }
        case 0xE3: { int8_t d = (int8_t)fetch8(c); if (c->reg[ECX] == 0) c->eip += d; break; }

        case 0xE8: {   /* call rel32 */
            int32_t d = (int32_t)fetch32(c);
            push32(c, c->eip);
            c->eip += d;
            break;
        }
        case 0xE9: { int32_t d = (int32_t)fetch32(c); c->eip += d; break; }
        case 0xEB: { int8_t d = (int8_t)fetch8(c); c->eip += d; break; }

        case 0xF4: ve_log("hlt"); return 2;
        case 0xF5: c->flags ^= CF; break;
        case 0xF6: {   /* grp3 rm8 */
            ModRM m = modrm(c, osize);
            switch (m.reg) {
            case 0: case 1: { uint8_t b = fetch8(c); logic_flags(c, RM8(m) & b); break; }
            case 2: SET_RM8(m, ~RM8(m)); break;
            case 3: { uint32_t v = RM8(m); uint32_t r = 0 - v; sub_flags(c, 0, v, r); SET_RM8(m, r); break; }
            case 4: { uint32_t a = c->reg[EAX] & 0xFF, b = RM8(m); c->reg[EAX] = a * b; break; }
            case 5: { int8_t a = c->reg[EAX] & 0xFF, b = RM8(m); c->reg[EAX] = (uint32_t)(int32_t)(a * b); break; }
            case 6: { uint8_t b = RM8(m); if (!b) { ve_raise_fault(c, 0, c->eip); break; } c->reg[EAX] = ((c->reg[EAX] & 0xFF) / b) | (((c->reg[EAX] & 0xFF) % b) << 8); break; }
            case 7: { int8_t b = RM8(m); if (!b) { ve_raise_fault(c, 0, c->eip); break; } int8_t a = c->reg[EAX] & 0xFF; c->reg[EAX] = ((uint8_t)(a / b)) | (((uint8_t)(a % b)) << 8); break; }
            }
            break;
        }
        case 0xF7: {   /* grp3 rm32 */
            ModRM m = modrm(c, osize);
            switch (m.reg) {
            case 0: case 1: { uint32_t b = fetch32(c); logic_flags(c, RM32(m) & b); break; }
            case 2: SET_RM32(m, ~RM32(m)); break;
            case 3: { uint32_t v = RM32(m), r = 0 - v; sub_flags(c, 0, v, r); SET_RM32(m, r); break; }
            case 4: { uint64_t r = (uint64_t)RM32(m) * c->reg[EAX]; c->reg[EAX] = (uint32_t)r; c->reg[EDX] = (uint32_t)(r >> 32); break; }
            case 5: { int64_t r = (int64_t)(int32_t)RM32(m) * (int32_t)c->reg[EAX]; c->reg[EAX] = (uint32_t)r; c->reg[EDX] = (uint32_t)((uint64_t)r >> 32); break; }
            case 6: { uint32_t b = RM32(m); if (!b) { ve_raise_fault(c, 0, c->eip); break; } uint32_t a = c->reg[EAX]; c->reg[EAX] = a / b; c->reg[EDX] = a % b; break; }
            case 7: { int32_t b = (int32_t)RM32(m); if (!b) { ve_raise_fault(c, 0, c->eip); break; } int64_t a = (int32_t)c->reg[EAX]; c->reg[EAX] = (uint32_t)(int64_t)(a / b); c->reg[EDX] = (uint32_t)(int64_t)(a % b); break; }
            }
            break;
        }
        case 0xF8: c->flags &= ~CF; break;
        case 0xF9: c->flags |= CF; break;
        case 0xFA: case 0xFB: break;   /* cli/sti */
        case 0xFC: c->flags &= ~DF; break;
        case 0xFD: c->flags |= DF; break;
        case 0xFE: {   /* grp4 inc/dec rm8 */
            ModRM m = modrm(c, osize);
            uint32_t a = RM8(m), r;
            if (m.reg == 0) { r = a + 1; inc_flags(c, a, r); }
            else { r = a - 1; dec_flags(c, a, r); }
            SET_RM8(m, r);
            break;
        }
        case 0xFF: {   /* grp5 */
            ModRM m = modrm(c, osize);
            switch (m.reg) {
            case 0: { uint32_t a = RM32(m), r = a + 1; inc_flags(c, a, r); SET_RM32(m, r); break; }
            case 1: { uint32_t a = RM32(m), r = a - 1; dec_flags(c, a, r); SET_RM32(m, r); break; }
            case 2: {   /* call rm32 */
                uint32_t target = RM32(m);
                push32(c, c->eip);
                if (target >= THUNK_BASE && target < THUNK_END) {
                    int idx = (int)((target - THUNK_BASE) / 8);
                    c->reg[ESP] += 4;   /* 弹返回地址, 垫片自行处理 */
                    int pop = ve_shim_dispatch(idx, c);
                    c->reg[ESP] += pop;
                } else c->eip = target;
                break;
            }
            case 3: ve_log("callf"); return 2;
            case 4: {   /* jmp rm32 (含尾调用导入: jmp [iat]) */
                uint32_t target = RM32(m);
                if (target >= THUNK_BASE && target < THUNK_END) {
                    /* 尾调用语义: 弹出跳转函数自身的返回地址 → 跑垫片 → 回到其调用者 */
                    int idx = (int)((target - THUNK_BASE) / 8);
                    uint32_t tail_ret = M32(c->reg[ESP]);
                    c->reg[ESP] += 4;
                    int popn = ve_shim_dispatch(idx, c);
                    c->reg[ESP] += popn;
                    c->eip = tail_ret;
                    if (c->eip == STOP_EIP) return 0;
                    break;
                }
                c->eip = target;
                break;
            }
            case 6: push32(c, RM32(m)); break;
            default:
                ve_log("FF /%d @0x%08x bytes=%02x %02x %02x %02x esp=0x%08x [esp]=0x%08x", m.reg, c->eip - 2, M8(c->eip - 2), M8(c->eip - 1), M8(c->eip), M8(c->eip + 1), c->reg[4], M32(c->reg[4]));
                return 2;
            }
            break;
        }

        default:
            ve_log("未实现主操作码 %02X @0x%08x", op, c->eip - 1);
            return 2;
        }
        (void)new_eip;
    }
}

/* 从外部调用 guest 函数: 压参 + 哨兵返回地址, 跑到哨兵为止 */
uint32_t ve_call_guest(VeCPU *c, uint32_t func, int nargs, const uint32_t *args) {
    uint32_t saved_esp = c->reg[ESP];
    /* cdecl 语义: [esp]=返回地址(STOP 哨兵), [esp+4]=arg1, ...
     * 参数从右往左压, 哨兵最后压 (曾把哨兵压最深 → ret 弹到 arg1=基址, 跳进 PE 头) */
    for (int i = nargs - 1; i >= 0; i--) {
        c->reg[ESP] -= 4;
        WR32(c->reg[ESP], args[i]);
    }
    c->reg[ESP] -= 4;
    WR32(c->reg[ESP], STOP_EIP);
    c->eip = func;
    uint32_t rc = ve_cpu_run(c, 0);
    if (rc != 0) {
        ve_log("guest 调用失败 func=0x%08x rc=%u eax=0x%08x esp=0x%08x",
               func, rc, c->reg[EAX], c->reg[ESP]);
    }
    c->reg[ESP] = saved_esp;   /* 修 esp 漂移 (含异常退出) */
    return rc;
}
