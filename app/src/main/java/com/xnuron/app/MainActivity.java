package com.xnuron.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.text.TextUtils;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.view.Gravity;
import android.app.Dialog;
import android.view.ViewGroup;
import android.view.Window;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.widget.Toast;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;

/**
 * XNUron front end. One screen: pick a file, press Run, watch the display / log / info.
 * It starts only what the native core can really run, shows the real boot log and the real guest framebuffer,
 * and says plainly when a file cannot be run. It never draws an interface of its own inside the emulation view.
 */
public class MainActivity extends Activity {
    private static final int REQ_FILE = 1, REQ_SAVE = 2;
    private static final int MAX_LOG_CHARS = 1_100_000;    // keeps the whole native log (1 MiB) for Copy
    private static final int VIEW_TAIL_CHARS = 150_000;    // the text views only show the end of it (a huge TextView is slow)
    private static final long MAX_FILE_BYTES = 64L << 20;
    private static final int TAB_KERNEL = 0, TAB_EMU = 1;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Ui t;
    private Button run, choose;
    private final Button[] tabButtons = new Button[2];
    private FrameLayout runLayer;
    private LinearLayout logPanel;
    private TextView runStatus;
    private Button logsBtn, stopBtn;
    private TextView fileTitle, fileSub, status;
    private FrameLayout content;
    private ScrollView kernelScroll, emuScroll;
    private TextView kernelView, emuView;
    private EmuView emu;
    private int tab = TAB_KERNEL;

    private volatile long handle;
    private Thread emuThread;
    private final int[] kernelOffset = new int[1], emuOffset = new int[1];
    private long lastFrame = -1;
    private boolean autoSwitchedToDisplay;
    private final StringBuilder kernelText = new StringBuilder(), emuText = new StringBuilder();

    private Uri chosen;
    private String chosenName = "";
    private int chosenFlags;
    private String diagText = "Choose a file to see what it is and whether it can run.";

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        t = new Ui(this);
        t.systemBars(this);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(t.surface);
        root.setPadding(t.dp(20), t.dp(24), t.dp(20), t.dp(16));

        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = t.text(this, 36f, t.onSurface, true);
        title.setText("XNUron");
        head.addView(title, new LinearLayout.LayoutParams(0, -2, 1f));
        ImageView gear = new ImageView(this);
        gear.setImageResource(R.drawable.ic_settings);
        gear.setColorFilter(t.onPrimaryContainer);
        gear.setPadding(t.dp(12), t.dp(12), t.dp(12), t.dp(12));
        gear.setBackground(t.shape(t.primaryContainer, 28));
        gear.setContentDescription("Settings");
        gear.setOnClickListener(v -> showSettings());
        head.addView(gear, new LinearLayout.LayoutParams(t.dp(48), t.dp(48)));
        root.addView(head);

        // ---- source card
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(t.shape(t.primaryContainer, 28));
        card.setPadding(t.dp(20), t.dp(18), t.dp(20), t.dp(14));
        fileTitle = t.text(this, 20f, t.onPrimaryContainer, true);
        fileTitle.setSingleLine(true);
        fileTitle.setEllipsize(TextUtils.TruncateAt.MIDDLE);
        fileSub = t.text(this, 13f, t.onPrimaryContainer, false);
        status = t.text(this, 12f, t.onPrimaryContainer, false);
        status.setTypeface(Typeface.MONOSPACE);
        status.setPadding(0, t.dp(6), 0, 0);
        card.addView(fileTitle);
        card.addView(fileSub);
        card.addView(status);
        LinearLayout row = new LinearLayout(this);
        row.setPadding(0, t.dp(12), 0, 0);
        choose = t.pill(this, "Choose file", t.primary, t.onPrimary, 44, 14f);
        row.addView(choose, new LinearLayout.LayoutParams(-2, -2));
        card.addView(row);
        LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(-1, -2);
        cl.topMargin = t.dp(20);
        root.addView(card, cl);

        // ---- the one big action
        run = t.pill(this, "Start", t.primary, t.onPrimary, 64, 18f);
        LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(-1, t.dp(64));
        rl.topMargin = t.dp(12);
        root.addView(run, rl);

        // ---- full-screen run layer: the display, a side column of buttons, a log panel on demand
        emu = new EmuView(this);
        emu.clear("No display output");
        emu.setTouchListener((x, y, down) -> { long h = handle; if (h != 0) Native.touch(h, x, y, down); });
        kernelView = mono(); emuView = mono();
        kernelView.setHint("Nothing written by the kernel yet. This tab shows only what the guest itself prints.");
        kernelView.setHintTextColor(t.onSurfaceVariant);
        kernelScroll = new ScrollView(this); kernelScroll.addView(kernelView);
        emuScroll = new ScrollView(this); emuScroll.addView(emuView);

        runLayer = new FrameLayout(this);
        runLayer.setBackgroundColor(Color.BLACK);
        runLayer.setVisibility(View.GONE);
        runLayer.addView(emu, new FrameLayout.LayoutParams(-1, -1));
        runStatus = t.text(this, 11f, Color.WHITE, false);
        runStatus.setTypeface(Typeface.MONOSPACE);
        runStatus.setPadding(t.dp(12), t.dp(8), t.dp(12), t.dp(8));
        FrameLayout.LayoutParams sp = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START);
        runLayer.addView(runStatus, sp);
        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.HORIZONTAL);
        logsBtn = t.pill(this, "Logs", Color.argb(170, 30, 30, 30), Color.WHITE, 44, 14f);
        stopBtn = t.pill(this, "Stop", Color.argb(170, 120, 20, 20), Color.WHITE, 44, 14f);
        side.addView(logsBtn, new LinearLayout.LayoutParams(t.dp(84), -2));
        LinearLayout.LayoutParams sbl = new LinearLayout.LayoutParams(t.dp(84), -2);
        sbl.leftMargin = t.dp(8);
        side.addView(stopBtn, sbl);
        FrameLayout.LayoutParams sdl = new FrameLayout.LayoutParams(-2, -2, Gravity.END | Gravity.TOP);
        sdl.rightMargin = t.dp(8); sdl.topMargin = t.dp(8);
        runLayer.addView(side, sdl);

        logPanel = new LinearLayout(this);
        logPanel.setOrientation(LinearLayout.VERTICAL);
        logPanel.setBackgroundColor(t.surface);
        logPanel.setPadding(t.dp(12), t.dp(16), t.dp(12), t.dp(12));
        logPanel.setVisibility(View.GONE);
        LinearLayout lh = new LinearLayout(this);
        lh.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout seg = new LinearLayout(this);
        seg.setBackground(t.shape(t.container, 60));
        seg.setPadding(t.dp(4), t.dp(4), t.dp(4), t.dp(4));
        String[] names = { "Kernel", "Normal" };
        for (int i = 0; i < 2; i++) {
            final int idx = i;
            tabButtons[i] = t.pill(this, names[i], Color.TRANSPARENT, t.onSurfaceVariant, 40, 14f);
            tabButtons[i].setOnClickListener(v -> selectTab(idx));
            LinearLayout.LayoutParams l2 = new LinearLayout.LayoutParams(0, -2, 1f);
            if (i > 0) l2.leftMargin = t.dp(4);
            seg.addView(tabButtons[i], l2);
        }
        lh.addView(seg, new LinearLayout.LayoutParams(0, -2, 1f));
        ImageView copyLogs = iconButton(t, R.drawable.ic_copy, "Copy");      // 0.4.68: icons instead of words
        ImageView saveLogs = iconButton(t, R.drawable.ic_save, "Save");
        LinearLayout.LayoutParams cpl = new LinearLayout.LayoutParams(t.dp(44), t.dp(44));
        cpl.leftMargin = t.dp(8);
        lh.addView(copyLogs, cpl);
        LinearLayout.LayoutParams svl = new LinearLayout.LayoutParams(t.dp(44), t.dp(44));
        svl.leftMargin = t.dp(8);
        lh.addView(saveLogs, svl);
        copyLogs.setOnClickListener(v -> copyCurrentLog());
        saveLogs.setOnClickListener(v -> saveCurrentLog());
        Button closeLogs = t.pill(this, "Close", t.primary, t.onPrimary, 44, 14f);
        LinearLayout.LayoutParams cll = new LinearLayout.LayoutParams(-2, -2);
        cll.leftMargin = t.dp(8);
        lh.addView(closeLogs, cll);
        logPanel.addView(lh);
        content = new FrameLayout(this);
        content.setBackground(t.shape(t.container, 28));
        content.setPadding(t.dp(4), t.dp(4), t.dp(4), t.dp(4));
        content.setClipToOutline(true);
        LinearLayout.LayoutParams ctl = new LinearLayout.LayoutParams(-1, 0, 1f);
        ctl.topMargin = t.dp(8);
        logPanel.addView(content, ctl);
        runLayer.addView(logPanel, new FrameLayout.LayoutParams(-1, -1));
        logsBtn.setOnClickListener(v -> showLogs(true));
        closeLogs.setOnClickListener(v -> showLogs(false));
        stopBtn.setOnClickListener(v -> { if (handle != 0) stopEmulation(); else leaveRun(); });

        FrameLayout top = new FrameLayout(this);
        top.addView(root, new FrameLayout.LayoutParams(-1, -1));
        top.addView(runLayer, new FrameLayout.LayoutParams(-1, -1));
        setContentView(top);

        choose.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ_FILE);
        });
        run.setOnClickListener(v -> { if (handle != 0) stopEmulation(); else startEmulation(); });
        refreshSource();
        status.setText("ready");
        selectTab(TAB_KERNEL);
    }

    private TextView mono() {
        TextView v = t.text(this, 11f, t.onSurface, false);
        v.setTypeface(Typeface.MONOSPACE);
        v.setTextIsSelectable(true);
        v.setPadding(t.dp(14), t.dp(12), t.dp(14), t.dp(12));
        return v;
    }

    private void selectTab(int which) {
        tab = which;
        content.removeAllViews();
        View v = which == TAB_KERNEL ? kernelScroll : emuScroll;
        content.addView(v, new FrameLayout.LayoutParams(-1, -1));
        if (which == TAB_EMU) refreshEmuView();
        for (int i = 0; i < 2; i++) {
            boolean on = i == which;
            t.style(tabButtons[i], on ? t.containerHigh : Color.TRANSPARENT, on ? t.onSurface : t.onSurfaceVariant);
        }
    }

    private void showLogs(boolean on) {
        logPanel.setVisibility(on ? View.VISIBLE : View.GONE);
        if (on) {
            if (tab == TAB_KERNEL) kernelScroll.post(() -> kernelScroll.fullScroll(View.FOCUS_DOWN));
            else emuScroll.post(() -> emuScroll.fullScroll(View.FOCUS_DOWN));
        }
    }

    private void enterRun() {
        runLayer.setVisibility(View.VISIBLE);
        showLogs(false);
        stopBtn.setText("Stop");
        runStatus.setText("starting...");
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private void leaveRun() {
        runLayer.setVisibility(View.GONE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_VISIBLE);
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override public void onBackPressed() {
        if (runLayer.getVisibility() == View.VISIBLE) {
            if (logPanel.getVisibility() == View.VISIBLE) showLogs(false);
            else if (handle != 0) stopEmulation();
            else leaveRun();
            return;
        }
        super.onBackPressed();
    }

    /** Emulator tab: emulator messages once a run produced some, otherwise the file inspection report. */
    private void refreshEmuView() { emuView.setText(emuText.length() > 0 ? tail(emuText) : diagText); }

    /** The last VIEW_TAIL_CHARS characters (the views are not meant to hold the whole log; Copy uses the full text). */
    private static CharSequence tail(StringBuilder sb) { return sb.length() <= VIEW_TAIL_CHARS ? sb : sb.substring(sb.length() - VIEW_TAIL_CHARS); }

    private ImageView iconButton(Ui t, int res, String desc) {
        ImageView b = new ImageView(this);
        b.setImageResource(res); b.setColorFilter(t.onSurface);
        b.setPadding(t.dp(10), t.dp(10), t.dp(10), t.dp(10));
        b.setBackground(t.shape(t.containerHigh, 22));
        b.setContentDescription(desc);
        return b;
    }

    /** The whole text of the log tab that is currently shown (Kernel or Normal). */
    private String currentLogText() {
        CharSequence txt = tab == TAB_KERNEL ? kernelText : (emuText.length() > 0 ? emuText : diagText);
        return txt == null ? "" : txt.toString();
    }
    private String currentLogName() { return tab == TAB_KERNEL ? "kernel" : "normal"; }

    /** 0.4.68: copies the whole log of the shown tab. The clipboard cannot hold much more than about 1 MB, so a log that is too big says so and points to Save. */
    private void copyCurrentLog() {
        String all = currentLogText();
        if (all.isEmpty()) { Toast.makeText(this, "Лог пуст", Toast.LENGTH_SHORT).show(); return; }
        try {
            ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("XNUron " + currentLogName() + " log", all));
        } catch (RuntimeException e) {
            Toast.makeText(this, "Не удалось скопировать (слишком большой лог?): " + e.getMessage() + ". Используй Save.", Toast.LENGTH_LONG).show();
        }
    }

    /** 0.4.68: Save = the whole log of the shown tab as a UTF-8 .txt file, written where the system file dialog says. */
    private String pendingSaveText;
    private void saveCurrentLog() {
        String all = currentLogText();
        if (all.isEmpty()) { Toast.makeText(this, "Лог пуст", Toast.LENGTH_SHORT).show(); return; }
        pendingSaveText = all;
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_TITLE, "xnuron-" + currentLogName() + "-log.txt");
        try { startActivityForResult(i, REQ_SAVE); }
        catch (RuntimeException e) { pendingSaveText = null; Toast.makeText(this, "Не удалось открыть диалог сохранения: " + e.getMessage(), Toast.LENGTH_LONG).show(); }
    }
    private void writeSavedLog(Uri uri) {
        String txt = pendingSaveText; pendingSaveText = null;
        if (txt == null) return;
        try (java.io.OutputStream os = getContentResolver().openOutputStream(uri, "wt")) {
            if (os == null) throw new java.io.IOException("no output stream");
            os.write(txt.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException | RuntimeException e) {
            Toast.makeText(this, "Не удалось сохранить: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void setDiag(String s) { diagText = s; emuText.setLength(0); refreshEmuView(); }

    /** Which native source the current selection means (-1 none, 3 Mach-O, 4 raw, 5 IPSW attempt). */
    private int effectiveSource() {
        if (chosen == null) return -1;
        if ((chosenFlags & Native.FLAG_RUNNABLE_USER) != 0) return 3;
        if (chosenFlags >> 2 == 5) return 5;
        return 4;
    }

    private void refreshSource() {
        int src = effectiveSource();
        if (chosen == null) {
            fileTitle.setText("No file chosen");
            fileSub.setText("Choose an IPSW, a Mach-O arm64 program or a raw AArch64 image.");
        } else {
            fileTitle.setText(chosenName);
            String k = src == 3 ? "Mach-O arm64 program - runs in user mode (not a full iOS)"
                     : src == 5 ? "IPSW firmware - Start will UNPACK THE KERNEL AND TRY to boot it (experimental, expected to stop early)"
                     : chosenFlags == 0 ? "Unknown file - Start will try it as a raw AArch64 image"
                     : (chosenFlags >> 2) == 8 ? "Raw AArch64 image - runs on the virtual machine"
                     : "Cannot be run: see the Normal log for the exact reason";
            fileSub.setText(k);
        }
    }

    // ---------------------------------------------------------------- file inspection
    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_SAVE) { if (res == RESULT_OK && data != null && data.getData() != null) writeSavedLog(data.getData()); else pendingSaveText = null; return; }
        if (req != REQ_FILE || res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        chosen = uri; chosenName = displayName(uri); chosenFlags = 0;
        refreshSource();
        setDiag("Inspecting " + chosenName + " ...");
        new Thread(() -> {
            String rep; int[] fl = new int[1];
            try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                rep = Native.inspectFd(pfd.getFd(), pfd.getStatSize(), fl);
            } catch (Exception e) {
                rep = "Could not open the file: " + e;
            }
            final String text = "File: " + chosenName + "\n\n" + rep; final int flags = fl[0];
            ui.post(() -> {
                chosenFlags = flags;
                setDiag(text);
                refreshSource();
            });
        }, "inspect").start();
    }

    private String displayName(Uri u) {
        try (android.database.Cursor c = getContentResolver().query(u, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (i >= 0) return c.getString(i);
            }
        } catch (Exception ignored) { }
        return String.valueOf(u.getLastPathSegment());
    }

    private byte[] readChosen() throws Exception {
        try (InputStream in = getContentResolver().openInputStream(chosen)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16]; int n; long total = 0;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > MAX_FILE_BYTES) throw new Exception("file is larger than " + (MAX_FILE_BYTES >> 20) + " MiB");
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }

    // ---------------------------------------------------------------- run control
    private void startEmulation() {
        if (handle != 0) return;
        final int src = effectiveSource();
        kernelText.setLength(0); emuText.setLength(0); kernelOffset[0] = 0; emuOffset[0] = 0;
        kernelView.setText(""); refreshEmuView(); lastFrame = -1;
        { int[] rs = Ui.RES[Ui.resolution(this)]; Native.setVideoSize(rs[0], rs[1]); emu.resize(Native.fbWidth(), Native.fbHeight()); }   // 0.4.85: resolution setting, applies from this start
        emu.clear("No display output");
        run.setEnabled(false);
        autoSwitchedToDisplay = false;
        enterRun();
        status.setText("starting...");
        new Thread(() -> {
            long h = 0; String why = null, name = "";
            try {
                if (chosen == null) why = "No file chosen.";
                else if (src == 5) {
                    try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(chosen, "r")) {
                        h = Native.createIpswAttempt(pfd.getFd(), pfd.getStatSize());
                    }
                    if (h == 0) why = "Out of memory while preparing the attempt.";
                    name = chosenName + " (EXPERIMENTAL IPSW boot attempt - NOT iOS)";
                } else if (src == 3) {
                    if ((chosenFlags & Native.FLAG_RUNNABLE_USER) == 0)
                        why = "The inspected file is not a runnable user-mode Mach-O. See the Emulator tab for the specific reason.";
                    else {
                        String[] err = new String[1];
                        h = Native.createMachO(readChosen(), err);
                        if (h == 0) why = err[0];
                        name = chosenName + " (Mach-O, user mode; not a full iOS)";
                    }
                } else if (chosenFlags != 0 && (chosenFlags >> 2) != 8) {
                    why = "The inspector identified this file as something that is not AArch64 code (32-bit ARM, an Apple container, an archive...). "
                        + "Starting it as a raw image would only execute garbage. See the Emulator tab.";
                } else {
                    h = Native.createRaw(readChosen());
                    if (h == 0) why = "Could not load the file as a raw image (empty or larger than 12 MiB).";
                    name = chosenName + " (raw AArch64 image on a64virt)";
                }
            } catch (Exception e) { why = String.valueOf(e); }
            final long fh = h; final String fwhy = why, fname = name;
            ui.post(() -> {
                if (fh == 0) {
                    run.setEnabled(true);
                    status.setText("not started");
                    leaveRun();
                    setDiag("Cannot start: " + (fwhy == null ? "unknown error" : fwhy) + "\n\n" + diagText);
                    new AlertDialog.Builder(this).setTitle("Cannot start").setMessage(fwhy == null ? "unknown error" : fwhy).setPositiveButton("OK", null).show();
                    return;
                }
                handle = fh;
                fileSub.setText(fname);
                setRunning(true);
                emuThread = new Thread(() -> {
                    while (true) {
                        int st = Native.run(fh, 200_000);
                        if (st == Native.POWEROFF || st == Native.FAULT || st == Native.STOPPED) break;
                    }
                }, "emulator");
                emuThread.start();
                ui.post(poll);
            });
        }, "create").start();
    }

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            long h = handle;
            if (h == 0) return;
            pollOnce(h);
            int st = Native.state(h);
            if (st == Native.POWEROFF || st == Native.FAULT || st == Native.STOPPED) { finishRun(h, st); return; }
            ui.postDelayed(this, 40);
        }
    };

    private void pollOnce(long h) {
        long f = Native.frame(h, emu.pixels());
        if (f >= 0 && f != lastFrame) {
            lastFrame = f; emu.frameReady();
            if (f > 0 && !autoSwitchedToDisplay) { autoSwitchedToDisplay = true; }   // the guest started drawing (0 = the still-black boot framebuffer of an IPSW attempt)
        }
        // drain everything that is available: one native read returns at most 256 KiB, and after a stop the whole report arrives at once
        boolean gotK = false, gotE = false;
        for (int guard = 0; guard < 64; guard++) {
            String k = Native.readLogKind(h, 1, kernelOffset);
            if (k.isEmpty()) break;
            kernelText.append(k); gotK = true;
        }
        if (kernelText.length() > MAX_LOG_CHARS) kernelText.delete(0, kernelText.length() - MAX_LOG_CHARS);
        if (gotK) {
            kernelView.setText(tail(kernelText));
            if (tab == TAB_KERNEL) kernelScroll.post(() -> kernelScroll.fullScroll(View.FOCUS_DOWN));
        }
        for (int guard = 0; guard < 64; guard++) {
            String e = Native.readLogKind(h, 2, emuOffset);
            if (e.isEmpty()) break;
            emuText.append(e); gotE = true;
        }
        if (emuText.length() > MAX_LOG_CHARS) emuText.delete(0, emuText.length() - MAX_LOG_CHARS);
        if (gotE) { emuView.setText(tail(emuText)); if (tab == TAB_EMU) emuScroll.post(() -> emuScroll.fullScroll(View.FOCUS_DOWN)); }
        String stt = stateName(Native.state(h)) + " - " + Native.steps(h) + " instructions";
        status.setText(stt); runStatus.setText(stt);
    }

    private void finishRun(long h, int st) {
        try { if (emuThread != null) emuThread.join(2000); } catch (InterruptedException ignored) { }
        pollOnce(h);
        if (kernelText.length() == 0 && lastFrame < 0) { selectTab(TAB_EMU); showLogs(true); }     // nothing from the kernel: show why in the normal log
        status.setText(stateName(st) + (st == Native.POWEROFF ? " (exit code " + Native.exitCode(h) + ")" : "")
                + " - " + Native.steps(h) + " instructions");
        runStatus.setText(status.getText());
        stopBtn.setText("Close");
        handle = 0;
        Native.destroy(h);
        setRunning(false);
        refreshSource();
    }

    private void setRunning(boolean on) {
        run.setEnabled(true);
        run.setText("Start");
        choose.setEnabled(!on);
        choose.setAlpha(on ? 0.4f : 1f);
    }

    private void stopEmulation() {
        long h = handle;
        if (h != 0) Native.stop(h);
    }

    private static String stateName(int s) {
        switch (s) {
            case Native.RUNNING: return "running";
            case Native.IDLE: return "idle (WFI)";
            case Native.POWEROFF: return "guest finished";
            case Native.FAULT: return "emulator fault";
            case Native.STOPPED: return "stopped";
            default: return "created";
        }
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacks(poll);
        long h = handle;
        if (h != 0) {
            Native.stop(h);
            try { if (emuThread != null) emuThread.join(2000); } catch (InterruptedException ignored) { }
            handle = 0;
            Native.destroy(h);
        }
    }

    // ---- settings sheet: theme (system / light / dark) and colours (XNUron / dynamic / classic)
    private void showSettings() {
        Dialog dlg = new Dialog(this);
        dlg.requestWindowFeature(Window.FEATURE_NO_TITLE);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(t.dp(24), t.dp(24), t.dp(24), t.dp(28));
        float r = t.dp(32);
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(t.surface);
        bg.setCornerRadii(new float[] { r, r, r, r, r, r, r, r });
        box.setBackground(bg);

        TextView h = t.text(this, 24f, t.onSurface, true);
        h.setText("Settings");
        box.addView(h);

        box.addView(label("Theme"));
        box.addView(choices(new String[] { "System", "Light", "Dark" }, Ui.themeMode(this), i -> {
            Ui.save(this, "theme", i);
            dlg.dismiss();
            recreate();
        }));

        box.addView(label("Screen resolution"));
        box.addView(choices(new String[] { "270\u00d7480", "540\u00d7960", "1080\u00d71920" }, Ui.resolution(this), i -> {
            Ui.save(this, "res", i);
            dlg.dismiss();
        }));
        TextView resNote = t.text(this, 12f, t.onSurfaceVariant, false);
        resNote.setText("Size of the framebuffer handed to the guest in boot_args (MODELLED; a real iPhone XS panel is 1125\u00d72436). Applies to the next Start. Bigger = sharper but slower.");
        resNote.setPadding(t.dp(4), t.dp(8), t.dp(4), 0);
        box.addView(resNote);

        box.addView(label("Colours"));
        final boolean canDyn = Build.VERSION.SDK_INT >= 31;
        int curS = Ui.scheme(this);
        if (curS == 1 && !canDyn) curS = 0;
        box.addView(choices(new String[] { "XNUron", "Dynamic", "Classic" }, curS, i -> {
            if (i == 1 && !canDyn) return;
            Ui.save(this, "scheme", i);
            dlg.dismiss();
            recreate();
        }));
        TextView note = t.text(this, 12f, t.onSurfaceVariant, false);
        note.setText("XNUron: the red and white brand theme. Dynamic: colours from your wallpaper (Material You, Android 12+" + (canDyn ? "" : "; not available on this phone") + "). Classic: plain black and white, no colour.");
        note.setPadding(t.dp(4), t.dp(8), t.dp(4), 0);
        box.addView(note);

        dlg.setContentView(box);
        Window w = dlg.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            w.setGravity(Gravity.BOTTOM);
        }
        dlg.show();
    }

    private TextView label(String s) {
        TextView l = t.text(this, 13f, t.primary, true);
        l.setText(s);
        l.setPadding(t.dp(4), t.dp(20), 0, t.dp(8));
        return l;
    }

    private interface Pick { void on(int i); }

    private LinearLayout choices(String[] names, int selected, Pick pick) {
        LinearLayout row = new LinearLayout(this);
        row.setBackground(t.shape(t.container, 60));
        row.setPadding(t.dp(4), t.dp(4), t.dp(4), t.dp(4));
        for (int i = 0; i < names.length; i++) {
            final int idx = i;
            boolean on = i == selected;
            Button b = t.pill(this, names[i], on ? t.primary : Color.TRANSPARENT, on ? t.onPrimary : t.onSurfaceVariant, 44, 14f);
            b.setOnClickListener(v -> pick.on(idx));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, -2, 1f);
            if (i > 0) lp.leftMargin = t.dp(4);
            row.addView(b, lp);
        }
        return row;
    }
}
