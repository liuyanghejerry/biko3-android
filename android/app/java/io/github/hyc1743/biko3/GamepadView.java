package io.github.hyc1743.biko3;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * Minimal virtual gamepad for the port: D-pad, the Biko3 diamond (bottom A,
 * right B, left X, top Y), L/R and +/−. Touches that do not land on a control
 * are left to the Activity so they reach the game's touch screen.
 */
final class GamepadView extends View {
    private static final int BTN_A = 1 << 0;
    private static final int BTN_B = 1 << 1;
    private static final int BTN_X = 1 << 2;
    private static final int BTN_Y = 1 << 3;
    private static final int BTN_ZL = 1 << 8;
    private static final int BTN_ZR = 1 << 9;
    private static final int BTN_PLUS = 1 << 10;
    private static final int BTN_MINUS = 1 << 11;
    private static final int BTN_LEFT = 1 << 12;
    private static final int BTN_UP = 1 << 13;
    private static final int BTN_RIGHT = 1 << 14;
    private static final int BTN_DOWN = 1 << 15;

    private static final class Control {
        final int mask;
        final String label;
        final RectF rect = new RectF();
        int pointer = -1;
        Control(int mask, String label) { this.mask = mask; this.label = label; }
    }

    private final List<Control> controls = new ArrayList<>();
    private final SparseArray<Control> tracked = new SparseArray<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int publishedMask = -1;

    interface Listener { void onButtons(int mask); }
    private Listener listener;

    GamepadView(Context context) {
        super(context);
        controls.add(new Control(BTN_Y, "Y"));
        controls.add(new Control(BTN_X, "X"));
        controls.add(new Control(BTN_B, "B"));
        controls.add(new Control(BTN_A, "A"));
        controls.add(new Control(BTN_UP, "▲"));
        controls.add(new Control(BTN_LEFT, "◀"));
        controls.add(new Control(BTN_RIGHT, "▶"));
        controls.add(new Control(BTN_DOWN, "▼"));
        controls.add(new Control(BTN_ZL, "ZL"));
        controls.add(new Control(BTN_ZR, "ZR"));
        controls.add(new Control(BTN_MINUS, "−"));
        controls.add(new Control(BTN_PLUS, "+"));
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x50FFFFFF);
        edge.setStyle(Paint.Style.STROKE);
        edge.setStrokeWidth(2f);
        edge.setColor(0x90FFFFFF);
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        setClickable(false);
        setFocusable(false);
    }

    void setListener(Listener listener) { this.listener = listener; }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        /* Size the controls in dp so they stay finger-sized on phones of any
         * resolution; the old max(1.4) cap made them tiny on a high-resolution display. */
        float density = getResources().getDisplayMetrics().density;
        float radius = 30f * density;                    // 60 dp face buttons
        float gap = 4f * density;
        float marginX = Math.min(150f * density, width * 0.22f);
        float marginY = Math.min(120f * density, height * 0.30f);
        float faceX = width - marginX;
        float faceY = height - marginY;
        Control y = controls.get(0), x = controls.get(1), b = controls.get(2), a = controls.get(3);
        /* Nintendo face layout: Y top, X left, A right, B bottom (the user
         * expects confirm on the right and cancel on the bottom). */
        circle(y, faceX, faceY - radius * 2 - gap, radius);
        circle(x, faceX - radius * 2 - gap, faceY, radius);
        circle(a, faceX + radius * 2 + gap, faceY, radius);
        circle(b, faceX, faceY + radius * 2 + gap, radius);
        float arm = 52f * density;                       // 52 dp d-pad arms
        float dpadX = Math.min(175f * density, width * 0.22f);
        float dpadY = height - marginY;
        box(controls.get(4), dpadX - arm / 2, dpadY - arm * 1.5f, arm, arm);           // up
        box(controls.get(5), dpadX - arm * 1.5f, dpadY - arm / 2, arm, arm);           // left
        box(controls.get(6), dpadX + arm / 2, dpadY - arm / 2, arm, arm);              // right
        box(controls.get(7), dpadX - arm / 2, dpadY + arm / 2, arm, arm);              // down
        box(controls.get(8), 20f * density, 18f * density, 96f * density, 46f * density);   // ZL
        box(controls.get(9), width - 116f * density, 18f * density, 96f * density, 46f * density); // ZR
        box(controls.get(10), width / 2f - 80f * density, 24f * density, 62f * density, 40f * density); // −
        box(controls.get(11), width / 2f + 18f * density, 24f * density, 62f * density, 40f * density); // +
        for (Control control : controls) control.pointer = -1;
        tracked.clear();
        publish(0);
        android.util.Log.i("Biko3", "pad size " + width + "x" + height + " density " + density);
    }

    private static void circle(Control control, float cx, float cy, float radius) {
        control.rect.set(cx - radius, cy - radius, cx + radius, cy + radius);
    }

    private static void box(Control control, float x, float y, float width, float height) {
        control.rect.set(x, y, x + width, y + height);
    }

    private Control hit(float x, float y) {
        for (Control control : controls)
            if (control.rect.contains(x, y)) return control;
        return null;
    }

    private void publish(int mask) {
        if (mask == publishedMask) return;
        publishedMask = mask;
        if (listener != null) listener.onButtons(mask);
        invalidate();
    }

    private void recompute() {
        int mask = 0;
        for (int i = 0; i < tracked.size(); i++) mask |= tracked.valueAt(i).mask;
        publish(mask);
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int index = event.getActionIndex();
                Control control = hit(event.getX(index), event.getY(index));
                if (control == null || control.pointer >= 0) return false;
                control.pointer = event.getPointerId(index);
                tracked.put(control.pointer, control);
                recompute();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                boolean handled = false;
                for (int i = 0; i < event.getPointerCount(); i++) {
                    Control control = tracked.get(event.getPointerId(i));
                    if (control == null) continue;
                    if (!control.rect.contains(event.getX(i), event.getY(i))) {
                        // A finger that slides off a button releases it, as on the iOS pad.
                        control.pointer = -1;
                        tracked.remove(event.getPointerId(i));
                        recompute();
                    }
                    handled = true;
                }
                return handled;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int index = event.getActionIndex();
                Control control = tracked.get(event.getPointerId(index));
                if (control == null) return false;
                control.pointer = -1;
                tracked.remove(event.getPointerId(index));
                recompute();
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                for (int i = 0; i < tracked.size(); i++) tracked.valueAt(i).pointer = -1;
                tracked.clear();
                recompute();
                return true;
            }
            default:
                return false;
        }
    }

    @Override protected void onDraw(Canvas canvas) {
        float radius = Math.min(getWidth(), getHeight()) * 0.02f;
        for (Control control : controls) {
            boolean active = control.pointer >= 0;
            fill.setColor(active ? 0xA0FFFFFF : 0x50FFFFFF);
            edge.setColor(active ? 0xFFFFFFFF : 0x90FFFFFF);
            RectF rect = control.rect;
            if (rect.width() == rect.height()) {
                canvas.drawCircle(rect.centerX(), rect.centerY(), rect.width() / 2f, fill);
                canvas.drawCircle(rect.centerX(), rect.centerY(), rect.width() / 2f, edge);
            } else {
                canvas.drawRoundRect(rect, 8f, 8f, fill);
                canvas.drawRoundRect(rect, 8f, 8f, edge);
            }
            text.setTextSize(Math.max(12f, rect.height() * 0.42f));
            canvas.drawText(control.label, rect.centerX(),
                    rect.centerY() + text.getTextSize() * 0.35f, text);
        }
        if (radius <= 0f) return;
    }

    void reset() {
        for (int i = 0; i < tracked.size(); i++) tracked.valueAt(i).pointer = -1;
        tracked.clear();
        recompute();
    }
}
