package com.xnuron.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.view.MotionEvent;
import android.view.View;

/** Shows the guest framebuffer exactly as the guest wrote it, or an honest "no display" notice. */
public class EmuView extends View {
    public interface TouchListener { void onTouch(int x, int y, boolean down); }

    private Bitmap bmp;
    private int[] pixels;
    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Rect dst = new Rect();
    private boolean hasFrame;
    private String notice = "No display output";
    private TouchListener listener;

    public EmuView(Context c) {
        super(c);
        int w = Native.fbWidth(), h = Native.fbHeight();
        pixels = new int[w * h];
        bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        text.setColor(Color.LTGRAY);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTextSize(36f);
        setBackgroundColor(Color.BLACK);
    }

    public int[] pixels() { return pixels; }
    /** 0.4.85: the framebuffer size is a setting; call before the next run. */
    public void resize(int w, int h) {
        if (bmp != null && bmp.getWidth() == w && bmp.getHeight() == h) return;
        pixels = new int[w * h];
        bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        hasFrame = false;
    }
    public void setTouchListener(TouchListener l) { listener = l; }

    /** Called with a fresh frame in pixels(). */
    public void frameReady() {
        bmp.setPixels(pixels, 0, bmp.getWidth(), 0, 0, bmp.getWidth(), bmp.getHeight());
        hasFrame = true;
        invalidate();
    }
    public void clear(String message) { hasFrame = false; notice = message; invalidate(); }

    private void layoutRect() {
        float s = Math.min(getWidth() / (float) bmp.getWidth(), getHeight() / (float) bmp.getHeight());
        int w = (int) (bmp.getWidth() * s), h = (int) (bmp.getHeight() * s);
        int l = (getWidth() - w) / 2, t = (getHeight() - h) / 2;
        dst.set(l, t, l + w, t + h);
    }

    @Override protected void onDraw(Canvas c) {
        layoutRect();
        if (hasFrame) c.drawBitmap(bmp, null, dst, paint);
        else c.drawText(notice, getWidth() / 2f, getHeight() / 2f, text);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (!hasFrame || listener == null) return false;
        layoutRect();
        int x = (int) ((e.getX() - dst.left) * bmp.getWidth() / (float) Math.max(1, dst.width()));
        int y = (int) ((e.getY() - dst.top) * bmp.getHeight() / (float) Math.max(1, dst.height()));
        x = Math.max(0, Math.min(bmp.getWidth() - 1, x));
        y = Math.max(0, Math.min(bmp.getHeight() - 1, y));
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_MOVE: listener.onTouch(x, y, true); return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: listener.onTouch(x, y, false); return true;
            default: return false;
        }
    }
}
