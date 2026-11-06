/* System-mode tests: a64virt machine + the bundled test guest kernel. */
#include "../app/src/main/cpp/machine.h"
#include "../app/src/main/cpp/guest_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)

static char *log_text(A64Machine *m) {
    size_t n = a64m_log_size(m); char *b = malloc(n + 1); n = a64m_log_read(m, 0, b, n); b[n] = 0; return b;
}

int main(int argc, char **argv) {
    int verbose = getenv("A64_VERBOSE") != NULL;
    /* 1. self-test mode: guest runs its tests and powers off */
    A64Machine *m = a64m_create_system(16u << 20);
    a64m_load_raw(m, a64_guest_image, a64_guest_image_size, 0);
    a64m_set_boot_arg(m, 0);
    a64m_set_arg_watch(m, 0x40010000ull);
    a64m_set_block_hist(m, 0x40010000ull, 0x1000);   /* the guest builds its page tables here */
    A64MState st = a64m_run(m, 200000000, false);
    char *t = log_text(m);
    if (verbose) puts(t);
    CHECK(st == A64M_POWEROFF && a64m_exit_code(m) == 0, "guest self-test finishes and powers off with exit code 0");
    CHECK(a64m_block_hist_count(m) > 0 && a64m_block_hist_count(m) <= 40, "block history records stores to a watched physical block (bounded)");
    CHECK(a64m_arg_watch_count(m) <= 48, "argument watch is bounded");
    CHECK(strstr(t, "[test] summary: 0 failure(s)") != NULL, "guest reports 0 failures");
    CHECK(strstr(t, "FAIL") == NULL, "no test line says FAIL");
    const char *want[] = { "exception: SVC at EL1", "data abort on unmapped address", "undefined instruction (UDF)",
        "timer: three IRQs", "EL0: syscalls via SVC", "EL0: privileged MRS SCTLR_EL1 traps", "atomics: ldxr/stxr", "128-bit multiply high" };
    for (unsigned k = 0; k < sizeof want / sizeof *want; k++) {
        char msg[96], pat[96]; snprintf(pat, sizeof pat, "%s", want[k]);
        char *p = strstr(t, pat); snprintf(msg, sizeof msg, "guest test passed: %s", want[k]);
        CHECK(p && strstr(p, ": PASS") && strstr(p, ": PASS") < (strchr(p, '\n') ? strchr(p, '\n') : p + 1000), msg);
    }
    CHECK(strstr(t, "NOT iOS") != NULL, "guest banner states it is not iOS");
    free(t); a64m_destroy(m);

    /* 2. interactive mode: display + touch */
    m = a64m_create_system(16u << 20);
    a64m_load_raw(m, a64_guest_image, a64_guest_image_size, 0);
    a64m_set_boot_arg(m, 1);
    static uint32_t frame[A64M_FB_W * A64M_FB_H];
    CHECK(a64m_frame(m, frame, A64M_FB_W, A64M_FB_H) == -1, "display reports 'not enabled' before the guest initialises it");
    a64m_run(m, 60000000, false);
    int64_t f0 = a64m_frame(m, frame, A64M_FB_W, A64M_FB_H);
    CHECK(f0 >= 1, "guest enabled the display and flipped a frame");
    CHECK(frame[100 * A64M_FB_W + 5] != 0xFF000000u && (frame[(100) * A64M_FB_W + 5] & 0xFFFFFF) != 0, "framebuffer contains guest-drawn pixels");
    uint32_t bg = frame[400 * A64M_FB_W + 135];
    a64m_touch(m, 100, 200, true);
    a64m_run(m, 3000000, false);
    int64_t f1 = a64m_frame(m, frame, A64M_FB_W, A64M_FB_H);
    CHECK(f1 > f0, "touch event caused the guest to redraw (frame counter advanced)");
    CHECK(frame[200 * A64M_FB_W + 100] == 0xFFFFD040u, "touch drawn by guest at (100,200)");
    CHECK(frame[400 * A64M_FB_W + 135] == bg, "pixels away from the touch are unchanged");
    a64m_touch(m, 100, 200, false);
    a64m_run(m, 3000000, false);
    a64m_frame(m, frame, A64M_FB_W, A64M_FB_H);
    CHECK(frame[200 * A64M_FB_W + 100] == 0xFFFF4060u, "touch release handled by guest");
    t = log_text(m);
    CHECK(strstr(t, "[touch] down/move x=100 y=200") != NULL && strstr(t, "[touch] up") != NULL, "touch events appear in the boot log");
    uint32_t hb0 = frame[14 * A64M_FB_W + (A64M_FB_W - 18)];
    int changed = 0;
    for (int k = 0; k < 400 && !changed; k++) {      /* guest idles in WFI; virtual time is fast-forwarded */
        a64m_run(m, 5000, false);
        a64m_frame(m, frame, A64M_FB_W, A64M_FB_H);
        changed = frame[14 * A64M_FB_W + (A64M_FB_W - 18)] != hb0;
    }
    CHECK(changed, "timer-driven heartbeat changes on screen");
    free(t);
    /* stop request */
    a64m_request_stop(m);
    CHECK(a64m_run(m, 1000000, false) == A64M_STOPPED, "stop request ends the run");
    a64m_destroy(m);

    /* 3. diagnostics: a non-bootable blob faults with a clear message */
    m = a64m_create_system(1u << 20);
    uint8_t junk[64]; for (int k = 0; k < 64; k++) junk[k] = (uint8_t)(0xA5 ^ k);
    a64m_load_raw(m, junk, sizeof junk, 0);
    st = a64m_run(m, 1000000, false);
    t = log_text(m);
    CHECK(st == A64M_FAULT && strstr(t, "double fault") != NULL && strstr(t, "last PCs") != NULL, "garbage image -> reported as double fault with PC history");
    free(t); a64m_destroy(m);

    /* 3b. 0.4.66: the kernel's message buffer (msgbuf) is found by its magic and its new text is mirrored into the guest log */
    m = a64m_create_system(1u << 20);
    {
        uint32_t *hd = (uint32_t *)a64m_ram_at(m, 0x40001000ull, 32); uint8_t *bf = a64m_ram_at(m, 0x40002000ull, 0x1000);
        uint64_t bc = 0x40002000ull; hd[0] = 0x063061; hd[1] = 0x1000; hd[2] = 0; hd[3] = 0; memcpy(hd + 4, &bc, 8);
        a64m_msgbuf_sync(m);
        char kb[256]; size_t kn = a64m_log_read_kind(m, 1, 0, kb, sizeof kb);
        CHECK(kn == 0, "msgbuf: empty buffer adds nothing to the Kernel log");
        memcpy(bf, "AppleKernel: hello\n", 19); hd[2] = 19; a64m_msgbuf_sync(m);
        kn = a64m_log_read_kind(m, 1, 0, kb, sizeof kb); kb[kn] = 0;
        CHECK(kn == 19 && strcmp(kb, "AppleKernel: hello\n") == 0, "msgbuf: new kernel text appears in the Kernel log");
        a64m_msgbuf_sync(m);
        CHECK(a64m_log_read_kind(m, 1, 0, kb, sizeof kb) == 19, "msgbuf: nothing is repeated");
        memcpy(bf + 19, "more\n", 5); hd[2] = 24; a64m_msgbuf_sync(m);
        kn = a64m_log_read_kind(m, 1, 0, kb, sizeof kb); kb[kn] = 0;
        CHECK(kn == 24 && strcmp(kb + 19, "more\n") == 0, "msgbuf: only the appended text is added");
        hd[2] = 3; bf[0] = 'X'; bf[1] = 'Y'; bf[2] = 'Z'; memset(bf + 24, 0, 8); a64m_msgbuf_sync(m);   /* wrap: tail of the ring (zeros skipped) then the start */
        kn = a64m_log_read_kind(m, 1, 0, kb, sizeof kb);
        CHECK(kn == 27, "msgbuf: wrap-around is followed (end of ring then start)");
    }
    a64m_destroy(m);
    m = a64m_create_system(1u << 20);
    { uint32_t *hd = (uint32_t *)a64m_ram_at(m, 0x40001000ull, 32); hd[0] = 0x063061; hd[1] = 0x10; hd[2] = 0; a64m_msgbuf_sync(m);
      char kb[64]; CHECK(a64m_log_read_kind(m, 1, 0, kb, sizeof kb) == 0, "msgbuf: an implausible header (size 16) is not accepted"); }
    a64m_destroy(m);

    /* 4. MMU: guest builds page tables, enables SCTLR_EL1.M and exercises translation (tests/asm/mmu.S) */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb"); uint8_t buf[16384]; size_t n = f ? fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
        m = a64m_create_system(16u << 20);
        a64m_load_raw(m, buf, n, 0);
        st = a64m_run(m, 2000000, false); t = log_text(m);
        CHECK(n > 0 && st == A64M_POWEROFF && a64m_exit_code(m) == 0, "MMU guest: translation, aliasing, page-crossing, fetch, faults, TLBI all pass");
        if (st != A64M_POWEROFF || a64m_exit_code(m) != 0) printf("      state=%d exit=%d log:\n%s\n", (int)st, a64m_exit_code(m), t);
        { /* boot_args.Video framebuffer (IPSW attempt): pixels written by the guest appear in a64m_frame, the counter advances when the content changes */
        A64Machine *vm = a64m_create_system(16u << 20);
        a64m_set_video(vm, 0x40100000ull, 270, 480, 1088);
        static uint32_t px[270 * 480]; int64_t f0 = a64m_frame(vm, px, 270, 480);
        CHECK(f0 >= 0 && px[0] == 0xFF000000u, "video: black framebuffer shown before the kernel draws");
        uint8_t *fbm = a64m_ram_at(vm, 0x40100000ull, 1088ull * 480); uint32_t red = 0x00FF1020u; memcpy(fbm + 5 * 1088 + 7 * 4, &red, 4);
        int64_t f1 = a64m_frame(vm, px, 270, 480);
        CHECK(f1 > f0 && px[5 * 270 + 7] == 0xFFFF1020u && px[0] == 0xFF000000u, "video: a drawn pixel appears (row stride honoured) and the frame counter advances");
        CHECK(a64m_frame(vm, px, 270, 480) == f1, "video: unchanged content keeps the frame counter");
        a64m_destroy(vm);
        { /* 0.4.51: the IPSW attempt's framebuffer size (540x960, rowBytes 2160) */
        A64Machine *vm2 = a64m_create_system(16u << 20);
        a64m_set_video(vm2, 0x40100000ull, A64M_VID_W, A64M_VID_H, A64M_VID_W * 4);
        static uint32_t px2[A64M_VID_W * A64M_VID_H]; int64_t g0 = a64m_frame(vm2, px2, A64M_VID_W, A64M_VID_H);
        CHECK(g0 >= 0 && px2[0] == 0xFF000000u, "video 540x960: black framebuffer shown");
        uint8_t *f2 = a64m_ram_at(vm2, 0x40100000ull, (uint64_t)A64M_VID_W * 4 * A64M_VID_H); uint32_t gr = 0x0000FF00u;
        memcpy(f2 + (size_t)(A64M_VID_H - 1) * A64M_VID_W * 4 + (A64M_VID_W - 1) * 4, &gr, 4);
        int64_t g1 = a64m_frame(vm2, px2, A64M_VID_W, A64M_VID_H);
        CHECK(g1 > g0 && px2[(size_t)A64M_VID_W * A64M_VID_H - 1] == 0xFF00FF00u, "video 540x960: last pixel (bottom-right) appears");
        CHECK(a64m_frame(vm2, px2, 270, 480) == -1, "video 540x960: a request with the old 270x480 size is refused");
        a64m_destroy(vm2); }
    }
    free(t); a64m_destroy(m);
    } else { printf("FAIL: mmu test binary not given\n"); fails++; }

    /* 5. idle: WFI with nothing to wake it */
    m = a64m_create_system(1u << 20);
    uint32_t wfi[] = { 0xD503207Fu, 0x14000000u };
    a64m_load_raw(m, (uint8_t *)wfi, sizeof wfi, 0);
    CHECK(a64m_run(m, 1000, false) == A64M_IDLE, "WFI with no wake source reports idle");
    a64m_destroy(m);

    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails != 0;
}
