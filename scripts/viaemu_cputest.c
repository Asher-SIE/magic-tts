#include "viaemu.h"
#include <stdio.h>
#include <string.h>
int ve_region_init(void);

static VeCPU cpu;
static uint32_t code_addr = 0x32000000;

/* 预期值手工推导 (CF=1 PF=4 AF=10 ZF=40 SF=80 OF=800) */
static const struct { const char *n; uint8_t c[8]; int len; uint32_t a, b, want; } T[] = {
    {"add 0+1",        {0x83,0xC0,0x01,0xC3}, 4, 0, 0, 0x000},
    {"add FFFFFFFF+1", {0x83,0xC0,0x01,0xC3}, 4, 0xFFFFFFFF, 0, 0x055},
    {"add 7FFF..+1",   {0x83,0xC0,0x01,0xC3}, 4, 0x7FFFFFFF, 0, 0x894},
    {"sub 0-1",        {0x83,0xE8,0x01,0xC3}, 4, 0, 0, 0x095},
    {"cmp eq",         {0x39,0xD8,0xC3},      3, 5, 5, 0x045},
    {"inc FFFFFFFF",   {0x40,0xC3},           2, 0xFFFFFFFF, 0, 0x054},
    {"dec 0",          {0x48,0xC3},           2, 0, 0, 0x094},
    {"shl 8000..,1",   {0xD1,0xE0,0xC3},      3, 0x80000000, 0, 0x845},
    {"neg 0",          {0xF7,0xD8,0xC3},      3, 0, 0, 0x044},
    {"neg 8000..",     {0xF7,0xD8,0xC3},      3, 0x80000000, 0, 0x884},
    {"test 0",         {0x85,0xC0,0xC3},      3, 0, 0, 0x044},
};

int main(void) {
    if (ve_region_init()) return 1;
    int fail = 0;
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        memset(&cpu, 0, sizeof(cpu));
        cpu.fctrl = 0x027F;
        cpu.reg[4] = 0x3E000000;
        cpu.reg[0] = T[i].a;
        cpu.reg[3] = T[i].b;
        memcpy(viaemu_guest(code_addr), T[i].c, T[i].len);
        uint32_t noargs[1] = { 0 };
        ve_call_guest(&cpu, code_addr, 0, noargs);
        uint32_t got = cpu.flags & 0x8D5;
        printf("%-18s got=0x%03x want=0x%03x %s\n", T[i].n, got, T[i].want,
               got == T[i].want ? "OK" : "FAIL");
        if (got != T[i].want) fail++;
    }
    return fail;
}
