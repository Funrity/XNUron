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
static int dt_has_kw(const char *s) { static const char *const kw[] = { "mtr", "aging" }; for (size_t i = 0; i < 2; i++) if (strstr(s, kw[i])) return 1; return 0; }
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
        if (dt_has_kw(nm) && g->len + 80 < g->cap) { g->len += (size_t)snprintf(g->out + g->len, g->cap - g->len, "%s/%s(%u) ", me, nm, len); g->hits++; }
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
#define LOG(...) a64m_log(m, __VA_ARGS__)

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
          size_t dtcap = dtlen + 2048; uint8_t *nd = realloc(dt, dtcap);
          if (nd) {
              dt = nd;
              int a1 = a64_dt_chosen_add_u32(dt, &dtlen, dtcap, "security-domain", 1);
              int a2 = a64_dt_chosen_add_u32(dt, &dtlen, dtcap, "chip-epoch", 0);
              { /* 0.4.65/0.4.75: ApplePMGR.cpp:899 panics "voltage-statesN not found" one N per run (log 0.4.74: N=5 after N=1); every missing voltage-states0..15 becomes an empty property so the next runs do not stop on each index */
                char vn[24], added[96] = ""; int na3 = 0, bad = 0;
                char filled[96] = ""; int nfill = 0;
                static const uint32_t vs_ph[2] = { 1u, 0u };   /* 0.4.88 PLACEHOLDER entry, see log text */
                for (int vi = 0; vi < 16; vi++) { snprintf(vn, sizeof vn, "voltage-states%d", vi);
                  if (a64_dt_node_fill_empty_prop(dt, &dtlen, dtcap, "pmgr", vn, vs_ph, 8) == 1) { size_t l = strlen(filled); snprintf(filled + l, sizeof filled - l, "%s%d", nfill ? "," : "", vi); nfill++; continue; }   /* 0.4.85: an existing but EMPTY table */
                  int a3 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "pmgr", vn, vs_ph, 8);
                  if (a3 == 1) { size_t l = strlen(added); snprintf(added + l, sizeof added - l, "%s%d", na3 ? "," : "", vi); na3++; } else if (a3 < 0) bad = a3; }
                if (na3) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'voltage-states<N>' properties of ONE 8-byte PLACEHOLDER entry {1,0} to the 'pmgr' node for N = %s (the IPSW tree lacks them; ApplePMGR.cpp:899 panics '%%s not found'; an EMPTY table makes the kernel index entry count-1 = -1 and fault, 0.4.76). The 8-byte entry stride is taken from the kernel's own code (index<<3 at 0x81713a0), what the two words mean is NOT known. ASSUMPTION: the real tables are chip-specific data that iBoot supplies; NO voltage/frequency values are invented, the entry {1,0} is a PLACEHOLDER (0.4.88): the kernel's own code (0x8171300..) counts the perf states of a type-1 domain (ECPU) as the number of leading entries with a NONZERO first word, so {0,0} gave 0 states and ApplePMGR.cpp:1303 panicked (log 0.4.87); the real first word is chip data from iBoot (UNKNOWN, NOT in the IPSW), 1 is NOT a real value, it only makes the count 1. Expect the derived frequency of that state to be meaningless\n", added);
                if (nfill) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): the IPSW tree has EMPTY (0 bytes) 'voltage-states<N>' tables for N = %s; an empty table gives a perf domain with 0 states and ApplePMGR.cpp:1303 panics 'ECPU0 ... request ECPU.0 > 0' (log 0.4.84, code decoded: needs state count > requested index 0). They now hold ONE entry {0,0} (8 zero bytes), like the first entry of the real tables (voltage-states0 = {0,0},{1,0},{2,0}). ASSUMPTION: iBoot fills the real voltages/states; NO values are invented\n", filled);
                if (bad) LOG("[boot] WARNING: could not add some 'voltage-states<N>' to the 'pmgr' node (%d)\n", bad); }
              { static const uint32_t mtr_ph[2] = { 1u, 0u };   /* 0.4.92 PLACEHOLDER: version 1, zero entries (see log text) */
                int a7 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "pmgr", "mtr_0", mtr_ph, 8);
                if (a7 == 1) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'mtr_0' = {1, 0} (8 bytes) to the 'pmgr' node. The kernel (ApplePMGRMTRSensor::agingRefDataFromProperty, code read from the log of 0.4.91, 0xfffffff008ce1750) looks this property up on its provider, requires data[0] == 1 and a length of (3*data[1]+2)*4 bytes; the IPSW tree has none (iBoot adds it: aging calibration data of the chip, UNKNOWN here). {1,0} = version 1 with ZERO entries is the smallest thing that passes the kernel's own check; it is a PLACEHOLDER, NOT real calibration data. ASSUMPTION: the provider is the 'pmgr' node (not verified; the tree has a node 'arm-io/mtrtempsensor3'). 'rosc-mtr_0' (the second name the caller passes) is NOT added\n");
                else if (a7 != 0) LOG("[boot] WARNING: could not add 'mtr_0' to 'pmgr' (%d)\n", a7); }
              { static const uint64_t f24 = 24000000ull;   /* 0.4.82: log 0.4.81; 0.4.86: 8 bytes (log 0.4.85: the kernel read 0x13016e3600 = 4 bytes + the next 4 bytes of the tree) */
                int a6 = a64_dt_node_add_prop(dt, &dtlen, dtcap, "cpu0", "fixed-frequency", &f24, 8);
                if (a6 == 1) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): added 'fixed-frequency' = 24000000 (u64, 8 bytes: log 0.4.85 showed the kernel reading 8 bytes, the 4-byte property gave 0x13016e3600) to the 'cpu0' node. The IPSW tree carries none of the cpu frequency properties (bus-/memory-/peripheral-/fixed-/clock-frequency: iBoot fills them); the kernel's frequency globals (dump at 0xfffffff00914e128) are 0 except the timebase = 24 MHz, and the watchdog (AppleS5L8960XWatchDogTimer.cpp:55) panics 'clock speed 0 does not match absolute time'. ASSUMPTION: the watchdog's clock is the fixed oscillator and equals the timebase (24 MHz) - the kernel's own check demands exactly that, and 24 MHz is the timebase this emulator models. Bus/memory/peripheral/cpu frequencies stay absent (unknown, NOT invented)\n");
                else if (a6 != 0) LOG("[boot] WARNING: could not add 'fixed-frequency' to 'cpu0' (%d)\n", a6); }
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
    { static const uint64_t want[2] = { 768ull << 20, 640ull << 20 };
      for (int wi = 0; wi < 2 && !sys; wi++) if (want[wi] > ram) { sys = a64m_create_system(want[wi]); if (sys) ram = want[wi]; }
      if (!sys) sys = a64m_create_system(ram); }
    if (!sys) { free(kern); free(dt); return dead("out of memory for guest RAM", "step 6", m); }
    { char b[65536]; size_t off = 0, k; while ((k = a64m_log_read(log_only, off, b, sizeof b)) > 0) { for (size_t q = 0; q < k; q += 400) { char t[401]; size_t c = k - q < 400 ? k - q : 400; memcpy(t, b + q, c); t[c] = 0; a64m_log(sys, "%s", t); } off += k; } }
    a64m_destroy(log_only); m = sys;
    uint64_t kphys = A64M_RAM_BASE + kbase_off;
    for (int i = 0; i < mo.nseg; i++) {
        const A64Seg *s = &mo.seg[i];
        if (!s->vmsize || !strcmp(s->name, "__PAGEZERO")) continue;
        uint64_t pa = kphys + (s->vmaddr - mo.lowest_vm);
        uint8_t *dst = a64m_ram_at(m, pa, s->vmsize);
        if (!dst || s->fileoff + s->filesize > klen - mo.slice_off || s->filesize > s->vmsize) { free(kern); free(dt); a64m_abort(m, "step 6 - segment does not fit (corrupt kernel or layout)"); return m; }
        memcpy(dst, kern + mo.slice_off + s->fileoff, s->filesize);
        LOG("[boot] step 6: %-16s %9llu bytes at physical 0x%llx (virtual 0x%llx)\n", s->name, (unsigned long long)s->filesize, (unsigned long long)pa, (unsigned long long)s->vmaddr);
    }
    uint64_t entry_v = mo.thread_pc, entry_p = kphys + (entry_v - mo.lowest_vm);
    free(kern);
    if (entry_v < mo.lowest_vm || entry_v >= mo.highest_vm) { free(dt); a64m_abort(m, "step 7 - entry point lies outside the kernel image"); return m; }

    /* boot_args, laid out as in XNU's public pexpert/arm64/boot.h as far as I know it (unverified here) */
    uint64_t ba = A64M_RAM_BASE + 0x1000, dtp = A64M_RAM_BASE + 0x10000;
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
        if (top + 0x20000 > A64M_RAM_BASE + ram) { free(dt); a64m_abort(m, "step 7 - no room for the DeviceTree above the kernel"); return m; }
    }
    /* 0.4.28 (MODELLED): iBoot hands the kernel a linear framebuffer in boot_args.Video {baseAddr(40) display(48) rowBytes(56) width(64) height(72) depth(80)}.
       Here: 540x960 by default (0.4.51; was 270x480), the size is a setting since 0.4.85, 32 bpp BGRX, rowBytes 2160, at the very top of RAM, excluded from memSize so the kernel does not hand those pages out as free memory.
       Real iBoot gives the panel size (1125x2436) and has drawn the logo already; this one starts black, everything on it is drawn by the kernel itself. */
    const uint32_t fbw = (uint32_t)a64m_video_w(), fbh = (uint32_t)a64m_video_h(), fbrb = fbw * 4; uint64_t fbsz = ((uint64_t)fbrb * fbh + 0x3FFFull) & ~0x3FFFull;
    uint64_t fbpa = A64M_RAM_BASE + ram - fbsz;
    v = fbpa - kphys; memcpy(b + 24, &v, 8);   /* memSize counts from physBase (the kernel computes end = physBase + memSize; 0.3.37 trace); the framebuffer lies above it */
    { uint64_t vd[6] = { fbpa, 0, fbrb, fbw, fbh, 32 }; memcpy(b + 40, vd, sizeof vd); a64m_set_video(m, fbpa, fbw, fbh, fbrb);
      LOG("[boot] MODELLED framebuffer: %ux%u, 32 bpp BGRX, rowBytes %u at physical 0x%llx, handed over in boot_args.Video; starts black, only the kernel draws on it (verbose boot '-v' is set, so kernel text should appear once its console is up)\n", fbw, fbh, fbrb, (unsigned long long)fbpa); }
    memcpy(b + 32, &top, 8);
    if (dt) { uint32_t pnb = 0, pom = 0, pnm = 0, pmiss = 0; int pr = a64_dt_pmgr_optional_bridges(dt, dtlen, &pnb, &pom, &pnm, &pmiss);   /* 0.4.64: must run before the tree is copied into RAM */
        if (pr == 1) LOG("[boot] DeviceTree fix-up (MODELLED, NOT what iBoot is known to do): 'pmgr' has #bridges=%u and optional-bridge-mask=0x%x but no 'bridge-settings-<i>' for %u of the bridges; the kernel (ApplePMGR.cpp:843) requires one of the two for every bridge, so those bridges were marked optional: mask 0x%x -> 0x%x. ASSUMPTION: the real settings (which iBoot or the hardware would supply) are unknown here; nothing is invented except the optional flag\n", pnb, pom, pmiss, pom, pnm);
        else if (pr == 0) LOG("[boot] DeviceTree 'pmgr': every one of the %u bridges already has bridge-settings or an optional bit (mask 0x%x), nothing changed\n", pnb, pom);
        else LOG("[boot] WARNING: 'pmgr' node with #bridges / optional-bridge-mask not found or the tree is malformed (%d), the ApplePMGR bridge check is not prepared\n", pr); }
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
        { /* 0.4.57: log 0.4.56: after aic start the kernel reads physical 0x23d1f0000 (external abort, `ldr w0,[x8,w1,uxtw]` register read at offset 0). Same method as for aic: ask the DeviceTree who owns it, model that node as an unknown block (reads 0). */
          static const uint64_t seen[2] = { 0x23d1f0000ull, 0x24ac04000ull };   /* 0.4.60: + dapf-aop (log 0.4.59: REQUIRE failed in AppleDAPF.cpp:158 for it) so its first 40 accesses are logged */
          for (int z = 0; z < 2; z++) { char nn[40], cc[100]; uint64_t nb = 0, ns = 0; int fr = a64_dt_find_pa(dt, dtlen, seen[z], nn, sizeof nn, cc, sizeof cc, &nb, &ns);
            if (fr == 1 && ns && ns <= 0x1000000ull && a64m_add_unknown_mmio_named(m, nb, ns, nn) == 0) {
                if (!strncmp(cc, "dapf", 4)) a64m_unknown_set_readback(m, nb);
                LOG("[boot] DeviceTree node '%s' (compatible '%s') owns physical 0x%llx (size 0x%llx), read by the kernel when it hit an external abort. MODELLED device: unknown SoC block '%s': reads return 0, writes are ignored, the first accesses are logged. ASSUMPTION: its real registers are UNKNOWN here\n", nn, cc, (unsigned long long)nb, (unsigned long long)ns, nn); }
            else LOG("[boot] DeviceTree lookup for physical 0x%llx: %s\n", (unsigned long long)seen[z], fr == 1 ? "found, but the size is unusable or the device could not be added" : fr == 0 ? "no /arm-io node covers it, nothing modelled" : "tree malformed"); } }
        if (a64m_add_unknown_mmio(m, 0x200000000ull, 0x100000) == 0) LOG("[boot] MODELLED device: unknown SoC register block at physical 0x200000000 (1 MiB). The kernel reads +0x680/+0x684 of it at several 0x40000-spaced offsets and compares them (\"Inconsistent memory configuration\" panic if different), so it is probably a set of memory-controller channels. Real contents UNKNOWN: reads return 0 here (so all channels agree), writes are ignored, the first accesses are logged\n");
        else LOG("[boot] WARNING: the unknown SoC block at physical 0x200000000 could NOT be added (device table full or overlapping range); the kernel's memory-configuration reads will hit an external abort\n");
        { /* 0.4.58: log 0.4.57: nub-gpio passed; the next stop is a write to node 'dapf-aop' (dapf,t8020), and the DeviceTree lists 263 such /arm-io nodes, so one round trip per node would take days. Every /arm-io node that is not modelled yet becomes an unknown zero block (reads 0, writes ignored); each block logs only its first accesses and the stop report lists every touched block with its read/write counts. A driver that polls a register for a status bit will now spin instead of aborting; the report shows which block it was. */
          static A64SocNode allb[300]; int nb2 = a64_dt_soc_nodes(dt, dtlen, allb, 300), addedn = 0, skipped = 0, ndapf = 0;
          for (int z = 0; z < nb2; z++) { if (!allb[z].size || allb[z].size > 0x1000000ull) { skipped++; continue; } if ((!strcmp(allb[z].name, "pmgr") ? a64m_add_unknown_mmio_named(m, allb[z].pa, allb[z].size, allb[z].name) : a64m_add_unknown_mmio_auto(m, allb[z].pa, allb[z].size, allb[z].name)) == 0) {   /* 0.4.62: 'pmgr' logs its first 40 accesses */ addedn++; if (!strncmp(allb[z].compat, "dapf", 4)) { a64m_unknown_set_readback(m, allb[z].pa); ndapf++; } } else skipped++; }
          LOG("[boot] MODELLED: %d more /arm-io DeviceTree nodes became unknown zero blocks (%d skipped: empty/huge range or overlapping one already modelled). ASSUMPTION: their real registers are UNKNOWN here; reads return 0, writes are ignored\n", addedn, skipped);
          LOG("[boot] MODELLED: 'dapf' blocks ('dapf-aop' and %d more) read back the last value written to a register (0 if none). Reason (the kernel's own code, AppleDAPF.cpp:158): it writes 0x80000001 to +0x0 and requires to read the same value back. ASSUMPTION: this is only a read-back, the real DAPF filter behaviour is NOT modelled\n", ndapf); }
    }
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
    { static char gb[3000]; DtGrep g = { gb, sizeof gb, 0, 0 }; size_t gp = 0; gb[0] = 0; dt_grep_walk(dt, dtlen, &gp, 0, "", &g);
      LOG("[boot] diagnostic (changes nothing): DeviceTree nodes/properties with 'mtr' or 'aging' in the name (%d): %s\n", g.hits, gb); }
    { size_t np2 = 0; int nn = dt_names_walk(dt, dtlen, &np2, 0, dt_names_to_machine, m);   /* 0.4.79 */
      if (nn >= 0) LOG("[boot] diagnostic (changes nothing): %d DeviceTree property names registered; at the stop the emulator lists lowercase dashed strings the kernel passed to calls that are NOT among them (heuristic list of missing DeviceTree properties)\n", nn);
      else LOG("[boot] diagnostic: DeviceTree property-name walk failed (tree malformed), the missing-name list stays off\n"); }
    free(dt);
    strcpy((char *)b + 112, "-v serial=3 debug=0x14e");
    a64m_mark_attempt(m, mo.lowest_vm, kphys);
    a64m_set_pc(m, entry_p); a64m_set_boot_arg(m, ba);
    if (dt_virt) { a64m_set_dt_phys(m, dtp); /* 0.4.14: code/caller/refs/data windows dropped (the log is cut at 20000 characters; they are known now) */   /* 0.4.13: first_avail reaches avail_end BEFORE pmap_startup (0x7b53ed0): the loop that consumes every page stores from pc 0x7b53e50 inside this function */   /* 0.3.41 panic: "Invalid waitq" - the function that panicked (return address 0x7ab26ec) */
        /* 0.4.14: caller/refs/data windows dropped (the log is cut at 20000 characters) */
        /* 0.4.6: 0xfffffff007ba0378 is the kernel's own chained-fixup (rebase) loop, so the array base is a static pointer. Who initialises the array? List every instruction that references it and say which ones ever ran. */
        a64m_set_coverage(m, mo.lowest_vm, (mo.highest_vm > mo.lowest_vm) ? (mo.highest_vm - mo.lowest_vm) : 0);

           /* entry of the function that wakes on the global waitq: x0 = its argument */



        /* kernel globals the region-slide code reads: region physical start (+1496) and the old base (+1096) in the page at 0xfffffff0077de000 */
        /* 0.4.22: log 0.4.21: V=0xfffffff00914dc30 (an OSMetaClass*) is stored into the 1 KiB block at ffffffe00055e400 by a copy routine (0x79fd570/0x79fd59c, called twice from lr 0x8009ffc), whose source is the next block (…55e800). That block is later used as the OSSymbolPool buckets. Dump the function around lr 0x8009ffc (it builds the OSMetaClass records) and the copy routine, and list who calls that function. */
        a64m_set_diag_code(m, 0, 0); a64m_set_quiet_exc(m, 1);
        a64m_set_diag_code(m, 0, 0);   /* 0.4.72: log 0.4.71 (Kernel tab): 'AppleT802xIO::start: clock frequencies not published in device tree' -> the code that decides that, and the one that reads clock-frequencies */
        /* 0.4.92: windows of 0.4.91 removed (decoded) */   /* 0.4.91: log 0.4.90: ApplePMGRMTRSensor.cpp:131 "propery mtr_0 missing" - panic call at lr 0x8ce17ac, its caller 0x8ce91e0: dump how the property name/size is used */   /* 0.4.87: log 0.4.86 - ECPU state count is still 0 though no table is empty; dump the function that handles perfDomain->voltageStateCount */   /* 0.4.84: log 0.4.83: ApplePMGR.cpp:1303 "ECPU0 1 request ECPU.0 > 0" is raised from the call at 0xfffffff008172708 (return address 0x817270c, in initDriver): dump the code before it to see what is compared. Valid for this kernelcache only. */
        /* 0.4.81: code window of 0.4.77 dropped (decoded: perf-state loop) */ //   /* 0.4.77: log 0.4.76: data abort at 0xfffffff0081713a0 (ldr w8,[x0,x8], x0=0, index (0-1)<<3 = 0x7fffffff8): code takes entry count-1 of an EMPTY property (our empty voltage-states<N>) -> dump the function (called from ApplePMGR path, 0xfffffff008ce3b74) to learn the entry size/layout. Valid for this kernelcache only. */
        /* 0.4.45: log 0.4.44 (0x200000000 as unknown block): the function at 0x7babec0 reads two words (+0x680, +0x684) from several blocks 0x40000 apart (0x200000000, 0x200040000, ...) and panics "Inconsistent memory configuration" if they differ, so it looks like per-channel memory-controller registers that must agree. Cover 1 MiB (the channels at +0x40000, +0x80000, +0xc0000) with the same unknown block: all read 0, so they agree. */
        /* 0.4.47: log 0.4.46: the memory-configuration check passes (all channels read 0 = equal). Next stop: AppleS5L8960XWatchDogTimer.cpp:55 "%s: clock speed %llu does not match absolute time" (called from 0xfffffff008ed2454, the %llu argument is 0). Dump the code before the call: where the clock speed comes from and what it is compared with. */
        /* 0.4.48: decoded 0.4.47: AppleS5L8960XWatchDogTimer::start gets a clock object (0x800a0b4(provider,key) -> [self+0x508]), calls its virtual method at vtable+0x548 (w1=0) at 0x8ed23f8 = the clock speed (x21), fills clock_timebase_info {numer,denom} (0x7bb1fa4; the log shows 125/3 = 24 MHz) and requires speed*numer == denom*1e9. Here speed = 0. Capture the first call (probe2: x1 == 0 at 0x8ed23f8): its target code and the clock object. */
        /* 0.4.73: probe2 dump no longer needed (log 0.4.72 explained it) */ // a64m_set_pc_probe2(m, 0xfffffff008ed23f8ull, 1, 0ull);
    }
    if (have_boot_reg) a64m_set_mpidr(m, 0x80000000ull | boot_reg);
    if (dt_virt) LOG("[boot] DeviceTree placed at physical 0x%llx, boot_args.deviceTreeP = virtual 0x%llx (MODELLED convention: a kernel virtual address pointing at the DT, placed right behind the kernel image, below topOfKernelData 0x%llx)\n", (unsigned long long)dtp, (unsigned long long)(mo.lowest_vm + (dtp - kphys)), (unsigned long long)top);
    LOG("[boot] step 7: RAM %llu MiB at 0x40000000; boot_args at 0x%llx; entering the kernel at physical 0x%llx (virtual 0x%llx), EL1, MMU off, x0=boot_args\n",
        (unsigned long long)(ram >> 20), (unsigned long long)ba, (unsigned long long)entry_p, (unsigned long long)entry_v);
    LOG("[boot] not provided: Apple SoC peripherals, MMU, SIMD/PAC, iBoot's device-tree fix-ups. Executing for real now:\n");
    (void)r64;
    return m;
}
