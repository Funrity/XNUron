#include <jni.h>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "arm64.h"
#include "a64demo.h"
#include "machine.h"
#include "macho.h"
#include "inspect.h"
#include "attempt.h"
#include "guest_image.h"

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_xnuron_app_Native_##name

static void sink(void *ud, const char *b, size_t n) { static_cast<std::string *>(ud)->append(b, n); }

/* Original v0.1 demo, kept: raw user-mode interpreter run. */
JNI(jstring, runDemo)(JNIEnv *env, jclass) {
    std::vector<uint8_t> mem(0x10000, 0);
    A64 cpu;
    a64_init(&cpu, mem.data(), 0, mem.size());
    std::string out;
    cpu.out = sink; cpu.ud = &out;
    a64_demo_load(&cpu);

    auto t0 = std::chrono::steady_clock::now();
    int rc = a64_run(&cpu, 10000000);
    auto t1 = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    char buf[256];
    snprintf(buf, sizeof buf, "\n--- rc=%d exit=%d steps=%llu time=%.1f us ---\n%s",
             rc, cpu.exit_code, (unsigned long long)cpu.steps, us, cpu.fault ? cpu.err : "");
    out += buf;
    return env->NewStringUTF(out.c_str());
}

/* mode 0 = test guest, self-test then power off; 1 = test guest, interactive display */
JNI(jlong, createGuest)(JNIEnv *, jclass, jint mode) {
    A64Machine *m = a64m_create_system(16u << 20);
    if (!m) return 0;
    a64m_load_raw(m, a64_guest_image, a64_guest_image_size, 0);
    a64m_set_boot_arg(m, mode == 1 ? 1 : 0);
    a64m_log(m, "[emu] a64virt: 16 MiB RAM at 0x40000000, UART, 270x480 display, touch, syscon\n");
    a64m_log(m, "[emu] loading bundled A64 test guest kernel (%zu bytes). It is our own test code, NOT iOS.\n", a64_guest_image_size);
    return (jlong)(intptr_t)m;
}

/* Returns a handle, or 0 with a reason in errOut[0]. */
JNI(jlong, createMachO)(JNIEnv *env, jclass, jbyteArray image, jobjectArray errOut) {
    jsize n = env->GetArrayLength(image);
    std::vector<uint8_t> buf((size_t)n);
    env->GetByteArrayRegion(image, 0, n, reinterpret_cast<jbyte *>(buf.data()));
    char err[512] = {0};
    A64Machine *m = a64_macho_create_machine(buf.data(), buf.size(), err, sizeof err);
    if (!m) {
        if (errOut && env->GetArrayLength(errOut) > 0) env->SetObjectArrayElement(errOut, 0, env->NewStringUTF(err));
        return 0;
    }
    a64m_log(m, "[emu] Mach-O arm64 mapped; running in USER mode with XNU/BSD syscall emulation (write, exit, getpid only).\n");
    a64m_log(m, "[emu] This is not iOS: no dyld, no libSystem, no kernel.\n");
    return (jlong)(intptr_t)m;
}

/* Unknown file started as a raw AArch64 image on a64virt (loaded at RAM base, boot arg 0). */
JNI(jlong, createRaw)(JNIEnv *env, jclass, jbyteArray image) {
    jsize n = env->GetArrayLength(image);
    if (n <= 0 || n > (12 << 20)) return 0;
    std::vector<uint8_t> buf((size_t)n);
    env->GetByteArrayRegion(image, 0, n, reinterpret_cast<jbyte *>(buf.data()));
    A64Machine *m = a64m_create_system(16u << 20);
    if (!m) return 0;
    a64m_load_raw(m, buf.data(), buf.size(), 0);
    a64m_mark_raw(m);
    a64m_log(m, "[emu] a64virt: raw image of %zu bytes loaded at 0x40000000, entering at EL1. Nothing is known about this image.\n", buf.size());
    return (jlong)(intptr_t)m;
}

/* Experimental: tries to boot the kernelcache inside an IPSW (zip) given as a file descriptor.
 * Always returns a machine whose log tells what happened; it may already be in the FAULT state. */
static int fd_read(void *ud, uint64_t off, void *buf, size_t len);
JNI(jlong, createIpswAttempt)(JNIEnv *, jclass, jint fd, jlong size) {
    int f = fd;
    return (jlong)(intptr_t)a64_attempt_ipsw(fd_read, &f, (uint64_t)size);
}

JNI(void, destroy)(JNIEnv *, jclass, jlong h) { a64m_destroy(reinterpret_cast<A64Machine *>(h)); }
JNI(void, stop)(JNIEnv *, jclass, jlong h) { a64m_request_stop(reinterpret_cast<A64Machine *>(h)); }
/* Runs on the caller's (emulation) thread. Returns A64MState. */
JNI(jint, run)(JNIEnv *, jclass, jlong h, jint steps) {
    return (jint)a64m_run(reinterpret_cast<A64Machine *>(h), (uint64_t)steps, true);
}
JNI(jint, state)(JNIEnv *, jclass, jlong h) { return (jint)a64m_state(reinterpret_cast<A64Machine *>(h)); }
JNI(jint, exitCode)(JNIEnv *, jclass, jlong h) { return a64m_exit_code(reinterpret_cast<A64Machine *>(h)); }
JNI(jlong, steps)(JNIEnv *, jclass, jlong h) { return (jlong)a64m_cpu(reinterpret_cast<A64Machine *>(h))->steps; }

/* Appends new log text of one stream (kind 0 all, 1 guest/kernel output, 2 emulator messages);
 * offset[0] is the byte offset already consumed in that stream and is advanced. */
JNI(jstring, readLogKind)(JNIEnv *env, jclass, jlong h, jint kind, jintArray offset) {
    A64Machine *m = reinterpret_cast<A64Machine *>(h);
    jint off; env->GetIntArrayRegion(offset, 0, 1, &off);
    std::vector<char> b(256 * 1024);
    size_t n = a64m_log_read_kind(m, kind, (size_t)off, b.data(), b.size());
    std::string s;                                  /* keep the Java string valid: guest text is ASCII */
    for (size_t k = 0; k < n; k++) s += ((unsigned char)b[k] < 0x80 || b[k] == '\n') ? b[k] : '?';
    jint no = off + (jint)n; env->SetIntArrayRegion(offset, 0, 1, &no);
    return env->NewStringUTF(s.c_str());
}
JNI(jstring, readLog)(JNIEnv *env, jclass c, jlong h, jintArray offset) { return Java_com_xnuron_app_Native_readLogKind(env, c, h, 0, offset); }

/* Copies the guest framebuffer into pixels[w*h] (ARGB), w x h = the video size setting (default 540x960). Returns frame counter or -1.
   0.4.51: the IPSW attempt's video is w x h; the demo machine's 270x480 display is scaled to it (nearest neighbour). 0.4.85: the size is a setting. */
JNI(jlong, frame)(JNIEnv *env, jclass, jlong h, jintArray pixels) {
    A64Machine *m = reinterpret_cast<A64Machine *>(h);
    const int W = a64m_video_w(), H = a64m_video_h();
    if (env->GetArrayLength(pixels) < W * H) return -1;
    static std::vector<uint32_t> tmp, small;
    tmp.resize((size_t)W * H);
    int64_t f = a64m_frame(m, tmp.data(), W, H);
    if (f < 0) {
        small.resize((size_t)A64M_FB_W * A64M_FB_H);
        f = a64m_frame(m, small.data(), A64M_FB_W, A64M_FB_H);
        if (f >= 0)
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    tmp[(size_t)y * W + x] = small[(size_t)((int64_t)y * A64M_FB_H / H) * A64M_FB_W + (size_t)((int64_t)x * A64M_FB_W / W)];
    }
    if (f >= 0) env->SetIntArrayRegion(pixels, 0, W * H, reinterpret_cast<jint *>(tmp.data()));
    return (jlong)f;
}
JNI(jint, fbWidth)(JNIEnv *, jclass) { return a64m_video_w(); }
JNI(jint, fbHeight)(JNIEnv *, jclass) { return a64m_video_h(); }
JNI(void, setVideoSize)(JNIEnv *, jclass, jint w, jint h) { a64m_set_video_size(w, h); }

JNI(void, touch)(JNIEnv *, jclass, jlong h, jint x, jint y, jboolean down) {
    a64m_touch(reinterpret_cast<A64Machine *>(h), (int)((int64_t)x * A64M_FB_W / a64m_video_w()), (int)((int64_t)y * A64M_FB_H / a64m_video_h()), down);   /* the demo touch device is 270x480 */
}

static std::string finish_inspect(A64Inspect &r, const char *rep, jint *flags) {
    if (flags) *flags = (jint)((r.kind << 2) | (r.runnable_user ? 1 : 0) | (r.bootable_system ? 2 : 0));
    return rep;
}

/* flags[0] = kind << 2 | runnable_user | bootable_system << 1 */
JNI(jstring, inspect)(JNIEnv *env, jclass, jbyteArray data, jintArray flags) {
    jsize n = env->GetArrayLength(data);
    std::vector<uint8_t> buf((size_t)n);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte *>(buf.data()));
    std::vector<char> rep(64 * 1024);
    A64Inspect r; a64_inspect_buffer(buf.data(), buf.size(), &r, rep.data(), rep.size());
    jint f = 0; std::string s = finish_inspect(r, rep.data(), &f);
    if (flags) env->SetIntArrayRegion(flags, 0, 1, &f);
    return env->NewStringUTF(s.c_str());
}

static int fd_read(void *ud, uint64_t off, void *buf, size_t len) {
    int fd = *static_cast<int *>(ud); uint8_t *p = static_cast<uint8_t *>(buf);
    while (len) {
        ssize_t k = pread(fd, p, len, (off_t)off);
        if (k <= 0) return -1;
        p += k; off += (uint64_t)k; len -= (size_t)k;
    }
    return 0;
}
/* Inspects a file through a file descriptor (random access, works for multi-GB IPSW archives). */
JNI(jstring, inspectFd)(JNIEnv *env, jclass, jint fd, jlong size, jintArray flags) {
    std::vector<char> rep(64 * 1024);
    A64Inspect r; int f = fd;
    a64_inspect_source(fd_read, &f, (uint64_t)size, &r, rep.data(), rep.size());
    jint fl = 0; std::string s = finish_inspect(r, rep.data(), &fl);
    if (flags) env->SetIntArrayRegion(flags, 0, 1, &fl);
    return env->NewStringUTF(s.c_str());
}
