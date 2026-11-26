package com.example.piliplus.vr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.MotionEvent;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.zip.CRC32;

public final class VrState {
    private static final String TAG = "PiliVR";

    public static volatile boolean vrActive = false;
    public static volatile boolean vrStarting = false;
    /** Depth source for synthetic 3D: false = stock heuristic, true = ZipDepth. */
    public static volatile boolean aiDepthEnabled = false;
    public static volatile boolean showDepthMap = false;
    public static volatile boolean vrAutoEnter = true;
    public static volatile Activity main2DActivity = null;
    public static volatile Activity vrActivity = null;

    /** mpv_handle* of the most recently created media_kit VideoOutput. */
    public static volatile long mpvHandle = 0;
    /** media_kit VideoOutput instance (obfuscated class vr0). */
    public static volatile Object videoOutput = null;

    /** Tracks all undisposed native mpv_handle* instances to prevent background audio leaks. */
    private static final Set<Long> activeHandles = new HashSet<Long>();
    /** True when the 2D FlutterEngine detached while a native mpv_handle was still alive. */
    public static volatile boolean engineDetached = false;

    private static volatile long suppressAutoUntil = 0;
    private static volatile boolean autoEnterScheduled = false;
    private static volatile int autoEnterAttempt = 0;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    // "Open a video -> jump straight into VR" preference. Shared with the VR
    // settings card (VrPlayerActivity uses the same "pili_vr" prefs file) and
    // with the 2D quick toggle next to the "Enter VR" button.
    private static final String AUTO_ENTER_PREFS = "pili_vr";
    private static final String AUTO_ENTER_KEY = "auto_enter_vr";

    /** Restore the auto-enter preference. Safe to call on every Activity start. */
    public static void loadAutoEnter(Context ctx) {
        if (ctx == null) return;
        try {
            vrAutoEnter = ctx.getSharedPreferences(AUTO_ENTER_PREFS, Context.MODE_PRIVATE)
                    .getBoolean(AUTO_ENTER_KEY, true);
        } catch (Throwable t) {
            Log.w(TAG, "loadAutoEnter failed", t);
        }
    }

    /**
     * Persist and apply the auto-enter toggle. Turning it off also cancels a
     * pending auto-enter so the video currently on screen stays in 2D. It takes
     * effect on the next video opened; the 2D "Enter VR" button still works.
     */
    public static void setAutoEnter(Context ctx, boolean on) {
        vrAutoEnter = on;
        if (ctx != null) {
            try {
                ctx.getSharedPreferences(AUTO_ENTER_PREFS, Context.MODE_PRIVATE)
                        .edit().putBoolean(AUTO_ENTER_KEY, on).apply();
            } catch (Throwable t) {
                Log.w(TAG, "saveAutoEnter failed", t);
            }
            // Mirror into the file the Dart widgets read, so changing the
            // setting inside VR is reflected in the 2D row on return.
            try {
                ctx.getSharedPreferences("FlutterSharedPreferences", Context.MODE_PRIVATE)
                        .edit().putBoolean("flutter.vr_auto_enter", on).apply();
            } catch (Throwable t) {
                Log.w(TAG, "syncAutoEnterToFlutter failed", t);
            }
        }
        if (!on) {
            MAIN.removeCallbacks(AUTO_ENTER_TASK);
            autoEnterScheduled = false;
        }
        Log.i(TAG, "Auto enter VR: " + on);
    }

    // Touch jitter filter state for 2D Quest controller laser pointer taps
    private static float touchDownX = 0f;
    private static float touchDownY = 0f;
    private static boolean touchSlopExceeded = false;

    private static final Runnable AUTO_ENTER_TASK = new Runnable() {
        @Override
        public void run() {
            autoEnterScheduled = false;
            if (vrActive || vrStarting || mpvHandle == 0 || !vrAutoEnter) return;
            if (SystemClock.uptimeMillis() < suppressAutoUntil) return;

            int[] sz = videoSize();
            if (sz == null && autoEnterAttempt < 24) {
                autoEnterAttempt++;
                autoEnterScheduled = true;
                MAIN.postDelayed(AUTO_ENTER_TASK, 70);
                return;
            }
            Log.i(TAG, "Auto entering VR (attempt=" + autoEnterAttempt
                    + ", size=" + (sz != null ? (sz[0] + "x" + sz[1]) : "unknown") + ")");
            enterVR(main2DActivity);
        }
    };

    private VrState() {}

    /**
     * Ensures PiliPlus's Hive setting box has `horizontalScreen = true` and
     * `defaultShowComment = true` so that in 16:9 landscape windows, the video detail
     * page uses the split landscape layout (video + intro on the left, comments on the right)
     * instead of stretching the video to 100% height and hiding the comments section.
     */
    public static void ensureLandscapeSettings(Activity activity) {
        if (activity == null) return;
        try {
            SharedPreferences sp = activity.getSharedPreferences("pili_vr_prefs", Context.MODE_PRIVATE);
            if (sp.getBoolean("vr_landscape_hive_v2", false)) {
                return;
            }
            File[] candidates = new File[]{
                new File(activity.getFilesDir(), "hive/setting.hive"),
                new File(activity.getApplicationInfo().dataDir, "app_flutter/hive/setting.hive")
            };
            boolean updatedAny = false;
            for (File f : candidates) {
                if (f.exists() && f.isFile() && f.length() >= 8) {
                    if (patchSettingHiveFile(f)) {
                        updatedAny = true;
                    }
                }
            }
            if (updatedAny) {
                sp.edit().putBoolean("vr_landscape_hive_v2", true).apply();
            }
        } catch (Throwable t) {
            Log.w(TAG, "ensureLandscapeSettings failed", t);
        }
    }

    private static boolean patchSettingHiveFile(File hiveFile) {
        try {
            byte[] data = readFileBytes(hiveFile);
            if (data == null || data.length < 8) return false;

            int pos = 0;
            Boolean crcIncludesLen = null;
            int boolValType = 3; // Hive FrameValueType.boolT = 3
            boolean foundHoriz = false;
            boolean foundComment = false;
            boolean modified = false;

            while (pos + 8 <= data.length) {
                int len = readLeInt(data, pos);
                if (len < 8 || pos + len > data.length) {
                    Log.w(TAG, "setting.hive frame length mismatch at pos=" + pos + " len=" + len + " total=" + data.length);
                    return false;
                }
                long storedCrc = readLeUint32(data, pos + len - 4);
                long crcFull = computeCrc32(data, pos, len - 4);
                long crcBody = computeCrc32(data, pos + 4, len - 8);
                if (crcIncludesLen == null) {
                    if (crcFull == storedCrc) {
                        crcIncludesLen = Boolean.TRUE;
                    } else if (crcBody == storedCrc) {
                        crcIncludesLen = Boolean.FALSE;
                    } else {
                        Log.w(TAG, "setting.hive CRC mismatch at pos=" + pos);
                        return false;
                    }
                }
                int keyType = data[pos + 4] & 0xFF;
                if (keyType == 1 && pos + 6 <= pos + len - 4) {
                    int keyLen = data[pos + 5] & 0xFF;
                    int valOffset = pos + 6 + keyLen;
                    if (valOffset <= pos + len - 4) {
                        String key = new String(data, pos + 6, keyLen, StandardCharsets.UTF_8);
                        if (valOffset + 2 == pos + len - 4) {
                            int vt = data[valOffset] & 0xFF;
                            int bv = data[valOffset + 1] & 0xFF;
                            if (bv == 0 || bv == 1) {
                                boolValType = vt;
                            }
                            if ("horizontalScreen".equals(key)) {
                                foundHoriz = true;
                                if (bv != 1) {
                                    data[valOffset + 1] = 1;
                                    long newCrc = crcIncludesLen
                                            ? computeCrc32(data, pos, len - 4)
                                            : computeCrc32(data, pos + 4, len - 8);
                                    writeLeInt(data, pos + len - 4, (int) newCrc);
                                    modified = true;
                                }
                            } else if ("defaultShowComment".equals(key)) {
                                foundComment = true;
                                if (bv != 1) {
                                    data[valOffset + 1] = 1;
                                    long newCrc = crcIncludesLen
                                            ? computeCrc32(data, pos, len - 4)
                                            : computeCrc32(data, pos + 4, len - 8);
                                    writeLeInt(data, pos + len - 4, (int) newCrc);
                                    modified = true;
                                }
                            }
                        }
                    }
                }
                pos += len;
            }

            if (pos != data.length) {
                return false;
            }
            boolean incLen = (crcIncludesLen == null) ? true : crcIncludesLen.booleanValue();
            ByteArrayOutputStream out = new ByteArrayOutputStream(data.length + 64);
            out.write(data);
            if (!foundHoriz) {
                out.write(buildHiveBoolFrame("horizontalScreen", boolValType, true, incLen));
                modified = true;
            }
            if (!foundComment) {
                out.write(buildHiveBoolFrame("defaultShowComment", boolValType, true, incLen));
                modified = true;
            }
            if (modified) {
                FileOutputStream fos = new FileOutputStream(hiveFile, false);
                try {
                    fos.write(out.toByteArray());
                    fos.getFD().sync();
                } finally {
                    fos.close();
                }
                Log.i(TAG, "Patched " + hiveFile.getAbsolutePath() + " (horizontalScreen=true, defaultShowComment=true)");
            } else {
                Log.i(TAG, "Verified " + hiveFile.getAbsolutePath() + " already has horizontalScreen=true, defaultShowComment=true");
            }
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "patchSettingHiveFile failed", t);
            return false;
        }
    }

    /**
     * Reads the latest boolean value for a key from PiliPlus's setting.hive box.
     * Returns null if the key is not present or cannot be read.
     */
    public static Boolean readHiveBool(Context ctx, String targetKey) {
        if (ctx == null || targetKey == null) return null;
        try {
            File[] candidates = new File[]{
                new File(ctx.getFilesDir(), "hive/setting.hive"),
                new File(ctx.getApplicationInfo().dataDir, "app_flutter/hive/setting.hive")
            };
            for (File hiveFile : candidates) {
                if (!hiveFile.exists() || !hiveFile.isFile() || hiveFile.length() < 8) continue;
                byte[] data = readFileBytes(hiveFile);
                if (data == null || data.length < 8) continue;
                int pos = 0;
                Boolean result = null;
                while (pos + 8 <= data.length) {
                    int len = readLeInt(data, pos);
                    if (len < 8 || pos + len > data.length) break;
                    int keyType = data[pos + 4] & 0xFF;
                    if (keyType == 1 && pos + 6 <= pos + len - 4) {
                        int keyLen = data[pos + 5] & 0xFF;
                        int valOffset = pos + 6 + keyLen;
                        if (valOffset <= pos + len - 4) {
                            String key = new String(data, pos + 6, keyLen, StandardCharsets.UTF_8);
                            if (targetKey.equals(key)) {
                                if (valOffset + 2 == pos + len - 4) {
                                    int bv = data[valOffset + 1] & 0xFF;
                                    if (bv == 0 || bv == 1) {
                                        result = (bv == 1) ? Boolean.TRUE : Boolean.FALSE;
                                    }
                                } else if (valOffset == pos + len - 4) {
                                    result = null;
                                }
                            }
                        }
                    }
                    pos += len;
                }
                if (result != null) return result;
            }
        } catch (Throwable ignored) {}
        return null;
    }

    private static byte[] buildHiveBoolFrame(String key, int valType, boolean val, boolean crcIncludesLen) {
        byte[] keyBytes = key.getBytes(StandardCharsets.UTF_8);
        int len = 4 + 1 + 1 + keyBytes.length + 1 + 1 + 4;
        byte[] frame = new byte[len];
        writeLeInt(frame, 0, len);
        frame[4] = 1; // ASCII key
        frame[5] = (byte) keyBytes.length;
        System.arraycopy(keyBytes, 0, frame, 6, keyBytes.length);
        int vo = 6 + keyBytes.length;
        frame[vo] = (byte) valType;
        frame[vo + 1] = (byte) (val ? 1 : 0);
        long crc = crcIncludesLen ? computeCrc32(frame, 0, len - 4) : computeCrc32(frame, 4, len - 8);
        writeLeInt(frame, len - 4, (int) crc);
        return frame;
    }

    private static byte[] readFileBytes(File f) throws Exception {
        FileInputStream fis = new FileInputStream(f);
        try {
            ByteArrayOutputStream bos = new ByteArrayOutputStream((int) f.length());
            byte[] buf = new byte[4096];
            int r;
            while ((r = fis.read(buf)) != -1) {
                bos.write(buf, 0, r);
            }
            return bos.toByteArray();
        } finally {
            fis.close();
        }
    }

    private static int readLeInt(byte[] b, int off) {
        return (b[off] & 0xFF)
                | ((b[off + 1] & 0xFF) << 8)
                | ((b[off + 2] & 0xFF) << 16)
                | ((b[off + 3] & 0xFF) << 24);
    }

    private static long readLeUint32(byte[] b, int off) {
        return ((long) readLeInt(b, off)) & 0xFFFFFFFFL;
    }

    private static void writeLeInt(byte[] b, int off, int v) {
        b[off] = (byte) (v & 0xFF);
        b[off + 1] = (byte) ((v >>> 8) & 0xFF);
        b[off + 2] = (byte) ((v >>> 16) & 0xFF);
        b[off + 3] = (byte) ((v >>> 24) & 0xFF);
    }

    private static long computeCrc32(byte[] b, int off, int len) {
        CRC32 crc = new CRC32();
        crc.update(b, off, len);
        return crc.getValue();
    }

    /**
     * Filters out small Quest controller trigger-pull laser jitter (< 14dp) on 2D UI taps
     * so PiliPlus's ImmediateTapGestureRecognizer (which rejects > 2px move) recognizes single taps.
     */
    public static void filterTouchJitter(Activity activity, MotionEvent ev) {
        if (ev == null) return;
        try {
            int action = ev.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                touchDownX = ev.getX();
                touchDownY = ev.getY();
                touchSlopExceeded = false;
            } else if (action == MotionEvent.ACTION_POINTER_DOWN) {
                touchSlopExceeded = true;
            } else if (action == MotionEvent.ACTION_MOVE) {
                if (!touchSlopExceeded && ev.getPointerCount() == 1) {
                    float density = 1.0f;
                    if (activity != null && activity.getResources() != null) {
                        density = activity.getResources().getDisplayMetrics().density;
                        if (density <= 0f) density = 1.0f;
                    }
                    float slop = 14.0f * density;
                    float dx = ev.getX() - touchDownX;
                    float dy = ev.getY() - touchDownY;
                    if (dx * dx + dy * dy < slop * slop) {
                        ev.setLocation(touchDownX, touchDownY);
                    } else {
                        touchSlopExceeded = true;
                    }
                }
            } else if (action == MotionEvent.ACTION_UP) {
                if (!touchSlopExceeded && ev.getPointerCount() == 1) {
                    ev.setLocation(touchDownX, touchDownY);
                }
            }
        } catch (Throwable ignored) {}
    }

    /** Silences and stops a native mpv_handle* so it cannot continue playing in the background. */
    public static void forceStopMpv(long handle) {
        if (handle == 0) return;
        try {
            Log.i(TAG, "Force-stopping mpv handle=0x" + Long.toHexString(handle));
            MpvBridge.setProp(handle, "mute", "yes");
            MpvBridge.setProp(handle, "pause", "yes");
            MpvBridge.command(handle, "stop");
        } catch (Throwable t) {
            Log.w(TAG, "forceStopMpv failed", t);
        }
    }

    /** Returns true if the current mpv stream is a live stream (explicit live URL or confirmed non-positive duration). */
    public static boolean isLiveStream(long handle) {
        if (handle == 0) return false;
        try {
            String path = MpvBridge.getProp(handle, "path");
            if (path != null) {
                if (path.contains("live-bvc") || path.contains("/live/") || path.contains(".flv")) {
                    return true;
                }
                // PiliPlus DASH VOD always uses edl:// for separate video/audio streams.
                if (path.startsWith("edl://")) {
                    return false;
                }
            }
            String dur = MpvBridge.getProp(handle, "duration");
            if (dur == null || dur.trim().isEmpty() || "no".equalsIgnoreCase(dur.trim())) {
                // Duration is transiently unavailable while opening a file or switching quality.
                return false;
            }
            double d = Double.parseDouble(dur.trim());
            return d <= 0.0;
        } catch (Throwable t) {
            return false;
        }
    }

    /** Hook: called from media_kit VideoOutputManager.Dispose (e40.smali). */
    public static void onVideoDispose(long handle) {
        Log.i(TAG, "VideoOutput disposed, mpv handle=0x" + Long.toHexString(handle));
        forceStopMpv(handle);
        synchronized (activeHandles) {
            activeHandles.remove(handle);
        }
        if (mpvHandle == handle) {
            mpvHandle = 0;
            videoOutput = null;
            VrModeButton.setVisible(false);
        }
    }

    /** Hook: called from media_kit MediaKitVideoPlugin.onDetachedFromEngine (e40.smali). */
    public static void onEngineDetached() {
        Log.i(TAG, "FlutterEngine detached (vrActive=" + vrActive + ", vrStarting=" + vrStarting + ")");
        engineDetached = true;
        videoOutput = null;
        List<Long> toStop = new ArrayList<Long>();
        synchronized (activeHandles) {
            for (Long h : activeHandles) {
                if (h != null && h != 0L) {
                    if ((!vrActive && !vrStarting) || h != mpvHandle) {
                        toStop.add(h);
                    }
                }
            }
            for (Long h : toStop) {
                activeHandles.remove(h);
            }
        }
        for (Long h : toStop) {
            forceStopMpv(h);
        }
        if (!vrActive && !vrStarting) {
            mpvHandle = 0;
            VrModeButton.setVisible(false);
        }
    }

    /**
     * Called when VR exits or pauses: stops only stale mpv handles, and pauses
     * playback only when the user explicitly paused in VR. Live streams must
     * keep playing when returning to the 2D live room.
     */
    public static void stopOrphanedOrPauseOnVrExit(long handle) {
        stopOrphanedOrPauseOnVrExit(handle, false);
    }

    public static void stopOrphanedOrPauseOnVrExit(final long handle, final boolean vrPaused) {
        if (engineDetached) {
            Log.i(TAG, "VR exit with detached 2D engine: keeping active mpv handle=0x" + Long.toHexString(handle));
            List<Long> stale = new ArrayList<Long>();
            synchronized (activeHandles) {
                for (Long h : activeHandles) {
                    if (h != null && h != 0L && h != handle) stale.add(h);
                }
                activeHandles.clear();
                if (handle != 0L) activeHandles.add(handle);
            }
            for (Long h : stale) forceStopMpv(h);
            if (handle != 0L) {
                try {
                    MpvBridge.setProp(handle, "pause", vrPaused ? "yes" : "no");
                } catch (Throwable ignored) {}
            }
            return;
        }
        if (handle != 0 && vrPaused) {
            Log.i(TAG, "VR exit (user paused=" + vrPaused + "): pausing mpv handle=0x" + Long.toHexString(handle));
            try {
                MpvBridge.setProp(handle, "pause", "yes");
            } catch (Throwable ignored) {}
            // PiliPlus's PLVideoPlayer.didChangeAppLifecycleState(AppLifecycleState.resumed)
            // auto-calls player.play() when MainActivity returns to front (~300ms after VR exit)
            // because _pauseDueToPauseUponEnteringBackgroundMode was set when entering VR.
            // Re-assert pause after MainActivity.onResume so live streams / VR-paused videos stay paused.
            final Runnable enforcePause = new Runnable() {
                @Override
                public void run() {
                    if (!vrActive && !vrStarting && mpvHandle == handle && handle != 0) {
                        try {
                            MpvBridge.setProp(handle, "pause", "yes");
                        } catch (Throwable ignored) {}
                    }
                }
            };
            MAIN.postDelayed(enforcePause, 450);
            MAIN.postDelayed(enforcePause, 850);
        }
    }

    /** Hook: called from media_kit VideoOutput constructor. */
    public static void onVideoOutput(Object output, long handle) {
        List<Long> stale = new ArrayList<Long>();
        synchronized (activeHandles) {
            for (Long old : activeHandles) {
                if (old != null && old != 0L && old != handle) {
                    stale.add(old);
                }
            }
            activeHandles.clear();
            if (handle != 0) {
                activeHandles.add(handle);
            }
        }
        for (Long old : stale) {
            Log.i(TAG, "Stopping previous undisposed mpv handle=0x" + Long.toHexString(old));
            forceStopMpv(old);
        }
        engineDetached = false;
        videoOutput = output;
        mpvHandle = handle;
        Log.i(TAG, "VideoOutput created, mpv handle=0x" + Long.toHexString(handle));
        VrModeButton.setVisible(true);
        // Schedule auto-enter; AUTO_ENTER_TASK will poll videoSize() until the first frame is ready.
        scheduleAutoEnter(350);
    }

    /** Hook: called when media_kit (re)creates the video Surface, i.e. a video is being opened. */
    public static void onVideoSurfaceRequested() {
        // Fast path: Surface is known; poll videoSize() starting after 220ms.
        scheduleAutoEnter(220);
    }

    private static void scheduleAutoEnter(long delayMs) {
        if (!vrAutoEnter || vrActive || vrStarting) return;
        if (SystemClock.uptimeMillis() < suppressAutoUntil) return;
        final Activity a = main2DActivity;
        if (a == null) return;
        MAIN.removeCallbacks(AUTO_ENTER_TASK);
        autoEnterAttempt = 0;
        autoEnterScheduled = true;
        MAIN.postDelayed(AUTO_ENTER_TASK, delayMs);
    }

    /** Legacy hook from the PiP code path. Intentionally does nothing now. */
    public static void onVideoPlay(Activity activity, boolean isPlaying) {}

    public static boolean isVideoActive() {
        return mpvHandle != 0;
    }

    /** Current Surface global-ref that media_kit handed to mpv (VideoOutput.b). */
    public static long flutterWid() {
        Object vo = videoOutput;
        if (vo == null) return 0;
        try {
            Field f = vo.getClass().getDeclaredField("b");
            f.setAccessible(true);
            return f.getLong(vo);
        } catch (Throwable t) {
            return 0;
        }
    }

    /**
     * Current video size as mpv displays it (after rotation / aspect correction),
     * or null when no video parameters are known yet.
     */
    public static int[] videoSize() {
        long h = mpvHandle;
        if (h == 0) return null;
        int w = intProp(h, "dwidth");
        int ht = intProp(h, "dheight");
        if (w <= 0 || ht <= 0) {
            w = intProp(h, "width");
            ht = intProp(h, "height");
        }
        if (w <= 0 || ht <= 0) return null;
        return new int[]{w, ht};
    }

    private static int intProp(long handle, String name) {
        try {
            String v = MpvBridge.getProp(handle, name);
            if (v == null) return 0;
            return Integer.parseInt(v.trim());
        } catch (Throwable t) {
            return 0;
        }
    }

    /** Blocks automatic VR entry for a while (used when XR init failed). */
    public static void suppressAutoEnter(long ms) {
        suppressAutoUntil = SystemClock.uptimeMillis() + ms;
    }

    public static boolean enterVR(Activity activity) {
        if (activity == null || vrActive || vrStarting) return false;
        if (mpvHandle == 0) {
            Toast.makeText(activity, "请先打开一个视频再进入 VR", Toast.LENGTH_SHORT).show();
            return false;
        }
        main2DActivity = activity;
        vrStarting = true;
        try {
            Intent intent = new Intent(activity, VrPlayerActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
            int[] size = videoSize();
            if (size != null) {
                intent.putExtra("video_w", size[0]);
                intent.putExtra("video_h", size[1]);
            }
            activity.startActivity(intent);
            try { activity.overridePendingTransition(0, 0); } catch (Throwable ignored) {}
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "Failed to launch VrPlayerActivity", t);
            vrStarting = false;
            return false;
        }
    }

    /** Called by VrPlayerActivity once mpv output has been handed back to Flutter. */
    public static void onVrExited(Activity vrAct) {
        Log.i(TAG, "Exiting VR mode, back to 2D");
        // Short suppression: only long enough to swallow the wid-restore / Surface
        // recreation that happens right after exit (which would otherwise instantly
        // re-enter VR). 5s was too long and blocked a genuinely new video the user
        // opened ~4s after exiting.
        suppressAutoUntil = SystemClock.uptimeMillis() + 1500;
        vrActive = false;
        vrStarting = false;
        vrActivity = null;
        VrModeButton.setVisible(mpvHandle != 0);
        final Activity main = main2DActivity;
        if (vrAct != null && !vrAct.isFinishing()) {
            vrAct.finish();
            try { vrAct.overridePendingTransition(0, 0); } catch (Throwable ignored) {}
        }
        if (main != null) {
            MAIN.postDelayed(new Runnable() {
                @Override
                public void run() {
                    try {
                        Intent i = new Intent(main, main.getClass());
                        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NO_ANIMATION);
                        main.startActivity(i);
                        try { main.overridePendingTransition(0, 0); } catch (Throwable ignored) {}
                    } catch (Throwable t) {
                        Log.w(TAG, "bring 2D to front failed", t);
                    }
                }
            }, 300);
        }
    }

    /** Kept for VrModeButton compatibility. */
    public static void switchTo2D(Activity vrAct) {
        if (vrAct instanceof VrPlayerActivity) {
            ((VrPlayerActivity) vrAct).exitVr();
        } else {
            onVrExited(vrAct);
        }
    }
}
