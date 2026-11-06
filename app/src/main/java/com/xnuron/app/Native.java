package com.xnuron.app;

/** JNI bridge to the C emulator core. Handles are opaque machine pointers (0 = none). */
public final class Native {
    static { System.loadLibrary("a64"); }

    /** A64MState in machine.h */
    public static final int CREATED = 0, RUNNING = 1, IDLE = 2, POWEROFF = 3, FAULT = 4, STOPPED = 5;
    /** Inspect flag bits: bit0 runnable as user-mode program, bit1 bootable as system image. */
    public static final int FLAG_RUNNABLE_USER = 1, FLAG_BOOTABLE_SYSTEM = 2;

    public static native String runDemo();

    /** mode 0: bundled test kernel, self-test then power off; mode 1: stays interactive (display+touch). */
    public static native long createGuest(int mode);
    /** Maps a Mach-O arm64 executable into a user-mode machine; returns 0 and sets errOut[0] on refusal. */
    public static native long createMachO(byte[] image, String[] errOut);
    /** Starts an arbitrary file as a raw AArch64 image on the a64virt machine (max 12 MiB). */
    public static native long createRaw(byte[] image);
    /** Experimental IPSW boot attempt; always returns a machine (its log explains where it stopped). */
    public static native long createIpswAttempt(int fd, long size);
    public static native void destroy(long h);
    public static native void stop(long h);
    /** Runs up to {@code steps} instructions on the calling thread; returns the machine state. */
    public static native int run(long h, int steps);
    public static native int state(long h);
    public static native int exitCode(long h);
    public static native long steps(long h);
    /** kind 0 = everything, 1 = only what the guest/kernel itself wrote, 2 = only emulator messages. Advances offset[0]. */
    public static native String readLogKind(long h, int kind, int[] offset);
    /** Returns log text after offset[0] and advances offset[0]. */
    public static native String readLog(long h, int[] offset);
    /** Fills pixels (ARGB, fbWidth*fbHeight); returns frame counter or -1 if the display is off. */
    public static native long frame(long h, int[] pixels);
    public static native int fbWidth();
    public static native int fbHeight();
    public static native void touch(long h, int x, int y, boolean down);

    public static native String inspect(byte[] data, int[] flags);
    public static native String inspectFd(int fd, long size, int[] flags);
}
