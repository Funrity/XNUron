#include "inspect.h"
#include "macho.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define P(...) do { if (n < cap) n += (size_t)snprintf(rep + n, cap - n, __VA_ARGS__); } while (0)

static void verdict(A64Inspect *r, char *rep, size_t cap, size_t *np) {
    size_t n = *np;
    if (r->bootable_system) P("Verdict: can be started as a system image.\n");
    else if (r->runnable_user) P("Verdict: can be started as a USER-MODE program (XNU/BSD syscall emulation). It does NOT boot iOS.\n");
    else P("Verdict: NOT runnable in XNUron. This is a property of the file and the platform, not a bug in the app.\n");
    *np = n;
}

size_t a64_inspect_buffer(const uint8_t *d, size_t n0, A64Inspect *res, char *rep, size_t cap) {
    size_t n = 0; memset(res, 0, sizeof *res); if (cap) rep[0] = 0;
    uint32_t magic_le = n0 >= 4 ? (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24 : 0;
    uint32_t magic_be = n0 >= 4 ? (uint32_t)d[3] | (uint32_t)d[2] << 8 | (uint32_t)d[1] << 16 | (uint32_t)d[0] << 24 : 0;
    P("File size: %zu bytes\n", n0);
    if (n0 >= 28 && magic_le == 0xFEEDFACEu) {
        uint32_t ct = (uint32_t)d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
        uint32_t st = (uint32_t)d[8] | (uint32_t)d[9] << 8 | (uint32_t)d[10] << 16 | (uint32_t)d[11] << 24;
        res->kind = A64_KIND_MACHO32;
        P("32-bit Mach-O, cputype %u (%s), subtype %u%s.\n", ct, ct == 12 ? "ARM" : ct == 7 ? "x86" : "other",
          st, ct == 12 ? (st == 6 ? " = ARMv6 (ARM11, e.g. S5L8900 devices)" : st == 9 ? " = ARMv7" : st == 11 ? " = ARMv7s" : "") : "");
        P("[BLOCKER] This is 32-bit code (AArch32). XNUron emulates only 64-bit AArch64 and has no 32-bit ARM instruction set, so no byte of it can be executed. "
          "Starting it as a raw AArch64 image would just decode garbage and fault.\n");
    } else if (n0 >= 8 && (!memcmp(d, "8900", 4) || !memcmp(d, "3gmI", 4) || !memcmp(d, "2gmI", 4))) {
        res->kind = A64_KIND_APPLE_CONTAINER;
        P("Apple firmware container: magic '%.4s' (%s).\n", (const char *)d,
          !memcmp(d, "8900", 4) ? "8900 format, used by the first Apple SoC generation" : !memcmp(d, "3gmI", 4) ? "IMG3, used from about 2009 to 2011 devices" : "IMG2");
        P("[BLOCKER] These containers come from 32-bit ARM devices (ARMv6/ARMv7) and are signed and often encrypted by Apple; the payload is not AArch64 code. "
          "XNUron neither decrypts them nor can execute 32-bit ARM.\n");
    } else if (n0 >= 4 && (magic_le == 0xFEEDFACFu || magic_be == 0xCAFEBABEu)) {
        A64MachO mo; char why[256];
        if (a64_macho_parse(d, n0, &mo) == 0) {
            res->kind = A64_KIND_MACHO;
            n += a64_macho_describe(&mo, rep + n, n < cap ? cap - n : 0);
            a64_macho_runnable_reason(&mo, why, sizeof why);
            if (why[0]) P("[BLOCKER] %s\n", why); else res->runnable_user = true;
            if (!mo.has_code_signature && mo.filetype == 2) P("Note: unsigned binary (fine for XNUron; real iOS would refuse to run it).\n");
        } else { P("Mach-O header present but invalid: %s\n", mo.err); }
    } else if (n0 >= 2 && d[0] == 0x30 && (d[1] == 0x82 || d[1] == 0x83 || d[1] == 0x84 || d[1] == 0x81 || d[1] < 0x80)) {
        size_t k = a64_img4_describe(d, n0, res, rep + n, n < cap ? cap - n : 0); n += k;
        if (res->kind == A64_KIND_UNKNOWN) P("Unrecognised DER structure.\n");
    } else if (n0 >= 4 && (!memcmp(d, "bvx2", 4) || !memcmp(d, "bvxn", 4))) {
        res->kind = A64_KIND_LZFSE; P("Raw LZFSE stream (probably a decrypted, compressed Apple payload).\n");
        P("[BLOCKER] LZFSE decompression is not implemented in XNUron yet, and the decompressed payload would still be an Apple boot component (see verdict rules).\n");
    } else if (n0 >= 2 && d[0] == 'P' && d[1] == 'K') {
        P("ZIP archive: pick it through the file chooser as a whole to list its contents (needs random access).\n");
        res->kind = A64_KIND_ZIP;
    } else if (n0 >= 8 && !memcmp(d, "AEA1", 4)) {
        P("Apple Encrypted Archive (AEA): encrypted container; no keys, cannot be read.\n");
    } else if (n0 >= 512 && (!memcmp(d + n0 - 512, "koly", 4))) {
        res->kind = A64_KIND_DMG; P("Apple disk image (UDIF .dmg). XNUron has no HFS+/APFS driver and the system volume of iOS is encrypted per build.\n");
    } else {
        res->kind = A64_KIND_RAW_A64;
        P("Unknown format. It can be started as a raw AArch64 image on a64virt (loaded at 0x%llx) but nothing is known about its contents.\n", 0x40000000ull);
    }
    verdict(res, rep, cap, &n);
    return n;
}

typedef struct { const uint8_t *d; size_t n; } MemSrc;

size_t a64_inspect_source(A64ReadAt rd, void *ud, uint64_t total, A64Inspect *res, char *rep, size_t cap) {
    memset(res, 0, sizeof *res);
    uint8_t head[4] = {0};
    if (total >= 4 && rd(ud, 0, head, 4) == 0 && head[0] == 'P' && head[1] == 'K' && total > (64u << 20)) {
        size_t n = 0; P("File size: %llu bytes\n", (unsigned long long)total);
        n += a64_zip_describe(rd, ud, total, res, rep + n, n < cap ? cap - n : 0);
        verdict(res, rep, cap, &n);
        return n;
    }
    size_t want = total < (128u << 20) ? (size_t)total : (128u << 20);
    uint8_t *buf = malloc(want ? want : 1);
    if (!buf || rd(ud, 0, buf, want)) { free(buf); snprintf(rep, cap, "Could not read the file.\n"); return strlen(rep); }
    size_t n;
    if (want >= 2 && buf[0] == 'P' && buf[1] == 'K') {
        n = 0; P("File size: %llu bytes\n", (unsigned long long)total);
        n += a64_zip_describe(rd, ud, total, res, rep + n, n < cap ? cap - n : 0);
        verdict(res, rep, cap, &n);
    } else {
        n = a64_inspect_buffer(buf, want, res, rep, cap);
        if (want < total) { P("Note: only the first %zu of %llu bytes were examined.\n", want, (unsigned long long)total); }
    }
    free(buf);
    return n;
}
#undef P
