/* ZIP / IPSW central-directory listing through random-access reads (supports zip64). */
#include "inspect.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)r16(p) | (uint32_t)r16(p + 2) << 16; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }

static const struct { const char *code, *what; int bits; } SOC[] = {
    {"s5l8900", "S5L8900 (ARM1176, ARMv6) - iPhone/iPhone 3G/iPod touch 1G", 32}, {"s5l8720", "S5L8720 (ARM1176, ARMv6) - iPod touch 2G", 32},
    {"s5l8920", "S5L8920 (Cortex-A8, ARMv7) - iPhone 3GS", 32}, {"s5l8922", "S5L8922 (Cortex-A8, ARMv7)", 32}, {"s5l8930", "S5L8930 (A4, ARMv7)", 32},
    {"s5l8940", "S5L8940 (A5, ARMv7)", 32}, {"s5l8942", "S5L8942 (A5, ARMv7)", 32}, {"s5l8945", "S5L8945 (A5X, ARMv7)", 32}, {"s5l8947", "S5L8947 (A5, ARMv7)", 32},
    {"s5l8950", "S5L8950 (A6, ARMv7s)", 32}, {"s5l8955", "S5L8955 (A6X, ARMv7s)", 32},
    {"s5l8960", "S5L8960 (A7, first 64-bit)", 64}, {"t7000", "T7000 (A8)", 64}, {"t7001", "T7001 (A8X)", 64}, {"s8000", "S8000 (A9)", 64}, {"s8003", "S8003 (A9)", 64},
    {"t8010", "T8010 (A10)", 64}, {"t8015", "T8015 (A11)", 64}, {"t8020", "T8020 (A12)", 64}, {"t8030", "T8030 (A13)", 64}, {"t8101", "T8101 (A14)", 64}, {"t8110", "T8110 (A15)", 64},
};
static bool has(const char *s, const char *sub) {
    size_t n = strlen(sub), l = strlen(s);
    for (size_t i = 0; i + n <= l; i++) if (!strncasecmp(s + i, sub, n)) return true;
    return false;
}
#define P(...) do { if (n < cap) n += (size_t)snprintf(rep + n, cap - n, __VA_ARGS__); } while (0)

size_t a64_zip_describe(A64ReadAt rd, void *ud, uint64_t total, A64Inspect *res, char *rep, size_t cap) {
    size_t n = 0; res->kind = A64_KIND_ZIP;
    size_t tail = total < 70000 ? (size_t)total : 70000;
    uint8_t *t = malloc(tail); if (!t) { P("out of memory\n"); return n; }
    if (rd(ud, total - tail, t, tail)) { free(t); P("cannot read end of file\n"); return n; }
    long e = -1; for (long k = (long)tail - 22; k >= 0; k--) if (r32(t + k) == 0x06054b50u) { e = k; break; }
    if (e < 0) { free(t); P("ZIP end-of-central-directory record not found (damaged or not a ZIP).\n"); return n; }
    uint64_t cnt = r16(t + e + 10), cdsz = r32(t + e + 12), cdoff = r32(t + e + 16);
    if ((cnt == 0xFFFF || cdsz == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) && e >= 20 && r32(t + e - 20) == 0x07064b50u) {
        uint64_t z64 = r64(t + e - 20 + 8); uint8_t b[56];
        if (rd(ud, z64, b, 56) == 0 && r32(b) == 0x06064b50u) { cnt = r64(b + 32); cdsz = r64(b + 40); cdoff = r64(b + 48); }
    }
    free(t);
    if (cdoff + cdsz > total || cdsz > (256u << 20)) { P("central directory is out of range\n"); return n; }
    uint8_t *cd = malloc(cdsz ? cdsz : 1); if (!cd || rd(ud, cdoff, cd, cdsz)) { free(cd); P("cannot read central directory\n"); return n; }
    P("ZIP archive with %llu entries\n", (unsigned long long)cnt);
    const char *soc_what = NULL; int soc_bits = 0;
    bool manifest = false, restore = false, kernel = false, boot = false, dtree = false, rootfs = false, sep = false;
    uint64_t pos = 0, shown = 0, usum = 0; uint64_t seen = 0;
    char name[256]; int listed = 0;
    while (pos + 46 <= cdsz && seen < cnt && r32(cd + pos) == 0x02014b50u) {
        uint16_t method = r16(cd + pos + 10), nl = r16(cd + pos + 28), xl = r16(cd + pos + 30), cl = r16(cd + pos + 32);
        uint64_t csz = r32(cd + pos + 20), usz = r32(cd + pos + 24);
        if (pos + 46 + nl + xl + cl > cdsz) break;
        size_t k = nl < sizeof name - 1 ? nl : sizeof name - 1; memcpy(name, cd + pos + 46, k); name[k] = 0;
        const uint8_t *x = cd + pos + 46 + nl; uint32_t off32 = r32(cd + pos + 42); (void)off32;
        for (const uint8_t *q = x; q + 4 <= x + xl;) {                      /* zip64 extra field */
            uint16_t id = r16(q), sz = r16(q + 2); const uint8_t *v = q + 4;
            if (id == 1) { if (usz == 0xFFFFFFFFu && v + 8 <= q + 4 + sz) { usz = r64(v); v += 8; } if (csz == 0xFFFFFFFFu && v + 8 <= q + 4 + sz) { csz = r64(v); } }
            q += 4 + sz;
        }
        usum += usz; seen++;
        bool is_manifest = has(name, "BuildManifest.plist"), is_restore = has(name, "Restore.plist");
        bool is_kc = has(name, "kernelcache"), is_boot = has(name, "iBoot") || has(name, "iBSS") || has(name, "iBEC") || has(name, "LLB");
        bool is_dt = has(name, "DeviceTree"), is_fs = has(name, ".dmg") && !has(name, ".dmg.trustcache"), is_sep = has(name, "sep-firmware");
        if (!soc_what) for (unsigned q = 0; q < sizeof SOC / sizeof *SOC; q++) if (has(name, SOC[q].code)) { soc_what = SOC[q].what; soc_bits = SOC[q].bits; break; }
        manifest |= is_manifest; restore |= is_restore; kernel |= is_kc; boot |= is_boot; dtree |= is_dt; rootfs |= is_fs; sep |= is_sep;
        if ((is_manifest || is_restore || is_kc || is_boot || is_dt || is_fs || is_sep) && listed < 40) {
            P("  %-60.60s %12llu bytes  %s\n", name, (unsigned long long)usz, method == 0 ? "stored" : method == 8 ? "deflate" : "other method");
            listed++;
        }
        pos += 46 + nl + xl + cl; shown++;
    }
    free(cd);
    P("Total uncompressed size: %llu bytes\n", (unsigned long long)usum);
    if ((manifest || restore) && kernel) {
        res->kind = A64_KIND_IPSW;
        if (soc_what) P("Target SoC (from file names): %s - %d-bit CPU.\n", soc_what, soc_bits);
        if (soc_bits == 32) P("[BLOCKER] This firmware is for a 32-bit ARM device. XNUron emulates only AArch64, so none of its code can run, even in principle.\n");
        if (!manifest) P("Old-style IPSW (no BuildManifest.plist): older devices used simpler, but still Apple-signed, containers (8900/IMG2/IMG3).\n");
        P("This looks like an Apple IPSW firmware archive (BuildManifest.plist, Restore.plist, kernelcache%s%s%s%s present).\n",
          boot ? ", bootloaders" : "", dtree ? ", DeviceTree" : "", rootfs ? ", disk images" : "", sep ? ", SEP firmware" : "");
        P("[BLOCKER] An IPSW is a restore package for real Apple hardware: every boot component is an IMG4 personalised/signed by Apple per device and boot; "
          "some components (e.g. SEP firmware, and on older versions the kernelcache/bootloaders) are encrypted with per-build key bags that are not in the archive.\n");
        P("[BLOCKER] Even with decrypted components, iOS needs an Apple SoC (AIC interrupt controller, Apple UART/PMGR/AMCC, SEP, ANS storage, display coprocessor, AGX GPU), "
          "an MMU and Apple-specific CPU registers. a64virt provides none of these.\n");
        if (soc_bits != 32) P("You can still press Start with the source \"IPSW - TRY to boot kernelcache\": XNUron will unpack the kernel and really execute it until the emulator hits something it lacks (expected: very early). It stops at once if the kernel is encrypted.\n");
        P("Licensing: Apple's iOS software licence restricts use to Apple-branded hardware. XNUron does not download, bundle or modify Apple firmware.\n");
    } else P("Not recognised as an IPSW (needs Restore.plist or BuildManifest.plist, plus a kernelcache). XNUron cannot boot a ZIP archive.\n");
    return n;
}
#undef P
