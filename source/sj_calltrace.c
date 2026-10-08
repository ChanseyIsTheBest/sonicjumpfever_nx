/* sj_calltrace.c -- log the first call the game makes to each import.
 * MIT licensed, see LICENSE.
 *
 * WHY
 * The orange screen (an external abort: Atmosphere's secure monitor stops the
 * console the instant it happens, no crash report) came within 250 ms of the
 * store switching on, every time, with nothing logged in between. Whatever
 * the game does in that window goes through its imports -- libc, GL, EGL,
 * OpenSL, sockets, dl*, the Android NDK -- so the first time it calls each
 * one is written to the card BEFORE the call runs, with the thread, the time
 * and the first three arguments. A call that kills the console still leaves
 * its own line behind. Only the first call per import is logged, so the
 * cost after boot is one table lookup per call.
 *
 * HOW
 * Every R_AARCH64_JUMP_SLOT (the PLT's GOT slots: calls, never data) is
 * pointed at a stub in sj_calltrace_stubs.s, which preserves the caller's
 * whole argument state, asks sj_ct_hit for the real target and branches to
 * it. GLOB_DAT slots (data such as __sF, __stack_chk_guard, SL_IID_*) are
 * left alone. Enabled with debug_log=1.
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>
#include "so_util.h"
#include "sj_calltrace.h"

#define CT_MAX 512

extern char sj_ct_stubs[];          /* CT_MAX stubs, 8 bytes each */

typedef struct {
    const char *name;
    uintptr_t   real;
    int         seen;
} CtEntry;

static CtEntry  g_ct[CT_MAX];
static int      g_ct_n;
static uint64_t g_t0;

/* Called from the stub with the import's index and the saved x0..x8. */
uintptr_t sj_ct_hit(unsigned k, const uint64_t *regs)
{
    CtEntry *e = &g_ct[k];
    if (!__atomic_load_n(&e->seen, __ATOMIC_RELAXED) &&
        !__atomic_exchange_n(&e->seen, 1, __ATOMIC_RELAXED)) {
        const double t = (double)armTicksToNs(armGetSystemTick() - g_t0) / 1e9;
        printf("sj: first call %7.3fs [%04x] %s(0x%llx, 0x%llx, 0x%llx)\n", t,
               (unsigned)((uintptr_t)armGetTls() & 0xFFFF), e->name,
               (unsigned long long)regs[0], (unsigned long long)regs[1],
               (unsigned long long)regs[2]);
    }
    return e->real;
}

int sj_calltrace_install(so_module *mod)
{
    int i, j, n = 0;

    g_t0 = armGetSystemTick();
    for (i = 0; i < mod->elf_hdr->e_shnum; i++) {
        const char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
        if (strcmp(sh_name, ".rela.plt") != 0) continue;
        Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
        const int count = (int)(mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela));
        for (j = 0; j < count; j++) {
            if (ELF64_R_TYPE(rels[j].r_info) != R_AARCH64_JUMP_SLOT) continue;
            if (g_ct_n >= CT_MAX) break;
            uintptr_t *slot = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
            const Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
            CtEntry *e = &g_ct[g_ct_n];
            /* COPY the name. dynstrtab points into load_base, which so_finalize
             * maps away (svcMapProcessCodeMemory aliases it to load_virtbase
             * and leaves the source inaccessible), so a pointer kept into it
             * faults on the first logged call -- build 4 died in the game's
             * first constructor exactly that way. */
            const char *nm = mod->dynstrtab + sym->st_name;
            const size_t nl = strlen(nm);
            char *copy = malloc(nl + 1);
            if (copy) memcpy(copy, nm, nl + 1);
            e->name = copy ? copy : "?";
            e->real = *slot;                       /* what so_resolve bound */
            e->seen = 0;
            *slot = (uintptr_t)(sj_ct_stubs + 8 * g_ct_n);
            g_ct_n++;
            n++;
        }
    }
    printf("sj: call trace on: the first call to each of %d imports is logged\n", n);
    return n;
}
