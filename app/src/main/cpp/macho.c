#include "macho.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define MH_MAGIC_64 0xFEEDFACFu
#define FAT_MAGIC 0xCAFEBABEu
#define CPU_TYPE_ARM64 0x0100000Cu
#define LC_SEGMENT_64 0x19u
#define LC_REQ_DYLD 0x80000000u

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int perr(A64MachO *o, const char *m) { snprintf(o->err, sizeof o->err, "%s", m); return -1; }

int a64_macho_parse(const uint8_t *d, size_t n, A64MachO *o) {
    memset(o, 0, sizeof *o);
    if (n < 32) return perr(o, "file too small for a Mach-O header");
    size_t base = 0, len = n;
    if (be32(d) == FAT_MAGIC) {                       /* universal binary: pick the arm64 slice */
        uint32_t na = be32(d + 4);
        if (na == 0 || na > 16 || 8 + (size_t)na * 20 > n) return perr(o, "invalid universal (fat) header");
        o->fat = true; o->fat_archs = na; bool found = false;
        for (uint32_t k = 0; k < na; k++) {
            const uint8_t *a = d + 8 + k * 20;
            if (be32(a) == CPU_TYPE_ARM64) {
                uint32_t off = be32(a + 8), sz = be32(a + 12);
                if ((uint64_t)off + sz > n) return perr(o, "fat slice extends past end of file");
                base = off; len = sz; found = true; break;
            }
        }
        if (!found) return perr(o, "universal binary has no arm64 slice");
    }
    o->slice_off = base; o->slice_len = len;
    const uint8_t *h = d + base;
    if (len < 32 || rd32(h) != MH_MAGIC_64) return perr(o, "not a 64-bit little-endian Mach-O (magic mismatch)");
    o->cputype = rd32(h + 4); o->cpusubtype = rd32(h + 8); o->filetype = rd32(h + 12);
    o->ncmds = rd32(h + 16); uint32_t sizeofcmds = rd32(h + 20); o->flags = rd32(h + 24);
    if (32 + (uint64_t)sizeofcmds > len) return perr(o, "load commands extend past end of file");
    o->lowest_vm = ~0ull;
    size_t off = 32;
    for (uint32_t k = 0; k < o->ncmds; k++) {
        if (off + 8 > 32 + (size_t)sizeofcmds) return perr(o, "truncated load command table");
        uint32_t cmd = rd32(h + off), cs = rd32(h + off + 4);
        if (cs < 8 || cs % 4 || off + cs > 32 + (size_t)sizeofcmds) return perr(o, "malformed load command size");
        const uint8_t *c = h + off;
        switch (cmd & ~LC_REQ_DYLD) {
        case LC_SEGMENT_64:
            if (cs < 72) return perr(o, "short LC_SEGMENT_64");
            if (o->nseg < A64_MACHO_MAX_SEG) {
                A64Seg *s = &o->seg[o->nseg++];
                memcpy(s->name, c + 8, 16); s->name[16] = 0;
                s->vmaddr = rd64(c + 24); s->vmsize = rd64(c + 32); s->fileoff = rd64(c + 40); s->filesize = rd64(c + 48);
                s->maxprot = rd32(c + 56); s->initprot = rd32(c + 60);
                if (s->vmsize && strcmp(s->name, "__PAGEZERO")) {
                    if (s->vmaddr < o->lowest_vm) o->lowest_vm = s->vmaddr;
                    if (s->vmaddr + s->vmsize > o->highest_vm) o->highest_vm = s->vmaddr + s->vmsize;
                }
            }
            break;
        case 0x28: if (cs >= 24) { o->has_main = true; o->entryoff = rd64(c + 8); } break;   /* LC_MAIN */
        case 0x05:                                                                           /* LC_UNIXTHREAD */
            if (cs >= 16 + 8 * 33 && rd32(c + 8) == 6) { o->has_unixthread = true; o->thread_pc = rd64(c + 16 + 8 * 32); }
            break;
        case 0x0E: o->has_dylinker = true; break;
        case 0x0C: case 0x18: case 0x1F: case 0x20: case 0x23:   /* LOAD, WEAK, REEXPORT, LAZY, UPWARD dylib */
            if (o->ndylibs < A64_MACHO_MAX_DYLIB && cs > 24) {
                size_t no = rd32(c + 8); size_t mx = cs > no ? cs - no : 0;
                snprintf(o->dylib[o->ndylibs], sizeof o->dylib[0], "%.*s", (int)(mx < 95 ? mx : 95), (const char *)c + no);
                o->ndylibs++;
            }
            break;
        case 0x22: o->has_dyld_info = true; break;                                         /* DYLD_INFO(_ONLY) */
        case 0x34: o->has_chained_fixups = true; break;                                   /* LC_DYLD_CHAINED_FIXUPS (0x80000034) */
        case 0x1D: o->has_code_signature = true; break;
        case 0x2C: if (cs >= 20) { o->has_encryption = true; o->cryptid = rd32(c + 16); } break;
        case 0x32: if (cs >= 16) { o->has_build_version = true; o->platform = rd32(c + 8); o->minos = rd32(c + 12); } break;
        default: break;
        }
        off += cs;
    }
    o->valid = true;
    return 0;
}

static const char *platform_name(uint32_t p) {
    switch (p) { case 1: return "macOS"; case 2: return "iOS"; case 3: return "tvOS"; case 4: return "watchOS";
                 case 5: return "bridgeOS"; case 6: return "Mac Catalyst"; case 7: return "iOS Simulator";
                 case 11: return "visionOS"; default: return "other"; }
}

size_t a64_macho_describe(const A64MachO *m, char *buf, size_t cap) {
    size_t n = 0;
#define P(...) do { if (n < cap) n += (size_t)snprintf(buf + n, cap - n, __VA_ARGS__); } while (0)
    const char *ft = m->filetype == 2 ? "EXECUTE" : m->filetype == 6 ? "DYLIB" : m->filetype == 0xC ? "FILESET (kernel collection)" :
                     m->filetype == 0xB ? "KEXT_BUNDLE" : m->filetype == 8 ? "BUNDLE" : m->filetype == 1 ? "OBJECT" : "other";
    P("Mach-O 64-bit%s, cputype 0x%x (%s) subtype 0x%x%s, filetype %u (%s)\n", m->fat ? " (from universal binary)" : "", m->cputype,
      m->cputype == CPU_TYPE_ARM64 ? "arm64" : "NOT arm64", m->cpusubtype & 0xFFFFFF, (m->cpusubtype & 0xFFFFFF) == 2 ? " = arm64e (pointer authentication)" : "", m->filetype, ft);
    if (m->has_build_version) P("Built for %s, minimum OS %u.%u\n", platform_name(m->platform), m->minos >> 16, (m->minos >> 8) & 0xFF);
    for (int k = 0; k < m->nseg; k++)
        P("  segment %-12s vm 0x%llx + 0x%llx  file %llu bytes  prot %u/%u\n", m->seg[k].name, (unsigned long long)m->seg[k].vmaddr,
          (unsigned long long)m->seg[k].vmsize, (unsigned long long)m->seg[k].filesize, m->seg[k].initprot, m->seg[k].maxprot);
    if (m->has_main) P("Entry: LC_MAIN offset 0x%llx\n", (unsigned long long)m->entryoff);
    if (m->has_unixthread) P("Entry: LC_UNIXTHREAD pc 0x%llx\n", (unsigned long long)m->thread_pc);
    if (m->has_dylinker) P("Uses dyld (LC_LOAD_DYLINKER)\n");
    for (int k = 0; k < m->ndylibs; k++) P("Links %s\n", m->dylib[k]);
    if (m->has_chained_fixups) P("Has chained fixups\n");
    if (m->has_code_signature) P("Has a code signature (not verified by XNUron)\n");
    if (m->has_encryption) P("Encryption info: cryptid=%u%s\n", m->cryptid, m->cryptid ? " (FairPlay-encrypted)" : "");
#undef P
    return n;
}

void a64_macho_runnable_reason(const A64MachO *m, char *buf, size_t cap) {
    buf[0] = 0;
#define R(...) do { snprintf(buf, cap, __VA_ARGS__); return; } while (0)
    if (!m->valid) R("%s", m->err);
    if (m->cputype != CPU_TYPE_ARM64) R("cputype 0x%x is not arm64", m->cputype);
    if (m->filetype == 0xC) R("MH_FILESET kernel collection: this is an XNU kernelcache, which needs the full Apple boot chain (iBoot boot-args, device tree, MMU, Apple SoC), not a user-mode loader");
    if (m->filetype != 2) R("filetype %u is not an executable", m->filetype);
    if (m->has_encryption && m->cryptid) R("segments are FairPlay-encrypted (cryptid=%u); XNUron does not decrypt them", m->cryptid);
    if (m->lowest_vm >= 0xFFFF000000000000ull) R("linked at kernel virtual addresses (0x%llx): needs an MMU with translation tables, which XNUron does not implement", (unsigned long long)m->lowest_vm);
    if (m->ndylibs) R("links %d dynamic librar%s (first: %s): the loader runs no dyld and provides no libSystem/dyld shared cache; only statically self-contained executables work", m->ndylibs, m->ndylibs == 1 ? "y" : "ies", m->dylib[0]);
    if (!m->has_main && !m->has_unixthread) R("no entry point (LC_MAIN / LC_UNIXTHREAD)");
    if (m->nseg == 0 || m->lowest_vm == ~0ull) R("no loadable segments");
    if (m->highest_vm - m->lowest_vm > (256ull << 20)) R("image spans more than 256 MiB of address space");
#undef R
}

A64Machine *a64_macho_create_machine(const uint8_t *d, size_t n, char *err, size_t errcap) {
    A64MachO mo;
    a64_macho_parse(d, n, &mo);
    a64_macho_runnable_reason(&mo, err, errcap);
    if (err[0]) return NULL;
    uint64_t base = mo.lowest_vm & ~0xFFFull, span = ((mo.highest_vm - base) + 0xFFFF) & ~0xFFFFull;
    uint64_t stack = 1u << 20;
    A64Machine *m = a64m_create_user(base, span + stack);
    if (!m) { snprintf(err, errcap, "out of memory"); return NULL; }
    for (int k = 0; k < mo.nseg; k++) {
        const A64Seg *s = &mo.seg[k];
        if (!s->vmsize || !strcmp(s->name, "__PAGEZERO")) continue;
        uint64_t fsz = s->filesize;
        if (s->fileoff + fsz > mo.slice_len || fsz > s->vmsize) { snprintf(err, errcap, "segment %s file range is outside the file", s->name); a64m_destroy(m); return NULL; }
        uint8_t *dst = a64m_ram_at(m, s->vmaddr, s->vmsize);
        if (!dst) { snprintf(err, errcap, "segment %s does not fit guest memory", s->name); a64m_destroy(m); return NULL; }
        memcpy(dst, d + mo.slice_off + s->fileoff, fsz);
    }
    uint64_t text = 0;
    for (int k = 0; k < mo.nseg; k++) if (!strcmp(mo.seg[k].name, "__TEXT")) text = mo.seg[k].vmaddr;
    A64 *c = a64m_cpu(m);
    c->pc = mo.has_main ? text + mo.entryoff : mo.thread_pc;
    c->sp = base + span + stack - 16;
    return m;
}
