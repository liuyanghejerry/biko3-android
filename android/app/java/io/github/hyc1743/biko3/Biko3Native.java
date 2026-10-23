package io.github.hyc1743.biko3;

import android.content.res.AssetManager;
import android.view.Surface;

/** Thin JNI surface for the nxnative host (android/port/src/biko3_android.c). */
final class Biko3Native {
    static {
        System.loadLibrary("biko3");
    }

    private Biko3Native() {}

    static native boolean start(AssetManager assets, String filesDir, String dataDir,
                                Surface surface, int width, int height);
    static native void stop();
    static native void pause();
    static native void resume();
    static native void touch(int action, int id, float x, float y);
    static native void buttons(int mask);
    static native String status();
}
