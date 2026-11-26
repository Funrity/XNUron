#define _GNU_SOURCE
#include "attempt.h"
#include "macho.h"
#include "lzfse/lzfse.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>

#define MAX_COMPRESSED   (160ull << 20)
#define MAX_KERNEL       (400ull << 20)
#define MAX_RAM          (768ull << 20)

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) | (uint32_t)r16(p + 2) << 16; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static bool has(const char *s, const char *sub) {
    size_t n = strlen(sub), l = strlen(s);
    for (size_t i = 0; i + n <= l; i++) if (!strncasecmp(s + i, sub, n)) return true;
    return false;
}

/* ------------------------------------------------------------------ LZSS */
size_t a64_lzss_decode(const uint8_t *src, size_t sl, uint8_t *dst, size_t cap) {
    enum { N = 4096, F = 18, T = 2 };
    uint8_t buf[N + F - 1]; memset(buf, ' ', sizeof buf);
    size_t r = N - F, si = 0, di = 0; unsigned flags = 0;
    for (;;) {
        if (((flags >>= 1) & 0x100) == 0) { if (si >= sl) break; flags = src[si++] | 0xFF00u; }
        if (flags & 1) {
            if (si >= sl || di >= cap) break;
            uint8_t c = src[si++]; dst[di++] = c; buf[r++] = c; r &= N - 1;
        } else {
            if (si + 2 > sl) break;
            unsigned i = src[si++], j = src[si++]; i |= (j & 0xF0u) << 4; j = (j & 0x0Fu) + T;
            for (unsigned k = 0; k <= j; k++) {
                if (di >= cap) return di;
                uint8_t c = buf[(i + k) & (N - 1)]; dst[di++] = c; buf[r++] = c; r &= N - 1;
            }
        }
    }
    return di;
}

uint8_t *a64_unpack_payload(const uint8_t *d, size_t n, size_t *out_len, char *err, size_t ec) {
    if (n >= 4 && (!memcmp(d, "bvx2", 4) || !memcmp(d, "bvxn", 4) || !memcmp(d, "bvx-", 4) || !memcmp(d, "bvx1", 4))) {
        size_t cap = n * 4 < (1u << 20) ? (1u << 20) : n * 4;
        for (;;) {
            if (cap > MAX_KERNEL) { snprintf(err, ec, "LZFSE output would exceed %llu MiB", (unsigned long long)(MAX_KERNEL >> 20)); return NULL; }
            uint8_t *o = malloc(cap); if (!o) { snprintf(err, ec, "out of memory (%zu bytes)", cap); return NULL; }
            size_t got = lzfse_decode_buffer(o, cap, d, n, NULL);
            if (got == 0) { free(o); snprintf(err, ec, "LZFSE decoder rejected the stream (corrupt or truncated)"); return NULL; }
            if (got < cap) { *out_len = got; return o; }
            free(o); cap *= 2;                       /* output buffer was too small: retry */
        }
    }
    if (n >= 0x180 && !memcmp(d, "complzss", 8)) {
        uint32_t usz = be32(d + 12), csz = be32(d + 16);
        if (!usz || usz > MAX_KERNEL || 0x180ull + csz > n) { snprintf(err, ec, "complzss header is inconsistent (size %u, compressed %u)", usz, csz); return NULL; }
        uint8_t *o = malloc(usz); if (!o) { snprintf(err, ec, "out of memory"); return NULL; }
        size_t got = a64_lzss_decode(d + 0x180, csz, o, usz);
        if (got != usz) { free(o); snprintf(err, ec, "LZSS produced %zu bytes, header says %u", got, usz); return NULL; }
        *out_len = got; return o;
    }
    snprintf(err, ec, "payload is not LZFSE or complzss"); return NULL;
}

/* ------------------------------------------------------------------- ZIP */
typedef struct { char name[256]; uint16_t method; uint64_t csz, usz, lho; } ZEnt;

static int zip_find(A64ReadAt rd, void *ud, uint64_t total, bool (*pick)(const char *), int skip_rank, ZEnt *out, char *why, size_t wc) {
    size_t tail = total < 70000 ? (size_t)total : 70000;
    uint8_t *t = malloc(tail); if (!t) { snprintf(why, wc, "out of memory"); return -1; }
    if (rd(ud, total - tail, t, tail)) { free(t); snprintf(why, wc, "cannot read end of file"); return -1; }
    long e = -1; for (long k = (long)tail - 22; k >= 0; k--) if (r32(t + k) == 0x06054b50u) { e = k; break; }
    if (e < 0) { free(t); snprintf(why, wc, "not a ZIP (no end-of-central-directory)"); return -1; }
    uint64_t cnt = r16(t + e + 10), cdsz = r32(t + e + 12), cdoff = r32(t + e + 16);
    if ((cnt == 0xFFFF || cdsz == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) && e >= 20 && r32(t + e - 20) == 0x07064b50u) {
        uint64_t z = r64(t + e - 20 + 8); uint8_t b[56];
        if (rd(ud, z, b, 56) == 0 && r32(b) == 0x06064b50u) { cnt = r64(b + 32); cdsz = r64(b + 40); cdoff = r64(b + 48); }
    }
    free(t);
    if (cdoff + cdsz > total || cdsz > (256u << 20)) { snprintf(why, wc, "central directory out of range"); return -1; }
    uint8_t *cd = malloc(cdsz ? cdsz : 1); if (!cd || rd(ud, cdoff, cd, cdsz)) { free(cd); snprintf(why, wc, "cannot read central directory"); return -1; }
    uint64_t pos = 0, seen = 0; int found = -1; int best = 1 << 30; (void)skip_rank;
    while (pos + 46 <= cdsz && seen < cnt && r32(cd + pos) == 0x02014b50u) {
        uint16_t method = r16(cd + pos + 10), nl = r16(cd + pos + 28), xl = r16(cd + pos + 30), cl = r16(cd + pos + 32);
        uint64_t csz = r32(cd + pos + 20), usz = r32(cd + pos + 24), lho = r32(cd + pos + 42);
        if (pos + 46 + nl + xl + cl > cdsz) break;
        char name[256]; size_t k = nl < sizeof name - 1 ? nl : sizeof name - 1; memcpy(name, cd + pos + 46, k); name[k] = 0;
        const uint8_t *x = cd + pos + 46 + nl;
        for (const uint8_t *q = x; q + 4 <= x + xl;) {
            uint16_t id = r16(q), sz = r16(q + 2); const uint8_t *v = q + 4, *ve = q + 4 + sz;
            if (id == 1) {
                if (usz == 0xFFFFFFFFu && v + 8 <= ve) { usz = r64(v); v += 8; }
                if (csz == 0xFFFFFFFFu && v + 8 <= ve) { csz = r64(v); v += 8; }
                if (lho == 0xFFFFFFFFu && v + 8 <= ve) { lho = r64(v); }
            }
            q += 4 + sz;
        }
        if (pick(name)) {
            int rank = (int)strlen(name);             /* shortest matching name wins: stable, no guessing by order */
            if (rank < best) { best = rank; found = 0; memcpy(out->name, name, sizeof out->name); out->method = method; out->csz = csz; out->usz = usz; out->lho = lho; }
        }
        pos += 46 + nl + xl + cl; seen++;
    }
    free(cd);
    if (found < 0) snprintf(why, wc, "no matching member in the archive");
    return found;
}

static bool pick_kernel(const char *n) { return has(n, "kernelcache") && !has(n, ".plist") && !has(n, "restore"); }
static bool pick_kernel_any(const char *n) { return has(n, "kernelcache") && !has(n, ".plist"); }
/* iBoot-equivalent device-tree fix-up (a MODELLED subset, labelled as such in the log): in Apple's flattened tree a property
 * length with bit 31 set marks a placeholder that iBoot resolves before handing the tree over. The kernel's own walker does
 * not mask that bit, so an untouched tree makes it step 2 GiB past the property (observed: pointer 0xc0010078). Here the
 * flag is only cleared; the data bytes stay exactly as the IPSW contains them (no values are invented).
 * Layout: node = u32 nprops, u32 nchildren; property = char name[32], u32 length, data padded to 4. Returns the number of
 * flagged properties, or -1 if the blob is not a well-formed tree (then nothing is modified). */
static int dt_walk(uint8_t *d, size_t n, size_t *pos, int depth, int apply, char *first, size_t firstcap, int *count) {
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4);
        if (len & 0x80000000u) {
            if (!*count && first) { memcpy(first, d + *pos, 32); first[32] = 0; (void)firstcap; }
            (*count)++;
            if (apply) { uint32_t l2 = len & 0x7FFFFFFFu; memcpy(d + *pos + 32, &l2, 4); }
            len &= 0x7FFFFFFFu;
        }
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        *pos += adv;
    }
    for (uint32_t i = 0; i < nc; i++) if (dt_walk(d, n, pos, depth + 1, apply, first, firstcap, count) < 0) return -1;
    return 0;
}
int a64_dt_fixup(uint8_t *d, size_t n, char *first_name) {
    int cnt = 0; size_t pos = 0;
    if (first_name) first_name[0] = 0;
    if (dt_walk(d, n, &pos, 0, 0, first_name, 33, &cnt) < 0) return -1;
    if (!cnt) return 0;
    int c2 = 0; pos = 0; dt_walk(d, n, &pos, 0, 1, NULL, 0, &c2);
    return cnt;
}

/* Second iBoot-equivalent fix-up (MODELLED): the kernel panics "unable to determine boot cpu!" unless exactly the boot CPU's node under /cpus
 * carries state = "running" (iBoot sets that). Here the first /cpus child that has a "state" property is marked "running" (same 8 bytes,
 * same length), and its "reg" is returned so the emulated MPIDR_EL1 can be made to agree with it. Every cpu node found is logged. */
typedef struct { size_t off[96]; int n; } DtProps;
static const uint8_t *dt_prop(const uint8_t *d, const DtProps *p, const char *name, uint32_t *len, size_t *at) {
    for (int i = 0; i < p->n; i++) if (!strncmp((const char *)d + p->off[i], name, 32)) { uint32_t l; memcpy(&l, d + p->off[i] + 32, 4); l &= 0x7FFFFFFFu; if (len) *len = l; if (at) *at = p->off[i] + 36; return d + p->off[i] + 36; }
    return NULL;
}
typedef struct { int found; int logged; uint32_t reg; char log[1500]; size_t logn; } DtCpu;
static int dt_cpus_walk(uint8_t *d, size_t n, size_t *pos, int depth, const char *parent, DtCpu *cc) {
    DtProps p; p.n = 0;
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (p.n < 96) p.off[p.n++] = *pos;
        *pos += adv;
    }
    char nm[33] = ""; uint32_t l = 0; const uint8_t *v = dt_prop(d, &p, "name", &l, NULL);
    if (v) { size_t k = l < 32 ? l : 32; memcpy(nm, v, k); nm[k] = 0; }
    if (depth == 2 && !strcmp(parent, "cpus")) {
        uint32_t rl = 0, sl = 0; size_t sat = 0; const uint8_t *rv = dt_prop(d, &p, "reg", &rl, NULL), *sv = dt_prop(d, &p, "state", &sl, &sat);
        uint32_t reg = 0; if (rv && rl >= 4) memcpy(&reg, rv, 4);
        char st[16] = "-"; if (sv && sl) { size_t k = sl < 15 ? sl : 15; memcpy(st, sv, k); st[k] = 0; }
        if (cc->logn < sizeof cc->log - 120) cc->logn += (size_t)snprintf(cc->log + cc->logn, sizeof cc->log - cc->logn, " [%s reg=0x%x state=%s]", nm, reg, st);
        if (!cc->found && sv && sl >= 8) { memcpy(d + sat, "running\0", 8); cc->found = 1; cc->reg = reg; }
    }
    for (uint32_t i = 0; i < nc; i++) if (dt_cpus_walk(d, n, pos, depth + 1, nm, cc) < 0) return -1;
    return 0;
}
/* Returns 1 and sets *reg if a boot cpu node was marked, 0 if none found, -1 if malformed. log gets the cpu list. */
int a64_dt_boot_cpu(uint8_t *d, size_t n, uint32_t *reg, char *log, size_t logcap) {
    DtCpu *cc = calloc(1, sizeof *cc); if (!cc) return -1;
    size_t pos = 0; int r = dt_cpus_walk(d, n, &pos, 0, "", cc);
    if (log && logcap) snprintf(log, logcap, "%s", cc->log);
    int res = r < 0 ? -1 : cc->found; if (res == 1) *reg = cc->reg;
    free(cc); return res;
}


/* Fourth iBoot-era fact taken from the tree (not invented): the console UART(s). The tree's /arm-io node carries "ranges" (child address -> SoC base) and each
 * uart* node a "reg"; the physical address is reg - child + parent. Both common layouts of "ranges" are understood (16 bytes: u32 child, u32+u32 parent, u32 size;
 * 24 bytes: u64 child, u64 parent, u64 size); anything else is logged raw and no device is made. */
typedef struct { uint64_t child[4], parent[4], size[4]; int nent; int have; char rawr[100]; int n; uint64_t pa[3], sz[3]; char log[900]; size_t logn;
  uint64_t want; int found; uint64_t fpa, fsz; char fname[40], fcompat[100]; A64SocNode *coll; int ncoll, maxcoll; } DtUart;
static int dt_uart_walk(uint8_t *d, size_t n, size_t *pos, int depth, DtUart *uu) {
    DtProps p; p.n = 0;
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (p.n < 96) p.off[p.n++] = *pos;
        *pos += adv;
    }
    char nm[33] = ""; uint32_t l = 0; const uint8_t *v = dt_prop(d, &p, "name", &l, NULL);
    if (v) { size_t k = l < 32 ? l : 32; memcpy(nm, v, k); nm[k] = 0; }
    if (depth == 1 && !strcmp(nm, "arm-io")) {
        uint32_t rl = 0; const uint8_t *rv = dt_prop(d, &p, "ranges", &rl, NULL);
        if (rv) {
            for (uint32_t k = 0; k < rl && k < 48; k++) snprintf(uu->rawr + 2 * k, 3, "%02x", rv[k]);
            if (rl && rl % 24 == 0) {   /* seen in a real iPhone XS tree: 48 bytes = two entries of u64 child, u64 parent, u64 size */
                for (uint32_t k = 0; k < rl / 24 && k < 4; k++) { uint64_t q[3]; memcpy(q, rv + 24 * k, 24); uu->child[k] = q[0]; uu->parent[k] = q[1]; uu->size[k] = q[2]; uu->nent++; }
                uu->have = 1;
            } else if (rl && rl % 16 == 0) {   /* u32 child, u32+u32 parent, u32 size */
                for (uint32_t k = 0; k < rl / 16 && k < 4; k++) { uint32_t w[4]; memcpy(w, rv + 16 * k, 16); uu->child[k] = w[0]; uu->parent[k] = ((uint64_t)w[1] << 32) | w[2]; uu->size[k] = w[3]; uu->nent++; }
                uu->have = 1;
            }
        }
        uu->logn += (size_t)snprintf(uu->log + uu->logn, sizeof uu->log - uu->logn, " [arm-io ranges(%u bytes)=%s -> %d entr%s%s", rl, uu->rawr, uu->nent, uu->nent == 1 ? "y" : "ies", uu->have ? "" : ", NOT UNDERSTOOD");
        for (int k = 0; k < uu->nent; k++) uu->logn += (size_t)snprintf(uu->log + uu->logn, sizeof uu->log - uu->logn, " (child 0x%llx parent 0x%llx size 0x%llx)", (unsigned long long)uu->child[k], (unsigned long long)uu->parent[k], (unsigned long long)uu->size[k]);
        uu->logn += (size_t)snprintf(uu->log + uu->logn, sizeof uu->log - uu->logn, "]");
    }
    if (depth >= 2 && !strncmp(nm, "uart", 4)) {
        uint32_t cl = 0, rl = 0; const uint8_t *cv = dt_prop(d, &p, "compatible", &cl, NULL), *rv = dt_prop(d, &p, "reg", &rl, NULL);
        int isuart = 0; for (uint32_t k = 0; cv && k + 4 <= cl; k++) if (!memcmp(cv + k, "uart", 4)) isuart = 1;
        uint64_t ra = 0, rs = 0; int okr = 0;
        if (rv && rl == 8) { uint32_t a2[2]; memcpy(a2, rv, 8); ra = a2[0]; rs = a2[1]; okr = 1; }
        else if (rv && rl == 16) { uint64_t a2[2]; memcpy(a2, rv, 16); ra = a2[0]; rs = a2[1]; okr = 1; }
        if (uu->logn < sizeof uu->log - 200) uu->logn += (size_t)snprintf(uu->log + uu->logn, sizeof uu->log - uu->logn, " [%s compatible-uart=%d reg=%s 0x%llx/0x%llx]", nm, isuart, okr ? "ok" : "?", (unsigned long long)ra, (unsigned long long)rs);
        for (int k = 0; isuart && okr && k < uu->nent && uu->n < 3; k++)
            if (ra >= uu->child[k] && ra - uu->child[k] < uu->size[k]) { uu->pa[uu->n] = ra - uu->child[k] + uu->parent[k]; uu->sz[uu->n] = rs; uu->n++; break; }
    }
    if (depth >= 2 && ((uu->want && !uu->found) || uu->coll)) {   /* 0.4.53: which /arm-io node's "reg" covers the wanted physical address? */
        uint32_t cl = 0, rl = 0; const uint8_t *cv = dt_prop(d, &p, "compatible", &cl, NULL), *rv = dt_prop(d, &p, "reg", &rl, NULL);
        size_t esz = (rl && rl % 16 == 0) ? 16 : 8;
        for (uint32_t o = 0; rv && o + esz <= rl && !uu->found; o += (uint32_t)esz) {
            uint64_t ra, rs; if (esz == 16) { uint64_t a2[2]; memcpy(a2, rv + o, 16); ra = a2[0]; rs = a2[1]; } else { uint32_t a2[2]; memcpy(a2, rv + o, 8); ra = a2[0]; rs = a2[1]; }
            for (int k = 0; k < uu->nent; k++) if (ra >= uu->child[k] && ra - uu->child[k] < uu->size[k]) {
                uint64_t pa = ra - uu->child[k] + uu->parent[k];
                if (uu->coll && uu->ncoll < uu->maxcoll) {   /* 0.4.57: collect every /arm-io node with its physical range (for the stop report) */
                    A64SocNode *sn = &uu->coll[uu->ncoll++]; memset(sn, 0, sizeof *sn); snprintf(sn->name, sizeof sn->name, "%s", nm);
                    size_t cn = cl < sizeof sn->compat - 1 ? cl : sizeof sn->compat - 1; if (cv) memcpy(sn->compat, cv, cn); sn->compat[cn] = 0;
                    for (size_t z = 0; z + 1 < cn; z++) if (!sn->compat[z]) sn->compat[z] = ',';
                    sn->pa = pa; sn->size = rs;
                }
                if (uu->want && !uu->found && uu->want >= pa && uu->want - pa < rs) {
                    uu->found = 1; uu->fpa = pa; uu->fsz = rs; snprintf(uu->fname, sizeof uu->fname, "%s", nm);
                    size_t cn = cl < sizeof uu->fcompat - 1 ? cl : sizeof uu->fcompat - 1; if (cv) memcpy(uu->fcompat, cv, cn); uu->fcompat[cn] = 0;
                    for (size_t z = 0; z + 1 < cn; z++) if (!uu->fcompat[z]) uu->fcompat[z] = ',';   /* several NUL-separated strings */
                }
                break;
            }
        }
    }
    for (uint32_t i = 0; i < nc; i++) if (dt_uart_walk(d, n, pos, depth + 1, uu) < 0) return -1;
    return 0;
}
/* Returns the number of UARTs found (<= 3, addresses in pa/sz), -1 if the tree is malformed. log gets what was seen. */
int a64_dt_uarts(uint8_t *d, size_t n, uint64_t *pa, uint64_t *sz, char *log, size_t logcap) {
    DtUart *uu = calloc(1, sizeof *uu); if (!uu) return -1;
    size_t pos = 0; int r = dt_uart_walk(d, n, &pos, 0, uu);
    if (log && logcap) snprintf(log, logcap, "%s", uu->log);
    int res = r < 0 ? -1 : uu->n; for (int k = 0; k < uu->n; k++) { pa[k] = uu->pa[k]; sz[k] = uu->sz[k]; }
    free(uu); return res;
}

/* 0.4.53: the /arm-io node whose reg range contains physical address `want`. Returns 1 found (name, compatible, base, size filled), 0 not found, -1 malformed. */
int a64_dt_find_pa(uint8_t *d, size_t n, uint64_t want, char *name, size_t ncap, char *compat, size_t ccap, uint64_t *base, uint64_t *size) {
    DtUart *uu = calloc(1, sizeof *uu); if (!uu) return -1;
    uu->want = want; size_t pos = 0; int r = dt_uart_walk(d, n, &pos, 0, uu);
    int res = r < 0 ? -1 : uu->found;
    if (res == 1) { snprintf(name, ncap, "%s", uu->fname); snprintf(compat, ccap, "%s", uu->fcompat); *base = uu->fpa; *size = uu->fsz; }
    free(uu); return res;
}

/* 0.4.57: every /arm-io node that has a reg (name, compatible, physical base, size). Returns the count, -1 malformed. */
int a64_dt_soc_nodes(uint8_t *d, size_t n, A64SocNode *out, int max) {
    DtUart *uu = calloc(1, sizeof *uu); if (!uu) return -1;
    uu->coll = out; uu->maxcoll = max; size_t pos = 0; int r = dt_uart_walk(d, n, &pos, 0, uu);
    int res = r < 0 ? -1 : uu->ncoll; free(uu); return res;
}

/* 0.4.56: first node called `node` (any depth): the length of property `prop` (*plen, -1 if absent) and a list "name:len" of all its properties. 1 found, 0 no such node, -1 malformed. */
typedef struct { const char *node, *prop; int found; int64_t plen; char log[2800]; size_t logn; uint8_t data[512]; size_t datan; } DtNodeInfo;
static int dt_nodeinfo_walk(uint8_t *d, size_t n, size_t *pos, int depth, DtNodeInfo *ni) {
    DtProps p; p.n = 0;
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (p.n < 96) p.off[p.n++] = *pos;
        *pos += adv;
    }
    char nm[33] = ""; uint32_t l = 0; const uint8_t *v = dt_prop(d, &p, "name", &l, NULL);
    if (v) { size_t k = l < 32 ? l : 32; memcpy(nm, v, k); nm[k] = 0; }
    if (!ni->found && !strcmp(nm, ni->node)) {
        ni->found = 1; ni->plen = -1;
        for (int i = 0; i < p.n; i++) {
            char pn[33]; memcpy(pn, d + p.off[i], 32); pn[32] = 0; uint32_t pl; memcpy(&pl, d + p.off[i] + 32, 4); pl &= 0x7FFFFFFFu;
            if (!strcmp(pn, ni->prop)) { ni->plen = pl; ni->datan = pl < sizeof ni->data ? pl : sizeof ni->data; memcpy(ni->data, d + p.off[i] + 36, ni->datan); }
            if (ni->logn < sizeof ni->log - 60) ni->logn += (size_t)snprintf(ni->log + ni->logn, sizeof ni->log - ni->logn, " %s(%u)", pn, pl);
        }
    }
    for (uint32_t i = 0; i < nc; i++) if (dt_nodeinfo_walk(d, n, pos, depth + 1, ni) < 0) return -1;
    return 0;
}
int a64_dt_node_info(uint8_t *d, size_t n, const char *node, const char *prop, int64_t *plen, char *log, size_t logcap) {
    DtNodeInfo *ni = calloc(1, sizeof *ni); if (!ni) return -1;
    ni->node = node; ni->prop = prop; size_t pos = 0; int r = dt_nodeinfo_walk(d, n, &pos, 0, ni);
    int res = r < 0 ? -1 : ni->found; if (res == 1) { *plen = ni->plen; if (log && logcap) snprintf(log, logcap, "%s", ni->log); }
    free(ni); return res;
}

/* 0.4.69: the data of property `prop` of the first node called `node`, as hex 32-bit little-endian words (at most 128 words). Returns the property length, -1 absent/malformed, -2 no such node. */
int a64_dt_node_prop_words(uint8_t *d, size_t n, const char *node, const char *prop, char *out, size_t cap) {
    DtNodeInfo *ni = calloc(1, sizeof *ni); if (!ni) return -1;
    ni->node = node; ni->prop = prop; size_t pos = 0; int r = dt_nodeinfo_walk(d, n, &pos, 0, ni); int64_t res;
    if (r < 0) res = -1; else if (!ni->found) res = -2; else res = ni->plen;
    if (res >= 0 && out && cap) { size_t o = 0; out[0] = 0;
        for (size_t i = 0; i + 4 <= ni->datan && i < 512 && o + 12 < cap; i += 4) { uint32_t w; memcpy(&w, ni->data + i, 4); o += (size_t)snprintf(out + o, cap - o, "%s%x", i ? " " : "", w); } }
    free(ni); return (int)res;
}

/* 0.4.64: ApplePMGR::initDriver (ApplePMGR.cpp:843) requires, for every bridge i < "#bridges", either a property "bridge-settings-<i>" in the 'pmgr' node or bit i of "optional-bridge-mask". The IPSW tree has neither bridge-settings-<i> nor a mask covering bridge 0 (log 0.4.63). MODELLED fix-up: mark every bridge that has no "bridge-settings-<i>" property as optional (set its bit in the mask, in place). Returns 1 changed, 0 nothing to change, -2 node/properties missing, -1 malformed. */
typedef struct { int found, done; uint32_t nbr, oldm, newm, nmiss; } DtPmgr;
static int dt_pmgr_walk(uint8_t *d, size_t n, size_t *pos, int depth, DtPmgr *pm) {
    DtProps p; p.n = 0;
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (p.n < 96) p.off[p.n++] = *pos;
        *pos += adv;
    }
    char nm[33] = ""; uint32_t l = 0; const uint8_t *v = dt_prop(d, &p, "name", &l, NULL);
    if (v) { size_t k = l < 32 ? l : 32; memcpy(nm, v, k); nm[k] = 0; }
    if (!pm->done && !strcmp(nm, "pmgr")) {
        pm->done = 1;
        uint32_t bl = 0, ml = 0; size_t mat = 0; const uint8_t *bv = dt_prop(d, &p, "#bridges", &bl, NULL), *mv = dt_prop(d, &p, "optional-bridge-mask", &ml, &mat);
        if (bv && mv && bl == 4 && ml == 4) {
            memcpy(&pm->nbr, bv, 4); memcpy(&pm->oldm, mv, 4); pm->newm = pm->oldm; pm->found = 1;
            if (pm->nbr <= 32) for (uint32_t i = 0; i < pm->nbr; i++) {
                char key[40]; snprintf(key, sizeof key, "bridge-settings-%u", i); uint32_t kl = 0;
                if (!dt_prop(d, &p, key, &kl, NULL)) { pm->newm |= 1u << i; pm->nmiss++; }
            }
            if (pm->newm != pm->oldm) memcpy(d + mat, &pm->newm, 4);   /* mat = offset of the property DATA (see dt_prop) */
        }
    }
    for (uint32_t i = 0; i < nc; i++) if (dt_pmgr_walk(d, n, pos, depth + 1, pm) < 0) return -1;
    return 0;
}
int a64_dt_pmgr_optional_bridges(uint8_t *d, size_t n, uint32_t *nbr, uint32_t *oldm, uint32_t *newm, uint32_t *nmiss) {
    DtPmgr pm; memset(&pm, 0, sizeof pm); size_t pos = 0; if (dt_pmgr_walk(d, n, &pos, 0, &pm) < 0) return -1;
    if (!pm.found) return -2;
    *nbr = pm.nbr; *oldm = pm.oldm; *newm = pm.newm; *nmiss = pm.nmiss; return pm.newm != pm.oldm;
}

/* Third iBoot-equivalent fix-up (MODELLED): iBoot fills /chosen "random-seed" and "entropy-data" with random bytes before handing the tree over; in the
 * IPSW tree they are empty placeholders and the kernel panics "EntropyData needed %lu bytes, but got %u". Every such property that is all zero is filled
 * with bytes from the supplied generator (production: the host's OS random source). All /chosen properties are listed in the log. */
typedef struct { int (*rng)(void *, size_t); int filled; size_t filled_bytes; char log[1500]; size_t logn; } DtEnt;
static int dt_ent_walk(uint8_t *d, size_t n, size_t *pos, int depth, const char *parent, DtEnt *ee) {
    DtProps p; p.n = 0;
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (p.n < 96) p.off[p.n++] = *pos;
        *pos += adv;
    }
    char nm[33] = ""; uint32_t l = 0; const uint8_t *v = dt_prop(d, &p, "name", &l, NULL);
    if (v) { size_t k = l < 32 ? l : 32; memcpy(nm, v, k); nm[k] = 0; }
    if (depth == 1 && !strcmp(nm, "chosen")) {
        for (int i = 0; i < p.n; i++) {
            char pn[33]; memcpy(pn, d + p.off[i], 32); pn[32] = 0;
            uint32_t pl; memcpy(&pl, d + p.off[i] + 32, 4); pl &= 0x7FFFFFFFu;
            if (ee->logn < sizeof ee->log - 60) ee->logn += (size_t)snprintf(ee->log + ee->logn, sizeof ee->log - ee->logn, " %s(%u)", pn, pl);
            if ((!strcmp(pn, "random-seed") || !strcmp(pn, "entropy-data")) && pl) {
                uint8_t *dat = d + p.off[i] + 36; int zero = 1; for (uint32_t k = 0; k < pl; k++) if (dat[k]) { zero = 0; break; }
                if (zero && ee->rng && ee->rng(dat, pl) == 0) { ee->filled++; ee->filled_bytes += pl; }
            }
        }
    }
    (void)parent;
    for (uint32_t i = 0; i < nc; i++) if (dt_ent_walk(d, n, pos, depth + 1, nm, ee) < 0) return -1;
    return 0;
}
/* Returns the number of properties filled, or -1 if the tree is malformed. log receives the /chosen property list. */
int a64_dt_entropy(uint8_t *d, size_t n, int (*rng)(void *, size_t), size_t *bytes, char *log, size_t logcap) {
    DtEnt *ee = calloc(1, sizeof *ee); if (!ee) return -1;
    ee->rng = rng; size_t pos = 0; int r = dt_ent_walk(d, n, &pos, 0, "", ee);
    if (log && logcap) snprintf(log, logcap, "%s", ee->log);
    int res = r < 0 ? -1 : ee->filled; if (bytes) *bytes = ee->filled_bytes;
    free(ee); return res;
}
/* 0.4.38 (MODELLED): add a missing uint32 property to /chosen, as iBoot would. Returns 1 added, 0 already present, -1 malformed tree / no room. */
static int dt_chosen_find(const uint8_t *d, size_t n, size_t *pos, int depth, size_t *hdr, size_t *ins) {
    if (depth > 32 || *pos + 8 > n) return -1;
    size_t node = *pos; uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    char nm[33] = "";
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (!memcmp(d + *pos, "name", 5)) { size_t k = len < 32 ? len : 32; memcpy(nm, d + *pos + 36, k); nm[k] = 0; }
        *pos += adv;
    }
    if (depth == 1 && !strcmp(nm, "chosen")) { *hdr = node; *ins = *pos; }
    for (uint32_t i = 0; i < nc; i++) if (dt_chosen_find(d, n, pos, depth + 1, hdr, ins) < 0) return -1;
    return 0;
}
int a64_dt_chosen_add_u32(uint8_t *d, size_t *n, size_t cap, const char *name, uint32_t val) {
    size_t pos = 0, hdr = 0, ins = 0;
    if (dt_chosen_find(d, *n, &pos, 0, &hdr, &ins) < 0 || !ins) return -1;
    uint32_t np; memcpy(&np, d + hdr, 4);
    size_t p = hdr + 8;
    for (uint32_t i = 0; i < np; i++) {
        uint32_t len; memcpy(&len, d + p + 32, 4); len &= 0x7FFFFFFFu;
        if (!strncmp((const char *)d + p, name, 32)) return 0;
        p += 36 + (((size_t)len + 3) & ~(size_t)3);
    }
    if (*n + 40 > cap) return -1;
    memmove(d + ins + 40, d + ins, *n - ins);
    memset(d + ins, 0, 32); strncpy((char *)d + ins, name, 31);
    uint32_t l4 = 4; memcpy(d + ins + 32, &l4, 4); memcpy(d + ins + 36, &val, 4);
    np++; memcpy(d + hdr, &np, 4);
    *n += 40; return 1;
}
/* 0.4.65: add a zero-length property `prop` to the first node called `node` (appended after its last property), if it has none. 1 added, 0 already there, -1 no room / malformed, -2 node not found. */
typedef struct { const char *node; int found; size_t hdr, ins; } DtNodeAt;
static int dt_nodeat_walk(uint8_t *d, size_t n, size_t *pos, int depth, DtNodeAt *na) {
    if (depth > 32 || *pos + 8 > n) return -1;
    size_t hdr = *pos; uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    char nm[33] = "";
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || *pos + adv > n) return -1;
        if (!strncmp((const char *)d + *pos, "name", 32)) { size_t k = len < 32 ? len : 32; memcpy(nm, d + *pos + 36, k); nm[k] = 0; }
        *pos += adv;
    }
    if (!na->found && !strcmp(nm, na->node)) { na->found = 1; na->hdr = hdr; na->ins = *pos; }
    for (uint32_t i = 0; i < nc; i++) if (dt_nodeat_walk(d, n, pos, depth + 1, na) < 0) return -1;
    return 0;
}
int a64_dt_node_add_empty_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop) {
    DtNodeAt na; memset(&na, 0, sizeof na); na.node = node; size_t pos = 0;
    if (dt_nodeat_walk(d, *n, &pos, 0, &na) < 0) return -1;
    if (!na.found) return -2;
    uint32_t np; memcpy(&np, d + na.hdr, 4); size_t p = na.hdr + 8;
    for (uint32_t i = 0; i < np; i++) { uint32_t len; memcpy(&len, d + p + 32, 4); len &= 0x7FFFFFFFu; if (!strncmp((const char *)d + p, prop, 32)) return 0; p += 36 + (((size_t)len + 3) & ~(size_t)3); }
    if (*n + 36 > cap) return -1;
    memmove(d + na.ins + 36, d + na.ins, *n - na.ins);
    memset(d + na.ins, 0, 36); strncpy((char *)d + na.ins, prop, 31);   /* length field stays 0 */
    np++; memcpy(d + na.hdr, &np, 4); *n += 36; return 1;
}
/* 0.4.302: rename property `oldp` to `newp` in the first node whose "name" is `node` (same-size field, the value stays). Used to disable one property of a real DeviceTree without deleting data. 1 renamed, 0 not found, -1 malformed. */
static int dt_ren_walk(uint8_t *d, size_t n, size_t *pos, int depth, const char *node, const char *oldp, const char *newp, int *done) {
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    char me[40] = "?"; size_t save = *pos;
    for (uint32_t i = 0; i < np; i++) { if (*pos + 36 > n) return -1; uint32_t l; memcpy(&l, d + *pos + 32, 4); l &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)l + 3) & ~(size_t)3); if (*pos + adv > n) return -1;
        if (!memcmp(d + *pos, "name\0", 5) && l < 39) { memcpy(me, d + *pos + 36, l); me[l] = 0; } *pos += adv; }
    size_t after = *pos; *pos = save;
    int all = !strcmp(node, "*");   /* 0.4.331: node "*" = rename the property in EVERY node, *done counts them */
    if ((all || !*done) && (all || !strcmp(me, node))) for (uint32_t i = 0; i < np; i++) { uint32_t l; memcpy(&l, d + *pos + 32, 4); l &= 0x7FFFFFFFu;
        if (!strncmp((const char *)d + *pos, oldp, 32) && strlen(oldp) < 32) { memset(d + *pos, 0, 32); memcpy(d + *pos, newp, strlen(newp)); (*done)++; if (!all) break; }
        *pos += 36 + (((size_t)l + 3) & ~(size_t)3); }
    *pos = after;
    for (uint32_t i = 0; i < nc; i++) if (dt_ren_walk(d, n, pos, depth + 1, node, oldp, newp, done) < 0) return -1;
    return 0;
}
int a64_dt_prop_rename(uint8_t *d, size_t n, const char *node, const char *oldp, const char *newp) {
    if (strlen(newp) >= 32) return -1; size_t pos = 0; int done = 0;
    if (dt_ren_walk(d, n, &pos, 0, node, oldp, newp, &done) < 0) return -1;
    return done;
}
/* 0.4.275: remove a whole node (with its subtree) called `node` from the flattened DeviceTree and fix the parent's child count. The node name is the "name" property. 1 removed (the first match), 0 not found, -1 malformed. Never removes the root. */
static char dt_sub_compat[64];   /* 0.4.277: "compatible" of the node dt_sub parsed last at depth of the call (set only when nm != NULL) */
static int dt_sub(const uint8_t *d, size_t n, size_t pos, int depth, char *nm, size_t *end) {
    if (depth > 32 || pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + pos, 4); memcpy(&nc, d + pos + 4, 4); pos += 8;
    if (np > 4096 || nc > 4096) return -1;
    if (nm) { nm[0] = 0; dt_sub_compat[0] = 0; }
    for (uint32_t i = 0; i < np; i++) {
        if (pos + 36 > n) return -1;
        uint32_t len; memcpy(&len, d + pos + 32, 4); len &= 0x7FFFFFFFu;
        size_t adv = 36 + (((size_t)len + 3) & ~(size_t)3);
        if (len > n || pos + adv > n) return -1;
        if (nm && !strncmp((const char *)d + pos, "compatible", 32)) { size_t kk = len < 63 ? len : 63; memcpy(dt_sub_compat, d + pos + 36, kk); dt_sub_compat[kk] = 0; }
        if (nm && !strncmp((const char *)d + pos, "name", 32)) { size_t kk = len < 32 ? len : 32; memcpy(nm, d + pos + 36, kk); nm[kk] = 0; }
        pos += adv;
    }
    for (uint32_t i = 0; i < nc; i++) { size_t e2; if (dt_sub(d, n, pos, depth + 1, NULL, &e2) < 0) return -1; pos = e2; }
    *end = pos; return 0;
}
static int dt_rm_walk(uint8_t *d, size_t *n, size_t hdr, int depth, const char *node) {
    size_t pos; uint32_t np, nc; size_t e0;
    if (depth > 32 || dt_sub(d, *n, hdr, depth, NULL, &e0) < 0) return -1;   /* validates the whole subtree before any raw read below */
    memcpy(&np, d + hdr, 4); memcpy(&nc, d + hdr + 4, 4); pos = hdr + 8;
    for (uint32_t i = 0; i < np; i++) { uint32_t len; memcpy(&len, d + pos + 32, 4); len &= 0x7FFFFFFFu; pos += 36 + (((size_t)len + 3) & ~(size_t)3); }
    for (uint32_t i = 0; i < nc; i++) {
        char nm[40]; size_t e; if (dt_sub(d, *n, pos, depth + 1, nm, &e) < 0) return -1;
        if (!strcmp(nm, node)) { memmove(d + pos, d + e, *n - e); *n -= e - pos; nc--; memcpy(d + hdr + 4, &nc, 4); return 1; }
        int r = dt_rm_walk(d, n, pos, depth + 1, node); if (r) return r;
        pos = e;
    }
    return 0;
}
int a64_dt_node_remove(uint8_t *d, size_t *n, const char *node) { if (!node || !*node) return 0; return dt_rm_walk(d, n, 0, 0, node); }
/* 0.4.277: remove every node (max 8, first-match-first) whose name or compatible contains `sub` (case-insensitive). Names of removed nodes are appended to `log` (comma separated). Returns the count, -1 malformed. */
static int dt_ci_has(const char *h, const char *nd) { size_t nl = strlen(nd); for (; *h; h++) { size_t i = 0; while (i < nl && h[i] && (h[i] | 0x20) == (nd[i] | 0x20)) i++; if (i == nl) return 1; } return 0; }
static int dt_rmc_walk(uint8_t *d, size_t *n, size_t hdr, int depth, const char *sub, char *rname, size_t rcap) {
    size_t pos, e0; uint32_t np, nc;
    if (depth > 32 || dt_sub(d, *n, hdr, depth, NULL, &e0) < 0) return -1;
    memcpy(&np, d + hdr, 4); memcpy(&nc, d + hdr + 4, 4); pos = hdr + 8;
    for (uint32_t i = 0; i < np; i++) { uint32_t len; memcpy(&len, d + pos + 32, 4); len &= 0x7FFFFFFFu; pos += 36 + (((size_t)len + 3) & ~(size_t)3); }
    for (uint32_t i = 0; i < nc; i++) {
        char nm[40]; size_t e; if (dt_sub(d, *n, pos, depth + 1, nm, &e) < 0) return -1;
        if (dt_ci_has(nm, sub) || dt_ci_has(dt_sub_compat, sub)) { snprintf(rname, rcap, "%s", nm); memmove(d + pos, d + e, *n - e); *n -= e - pos; nc--; memcpy(d + hdr + 4, &nc, 4); return 1; }
        int r = dt_rmc_walk(d, n, pos, depth + 1, sub, rname, rcap); if (r) return r;
        pos = e;
    }
    return 0;
}
int a64_dt_node_remove_match(uint8_t *d, size_t *n, const char *sub, char *log, size_t logcap) {
    int cnt = 0; if (log && logcap) log[0] = 0; if (!sub || !*sub) return 0;
    for (int it = 0; it < 16; it++) { char rn[40]; int r = dt_rmc_walk(d, n, 0, 0, sub, rn, sizeof rn); if (r < 0) return -1; if (!r) break; if (log && logcap) { size_t l = strlen(log); snprintf(log + l, logcap - l, "%s%s", cnt ? "," : "", rn); } cnt++; }
    return cnt;
}
/* 0.4.74: add a property of `len` zero bytes to the first node with this name; 1 added, 0 exists, -1 no room, -2 no node. */
int a64_dt_node_add_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop, const void *data, size_t len) {
    DtNodeAt na; memset(&na, 0, sizeof na); na.node = node; size_t pos = 0;
    if (dt_nodeat_walk(d, *n, &pos, 0, &na) < 0) return -1;
    if (!na.found) return -2;
    uint32_t np; memcpy(&np, d + na.hdr, 4); size_t p = na.hdr + 8;
    for (uint32_t i = 0; i < np; i++) { uint32_t l; memcpy(&l, d + p + 32, 4); l &= 0x7FFFFFFFu; if (!strncmp((const char *)d + p, prop, 32)) return 0; p += 36 + (((size_t)l + 3) & ~(size_t)3); }
    size_t add = 36 + ((len + 3) & ~(size_t)3);
    if (*n + add > cap) return -1;
    memmove(d + na.ins + add, d + na.ins, *n - na.ins);
    memset(d + na.ins, 0, add); strncpy((char *)d + na.ins, prop, 31);
    uint32_t l32 = (uint32_t)len; memcpy(d + na.ins + 32, &l32, 4); if (data && len) memcpy(d + na.ins + 36, data, len);
    np++; memcpy(d + na.hdr, &np, 4); *n += add; return 1;
}
/* 0.4.79: call cb(name) for every property name of every node of the flattened DeviceTree; returns the count, -1 malformed. */
/* 0.4.85: give an EXISTING zero-length property of the first node with this name `len` bytes of data (NULL = zeros). 1 filled, 0 absent or not empty, -1 no room/malformed, -2 no node. */
int a64_dt_node_fill_empty_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop, const void *data, size_t len) {
    DtNodeAt na; memset(&na, 0, sizeof na); na.node = node; size_t pos = 0;
    if (dt_nodeat_walk(d, *n, &pos, 0, &na) < 0) return -1;
    if (!na.found) return -2;
    uint32_t np; memcpy(&np, d + na.hdr, 4); size_t p = na.hdr + 8;
    for (uint32_t i = 0; i < np; i++) {
        if (p + 36 > *n) return -1;
        uint32_t l; memcpy(&l, d + p + 32, 4); uint32_t flag = l & 0x80000000u; l &= 0x7FFFFFFFu;
        if (!strncmp((const char *)d + p, prop, 32)) {
            if (l != 0) return 0;
            size_t add = (len + 3) & ~(size_t)3;
            if (*n + add > cap) return -1;
            memmove(d + p + 36 + add, d + p + 36, *n - (p + 36));
            memset(d + p + 36, 0, add); if (data && len) memcpy(d + p + 36, data, len);
            uint32_t nl = (uint32_t)len | flag; memcpy(d + p + 32, &nl, 4); *n += add; return 1;
        }
        p += 36 + (((size_t)l + 3) & ~(size_t)3);
    }
    return 0;
}
int a64_dt_node_add_zero_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop, size_t len) { return a64_dt_node_add_prop(d, n, cap, node, prop, NULL, len); }
static int dt_names_walk(const uint8_t *d, size_t n, size_t *pos, int depth, void (*cb)(const char *, void *), void *ud) {
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8; int cnt = 0;
    for (uint32_t i = 0; i < np; i++) {
        if (*pos + 36 > n) return -1;
        char nm[33]; memcpy(nm, d + *pos, 32); nm[32] = 0; uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        cb(nm, ud); cnt++; *pos += 36 + (((size_t)len + 3) & ~(size_t)3);
    }
    for (uint32_t i = 0; i < nc; i++) { int r = dt_names_walk(d, n, pos, depth + 1, cb, ud); if (r < 0) return -1; cnt += r; }
    return cnt;
}
/* 0.4.89 diagnostic (changes nothing): list DeviceTree nodes/properties whose name contains one of the key words (log 0.4.88: ApplePMGRMTRSensor.cpp:131 'propery mtr_0 missing in devicetree') */
typedef struct { char *out; size_t cap, len; int hits; } DtGrep;
static int dt_has_kw(const char *s) { static const char *const kw[] = { "function-vbus_voltage", "function-reset", "function-bb_on", "function-external_standby", "function-vci", "function-pcie_port_control", "function-micbias-enable", "function-bb_rst" }; for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++) if (strstr(s, kw[i])) return 1; return 0; }
/* 0.4.117 diagnostic: every property (name, length, first 8 words) of the nodes called `target`, comma separated */
static int dt_dump_node(const uint8_t *d, size_t n, size_t *pos, int depth, const char *target, char *out, size_t cap, size_t *len) {
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    char me[40] = "?"; size_t save = *pos;
    for (uint32_t i = 0; i < np; i++) { if (*pos + 36 > n) return -1; uint32_t l; memcpy(&l, d + *pos + 32, 4); l &= 0x7FFFFFFFu;
        if (!memcmp(d + *pos, "name\0", 5) && l < 39) { memcpy(me, d + *pos + 36, l); me[l] = 0; } *pos += 36 + (((size_t)l + 3) & ~(size_t)3); }
    *pos = save; int hit = !strcmp(me, target);
    for (uint32_t i = 0; i < np; i++) { char nm[33]; memcpy(nm, d + *pos, 32); nm[32] = 0; uint32_t l; memcpy(&l, d + *pos + 32, 4); l &= 0x7FFFFFFFu;
        if (hit && *len + 200 < cap) { *len += (size_t)snprintf(out + *len, cap - *len, "%s(%u)=", nm, l);
            for (uint32_t b = 0; b < l && b < 32 && b + 4 <= l; b += 4) { uint32_t w; memcpy(&w, d + *pos + 36 + b, 4); *len += (size_t)snprintf(out + *len, cap - *len, "%x,", w); }
            if (l >= 4 && l <= 32 && !strncmp(nm, "name", 4)) { /* text */ }
            *len += (size_t)snprintf(out + *len, cap - *len, " "); }
        *pos += 36 + (((size_t)l + 3) & ~(size_t)3); }
    for (uint32_t i = 0; i < nc; i++) { if (dt_dump_node(d, n, pos, depth + 1, target, out, cap, len) < 0) return -1; }
    return 0;
}
/* 0.4.445 diagnostic: every node whose AAPL,phandle equals `want` (name + compatible + parent name) */
static int dt_find_phandle(const uint8_t *d, size_t n, size_t *pos, int depth, const char *parent, uint32_t want, char *out, size_t cap, size_t *len) {
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    char me[40] = "?", comp[48] = "?"; uint32_t ph = 0xffffffffu; size_t save = *pos;
    for (uint32_t i = 0; i < np; i++) { if (*pos + 36 > n) return -1; uint32_t l; memcpy(&l, d + *pos + 32, 4); l &= 0x7FFFFFFFu;
        if (!memcmp(d + *pos, "name\0", 5) && l < 39) { memcpy(me, d + *pos + 36, l); me[l] = 0; }
        if (!memcmp(d + *pos, "compatible\0", 11) && l < 47) { memcpy(comp, d + *pos + 36, l); comp[l] = 0; }
        if (!memcmp(d + *pos, "AAPL,phandle\0", 13) && l == 4) memcpy(&ph, d + *pos + 36, 4);
        *pos += 36 + (((size_t)l + 3) & ~(size_t)3); }
    (void)save;
    if (ph == want && *len + 200 < cap) *len += (size_t)snprintf(out + *len, cap - *len, "[node '%s' compatible '%s' parent '%s'] ", me, comp, parent);
    for (uint32_t i = 0; i < nc; i++) { if (dt_find_phandle(d, n, pos, depth + 1, me, want, out, cap, len) < 0) return -1; }
    return 0;
}
static int dt_grep_walk(const uint8_t *d, size_t n, size_t *pos, int depth, const char *parent, DtGrep *g) {
    if (depth > 32 || *pos + 8 > n) return -1;
    uint32_t np, nc; memcpy(&np, d + *pos, 4); memcpy(&nc, d + *pos + 4, 4); *pos += 8;
    char me[40] = "?"; size_t save = *pos;
    for (uint32_t i = 0; i < np; i++) { if (*pos + 36 > n) return -1; uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        if (!memcmp(d + *pos, "name\0", 5) && len < 39) { memcpy(me, d + *pos + 36, len); me[len] = 0; } *pos += 36 + (((size_t)len + 3) & ~(size_t)3); }
    *pos = save;
    if (dt_has_kw(me) && g->len + 80 < g->cap) { g->len += (size_t)snprintf(g->out + g->len, g->cap - g->len, "[node %s/%s] ", parent, me); g->hits++; }
    for (uint32_t i = 0; i < np; i++) {
        char nm[33]; memcpy(nm, d + *pos, 32); nm[32] = 0; uint32_t len; memcpy(&len, d + *pos + 32, 4); len &= 0x7FFFFFFFu;
        if (dt_has_kw(nm) && g->len + 80 < g->cap) { g->len += (size_t)snprintf(g->out + g->len, g->cap - g->len, "%s/%s(%u)=", me, nm, len);
            for (uint32_t b = 0; b < len && b < 24 && b + 4 <= len; b += 4) { uint32_t w; memcpy(&w, d + *pos + 36 + b, 4); g->len += (size_t)snprintf(g->out + g->len, g->cap - g->len, "%x,", w); } { g->len += (size_t)snprintf(g->out + g->len, g->cap - g->len, "\"%.40s\" ", (const char *)(d + *pos + 36)); }
            if (g->len + 2 < g->cap) g->out[g->len++] = ' ', g->out[g->len] = 0; g->hits++; }
        *pos += 36 + (((size_t)len + 3) & ~(size_t)3);
    }
    for (uint32_t i = 0; i < nc; i++) { if (dt_grep_walk(d, n, pos, depth + 1, me, g) < 0) return -1; }
    return 0;
}
static void dt_names_to_machine(const char *nm, void *ud) { a64m_nm_add_dt_name((A64Machine *)ud, nm); }
/* 0.4.23 (MODELLED): fixed pseudo-random bytes (xorshift64*, constant seed) instead of host entropy. Real iBoot uses a hardware RNG; with host entropy the kernel's
 * zone/KASLR randomisation changed every run, so a failing run could not be reproduced. Same bytes every run = same kernel behaviour = comparable logs. */
static int host_random(void *buf, size_t n) {
    uint64_t x = 0x58A64E55D0C0FFEEull; uint8_t *o = buf;
    for (size_t i = 0; i < n; i++) { x ^= x >> 12; x ^= x << 25; x ^= x >> 27; o[i] = (uint8_t)((x * 0x2545F4914F6CDD1Dull) >> 56); }
    return 0;
}

/* 0.4.122: property data of `prop` (first occurrence anywhere in the flat tree; the 32-byte name field + length 16) set to two u64.
   Used to fill /chosen/memory-map 'RAMDisk' = {physical address, size} once the final layout is known. 1 patched, 0 not found / length != 16, -1 ambiguous. */
int a64_dt_patch_u64x2(uint8_t *d, size_t n, const char *prop, uint64_t a, uint64_t b) {
    uint8_t pat[36]; memset(pat, 0, sizeof pat); strncpy((char *)pat, prop, 32); uint32_t l16 = 16; memcpy(pat + 32, &l16, 4);
    uint8_t *hit = NULL; int cnt = 0;
    for (size_t i = 0; i + 36 + 16 <= n; i += 4) if (!memcmp(d + i, pat, 36)) { if (!hit) hit = d + i; cnt++; }
    if (cnt > 1) return -1; if (!hit) return 0;
    memcpy(hit + 36, &a, 8); memcpy(hit + 44, &b, 8); return 1;
}

/* 0.4.122: BuildManifest.plist (XML text, `n` bytes, no NUL needed): the <string> after the first '<key>Path</key>' that follows the first
   '<key>`key`</key>' whose value is a <dict>; copied to out. 1 found, 0 not found. */
int a64_plist_first_path(const char *p, size_t n, const char *key, char *out, size_t cap) {
    char pat[96]; snprintf(pat, sizeof pat, "<key>%s</key>", key);
    const char *e = p + n, *k = NULL; size_t pl = strlen(pat);
    /* 0.4.124: the key also occurs as <key>K</key><string>Production</string> (policy lists); only the occurrence whose value is a <dict> is the entry */
    for (const char *q = p; q + pl <= e; q++) if (!memcmp(q, pat, pl)) {
        const char *v = q + pl; while (v < e && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        if (v + 6 <= e && !memcmp(v, "<dict>", 6)) { k = q; break; } }
    if (!k) return 0;
    static const char pk[] = "<key>Path</key>"; const char *pp = NULL;
    for (const char *q = k; q + sizeof pk - 1 <= e; q++) if (!memcmp(q, pk, sizeof pk - 1)) { pp = q + sizeof pk - 1; break; }
    if (!pp) return 0;
    const char *a = NULL; for (const char *q = pp; q + 8 <= e; q++) if (!memcmp(q, "<string>", 8)) { a = q + 8; break; }
    if (!a) return 0;
    const char *z = NULL; for (const char *q = a; q + 9 <= e; q++) if (!memcmp(q, "</string>", 9)) { z = q; break; }
    if (!z || (size_t)(z - a) + 1 > cap) return 0;
    memcpy(out, a, (size_t)(z - a)); out[z - a] = 0; return 1;
}
/* 0.4.338: the <string> value that directly follows '<key>`key`</key>' (plain key/string pair, e.g. ProductVersion in BuildManifest.plist). 1 found, 0 not found/too long. */
int a64_plist_string_value(const char *p, size_t n, const char *key, char *out, size_t cap) {
    char pat[96]; snprintf(pat, sizeof pat, "<key>%s</key>", key); size_t pl = strlen(pat);
    for (size_t i = 0; i + pl <= n; i++) if (!memcmp(p + i, pat, pl)) {
        const char *v = p + i + pl, *e = p + n; while (v < e && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        if (v + 8 > e || memcmp(v, "<string>", 8)) continue;
        const char *a = v + 8, *z = NULL; for (const char *q = a; q + 9 <= e; q++) if (!memcmp(q, "</string>", 9)) { z = q; break; }
        if (!z || (size_t)(z - a) + 1 > cap) return 0;
        memcpy(out, a, (size_t)(z - a)); out[z - a] = 0; return 1; }
    return 0;
}
/* 0.4.338: 1 = the version string "12.x[.y]" is a supported iOS major (12), 0 = another/unparsable. Only iOS 12.x on iPhone11,4 was ever tested; the attempt still runs for anything else. */
int a64_ios_version_supported(const char *v) { return v && v[0] == '1' && v[1] == '2' && (v[2] == '.' || v[2] == 0); }
#define TEMPSENSOR_RAW 0x8c0ull   /* PLACEHOLDER raw reading: the kernel converts raw*100*(1/64) (double constant 0x3f90000000000000 at 0x706f4a0, 0.4.144 dump) -> raw 0x8c0 = 35.0 (units: 1/64 degree, ASSUMPTION) */
static uint64_t g_ch_fn[48], g_ch_site[48]; static int g_ch_d[48], g_ch_n;   /* 0.4.159 diagnostic: callers chain of the code referencing '/sbin/launchd' */
int a64_attempt_chain(int i, uint64_t *fn, uint64_t *site, int *depth) { if (i < 0 || i >= g_ch_n) return 0; *fn = g_ch_fn[i]; *site = g_ch_site[i]; *depth = g_ch_d[i]; return 1; }
static uint64_t g_bi_site[600], g_bi_tg[600]; static int g_bi_n, g_bi_cnt[48], g_bi_first[48];   /* 0.4.160 diagnostic: every bl inside the depth-4 caller of the launchd code (bsd_init?) up to the call towards launchd */
int a64_attempt_bsdinit(int chain_i, int j, uint64_t *site, uint64_t *target) { if (chain_i < 0 || chain_i >= 48 || j < 0 || j >= g_bi_cnt[chain_i]) return 0; *site = g_bi_site[g_bi_first[chain_i] + j]; *target = g_bi_tg[g_bi_first[chain_i] + j]; return 1; }
static uint64_t *g_sym_a; static uint32_t *g_sym_n; static char *g_sym_str; static size_t g_sym_cnt;   /* 0.4.170 diagnostic: kernelcache LC_SYMTAB (if the file carries one), sorted by address */
const char *a64_attempt_symbolize(uint64_t va, uint64_t *off) { if (!g_sym_cnt) return 0; size_t lo = 0, hi = g_sym_cnt; while (hi - lo > 1) { size_t md = (lo + hi) / 2; if (g_sym_a[md] <= va) lo = md; else hi = md; } if (g_sym_a[lo] > va || va - g_sym_a[lo] > 0x4000) return 0; *off = va - g_sym_a[lo]; return g_sym_str + g_sym_n[lo]; }
static int sym_cmp(const void *x, const void *y) { uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y; return a < b ? -1 : a > b; }
static uint64_t g_xref[4];   /* 0.4.126 diagnostic: code addresses referencing 'RAMDisk' (0,1) and 'root-matching' (2,3) */
static const char *g_pick_name;
static bool pick_exact(const char *n) { return g_pick_name && !strcmp(n, g_pick_name); }
static bool pick_manifest(const char *n) { return !strcmp(n, "BuildManifest.plist"); }
static bool pick_dtree(const char *n) { return has(n, "DeviceTree") && !has(n, ".plist") && !has(n, ".dmg"); }

static uint8_t *zip_extract(A64ReadAt rd, void *ud, const ZEnt *z, char *why, size_t wc) {
    uint8_t lh[30];
    if (rd(ud, z->lho, lh, 30) || r32(lh) != 0x04034b50u) { snprintf(why, wc, "bad local file header"); return NULL; }
    uint64_t data = z->lho + 30 + r16(lh + 26) + r16(lh + 28);
    if (z->csz > MAX_COMPRESSED || z->usz > MAX_KERNEL) { snprintf(why, wc, "member too large (%llu bytes compressed / %llu uncompressed)", (unsigned long long)z->csz, (unsigned long long)z->usz); return NULL; }
    uint8_t *c = malloc(z->csz ? z->csz : 1), *u = malloc(z->usz ? z->usz : 1);
    if (!c || !u) { free(c); free(u); snprintf(why, wc, "out of memory"); return NULL; }
    if (rd(ud, data, c, z->csz)) { free(c); free(u); snprintf(why, wc, "cannot read member data"); return NULL; }
    if (z->method == 0) {
        if (z->csz != z->usz) { free(c); free(u); snprintf(why, wc, "stored member with mismatching sizes"); return NULL; }
        free(u); return c;
    }
    if (z->method != 8) { free(c); free(u); snprintf(why, wc, "unsupported ZIP compression method %u", z->method); return NULL; }
    z_stream s; memset(&s, 0, sizeof s);
    if (inflateInit2(&s, -15) != Z_OK) { free(c); free(u); snprintf(why, wc, "zlib init failed"); return NULL; }
    s.next_in = c; s.avail_in = (uInt)z->csz; s.next_out = u; s.avail_out = (uInt)z->usz;
    int rc = inflate(&s, Z_FINISH); inflateEnd(&s); free(c);
    if (rc != Z_STREAM_END || s.total_out != z->usz) { free(u); snprintf(why, wc, "inflate failed (rc=%d, got %lu of %llu bytes)", rc, s.total_out, (unsigned long long)z->usz); return NULL; }
    return u;
}

/* ------------------------------------------------------------- the attempt */
char g_attempt_last[200];   /* 0.4.355: last boot-log line start, read by the JNI crash guard to say where a native crash happened */
#define LOG(...) (snprintf(g_attempt_last, sizeof g_attempt_last, __VA_ARGS__), a64m_log(m, __VA_ARGS__))

static A64Machine *dead(const char *why, const char *step, A64Machine *m) {
    if (!m) m = a64m_create_system(1u << 20);
    if (!m) return NULL;
    char b[400]; snprintf(b, sizeof b, "%s - %s", step, why);
    a64m_abort(m, b);
    return m;
}

/* Unwraps an IM4P/IMG4 (or accepts an already-plain buffer); returns malloc'ed plain bytes. */
static uint8_t *to_plain(A64Machine *m, const uint8_t *d, size_t n, const char *what, size_t *out, const char **stop) {
    static char why[300];
    const uint8_t *p = d; size_t pl = n; A64Im4p im;
    if (n > 2 && d[0] == 0x30 && a64_im4p_unwrap(d, n, &im) == 0) {
        LOG("[boot] %s: Apple %s container, component type '%.4s', payload %zu bytes%s\n", what, im.has_im4m ? "IMG4" : "IM4P", im.type, im.len, im.has_im4m ? " (signed manifest IM4M present; not verified - XNUron has no boot ROM)" : "");
        if (im.has_kbag) {
            *stop = "payload is encrypted (KBAG key bag present). The AES keys are wrapped for Apple's hardware GID key and are not in the file; XNUron does not hold, derive or fetch them, so there is nothing it can execute. This is the end of the road for this firmware component.";
            return NULL;
        }
        p = im.data; pl = im.len;
    } else if (n > 2 && d[0] == 0x30) {
        *stop = "DER container that is not a valid IM4P/IMG4"; return NULL;
    } else LOG("[boot] %s: not an IM4P/IMG4 wrapper, using the bytes as they are\n", what);
    if (pl >= 4 && (!memcmp(p, "bvx", 3) || (pl >= 8 && !memcmp(p, "complzss", 8)))) {
        LOG("[boot] %s: decompressing %s ...\n", what, !memcmp(p, "bvx", 3) ? "LZFSE" : "LZSS (complzss)");
        uint8_t *o = a64_unpack_payload(p, pl, out, why, sizeof why);
        if (!o) { *stop = why; return NULL; }
        LOG("[boot] %s: decompressed to %zu bytes\n", what, *out);
        return o;
    }
    uint8_t *o = malloc(pl ? pl : 1); if (!o) { *stop = "out of memory"; return NULL; }
    memcpy(o, p, pl); *out = pl; return o;
}

A64Machine *a64_attempt_ipsw(A64ReadAt rd, void *ud, uint64_t total) {
    char why[320]; const char *stop = NULL;
    A64Machine *log_only = a64m_create_system(1u << 20);          /* holds the log until the real machine exists */
    if (!log_only) return NULL;
    A64Machine *m = log_only;
    LOG("[boot] === EXPERIMENTAL IPSW BOOT ATTEMPT ===\n[boot] This tries each step for real and shows the real result. It is expected to fail early. It is NOT iOS booting.\n");
    ZEnt ke;
    if (zip_find(rd, ud, total, pick_kernel, 0, &ke, why, sizeof why) < 0 && zip_find(rd, ud, total, pick_kernel_any, 0, &ke, why, sizeof why) < 0)
        return dead(why, "step 1 (find kernelcache in the archive)", m);
    LOG("[boot] step 1: kernelcache member '%s' (%llu -> %llu bytes, %s)\n", ke.name, (unsigned long long)ke.csz, (unsigned long long)ke.usz, ke.method == 8 ? "deflate" : "stored");
    uint8_t *raw = zip_extract(rd, ud, &ke, why, sizeof why);
    if (!raw) return dead(why, "step 2 (extract)", m);
    LOG("[boot] step 2: extracted %llu bytes\n", (unsigned long long)ke.usz);
    size_t klen = 0; uint8_t *kern = to_plain(m, raw, (size_t)ke.usz, "kernelcache", &klen, &stop);
    free(raw);
    if (!kern) return dead(stop, "step 3 (unwrap/decompress)", m);
    if (klen >= 4 && !memcmp(kern, "\xCE\xFA\xED\xFE", 4) ) { free(kern); return dead("this is a 32-bit ARM (AArch32) Mach-O kernel. XNUron emulates AArch64 only: 32-bit ARM code cannot be executed by it at all, so there is nothing to attempt.", "step 4 (check CPU architecture)", m); }
    A64MachO mo;
    if (a64_macho_parse(kern, klen, &mo) != 0) {
        char b[300]; snprintf(b, sizeof b, "decoded data is not a 64-bit arm64 Mach-O (%s). First bytes: %02x %02x %02x %02x", mo.err, klen > 0 ? kern[0] : 0, klen > 1 ? kern[1] : 0, klen > 2 ? kern[2] : 0, klen > 3 ? kern[3] : 0);
        free(kern); return dead(b, "step 4 (parse kernel)", m);
    }
    LOG("[boot] step 4: Mach-O arm64 kernel, filetype 0x%x, %d segments, vm range 0x%llx..0x%llx\n", mo.filetype, mo.nseg, (unsigned long long)mo.lowest_vm, (unsigned long long)mo.highest_vm);
    if (!mo.has_unixthread) { free(kern); return dead("no LC_UNIXTHREAD entry point in the kernel", "step 4", m); }
    uint64_t span = mo.highest_vm - mo.lowest_vm;
    /* Real iBoot keeps virtual and physical addresses congruent modulo the 32 MiB L2 block size (the kernel maps RAM with
     * 32 MiB blocks starting at the rounded-down addresses), so the image must sit at the same offset inside a 32 MiB block
     * physically as it does virtually. Observed on a real iOS 12 kernelcache: it builds VA 0xfffffff006000000 -> PA 0x40000000. */
    uint64_t kbase_off = (mo.lowest_vm == ~0ull ? 0 : mo.lowest_vm) & 0x1FFFFFFull;
    if (kbase_off < 0x400000) kbase_off += 0x2000000;
    uint64_t ram = (kbase_off + span + (64u << 20) + 0xFFFFu) & ~0xFFFFull;
    if (mo.lowest_vm == ~0ull || mo.highest_vm <= mo.lowest_vm || ram > MAX_RAM) { free(kern); return dead("kernel memory layout is unusable or larger than the 768 MiB guest RAM limit", "step 5", m); }

    /* optional DeviceTree */
    uint8_t *dt = NULL; size_t dtlen = 0; ZEnt de;
    if (zip_find(rd, ud, total, pick_dtree, 0, &de, why, sizeof why) == 0) {
        uint8_t *dr = zip_extract(rd, ud, &de, why, sizeof why);
        if (dr) {
            const char *s2 = NULL; dt = to_plain(m, dr, (size_t)de.usz, "DeviceTree", &dtlen, &s2); free(dr);
            if (!dt) LOG("[boot] step 5: DeviceTree '%s' unusable: %s. Continuing without it.\n", de.name, s2);
            else LOG("[boot] step 5: DeviceTree '%s' %zu bytes\n", de.name, dtlen);
        } else LOG("[boot] step 5: DeviceTree '%s' could not be extracted: %s. Continuing without it.\n", de.name, why);
    } else LOG("[boot] step 5: no DeviceTree member found. Continuing without it.\n");
    if (dt && dtlen > 0x300000) { free(dt); dt = NULL; LOG("[boot] DeviceTree larger than 3 MiB, ignored\n"); }
    /* 0.4.122 (MODELLED, iBoot does this): the kernel waits for a root device (IOMedia Apple_HFS, log 0.4.118) and the IPSW holds no firmware for the ANS
       storage coprocessor, so the way a restore boot gets a root disk is used: the Restore RAM disk named by BuildManifest.plist is loaded into memory and
       handed over in /chosen/memory-map 'RAMDisk' with boot-arg rd=md0. Its IM4P has no KBAG (not encrypted); nothing is decrypted or bypassed. */
    uint8_t *rdk = NULL; size_t rdlen = 0; uint64_t rdpa = 0; int rd_on = 0;
    if (dt) {
        ZEnt me2; char rname[160] = "";
        if (zip_find(rd, ud, total, pick_manifest, 0, &me2, why, sizeof why) == 0 && me2.usz < (32u << 20)) {
            uint8_t *mf = zip_extract(rd, ud, &me2, why, sizeof why);
            if (mf) { { char ver[32] = "", bld[32] = ""; int hv = a64_plist_string_value((const char *)mf, (size_t)me2.usz, "ProductVersion", ver, sizeof ver); a64_plist_string_value((const char *)mf, (size_t)me2.usz, "ProductBuildVersion", bld, sizeof bld);
                  int dev_ok = 0; for (size_t qi = 0; qi + 10 <= (size_t)me2.usz && !dev_ok; qi++) if (!memcmp(mf + qi, "iPhone11,4", 10)) dev_ok = 1;
                  if (!hv) LOG("[boot] [UNSUPPORTED] iOS version could not be read from BuildManifest.plist; only iOS 12.x on iPhone11,4 was ever tested. The attempt still runs.\n");
                  else if (!a64_ios_version_supported(ver)) LOG("[boot] [UNSUPPORTED] iOS %s (build %s) is NOT supported: only iOS 12.x on iPhone11,4 was ever tested. The attempt still runs, but expect it to fail early.\n", ver, bld[0] ? bld : "?");
                  else LOG("[boot] iOS %s (build %s): major version 12, the version this project is developed against.\n", ver, bld[0] ? bld : "?");
                  if (!dev_ok) LOG("[boot] [UNSUPPORTED] this IPSW does not list iPhone11,4 (iPhone XS Max): only that device was ever tested. The attempt still runs.\n"); }
              if (!a64_plist_first_path((const char *)mf, (size_t)me2.usz, "RestoreRamDisk", rname, sizeof rname)) rname[0] = 0; free(mf); }
        }
        if (!rname[0]) LOG("[boot] RAM disk: BuildManifest.plist or its RestoreRamDisk entry not found, no root device is provided\n");
        else {
            ZEnt re; g_pick_name = rname;
            if (zip_find(rd, ud, total, pick_exact, 0, &re, why, sizeof why) < 0) LOG("[boot] RAM disk: '%s' named by BuildManifest.plist is not in the archive (%s)\n", rname, why);
            else {
                uint8_t *rr = zip_extract(rd, ud, &re, why, sizeof why);
                if (!rr) LOG("[boot] RAM disk: '%s' could not be extracted: %s\n", rname, why);
                else { const char *s3 = NULL; rdk = to_plain(m, rr, (size_t)re.usz, "RestoreRamDisk", &rdlen, &s3); free(rr);
                    if (!rdk) LOG("[boot] RAM disk '%s' unusable: %s\n", rname, s3); }
            }
        }
        if (rdk) {
            uint64_t add = ((uint64_t)rdlen + 0x3FFFull) & ~0x3FFFull;
            if (ram + add > MAX_RAM) { LOG("[boot] RAM disk %zu bytes does not fit the %llu MiB guest RAM limit, not used\n", rdlen, (unsigned long long)(MAX_RAM >> 20)); free(rdk); rdk = NULL; }
            else { ram += add; LOG("[boot] RAM disk '%s': %zu bytes, guest RAM grown by %llu MiB for it\n", rname, rdlen, (unsigned long long)(add >> 20)); }
        }
    }
    uint32_t boot_reg = 0; int have_boot_reg = 0;
    if (dt) {
        char fn[40]; int nf = a64_dt_fixup(dt, dtlen, fn);
        if (nf < 0) LOG("[boot] DeviceTree: not a well-formed Apple tree, left untouched (no iBoot fix-up applied)\n");
        else if (nf == 0) LOG("[boot] DeviceTree: well-formed, no placeholder properties\n");
        else LOG("[boot] DeviceTree fix-up (MODELLED, a small subset of what iBoot does): cleared the placeholder flag (length bit 31) on %d properties, first: '%s'. Their data bytes are unchanged, not invented.\n", nf, fn);
        char cl[1600]; int bc = a64_dt_boot_cpu(dt, dtlen, &boot_reg, cl, sizeof cl);
        LOG("[boot] DeviceTree /cpus nodes:%s\n", cl[0] ? cl : " (none found)");
        if (bc == 1) { have_boot_reg = 1; LOG("[boot] DeviceTree fix-up (MODELLED, iBoot does this): first cpu node marked state='running' (boot CPU, reg=0x%x); MPIDR_EL1 will be modelled as 0x%x to match.\n", boot_reg, 0x80000000u | boot_reg); }
        else LOG("[boot] DeviceTree: no cpu node with a 'state' property found, boot CPU not marked\n");
        char ch[1600]; size_t eb = 0; int ne = a64_dt_entropy(dt, dtlen, host_random, &eb, ch, sizeof ch);
        { /* 0.4.38 (MODELLED): properties AppleImage4 requires in /chosen that iBoot creates at run time and that the IPSW DeviceTree does not have */
          size_t dtcap = dtlen + 8192; uint8_t *nd = realloc(dt, dtcap);
          if (nd) {
              dt = nd;
              int a1 = a64_dt_chosen_add_u32(dt, &dtlen, dtcap, "security-domain", 1);
              int a2 = a64_dt_chosen_add_u32(dt, &dtlen, dtcap, "chip-epoch", 0);
              if (rdk) { int ar = a64_dt_node_add_zero_prop(dt, &dtlen, dtcap, "memory-map", "RAMDisk", 16); LOG("[boot] DeviceTree fix-up (MODELLED, iBoot does this): /chosen/memory-map 'RAMDisk' %s (filled with the physical address and size once the layout is known)\n", ar == 1 ? "added" : ar == 0 ? "already present" : "NOT possible (no memory-map node?)"); }
              { /* 0.4.65/0.4.75: ApplePMGR.cpp:899 panics "voltage-statesN not found" one N per run (log 0.4.74: N=5 after N=1); every missing voltage-states0..15 becomes an empty property so the next runs do not stop on each index */
                char vn[24], added[96] = ""; int na3 = 0, bad = 0;
                char filled[96] = ""; int nfill = 0;
                static const uint32_t vs_ph[18] = { 1u, 0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u, 0u, 1u, 0u };   /* 0.4.88 PLACEHOLDER entry, see log text */
                static uint32_t vs_ph8[18]; for (int z = 0; z < 9; z++) { vs_ph8[2 * z] = 1000000u; vs_ph8[2 * z + 1] = 1u; }   /* 0.4.253 PLACEHOLDER for the ANE perf domain (id 8), see log text */
                for (int vi = 0; vi < 16; vi++) { snprintf(vn, sizeof vn, "voltage-states%d", vi); const uint32_t *vsp = vi == 8 ? vs_ph8 : vs_ph;
                  if (a64_dt_node_fill_empty_prop(dt, &dtlen, dtcap, "pmgr", vn, vsp, 72) == 1) { size_t l = strlen(filled); snprintf(filled + l, sizeof filled - l, "%s%d", nfill ? "," : "", vi); nfill++; continue; }   /* 0.4.85: an existing but EMPTY table */
                  int a3 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "pmgr", vn, vsp, 72);
                  if (a3 == 1) { size_t l = strlen(added); snprintf(added + l, sizeof added - l, "%s%d", na3 ? "," : "", vi); na3++; } else if (a3 < 0) bad = a3; }
                if (na3) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'voltage-states<N>' properties of NINE 8-byte PLACEHOLDER entries {1,0} (0.4.99: 9 = the number of rows of the IPSW's own pmgr/ifane-max; ONE entry made ApplePMGR.cpp:3084 'state < domain->perfStateCount' fail for PCPU) to the 'pmgr' node for N = %s (the IPSW tree lacks them; ApplePMGR.cpp:899 panics '%%s not found'; an EMPTY table makes the kernel index entry count-1 = -1 and fault, 0.4.76). The 8-byte entry stride is taken from the kernel's own code (index<<3 at 0x81713a0), what the two words mean is NOT known. ASSUMPTION: the real tables are chip-specific data that iBoot supplies; NO voltage/frequency values are invented, the entry {1,0} is a PLACEHOLDER (0.4.88): the kernel's own code (0x8171300..) counts the perf states of a type-1 domain (ECPU) as the number of leading entries with a NONZERO first word, so {0,0} gave 0 states and ApplePMGR.cpp:1303 panicked (log 0.4.87); the real first word is chip data from iBoot (UNKNOWN, NOT in the IPSW), 1 is NOT a real value, it only makes the count 1. Expect the derived frequency of that state to be meaningless\n", added);
                LOG("[boot] DeviceTree fix-up (MODELLED 0.4.253, PLACEHOLDER, NOT real data): 'voltage-states8' (perf domain id 8 = ANE) gets NINE entries {word0 = 1000000, word1 = 1} instead of {1,0}. Reason (ApplePMGR code 0x8171268..0x81715e0, ASSUMPTION about the field meaning): for this domain mode 0 the frequency is word0 / 1e6 MHz only when word1 (voltage) is non-zero, otherwise 0 MHz; with {1,0} every ANE state had frequency 0, the CLPC ANE table builder then counted 0 states and read index -1 (abort at 0x818adf4). 1 MHz / non-zero voltage are INVENTED placeholders, not iBoot data.\n");
                if (nfill) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): the IPSW tree has EMPTY (0 bytes) 'voltage-states<N>' tables for N = %s; an empty table gives a perf domain with 0 states and ApplePMGR.cpp:1303 panics 'ECPU0 ... request ECPU.0 > 0' (log 0.4.84, code decoded: needs state count > requested index 0). They now hold ONE entry {0,0} (8 zero bytes), like the first entry of the real tables (voltage-states0 = {0,0},{1,0},{2,0}). ASSUMPTION: iBoot fills the real voltages/states; NO values are invented\n", filled);
                if (bad) LOG("[boot] WARNING: could not add some 'voltage-states<N>' to the 'pmgr' node (%d)\n", bad); }
              { /* 0.4.407 MODELLED: iBoot passes the NVRAM image to the kernel in /chosen "nvram-proxy-data" (IODTNVRAM::initProxyData). The restore ramdisk's launchd/restored waits forever for IOResources 'IONVRAM'. We have no NOR/NVRAM storage: give the kernel an EMPTY (all-zero) 8 KiB image, i.e. a never-programmed NVRAM. PLACEHOLDER: no real NVRAM contents exist; nothing invented inside it. */
                static uint8_t nv_blank[0x2000]; int a4 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "chosen", "nvram-proxy-data", nv_blank, sizeof nv_blank);
                LOG("[boot] DeviceTree fix-up (MODELLED 0.4.407, PLACEHOLDER): /chosen 'nvram-proxy-data' = %u zero bytes (empty, never-programmed NVRAM; iBoot would pass the device's real image) -> %d (1 added, 0 already present). Reason: the restore daemon in the ramdisk waits for the IOResource 'IONVRAM' (console of 0.4.406).\n", (unsigned)sizeof nv_blank, a4); }
              { static const uint32_t mtr_ph[11] = { 1u, 3u, 0x5a5a5a5au,0x5a5a5a5au,0x5a5a5a5au, 0x5a5a5a5au,0x5a5a5a5au,0x5a5a5a5au, 0x5a5a5a5au,0x5a5a5a5au,0x5a5a5a5au };   /* 0.4.106 PLACEHOLDER: version 1, 3 entries of a constant NONZERO pattern (0.4.105 dump: the fit routine fails if any bit-field it unpacks is 0) */   /* 0.4.92 PLACEHOLDER: version 1, zero entries (see log text) */
                char mn[16], madd[96] = ""; int n7 = 0, bad7 = 0;
                for (int mi = 0; mi < 16; mi++) { snprintf(mn, sizeof mn, "mtr_%d", mi); int a7 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "pmgr", mn, mtr_ph, 44);
                  if (a7 == 1) { size_t l = strlen(madd); snprintf(madd + l, sizeof madd - l, "%s%d", n7 ? "," : "", mi); n7++; } else if (a7 < 0) bad7 = a7; }
                if (n7) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'mtr_<N>' = {1, 3, 9 x 0x5a5a5a5a} (44 bytes each, 0.4.106 PLACEHOLDER: the fit routine of MTRROCalibrate fails on zero bit-fields; values are meaningless, NOT real calibration) to the 'pmgr' node for N = %s. The kernel (ApplePMGRMTRSensor::agingRefDataFromProperty, code read from the log of 0.4.91, 0xfffffff008ce1750) looks each property up on its provider, requires data[0] == 1 and a length of (3*data[1]+2)*4 bytes; the IPSW tree has none (iBoot adds them: aging calibration data of the chip, UNKNOWN here). {1,0} = version 1 with ZERO entries is the smallest thing that passes the kernel's own check; it is a PLACEHOLDER, NOT real calibration data. Verified by a headless run (0.4.92): the kernel accepted 'mtr_0' on 'pmgr' and then asked for 'mtr_1', so the provider IS 'pmgr'. How many N exist is not known (16 tried). 'rosc-mtr_<N>' (the second name the caller passes) is NOT added\n", madd);
                if (bad7) LOG("[boot] WARNING: could not add some 'mtr_<N>' to 'pmgr' (%d)\n", bad7); }
              { static const uint64_t f24 = 24000000ull;   /* 0.4.82: log 0.4.81; 0.4.86: 8 bytes (log 0.4.85: the kernel read 0x13016e3600 = 4 bytes + the next 4 bytes of the tree) */
                int a6 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "cpu0", "fixed-frequency", &f24, 8);
                if (a6 == 1) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'fixed-frequency' = 24000000 (u64, 8 bytes: log 0.4.85 showed the kernel reading 8 bytes, the 4-byte property gave 0x13016e3600) to the 'cpu0' node. The IPSW tree carries none of the cpu frequency properties (bus-/memory-/peripheral-/fixed-/clock-frequency: iBoot fills them); the kernel's frequency globals (dump at 0xfffffff00914e128) are 0 except the timebase = 24 MHz, and the watchdog (AppleS5L8960XWatchDogTimer.cpp:55) panics 'clock speed 0 does not match absolute time'. ASSUMPTION: the watchdog's clock is the fixed oscillator and equals the timebase (24 MHz) - the kernel's own check demands exactly that, and 24 MHz is the timebase this emulator models. Bus/memory/peripheral/cpu frequencies stay absent (unknown, NOT invented)\n");
                else if (a6 != 0) LOG("[boot] WARNING: could not add 'fixed-frequency' to 'cpu0' (%d)\n", a6); }
        { /* 0.4.275 MODELLED (ASSUMPTION): the camera ISP and the video decoder (AVD) are not needed to reach /sbin/launchd, but their drivers panic when the hardware does not answer (0.4.259: AppleH10CamIn fISPVersion assert; 0.4.274: RTBuddy 'No response received' for the AVD coprocessor). The DeviceTree is built here, not by a real iBoot, so these nodes (and their DARTs) are removed. This is NOT what iBoot does on a real phone; it just means the kernel never sees these devices. Revert by deleting this block. */
            static const char *const rmn[] = { "isp", "avd", "dart-isp", "dart-avd" }; char rl[160] = ""; int nrm = 0;   /* 0.4.276: + pmp, pmp0 (0.4.275 log: after avd/isp were gone the next RTBuddy, PMP, panicked with "No response received") */
            for (int ri = 0; ri < 4; ri++) { int rr = a64_dt_node_remove(dt, &dtlen, rmn[ri]); if (rr == 1) { size_t l = strlen(rl); snprintf(rl + l, sizeof rl - l, "%s%s", nrm ? "," : "", rmn[ri]); nrm++; } else if (rr < 0) LOG("[boot] WARNING: DeviceTree malformed while removing '%s'\n", rmn[ri]); }
            { /* 0.4.331 MODELLED, ASSUMPTION: the platform functions whose providers (GPIO/SMC/PMU/accessory port drivers on real buses) cannot start here are hidden so IOKit threads do not wait for them forever */
              static const char *const fn[] = { "function-vbus_voltage", "function-reset", "function-bb_on", "function-external_standby", "function-vci", "function-pcie_port_control", "function-micbias-enable", "function-bb_rst"  };
              /* function-dock_parent is NOT hidden: IOAccessoryManager::start (downstream mode) dereferences the NULL upstream manager (log 0.4.335, "downstream manager has no upstream manager"). function-dispidle_ctrl is NOT hidden either: AppleSOCTuner::start REQUIREs _dispIdleCtrlFunction (panic, log 0.4.332). function-button_volup is NOT hidden: AppleM68Buttons::handleStart panics ("failed to instantiate") when it is missing (log 0.4.331) */
              for (size_t fi = 0; fi < sizeof fn / sizeof *fn; fi++) { char nn[40]; snprintf(nn, sizeof nn, "x-off-%s", fn[fi] + 9); int r2 = a64_dt_prop_rename(dt, dtlen, "*", fn[fi], nn);
                LOG("[boot] DeviceTree fix-up (MODELLED, ASSUMPTION; NOT what iBoot does, 0.4.331): property '%s' renamed to '%s' in %d node(s). Reason: its provider driver (real GPIO/SMC/PMU/accessory hardware) cannot run here and IOKit threads wait for it forever. Consequence: drivers that need this function (reset lines, vbus voltage, etc.) get 'no function' and the hardware is NOT driven. Reversible: delete this block.\n", fn[fi], nn, r2); } }
            { int rr = a64_dt_prop_rename(dt, dtlen, "aes", "wait-for", "x-wait-for-disabled"); LOG("[boot] DeviceTree fix-up (MODELLED, ASSUMPTION; NOT what iBoot does, 0.4.302): property 'wait-for' of node 'aes' (= \"AppleSmartIO\", real value from the IPSW DeviceTree) renamed to 'x-wait-for-disabled' (result %d). Reason: AppleS8000AES::start waits for the service AppleSmartIO forever; AppleSmartIO is an RTBuddy (RTKit coprocessor SIO) driver and the emulator has no RTKit firmware boot / mailbox responder for SIO. Consequence: the AES driver does not wait for the SIO coprocessor; any AES DMA through SIO would NOT work. Reversible: delete this block.\n", rr); }
            { /* 0.4.340 MODELLED, ASSUMPTION (NOT what iBoot does): there is no AES engine behind the 'aes' DT node here. With the node present AppleS8000AESAccelerator registers, and the thread that starts AppleMobileApNonce blocks forever in IOAESAccelerator::performAES waiting for a completion that no hardware can raise (log 0.4.339: stack 0x1b400000, frames 83d2d9c/83d33b8 under commandSleep), so the bootstrap never reaches bsd_utaskbootstrap/launchd. Removing the node means "no AES accelerator": drivers that want one see it missing (AppleMobileApNonce already logs 'Nonce entanglement requires AES accelerator'). Reversible: delete this block. A real engine model (FIFO commands, DART, UID key = PLACEHOLDER) is a later, separate task. */
              char pl2[200]; int na = a64_dt_node_remove_match(dt, &dtlen, "aes,s8000", pl2, sizeof pl2); LOG("[boot] DeviceTree fix-up (MODELLED, ASSUMPTION, 0.4.340): removed %d node(s) whose compatible contains 'aes,s8000' (the AES engine, not modelled): %s\n", na, pl2); }
            { char pl[200]; int np2 = a64_dt_node_remove_match(dt, &dtlen, "apcie", pl, sizeof pl); LOG("[boot] DeviceTree fix-up (MODELLED, ASSUMPTION; NOT what iBoot does, 0.4.283): removed %d node(s) whose name or compatible contains 'apcie' (PCIe/NVMe flash controller; the root is the RAM disk md0, and 0.4.282 shows the kernel spinning forever polling bit 2 of register 0 at physical 0x600080000, an apcie range, with no timeout): %s\n", np2, pl); }
            { char ml[200]; int nm2 = a64_dt_node_remove_match(dt, &dtlen, "ad5860", ml, sizeof ml); LOG("[boot] DeviceTree fix-up (MODELLED, ASSUMPTION; NOT what iBoot does, 0.4.277): removed %d node(s) whose name or compatible contains 'ad5860' (audio amplifier on i2c0; 0.4.276 log: its driver retries failed I2C transfers for hundreds of millions of steps): %s\n", nm2, ml); }
            LOG("[boot] DeviceTree fix-up (MODELLED, ASSUMPTION; NOT what iBoot does): removed %d node(s): %s\n", nrm, rl); }
        { /* 0.4.222 MODELLED (ASSUMPTION, PLACEHOLDER): log 0.4.219: AppleT802xPCIe::configure panics 'REQUIRE _phyTunables != nullptr' (AppleT802xPCIe.cpp:135). The kernel reads the 'apcie' properties apcie-phy-tunables / apcie-phy-ip-tunables / apcie-axi2af-tunables (strings at 0x8cea130..0x8cea478, executed); the IPSW tree has none of them (iBoot adds the chip's PHY tuning). We add them EMPTY (0 entries: the kernel checks numTunables*sizeof(spdsTunable)==length, so 0 is valid). NO tuning values are invented; a real PCIe link would need the real ones, here nothing is attached to the PCIe bus anyway. */
          static const char *tn[3] = { "apcie-phy-tunables", "apcie-phy-ip-tunables", "apcie-axi2af-tunables" }; uint32_t dummy = 0;
          for (int z = 0; z < 3; z++) { int r = a64_dt_node_add_prop(dt, &dtlen, dtcap, "apcie", tn[z], &dummy, 0);
              if (r == 1) LOG("[boot] DeviceTree fix-up (MODELLED 0.4.222, PLACEHOLDER): added EMPTY property '%s' (0 tunables) to node 'apcie' - iBoot normally supplies the chip's PCIe PHY tuning; not in the IPSW; no values invented\n", tn[z]);
              else LOG("[boot] WARNING: could not add '%s' to 'apcie' (%d)\n", tn[z], r); } }
              { int64_t cl = -1; char lg[256]; int a4 = a64_dt_node_info(dt, dtlen, "arm-io", "clock-frequencies", &cl, lg, sizeof lg);   /* 0.4.74: log 0.4.73: method 0x814fc38 returns 0 unless arm-io has BOTH clock-frequencies and clock-frequencies-nclk */
                if (a4 >= 0 && cl > 0) { int a5 = a64_dt_node_add_zero_prop(dt, &dtlen, dtcap, "arm-io", "clock-frequencies-nclk", (size_t)cl);
                  if (a5 == 1) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'clock-frequencies-nclk' (%lld zero bytes, same size as 'clock-frequencies') to /arm-io. AppleT802xIO::start needs both properties; the IPSW tree has only 'clock-frequencies' (all zeros). ASSUMPTION: iBoot fills both with chip data that is UNKNOWN here; NO frequency or clock-type values are invented. Expect the watchdog check (clock speed vs 24 MHz timebase) to still see 0 until the real table layout is understood\n", (long long)cl);
                  else if (a5 != 0) LOG("[boot] WARNING: could not add 'clock-frequencies-nclk' to /arm-io (%d)\n", a5); } }
              if (a1 == 1) LOG("[boot] DeviceTree fix-up (MODELLED, iBoot does this): added /chosen 'security-domain' = 1 (the production security domain: ApSecurityDomain 1 of retail iOS firmware). AppleImage4 requires it.\n");
              if (a2 == 1) LOG("[boot] DeviceTree fix-up (MODELLED, iBoot does this): added /chosen 'chip-epoch' = 0 (the name AppleImage4 looks up for CEPO, read from the kernel's own table). ASSUMPTION: the real value is a chip property that iBoot supplies; it is not in the IPSW and is unknown here, 0 is a placeholder. Nothing is verified against it in this emulator.\n");
              if (a1 < 0 || a2 < 0) LOG("[boot] DeviceTree: could not add the missing /chosen properties (tree malformed or no room)\n");
          }
        }
        if (ne > 0) LOG("[boot] DeviceTree fix-up (MODELLED, iBoot does this): filled %d empty entropy placeholder(s) in /chosen (%zu bytes) with FIXED pseudo-random bytes (constant seed, for reproducible runs; real iBoot uses a hardware RNG)\n", ne, eb);
        else if (ne == 0) LOG("[boot] DeviceTree: no empty random-seed/entropy-data property in /chosen to fill\n");
    }

    /* build the real machine, move the log over */
    /* 0.4.18 (MODELLED): the kernel puts the first ~500 MiB of free pages (secluded pool, [0x912b860]=0x1f400000) on the secluded queue before the normal
     * free queue; with the minimal 114 MiB of 0.4.17 every page went there, vm_page_free_count stayed 0 and pmap_startup woke the (not yet initialised)
     * global waitq with 0 free pages -> "Invalid waitq". A real iPhone XS has 4 GiB, so the guest gets more RAM: 768 MiB, then 640 MiB, then the minimum
     * if the host cannot give that much. RAM is lazily committed by the host (calloc), only touched pages cost memory. */
    A64Machine *sys = NULL;
    { static const uint64_t want[5] = { 2048ull << 20, 1536ull << 20, 1024ull << 20, 768ull << 20, 640ull << 20 };   /* 0.4.396 ASSUMPTION: more guest RAM (a real XS has 4 GiB) because with 768 MiB the first user VM fault starves (see CHANGELOG) */
      for (int wi = 0; wi < 5 && !sys; wi++) if (want[wi] > ram) { sys = a64m_create_system_at(want[wi], A64M_IOS_RAM_BASE); if (sys) ram = want[wi]; }
      if (!sys) sys = a64m_create_system_at(ram, A64M_IOS_RAM_BASE); }
    if (!sys) { free(kern); free(dt); return dead("out of memory for guest RAM", "step 6", m); }
    { char b[65536]; size_t off = 0, k; while ((k = a64m_log_read(log_only, off, b, sizeof b)) > 0) { for (size_t q = 0; q < k; q += 400) { char t[401]; size_t c = k - q < 400 ? k - q : 400; memcpy(t, b + q, c); t[c] = 0; a64m_log(sys, "%s", t); } off += k; } }
    a64m_destroy(log_only); m = sys;
    /* 0.4.354 MODELLED, ASSUMPTION (NOT what a real device does): the kernel's IOSecureBSDRoot asks AppleARMPE ("SecureRootName") whether the root is secure; the platform expert then WAITS for the SEP key store (the module that logs "Creating Special Keys Twice") to answer with "SecureRootCallBack". There is no SEP and no AES engine here, so that answer never comes and bsd_init never reaches launchd (log 0.4.352: the answering branch ran 0 times). The hook answers on its behalf with the ONLY honest outcome available: it sets the "answered" flag [obj+0x10b]=1 and the "secure" result [obj+0x10c]=0, so the platform function returns kIOReturnNotPrivileged = "root NOT verified secure". The secure-root check is NOT passed and NO cryptography is faked; the kernel continues the way it does for a non-secure root. The hook fires at the first flag test, pc 0x8151c9c, specific to this kernelcache (iOS 12.0 16A366). Reversible: delete this block. */
    LOG("[boot] MODELLED hook (ASSUMPTION, 0.4.354): no SEP/AES here, so AppleARMPE's SecureRootName request is answered as 'root NOT verified secure' (kIOReturnNotPrivileged) at pc 0xfffffff008151c9c. The secure-root check is NOT passed; no cryptography is faked.\n");
    a64m_cpu(m)->wp2_pc = 0xfffffff0089a7744ull; /* 0.4.547: entry of the ApplePMP sync-call wait: x0 = this (read vtable pointer after the run) */ (void)0; a64m_cpu(m)->fp_pc = 0xfffffff00814a8f0ull;   /* 0.4.460: instruction after the setter call in ApplePMGR::start: x0 = setter return value */
    a64m_cpu(m)->wp_pc = 0xfffffff0082e9becull; /* 0.4.565: entry of RTBuddy power-state method (setPowerState(state=x1, device=x2)); was the IOFunctionParent setter hook */ (void)0;   /* 0.4.459 diagnostic: entry of the IOFunctionParent setter (x0 = DT nub, x1 = service); snapshots only for the call from ApplePMGR::start (lr 0x816bdcc) */
    a64m_cpu(m)->hk_pc = 0xfffffff008151c9cull;
    LOG("[boot] MODELLED hook (ASSUMPTION, 0.4.378): because the root is answered as NOT verified secure, the kernel's IOSecureBSDRoot would call its 'remove all memory devices' loop (at 0xfffffff0080eaaac), deleting md0 - the RAM disk that is the kernel root - so every later read of it fails with ENXIO (0.4.375 dump). The emulator SKIPS that loop and keeps md0. The root is still NOT claimed to be secure; no check is passed and no cryptography is faked.\n");
    /* 0.4.645 diagnostic (changes nothing): trace the first RTBuddy::setPowerState call whose object has [+176] low 24 bits 0x94abc0 (the ANS2 RTBuddy of this DeviceTree) */
    { A64 *cr = a64m_cpu(m); cr->ct_arm_pc = 0; cr->ct_off = ~0ull; cr->ct_val = 0xd1f800ull;  /* object address (ANS2 RTBuddy, same heap address in the last runs; [+176] low bits vary between builds) */ cr->ct_cap = 0; cr->ct_max = 400000; }
    a64m_cpu(m)->jmp_pc = 0xfffffff0080eaaacull; a64m_cpu(m)->jmp_to = 0xfffffff0080eaaf0ull;
    uint64_t kphys = A64M_IOS_RAM_BASE + kbase_off;
    for (int i = 0; i < mo.nseg; i++) {
        const A64Seg *s = &mo.seg[i];
        if (!s->vmsize || !strcmp(s->name, "__PAGEZERO")) continue;
        uint64_t pa = kphys + (s->vmaddr - mo.lowest_vm);
        uint8_t *dst = a64m_ram_at(m, pa, s->vmsize);
        if (!dst || s->fileoff + s->filesize > klen - mo.slice_off || s->filesize > s->vmsize) { free(kern); free(dt); a64m_abort(m, "step 6 - segment does not fit (corrupt kernel or layout)"); return m; }
        memcpy(dst, kern + mo.slice_off + s->fileoff, s->filesize);
        LOG("[boot] step 6: %-16s %9llu bytes at physical 0x%llx (virtual 0x%llx)\n", s->name, (unsigned long long)s->filesize, (unsigned long long)pa, (unsigned long long)s->vmaddr);
    }
    { /* 0.4.125 diagnostic (changes nothing): where does the kernel itself mention the RAM disk / root device? Strings with a few characters of context. */
      static const char *const kws[] = { "RAMDisk", "md0", "rd=", "rootdev", "root-matching", "mdev", "AppleRAMDisk", "Apple_HFS", "root_device", "bsd root" };
      for (size_t ki = 0; ki < sizeof kws / sizeof *kws; ki++) { size_t kl = strlen(kws[ki]); int hits = 0; char line[700]; size_t ll = 0; ll += (size_t)snprintf(line, sizeof line, "[boot] diagnostic (changes nothing): kernel strings containing '%s':", kws[ki]);
        for (size_t i = 0; i + kl <= klen && hits < 8; i++) if (!memcmp(kern + i, kws[ki], kl)) {
            size_t a = i, e = i + kl; while (a > 0 && i - a < 60 && kern[a - 1] >= 32 && kern[a - 1] < 127) a--; while (e < klen && e - i < 80 && kern[e] >= 32 && kern[e] < 127) e++;
            if (e - a > 150) continue; ll += (size_t)snprintf(line + ll, sizeof line - ll, " [%.*s]", (int)(e - a), kern + a); hits++; if (ll > 560) break; i = e; }
        LOG("%s (%d)\n", line, hits); } }
    { /* 0.4.480 diagnostic (changes nothing): where does the kernel build the RTKit "bad version" pattern 0x0badffff? Instructions MOVK Wd,#0xbad,LSL #16 and their virtual addresses. */
      char ln[800]; size_t lw = (size_t)snprintf(ln, sizeof ln, "[boot] diagnostic (changes nothing, 0.4.480): MOVK w,#0xbad,lsl#16 at virtual:"); int nh2 = 0;
      for (int q = 0; q < mo.nseg && nh2 < 12; q++) { const A64Seg *g = &mo.seg[q];
        for (uint64_t off = 0; off + 4 <= g->filesize && nh2 < 12; off += 4) { size_t fi = mo.slice_off + g->fileoff + off; if (fi + 4 > klen) break; uint32_t w = (uint32_t)kern[fi] | (uint32_t)kern[fi + 1] << 8 | (uint32_t)kern[fi + 2] << 16 | (uint32_t)kern[fi + 3] << 24;
          if ((w & 0x7fe00000u) == 0x72a00000u && ((w >> 5) & 0xffff) == 0xbad) { lw += (size_t)snprintf(ln + lw, sizeof ln - lw, " 0x%llx", (unsigned long long)(g->vmaddr + off)); nh2++; } } }
      LOG("%s (%d)\n", ln, nh2); }
    { /* 0.4.508 diagnostic (changes nothing): BL/B call sites of the IOFunctionParent name builder 0x814a870 and the waiter 0x814a8f0 */
      static const uint64_t tg[2] = { 0xfffffff00814a870ull, 0xfffffff00814a8f0ull };
      for (int ti = 0; ti < 2; ti++) { char ln[900]; size_t lw = (size_t)snprintf(ln, sizeof ln, "[boot] diagnostic (changes nothing, 0.4.508): callers (BL) of 0x%llx:", (unsigned long long)tg[ti]); int nh2 = 0;
        for (int q = 0; q < mo.nseg && nh2 < 40; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4)) continue;
          for (uint64_t off = 0; off + 4 <= g->filesize && nh2 < 40; off += 4) { size_t fi = mo.slice_off + g->fileoff + off; if (fi + 4 > klen) break; uint32_t w = (uint32_t)kern[fi] | (uint32_t)kern[fi + 1] << 8 | (uint32_t)kern[fi + 2] << 16 | (uint32_t)kern[fi + 3] << 24;
            if ((w & 0xfc000000u) != 0x94000000u) continue; int64_t im = (int64_t)(w & 0x3ffffffu); if (im & 0x2000000) im -= 0x4000000;
            if (g->vmaddr + off + (uint64_t)(im * 4) == tg[ti]) { lw += (size_t)snprintf(ln + lw, sizeof ln - lw, " 0x%llx", (unsigned long long)(g->vmaddr + off)); nh2++; } } }
        LOG("%s (%d)\n", ln, nh2); } }
    { /* 0.4.540 diagnostic (changes nothing): who uses the PMGR callback 0x89a7690 and waiter 0x89a7740? BL/B callers plus 8-byte data words whose low 32 bits equal the address (vtable slots / registered callbacks; PAC bits ignored) with neighbouring words */
      static const uint64_t tg[3] = { 0xfffffff0089a7690ull, 0xfffffff0089a7740ull, 0xfffffff0089a7744ull };
      for (int ti = 0; ti < 3; ti++) { char ln[1600]; size_t lw = (size_t)snprintf(ln, sizeof ln, "[boot] diagnostic (changes nothing, 0.4.540): refs to 0x%llx: BL from", (unsigned long long)tg[ti]); int nh2 = 0, nd = 0;
        for (int q = 0; q < mo.nseg && nh2 < 12; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4)) continue;
          for (uint64_t off = 0; off + 4 <= g->filesize && nh2 < 12; off += 4) { size_t fi = mo.slice_off + g->fileoff + off; if (fi + 4 > klen) break; uint32_t w; memcpy(&w, kern + fi, 4);
            if ((w & 0x7c000000u) != 0x14000000u) continue; int64_t im = (int64_t)(w & 0x3ffffffu); if (im & 0x2000000) im -= 0x4000000;
            if (g->vmaddr + off + (uint64_t)(im * 4) == tg[ti]) { lw += (size_t)snprintf(ln + lw, sizeof ln - lw, " 0x%llx", (unsigned long long)(g->vmaddr + off)); nh2++; } } }
        lw += (size_t)snprintf(ln + lw, sizeof ln - lw, "; data words:");
        for (int q = 0; q < mo.nseg && nd < 4; q++) { const A64Seg *g = &mo.seg[q]; if (g->initprot & 4) continue;
          for (uint64_t off = 0; off + 8 <= g->filesize && nd < 4; off += 8) { size_t fi = mo.slice_off + g->fileoff + off; if (fi + 8 > klen) break; uint64_t v; memcpy(&v, kern + fi, 8);
            if ((v & 0xffffffffull) != (tg[ti] & 0xffffffffull)) continue;
            lw += (size_t)snprintf(ln + lw, sizeof ln - lw, " [0x%llx:", (unsigned long long)(g->vmaddr + off));
            for (int64_t d = -8; d <= 8; d++) { int64_t o2 = (int64_t)off + d * 8; if (o2 < 0 || (uint64_t)o2 + 8 > g->filesize || mo.slice_off + g->fileoff + (uint64_t)o2 + 8 > klen) continue; uint64_t v2; memcpy(&v2, kern + mo.slice_off + g->fileoff + (uint64_t)o2, 8); lw += (size_t)snprintf(ln + lw, sizeof ln - lw, "%s%llx", d == 0 ? " >" : " ", (unsigned long long)(v2 & 0xffffffffull)); }
            lw += (size_t)snprintf(ln + lw, sizeof ln - lw, "]"); nd++; } }
        LOG("%s (%d BL, %d data)\n", ln, nh2, nd); } }
    { /* 0.4.126 diagnostic (changes nothing): code that references the strings 'RAMDisk' and 'root-matching' (ADRP+ADD pairs); the first hit of each gets a code dump window */
      static const char *const xs[] = { "EndpointNN", "RTBuddyEndpointService" };   /* 0.4.529 */
      for (int xi = 0; xi < 2; xi++) { size_t kl = strlen(xs[xi]) + 1; uint64_t sva = 0;
        for (size_t i = 0; i + kl <= klen && !sva; i++) if (!memcmp(kern + i, xs[xi], kl) && (i == 0 || kern[i - 1] == 0)) {
            for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; uint64_t off = i - mo.slice_off; if (i >= mo.slice_off && off >= g->fileoff && off < g->fileoff + g->filesize) { sva = g->vmaddr + (off - g->fileoff); break; } } }
        char hl[400]; size_t hn = 0; int nh = 0; hn += (size_t)snprintf(hl, sizeof hl, "[boot] diagnostic (changes nothing): string '%s' at virtual 0x%llx; ADRP+ADD references from:", xs[xi], (unsigned long long)sva);
        for (int q = 0; q < mo.nseg && sva && nh < 4; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4) || mo.slice_off + g->fileoff + g->filesize > klen) continue;
          for (uint64_t o = 0; o + 8 <= g->filesize && nh < 4; o += 4) { uint32_t w1, w2; memcpy(&w1, kern + mo.slice_off + g->fileoff + o, 4); memcpy(&w2, kern + mo.slice_off + g->fileoff + o + 4, 4);
            if ((w1 & 0x9f000000u) != 0x90000000u || (w2 & 0xffc00000u) != 0x91000000u || (w1 & 31) != ((w2 >> 5) & 31)) continue;
            int64_t imm = (int64_t)((((w1 >> 5) & 0x7ffffu) << 2) | ((w1 >> 29) & 3)); if (imm & 0x100000) imm -= 0x200000;
            uint64_t pc = g->vmaddr + o, tgt = ((pc & ~0xfffull) + (uint64_t)(imm << 12)) + ((w2 >> 10) & 0xfff);
            if (tgt == sva) { hn += (size_t)snprintf(hl + hn, sizeof hl - hn, " 0x%llx", (unsigned long long)pc); if (nh < 2 && xi * 2 + nh < 4) g_xref[xi * 2 + nh] = pc; nh++; } } }
        LOG("%s (%d)\n", hl, nh); } }
    { /* 0.4.151 diagnostic (changes nothing): code that references the string containing 'mounted %s on device' (hfs mount message) and a window of code before it */
      size_t kl2 = 20; uint64_t sva2 = 0; size_t si = 0;
      for (size_t i = 1; i + kl2 <= klen && !sva2; i++) if (!memcmp(kern + i, "mounted %s on device", kl2)) { si = i; while (si > 0 && kern[si - 1] != 0) si--;
          for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; uint64_t off = si - mo.slice_off; if (si >= mo.slice_off && off >= g->fileoff && off < g->fileoff + g->filesize) { sva2 = g->vmaddr + (off - g->fileoff); break; } } }
      uint64_t xr = 0; char hl[300]; size_t hn = (size_t)snprintf(hl, sizeof hl, "[boot] diagnostic (changes nothing): string '%s' at virtual 0x%llx; ADRP+ADD references from:", sva2 ? (const char *)(kern + si) : "?", (unsigned long long)sva2);
      for (int q = 0; q < mo.nseg && sva2; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4) || mo.slice_off + g->fileoff + g->filesize > klen) continue;
        for (uint64_t o = 0; o + 8 <= g->filesize; o += 4) { uint32_t w1, w2; memcpy(&w1, kern + mo.slice_off + g->fileoff + o, 4); memcpy(&w2, kern + mo.slice_off + g->fileoff + o + 4, 4);
          if ((w1 & 0x9f000000u) != 0x90000000u || (w2 & 0xffc00000u) != 0x91000000u || (w1 & 31) != ((w2 >> 5) & 31)) continue;
          int64_t imm = (int64_t)((((w1 >> 5) & 0x7ffffu) << 2) | ((w1 >> 29) & 3)); if (imm & 0x100000) imm -= 0x200000;
          uint64_t pc = g->vmaddr + o, tgt = ((pc & ~0xfffull) + (uint64_t)(imm << 12)) + ((w2 >> 10) & 0xfff);
          if (tgt == sva2) { if (!xr || (!(xr >= 0xfffffff007ed0000ull && xr < 0xfffffff007ee0000ull) && pc >= 0xfffffff007ed0000ull && pc < 0xfffffff007ee0000ull)) xr = pc; /* 0.4.343: prefer the reference in 0x7ed0000..0x7ee0000 (load_init_program), not the first one found (a kext/policy at 0x8e7ef14) */ hn += (size_t)snprintf(hl + hn, sizeof hl - hn, " 0x%llx", (unsigned long long)pc); } } }
      LOG("%s\n", hl);
      xr = 0xfffffff007ed1db0ull;   /* 0.4.344 diagnostic, ASSUMPTION specific to this build (iOS 12.0 16A366): the load_init_program reference found by the [vs] scan (adrp/add not adjacent, so the pair scan above misses it) */
      if (xr) { /* 0.4.159: who calls the function that references '/sbin/launchd'? breadth-first, 5 levels, function start = nearest preceding pacibsp (ASSUMPTION; a heuristic) */
        uint64_t cur[8], nxt[8]; int ncur = 0, nnx; g_ch_n = 0;
        { uint64_t f = xr; for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (xr >= g->vmaddr && xr < g->vmaddr + g->filesize && mo.slice_off + g->fileoff + g->filesize <= klen) { for (uint64_t o = xr - g->vmaddr; o >= 4 && xr - g->vmaddr - o < 0x4000; o -= 4) { uint32_t w; memcpy(&w, kern + mo.slice_off + g->fileoff + o, 4); if (w == 0xd503237fu) { f = g->vmaddr + o; break; } } break; } }
          cur[ncur++] = f; g_ch_fn[g_ch_n] = f; g_ch_site[g_ch_n] = xr; g_ch_d[g_ch_n++] = 0; }
        for (int dep = 1; dep <= 8 && ncur && g_ch_n < 44; dep++) { nnx = 0;
          for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4) || mo.slice_off + g->fileoff + g->filesize > klen) continue;
            for (uint64_t o = 0; o + 4 <= g->filesize && nnx < 8; o += 4) { uint32_t w; memcpy(&w, kern + mo.slice_off + g->fileoff + o, 4); if ((w & 0xfc000000u) != 0x94000000u && (w & 0xfc000000u) != 0x14000000u) continue;
              int64_t d = (int64_t)(w & 0x3ffffffu); if (d & 0x2000000) d -= 0x4000000; uint64_t pc = g->vmaddr + o, tg = pc + (uint64_t)(d * 4); int hit = 0; for (int k = 0; k < ncur; k++) if (cur[k] == tg) hit = 1; if (!hit) continue;
              uint64_t f = pc; for (uint64_t o2 = o; o2 >= 4 && o - o2 < 0x4000; o2 -= 4) { uint32_t w2; memcpy(&w2, kern + mo.slice_off + g->fileoff + o2, 4); if (w2 == 0xd503237fu) { f = g->vmaddr + o2; break; } }
              int dup = 0; for (int k = 0; k < nnx; k++) if (nxt[k] == f) dup = 1; if (dup) continue; nxt[nnx++] = f; if (g_ch_n < 48) { g_ch_fn[g_ch_n] = f; g_ch_site[g_ch_n] = pc; g_ch_d[g_ch_n++] = dep; } } }
          ncur = nnx; memcpy(cur, nxt, sizeof nxt); } }
      for (int k = 0; xr && k < g_ch_n; k++) { uint64_t bf = g_ch_fn[k];   /* 0.4.162: every bl of each chain function (up to the next pacibsp or 0x1800 bytes), to see where an executed function stopped */
        for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (bf < g->vmaddr || bf >= g->vmaddr + g->filesize || mo.slice_off + g->fileoff + g->filesize > klen) continue;
          for (uint64_t pc = bf; pc < bf + 0x1800 && pc + 4 <= g->vmaddr + g->filesize && g_bi_n < 600 && g_bi_cnt[k] < 60; pc += 4) { uint32_t w; memcpy(&w, kern + mo.slice_off + g->fileoff + (pc - g->vmaddr), 4); if (pc > bf && w == 0xd503237fu) break; if ((w & 0xfc000000u) != 0x94000000u) continue; int64_t d = (int64_t)(w & 0x3ffffffu); if (d & 0x2000000) d -= 0x4000000; if (!g_bi_cnt[k]) g_bi_first[k] = g_bi_n; g_bi_cnt[k]++; g_bi_site[g_bi_n] = pc; g_bi_tg[g_bi_n++] = pc + (uint64_t)(d * 4); } break; } }
      if (xr) a64m_cpu(m)->hist_pc = xr;   /* 0.4.151: the run's [hist] line then says whether this code (the first reference to '/sbin/launchd') is ever executed */
      if (xr) for (uint64_t a = (xr - 0x300) & ~0x7full; a < xr + 0x100; a += 0x80) { char dl[1200]; size_t dn = (size_t)snprintf(dl, sizeof dl, "[stop] code 0x%llx (kernel image dump 0.4.143):", (unsigned long long)a); int okd = 0;
        for (int q = 0; q < mo.nseg && !okd; q++) { const A64Seg *g = &mo.seg[q]; if (a >= g->vmaddr && a + 0x80 <= g->vmaddr + g->filesize && mo.slice_off + g->fileoff + (a - g->vmaddr) + 0x80 <= klen) { okd = 1;
            for (int w = 0; w < 32; w++) { uint32_t x; memcpy(&x, kern + mo.slice_off + g->fileoff + (a - g->vmaddr) + 4 * (uint64_t)w, 4); dn += (size_t)snprintf(dl + dn, sizeof dl - dn, " %08x", x); } } }
        if (okd) LOG("%s\n", dl); } }
    { /* 0.4.408 diagnostic (changes nothing): printable strings of the kernelcache that contain 'nvram' (any case), distinct, with file offsets: which classes / properties does this kernel have for NVRAM? */
      char nl[7000]; size_t nn = (size_t)snprintf(nl, sizeof nl, "[boot] diagnostic (changes nothing): kernelcache strings containing Endpoint (0.4.528, file offset:string):"); int cnt = 0; static uint64_t seenh[512];
      for (size_t o = 0; o + 16 < klen && cnt < 120 && nn < sizeof nl - 200; o++) {
        if (memcmp(kern + o, "Endpoint", 8) && memcmp(kern + o, "%sEndpoint", 10) && memcmp(kern + o, "Endpoint%", 9)) continue;   /* 0.4.528: Endpoint name format strings */   /* 0.4.498 */   /* 0.4.464: ApplePMGR strings */   /* 0.4.450: RTBuddy firmware service strings */   /* 0.4.434: 'aic,' (DT compatible) or 'pleint' (AppleInterrupt...) */
        size_t b = o; while (b > 0 && o - b < 60 && kern[b - 1] >= 32 && kern[b - 1] < 127) b--;
        size_t e = o; while (e < klen && e - o < 80 && kern[e] >= 32 && kern[e] < 127) e++;
        if (e - b < 6) continue; uint64_t h = 1469598103934665603ull; for (size_t q = b; q < e; q++) h = (h ^ kern[q]) * 1099511628211ull;
        int dup = 0; for (int q = 0; q < cnt && q < 512; q++) if (seenh[q] == h) { dup = 1; break; } if (dup) { o = e; continue; }
        seenh[cnt] = h; cnt++; nn += (size_t)snprintf(nl + nn, sizeof nl - nn, " %zx:%.*s |", b, (int)(e - b), (const char *)kern + b); o = e; }
      LOG("%s (%d)\n", nl, cnt); }
    { /* 0.4.411 diagnostic (changes nothing): where does the kernel call IOService::publishResource? Find the string "_publishIOResource: " (AppleCredentialManager), its ADRP+ADD xrefs, and dump the code around the first two (0x1c0 bytes before .. 0x200 after) so the bl to publishResource can be identified offline. */
      static const char key[] = "IOFunctionParent%08X"; uint64_t saddr = 0;
      for (size_t o = 1; o + sizeof key < klen && !saddr; o++) if (kern[o - 1] == 0 && !memcmp(kern + o, key, sizeof key - 1)) for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (mo.slice_off + g->fileoff <= o && o < mo.slice_off + g->fileoff + g->filesize) { saddr = g->vmaddr + (o - mo.slice_off - g->fileoff); break; } }
      LOG("[boot] diagnostic (changes nothing): string IOInterruptController%%08X (key) at virtual 0x%llx (0.4.411)\n", (unsigned long long)saddr);
      int nx = 0;
      for (int q = 0; q < mo.nseg && saddr && nx < 6; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4) || mo.slice_off + g->fileoff + g->filesize > klen) continue;
        for (uint64_t o = 0; o + 8 <= g->filesize && nx < 6; o += 4) { uint64_t pc = g->vmaddr + o; uint32_t w1, w2; memcpy(&w1, kern + mo.slice_off + g->fileoff + o, 4); memcpy(&w2, kern + mo.slice_off + g->fileoff + o + 4, 4);
          if ((w1 & 0x9f000000u) != 0x90000000u || (w2 & 0xffc00000u) != 0x91000000u || (w1 & 31) != ((w2 >> 5) & 31)) continue;
          int64_t imm = (int64_t)((((w1 >> 5) & 0x7ffffu) << 2) | ((w1 >> 29) & 3)); if (imm & 0x100000) imm -= 0x200000;
          uint64_t tgt = ((pc & ~0xfffull) + (uint64_t)(imm << 12)) + ((w2 >> 10) & 0xfff); if (tgt != saddr) continue;   /* 0.4.431: exact target */
          nx++; LOG("[boot] diagnostic (changes nothing): xref #%d of the key string (0.4.431) at pc 0x%llx\n", nx, (unsigned long long)pc);
          for (uint64_t a2 = (pc - 0x1c0) & ~0x7full; a2 < pc + 0x100; a2 += 0x80) { char dl[1200]; size_t dn = (size_t)snprintf(dl, sizeof dl, "[stop] code 0x%llx (kernel image dump 0.4.127):", (unsigned long long)a2);
            if (a2 >= g->vmaddr && a2 + 0x80 <= g->vmaddr + g->filesize) { for (int w = 0; w < 32; w++) { uint32_t x; memcpy(&x, kern + mo.slice_off + g->fileoff + (a2 - g->vmaddr) + 4 * (uint64_t)w, 4); dn += (size_t)snprintf(dl + dn, sizeof dl - dn, " %08x", x); } LOG("%s\n", dl); } } } } }
    { /* 0.4.456 diagnostic (changes nothing): callers (BL) of the functions that build/use "IOFunctionParent%08X": 0x814a870 (name builder) and 0x814a944 (waiter) and of 0x814a8ec (wrapper) */
      static const uint64_t tg[3] = { 0xfffffff00814a7a0ull, 0xfffffff00814a870ull, 0xfffffff00814a944ull };   /* 0.4.457: 0x814a7a0 = the function that SETS IOFunctionParent%08X (getProperty phandle -> name -> this->setProperty) */ char cl[1500]; size_t cn = 0; int cnt[3] = { 0, 0, 0 };
      cn += (size_t)snprintf(cl, sizeof cl, "[boot] diagnostic (changes nothing): BL/B callers of 0x814a7a0 [0] / 0x814a870 [1] / 0x814a944 [2] (0.4.457):");
      for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4) || mo.slice_off + g->fileoff + g->filesize > klen) continue;
        for (uint64_t o = 0; o + 4 <= g->filesize; o += 4) { uint32_t w; memcpy(&w, kern + mo.slice_off + g->fileoff + o, 4); if ((w & 0xfc000000u) != 0x94000000u) continue; int64_t im = (int64_t)(w & 0x3ffffffu); if (im & 0x2000000) im -= 0x4000000; uint64_t t = g->vmaddr + o + (uint64_t)(im * 4);
          for (int k = 0; k < 3; k++) if (t == tg[k] && cnt[k] < 8 && cn < sizeof cl - 60) { cnt[k]++; cn += (size_t)snprintf(cl + cn, sizeof cl - cn, " [%d]0x%llx", k, (unsigned long long)(g->vmaddr + o)); } } }
      LOG("%s\n", cl); }
    { /* 0.4.410 diagnostic (changes nothing): all printable strings (>=3 chars) in two kernelcache windows around the NVRAM controller class names (offsets from 0.4.408): which names does AppleARMCHRPNVRAM / IONVRAMController sit next to? */
      static const size_t win[2][2] = { { 0x4d2080, 0x4d2300 }, { 0x4cfe00, 0x4d0000 } }; char wl[3500]; size_t wn = 0;
      for (int wi = 0; wi < 2; wi++) { wn += (size_t)snprintf(wl + wn, sizeof wl - wn, "[boot] diagnostic (changes nothing): kernelcache strings in file window 0x%zx..0x%zx (0.4.410):", win[wi][0], win[wi][1]);
        for (size_t o = win[wi][0]; o < win[wi][1] && o < klen && wn < sizeof wl - 150;) { size_t e = o; while (e < klen && e < win[wi][1] && kern[e] >= 32 && kern[e] < 127) e++; if (e - o >= 3) { wn += (size_t)snprintf(wl + wn, sizeof wl - wn, " %.*s |", (int)(e - o), (const char *)kern + o); o = e; } else o++; }
        wn += (size_t)snprintf(wl + wn, sizeof wl - wn, "\n"); }
      LOG("%s", wl); }
    if (g_xref[0] && g_xref[2]) { /* 0.4.127: the code between/around the two references, straight from the kernel image (the run no longer stops, so no stop-report dump) */
      uint64_t hi0 = (g_xref[0] > g_xref[2] ? g_xref[0] : g_xref[2]) + 0x1c0, lo = 0xfffffff00863fc00ull, hi = 0xfffffff00863fc00ull;   /* 0.4.135: dump done; was hi=..fe80, 0.4.134: spin loop 0x863fe1c..0x863fe30 (headless [cpu] state after the root mount), caller lr 0x863fc90 */ (void)hi0;   /* 0.4.129: the part after the 0.4.127 window (0x80e9fac..) */
      static const uint64_t rg[6][2] = { { 0xfffffff0082f6000ull, 0xfffffff0082f6200ull },{ 0xfffffff0082eca00ull, 0xfffffff0082eca30ull },{ 0,0 },{ 0,0 },{ 0,0 },{ 0,0 } };  /* 0.4.617 */  /* 0.4.615 */  /* 0.4.610 */  /* 0.4.608 */  /* 0.4.606 */  /* 0.4.605 */  /* 0.4.601 */  /* 0.4.599 */  /* 0.4.597 */  /* 0.4.594 */  /* 0.4.592 */  /* 0.4.590 */  /* 0.4.589 */  /* 0.4.587 */  /* 0.4.586 */  /* 0.4.585 */  /* 0.4.581 */  /* 0.4.578 */  /* 0.4.575 */  /* 0.4.574 */  /* 0.4.570: code around the OFF 0x270 loaders in RTBuddy/PM regions */   /* 0.4.491: caller of the resource wait (return 0x82e94ec, RTBuddy::_createReporters region) */   /* 0.4.481: code around the kernel place that builds 0x0badffff (movk at 0x7c2945c) */   /* 0.4.473: PMP ASC start code (CPU_CONTROL write 0x82e0708, mailbox 0x10c write 0x82e0580) */   /* 0.4.458: ApplePMGR::start-like function around 0x816bdc8 (setter call) .. 0x816be24 */   /* 0.4.442: waiter 806cb60 and its PMGR callers */   /* 0.4.439: AppleA7IOP::start (entry unknown, return address 0x82db9e8 inside) */   /* 0.4.438: AppleANS2NVMeController::start 0x8cbf6b4-0x8cbfc88 in full */   /* 0.4.427: stack of AppleA7IOP::start thread: 80bc488, 80639a0, 8063c44, 7a5a3e0 */   /* 0.4.172: the lck_mtx code where the newest never-before-executed instructions are (0.4.171 [new]) */ (void)lo; (void)hi;
      for (int ri = 0; ri < 6; ri++) for (uint64_t a = rg[ri][0]; a < rg[ri][1]; a += 0x80) { char dl[1200]; size_t dn = (size_t)snprintf(dl, sizeof dl, "[stop] code 0x%llx (kernel image dump 0.4.127):", (unsigned long long)a); int okd = 0;
        for (int q = 0; q < mo.nseg && !okd; q++) { const A64Seg *g = &mo.seg[q]; if (a >= g->vmaddr && a + 0x80 <= g->vmaddr + g->filesize && mo.slice_off + g->fileoff + (a - g->vmaddr) + 0x80 <= klen) { okd = 1;
            for (int w = 0; w < 32; w++) { uint32_t x; memcpy(&x, kern + mo.slice_off + g->fileoff + (a - g->vmaddr) + 4 * (uint64_t)w, 4); dn += (size_t)snprintf(dl + dn, sizeof dl - dn, " %08x", x); } } }
        if (okd) LOG("%s\n", dl); } }
    if (g_xref[0] && g_xref[2]) { /* 0.4.128: strings referenced (ADRP+ADD) by that window, in address order: which DT properties / boot-args does the root selection read? */
      static const uint64_t sw[4] = { 0xfffffff008ca0000ull, 0xfffffff008d24000ull, 0xfffffff008d25000ull, 0xfffffff008d26000ull }; uint64_t lo = sw[0], hi = sw[0] + 0x1000; char sl[12000]; size_t sn = 0; int nsr = 0;
      sn += (size_t)snprintf(sl, sizeof sl, "[boot] diagnostic (changes nothing): strings referenced (0.4.154) by 0x%llx / 0x%llx / 0x%llx / 0x%llx (+0x200 each, @=low address bits of the referencing code):", (unsigned long long)sw[0], (unsigned long long)sw[1], (unsigned long long)sw[2], (unsigned long long)sw[3]); (void)lo; (void)hi;
      for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (!(g->initprot & 4) || mo.slice_off + g->fileoff + g->filesize > klen) continue;
        for (uint64_t o = 0; o + 8 <= g->filesize; o += 4) { uint64_t pc = g->vmaddr + o; { int inw = 0; for (int wq = 0; wq < 4; wq++) if (pc >= sw[wq] && pc < sw[wq] + 0x1000) inw = 1; if (!inw) continue; }
            uint32_t w1, w2; memcpy(&w1, kern + mo.slice_off + g->fileoff + o, 4); memcpy(&w2, kern + mo.slice_off + g->fileoff + o + 4, 4);
            if ((w1 & 0x9f000000u) != 0x90000000u || (w2 & 0xffc00000u) != 0x91000000u || (w1 & 31) != ((w2 >> 5) & 31)) continue;
            int64_t imm = (int64_t)((((w1 >> 5) & 0x7ffffu) << 2) | ((w1 >> 29) & 3)); if (imm & 0x100000) imm -= 0x200000;
            uint64_t tgt = ((pc & ~0xfffull) + (uint64_t)(imm << 12)) + ((w2 >> 10) & 0xfff);
            for (int r = 0; r < mo.nseg; r++) { const A64Seg *h = &mo.seg[r]; if (tgt >= h->vmaddr && tgt < h->vmaddr + h->filesize && mo.slice_off + h->fileoff + (tgt - h->vmaddr) < klen) {
                const uint8_t *sp = kern + mo.slice_off + h->fileoff + (tgt - h->vmaddr); size_t L = 0; while (L < 70 && tgt - h->vmaddr + L < h->filesize && sp[L] >= 32 && sp[L] < 127) L++;
                if (L >= 3 && sp[L] == 0 && sn + L + 30 < sizeof sl) { sn += (size_t)snprintf(sl + sn, sizeof sl - sn, " @%llx=[%.*s]", (unsigned long long)(pc & 0xfffff), (int)L, sp); nsr++; } break; } } } }
      LOG("%s (%d)\n", sl, nsr); }
    { /* 0.4.170: does the kernelcache carry a symbol table (LC_SYMTAB)? If yes, keep it so that addresses in the headless log can be named */
      const uint8_t *hd = kern + mo.slice_off; uint32_t nc = mo.ncmds, symoff = 0, nsyms = 0, stroff = 0, strsize = 0; size_t po = 32;
      for (uint32_t ci = 0; ci < nc && mo.slice_off + po + 8 <= klen; ci++) { uint32_t cmd, cs; memcpy(&cmd, hd + po, 4); memcpy(&cs, hd + po + 4, 4); if (cs < 8) break; if (cmd == 2 && mo.slice_off + po + 24 <= klen) { memcpy(&symoff, hd + po + 8, 4); memcpy(&nsyms, hd + po + 12, 4); memcpy(&stroff, hd + po + 16, 4); memcpy(&strsize, hd + po + 20, 4); } po += cs; }
      uint64_t nok = 0; if (nsyms && nsyms < 4000000u && (uint64_t)symoff + (uint64_t)nsyms * 16 <= klen - mo.slice_off && (uint64_t)stroff + strsize <= klen - mo.slice_off && strsize) {
        uint64_t *tmp = malloc(sizeof(uint64_t) * 2 * nsyms); g_sym_str = malloc(strsize + 1);
        if (tmp && g_sym_str) { memcpy(g_sym_str, hd + stroff, strsize); g_sym_str[strsize] = 0;
          for (uint32_t k = 0; k < nsyms; k++) { const uint8_t *e = hd + symoff + (uint64_t)k * 16; uint32_t sx; uint64_t val; memcpy(&sx, e, 4); memcpy(&val, e + 8, 8); if ((e[4] & 0x0e) != 0x0e || !val || sx >= strsize || !g_sym_str[sx]) continue; tmp[nok * 2] = val; tmp[nok * 2 + 1] = sx; nok++; }
          qsort(tmp, nok, 16, sym_cmp); g_sym_a = malloc(8 * (nok + 1)); g_sym_n = malloc(4 * (nok + 1)); if (g_sym_a && g_sym_n) { for (uint64_t k = 0; k < nok; k++) { g_sym_a[k] = tmp[k * 2]; g_sym_n[k] = (uint32_t)tmp[k * 2 + 1]; } g_sym_cnt = nok; } }
        free(tmp); }
      LOG("[boot] diagnostic (changes nothing): kernelcache symbol table: LC_SYMTAB %s, %u entries, %llu usable named section symbols\n", symoff ? "present" : "absent", nsyms, (unsigned long long)g_sym_cnt); }
    uint64_t entry_v = mo.thread_pc, entry_p = kphys + (entry_v - mo.lowest_vm);
    { /* 0.4.454 diagnostic (changes nothing): personalities that mention ascwrap / RTBuddyService / ANS2 in the prelink plist */
      static const char *const pats[] = { "<key>ApplePMP</key>", "<string>ApplePMP<", "<key>ApplePMPControlFunction</key>", "<string>ApplePMPControlFunction<" };   /* 0.4.522 */
      for (int pi = 0; pi < 4; pi++) { size_t from = 0; int nf = 0; size_t pl = strlen(pats[pi]); while (nf < 10 && from + 64 < klen) { const uint8_t *q = (const uint8_t *)memmem(kern + from, klen - from - 64, pats[pi], pl); if (!q) break; size_t qo = (size_t)(q - kern); char cb[460]; size_t a0 = qo > 200 ? qo - 200 : 0, a1 = qo + 220 < klen ? qo + 220 : klen, nn = 0;
        for (size_t z = a0; z < a1 && nn < sizeof cb - 1; z++) cb[nn++] = (kern[z] >= 32 && kern[z] < 127) ? (char)kern[z] : '|'; cb[nn] = 0;
        LOG("[boot] diagnostic (changes nothing): plist pattern %d #%d: %s\n", pi, nf, cb); nf++; from = qo + pl; } } }
    { /* 0.4.447 MODELLED (user decision, re-installed; 0.4.414 search had an off-by-one): the IOService resource 'IONVRAM' is normally published once the ANS2 NVMe controller (which needs the ANS coprocessor: not emulated) brings up the NVRAM. The restore daemon in the ramdisk waits for it forever. Stand-in: right after AppleCredentialManager publishes its own resource (kernel code at 0xfffffff008ae88ec, wrapper 0xfffffff008ae8afc(key, uint32) -> OSNumber -> IOService::publishResource(const char *, OSObject *) at 0xfffffff008075860; addresses from the 0.4.412/413 disassembly of build 16A366, ASSUMPTION they apply to this kernelcache only) the emulator calls the SAME wrapper once more with key 'IONVRAM', value 1. Nothing behind the resource (no IODTNVRAM controller, no storage) exists. */
      static const char nk[] = "\0IONVRAM"; uint64_t nva = 0;
      for (size_t o = 0; o + sizeof nk < klen && !nva; o++) if (!memcmp(kern + o, nk, sizeof nk)) for (int q = 0; q < mo.nseg; q++) { const A64Seg *g = &mo.seg[q]; if (mo.slice_off + g->fileoff <= o + 1 && o + 1 < mo.slice_off + g->fileoff + g->filesize) { nva = g->vmaddr + (o + 1 - mo.slice_off - g->fileoff); break; } }
      if (nva) { A64 *cc = a64m_cpu(m); cc->ch_pc = 0xfffffff008ae88f0ull; cc->ch_target = 0xfffffff008ae8afcull; cc->ch_x1 = nva; cc->ch_x2 = 1;
        LOG("[boot] MODELLED (user decision, NOT real): at 0xfffffff008ae88f0 the emulator calls the kernel's publish-resource wrapper 0xfffffff008ae8afc once with key 'IONVRAM' (string at virtual 0x%llx) and value 1, so that the restore daemon stops waiting. The real provider (ANS2 NVMe + ANS coprocessor) is NOT emulated; no NVRAM storage exists behind it.\n", (unsigned long long)nva); }
      else LOG("[boot] MODELLED IONVRAM stand-in NOT installed: the string 'IONVRAM' was not found in the kernelcache\n"); }
    free(kern);
    if (entry_v < mo.lowest_vm || entry_v >= mo.highest_vm) { free(dt); a64m_abort(m, "step 7 - entry point lies outside the kernel image"); return m; }

    /* boot_args, laid out as in XNU's public pexpert/arm64/boot.h as far as I know it (unverified here) */
    uint64_t ba = A64M_IOS_RAM_BASE + 0x1000, dtp = A64M_IOS_RAM_BASE + 0x10000;
    uint8_t *b = a64m_ram_at(m, ba, 0x400);
    uint16_t rev = 2, ver = 2; memcpy(b, &rev, 2); memcpy(b + 2, &ver, 2);
    uint64_t v;
    v = mo.lowest_vm; memcpy(b + 8, &v, 8); v = kphys; memcpy(b + 16, &v, 8);
    uint64_t kend = (kphys + span + 0x3FFFull) & ~0x3FFFull;   /* 16K-aligned end of the kernel image (physical) */
    uint64_t top = kend;
    int dt_virt = mo.lowest_vm >= 0xffff000000000000ull;   /* XNU-style high-half kernel */
    if (dt && dt_virt) {
        /* MODELLED (0.3.37 trace of the kernel's region list): the kernel builds one region [end of its image, topOfKernelData) right behind the image,
           slides the DT pointer with that region's delta, and maps the memory above topOfKernelData (+0x20000 for its own first page tables) as the last
           region. So the DT must sit between the end of the kernel image and topOfKernelData, and deviceTreeP is a kernel VIRTUAL address. */
        dtp = kend; top = (kend + dtlen + 0x3FFFull) & ~0x3FFFull;
        if (rdk) { rdpa = top; top = (rdpa + rdlen + 0x3FFFull) & ~0x3FFFull; rd_on = 1; }   /* 0.4.122: the RAM disk lies between the DT and topOfKernelData, i.e. inside the kernel-data region iBoot reserves */
        if (top + 0x20000 > A64M_IOS_RAM_BASE + ram) { free(dt); a64m_abort(m, "step 7 - no room for the DeviceTree above the kernel"); return m; }
    }
    /* 0.4.28 (MODELLED): iBoot hands the kernel a linear framebuffer in boot_args.Video {baseAddr(40) display(48) rowBytes(56) width(64) height(72) depth(80)}.
       Here: 540x960 by default (0.4.51; was 270x480), the size is a setting since 0.4.85, 32 bpp BGRX, rowBytes 2160, at the very top of RAM, excluded from memSize so the kernel does not hand those pages out as free memory.
       Real iBoot gives the panel size (1125x2436) and has drawn the logo already; this one starts black, everything on it is drawn by the kernel itself. */
    const uint32_t fbw = (uint32_t)a64m_video_w(), fbh = (uint32_t)a64m_video_h(), fbrb = fbw * 4; uint64_t fbsz = ((uint64_t)fbrb * fbh + 0x3FFFull) & ~0x3FFFull;
    uint64_t fbpa = A64M_IOS_RAM_BASE + ram - fbsz;
    v = fbpa - kphys; memcpy(b + 24, &v, 8);   /* memSize counts from physBase (the kernel computes end = physBase + memSize; 0.3.37 trace); the framebuffer lies above it */
    { uint64_t vd[6] = { fbpa, 0, fbrb, fbw, fbh, 32 }; memcpy(b + 40, vd, sizeof vd); a64m_set_video(m, fbpa, fbw, fbh, fbrb);
      LOG("[boot] MODELLED framebuffer: %ux%u, 32 bpp BGRX, rowBytes %u at physical 0x%llx, handed over in boot_args.Video; starts black, only the kernel draws on it (verbose boot '-v' is set, so kernel text should appear once its console is up)\n", fbw, fbh, fbrb, (unsigned long long)fbpa); }
    memcpy(b + 32, &top, 8);
    if (dt) { uint32_t pnb = 0, pom = 0, pnm = 0, pmiss = 0; int pr = a64_dt_pmgr_optional_bridges(dt, dtlen, &pnb, &pom, &pnm, &pmiss);   /* 0.4.64: must run before the tree is copied into RAM */
        if (pr == 1) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): 'pmgr' has #bridges=%u and optional-bridge-mask=0x%x but no 'bridge-settings-<i>' for %u of the bridges; the kernel (ApplePMGR.cpp:843) requires one of the two for every bridge, so those bridges were marked optional: mask 0x%x -> 0x%x. ASSUMPTION: the real settings (which iBoot or the hardware would supply) are unknown here; nothing is invented except the optional flag\n", pnb, pom, pmiss, pom, pnm);
        else if (pr == 0) LOG("[boot] DeviceTree 'pmgr': every one of the %u bridges already has bridge-settings or an optional bit (mask 0x%x), nothing changed\n", pnb, pom);
        else LOG("[boot] WARNING: 'pmgr' node with #bridges / optional-bridge-mask not found or the tree is malformed (%d), the ApplePMGR bridge check is not prepared\n", pr); }
    if (rd_on) { /* 0.4.367: the 0.4.366 experiment (RAMDisk size rounded up to a page) changed nothing (identical step counts) and was removed. */
    int pr2 = a64_dt_patch_u64x2(dt, dtlen, "RAMDisk", rdpa, (uint64_t)rdlen);
        if (pr2 == 1) { memcpy(a64m_ram_at(m, rdpa, rdlen), rdk, rdlen); LOG("[boot] MODELLED (iBoot does this): RAM disk placed at physical 0x%llx (%zu bytes), /chosen/memory-map RAMDisk = {0x%llx, 0x%zx}, boot-arg rd=md0\n", (unsigned long long)rdpa, rdlen, (unsigned long long)rdpa, rdlen); }
        else { rd_on = 0; LOG("[boot] RAM disk NOT handed over: the 'RAMDisk' property could not be patched (%d)\n", pr2); } }
    free(rdk); rdk = NULL;
    if (dt) { memcpy(a64m_ram_at(m, dtp, dtlen), dt, dtlen); v = dt_virt ? mo.lowest_vm + (dtp - kphys) : dtp; memcpy(b + 96, &v, 8); uint32_t l = (uint32_t)dtlen; memcpy(b + 104, &l, 4); }
    if (dt && dt_virt) {
        uint64_t upa[3], usz[3]; char ul[1000]; int nu = a64_dt_uarts(dt, dtlen, upa, usz, ul, sizeof ul);
        (void)ul; (void)nu;   /* 0.4.25: UART node dump dropped (log budget); the MODELLED device lines below still name the addresses */
        for (int k = 0; k < nu; k++) if (a64m_add_s5l_uart(sys, upa[k], usz[k]) == 0)
            LOG("[boot] MODELLED device: Apple S5L UART at physical 0x%llx (size 0x%llx), address from the DeviceTree; transmitted bytes go to the Kernel log, nothing is ever received\n", (unsigned long long)upa[k], (unsigned long long)usz[k]);
        { /* 0.4.53: log 0.4.52: the kernel maps a device page at physical 0x23b100000 and reads its first register (ldr w0,[base,w1,uxtw] with w1 = 0); nothing is there, so the access is a synchronous external abort and the kernel panics. Ask the DeviceTree which node owns that address and model it as an unknown block. */
          char nn[40], cc[100]; uint64_t nb = 0, ns = 0; int fr = a64_dt_find_pa(dt, dtlen, 0x23b100000ull, nn, sizeof nn, cc, sizeof cc, &nb, &ns);
          { static A64SocNode all[300]; int na = a64_dt_soc_nodes(dt, dtlen, all, 300); if (na > 0) { a64m_set_soc_nodes(m, all, na); LOG("[boot] DeviceTree: %d /arm-io nodes with a reg range are known to the stop report (it names the node that owns the first access to unmapped hardware)\n", na); } }
          int added = fr == 1 && ns && ns <= 0x1000000ull && a64m_add_unknown_mmio_named(m, nb, ns, nn) == 0;
          if (added) {
              /* 0.4.56: log 0.4.55: AppleInterruptController::start panics "device tree ipid-mask property length is 64 but should be 0" (AppleInterruptController.cpp:128). The kernel read aic +0x0 and +0x4 (twice), got 0, and derived the expected mask length 0 from it. The tree's ipid-mask is 64 bytes = 512 interrupt bits. */
              char al[900]; int64_t ipl = -1; int ar = a64_dt_node_info(dt, dtlen, "aic", "ipid-mask", &ipl, al, sizeof al);
              if (ar == 1 && ipl > 0 && ipl <= 0x1000 && a64m_unknown_set_read(m, nb, 0x4, (uint64_t)ipl * 8) == 0)
                  LOG("[boot] MODELLED: 'aic' register +0x4 reads %lld (= ipid-mask length %lld bytes x 8 bits, from the DeviceTree). ASSUMPTION: +0x4 holds the interrupt count in its low 16 bits (the layout of the Apple interrupt controller known from public Linux driver sources); the kernel's own check compares this with the same DT property, so it is consistent by construction, NOT verified against hardware\n", (long long)ipl * 8, (long long)ipl);
          }
          if (added)
              LOG("[boot] DeviceTree node '%s' (compatible '%s') owns physical 0x%llx (size 0x%llx), the page the kernel read when it hit the external abort. MODELLED device: unknown SoC block '%s': reads return 0, writes are ignored, the first accesses are logged. ASSUMPTION: real registers (and any interrupt behaviour) of this block are UNKNOWN here\n", nn, cc, (unsigned long long)nb, (unsigned long long)ns, nn);
          else LOG("[boot] DeviceTree lookup for physical 0x23b100000: %s\n", fr == 1 ? "found, but the size is unusable or the device could not be added" : fr == 0 ? "no /arm-io node covers it, nothing modelled" : "tree malformed"); }
        { /* 0.4.195 diagnostic (changes nothing): which /arm-io nodes describe the Secure Enclave (the kernel's keystore waits for the SEP) - name, compatible, physical range - and all properties of the node called 'sep' */
          static A64SocNode all2[300]; int na2 = a64_dt_soc_nodes(dt, dtlen, all2, 300); int shown = 0;
          for (int z = 0; z < na2 && shown < 16; z++) if (strstr(all2[z].name, "sep") || strstr(all2[z].name, "mailbox") || strstr(all2[z].compat, "sep") || strstr(all2[z].compat, "mailbox")) {
              LOG("[boot] diagnostic (0.4.195): SEP-related DeviceTree node '%s' compatible '%s' physical 0x%llx size 0x%llx\n", all2[z].name, all2[z].compat, (unsigned long long)all2[z].pa, (unsigned long long)all2[z].size); shown++; }
          if (!shown) LOG("[boot] diagnostic (0.4.195): no /arm-io node with 'sep' or 'mailbox' in name or compatible\n");
          char sl[2400]; int64_t spl = -1; int sr = a64_dt_node_info(dt, dtlen, "sep", "nonexistent-prop", &spl, sl, sizeof sl);
          LOG("[boot] diagnostic (0.4.195): node 'sep' lookup %d: %s\n", sr, sr >= 0 ? sl : "");
          { static const char *const an[3] = { "ans", "sart-ans", "aic" }; (void)an; for (int z = 0; z < 2; z++) { int64_t il2 = -1; char ib[900]; ib[0] = 0; int ir = a64_dt_node_info(dt, dtlen, an[z], "interrupts", &il2, ib, sizeof ib); LOG("[boot] diagnostic (changes nothing, 0.4.417): DT node '%s' property 'interrupts' (%d, len %lld): %s\n", an[z], ir, (long long)il2, ib); } }
          { char il[5000]; size_t in = (size_t)snprintf(il, sizeof il, "[boot] diagnostic (changes nothing, 0.4.416): DeviceTree nodes with compatible 'iop'/'ascwrap'/'sart'/'ans'/'nvme' or name 'ans*'/'sart*'/'nvme*' (name compat @physical+size):");
            for (int z = 0; z < na2 && in < sizeof il - 200; z++) if (strstr(all2[z].compat, "iop") || strstr(all2[z].compat, "ascwrap") || strstr(all2[z].compat, "sart") || strstr(all2[z].compat, "ans") || strstr(all2[z].compat, "nvme") || !strncmp(all2[z].name, "ans", 3) || !strncmp(all2[z].name, "sart", 4) || !strncmp(all2[z].name, "nvme", 4)) in += (size_t)snprintf(il + in, sizeof il - in, " | %s %s @0x%llx+0x%llx", all2[z].name, all2[z].compat, (unsigned long long)all2[z].pa, (unsigned long long)all2[z].size);
            LOG("%s\n", il); }
          for (int z = 0; z < na2; z++) if (!strcmp(all2[z].name, "pmp")) LOG("[boot] diagnostic (0.4.208): DT node 'pmp' compatible '%s' physical 0x%llx size 0x%llx\n", all2[z].compat, (unsigned long long)all2[z].pa, (unsigned long long)all2[z].size);
          { static const char *nn[2] = {"pmp", "pmgr"}; for (int z = 0; z < 2; z++) { static char pl[6000]; int64_t ppl = -1; int pr = a64_dt_node_info(dt, dtlen, nn[z], "nonexistent-prop", &ppl, pl, sizeof pl);
              LOG("[boot] diagnostic (0.4.206): node '%s' lookup %d: %s\n", nn[z], pr, pr >= 0 ? pl : ""); } }
        }
        { /* 0.4.220 diagnostic (changes nothing): AppleT802xPCIe::configure asserts _phyTunables != nullptr (AppleT802xPCIe.cpp:135) - which PCIe nodes exist and which properties do they carry */
          static A64SocNode pc[300]; int npc = a64_dt_soc_nodes(dt, dtlen, pc, 300), sh = 0;
          for (int z = 0; z < npc && sh < 8; z++) if (strstr(pc[z].name, "pcie") || strstr(pc[z].compat, "pcie")) { static char pl[6000]; int64_t ppl = -1; int pr = a64_dt_node_info(dt, dtlen, pc[z].name, "nonexistent-prop", &ppl, pl, sizeof pl);
              LOG("[boot] diagnostic (0.4.220): PCIe node '%s' compatible '%s' physical 0x%llx size 0x%llx; properties (lookup %d): %s\n", pc[z].name, pc[z].compat, (unsigned long long)pc[z].pa, (unsigned long long)pc[z].size, pr, pr >= 0 ? pl : ""); sh++; }
          if (!sh) LOG("[boot] diagnostic (0.4.220): no /arm-io node with 'pcie' in name or compatible\n"); }
        { /* 0.4.226 diagnostic (changes nothing): AppleSPMI asserts 'spmi timeout' (0.4.224 log, called from AppleDialogSPMIPMU); no block called spmi was ever touched - which DT nodes mention spmi, where are they, what do they carry */
          static A64SocNode sp[300]; int nsp = a64_dt_soc_nodes(dt, dtlen, sp, 300), sh = 0;
          for (int z = 0; z < nsp && sh < 8; z++) if (strstr(sp[z].name, "spmi") || strstr(sp[z].compat, "spmi") || strstr(sp[z].name, "dwi")) { static char pl[3000]; int64_t ppl = -1; int pr = a64_dt_node_info(dt, dtlen, sp[z].name, "nonexistent-prop", &ppl, pl, sizeof pl);
              LOG("[boot] diagnostic (0.4.226): node '%s' compatible '%s' physical 0x%llx size 0x%llx; properties (lookup %d): %s\n", sp[z].name, sp[z].compat, (unsigned long long)sp[z].pa, (unsigned long long)sp[z].size, pr, pr >= 0 ? pl : ""); sh++; }
          if (!sh) LOG("[boot] diagnostic (0.4.226): no /arm-io node with 'spmi' or 'dwi' in name or compatible\n"); }
        { /* 0.4.233 diagnostic: interrupt numbers of the spmi node, the aic node's own properties (AIC register-layout hints) */
          { char wf[300], wp[100], wq[100]; int f1 = a64_dt_node_prop_words(dt, dtlen, "pmgr", "function-pmp_control", wf, sizeof wf), f2 = a64_dt_node_prop_words(dt, dtlen, "pmp", "AAPL,phandle", wp, sizeof wp), f3 = a64_dt_node_prop_words(dt, dtlen, "pmgr", "AAPL,phandle", wq, sizeof wq); LOG("[boot] diagnostic (0.4.499): pmgr function-pmp_control words(%d): %s | pmp AAPL,phandle(%d): %s | pmgr AAPL,phandle(%d): %s\n", f1, wf, f2, wp, f3, wq); }
          { static const char *const pn[2] = { "pmp", "ans" }; for (int z = 0; z < 2; z++) { char wi[600], wv[100], wr[100]; int a1 = a64_dt_node_prop_words(dt, dtlen, pn[z], "interrupts", wi, sizeof wi), a2 = a64_dt_node_prop_words(dt, dtlen, pn[z], "iop-version", wv, sizeof wv), a3 = a64_dt_node_prop_words(dt, dtlen, pn[z], "role", wr, sizeof wr); LOG("[boot] diagnostic (0.4.477): node '%s' interrupts(%d): %s | iop-version(%d): %s | role(%d): %s\n", pn[z], a1, wi, a2, wv, a3, wr); char wc[300]; int a4 = a64_dt_node_prop_words(dt, dtlen, pn[z], "compatible", wc, sizeof wc); char wn[200]; int a5 = a64_dt_node_prop_words(dt, dtlen, pn[z], "device_type", wn, sizeof wn); LOG("[boot] diagnostic (0.4.494): node '%s' compatible words(%d): %s | device_type(%d): %s\n", pn[z], a4, wc, a5, wn); } }
          char w1[1200]; int r1 = a64_dt_node_prop_words(dt, dtlen, "spmi", "interrupts", w1, sizeof w1); LOG("[boot] diagnostic (0.4.233): node 'spmi' property 'interrupts' (%d): %s\n", r1, r1 >= 0 ? w1 : "");
          char w2[1200]; int r2 = a64_dt_node_prop_words(dt, dtlen, "aic", "reg", w2, sizeof w2); LOG("[boot] diagnostic (0.4.233): node 'aic' property 'reg' (%d): %s\n", r2, r2 >= 0 ? w2 : "");
          char w3[1200]; int64_t l3 = -1; int r3 = a64_dt_node_info(dt, dtlen, "aic", "nonexistent-prop", &l3, w3, sizeof w3); LOG("[boot] diagnostic (0.4.233): node 'aic' properties (%d): %s\n", r3, r3 >= 0 ? w3 : "");
          char w4[1200]; int r4 = a64_dt_node_prop_words(dt, dtlen, "aic", "compatible", w4, sizeof w4); LOG("[boot] diagnostic (0.4.233): node 'aic' compatible words (%d): %s\n", r4, r4 >= 0 ? w4 : "");
          char w5[1200]; int r5 = a64_dt_node_prop_words(dt, dtlen, "aic", "ipid-mask", w5, sizeof w5); LOG("[boot] diagnostic (0.4.233): node 'aic' ipid-mask (%d): %s\n", r5, r5 >= 0 ? w5 : ""); }
        { /* 0.4.250 diagnostic: pmgr properties that define the perf domains (domain id 8 = ANE has 2 states with frequency 0) */
          static const char *pn[] = { "perf-domains", "ifane-max", "voltage-states8", "voltage-states9", "voltage-states2", "perf-regs", "fane" };
          for (unsigned z = 0; z < sizeof pn / sizeof pn[0]; z++) { static char w6[3000]; int r6 = a64_dt_node_prop_words(dt, dtlen, "pmgr", pn[z], w6, sizeof w6); LOG("[boot] diagnostic (0.4.250): pmgr '%s' (%d): %s\n", pn[z], r6, r6 >= 0 ? w6 : ""); } }
        { /* 0.4.254 diagnostic: the guest read 0x23bc0810c (external abort: no RAM, no device) - which DeviceTree nodes live near it? */
          static A64SocNode nb[400]; int nnb = a64_dt_soc_nodes(dt, dtlen, nb, 400), shn = 0;
          for (int z = 0; z < nnb && shn < 12; z++) if (nb[z].pa >= 0x23bc00000ull - 0x100000 && nb[z].pa < 0x23bc00000ull + 0x200000) { LOG("[boot] diagnostic (0.4.254): node '%s' compatible '%s' physical 0x%llx size 0x%llx\n", nb[z].name, nb[z].compat, (unsigned long long)nb[z].pa, (unsigned long long)nb[z].size); shn++; }
          if (!shn) LOG("[boot] diagnostic (0.4.254): no /arm-io node within 0x23bb00000..0x23be00000\n"); }
          { static char wi[3000]; int ri = a64_dt_node_prop_words(dt, dtlen, "isp", "reg", wi, sizeof wi); LOG("[boot] diagnostic (0.4.262): node 'isp' reg words (%d): %s\n", ri, ri >= 0 ? wi : ""); }
          { static char wv[600]; int rv = a64_dt_node_prop_words(dt, dtlen, "avd", "avd-version", wv, sizeof wv); LOG("[boot] diagnostic (0.4.272): node 'avd' property 'avd-version' words (%d): %s\n", rv, rv >= 0 ? wv : ""); }
          { static A64SocNode nbi[400]; int nni = a64_dt_soc_nodes(dt, dtlen, nbi, 400), ni = 0; for (int z = 0; z < nni && ni < 10; z++) if (strstr(nbi[z].name, "isp") || strstr(nbi[z].compat, "isp") || strstr(nbi[z].compat, "cam")) { LOG("[boot] diagnostic (0.4.261): node '%s' compatible '%s' physical 0x%llx size 0x%llx (ISP/camera candidates: AppleH10CamIn::start reads +0x1800000 or +0x3000000)\n", nbi[z].name, nbi[z].compat, (unsigned long long)nbi[z].pa, (unsigned long long)nbi[z].size); ni++; } if (!ni) LOG("[boot] diagnostic (0.4.261): no /arm-io node with isp/cam in name or compatible\n"); }
        { /* 0.4.257 diagnostic: the guest read 0x269104114 (external abort) - DeviceTree nodes within 0x1000000 of it */
          static A64SocNode nc[400]; int nnc = a64_dt_soc_nodes(dt, dtlen, nc, 400), shc = 0;
          for (int z = 0; z < nnc && shc < 12; z++) if (nc[z].pa + nc[z].size > 0x269104114ull - 0x400000 && nc[z].pa < 0x269104114ull + 0x400000) { LOG("[boot] diagnostic (0.4.257): node '%s' compatible '%s' physical 0x%llx size 0x%llx\n", nc[z].name, nc[z].compat, (unsigned long long)nc[z].pa, (unsigned long long)nc[z].size); shc++; }
          if (!shc) LOG("[boot] diagnostic (0.4.257): no /arm-io node within 4 MiB of 0x269104114\n"); }
        { /* 0.4.57: log 0.4.56: after aic start the kernel reads physical 0x23d1f0000 (external abort, `ldr w0,[x8,w1,uxtw]` register read at offset 0). Same method as for aic: ask the DeviceTree who owns it, model that node as an unknown block (reads 0). */
          static const uint64_t seen[2] = { 0x23d1f0000ull, 0x24ac04000ull };   /* 0.4.60: + dapf-aop (log 0.4.59: REQUIRE failed in AppleDAPF.cpp:158 for it) so its first 40 accesses are logged */
          for (int z = 0; z < 2; z++) { char nn[40], cc[100]; uint64_t nb = 0, ns = 0; int fr = a64_dt_find_pa(dt, dtlen, seen[z], nn, sizeof nn, cc, sizeof cc, &nb, &ns);
            if (fr == 1 && ns && ns <= 0x1000000ull && a64m_add_unknown_mmio_named(m, nb, ns, nn) == 0) {
                if (!strncmp(cc, "dapf", 4)) a64m_unknown_set_readback(m, nb);
                LOG("[boot] DeviceTree node '%s' (compatible '%s') owns physical 0x%llx (size 0x%llx), read by the kernel when it hit an external abort. MODELLED device: unknown SoC block '%s': reads return 0, writes are ignored, the first accesses are logged. ASSUMPTION: its real registers are UNKNOWN here\n", nn, cc, (unsigned long long)nb, (unsigned long long)ns, nn); }
            else LOG("[boot] DeviceTree lookup for physical 0x%llx: %s\n", (unsigned long long)seen[z], fr == 1 ? "found, but the size is unusable or the device could not be added" : fr == 0 ? "no /arm-io node covers it, nothing modelled" : "tree malformed"); } }
        if (a64m_add_unknown_mmio(m, 0x200000000ull, 0x100000) == 0) LOG("[boot] MODELLED device: unknown SoC register block at physical 0x200000000 (1 MiB). The kernel reads +0x680/+0x684 of it at several 0x40000-spaced offsets and compares them (\"Inconsistent memory configuration\" panic if different), so it is probably a set of memory-controller channels. Real contents UNKNOWN: reads return 0 here (so all channels agree), writes are ignored, the first accesses are logged\n");
        else LOG("[boot] WARNING: the unknown SoC block at physical 0x200000000 could NOT be added (device table full or overlapping range); the kernel's memory-configuration reads will hit an external abort\n");
        LOG("[boot] MODELLED (ASSUMPTION 0.4.219, PLACEHOLDER): I2C controllers (DeviceTree names containing 'i2c'): status register +0x14 reads with bit 24 set (idle bus, SCL level high); event bits are write-1-to-clear. Reason: the crash is an assert in AppleS5L8940XI2CController::_checkBusStatus (\"SCL is stuck low\") when bit 24 reads 0. Derived from the driver code, not from documentation.\n");
        LOG("[boot] MODELLED (ASSUMPTION 0.4.224, PLACEHOLDER): DART blocks (DeviceTree names starting with 'dart'): register +0x0 reads 0x0e000000 (bits 27:24 = page-size shift 14 = 16 KiB). Reason: AppleT8020DART.cpp:1918 panicked 'page size is incorrectly configured: config 0x4000 actual 0x1' for dart-ane; the check compares that 4-bit field with the configured page shift. Other bits unknown (0).\n");
        /* 0.4.213 (MODELLED, ASSUMPTION): AppleT8020MemCacheController::start maps the AMCC non-plane block at 0x200100000 and the DCS block at 0x200200000 (its own log lines); AppleT8020PlatformErrorHandler::start then WRITES to 0x200158000, which has no device: that is the first exception of the run (ESR 0x96000050, external abort) and the cause of the kernel panic. The block 0x200000000 (1 MiB) ends at 0x200100000, so the AMCC region was never covered. Zero-reading registers; the real register contents are unknown. */
        if (a64m_add_unknown_mmio_named(m, 0x200100000ull, 0x100000, "amcc") == 0) LOG("[boot] MODELLED (ASSUMPTION 0.4.213, PLACEHOLDER contents): memory-controller AMCC/DCS registers: unknown SoC block 'amcc' at physical 0x200100000 (1 MiB), reads 0 until the kernel programs them. Reason: the kernel (AppleT8020PlatformErrorHandler::start) writes 0x200158000 and without a device that is an external abort -> panic.\n");
        else LOG("[boot] WARNING: the 'amcc' block at physical 0x200100000 could NOT be added (overlap or table full)\n");
        LOG("[boot] MODELLED (ASSUMPTION 0.4.234, PLACEHOLDER): minimal interrupt controller on the 'aic' block: EVENT +0x2004 / MASK_SET +0x4100 / MASK_CLR +0x4180 (layout of the PUBLIC Asahi Linux driver for the first-generation Apple AIC, NOT verified on T8020), all masked at start, no priorities/targets/IPIs; its output drives the CPU IRQ input. SPMI: every command written to +0x704 raises interrupt 192 (AppleSPMI waits for it: object +0x168=0xc0, first 'interrupts' entry of the spmi node) and sets bit 0 of status word +0x778 (write-1-to-clear). Reason: without it every SPMI command times out and AppleSPMI asserts after 10.\n");
        { /* 0.4.255 MODELLED (PLACEHOLDER): DT node 'pmp' (iop,ascwrap-v2) is 0x23bc00000 size 0x60000 but overlaps 'disp0' (0x23bc04840) and 'pmgr' (0x23bc1c000), so the automatic block creation skipped it; the guest then took an external abort reading 0x23bc0810c (0.4.253 log). Add ONE named block 'pmp' on the page 0x23bc08000 (0x4000): reads 0, writes kept, first accesses logged. No IOP/mailbox protocol is modelled. */
          int rp = a64m_add_unknown_mmio_named(m, 0x23bc08000ull, 0x4000, "pmp"); int rq = a64m_add_unknown_mmio_named(m, 0x23bc00000ull, 0x4000, "pmp0");   /* 0.4.256: the next abort was a read at 0x23bc00044 (ASC CPU control area) */
          LOG("[boot] MODELLED (0.4.256, PLACEHOLDER): unknown SoC block 'pmp0' over 0x23bc00000 (0x4000): %s; reads 0, first accesses logged\n", rq == 0 ? "added" : "NOT added (overlap)");
          LOG("[boot] MODELLED (0.4.255, PLACEHOLDER): unknown SoC block 'pmp' (ASC wrapper of the power-management processor) over 0x23bc08000 (0x4000): %s; reads 0, first accesses logged; NO mailbox/RTKit protocol\n", rp == 0 ? "added" : "NOT added (overlap)");
          { int ra = a64m_asc_add(m, 0x23bc00000ull, 0x12f, "PMP"); LOG("[boot] MODELLED (ASSUMPTION 0.4.478, PLACEHOLDER; NOT the PMP firmware, it is NOT executed): scripted IOP side of the PMP ASC mailbox (%d): RUN -> RTKit ep0 HELLO (max version 10, the value the kernel code accepts), HELLO_ACK -> EPMAP {ep0}; irq 0x12f; messages logged as [asc-PMP]\n", ra); } }
        { /* 0.4.532 MODELLED (ASSUMPTION, PLACEHOLDER): the same scripted IOP side for the ANS coprocessor ('ans', iop,ascwrap-v2). ASC window = first DT reg 0x277400000+0xc000 (the pattern of aop/sio/smc/pmp: first reg is the ASC), mailbox at +0x8000, irq = the 3rd listed interrupt 0x16f (the kernel unmasks only that one, as for pmp). EPMAP: ep0..4 + ep6 (the NVMe driver waits for the nub 'ANS2Endpoint1' = endpoint 6 by the kernel's name builder 0x82edd1c). The ANS firmware is NOT executed, no NVMe/SART behaviour is modelled. */
          int r1 = a64m_add_unknown_mmio_named(m, 0x277400000ull, 0x4000, "ans0"), r2 = a64m_add_unknown_mmio_named(m, 0x277408000ull, 0x4000, "ans");
          int ra = a64m_asc_add(m, 0x277400000ull, 0x16f, "ANS2"); if (ra >= 0) a64m_asc_set_epmap(0x277400000ull, 0x5full);
          LOG("[boot] MODELLED (ASSUMPTION 0.4.532, PLACEHOLDER; the ANS firmware is NOT executed): scripted ASC mailbox for ANS at 0x277400000 (blocks added: %d/%d, asc_add result %d), irq 0x16f, EPMAP ep0..4+ep6; messages logged as [asc-ANS2]\n", r1, r2, ra); }
        { /* 0.4.258 MODELLED (PLACEHOLDER): DT node 'avd' (avd,t8020, video decoder) is 0x268000000 size 0x2000000 but overlaps 'dart-avd' (0x269010000), so no block was made and the guest took an external abort reading 0x269104114 (0.4.256 log). Two named blocks cover the rest: 'avd' 0x268000000..0x269010000 and 'avd2' 0x269014000..0x26a000000; reads 0, writes kept, first accesses logged; the decoder itself is not modelled */
          int ra = a64m_add_unknown_mmio_named(m, 0x268000000ull, 0x1010000, "avd"), rb = a64m_add_unknown_mmio_named(m, 0x269014000ull, 0xfec000, "avd2");
          LOG("[boot] MODELLED (0.4.258, PLACEHOLDER): unknown SoC blocks 'avd' (0x268000000, 0x1010000): %s and 'avd2' (0x269014000, 0xfec000): %s; reads 0\n", ra == 0 ? "added" : "NOT added", rb == 0 ? "added" : "NOT added");
          if (rb == 0) { a64m_unknown_set_read(m, 0x269014000ull, 0xf0114, 0x20001ull); LOG("[boot] MODELLED (0.4.274, PLACEHOLDER): AVD version register at physical 0x269104114 reads 0x20001 (0x20000 was not enough: the kernel prints nAVD2Revision = (value & 1) and refuses revision 0). AppleAVD getAVDVersion (kernel code 0x8c0fa68..) loads [avd base 0x268000000 + 0x1104114] and requires (value & 0xffff0000) == 0x20000, else prints 'AVD2 Version is wrong' (0.4.266 log). The low 16 bits (revision) are UNKNOWN and left 0: the driver may still print 'Not supported AVD2 Revision'\n"); } }
        { /* 0.4.227 diagnostic/model: the two 'spmi' nodes (spmi,t8015) are at 0x23d180700 and 0x23d180d00 (0x100 each); no block with that name was ever touched, so the registers sit in some other block. Add ONE named block 'spmi' over the page 0x23d180000 (0x4000), reads 0 (as before), first accesses logged */
          int rs = a64m_add_unknown_mmio_named(m, 0x23d180000ull, 0x4000, "spmi");
          LOG("[boot] MODELLED (0.4.227, PLACEHOLDER): unknown SoC block 'spmi' over 0x23d180000 (0x4000) for the SPMI controllers at +0x700/+0xd00: %s; reads 0, first accesses logged\n", rs == 0 ? "added" : "NOT added (overlaps another block)"); }
        { /* 0.4.58: log 0.4.57: nub-gpio passed; the next stop is a write to node 'dapf-aop' (dapf,t8020), and the DeviceTree lists 263 such /arm-io nodes, so one round trip per node would take days. Every /arm-io node that is not modelled yet becomes an unknown zero block (reads 0, writes ignored); each block logs only its first accesses and the stop report lists every touched block with its read/write counts. A driver that polls a register for a status bit will now spin instead of aborting; the report shows which block it was. */
          static A64SocNode allb[300]; int nb2 = a64_dt_soc_nodes(dt, dtlen, allb, 300), addedn = 0, skipped = 0, ndapf = 0, npsm = 0, ntemp = 0;
          for (int z = 0; z < nb2; z++) { if (!allb[z].size || allb[z].size > 0x1000000ull) { skipped++; continue; } if ((!strcmp(allb[z].name, "pmgr") ? a64m_add_unknown_mmio_named(m, allb[z].pa, allb[z].size, allb[z].name) : a64m_add_unknown_mmio_auto(m, allb[z].pa, allb[z].size, allb[z].name)) == 0) {   /* 0.4.62: 'pmgr' logs its first 40 accesses */ addedn++; if (!strcmp(allb[z].name, "pmgr")) { a64m_unknown_set_psmirror(m, allb[z].pa); npsm++; } if (!strncmp(allb[z].compat, "dapf", 4)) { a64m_unknown_set_readback(m, allb[z].pa); ndapf++; } } else skipped++; }
          { /* 0.4.259 MODELLED (PLACEHOLDER): gap fill. Nodes that were skipped above (size 0, > 16 MiB, or overlapping another node: avd, pmp, ...) leave holes in which the guest takes an external abort (0.4.258 log: read at 0x22b800000). Every skipped node with a size is cut into 64 KiB pieces (1 MiB for nodes over 1 MiB, at most 0x40 pieces per node, 120 pieces in all); each piece becomes an unknown zero block named '<node>~' if it does not overlap an existing block (refused otherwise). Reads 0, writes ignored. */
            int gfill = 0, gnode = 0; for (int z = 0; z < nb2 && gfill < 120; z++) { if (!allb[z].size) continue; uint64_t cs = allb[z].size > 0x100000ull ? 0x100000ull : 0x10000ull, a0 = allb[z].pa & ~0x3fffull, e0 = allb[z].pa + allb[z].size; int pc = 0, got = 0; char gn[44]; snprintf(gn, sizeof gn, "%.30s~", allb[z].name);
              for (uint64_t a = a0; a < e0 && pc < 0x40 && gfill < 120; a += cs, pc++) { uint64_t sz = cs; if (a + sz > e0) sz = (e0 - a + 0x3fffull) & ~0x3fffull; if (a64m_add_unknown_mmio_named(m, a, sz, gn) == 0) { gfill++; got++; } }
              if (got) gnode++; }
            LOG("[boot] MODELLED (0.4.259, PLACEHOLDER): gap fill: %d zero-block piece(s) added from %d /arm-io node(s) that were skipped or only partly covered (pieces overlapping an existing block are refused)\n", gfill, gnode); }
          { /* 0.4.263/264 MODELLED (PLACEHOLDER): ISP version register. AppleH10CamIn::start (kernel code 0x8c51a00..) reads a word at <isp reg 0 base> + 0x1800000 when fISPVersion == 9 (+0x3000000 otherwise) and requires (word>>16) == fISPVersion, else it prints 'fISPVersion=-1' and its assert panics (0.4.259 log). 0.4.263 first guessed version 8 from a table dump and was WRONG: the 0.4.263 log shows the guest read 'isp~'@0x22b800000+0x0 = isp+0x1800000, i.e. the version-9 branch, so fISPVersion = 9. The register is inside the DT range (0x1e8d000) but what the real chip holds there is UNKNOWN: the value 9<<16 = 0x90000 is only what the kernel's check demands (other bits 0). */
            for (int z = 0; z < nb2; z++) if (!strcmp(allb[z].name, "isp")) { uint64_t ip = allb[z].pa + 0x1800000ull; int ra = a64m_unknown_set_read(m, ip, 0, 0x90000ull); if (ra) { ra = a64m_add_unknown_mmio_named(m, ip, 0x4000, "isp-version"); if (ra == 0) a64m_unknown_set_read(m, ip, 0, 0x90000ull); } LOG("[boot] MODELLED (0.4.264, PLACEHOLDER): ISP version register at physical 0x%llx reads 0x90000 (fISPVersion 9: the version-9 branch of AppleH10CamIn::start reads isp+0x1800000 and needs (reg>>16)==9; %s)\n", (unsigned long long)ip, ra == 0 ? "ok" : "block NOT available"); break; } }
          for (int z = 0; z < nb2; z++) if (!strcmp(allb[z].name, "aes") && allb[z].size >= 0x1000) { int ra = a64m_unknown_set_read(m, allb[z].pa, 0xc, 0x3f80ull);   /* 0.4.313 */
              LOG("[boot] MODELLED (0.4.313, ASSUMPTION, PLACEHOLDER): AES engine status register at physical 0x%llx+0xc reads 0x3f80 (result %d). Reason: AppleS8000AESAccelerator::_enableAES (log 0.4.311, panic 'DPA has not been seeded') polls that register until bits 0x3f80 (mode 2: 0x180) are set and asserts after a timeout; the guest seeds the DPA itself, the hardware is expected to flag it as done. The bits' meaning is INFERRED from the polling code only. This is NOT an AES implementation: no data is encrypted or decrypted and no key is used; any real AES request will still not work.\n", (unsigned long long)allb[z].pa, ra); break; }
          for (int z = 0; z < nb2; z++) if (strstr(allb[z].name, "tempsensor")) for (int q = 0; q < nb2; q++) { uint64_t bp = allb[q].pa;   /* 0.4.150: per node, register offsets from its own DT properties */
              if (strcmp(allb[q].name, "pmgr") || allb[z].pa < bp || allb[z].pa >= bp + allb[q].size) continue;
              uint64_t sb = allb[z].pa - bp; a64m_unknown_ts_window(m, bp, sb);
              static const struct { const char *p; int kind; } tp[] = { { "sensor-offset-CTL0-set", 1 }, { "sensor-offset-CTL0-clr", 1 }, { "sensor-offset-CTL1", 1 }, { "sensor-offset-CTL2", 1 }, { "sensor-offset-status", 1 }, { "sensor-offset-readBK", 2 }, { "sensor-offset-readBK1", 2 }, { "sensor-offset-readSum", 3 }, { "sensor-offset-readCnt", 4 } };
              for (size_t pi = 0; pi < sizeof tp / sizeof tp[0]; pi++) { char w[64]; unsigned off32 = 0; if (a64_dt_node_prop_words(dt, dtlen, allb[z].name, tp[pi].p, w, sizeof w) != 4 || sscanf(w, "%x", &off32) != 1) continue;
                  uint64_t v = tp[pi].kind == 1 ? 1 : tp[pi].kind == 2 ? (0x80000000ull | TEMPSENSOR_RAW) : tp[pi].kind == 3 ? 4 * TEMPSENSOR_RAW : 4;
                  a64m_unknown_ts_read(m, bp, sb + off32, v); }
              ntemp++; break; }
          if (ntemp) LOG("[boot] MODELLED (ASSUMPTION 0.4.150, PLACEHOLDER values): %d temperature sensor nodes (name contains 'tempsensor') lie inside 'pmgr' blocks; each answers like a running sensor at ITS OWN register offsets from the DT sensor-offset-* properties: CTL0-set/CTL0-clr/CTL1/CTL2/status read 1 (bit0 'on': the kernel's AppleT8015TempSensor timeout code prints 'sensor off' when bit0 of one of them is 0; which one is not known), readBK/readBK1 = bit31 'valid' | raw 0x%x, readSum = 4*raw, readCnt = 4. The real sensor (conversion, alarms, interrupt) is NOT modelled; raw is invented (1/64 degree units, ASSUMPTION from the kernel's conversion constant)\n", ntemp, (unsigned)TEMPSENSOR_RAW);
          LOG("[boot] MODELLED: %d more /arm-io DeviceTree nodes became unknown zero blocks (%d skipped: empty/huge range or overlapping one already modelled). ASSUMPTION: their real registers are UNKNOWN here; reads return 0, writes are ignored\n", addedn, skipped);
          LOG("[boot] MODELLED (ASSUMPTION 0.4.110): %d 'pmgr' blocks: a written register reads back with bits 7:4 = bits 3:0 (power state: requested -> current), unwritten read 0. Reason: ApplePMGR::waitReg32 (ApplePMGR.cpp:3241) waited for (reg & 0xf0) == 0xf0 at pmgr 0x23b000000+0x802d8 after requesting state 0xf (headless log 0.4.109). No real delay or failure of the hardware is modelled\n", npsm);
          LOG("[boot] MODELLED: 'dapf' blocks ('dapf-aop' and %d more) read back the last value written to a register (0 if none). Reason (the kernel's own code, AppleDAPF.cpp:158): it writes 0x80000001 to +0x0 and requires to read the same value back. ASSUMPTION: this is only a read-back, the real DAPF filter behaviour is NOT modelled\n", ndapf); }
    }
    { /* 0.4.102 (MODELLED): the kernel code 0xfffffff008ce4e84..0x5298 (read from the logs of the headless runs): for each of 2 clusters it reads the registers +0x40, +0x60 ... +0x1e0 (step 0x20; 14 of them) of the 'pmgr' blocks 0x210e70000 / 0x211e70000; a register EQUAL to the first one is an unused slot (skipped), any other is performance state (index - 2); a used register: bit 21 = PLL on, (reg>>4)&0x1ff = multiplier, (reg&0xf)+1 = divider, frequency = multiplier/divider x 24 MHz, and that frequency must be a row of pmgr/ifane-max */
      static const unsigned mult[4] = { 21, 31, 49, 71 };   /* x 24 MHz = 504 744 1176 1704 MHz: the first four rows of the IPSW's own ifane-max (504 744 1176 1704 2136 2376 2436 2448 2496); 4 = the entry count of the IPSW's own voltage-states2, the larger real table */
      static const uint64_t bases[2] = { 0x210e70000ull, 0x211e70000ull };
      int bad = 0;
      for (int bi = 0; bi < 2; bi++) for (unsigned k = 0; k < 14; k++)
          bad += a64m_unknown_set_read(m, bases[bi], 0x40 + 0x20 * k, (1ull << 21) | ((uint64_t)mult[k < 4 ? k : 0] * (bi ? 2u : 1u) << 4)) != 0;   /* slots 4..13 repeat the first word: unused; 0.4.103: block 1 (the P cluster, the only one checked against ifane-max): the kernel computed multiplier x 12 MHz (freqHz=252000000 for multiplier 21), so the multiplier is doubled there */
      LOG("[boot] MODELLED (EXPERIMENT 0.4.102, NOT verified): CPU performance-state registers: +0x40..+0x100 (step 0x20) of the pmgr blocks 0x210e70000 and 0x211e70000 read PLL words (bit 21 on, multiplier 21 31 49 71, divider 1) = 504 744 1176 1704 MHz at a 24 MHz oscillator, the first four rows of the IPSW's own pmgr/ifane-max; +0x120..+0x1e0 repeat the first word (the kernel's loop skips such slots as unused) (failures: %d). 0.4.103: for the P cluster (0x211e70000) the multipliers are doubled (42 62 98 142): headless run 0.4.102 showed the kernel computing multiplier x 12 MHz there (freqHz=252000000 for 21). ASSUMPTION: the layout is read from the kernel's code; the real chip's states are chip state UNKNOWN here; 4 states is a guess (the largest real voltage-states table of the tree has 4 entries)\n", bad); }
    { /* 0.4.109 (MODELLED, PLACEHOLDER values): code of the caller of MTRGetPolynom (0x8ce4140.., decoded from the 0.4.108 headless log) reads the 'pmgr' block 0x23d200000: 7 words +0xbc0a8..+0xbc0c0 (calibration data for the polynomial fit, which fails if any unpacked bit-field is 0) and +0xbc088/+0xbc08c/+0xbc090, from which two floats are built: v0 = bits[37:28] of {+0xbc08c,+0xbc088} x 0.125 and v1 = bits[31:24] of +0xbc08c plus bits[1:0] of +0xbc090 above them, x 0.125. They read 0, so v0 == v1 == 0 and the fit divided by zero (inf/NaN in the panic arguments). */
      static const uint32_t cal[7] = { 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au, 0x5a5a5a5au };
      int bad = 0;
      for (unsigned k = 0; k < 7; k++) bad += a64m_unknown_set_read(m, 0x23d200000ull, 0xbc0a8 + 4 * k, cal[k]) != 0;
      bad += a64m_unknown_set_read(m, 0x23d200000ull, 0xbc088, 0x80000000u) != 0;   /* v0 low 4 bits = 8 */
      bad += a64m_unknown_set_read(m, 0x23d200000ull, 0xbc08c, 0x4800000cu) != 0;   /* v0 high 6 bits = 0x0c -> 0xc8 = 200 -> 25.0; v1 low 8 bits = 0x48 */
      bad += a64m_unknown_set_read(m, 0x23d200000ull, 0xbc090, 0x3u) != 0;          /* v1 = 0x348 = 840 -> 105.0 */
      LOG("[boot] MODELLED (EXPERIMENT 0.4.109, PLACEHOLDER values, NOT real fuse data): pmgr block 0x23d200000 +0xbc0a8..+0xbc0c0 read 0x5a5a5a5a (7 words, calibration input of the MTR polynomial fit) and +0xbc088/+0xbc08c/+0xbc090 = 0x80000000 / 0x4800000c / 0x3, which the kernel turns into two floats 25.0 and 105.0 (v1 != v0, so no division by zero). Real values are per-chip fuses, UNKNOWN here%s\n", bad ? " (WARNING: some could not be set)" : ""); }
    { /* 0.4.112 (MODELLED, ASSUMPTION): decoded kernel code 0x8ce4604..0x8ce46dc (0.4.111 headless dump): to enable, it sets the low nibble of pmgr 0x23b000000+0x98000 to 0xf and then ApplePMGR::waitReg32 waits for (reg & 0x10) == 0, i.e. bit 4 is a busy/pending flag that clears. A fixed read value 0x0f (low nibble 0xf, bit 4 clear) satisfies that. */
      int bad = a64m_unknown_set_read(m, 0x23b000000ull, 0x98000, 0x0f) != 0; bad += a64m_unknown_set_read(m, 0x23b000000ull, 0x98004, 0x0f) != 0;   /* 0.4.113: the same function has a second branch for +0x98004 (the only two branches it has) */
      LOG("[boot] MODELLED (ASSUMPTION 0.4.112): pmgr 0x23b000000+0x98000 and +0x98004 read 0x0f: the kernel waits for bit 4 (busy) of this register to clear after setting its low nibble to 0xf. Real timing/behaviour UNKNOWN%s\n", bad ? " (WARNING: could not be set)" : ""); }
    { /* 0.4.116 (MODELLED, ASSUMPTION): headless log 0.4.114: the kernel wrote 0xC5ACCE55 (CoreSight lock access key) to physical 0x210010fb0, no RAM/device. The DT 'cpuN/reg-private' = {lo, hi} (cpu0 = 0x210010000, ... 6 cores) are the per-core debug blocks. The DT gives NO size: 0x10000 was the first ASSUMPTION, 0.4.118 widened it to 0x40000 (the next write went to base+0x20fb0). Reads 0, writes ignored. */
      int nc = 0, bad = 0; char pa[400] = "";
      for (int ci = 0; ci < 16; ci++) { char cn[16], w[64]; snprintf(cn, sizeof cn, "cpu%d", ci);
          int pl = a64_dt_node_prop_words(dt, dtlen, cn, "reg-private", w, sizeof w); unsigned lo = 0, hi = 0;
          if (pl != 8 || sscanf(w, "%x %x", &lo, &hi) != 2) continue;
          uint64_t base = ((uint64_t)hi << 32) | lo; char lb[24]; snprintf(lb, sizeof lb, "%s-private", cn);
          if (a64m_add_unknown_mmio_named(m, base, 0x40000, lb) == 0) { size_t l = strlen(pa); snprintf(pa + l, sizeof pa - l, " %s=0x%llx", cn, (unsigned long long)base); nc++; } else bad++; }
      LOG("[boot] MODELLED (ASSUMPTION 0.4.116): %d per-core debug blocks from the DT 'cpuN/reg-private' (%s) as unknown 0x40000-byte blocks (size NOT in the DT; 0.4.116 used 0x10000, the kernel then wrote base+0x20fb0; reads 0, writes ignored). Reason: the kernel wrote the CoreSight lock key 0xC5ACCE55 to 0x210010fb0 (headless log 0.4.115)%s\n", nc, pa, bad ? " (WARNING: some could not be added)" : ""); }
    { static char pb[1500]; static const uint32_t pw[] = { 0x3b, 0x3a, 0x37, 0x38, 0x39, 0x3c, 0x3d, 0x34 };   /* 0.4.445 */
      for (int k = 0; k < 8; k++) { size_t pp = 0, pl = 0; pb[0] = 0; dt_find_phandle(dt, dtlen, &pp, 0, "-", pw[k], pb, sizeof pb, &pl); LOG("[boot] diagnostic (changes nothing): DT nodes with AAPL,phandle=0x%x: %s\n", pw[k], pl ? pb : "(none found)"); } }
    { static char db[5000]; static const char *const tg[] = { "iop-ans-nub", "iop-pmp-nub" };   /* 0.4.533 */
      for (int ti = 0; ti < 2; ti++) { size_t dp = 0, dl = 0; db[0] = 0; dt_dump_node(dt, dtlen, &dp, 0, tg[ti], db, sizeof db, &dl); LOG("[boot] diagnostic (changes nothing): DeviceTree node '%s' properties (name(len)=first words): %s\n", tg[ti], db); } }
    { /* 0.4.135 (MODELLED, ASSUMPTION): headless [cpu] state 0.4.133 + decoded code 0.4.134: the kernel (_mccEnableCacheMode) spins forever on the memory controller (block 0x200000000): it writes the cache mode to +0x1c00 and polls +0x1c04 + 0x40000*k (k = 0..3) until bits 14:10 and 9:5 equal the mode. */
      int bad = 0; for (int ch = 0; ch < 4; ch++) bad += a64m_unknown_set_modelink(m, 0x200000000ull, 0x1c00, 0x1c04 + 0x40000ull * (uint64_t)ch) != 0;
      LOG("[boot] MODELLED (ASSUMPTION 0.4.135): memory controller 0x200000000: after the kernel writes a cache mode v to +0x1c00, +0x1c04 + 0x40000*k (4 channels) read (v<<10)|(v<<5), because _mccEnableCacheMode polls them for exactly that (headless run: a 6-instruction spin loop for 6 billion steps). Real behaviour UNKNOWN%s\n", bad ? " (WARNING: some could not be set)" : ""); }
    { /* 0.4.80 diagnostic (changes nothing): log 0.4.79 shows the watchdog panic again (clock speed 0). Which frequency properties does the IPSW tree really carry, and are they zero placeholders? */
      static const char *const pn[] = { "timebase-frequency", "clock-frequency", "bus-frequency", "fixed-frequency", "memory-frequency", "peripheral-frequency", "bus-frequency-min", "bus-frequency-max", "cpu-frequency", "usbphy-frequency", "soc-generation" };
      static const char *const nd[] = { "cpu0", "arm-io", "device-tree" };
      char ln[1800]; size_t lo = 0; ln[0] = 0;
      for (unsigned z = 0; z < sizeof nd / sizeof nd[0]; z++) for (unsigned y = 0; y < sizeof pn / sizeof pn[0]; y++) {
          char w[160]; int pl = a64_dt_node_prop_words(dt, dtlen, nd[z], pn[y], w, sizeof w);
          if (pl >= 0 && lo < sizeof ln - 200) lo += (size_t)snprintf(ln + lo, sizeof ln - lo, " %s/%s(%d)=%s", nd[z], pn[y], pl, pl ? w : "-");
      }
      { int64_t pl2 = -1; char nl[700]; int r2 = a64_dt_node_info(dt, dtlen, "cpu0", "timebase-frequency", &pl2, nl, sizeof nl); if (r2 >= 0 && lo < sizeof ln - 750) lo += (size_t)snprintf(ln + lo, sizeof ln - lo, " cpu0 properties: %s;", nl); else if (lo < sizeof ln - 100) lo += (size_t)snprintf(ln + lo, sizeof ln - lo, " cpu0 node lookup=%d;", r2); }
      { char w[160]; int pl = a64_dt_node_prop_words(dt, dtlen, "arm-io", "clock-frequencies", w, sizeof w); if (pl >= 0 && lo < sizeof ln - 200) lo += (size_t)snprintf(ln + lo, sizeof ln - lo, " arm-io/clock-frequencies(%d) first words=%s", pl, w); }
      LOG("[boot] diagnostic (changes nothing): frequency-like DeviceTree properties:%s\n", lo ? ln : " none found"); }
    { /* 0.4.83 diagnostic (changes nothing): log 0.4.82 passed the watchdog; ApplePMGR.cpp:1303 now panics "ECPU0 request ECPU.<n> > <m>" - what do the REAL voltage-states tables of the IPSW tree look like (length, first words)? */
      char ln2[1500]; size_t l2 = 0; ln2[0] = 0;
      for (int vi = 0; vi < 16; vi++) { char vn[24], w[120]; snprintf(vn, sizeof vn, "voltage-states%d", vi); int pl = a64_dt_node_prop_words(dt, dtlen, "pmgr", vn, w, sizeof w);
        if (pl > 8 && l2 < sizeof ln2 - 200) l2 += (size_t)snprintf(ln2 + l2, sizeof ln2 - l2, " %s(%d)=%s;", vn, pl, w); }
      LOG("[boot] diagnostic (changes nothing): pmgr voltage-states tables of the IPSW tree (length in bytes, first words; the added zero ones are not listed):%s\n", l2 ? ln2 : " none"); }
    { char w[1400]; int r = a64_dt_node_prop_words(dt, dtlen, "pmgr", "ifane-max", w, sizeof w);   /* 0.4.95 diagnostic, changes nothing */
      LOG("[boot] diagnostic (changes nothing): pmgr/ifane-max (%d): %s\n", r, r >= 0 ? w : "(absent)"); }
    { char w[1800]; int r = a64_dt_node_prop_words(dt, dtlen, "pmgr", "energy-counters", w, sizeof w);
      LOG("[boot] diagnostic (changes nothing): pmgr/energy-counters (%d): %s\n", r, r >= 0 ? w : "(absent)"); }
    { static char gb[6000]; DtGrep g = { gb, sizeof gb, 0, 0 }; size_t gp = 0; gb[0] = 0; dt_grep_walk(dt, dtlen, &gp, 0, "", &g);
      LOG("[boot] diagnostic (changes nothing): DeviceTree nodes/properties with 'nvme', 'nand', 'ans-', 'ans3', 'ans2', 'sep-', 'storage' or 'nvram' in the name (0.4.119: first 6 words of each property; the kernel waits for IOMedia Apple_HFS, which storage does the DT describe?) (%d): %s\n", g.hits, gb); }
    { size_t np2 = 0; int nn = dt_names_walk(dt, dtlen, &np2, 0, dt_names_to_machine, m);   /* 0.4.79 */
      if (nn >= 0) LOG("[boot] diagnostic (changes nothing): %d DeviceTree property names registered; at the stop the emulator lists lowercase dashed strings the kernel passed to calls that are NOT among them (heuristic list of missing DeviceTree properties)\n", nn);
      else LOG("[boot] diagnostic: DeviceTree property-name walk failed (tree malformed), the missing-name list stays off\n"); }
    free(dt);
    /* 0.4.132 FIX: boot_args.CommandLine is at offset 0x6c = 108 (right behind deviceTreeLength at 104), not 112. Decoded from the kernel's own
       PE_parse_boot_argn_internal (0.4.131 dump: it reads [bootArgs + 0x6c]). Up to 0.4.131 the string was written 4 bytes too far, so the kernel
       always saw an EMPTY command line: none of '-v serial=3 debug=0x14e' (nor rd=md0) ever reached it. The old 'serial=3 debug=0x14e' is NOT
       restored here: it never took effect, so its behaviour is untested; only what this experiment needs is passed. */
    /* 0.4.210 ASSUMPTION: this emulator runs ONE CPU; the 0.4.209 stacks show many driver start()s blocked in AppleARMCPU's wait for "AppleARMCPU-AllCPU-Initialized" (the 5 secondary cores never check in). 'cpus=1' is XNU's documented boot-arg that limits the number of CPUs; it only tells the kernel not to wait for cores this machine does not have. Nothing is faked. */
    strcpy((char *)b + 108, rd_on ? "-v rd=md0 cpus=1" : "-v cpus=1");
    LOG("[boot] boot_args.CommandLine (offset 108) = \"%s\"\n", (const char *)b + 108);
    a64m_mark_attempt(m, mo.lowest_vm, kphys);
    a64m_set_store_watch2(m, 0, kphys + (0xfffffff00912e490ull + 8 - mo.lowest_vm), 8);   /* 0.4.376 diagnostic: md device 0 table entry (size, flags): who sets mdInited and who clears it */
    a64m_set_pc(m, entry_p); a64m_set_boot_arg(m, ba);
    if (dt_virt) { a64m_set_dt_phys(m, dtp); /* 0.4.14: code/caller/refs/data windows dropped (the log is cut at 20000 characters; they are known now) */   /* 0.4.13: first_avail reaches avail_end BEFORE pmap_startup (0x7b53ed0): the loop that consumes every page stores from pc 0x7b53e50 inside this function */   /* 0.3.41 panic: "Invalid waitq" - the function that panicked (return address 0x7ab26ec) */
        /* 0.4.14: caller/refs/data windows dropped (the log is cut at 20000 characters) */
        /* 0.4.6: 0xfffffff007ba0378 is the kernel's own chained-fixup (rebase) loop, so the array base is a static pointer. Who initialises the array? List every instruction that references it and say which ones ever ran. */
        a64m_set_coverage(m, mo.lowest_vm, (mo.highest_vm > mo.lowest_vm) ? (mo.highest_vm - mo.lowest_vm) : 0);

           /* entry of the function that wakes on the global waitq: x0 = its argument */



        /* kernel globals the region-slide code reads: region physical start (+1496) and the old base (+1096) in the page at 0xfffffff0077de000 */
        /* 0.4.22: log 0.4.21: V=0xfffffff00914dc30 (an OSMetaClass*) is stored into the 1 KiB block at ffffffe00055e400 by a copy routine (0x79fd570/0x79fd59c, called twice from lr 0x8009ffc), whose source is the next block (…55e800). That block is later used as the OSSymbolPool buckets. Dump the function around lr 0x8009ffc (it builds the OSMetaClass records) and the copy routine, and list who calls that function. */
        a64m_set_diag_code(m, 0, 0); a64m_set_quiet_exc(m, 1);
        for (int xi = 0; xi < 4; xi += 2) if (g_xref[xi]) a64m_set_diag_code(m, g_xref[xi] - 0x1c0, 0x400);   /* 0.4.118: function that writes 0xC5ACCE55 to base+0xfb0 of each debug block (first exception ELR 0x7b9d704/0x7b9d73c) */  /* 0.4.72: log 0.4.71 (Kernel tab): 'AppleT802xIO::start: clock frequencies not published in device tree' -> the code that decides that, and the one that reads clock-frequencies */
           /* 0.4.111: waitReg32 panic at pmgr 0x23b000000+0x98000 (waits bit4 == 0): callers 0x817c7f8, 0x8ce46dc */ /* 0.4.108: caller of MTRGetPolynom (return 0x8ce4348): source of the float args (v0,v1) */ /* 0.4.107: MTRROCalibrate body: where do its float args (v0,v1) come from (inf/NaN = equal floats?) */ /* 0.4.104: AppleMTRPolynomT8020.cpp:1203 MTRROCalibrate REQUIRE ret == kIOReturnSuccess, panic call near lr 0x8cdff1c */   /* 0.4.101: loop tail of calculateCPUEnergyAccumulatorsTuning, target of the b.eq at 0x8ce4ebc (0x8ce5294) */   /* 0.4.94 (headless run of 0.4.93): AppleT8020PMGR.cpp:568 "ifane-max for freqHz=%llu not found", panic call at lr 0x8ce5104 */   /* 0.4.91: log 0.4.90: ApplePMGRMTRSensor.cpp:131 "propery mtr_0 missing" - panic call at lr 0x8ce17ac, its caller 0x8ce91e0: dump how the property name/size is used */   /* 0.4.87: log 0.4.86 - ECPU state count is still 0 though no table is empty; dump the function that handles perfDomain->voltageStateCount */   /* 0.4.84: log 0.4.83: ApplePMGR.cpp:1303 "ECPU0 1 request ECPU.0 > 0" is raised from the call at 0xfffffff008172708 (return address 0x817270c, in initDriver): dump the code before it to see what is compared. Valid for this kernelcache only. */
        /* 0.4.81: code window of 0.4.77 dropped (decoded: perf-state loop) */ //   /* 0.4.77: log 0.4.76: data abort at 0xfffffff0081713a0 (ldr w8,[x0,x8], x0=0, index (0-1)<<3 = 0x7fffffff8): code takes entry count-1 of an EMPTY property (our empty voltage-states<N>) -> dump the function (called from ApplePMGR path, 0xfffffff008ce3b74) to learn the entry size/layout. Valid for this kernelcache only. */
        /* 0.4.45: log 0.4.44 (0x200000000 as unknown block): the function at 0x7babec0 reads two words (+0x680, +0x684) from several blocks 0x40000 apart (0x200000000, 0x200040000, ...) and panics "Inconsistent memory configuration" if they differ, so it looks like per-channel memory-controller registers that must agree. Cover 1 MiB (the channels at +0x40000, +0x80000, +0xc0000) with the same unknown block: all read 0, so they agree. */
        /* 0.4.47: log 0.4.46: the memory-configuration check passes (all channels read 0 = equal). Next stop: AppleS5L8960XWatchDogTimer.cpp:55 "%s: clock speed %llu does not match absolute time" (called from 0xfffffff008ed2454, the %llu argument is 0). Dump the code before the call: where the clock speed comes from and what it is compared with. */
        /* 0.4.48: decoded 0.4.47: AppleS5L8960XWatchDogTimer::start gets a clock object (0x800a0b4(provider,key) -> [self+0x508]), calls its virtual method at vtable+0x548 (w1=0) at 0x8ed23f8 = the clock speed (x21), fills clock_timebase_info {numer,denom} (0x7bb1fa4; the log shows 125/3 = 24 MHz) and requires speed*numer == denom*1e9. Here speed = 0. Capture the first call (probe2: x1 == 0 at 0x8ed23f8): its target code and the clock object. */
        /* 0.4.73: probe2 dump no longer needed (log 0.4.72 explained it) */ // a64m_set_pc_probe2(m, 0xfffffff008ed23f8ull, 1, 0ull);
    }
    if (have_boot_reg) a64m_set_mpidr(m, 0x80000000ull | boot_reg);
    if (dt_virt) LOG("[boot] DeviceTree placed at physical 0x%llx, boot_args.deviceTreeP = virtual 0x%llx (MODELLED convention: a kernel virtual address pointing at the DT, placed right behind the kernel image, below topOfKernelData 0x%llx)\n", (unsigned long long)dtp, (unsigned long long)(mo.lowest_vm + (dtp - kphys)), (unsigned long long)top);
    LOG("[boot] step 7: RAM %llu MiB at 0x800000000 (0.4.311 ASSUMPTION: real A12 DRAM base; before: 0x40000000); boot_args at 0x%llx; entering the kernel at physical 0x%llx (virtual 0x%llx), EL1, MMU off, x0=boot_args\n",
        (unsigned long long)(ram >> 20), (unsigned long long)ba, (unsigned long long)entry_p, (unsigned long long)entry_v);
    LOG("[boot] not provided: Apple SoC peripherals, MMU, SIMD/PAC, iBoot's device-tree fix-ups. Executing for real now:\n");
    (void)r64;
    return m;
}
