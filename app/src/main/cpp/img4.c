/* Apple IMG4 / IM4P (DER) container inspection. Read-only; no key handling. */
#include "inspect.h"
#include <string.h>
#include <stdio.h>

typedef struct { uint8_t tag; const uint8_t *val; size_t len; const uint8_t *next; } Der;

static int der_read(const uint8_t *p, const uint8_t *end, Der *o) {
    if (p + 2 > end) return -1;
    o->tag = p[0];
    if ((p[0] & 0x1F) == 0x1F) return -1;                 /* multi-byte tags not used here */
    size_t len = p[1]; const uint8_t *q = p + 2;
    if (len & 0x80) {
        unsigned nb = len & 0x7F; if (nb == 0 || nb > 4 || q + nb > end) return -1;
        len = 0; for (unsigned k = 0; k < nb; k++) len = (len << 8) | *q++;
    }
    if (len > (size_t)(end - q)) return -1;
    o->val = q; o->len = len; o->next = q + len;
    return 0;
}

static const char *type_name(const char *t) {
    static const struct { const char *t, *n; } k[] = {
        {"ibot", "iBoot (second-stage bootloader)"}, {"ibss", "iBSS (DFU-stage bootloader)"}, {"ibec", "iBEC (recovery bootloader)"},
        {"illb", "LLB (low-level bootloader)"}, {"krnl", "kernelcache (XNU + kexts)"}, {"dtre", "DeviceTree"},
        {"rdsk", "restore ramdisk"}, {"rosi", "restore OS image"}, {"sepi", "SEP firmware (Secure Enclave)"},
        {"trst", "static trust cache"}, {"logo", "boot logo"}, {"batC", "battery image"}, {"glyC", "recovery glyph"},
        {"rkrn", "restore kernelcache"}, {"rtsc", "ramdisk trust cache"}, {"ansf", "NAND/storage firmware"},
    };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!memcmp(t, k[i].t, 4)) return k[i].n;
    return "unrecognised component type";
}

static size_t payload_magic(const uint8_t *p, size_t n, char *out, size_t cap) {
    if (n >= 4 && (!memcmp(p, "bvx2", 4) || !memcmp(p, "bvxn", 4) || !memcmp(p, "bvx-", 4) || !memcmp(p, "bvx1", 4)))
        return (size_t)snprintf(out, cap, "LZFSE stream (magic %.4s)", (const char *)p);
    if (n >= 8 && !memcmp(p, "complzss", 8)) return (size_t)snprintf(out, cap, "LZSS-compressed kernel (complzss)");
    if (n >= 4 && p[0] == 0xCF && p[1] == 0xFA && p[2] == 0xED && p[3] == 0xFE) return (size_t)snprintf(out, cap, "Mach-O 64-bit (already uncompressed)");
    return (size_t)snprintf(out, cap, "unrecognised/high-entropy data (first bytes %02x %02x %02x %02x)", n > 0 ? p[0] : 0, n > 1 ? p[1] : 0, n > 2 ? p[2] : 0, n > 3 ? p[3] : 0);
}

#define P(...) do { if (n < cap) n += (size_t)snprintf(rep + n, cap - n, __VA_ARGS__); } while (0)

static size_t describe_im4p(const uint8_t *val, size_t len, bool has_manifest, char *rep, size_t cap, size_t n) {
    const uint8_t *e = val + len, *p = val; Der d;
    char type[5] = "????", desc[64] = ""; bool kbag = false, comp = false; int nkb = 0;
    const uint8_t *data = NULL; size_t dlen = 0; int idx = 0;
    while (p < e && der_read(p, e, &d) == 0) {
        if (idx == 0 && d.tag == 0x16) { /* "IM4P" already known */ }
        else if (idx == 1 && d.tag == 0x16 && d.len == 4) memcpy(type, d.val, 4);
        else if (idx == 2 && d.tag == 0x16) { size_t k = d.len < 63 ? d.len : 63; memcpy(desc, d.val, k); desc[k] = 0; }
        else if (idx == 3 && d.tag == 0x04) { data = d.val; dlen = d.len; }
        else if (idx >= 4 && d.tag == 0x04) {
            kbag = true; Der s;
            if (der_read(d.val, d.val + d.len, &s) == 0 && s.tag == 0x30) {
                const uint8_t *q = s.val; Der x; while (q < s.val + s.len && der_read(q, s.val + s.len, &x) == 0) { nkb++; q = x.next; }
            }
        } else if (idx >= 4 && d.tag == 0x30) comp = true;
        p = d.next; idx++;
    }
    P("Component type '%.4s': %s\n", type, type_name(type));
    if (desc[0]) P("Description string: %s\n", desc);
    P("Payload: %zu bytes\n", dlen);
    if (kbag) {
        P("[BLOCKER] Key bag (KBAG) present with %d key slot(s): the payload is AES-encrypted. The keys are wrapped for Apple's hardware GID key; "
          "they are not in this file and XNUron neither contains, derives nor fetches them.\n", nkb);
    } else if (data) {
        char m[160]; payload_magic(data, dlen, m, sizeof m);
        P("Payload is not encrypted (no KBAG). Content: %s\n", m);
        if (comp) P("Compression info present; XNUron has no LZFSE/LZSS decompressor yet (see docs/FEASIBILITY.md, next steps).\n");
    }
    if (has_manifest) P("[BLOCKER] IMG4 manifest (IM4M) present: Apple signs it for one specific device (ECID) and boot nonce. SecureROM/iBoot verify this signature in the real boot chain; "
                         "a64virt has no boot ROM, so there is nothing that could verify or consume it, and patching verification out of Apple's boot software is outside this project.\n");
    return n;
}

size_t a64_img4_describe(const uint8_t *d0, size_t len, A64Inspect *res, char *rep, size_t cap) {
    size_t n = 0; Der top;
    if (der_read(d0, d0 + len, &top) || top.tag != 0x30) { P("Not a DER SEQUENCE.\n"); return n; }
    Der a; if (der_read(top.val, top.val + top.len, &a) || a.tag != 0x16 || a.len != 4) { P("DER container without a 4-character IA5String tag.\n"); return n; }
    if (!memcmp(a.val, "IM4P", 4)) {
        res->kind = A64_KIND_IM4P; P("Apple IM4P payload container (DER)\n");
        n = describe_im4p(top.val, top.len, false, rep, cap, n);
    } else if (!memcmp(a.val, "IMG4", 4)) {
        res->kind = A64_KIND_IMG4; P("Apple IMG4 container (DER)\n");
        const uint8_t *p = a.next, *e = top.val + top.len; Der c; bool im4m = false, im4r = false; const Der *pl = NULL; Der plv;
        while (p < e && der_read(p, e, &c) == 0) {
            if (c.tag == 0x30) { Der t; if (der_read(c.val, c.val + c.len, &t) == 0 && t.tag == 0x16 && t.len == 4 && !memcmp(t.val, "IM4P", 4)) { plv = c; pl = &plv; } }
            else if (c.tag == 0xA0) im4m = true; else if (c.tag == 0xA1) im4r = true;
            p = c.next;
        }
        P("Contains: IM4P payload%s%s\n", im4m ? ", IM4M signed manifest" : "", im4r ? ", IM4R restore info" : "");
        if (pl) n = describe_im4p(pl->val, pl->len, im4m, rep, cap, n); else P("No IM4P payload inside the IMG4.\n");
    } else { P("DER container tagged '%.4s' (not IM4P/IMG4/IM4M).\n", (const char *)a.val); }
    return n;
}
#undef P

/* Extracts the payload of an IM4P (or of the IM4P inside an IMG4). Never decrypts. */
int a64_im4p_unwrap(const uint8_t *d0, size_t len, A64Im4p *o) {
    memset(o, 0, sizeof *o); Der top;
    if (der_read(d0, d0 + len, &top) || top.tag != 0x30) return -1;
    Der a; if (der_read(top.val, top.val + top.len, &a) || a.tag != 0x16 || a.len != 4) return -1;
    const uint8_t *val = top.val; size_t vl = top.len;
    if (!memcmp(a.val, "IMG4", 4)) {
        const uint8_t *p = a.next, *e = top.val + top.len; Der c; bool found = false;
        while (p < e && der_read(p, e, &c) == 0) {
            if (c.tag == 0x30) { Der t; if (der_read(c.val, c.val + c.len, &t) == 0 && t.tag == 0x16 && t.len == 4 && !memcmp(t.val, "IM4P", 4)) { val = c.val; vl = c.len; found = true; } }
            else if (c.tag == 0xA0) o->has_im4m = true;
            p = c.next;
        }
        if (!found) return -1;
    } else if (memcmp(a.val, "IM4P", 4)) return -1;
    const uint8_t *e = val + vl, *p = val; Der d; int idx = 0;
    while (p < e && der_read(p, e, &d) == 0) {
        if (idx == 1 && d.tag == 0x16 && d.len == 4) memcpy(o->type, d.val, 4);
        else if (idx == 3 && d.tag == 0x04) { o->data = d.val; o->len = d.len; }
        else if (idx >= 4 && d.tag == 0x04) o->has_kbag = true;
        p = d.next; idx++;
    }
    return o->data ? 0 : -1;
}
