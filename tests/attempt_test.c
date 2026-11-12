#include "../app/src/main/cpp/attempt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static FILE *fp;
static int rd(void *u, uint64_t off, void *b, size_t n) { (void)u; if (fseek(fp, (long)off, SEEK_SET)) return -1; return fread(b, 1, n, fp) == n ? 0 : -1; }
static char *logof(A64Machine *m) { size_t n = a64m_log_size(m); char *s = malloc(n + 1); n = a64m_log_read(m, 0, s, n); s[n] = 0; return s; }
static char *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) { *n = 0; return NULL; } fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET); char *b = malloc(*n); if (fread(b, 1, *n, f) != *n) *n = 0; fclose(f); return b; }

static void one(const char *dir, const char *name, const char *want_state, const char *must[], const char *mustnot[], bool dump) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name); fp = fopen(p, "rb");
    if (!fp) { fails++; printf("  FAIL: cannot open %s\n", p); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp);
    A64Machine *m = a64_attempt_ipsw(rd, NULL, (uint64_t)sz);
    fclose(fp);
    if (!m) { fails++; printf("  FAIL: %s: NULL machine\n", name); return; }
    A64MState st = a64m_run(m, 60000000, false);
    char *log = logof(m);
    printf("-- %s: state=%d steps=%llu\n", name, st, (unsigned long long)a64m_cpu(m)->steps);
    CHECK(!strcmp(want_state, "fault") ? st == A64M_FAULT : st != A64M_FAULT, "%s: unexpected state %d", name, st);
    for (int i = 0; must && must[i]; i++) CHECK(strstr(log, must[i]), "%s: log lacks \"%s\"", name, must[i]);
    for (int i = 0; mustnot && mustnot[i]; i++) CHECK(!strstr(log, mustnot[i]), "%s: log must not contain \"%s\"", name, mustnot[i]);
    CHECK(!strstr(log, "booted") && !strstr(log, "Booted"), "%s: log claims a boot", name);
    if (dump) printf("%s\n", log);
    free(log); a64m_destroy(m);
}
static int ent_rng(void *b, size_t n) { memset(b, 0xA5, n); return 0; }
int main(int argc, char **argv) {
    const char *d = argv[1]; bool dump = argc > 2;
    const char *ok[] = { "step 1", "step 6: __TEXT", "step 7", "test-kernel: started at EL1", "FAKE-DEVICETREE-BLOB", "enabling MMU now", "S3_4_C15_C2_1(", "0x55", "S3_0_C15_C1_0(", "MMU is on: VBAR_EL1 address does not translate", "[stop] last executed pc=", NULL };
    const char *ok_not[] = { "ATTEMPT ENDED BEFORE", NULL };
    one(d, "ok_lzfse.zip", "fault", ok, ok_not, dump);
    one(d, "ok_lzss_stored.zip", "fault", ok, ok_not, dump);
    one(d, "ok_plain.zip", "fault", ok, ok_not, dump);
    const char *enc[] = { "encrypted (KBAG", "ATTEMPT ENDED BEFORE ANY GUEST INSTRUCTION RAN", NULL };
    const char *enc_not[] = { "step 6", "step 7", NULL };
    one(d, "encrypted.zip", "fault", enc, enc_not, dump);
    const char *a32[] = { "AArch32", "ATTEMPT ENDED BEFORE", NULL };
    one(d, "arm32.zip", "fault", a32, NULL, dump);
    const char *gb[] = { "ATTEMPT ENDED BEFORE", "not a 64-bit arm64 Mach-O", NULL };
    one(d, "garbage.zip", "fault", gb, NULL, dump);
    const char *sp[] = { "step 7", "[stop] loop window", "stuck in a tight loop", "code 0x", NULL };
    one(d, "spin.zip", "fault", sp, NULL, dump);
    const char *vs[] = { "the loop follows an exception", "unimplemented system register S3_5_C9_C9_5", "its own handler", NULL };
    one(d, "vecspin.zip", "fault", vs, NULL, dump);
    const char *nk[] = { "no matching member", NULL };
    one(d, "nokernel.zip", "fault", nk, NULL, dump);
    /* LZSS decoder against an independent (python) encoder, incl. back-references */
    size_t n1, n2; char *in = slurp("lzss_in.bin", &n1); char b[512]; snprintf(b, sizeof b, "%s/lzss_in.bin", d); free(in); in = slurp(b, &n1);
    snprintf(b, sizeof b, "%s/lzss.cmp", d); char *cmp = slurp(b, &n2);
    char err[200]; size_t olen = 0; uint8_t *o = a64_unpack_payload((uint8_t *)cmp, n2, &olen, err, sizeof err);
    CHECK(o && olen == n1 && !memcmp(o, in, n1), "LZSS round trip failed: %s", o ? "content differs" : err);
    printf("-- LZSS round trip: %zu -> %zu -> %zu bytes\n", n1, n2, olen); free(o);
    /* corrupt streams must be rejected, not crash */
    { /* device-tree fix-up: root with 2 props (one flagged placeholder) and one child */
        uint8_t dt[512]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        u = 2; memcpy(dt + q, &u, 4); u = 1; memcpy(dt + q + 4, &u, 4); q += 8;
        memcpy(dt + q, "name", 4); u = 5; memcpy(dt + q + 32, &u, 4); memcpy(dt + q + 36, "root", 5); q += 36 + 8;
        memcpy(dt + q, "serial-number", 13); u = 0x80000000u | 12; memcpy(dt + q + 32, &u, 4); q += 36 + 12;
        u = 1; memcpy(dt + q, &u, 4); u = 0; memcpy(dt + q + 4, &u, 4); q += 8;
        memcpy(dt + q, "x", 1); u = 0x80000000u | 4; memcpy(dt + q + 32, &u, 4); q += 36 + 4;
        char fnm[40]; int r = a64_dt_fixup(dt, q, fnm);
        CHECK(r == 2 && !strcmp(fnm, "serial-number"), "dt fixup counted %d first '%s'", r, fnm);
        uint32_t chk; memcpy(&chk, dt + 8 + 36 + 8 + 32, 4); CHECK(chk == 12, "placeholder flag not cleared (%08x)", chk);
        CHECK(a64_dt_fixup(dt, q, fnm) == 0, "second pass must find nothing");
        uint8_t bad[16] = {9, 9, 9, 9, 9, 9, 9, 9}; CHECK(a64_dt_fixup(bad, sizeof bad, fnm) == -1, "garbage tree accepted");
        CHECK(a64_dt_fixup(dt, 20, fnm) == -1, "truncated tree accepted");
    }
    { /* boot-cpu fix-up: root -> cpus -> cpu0 (reg 0x100, state "waiting") */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        #define NODE(np_, nc_) do { u = np_; memcpy(dt + q, &u, 4); u = nc_; memcpy(dt + q + 4, &u, 4); q += 8; } while (0)
        #define PROP(nm_, len_, data_) do { memcpy(dt + q, nm_, strlen(nm_)); u = len_; memcpy(dt + q + 32, &u, 4); memcpy(dt + q + 36, data_, len_); q += 36 + (((len_) + 3) & ~3); } while (0)
        NODE(1, 1); PROP("name", 5, "root");
        NODE(1, 1); PROP("name", 5, "cpus");
        uint32_t reg = 0x100; NODE(3, 0); PROP("name", 5, "cpu0"); PROP("reg", 4, &reg); PROP("state", 8, "waiting");
        char lg[600]; uint32_t rr = 0; int r = a64_dt_boot_cpu(dt, q, &rr, lg, sizeof lg);
        CHECK(r == 1 && rr == 0x100, "boot cpu fix-up r=%d reg=0x%x", r, rr);
        CHECK(strstr(lg, "cpu0 reg=0x100 state=waiting"), "cpu list: %s", lg);
        CHECK(!memcmp(dt + q - 8, "running", 8), "state not rewritten to running");
        CHECK(a64_dt_boot_cpu((uint8_t[8]){9,9,9,9,9,9,9,9}, 8, &rr, lg, sizeof lg) == -1, "malformed tree accepted");
    }
    { /* entropy fix-up: root -> chosen with an all-zero random-seed placeholder (flagged length already cleared) */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root");
        NODE(2, 0); PROP("name", 7, "chosen"); uint8_t zeros[16] = {0}; PROP("random-seed", 16, zeros);
        size_t seed_at = q - 16;
        char lg[400]; size_t nb = 0; int r = a64_dt_entropy(dt, q, ent_rng, &nb, lg, sizeof lg);
        CHECK(r == 1 && nb == 16, "entropy fill r=%d bytes=%zu", r, nb);
        CHECK(dt[seed_at] == 0xA5 && dt[seed_at + 15] == 0xA5, "seed bytes not filled");
        CHECK(strstr(lg, "random-seed(16)"), "chosen list: %s", lg);
        CHECK(a64_dt_entropy(dt, q, ent_rng, &nb, lg, sizeof lg) == 0, "non-empty seed must not be overwritten");
    }
    { /* MODELLED iBoot property insertion: /chosen gets a uint32, the sibling node behind it must survive */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 2); PROP("name", 5, "root");
        NODE(2, 0); PROP("name", 7, "chosen"); uint8_t seed[8] = {1,2,3,4,5,6,7,8}; PROP("random-seed", 8, seed);
        NODE(1, 0); PROP("name", 5, "cpus");
        size_t n0 = q, cap = sizeof dt;
        CHECK(a64_dt_chosen_add_u32(dt, &q, cap, "security-domain", 1) == 1 && q == n0 + 40, "chosen insert: added, grew by 40 (q=%zu n0=%zu)", q, n0);
        CHECK(a64_dt_chosen_add_u32(dt, &q, cap, "security-domain", 1) == 0 && q == n0 + 40, "chosen insert: second add finds it present");
        char lg[400]; size_t nb = 0; int r = a64_dt_entropy(dt, q, ent_rng, &nb, lg, sizeof lg);
        CHECK(r == 0 && strstr(lg, "random-seed(8)") && strstr(lg, "security-domain(4)"), "chosen insert: tree still well-formed, list: %s (r=%d)", lg, r);
        CHECK(!memcmp(dt + q - 8, "cpus", 5), "chosen insert: sibling node intact");
        CHECK(a64_dt_chosen_add_u32(dt, &q, q + 10, "x", 1) == -1, "chosen insert: no room is refused");
        uint8_t bad[16] = {9,9,9,9,9,9,9,9}; size_t bn = 8; CHECK(a64_dt_chosen_add_u32(bad, &bn, 16, "x", 1) == -1, "chosen insert: malformed tree refused");
    }
    { /* UART nodes: root -> arm-io (ranges) -> uart0 (compatible, reg); both layouts of "ranges" */
        for (int lay = 0; lay < 3; lay++) {
            uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
            uint32_t r16[4] = { 0, 2, 0, 0x40000000u }; uint64_t r24[3] = { 0, 0x200000000ull, 0x40000000ull }; uint64_t r48[6] = { 0, 0x200000000ull, 0x100000000ull, 0x100000000ull, 0x800000000ull, 0x1000000ull };   /* shape seen in a real tree */
            uint32_t reg[2] = { 0x35200000u, 0x4000 };
            NODE(1, 1); PROP("name", 5, "root");
            NODE(2, 1); PROP("name", 7, "arm-io"); if (lay == 0) PROP("ranges", 16, r16); else if (lay == 1) PROP("ranges", 24, r24); else PROP("ranges", 48, r48);
            NODE(3, 0); PROP("name", 6, "uart0"); PROP("compatible", 15, "uart-1,samsung"); PROP("reg", 8, reg);
            uint64_t pa[3] = {0}, sz[3] = {0}; char lg[1000]; int n = a64_dt_uarts(dt, q, pa, sz, lg, sizeof lg);
            CHECK(n == 1 && pa[0] == 0x235200000ull && sz[0] == 0x4000, "uart layout %d: n=%d pa=0x%llx sz=0x%llx log=%s", lay, n, (unsigned long long)pa[0], (unsigned long long)sz[0], lg);
            CHECK(strstr(lg, "uart0"), "uart log: %s", lg);
            CHECK(a64_dt_uarts((uint8_t[8]){9,9,9,9,9,9,9,9}, 8, pa, sz, lg, sizeof lg) == -1, "malformed tree accepted");
        }
        { /* no ranges -> no device */
            uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u; uint32_t reg[2] = { 0x35200000u, 0x4000 };
            NODE(1, 1); PROP("name", 5, "root"); NODE(1, 1); PROP("name", 7, "arm-io"); NODE(3, 0); PROP("name", 6, "uart0"); PROP("compatible", 15, "uart-1,samsung"); PROP("reg", 8, reg);
            uint64_t pa[3], sz[3]; char lg[1000]; CHECK(a64_dt_uarts(dt, q, pa, sz, lg, sizeof lg) == 0, "uart without ranges must not produce a device");
        }
    }
    { /* 0.4.53: which /arm-io node covers a physical address (reg u64 addr, u64 size; ranges u64 child/parent/size) */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u; uint64_t r24[3] = { 0, 0x200000000ull, 0x40000000ull };
        uint64_t reg1[2] = { 0x3b100000ull, 0x100000 }, reg2[2] = { 0x35200000ull, 0x4000 };
        NODE(1, 1); PROP("name", 5, "root"); NODE(2, 2); PROP("name", 7, "arm-io"); PROP("ranges", 24, r24);
        NODE(3, 0); PROP("name", 4, "aic"); PROP("compatible", 14, "aic,1\0aic,pub"); PROP("reg", 16, reg1);
        NODE(2, 0); PROP("name", 6, "uart0"); PROP("reg", 16, reg2);
        char nn[40], cc[100]; uint64_t b = 0, z = 0;
        CHECK(a64_dt_find_pa(dt, q, 0x23b100000ull, nn, sizeof nn, cc, sizeof cc, &b, &z) == 1 && !strcmp(nn, "aic") && b == 0x23b100000ull && z == 0x100000 && !strncmp(cc, "aic,1", 5), "find_pa: aic node: name=%s base=0x%llx", nn, (unsigned long long)b);
        CHECK(a64_dt_find_pa(dt, q, 0x23b1fffffull, nn, sizeof nn, cc, sizeof cc, &b, &z) == 1 && !strcmp(nn, "aic"), "find_pa: last byte of the range");
        CHECK(a64_dt_find_pa(dt, q, 0x23b200000ull, nn, sizeof nn, cc, sizeof cc, &b, &z) == 0, "find_pa: one past the range is not found");
        CHECK(a64_dt_find_pa((uint8_t[8]){9,9,9,9,9,9,9,9}, 8, 0x23b100000ull, nn, sizeof nn, cc, sizeof cc, &b, &z) == -1, "find_pa: malformed tree");
        A64Machine *m = a64m_create_system(1u << 20);   /* the named unknown block labels its log lines */
        CHECK(a64m_add_unknown_mmio_named(m, 0x23b100000ull, 0x100000, "aic") == 0, "add named block");
        uint32_t code[] = { 0xD2C00040, 0xF2A76200, 0xB9400001, 0x14000000 };   /* x0 = 0x23b100000; ldr w1,[x0]; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 40, false);
        char *lg = logof(m);
        CHECK(a64m_cpu(m)->x[0] == 0x23b100000ull && a64m_cpu(m)->xab_n == 0 && ({ char sm[400]; a64m_unknown_summary(m, sm, sizeof sm); strstr(sm, "'aic'=1r/0w") != NULL; }), "named block: read of +0 works and is labelled in the stop summary (x0=0x%llx)", (unsigned long long)a64m_cpu(m)->x[0]);
        free(lg); a64m_destroy(m);
    }
    { /* 0.4.56: property length of a named node, and a MODELLED fixed read value of an unknown block */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u; uint8_t mask[64] = {0};
        NODE(1, 1); PROP("name", 5, "root"); NODE(2, 1); PROP("name", 7, "arm-io"); PROP("ranges", 24, ((uint64_t[3]){0, 0x200000000ull, 0x40000000ull}));
        NODE(3, 0); PROP("name", 4, "aic"); PROP("ipid-mask", 64, mask); PROP("reg", 16, ((uint64_t[2]){0x3b100000ull, 0x8000}));
        int64_t pl = 0; char lg[300];
        CHECK(a64_dt_node_info(dt, q, "aic", "ipid-mask", &pl, lg, sizeof lg) == 1 && pl == 64 && strstr(lg, "ipid-mask(64)") && strstr(lg, "reg(16)"), "node_info: ipid-mask length %lld, list: %s", (long long)pl, lg);
        CHECK(a64_dt_node_info(dt, q, "aic", "nonexistent", &pl, lg, sizeof lg) == 1 && pl == -1, "node_info: absent property gives -1");
        { char w[100]; CHECK(a64_dt_node_prop_words(dt, q, "aic", "reg", w, sizeof w) == 16 && !strcmp(w, "3b100000 0 8000 0"), "prop_words: reg of aic as hex words (%s)", w);
          CHECK(a64_dt_node_prop_words(dt, q, "aic", "nonexistent", w, sizeof w) == -1 && a64_dt_node_prop_words(dt, q, "nosuch", "reg", w, sizeof w) == -2, "prop_words: absent property -1, absent node -2"); }
        CHECK(a64_dt_node_info(dt, q, "nosuchnode", "x", &pl, lg, sizeof lg) == 0, "node_info: absent node gives 0");
        CHECK(a64_dt_node_info((uint8_t[8]){9,9,9,9,9,9,9,9}, 8, "aic", "x", &pl, lg, sizeof lg) == -1, "node_info: malformed tree");
        A64Machine *m = a64m_create_system(1u << 20);
        CHECK(a64m_add_unknown_mmio_named(m, 0x23b100000ull, 0x8000, "aic") == 0 && a64m_unknown_set_read(m, 0x23b100000ull, 0x4, 512) == 0, "set_read on the block");
        CHECK(a64m_unknown_set_read(m, 0x300000000ull, 0x4, 1) == -1, "set_read on an unknown address is refused");
        CHECK(a64m_add_unknown_mmio_named(m, 0x24b000000ull, 0x400000, "pmgr") == 0 && a64m_unknown_ts_window(m, 0x24b000000ull, 0x288000) == 0 && a64m_unknown_ts_read(m, 0x24b000000ull, 0x288010, 1) == 0 && a64m_unknown_ts_read(m, 0x300000000ull, 0, 1) == -1, "0.4.150: temperature sensor window and register added inside a pmgr block, unknown block refused");
        CHECK(a64m_add_unknown_mmio_named(m, 0x200000000ull, 0x100000, "mem") == 0 && a64m_add_unknown_mmio_named(m, 0x200100000ull, 0x100000, "amcc") == 0 && a64m_add_unknown_mmio_named(m, 0x200100000ull, 0x100000, "amcc") != 0, "0.4.213: the AMCC block 0x200100000 sits right behind the 0x200000000 block (no overlap) and covers 0x200158000");
        { uint64_t st = 99; CHECK(a64m_add_unknown_mmio_named(m, 0x235100000ull, 0x4000, "i2c9") == 0, "0.4.218: i2c block added");
          a64m_unknown_rw(m, 0x235100000ull, 0, 0x14, 0, &st); CHECK(st == 0x01000000u, "0.4.219: idle i2c status has only bit 24 (SCL level) set");
          a64m_unknown_rw(m, 0x235100000ull, 1, 0x14, 0x01000000u, NULL); a64m_unknown_rw(m, 0x235100000ull, 0, 0x14, 0, &st); CHECK(st == 0x01000000u, "0.4.219: the SCL level bit survives a write-1-to-clear");
          CHECK(a64m_add_unknown_mmio_named(m, 0x235200000ull, 0x4000, "pmgr9") == 0, "0.4.218: non-i2c block added");
          a64m_unknown_rw(m, 0x235200000ull, 0, 0x14, 0, &st); CHECK(st == 0, "0.4.219: only i2c blocks get the status-level model"); }
        CHECK(a64m_add_unknown_mmio_auto(m, 0x23b100000ull, 0x8000, "aic") == -1, "0.4.59: re-adding the same block is refused");
        uint32_t code[] = { 0xD2C00040, 0xF2A76200, 0xB9400402, 0xB9400003, 0x14000000 };   /* x0 = 0x23b100000 (the 0x3b10 imm is the same as in the other test); ldr w2,[x0,#4]; ldr w3,[x0]; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 40, false);
        CHECK(a64m_cpu(m)->x[2] == 512 && a64m_cpu(m)->x[3] == 0, "fixed read: +0x4 gave %llu (want 512), +0 gave %llu (want 0)", (unsigned long long)a64m_cpu(m)->x[2], (unsigned long long)a64m_cpu(m)->x[3]);
        { char sm2[400]; a64m_unknown_summary(m, sm2, sizeof sm2); CHECK(strstr(sm2, "r+0x4=0x200"), "fixed read value 0x200 shows in the stop summary: %s", sm2); }
        a64m_destroy(m);
    }
    { /* 0.4.65: empty property added to a named node */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(2, 0); PROP("name", 5, "pmgr"); PROP("x", 4, ((uint32_t[1]){5}));
        size_t n0 = q; int r = a64_dt_node_add_empty_prop(dt, &q, sizeof dt, "pmgr", "voltage-states1");
        CHECK(r == 1 && q == n0 + 36, "add empty prop: r=%d size %zu -> %zu (want 1, +36)", r, n0, q);
        int64_t pl = -1; char lg[600]; int ni = a64_dt_node_info(dt, q, "pmgr", "voltage-states1", &pl, lg, sizeof lg);
        CHECK(ni == 1 && pl == 0 && strstr(lg, "voltage-states1(0)") && strstr(lg, "x(4)"), "added property is visible with length 0 next to the old one: ni=%d pl=%lld log=%s", ni, (long long)pl, lg);
        CHECK(a64_dt_node_add_empty_prop(dt, &q, sizeof dt, "pmgr", "voltage-states1") == 0 && q == n0 + 36, "second add is a no-op");
        CHECK(a64_dt_node_add_empty_prop(dt, &q, sizeof dt, "nosuch", "y") == -2, "unknown node -> -2");
        size_t q2 = n0 + 10; CHECK(a64_dt_node_add_empty_prop(dt, &q2, sizeof dt, "pmgr", "z") == -1, "truncated tree -> -1");
    }
    { /* 0.4.74: zero-filled property of a given length */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(2, 0); PROP("name", 7, "arm-io"); PROP("clock-frequencies", 8, ((uint32_t[2]){0, 0}));
        size_t n0 = q; int r = a64_dt_node_add_zero_prop(dt, &q, sizeof dt, "arm-io", "clock-frequencies-nclk", 8);
        CHECK(r == 1 && q == n0 + 36 + 8, "add zero prop: r=%d size %zu -> %zu (want 1, +44)", r, n0, q);
        int64_t pl = -1; char lg[600]; int ni = a64_dt_node_info(dt, q, "arm-io", "clock-frequencies-nclk", &pl, lg, sizeof lg);
        CHECK(ni == 1 && pl == 8 && strstr(lg, "clock-frequencies(8)"), "added property has length 8 next to the old one: ni=%d pl=%lld log=%s", ni, (long long)pl, lg);
        CHECK(a64_dt_node_add_zero_prop(dt, &q, sizeof dt, "arm-io", "clock-frequencies-nclk", 8) == 0 && q == n0 + 44, "second add is a no-op");
        CHECK(a64_dt_node_add_zero_prop(dt, &q, sizeof dt, "nosuch", "y", 4) == -2, "unknown node -> -2");
    }
    { /* 0.4.79: probe for lowercase dashed names passed to calls that are not DeviceTree property names */
        A64Machine *m = a64m_create_system(1u << 20);
        a64m_nm_add_dt_name(m, "present-name"); a64m_set_coverage(m, A64M_RAM_BASE, 0x1000);   /* strings count only inside the kernel image range */
        uint32_t code[] = { 0x94000002, 0x14000000, 0xD65F03C0 };   /* bl +8 ; b . ; ret */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        const char *strs[] = { "voltage-states5", "present-name", "Capital-Name", "plain", "voltage-states5", "x" };
        for (int z = 0; z < 6; z++) {
            uint64_t sa = A64M_RAM_BASE + 0x100 + (uint64_t)z * 0x40; strcpy((char *)a64m_ram_at(m, sa, 0x40), strs[z]);
            a64m_set_pc(m, A64M_RAM_BASE); a64m_cpu(m)->x[1] = sa; a64m_run(m, 3, false);
        }
        A64 *cp = a64m_cpu(m);
        CHECK(cp->nm_n == 1 && !strcmp(cp->nm[0], "voltage-states5"), "probe lists exactly the missing dashed name once: n=%u first=%s", cp->nm_n, cp->nm_n ? cp->nm[0] : "-");
        a64m_destroy(m);
    }
    { /* 0.4.82: property with data */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(1, 0); PROP("name", 5, "cpu0");
        uint32_t v = 24000000u; int r = a64_dt_node_add_prop(dt, &q, sizeof dt, "cpu0", "fixed-frequency", &v, 4);
        char w[40]; int pl = a64_dt_node_prop_words(dt, q, "cpu0", "fixed-frequency", w, sizeof w);
        CHECK(r == 1 && pl == 4 && !strcmp(w, "16e3600"), "added property keeps its data: r=%d len=%d words=%s", r, pl, w);
    }
    { /* 0.4.222: an EMPTY (0-byte) property can be added; the next node is intact */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(1, 0); PROP("name", 5, "apcie");
        uint32_t d0 = 0; size_t n0 = q; int r = a64_dt_node_add_prop(dt, &q, sizeof dt, "apcie", "apcie-phy-tunables", &d0, 0);
        char w[40]; int pl = a64_dt_node_prop_words(dt, q, "apcie", "apcie-phy-tunables", w, sizeof w);
        CHECK(r == 1 && pl == 0 && q > n0, "empty tunables property added: r=%d len=%d size %zu->%zu", r, pl, n0, q);
    }
    { /* 0.4.85: fill an existing EMPTY property; video size setting */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(3, 0); PROP("name", 5, "pmgr"); PROP("voltage-states9", 0, ""); PROP("x", 4, ((uint32_t[1]){7}));
        size_t n0 = q; int r = a64_dt_node_fill_empty_prop(dt, &q, sizeof dt, "pmgr", "voltage-states9", NULL, 8);
        char w[40]; int pl = a64_dt_node_prop_words(dt, q, "pmgr", "voltage-states9", w, sizeof w); char w2[40]; int pl2 = a64_dt_node_prop_words(dt, q, "pmgr", "x", w2, sizeof w2);
        CHECK(r == 1 && q == n0 + 8 && pl == 8 && !strcmp(w, "0 0") && pl2 == 4 && !strcmp(w2, "7"), "empty property filled, the next one is intact: r=%d size %zu->%zu len=%d words=%s next=%s", r, n0, q, pl, w, w2);
        CHECK(a64_dt_node_fill_empty_prop(dt, &q, sizeof dt, "pmgr", "voltage-states9", NULL, 8) == 0, "a non-empty property is left alone");
        CHECK(a64_dt_node_fill_empty_prop(dt, &q, sizeof dt, "pmgr", "nosuch", NULL, 8) == 0 && a64_dt_node_fill_empty_prop(dt, &q, sizeof dt, "nonode", "x", NULL, 8) == -2, "absent property 0, absent node -2");
        a64m_set_video_size(1080, 1920); CHECK(a64m_video_w() == 1080 && a64m_video_h() == 1920, "video size setting");
        a64m_set_video_size(5, 99999); CHECK(a64m_video_w() == A64M_VID_W && a64m_video_h() == A64M_VID_H, "nonsense video size falls back to the default");
        a64m_set_video_size(A64M_VID_W, A64M_VID_H);
    }
    { /* 0.4.64: pmgr optional bridges fix-up */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(4, 0);
        PROP("name", 5, "pmgr"); PROP("#bridges", 4, ((uint32_t[1]){3})); PROP("optional-bridge-mask", 4, ((uint32_t[1]){0x2})); PROP("bridge-settings-1", 4, ((uint32_t[1]){7}));
        uint32_t nb = 0, om = 0, nm = 0, mi = 0; int r = a64_dt_pmgr_optional_bridges(dt, q, &nb, &om, &nm, &mi);
        CHECK(r == 1 && nb == 3 && om == 2 && nm == 7 && mi == 2, "pmgr fix-up: r=%d nb=%u old=0x%x new=0x%x missing=%u (want 1 3 0x2 0x7 2)", r, nb, om, nm, mi);
        r = a64_dt_pmgr_optional_bridges(dt, q, &nb, &om, &nm, &mi);
        CHECK(r == 0 && om == 7, "pmgr fix-up is idempotent: r=%d old=0x%x (want 0, 0x7: the tree itself was changed)", r, om);
        CHECK(a64_dt_pmgr_optional_bridges((uint8_t[8]){9,9,9,9,9,9,9,9}, 8, &nb, &om, &nm, &mi) == -1, "pmgr fix-up: malformed tree");
    }
    { /* 0.4.61: read-back block (AppleDAPF writes 0x80000001 to +0 and requires the same value back) */
        A64Machine *m = a64m_create_system(1u << 20);
        CHECK(a64m_add_unknown_mmio_named(m, 0x23b100000ull, 0x8000, "dapf-x") == 0 && a64m_unknown_set_readback(m, 0x23b100000ull) == 0, "readback block added");
        CHECK(a64m_unknown_set_readback(m, 0x300000000ull) == -1, "readback on an unknown address is refused");
        uint32_t code[] = { 0xD2C00040, 0xF2A76200, 0x52824681, 0xB9000801, 0xB9400802, 0xB9400403, 0x14000000 };   /* x0 = 0x23b100000; w1 = 0x1234; str w1,[x0,#8]; ldr w2,[x0,#8]; ldr w3,[x0,#4]; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 40, false);
        CHECK(a64m_cpu(m)->x[2] == 0x1234 && a64m_cpu(m)->x[3] == 0, "read-back: +8 gave 0x%llx (want 0x1234), unwritten +4 gave 0x%llx (want 0)", (unsigned long long)a64m_cpu(m)->x[2], (unsigned long long)a64m_cpu(m)->x[3]);
        a64m_destroy(m);
    }
    { /* 0.4.122: RAMDisk property patch and BuildManifest path lookup */
        uint8_t d[8 + 36 + 16 + 8 + 36 + 16]; memset(d, 0, sizeof d); uint32_t np = 2, nc = 0, l16 = 16; memcpy(d, &np, 4); memcpy(d + 4, &nc, 4);
        strcpy((char *)d + 8, "name"); l16 = 16; memcpy(d + 8 + 32, &l16, 4); strcpy((char *)d + 8 + 36, "memory-map");
        strcpy((char *)d + 8 + 36 + 16, "RAMDisk"); memcpy(d + 8 + 36 + 16 + 32, &l16, 4);
        CHECK(a64_dt_patch_u64x2(d, sizeof d, "RAMDisk", 0x1122334455ull, 0x6677ull) == 1, "RAMDisk patched");
        uint64_t a, b; memcpy(&a, d + 8 + 36 + 16 + 36, 8); memcpy(&b, d + 8 + 36 + 16 + 44, 8);
        CHECK(a == 0x1122334455ull && b == 0x6677ull, "RAMDisk data is {0x%llx, 0x%llx}", (unsigned long long)a, (unsigned long long)b);
        CHECK(a64_dt_patch_u64x2(d, sizeof d, "Nothing", 1, 2) == 0, "unknown property is not patched");
        const char *pl = "<dict><key>RestoreRamDisk</key>\n<string>Production</string></dict><dict><key>RestoreRamDisk</key>\n<dict><key>Info</key><dict><key>Path</key><string>048-16246-204.dmg</string></dict></dict></dict><key>Path</key><string>other</string>";
        char o[64] = "";
        CHECK(a64_plist_first_path(pl, strlen(pl), "RestoreRamDisk", o, sizeof o) == 1 && !strcmp(o, "048-16246-204.dmg"), "path found: '%s'", o);
        CHECK(a64_plist_first_path(pl, strlen(pl), "UpdateRamDisk", o, sizeof o) == 0, "missing key reports 0");
        CHECK(a64_plist_first_path(pl, strlen(pl), "RestoreRamDisk", o, 5) == 0, "too small output buffer reports 0");
    }
    { /* 0.4.137: identical consecutive guest lines are collapsed */
        A64Machine *m = a64m_create_system(1u << 20); a64m_log_collapse(m, true);
        const char *txt = "T\nT\nT\nT\nU\nV\nV\n"; uint32_t code[64]; int nc = 0;
        code[nc++] = 0xD2A12001;   /* movz x1, #0x0900, lsl #16 -> UART base 0x09000000 */
        for (const char *q = txt; *q; q++) { code[nc++] = 0x52800002u | ((uint32_t)(unsigned char)*q << 5); code[nc++] = 0x39000022u; }   /* movz w2,#ch ; strb w2,[x1] */
        code[nc++] = 0x14000000;
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, (size_t)nc * 4), code, (size_t)nc * 4);
        a64m_run(m, 100, false); a64m_log_flush(m);
        char buf[600]; size_t n = a64m_log_read_kind(m, 1, 0, buf, sizeof buf - 1); buf[n] = 0;
        CHECK(!strcmp(buf, "T\n[last line repeated 3 more times]\nU\nV\n[last line repeated 1 more time]\n"), "collapsed guest log is '%s'", buf);
        a64m_destroy(m);
    }
    { /* 0.4.135: mode link (memory controller cache mode request/status) */
        A64Machine *m = a64m_create_system(1u << 20);
        CHECK(a64m_add_unknown_mmio_named(m, 0x200000000ull, 0x100000, "mem") == 0, "mem block added");
        CHECK(a64m_unknown_set_modelink(m, 0x200000000ull, 0x1c00, 0x1c04) == 0 && a64m_unknown_set_modelink(m, 0x200000000ull, 0x1c00, 0x41c04) == 0, "links added");
        CHECK(a64m_unknown_set_modelink(m, 0x300000000ull, 0x1c00, 0x1c04) == -1, "link on an unknown address is refused");
        /* x0 = 0x200000000 + 0x1000 + 0xc00; w1 = 5; str w1,[x0]; ldr w2,[x0,#4]; x3 = x0 + 0x40000; ldr w4,[x3,#4]; ldr w5,[x0,#8]; b . */
        uint32_t code[] = { 0xD2C00040, 0x91400400, 0x91300000, 0x528000A1, 0xB9000001, 0xB9400402, 0x91410003, 0xB9400464, 0xB9400805, 0x14000000 };
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 80, false);
        CHECK(a64m_cpu(m)->x[2] == 0x14a0 && a64m_cpu(m)->x[4] == 0x14a0 && a64m_cpu(m)->x[5] == 0, "mode link: status reads 0x%llx / 0x%llx (want 0x14a0), unrelated register reads 0x%llx (want 0)", (unsigned long long)a64m_cpu(m)->x[2], (unsigned long long)a64m_cpu(m)->x[4], (unsigned long long)a64m_cpu(m)->x[5]);
        a64m_destroy(m);
    }
    { /* 0.4.110: power-state mirror (ApplePMGR::waitReg32 waits for (reg & 0xf0) == 0xf0 after writing 0xf) */
        A64Machine *m = a64m_create_system(1u << 20);
        CHECK(a64m_add_unknown_mmio_named(m, 0x23b000000ull, 0x100000, "pmgr") == 0 && a64m_unknown_set_psmirror(m, 0x23b000000ull) == 0, "psmirror block added");
        CHECK(a64m_unknown_set_psmirror(m, 0x300000000ull) == -1, "psmirror on an unknown address is refused");
        /* x0 = 0x23b000000; x0 += 0x80000 + 0x2d8 ; w1 = 0xf; str w1,[x0]; ldr w2,[x0]; ldr w3,[x0,#4]; b . */
        uint32_t code[] = { 0xD2C00040, 0xF2A76000, 0x91400000 | (0x80u << 10) | (0u << 5), 0x910B6000, 0x528001E1, 0xB9000001, 0xB9400002, 0xB9400403, 0x14000000 };
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 60, false);
        CHECK(a64m_cpu(m)->x[2] == 0xff && a64m_cpu(m)->x[3] == 0, "psmirror: written 0xf reads 0x%llx (want 0xff), neighbour reads 0x%llx (want 0)", (unsigned long long)a64m_cpu(m)->x[2], (unsigned long long)a64m_cpu(m)->x[3]);
        a64m_destroy(m);
    }
    { /* 0.4.57: all /arm-io nodes with a physical range, and the stop report naming the node that owns the first unmapped access */
        uint8_t dt[1024]; memset(dt, 0, sizeof dt); size_t q = 0; uint32_t u;
        NODE(1, 1); PROP("name", 5, "root"); NODE(2, 2); PROP("name", 7, "arm-io"); PROP("ranges", 24, ((uint64_t[3]){0, 0x200000000ull, 0x40000000ull}));
        NODE(3, 0); PROP("name", 4, "aic"); PROP("compatible", 14, "aic,1\0aic,pub"); PROP("reg", 16, ((uint64_t[2]){0x3b100000ull, 0x8000}));
        NODE(3, 0); PROP("name", 4, "wdt"); PROP("compatible", 8, "wdt,xyz"); PROP("reg", 16, ((uint64_t[2]){0x3d1f0000ull, 0x4000}));
        A64SocNode nodes[8]; int nn = a64_dt_soc_nodes(dt, q, nodes, 8);
        CHECK(nn == 2 && !strcmp(nodes[0].name, "aic") && nodes[1].pa == 0x23d1f0000ull && nodes[1].size == 0x4000 && !strcmp(nodes[1].compat, "wdt,xyz"), "soc_nodes: n=%d second pa=0x%llx", nn, (unsigned long long)nodes[1].pa);
        CHECK(a64_dt_soc_nodes((uint8_t[8]){9,9,9,9,9,9,9,9}, 8, nodes, 8) == -1, "soc_nodes: malformed tree");
        A64Machine *m = a64m_create_system(1u << 20);
        a64m_set_soc_nodes(m, nodes, nn); a64m_mark_attempt(m, A64M_RAM_BASE, A64M_RAM_BASE);   /* the stop report is only written in attempt mode */
        uint32_t code[] = { 0xD2C00040, 0xF2A7A3E0, 0xB9400001, 0x14000000 };   /* x0 = 0x2_3d1f_0000; ldr w1,[x0]; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 60000000, false);
        char *l3 = logof(m);
        CHECK(a64m_cpu(m)->x[0] == 0x23d1f0000ull, "report test: x0 = 0x%llx", (unsigned long long)a64m_cpu(m)->x[0]);
        CHECK(a64m_cpu(m)->xab_n >= 1, "report test: the read hit no device (xab_n=%u)", a64m_cpu(m)->xab_n);
        CHECK(strstr(l3, "belongs to DeviceTree node 'wdt' (compatible 'wdt,xyz', base 0x23d1f0000, size 0x4000; offset +0x0)"), "stop report names the DT node that owns the unmapped address");
        free(l3); a64m_destroy(m);
    }
    { /* 0.4.72: the stop report finds a text in the kernel image and the code that references it (ADRP+ADD) */
        A64Machine *m = a64m_create_system(1u << 20); a64m_mark_attempt(m, A64M_RAM_BASE, A64M_RAM_BASE); a64m_set_coverage(m, A64M_RAM_BASE, 0x1000);
        uint32_t code[] = { 0x90000000, 0x91200000, 0x14000000 };   /* adrp x0,#0 (page of RAM base); add x0,x0,#0x800; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        memcpy(a64m_ram_at(m, A64M_RAM_BASE + 0x800, 40), "Drv::start: clock frequencies not published\n", 44 > 40 ? 40 : 44);
        memcpy(a64m_ram_at(m, A64M_RAM_BASE + 0x800, 12), "Drv::start: ", 12);
        a64m_set_diag_string(m, 0, "clock frequencies", 0x8, 0x8); a64m_set_diag_string(m, 1, "no such text anywhere", 0x8, 0x8);
        a64m_run(m, 60000000, false);
        char *l4 = logof(m);
        CHECK(strstr(l4, "text \"clock frequencies\" is at kernel virtual 0x40000800; first code that references it: 0x40000000"), "string ref: found the text and its ADRP+ADD reference");
        CHECK(strstr(l4, "text \"no such text anywhere\" not found in the kernel image"), "string ref: a missing text is reported as not found");
        free(l4); a64m_destroy(m);
        a64m_set_diag_string(NULL, 0, "", 0, 0); a64m_set_diag_string(NULL, 1, "", 0, 0);
    }
    { /* 0.4.58: bulk (auto) unknown blocks: only the first few accesses are logged, the summary counts all of them; device table holds hundreds */
        A64Machine *m = a64m_create_system(1u << 20); int okn = 0;
        okn += a64m_add_unknown_mmio_auto(m, 0x23b100000ull, 0x4000, "zz") == 0;
        for (int z = 1; z < 300; z++) okn += a64m_add_unknown_mmio_auto(m, 0x230000000ull + (uint64_t)z * 0x10000, 0x4000, "bulk") == 0;
        CHECK(okn == 300, "300 auto blocks fit (%d)", okn);
        uint32_t code[20]; int ci = 0; code[ci++] = 0xD2C00040; code[ci++] = 0xF2A76200;
        for (int z = 0; z < 10; z++) code[ci++] = 0xB9400401;   /* ldr w1,[x0,#4] */
        for (int z = 0; z < 3; z++) code[ci++] = 0xB9000801;    /* str w1,[x0,#8] */
        code[ci++] = 0x14000000;
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 80, false);
        char *lg = logof(m); int lines = 0; for (const char *p = lg; (p = strstr(p, "block 'zz': read")); p++) lines++;
        char sm[400]; int cnt = a64m_unknown_summary(m, sm, sizeof sm);
        { int firsts = 0; for (const char *p = sm; (p = strstr(p, "r+0x4=")); p++) firsts++; CHECK(lines == 0 && firsts == 6, "no per-access log lines (%d); the summary keeps only the first 6 accesses (%d)", lines, firsts); }
        CHECK(cnt == 1 && strstr(sm, "'zz'=10r/3w(last+0x8;"), "summary lists the touched block with counts: [%d]%s", cnt, sm);
        free(lg); a64m_destroy(m);
    }
    { /* 0.4.55: the system machine can hold more than 8 devices (it ran out at the 9th: 4 board devices + 3 UARTs + aic left no room for the memory-controller block) */
        A64Machine *m = a64m_create_system(1u << 20); int okn = 0;
        okn += a64m_add_s5l_uart(m, 0x235200000ull, 0x4000) == 0; okn += a64m_add_s5l_uart(m, 0x235204000ull, 0x4000) == 0; okn += a64m_add_s5l_uart(m, 0x23520c000ull, 0x4000) == 0;
        okn += a64m_add_unknown_mmio_named(m, 0x23b100000ull, 0x8000, "aic") == 0; okn += a64m_add_unknown_mmio(m, 0x200000000ull, 0x100000) == 0;
        CHECK(okn == 5, "all five SoC devices fit next to the board devices (%d of 5)", okn);
        a64m_destroy(m);
    }
    { /* the S5L UART model: a guest stores 'H' to UTXH and reads UTRSTAT (tx empty = 0x6) at the address given */
        A64Machine *m = a64m_create_system(1u << 20);
        CHECK(a64m_add_s5l_uart(m, 0x235200000ull, 0x4000) == 0, "add uart");
        uint32_t code[] = { 0xD2C00040, 0xF2A6A400, 0x52800901, 0xB9002001, 0xB9401002, 0x14000000 };
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 40, false);
        char b[64]; size_t k = a64m_log_read_kind(m, 1, 0, b, sizeof b - 1); b[k] = 0;
        CHECK(!strcmp(b, "H"), "UART output was '%s'", b);
        CHECK(a64m_cpu(m)->x[2] == 6, "UTRSTAT read gave 0x%llx", (unsigned long long)a64m_cpu(m)->x[2]);
        a64m_destroy(m);
    }
    { /* MODELLED unknown SoC block: reads 0, writes ignored, accesses logged (no external abort) */
        A64Machine *m = a64m_create_system(1u << 20);
        CHECK(a64m_add_unknown_mmio(m, 0x200000000ull, 0x4000) == 0, "add unknown block");
        uint32_t code[] = { 0xD2800040, 0xD3607C00, 0xD2801FE1, 0xB9468001, 0x52800AA2, 0xB9068402, 0x14000000 };   /* x0=0x200000000; x1=0xff; ldr w1,[x0,#0x680]; mov w2,#0x55; str w2,[x0,#0x684]; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 60, false);
        CHECK(a64m_cpu(m)->x[0] == 0x200000000ull, "x0 = 0x%llx", (unsigned long long)a64m_cpu(m)->x[0]);
        CHECK(a64m_cpu(m)->x[1] == 0, "unknown block read gave 0x%llx, want 0", (unsigned long long)a64m_cpu(m)->x[1]);
        CHECK(a64m_cpu(m)->xab_n == 0, "unknown block caused an external abort");
        char b[600]; size_t k = a64m_log_read(m, 0, b, sizeof b - 1); b[k] = 0;
        { char sm[600]; a64m_unknown_summary(m, sm, sizeof sm); CHECK(strstr(sm, "1r/1w") && strstr(sm, "r+0x680=0x0") && strstr(sm, "w+0x684=0x55") && !strstr(b, "unknown SoC block"), "access summary missing or log noisy: %s", sm); }
        a64m_destroy(m);
    }
    { /* EL1 physical timer registers (the kernel writes CNTP_CTL_EL0 right after the UART is up) */
        A64Machine *m = a64m_create_system(1u << 20);
        uint32_t code[] = { 0xD2800020, 0xD51BE220, 0xD53BE221, 0xD2800F02, 0xD51BE242, 0xD53BE243, 0x14000000 };   /* mov x0,#1; msr cntp_ctl,x0; mrs x1,cntp_ctl; mov x2,#0x78; msr cntp_cval,x2; mrs x3,cntp_cval; b . */
        memcpy(a64m_ram_at(m, A64M_RAM_BASE, sizeof code), code, sizeof code);
        a64m_run(m, 40, false);
        CHECK(a64m_cpu(m)->x[1] == 5, "CNTP_CTL read 0x%llx (want enable|istatus)", (unsigned long long)a64m_cpu(m)->x[1]);
        CHECK(a64m_cpu(m)->x[3] == 0x78, "CNTP_CVAL read 0x%llx", (unsigned long long)a64m_cpu(m)->x[3]);
        a64m_destroy(m);
    }
    uint8_t junk[64] = "bvx2\xff\xff\xff\xff"; o = a64_unpack_payload(junk, sizeof junk, &olen, err, sizeof err); CHECK(!o, "corrupt LZFSE accepted"); free(o);
    memcpy(junk, "complzss", 8); o = a64_unpack_payload(junk, sizeof junk, &olen, err, sizeof err); CHECK(!o, "short complzss accepted"); free(o);
    printf(fails ? "ATTEMPT TESTS FAILED (%d)\n" : "attempt tests ok\n", fails);
    return fails != 0;
}
