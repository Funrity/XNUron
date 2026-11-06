/* Mach-O (arm64) parser and loader for user-mode machines. */
#pragma once
#include "machine.h"
#ifdef __cplusplus
extern "C" {
#endif

#define A64_MACHO_MAX_SEG 48
#define A64_MACHO_MAX_DYLIB 16
typedef struct {
    char name[17];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot;
} A64Seg;

typedef struct {
    bool valid;                       /* header recognised and load commands parsed */
    bool fat; uint32_t fat_archs;     /* universal binary; slice chosen = arm64 */
    uint64_t slice_off, slice_len;    /* slice inside the input buffer */
    uint32_t cputype, cpusubtype, filetype, ncmds, flags;
    int nseg; A64Seg seg[A64_MACHO_MAX_SEG];
    bool has_main; uint64_t entryoff;
    bool has_unixthread; uint64_t thread_pc;
    bool has_dylinker;
    int ndylibs; char dylib[A64_MACHO_MAX_DYLIB][96];
    bool has_dyld_info, has_chained_fixups, has_code_signature, has_encryption;
    uint32_t cryptid;
    bool has_build_version; uint32_t platform, minos;
    uint64_t lowest_vm, highest_vm;   /* over segments with vmsize and a non-zero-page mapping */
    char err[160];
} A64MachO;

int a64_macho_parse(const uint8_t *d, size_t n, A64MachO *out);           /* 0 ok */
/* Human-readable summary, one finding per line. */
size_t a64_macho_describe(const A64MachO *m, char *buf, size_t cap);
/* Why this image cannot be executed by the user-mode HLE machine ("" if it can). */
void a64_macho_runnable_reason(const A64MachO *m, char *buf, size_t cap);
/* Builds a user-mode machine with the image mapped at its link addresses, PC at the
 * entry point and SP at the top of a fresh stack.  NULL + reason in err on failure. */
A64Machine *a64_macho_create_machine(const uint8_t *d, size_t n, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
