import com.xnuron.app.Native;
import java.nio.file.Files;
import java.nio.file.Paths;

/** Plain-JVM smoke test of the real JNI glue (same native library the APK uses). */
public class JniSmoke {
    static int fails;
    static void check(boolean ok, String msg) { System.out.println((ok ? "ok:   " : "FAIL: ") + msg); if (!ok) fails++; }

    public static void main(String[] a) throws Exception {
        check(Native.runDemo().contains("sum(1..100) = 5050"), "runDemo (v0.1 demo) still works");

        long h = Native.createGuest(0);
        check(h != 0, "createGuest(self-test)");
        int st;
        do { st = Native.run(h, 1_000_000); } while (st != Native.POWEROFF && st != Native.FAULT && st != Native.STOPPED);
        int[] off = new int[1];
        String log = Native.readLog(h, off);
        check(st == Native.POWEROFF && Native.exitCode(h) == 0, "self-test guest powers off with exit code 0");
        check(log.contains("[test] summary: 0 failure(s)") && off[0] > 0, "readLog returns the guest's boot log and advances the offset");
        check(Native.readLog(h, off).isEmpty(), "second readLog returns nothing new");
        String kl = Native.readLogKind(h, 1, new int[1]), el = Native.readLogKind(h, 2, new int[1]);
        check(kl.contains("[test] summary") && !kl.contains("[emu]"), "kernel stream holds only what the guest wrote");
        check(el.contains("[emu] a64virt") && !el.contains("[test] summary"), "emulator stream holds only emulator messages");
        Native.destroy(h);

        h = Native.createGuest(1);
        final long hh = h;
        Thread t = new Thread(() -> { while (true) { int s = Native.run(hh, 100_000); if (s >= Native.POWEROFF) break; } });
        t.start();
        int[] px = new int[Native.fbWidth() * Native.fbHeight()];
        long f = -1; long end = System.currentTimeMillis() + 10000;
        while (f < 1 && System.currentTimeMillis() < end) { f = Native.frame(h, px); Thread.sleep(20); }
        check(f >= 1, "interactive guest produces a frame (real-time run on another thread)");
        Native.touch(h, 120, 240, true);
        end = System.currentTimeMillis() + 10000;
        while (px[240 * Native.fbWidth() + 120] != 0xFFFFD040 && System.currentTimeMillis() < end) { Native.frame(h, px); Thread.sleep(20); }
        check(px[240 * Native.fbWidth() + 120] == 0xFFFFD040, "touch injected through JNI is drawn by the guest");
        Native.stop(h); t.join(5000);
        check(!t.isAlive() && Native.state(h) == Native.STOPPED, "stop() ends the emulation thread");
        Native.destroy(h);

        byte[] mo = Files.readAllBytes(Paths.get(a[0]));
        int[] fl = new int[1];
        String rep = Native.inspect(mo, fl);
        check((fl[0] & Native.FLAG_RUNNABLE_USER) != 0 && (fl[0] & Native.FLAG_BOOTABLE_SYSTEM) == 0 && rep.contains("arm64"), "inspect(Mach-O): runnable in user mode only");
        String[] err = new String[1];
        h = Native.createMachO(mo, err);
        check(h != 0, "createMachO");
        while (Native.run(h, 100000) < Native.POWEROFF) { }
        check(Native.exitCode(h) == 7 && Native.readLog(h, new int[1]).contains("hello from a Mach-O arm64"), "Mach-O ran: exit 7 and output in log");
        Native.destroy(h);
        byte[] bad = mo.clone(); bad[4] = 7;
        check(Native.createMachO(bad, err) == 0 && err[0].contains("not arm64"), "refused Mach-O returns reason through errOut");

        check(Native.inspect(new byte[] {1, 2, 3}, fl).contains("nothing is known"), "inspect(unknown) is honest");
        h = Native.createRaw(new byte[] { 0x1f, 0x20, 0x03, (byte) 0xd5, 0, 0, 0, 0x14 });
        check(h != 0, "createRaw");
        Native.destroy(h);
        // IPSW attempt through a real file descriptor (a[2] = fixture zip whose kernelcache is a synthetic arm64 kernel)
        if (a.length > 2) {
            try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(a[2], "r")) {
                java.lang.reflect.Field ff = raf.getFD().getClass().getDeclaredField("fd");
                ff.setAccessible(true);
                h = Native.createIpswAttempt(ff.getInt(raf.getFD()), raf.length());
            }
            check(h != 0, "createIpswAttempt returns a machine");
            while (Native.run(h, 100000) < Native.POWEROFF) { }
            String lg = Native.readLog(h, new int[1]);
            check(Native.state(h) == Native.FAULT && lg.contains("test-kernel: started at EL1") && lg.contains("MMU is on: VBAR_EL1 address does not translate") && lg.contains("NOT iOS booting"),
                  "IPSW attempt really executed the unpacked kernel and reported the real stop reason");
            Native.destroy(h);
        }
        System.out.println(fails == 0 ? "\nALL PASSED" : "\n" + fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
