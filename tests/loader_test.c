/* Tests for Mach-O loading/running, IMG4 inspection, ZIP/IPSW listing. */
#include "../app/src/main/cpp/macho.h"
#include "../app/src/main/cpp/inspect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 1); if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; } fclose(f); return b;
}
static int rd_file(void *ud, uint64_t off, void *buf, size_t len) {
    FILE *f = ud; if (fseek(f, (long)off, SEEK_SET)) return -1; return fread(buf, 1, len, f) == len ? 0 : -1;
}
/* minimal DER writer */
static size_t der(uint8_t *o, uint8_t tag, const uint8_t *v, size_t n) {
    size_t k = 0; o[k++] = tag;
    if (n < 128) o[k++] = (uint8_t)n; else if (n < 256) { o[k++] = 0x81; o[k++] = (uint8_t)n; } else { o[k++] = 0x82; o[k++] = (uint8_t)(n >> 8); o[k++] = (uint8_t)n; }
    memcpy(o + k, v, n); return k + n;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: loader_test hello.macho ipsw.zip plain.zip old_ipsw.zip\n"); return 2; }
    char rep[8192]; A64Inspect res;

    /* ---- Mach-O: real arm64 binary linked by lld, runs in user mode via XNU syscall ABI */
    size_t n = 0; uint8_t *mo = slurp(argv[1], &n);
    CHECK(mo != NULL, "test Mach-O present");
    a64_inspect_buffer(mo, n, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_MACHO && res.runnable_user && !res.bootable_system, "Mach-O inspected: runnable as user-mode program, not as system");
    CHECK(strstr(rep, "arm64") && strstr(rep, "iOS") && strstr(rep, "__TEXT"), "report names arm64, iOS platform and segments");
    char err[256]; A64Machine *m = a64_macho_create_machine(mo, n, err, sizeof err);
    CHECK(m != NULL, "Mach-O mapped into a user-mode machine");
    if (m) {
        A64MState st = a64m_run(m, 1000000, false);
        char log[1024]; size_t k = a64m_log_read(m, 0, log, sizeof log - 1); log[k] = 0;
        CHECK(st == A64M_POWEROFF && a64m_exit_code(m) == 7, "Mach-O program exits with code 7 through BSD SYS_exit");
        CHECK(strstr(log, "hello from a Mach-O arm64 (own code)") != NULL, "BSD SYS_write output reaches the boot log");
        CHECK(strstr(log, "unimplemented BSD/XNU syscall 999") != NULL, "unimplemented syscall is reported in the log");
        CHECK(a64m_cpu(m)->x[0] == 7, "exit status in x0");
        a64m_destroy(m);
    }
    /* corrupt / foreign Mach-O are rejected with reasons */
    uint8_t *bad = malloc(n); memcpy(bad, mo, n); bad[4] = 0x07; /* cputype x86 */
    m = a64_macho_create_machine(bad, n, err, sizeof err);
    CHECK(m == NULL && strstr(err, "not arm64"), "non-arm64 Mach-O refused with reason");
    memcpy(bad, mo, n); bad[12] = 0x0C;                                     /* filetype FILESET */
    m = a64_macho_create_machine(bad, n, err, sizeof err);
    CHECK(m == NULL && strstr(err, "kernelcache"), "kernel collection (MH_FILESET) refused: needs Apple boot chain");
    m = a64_macho_create_machine(mo, 40, err, sizeof err);
    CHECK(m == NULL && err[0], "truncated Mach-O refused");
    free(bad); free(mo);

    /* ---- IM4P, encrypted (KBAG) */
    uint8_t buf[2048], t1[600], t2[600], body[1200]; size_t k = 0, j;
    k += der(body + k, 0x16, (const uint8_t *)"IM4P", 4);
    k += der(body + k, 0x16, (const uint8_t *)"krnl", 4);
    k += der(body + k, 0x16, (const uint8_t *)"KernelCacheBuilder-fake", 23);
    uint8_t payload[64]; memset(payload, 0xA7, sizeof payload);
    k += der(body + k, 0x04, payload, sizeof payload);
    uint8_t slot[64]; memset(slot, 1, sizeof slot);
    j = der(t1, 0x30, slot, sizeof slot); j += der(t1 + j, 0x30, slot, sizeof slot);
    size_t kb = der(t2, 0x30, t1, j);
    k += der(body + k, 0x04, t2, kb);
    size_t im4p = der(buf, 0x30, body, k);
    a64_inspect_buffer(buf, im4p, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_IM4P && !res.runnable_user && !res.bootable_system, "IM4P recognised and not runnable");
    CHECK(strstr(rep, "kernelcache") && strstr(rep, "KBAG") && strstr(rep, "AES-encrypted") && strstr(rep, "2 key slot"), "IM4P report: kernelcache, key bag with 2 slots, AES-encrypted");
    CHECK(strstr(rep, "neither contains, derives nor fetches"), "report states no keys are handled");
    /* ---- IM4P unencrypted LZFSE payload inside signed IMG4 */
    k = 0; k += der(body + k, 0x16, (const uint8_t *)"IM4P", 4); k += der(body + k, 0x16, (const uint8_t *)"dtre", 4);
    k += der(body + k, 0x16, (const uint8_t *)"DeviceTree", 10);
    uint8_t lz[32] = "bvx2"; k += der(body + k, 0x04, lz, sizeof lz);
    size_t ip = der(t1, 0x30, body, k);
    size_t tot = 0; uint8_t outer[1500];
    tot += der(outer + tot, 0x16, (const uint8_t *)"IMG4", 4);
    memcpy(outer + tot, t1, ip); tot += ip;
    uint8_t man[8] = { 0x30, 0x06, 0x16, 0x04, 'I', 'M', '4', 'M' };
    tot += der(outer + tot, 0xA0, man, sizeof man);
    size_t img4 = der(buf, 0x30, outer, tot);
    a64_inspect_buffer(buf, img4, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_IMG4 && strstr(rep, "DeviceTree") && strstr(rep, "LZFSE") && strstr(rep, "IM4M") && strstr(rep, "ECID"), "IMG4 with manifest: DeviceTree, LZFSE payload, signed-manifest blocker");
    /* garbage DER must not crash */
    uint8_t g[40]; for (int q = 0; q < 40; q++) g[q] = (uint8_t)(q * 37); g[0] = 0x30; g[1] = 0x7F;
    a64_inspect_buffer(g, sizeof g, &res, rep, sizeof rep);
    CHECK(!res.runnable_user, "malformed DER handled without crash");

    /* ---- ZIP / IPSW listing (zip64 members) */
    FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); uint64_t sz = (uint64_t)ftell(f);
    a64_inspect_source(rd_file, f, sz, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_IPSW && strstr(rep, "BuildManifest.plist") && strstr(rep, "kernelcache") && strstr(rep, "DeviceTree") && strstr(rep, "7 entries"), "IPSW-like zip64 archive listed and recognised");
    CHECK(strstr(rep, "personalised/signed by Apple") && strstr(rep, "AGX GPU") && strstr(rep, "licence"), "IPSW verdict states signing, hardware and licensing blockers");
    fclose(f);
    f = fopen(argv[3], "rb"); fseek(f, 0, SEEK_END); sz = (uint64_t)ftell(f);
    a64_inspect_source(rd_file, f, sz, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_ZIP && strstr(rep, "Not recognised as an IPSW"), "ordinary zip is not mistaken for an IPSW");
    fclose(f);

    /* ---- old-style IPSW for a 32-bit SoC (like the s5l8900 kernelcache a user tried to boot) */
    f = fopen(argv[4], "rb"); fseek(f, 0, SEEK_END); sz = (uint64_t)ftell(f);
    a64_inspect_source(rd_file, f, sz, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_IPSW && strstr(rep, "S5L8900") && strstr(rep, "32-bit") && strstr(rep, "Old-style IPSW"), "old-style IPSW without BuildManifest recognised, 32-bit SoC identified");
    fclose(f);
    /* ---- 32-bit Mach-O and Apple containers are explained, not run as AArch64 */
    uint8_t m32[64] = { 0xCE, 0xFA, 0xED, 0xFE, 12, 0, 0, 0, 6, 0, 0, 0, 2 };
    a64_inspect_buffer(m32, sizeof m32, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_MACHO32 && !res.runnable_user && strstr(rep, "ARMv6") && strstr(rep, "AArch32"), "32-bit ARMv6 Mach-O explained as not executable");
    uint8_t c8900[64] = { '8', '9', '0', '0', '1', '.', '0', 1 };
    a64_inspect_buffer(c8900, sizeof c8900, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_APPLE_CONTAINER && !res.runnable_user && strstr(rep, "not AArch64"), "8900 container explained as not AArch64 code");

    /* ---- unknown blob */
    uint8_t raw[16] = { 1, 2, 3, 4 };
    a64_inspect_buffer(raw, sizeof raw, &res, rep, sizeof rep);
    CHECK(res.kind == A64_KIND_RAW_A64 && strstr(rep, "nothing is known"), "unknown file is described honestly");

    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails != 0;
}
