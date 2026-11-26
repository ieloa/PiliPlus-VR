package com.example.piliplus.vr;

import java.nio.ByteBuffer;

/**
 * Zero-copy CPU-only bridge to the AI depth channel in libp5m-vr.so.
 *
 * The library is loaded by XrBridge's companion object before this class is
 * first used, so no System.loadLibrary() here: a second load with a wrong name
 * would throw UnsatisfiedLinkError and take the VR session down.
 *
 * Double-buffered DirectByteBuffers are registered once per model load; native
 * C++ writes flipped FP32 input directly from mapped PBOs and reads output
 * directly on the shared-EGL stage thread to normalize, smooth and upload
 * GL_RGBA8 [R, G, B, D] without JNI array copies or per-frame heap allocations.
 */
public final class AiDepthNative {

    public static final int DTYPE_FLOAT32 = 0;
    public static final int DTYPE_UINT8 = 1;
    public static final int DTYPE_INT8 = 2;

    private AiDepthNative() {
    }

    /** Enable/disable AI depth. When off, the stock heuristic 3D is untouched. */
    public static native void setMode(boolean on, int mapsPerSecond);

    /** Update capture cadence without resetting the active mailbox/depth texture. */
    public static native void setCaptureRate(int mapsPerSecond);

    /** Configure bilateral Sigmoid edge sharpening and warp 4-tap B-spline interpolation. */
    public static native void setEdgeEnhance(float sharp, boolean cubic);

    /** Rolling EMA GPU render cost in ms via GL_EXT_disjoint_timer_query (-1 if unavailable). */
    public static native float getWarpGpuMs();

    /** Current active headset display refresh rate in Hz. */
    public static native float getDisplayRefreshHz();

    /** Percentage (0..100) of OpenXR refreshes missed in the latest window, or -1 if unready. */
    public static native float getMissedRefreshPct();

    /** Wait until OpenXR submits the next frame (> lastSeq) or timeoutMs elapses. */
    public static native long waitForXrFrame(long lastSeq, int timeoutMs);

    /** Predicted display timestamp (ns) for the upcoming OpenXR frame. */
    public static native long getTargetDisplayNs();

    /** Hint active row height (pixels from top) for partial danmaku upload. */
    public static native void setDanmakuUploadHeight(int height);

    /** Intro curtain length: 3600 ms cold, 1000 ms with a cached model / kernel. */
    public static native void setCurtainDuration(int millis);

    /** Debug view: render the depth map heat map instead of the video. */
    public static native void setShowDepth(boolean on);

    /** Toggle fast per-pixel temporal EMA vs slow blend. */
    public static native void setFastEma(boolean on);

    /** Width of the RGBA frame the native side captures (512). */
    public static native int frameWidth();

    /** Height of the RGBA frame the native side captures (288). */
    public static native int frameHeight();

    /**
     * Register double-buffered direct ByteBuffers owned by ZipDepthSource so
     * native C++ can fill input[slot] directly from mapped PBOs and consume
     * output[slot] directly on the shared-EGL upload thread.
     */
    public static native boolean registerBuffers(
            ByteBuffer in0, ByteBuffer out0,
            ByteBuffer in1, ByteBuffer out1,
            int inW, int inH, int inType,
            int outW, int outH, int outType);

    /** Release native global references to previously registered buffers. */
    public static native void unregisterBuffers();

    /**
     * Block until native has prepared an inference input slot (0 or 1), or
     * return -1 if timed out, disabled, or torn down.
     */
    public static native int acquireInputSlot(int timeoutMs);

    /** Return an acquired slot to FREE if inference failed before submission. */
    public static native void releaseInputSlot(int slot);

    /** Mark output[slot] ready and wake the shared-EGL stage thread immediately. */
    public static native void submitOutputSlot(int slot);

    /**
     * Poll OpenXR hand tracking state:
     * [0]=active, [1]=pinchDown, [2]=hitType, [3]=u, [4]=v,
     * [5]=rayYaw, [6]=rayPitch, [7]=toggleEvent (+1/-1), [8]=isPaused.
     */
    public static native boolean pollHandTracking(float[] outState);

    /** Enable or pause OpenXR hand tracking pointer/trigger interaction. */
    public static native void setHandTrackingEnabled(boolean enabled);

    /** Hide hand tracking cursors while dragging/resizing the screen. */
    public static native void setHandCursorHidden(boolean hidden);

    /** Sync base screen FOV and distance so native hand gestures stay VSync-locked. */
    public static native void syncBaseScreenState(float baseFov, float screenDist);

    /** Notify native renderer when Java/controller drag or zoom is active. */
    public static native void setScreenManipulating(boolean manipulating);

    /** Keep bottom-right corner resize handle visible while resizing. */
    public static native void setCornerResizing(boolean resizing);
}
