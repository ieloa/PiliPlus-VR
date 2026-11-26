package com.example.piliplus.vr;

import android.util.Log;
import android.view.Surface;

/** JNI bridge into the libmpv instance that media_kit already created. */
public final class MpvBridge {
    private static boolean loaded;

    static {
        try {
            System.loadLibrary("pilivr");
            loaded = true;
        } catch (Throwable t) {
            Log.e("PiliVR", "load libpilivr failed", t);
        }
    }

    private MpvBridge() {}

    public static boolean isLoaded() { return loaded && available(); }

    public static native boolean available();
    public static native int setProp(long handle, String name, String value);
    public static native String getProp(long handle, String name);
    public static native int command(long handle, String cmd);
    public static native long newGlobalRef(Object obj);
    public static native void deleteGlobalRef(long ref);
}
