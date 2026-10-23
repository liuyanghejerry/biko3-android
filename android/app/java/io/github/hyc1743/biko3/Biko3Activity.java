package io.github.hyc1743.biko3;

import android.app.Activity;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.graphics.SurfaceTexture;
import android.view.TextureView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;

/**
 * Single-activity shell: presentation surface, resource import screen, status
 * overlay and the virtual gamepad. The native host starts only after the game
 * data is in place (imported from the shipped .b3data volumes or pushed).
 */
public final class Biko3Activity extends Activity implements TextureView.SurfaceTextureListener {
    private static final String TAG = "Biko3";
    private static final int MAX_TEXT = 20000;

    private TextureView surfaceView;
    private TextView statusView;
    private LinearLayout importPanel;
    private TextView importStatus;
    private ProgressBar importProgress;
    private Button importButton;
    private GamepadView pad;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private Surface surface;
    private int physicalMask;
    private int padMask;
    private int lastPublished = -1;
    private boolean started;
    private boolean importing;
    private int surfaceWidth;
    private int surfaceHeight;
    private File dataRoot;
    private ResourceImporter importer;
    private boolean debuggable;

    private final Runnable statusPoll = new Runnable() {
        @Override public void run() {
            String status;
            try {
                status = Biko3Native.status();
            } catch (Throwable error) {
                status = "native status unavailable: " + error;
            }
            if (status != null) {
                if (status.length() > MAX_TEXT) status = status.substring(status.length() - MAX_TEXT);
                String shortStatus = tail(status, 10);
                if (!shortStatus.contentEquals(statusView.getText())) statusView.setText(shortStatus);
            }
            handler.postDelayed(this, 500);
        }
    };

    @Override protected void onCreate(Bundle bundle) {
        super.onCreate(bundle);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        if (Build.VERSION.SDK_INT >= 30) {
            // Lay the window out over the whole display so the 9:16 picture area
            // is not shortened by the navigation bar (which stretched the game
            // horizontally).
            getWindow().setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(android.view.WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                        android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }

        debuggable = (getApplicationInfo().flags
                & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        File files = getFilesDir();
        File external = getExternalFilesDir(null);
        if (external == null) external = files;
        dataRoot = external;
        for (String name : new String[] { "guest", "game", "saves", "captures", "ResourcePacks" }) {
            File directory = new File(name.equals("guest") ? files : external, name);
            //noinspection ResultOfMethodCallIgnored
            directory.mkdirs();
        }
        importer = new ResourceImporter(getAssets(), external);
        Log.i(TAG, "external root: " + external + ", data ready: " + importer.dataReady());

        PictureLayout root = new PictureLayout(this);
        root.setBackgroundColor(Color.BLACK);
        /* TextureView composites through hwui, so the portrait/landscape display
         * rotation is applied to the game picture exactly like the overlay. A
         * SurfaceView on a rotated display keeps its own orientation and showed
         * the game sideways in landscape on the device. */
        surfaceView = new TextureView(this);
        surfaceView.setSurfaceTextureListener(this);
        surfaceView.setOpaque(true);
        // Biko3 is a landscape 16:9 game (its panel is 1280x720). The root
        // layout gives the SurfaceView the largest centred 16:9 box on its
        // first measure pass, so the surface is created at its final size.
        root.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        pad = new GamepadView(this);
        pad.setListener(mask -> {
            if (mask != padMask) Log.i(TAG, "pad mask " + mask);
            padMask = mask;
            publishButtons();
        });
        root.addView(pad, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(10f);
        statusView.setTypeface(android.graphics.Typeface.MONOSPACE);
        statusView.setBackgroundColor(0x88000000);
        statusView.setPadding(14, 14, 14, 14);
        statusView.setClickable(false);
        statusView.setFocusable(false);
        statusView.setTextIsSelectable(false);
        FrameLayout.LayoutParams overlay = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        overlay.setMargins(16, 16, 16, 16);
        statusView.setVisibility(debuggable ? View.VISIBLE : View.GONE);
        root.addView(statusView, overlay);

        importPanel = new LinearLayout(this);
        importPanel.setOrientation(LinearLayout.VERTICAL);
        importPanel.setBackgroundColor(0xF0101824);
        importPanel.setPadding(48, 48, 48, 48);
        LinearLayout.LayoutParams inner = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        TextView title = new TextView(this);
        title.setText("Biko3 资源准备");
        title.setTextColor(Color.WHITE);
        title.setTextSize(22f);
        title.setGravity(android.view.Gravity.CENTER);
        importStatus = new TextView(this);
        importStatus.setTextColor(0xFFB8C4D8);
        importStatus.setTextSize(13f);
        importStatus.setGravity(android.view.Gravity.CENTER);
        importStatus.setPadding(0, 18, 0, 18);
        importStatus.setText("把六个 Biko3Data.partNNN.b3data 分卷放到\n"
                + "Android/data/io.github.hyc1743.biko3/files/ResourcePacks/\n"
                + "(adb push)，再点下面的按钮校验并展开。");
        importProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        importProgress.setMax(1000);
        importProgress.setProgress(0);
        importButton = new Button(this);
        importButton.setText("校验并展开");
        importButton.setOnClickListener(view -> startImport());
        importPanel.addView(title, inner);
        importPanel.addView(importStatus, inner);
        importPanel.addView(importProgress, inner);
        importPanel.addView(importButton, inner);
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        panelParams.gravity = android.view.Gravity.CENTER;
        root.addView(importPanel, panelParams);

        importPanel.setVisibility(View.GONE);
        setContentView(root);
        handler.post(statusPoll);
        Log.i(TAG, "activity created");
    }

    private static String tail(String text, int lines) {
        String[] all = text.split("\n");
        if (all.length <= lines) return text;
        StringBuilder builder = new StringBuilder();
        for (int i = all.length - lines; i < all.length; i++) builder.append(all[i]).append('\n');
        return builder.toString();
    }

    private void publishButtons() {
        if (started) {
            int mask = padMask | physicalMask;
            if (mask != lastPublished) Log.i(TAG, "buttons " + mask);
            lastPublished = mask;
            Biko3Native.buttons(mask);
        }
    }

    private void startNative() {
        if (started || surface == null) return;
        started = Biko3Native.start(getAssets(), getFilesDir().getAbsolutePath(),
                dataRoot.getAbsolutePath(), surface, surfaceWidth, surfaceHeight);
        Log.i(TAG, "native start: " + started);
        if (started) {
            importPanel.setVisibility(View.GONE);
            pad.setVisibility(View.VISIBLE);
            // The status overlay is a development aid; hide it once the game is
            // up so it does not cover the picture.
            handler.postDelayed(() -> statusView.setVisibility(View.GONE), 8000);
        }
    }

    private void startImport() {
        if (importing) return;
        importing = true;
        importButton.setEnabled(false);
        importStatus.setText("正在校验分卷…");
        importProgress.setProgress(0);
        new Thread(() -> {
            String error;
            try {
                error = importer.install((phase, fraction) -> handler.post(() -> {
                    importStatus.setText(phase);
                    importProgress.setProgress((int) (fraction * 1000));
                }));
            } catch (Throwable problem) {
                error = "导入失败：" + problem;
                Log.w(TAG, "import failed", problem);
            }
            final String failure = error;
            handler.post(() -> {
                importing = false;
                importButton.setEnabled(true);
                if (failure == null && importer.dataReady()) {
                    importStatus.setText("资源就绪，正在进入游戏…");
                    importProgress.setProgress(1000);
                    startNative();
                } else {
                    importStatus.setText((failure == null ? "资源未就绪" : failure)
                            + "\n补齐分卷后重试；已完成的文件会保留。");
                }
            });
        }, "biko3-import").start();
    }

    @Override protected void onPause() {
        super.onPause();
        Log.i(TAG, "activity paused");
        pad.reset();
        physicalMask = 0;
        publishButtons();
        if (started) Biko3Native.pause();
    }

    @Override protected void onResume() {
        super.onResume();
        Log.i(TAG, "activity resumed");
        if (started) Biko3Native.resume();
    }

    @Override protected void onDestroy() {
        Log.i(TAG, "activity destroyed");
        handler.removeCallbacks(statusPoll);
        if (started) Biko3Native.stop();
        if (surface != null) surface.release();
        super.onDestroy();
    }

    @Override public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
        Log.i(TAG, "texture available " + width + "x" + height);
        surface = new Surface(texture);
        surfaceWidth = width;
        surfaceHeight = height;
        if (importer.dataReady()) {
            startNative();
        } else {
            importPanel.setVisibility(View.VISIBLE);
            statusView.setText("等待游戏资源导入…");
            if (importer.volumesAvailable()) startImport();
        }
    }

    @Override public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {
        surfaceWidth = width;
        surfaceHeight = height;
    }

    @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
        Log.i(TAG, "texture destroyed");
        surface = null;
        return true;
    }

    @Override public void onSurfaceTextureUpdated(SurfaceTexture texture) { }

    private static int keyMask(int keyCode) {
        switch (keyCode) {
            case android.view.KeyEvent.KEYCODE_BUTTON_A: return 1 << 0;
            case android.view.KeyEvent.KEYCODE_BUTTON_B: return 1 << 1;
            case android.view.KeyEvent.KEYCODE_BUTTON_X: return 1 << 2;
            case android.view.KeyEvent.KEYCODE_BUTTON_Y: return 1 << 3;
            case android.view.KeyEvent.KEYCODE_BUTTON_L1:
            case android.view.KeyEvent.KEYCODE_BUTTON_L2: return 1 << 8;
            case android.view.KeyEvent.KEYCODE_BUTTON_R1:
            case android.view.KeyEvent.KEYCODE_BUTTON_R2: return 1 << 9;
            case android.view.KeyEvent.KEYCODE_BUTTON_START:
            case android.view.KeyEvent.KEYCODE_MENU: return 1 << 10;
            case android.view.KeyEvent.KEYCODE_BUTTON_SELECT:
            case android.view.KeyEvent.KEYCODE_BACK: return 1 << 11;
            case android.view.KeyEvent.KEYCODE_DPAD_LEFT: return 1 << 12;
            case android.view.KeyEvent.KEYCODE_DPAD_UP: return 1 << 13;
            case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: return 1 << 14;
            case android.view.KeyEvent.KEYCODE_DPAD_DOWN: return 1 << 15;
            default: return 0;
        }
    }

    @Override public boolean onKeyDown(int keyCode, android.view.KeyEvent event) {
        int mask = keyMask(keyCode);
        if (mask == 0) return super.onKeyDown(keyCode, event);
        physicalMask |= mask;
        publishButtons();
        return true;
    }

    @Override public boolean onKeyUp(int keyCode, android.view.KeyEvent event) {
        int mask = keyMask(keyCode);
        if (mask == 0) return super.onKeyUp(keyCode, event);
        physicalMask &= ~mask;
        publishButtons();
        return true;
    }

    /** Centres a 16:9 SurfaceView (first child) inside the window; the overlay
     *  views stay full-screen. */
    static final class PictureLayout extends FrameLayout {
        PictureLayout(android.content.Context context) { super(context); }

        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            int width = MeasureSpec.getSize(widthSpec);
            int height = MeasureSpec.getSize(heightSpec);
            int pictureWidth = Math.min(width, Math.round(height * 16f / 9f));
            int pictureHeight = Math.round(pictureWidth * 9f / 16f);
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                if (i == 0) {
                    child.measure(MeasureSpec.makeMeasureSpec(pictureWidth, MeasureSpec.EXACTLY),
                            MeasureSpec.makeMeasureSpec(pictureHeight, MeasureSpec.EXACTLY));
                } else {
                    child.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
                            MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
                }
            }
            setMeasuredDimension(width, height);
        }

        @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            int width = right - left, height = bottom - top;
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                if (i == 0) {
                    int x = (width - child.getMeasuredWidth()) / 2;
                    int y = (height - child.getMeasuredHeight()) / 2;
                    child.layout(x, y, x + child.getMeasuredWidth(), y + child.getMeasuredHeight());
                } else {
                    child.layout(0, 0, width, height);
                }
            }
        }
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!started || surfaceWidth <= 0 || surfaceHeight <= 0) return super.onTouchEvent(event);
        int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                break;
            default:
                return true;
        }
        for (int index = 0; index < event.getPointerCount(); index++) {
            /* Only touches on the picture reach the game; the letterbox bars
             * around it are not part of its touchscreen. */
            float x = event.getX(index) - surfaceView.getLeft();
            float y = event.getY(index) - surfaceView.getTop();
            if (x < 0 || y < 0 || x >= surfaceWidth || y >= surfaceHeight) continue;
            Biko3Native.touch(action, event.getPointerId(index),
                    x / surfaceWidth, y / surfaceHeight);
        }
        return true;
    }
}
