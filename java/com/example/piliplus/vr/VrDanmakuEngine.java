package com.example.piliplus.vr;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import io.github.gblandro.p5m.XrBridge;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserFactory;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;

/**
 * High-refresh-rate (~83fps) Danmaku & CC Subtitle renderer for the OpenXR overlay layer.
 * - Dynamically adapts canvas aspect ratio to match any video aspect ratio (16:9, 4:3, 21:9, 9:16)
 *   so text is never stretched or compressed.
 * - Uses continuous nanosecond clock interpolation + pre-rendered Bitmap sprites per active danmaku
 *   to eliminate 24fps quantization judder and sample-and-hold ghosting.
 * - Supports independent toggles and font size scales for Danmaku and CC Subtitles.
 */
public final class VrDanmakuEngine {
    private static final String TAG = "PiliVR-Danmaku";

    private static final int MAX_SCROLL_TRACKS = 32;
    private static final int MAX_TOP_TRACKS = 6;
    private static final int MAX_BOTTOM_TRACKS = 5;
    private static final float SCROLL_DURATION_SEC = 8.0f;
    private static final float FIXED_DURATION_SEC = 4.0f;
    private static final int MAX_ACTIVE_DANMAKU = 220;
    private static final float SEG_DURATION_SEC = 360.0f;
    private static final int MAX_PENDING_SCROLL = 120;

    public static final class DanmakuItem {
        public final float timeSec;
        public final int mode; // 1,2,3 = scroll; 4 = bottom; 5 = top
        public final int color; // 0xFFRRGGBB
        public final float baseFontSize;
        public final String text;

        public DanmakuItem(float timeSec, int mode, int color, float baseFontSize, String text) {
            this.timeSec = timeSec;
            this.mode = mode;
            this.color = color | 0xFF000000;
            this.baseFontSize = baseFontSize;
            this.text = text;
        }
    }

    public static final class SubtitleCue {
        public final float fromSec;
        public final float toSec;
        public final String content;

        public SubtitleCue(float fromSec, float toSec, String content) {
            this.fromSec = fromSec;
            this.toSec = toSec;
            this.content = content;
        }
    }

    private static final class ActiveDanmaku {
        DanmakuItem item;
        int track;
        float width;
        float speedPxPerSec;
        float startSec;
        float endSec;
        Bitmap sprite;
        float spriteScale;
        float[] charWidths;
        float[] charMidOffsets;
        int[] charSrcLeft;
        int[] charSrcRight;
        float spritePadX;
        float spriteCenterOffsetY;
    }

    private final Context context;
    private final Object renderLock = new Object();
    private XrBridge xr;
    private Thread workerThread;
    private Thread mpvPollThread;
    private long lastXrSeq = 0L;
    private long lastDispNs = 0L;
    private volatile boolean running = false;
    private volatile boolean danmakuEnabled = true;
    private volatile boolean fixedDanmakuEnabled = true;
    private volatile boolean subtitleEnabled = true;
    // When ZipDepth is active the whole 2400x1440 overlay swapchain is parked:
    // no 83 Hz canvas draw and no per-tick glTexSubImage2D, so the GPU budget
    // goes to the depth model instead. The user's on/off settings are untouched.
    private volatile boolean suspended = false;
    private volatile float danmakuScale = 1.0f;
    private volatile float subtitleScale = 1.0f;
    private volatile int scaleGeneration = 0;
    private int appliedScaleGeneration = 0;
    private volatile int densityIdx = 2;
    private volatile int danmakuMode = 0;
    private volatile int danmakuDistIdx = 0;
    private volatile int danmakuSpanIdx = 0;
    private volatile boolean danmakuVOut = false;
    private float spanX = 1.0f;
    private float spanY = 1.0f;
    private volatile float videoAspect = 1.7777778f;
    private volatile float screenViewDist = 2.5f;
    private volatile float screenViewFov = 1.0f;
    private static final int[] DENSITY_MAX_ACTIVE = {36, 68, 108, 160};
    private static final float[] DENSITY_HEIGHT_FRAC = {0.30f, 0.50f, 0.72f, 0.90f};
    private static final float[] DENSITY_MIN_GAP = {56f, 32f, 18f, 8f};
    private static final int[] DENSITY_PIN_TRACKS = {2, 3, 5, 6};

    private int canvasW = 1280;
    private int canvasH = 720;
    private int projectionMode = 0;
    private Bitmap bitmap;
    private Canvas canvas;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint spritePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint subBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint subStrokePaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);

    private final List<DanmakuItem> allDanmaku = new ArrayList<>();
    private final List<ActiveDanmaku> activeList = new ArrayList<>();
    private final ActiveDanmaku[] scrollTracks = new ActiveDanmaku[MAX_SCROLL_TRACKS];
    private final ActiveDanmaku[] topTracks = new ActiveDanmaku[MAX_TOP_TRACKS];
    private final ActiveDanmaku[] bottomTracks = new ActiveDanmaku[MAX_BOTTOM_TRACKS];
    private final java.util.ArrayDeque<DanmakuItem> pendingScroll = new java.util.ArrayDeque<>();
    private final java.util.ArrayDeque<DanmakuItem> pendingTop = new java.util.ArrayDeque<>();
    private final java.util.ArrayDeque<DanmakuItem> pendingBottom = new java.util.ArrayDeque<>();
    private float lastScrollSpawnSec = -100f;
    private int nextDanmakuIdx = 0;

    // On-demand Bilibili 6-minute segment cache (so arbitrary large seeks never lose danmaku)
    private final java.util.TreeMap<Integer, List<DanmakuItem>> segmentBuckets = new java.util.TreeMap<>();
    private final Object segLock = new Object();
    private final java.util.Set<Integer> loadedSegments = new java.util.HashSet<>();
    private final java.util.Set<Integer> netFetchedSegments = new java.util.HashSet<>();
    private final java.util.Set<Integer> inFlightSegments = new java.util.HashSet<>();
    private final java.util.Map<Integer, Long> failedSegmentRetryMs = new java.util.HashMap<>();
    private volatile int fastSegHitA = -1;
    private volatile int fastSegHitB = -1;
    private volatile boolean segmentDataUpdated = false;
    private java.util.concurrent.ExecutorService dmFetchExecutor;

    private final List<SubtitleCue> subtitleCues = new ArrayList<>();
    private long lastSubFileMtime = 0L;
    private long lastSubFileLen = 0L;

    private volatile long currentCid = 0L;
    private long lastTsvMtime = 0L;
    private long lastTsvLen = 0L;
    private boolean xmlFallbackAttempted = false;
    private volatile boolean isLiveStreamMode = false;
    private final Object liveLock = new Object();
    private final java.util.Queue<DanmakuItem> liveQueue = new java.util.concurrent.ConcurrentLinkedQueue<>();
    private volatile long lastLiveTsvOffset = 0L;
    private long lastLiveReadMs = 0L;
    private volatile long lastLiveTsvGrowthMs = 0L;
    private volatile long liveRoomId = 0L;
    private long liveTsvReadCount = 0L;
    private long lastLiveStatsMs = 0L;
    private volatile long lastNativeLiveCheckMs = 0L;
    private volatile LiveDanmakuClient nativeLiveClient;

    // Continuous nanosecond clock state synchronized to OpenXR display pacing
    private long lastRenderNs = 0L;
    private volatile double renderVideoSec = -1.0;
    private double playbackSpeed = 1.0;
    private double clockDriftRate = 0.0;
    private boolean isPaused = false;
    private String cachedMpvSubText = null;
    private boolean wasCanvasEmpty = false;
    private java.util.concurrent.ExecutorService ioExecutor;
    private volatile boolean ioCheckPending = false;
    private final RectF subBoxRect = new RectF();

    // Asynchronous MPV property poll snapshot (keeps render thread free of libmpv locks)
    private volatile boolean polledPaused = false;
    private volatile double polledSpeed = 1.0;
    private volatile double polledMpvSec = -1.0;
    private volatile long polledMpvAtNs = 0L;
    private volatile long polledMpvSeq = 0L;
    private volatile boolean polledIsLive = false;
    private volatile boolean polledLiveKnown = false;
    private volatile String polledSubText = null;
    private volatile boolean aspectCheckPending = true;
    private long appliedMpvSeq = 0L;

    // Redraw deduplication state (skips redundant canvas clear + texture upload when static)
    private float lastDrawnSec = Float.NaN;
    private int lastDrawnActiveCount = -1;
    private String lastDrawnCcText = null;
    private int lastDrawnScaleGen = -1;
    private int lastDrawnSpanIdx = -1;
    private int lastDrawnDensityIdx = -1;
    private int lastDrawnDistIdx = -1;
    private boolean lastDrawnVOut = false;
    private float lastDrawnSubScale = -1f;
    private boolean lastDrawnFixed = true;
    private int lastDrawnWidth = 0;
    private int lastDrawnHeight = 0;
    private int prevCpuBottomPx = 0;
    private int curFrameMaxBottomPx = 0;
    private final int[] recentBottomPx = new int[5];
    private int recentBottomIdx = 0;

    // Reusable 3D surround draw buffers (precomputed once per frame, replayed for both eyes)
    private final Rect charSrcRect = new Rect();
    private final RectF charDstRect = new RectF();
    private static final int SURROUND_PASSES = 2; // 0 = Back (Z < 0), 1 = Front (Z >= 0)
    private final int[] passCount = new int[SURROUND_PASSES];
    private final int[] passCap = new int[SURROUND_PASSES];
    private final Bitmap[][] passSprite = new Bitmap[SURROUND_PASSES][];
    private final int[][] passSrcL = new int[SURROUND_PASSES][];
    private final int[][] passSrcR = new int[SURROUND_PASSES][];
    private final float[][] passMonoX = new float[SURROUND_PASSES][];
    private final float[][] passDisp = new float[SURROUND_PASSES][];
    private final float[][] passLocalY = new float[SURROUND_PASSES][];
    private final float[][] passRelL = new float[SURROUND_PASSES][];
    private final float[][] passRelT = new float[SURROUND_PASSES][];
    private final float[][] passRelR = new float[SURROUND_PASSES][];
    private final float[][] passRelB = new float[SURROUND_PASSES][];
    private final int[][] passAlpha = new int[SURROUND_PASSES][];
    private final boolean[][] passMirrored = new boolean[SURROUND_PASSES][];

    public VrDanmakuEngine(Context context) {
        this.context = context.getApplicationContext();

        Typeface boldSans = Typeface.create("sans-serif", Typeface.BOLD);
        fillPaint.setTypeface(boldSans);
        fillPaint.setStyle(Paint.Style.FILL);

        strokePaint.setTypeface(boldSans);
        strokePaint.setStyle(Paint.Style.STROKE);
        strokePaint.setStrokeJoin(Paint.Join.ROUND);
        strokePaint.setStrokeCap(Paint.Cap.ROUND);
        strokePaint.setStrokeWidth(4.2f);
        strokePaint.setColor(0xEE000000);

        subBgPaint.setStyle(Paint.Style.FILL);
        subBgPaint.setColor(0xB810141D);

        subTextPaint.setTypeface(boldSans);
        subTextPaint.setStyle(Paint.Style.FILL);
        subTextPaint.setColor(Color.WHITE);
        subTextPaint.setTextAlign(Paint.Align.CENTER);

        subStrokePaint.setTypeface(boldSans);
        subStrokePaint.setStyle(Paint.Style.STROKE);
        subStrokePaint.setStrokeJoin(Paint.Join.ROUND);
        subStrokePaint.setStrokeWidth(4.0f);
        subStrokePaint.setColor(0xEE000000);
        subStrokePaint.setTextAlign(Paint.Align.CENTER);
    }

    public void start(XrBridge xrBridge, boolean initDanmakuEnabled, boolean initSubtitleEnabled,
                                   float initDanmakuScale, float initSubtitleScale, int initDensityIdx, int initDanmakuMode) {
        synchronized (renderLock) {
            if (running) return;
            this.xr = xrBridge;
            this.danmakuEnabled = initDanmakuEnabled;
            this.subtitleEnabled = initSubtitleEnabled;
            this.danmakuScale = initDanmakuScale;
            this.subtitleScale = initSubtitleScale;
            this.densityIdx = Math.max(0, Math.min(3, initDensityIdx));
            this.danmakuMode = Math.max(0, Math.min(4, initDanmakuMode));
            this.running = true;
            this.lastXrSeq = 0L;
            this.lastDispNs = 0L;
            this.lastRenderNs = 0L;
            this.clockDriftRate = 0.0;
            this.appliedMpvSeq = 0L;
            this.polledMpvSeq = 0L;
            this.polledMpvSec = -1.0;
            this.polledPaused = false;
            this.polledSpeed = 1.0;
            this.polledIsLive = false;
            this.polledLiveKnown = false;
            this.polledSubText = null;
            this.cachedMpvSubText = null;
            this.aspectCheckPending = true;
            resetLastDrawnState();
            clearSegmentState();
            this.ioExecutor = java.util.concurrent.Executors.newSingleThreadExecutor();
            this.dmFetchExecutor = java.util.concurrent.Executors.newSingleThreadExecutor();
            this.ioCheckPending = false;

            updateCanvasAspect();
            xrBridge.setDanmakuVisible(false);

            mpvPollThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    mpvPollLoop();
                }
            }, "VrDanmakuMpvPoll");
            mpvPollThread.start();

            workerThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    workerLoop();
                }
            }, "VrDanmakuWorker");
            workerThread.start();
        }
    }

    public void stop() {
        stopNativeLiveFeed();
        Thread wt;
        Thread pt;
        synchronized (renderLock) {
            running = false;
            wt = workerThread;
            pt = mpvPollThread;
            workerThread = null;
            mpvPollThread = null;
        }
        if (pt != null) {
            pt.interrupt();
            try { pt.join(250L); } catch (InterruptedException ignored) {}
        }
        if (wt != null) {
            wt.interrupt();
            try { wt.join(250L); } catch (InterruptedException ignored) {}
        }
        synchronized (renderLock) {
            if (ioExecutor != null) {
                try {
                    ioExecutor.shutdownNow();
                } catch (Throwable ignored) {}
                ioExecutor = null;
            }
            if (dmFetchExecutor != null) {
                try {
                    dmFetchExecutor.shutdownNow();
                } catch (Throwable ignored) {}
                dmFetchExecutor = null;
            }
            clearActiveTracks();
            clearSegmentState();
            resetLastDrawnState();
            bitmap = null;
            canvas = null;
            if (xr != null) {
                try { xr.setDanmakuVisible(false); } catch (Throwable ignored) {}
                xr = null;
            }
        }
    }

    /** Park/resume the whole overlay (danmaku + CC). Settings are preserved. */
    public void setSuspended(boolean value) {
        suspended = value;
        if (value) {
            XrBridge bridge = this.xr;
            synchronized (renderLock) {
                clearActiveTracks();
                if (canvas != null && bitmap != null) {
                    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
                    if (bridge != null) bridge.setDanmakuBitmap(bitmap);
                }
                wasCanvasEmpty = true;
            }
            if (bridge != null) bridge.setDanmakuVisible(false);
        } else {
            synchronized (renderLock) {
                wasCanvasEmpty = false;
                resetLastDrawnState();
                updateLayerVisibilityLocked();
            }
        }
    }

    public void setDanmakuEnabled(boolean enabled) {
        this.danmakuEnabled = enabled;
        if (!enabled) stopNativeLiveFeed();
        synchronized (renderLock) {
            if (!enabled) {
                clearActiveTracks();
                if (canvas != null && bitmap != null) {
                    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
                    XrBridge bridge = this.xr;
                    if (bridge != null) {
                        bridge.setDanmakuBitmap(bitmap);
                    }
                }
            }
            this.wasCanvasEmpty = false;
            updateLayerVisibilityLocked();
        }
    }

    private static boolean isFixedDanmakuMode(int mode) {
        return mode == 4 || mode == 5;
    }

    /**
     * Enable or disable fixed centered danmaku (top mode=5 and bottom mode=4).
     * When disabled, any currently displayed fixed danmaku are cleared immediately.
     */
    public void setFixedDanmakuEnabled(boolean enabled) {
        this.fixedDanmakuEnabled = enabled;
        synchronized (renderLock) {
            if (!enabled) {
                for (int i = 0; i < topTracks.length; i++) topTracks[i] = null;
                for (int i = 0; i < bottomTracks.length; i++) bottomTracks[i] = null;
                for (int i = activeList.size() - 1; i >= 0; i--) {
                    ActiveDanmaku ad = activeList.get(i);
                    if (isFixedDanmakuMode(ad.item.mode)) {
                        ad.sprite = null;
                        activeList.remove(i);
                    }
                }
            }
            this.wasCanvasEmpty = false;
            updateLayerVisibilityLocked();
        }
    }

    public void setSubtitleEnabled(boolean enabled) {
        this.subtitleEnabled = enabled;
        synchronized (renderLock) {
            if (!enabled) {
                if (canvas != null && bitmap != null) {
                    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
                    if (danmakuEnabled && !activeList.isEmpty()) {
                        drawActiveDanmaku((float) Math.max(0.0, renderVideoSec));
                    }
                    XrBridge bridge = this.xr;
                    if (bridge != null) {
                        bridge.setDanmakuBitmap(bitmap);
                    }
                }
            }
            this.wasCanvasEmpty = false;
            resetLastDrawnState();
            updateLayerVisibilityLocked();
        }
    }

    private void updateLayerVisibilityLocked() {
        XrBridge bridge = this.xr;
        if (bridge == null) return;
        if (suspended) {
            bridge.setDanmakuVisible(false);
            return;
        }
        boolean hasDanmaku = danmakuEnabled && !activeList.isEmpty();
        String sub = subtitleEnabled ? resolveSubtitleText((float) Math.max(0.0, renderVideoSec)) : null;
        boolean hasCc = (sub != null && !sub.isEmpty());
        bridge.setDanmakuVisible(hasDanmaku || hasCc);
    }

    public void setDanmakuScale(float scale) {
        synchronized (renderLock) {
            this.danmakuScale = Math.max(0.5f, Math.min(2.0f, scale));
            this.scaleGeneration++;
            this.wasCanvasEmpty = false;
        }
    }

    public void setSubtitleScale(float scale) {
        synchronized (renderLock) {
            this.subtitleScale = Math.max(0.5f, Math.min(2.0f, scale));
            this.wasCanvasEmpty = false;
        }
    }

    public void setDanmakuDensity(int idx) {
        synchronized (renderLock) {
            this.densityIdx = Math.max(0, Math.min(3, idx));
            this.wasCanvasEmpty = false;
        }
    }

    public void setDanmakuMode(int mode) {
        synchronized (renderLock) {
            this.danmakuMode = Math.max(0, Math.min(4, mode));
            this.spanX = (danmakuMode == 4) ? 2.25f : ((danmakuMode == 3) ? 1.55f : 1.0f);
            this.spanY = 1.0f;
            this.scaleGeneration++;
            this.wasCanvasEmpty = false;
            updateCanvasAspect();
            clearActiveTracks();
        }
    }

    public float getModeScaleRatio() {
        // Base reference is 720p eye video height in VR space.
        // effectiveEyeH / curSpanY represents the pixel height of 1x video height on the canvas.
        // Dividing by 720 gives the exact uniform scale ratio so font size and track spacing in VR
        // remain completely invariant whether 【上下出屏】 (danmakuVOut / spanY) is ON (1.6x) or OFF (1.0x).
        float effectiveEyeH = (danmakuSpanIdx == 3) ? (canvasH * 0.5f) : canvasH;
        if (effectiveEyeH <= 10f) effectiveEyeH = 720f;
        float curSpanY = (spanY > 0.05f) ? spanY : 1.0f;
        return (effectiveEyeH / curSpanY) / 720f;
    }

    public float getDanmakuScaleRatio() {
        float base = getModeScaleRatio();
        // On portrait videos (aspect < 1.0), screen height spans a larger vertical FOV,
        // so scale danmaku down proportionally to keep VR angular glyph size balanced.
        if (projectionMode == 0 && videoAspect < 0.99f) {
            float factor = Math.max(0.50f, Math.min(1.0f, videoAspect));
            return base * factor;
        }
        return base;
    }

    private static float computeTrackSpanX(float videoAspect) {
        float a = Math.max(0.35f, Math.min(3.20f, videoAspect));
        float span = 1.0f + 0.70f / a;
        return Math.max(1.45f, Math.min(2.50f, span));
    }

    public void setScreenViewParams(float distM, float fovRad) {
        this.screenViewDist = Math.max(0.2f, distM);
        this.screenViewFov = Math.max(0.05f, fovRad);
    }

    public void setDanmakuParams(int distIdx, int spanIdx, boolean vOut) {
        synchronized (renderLock) {
            this.danmakuDistIdx = Math.max(0, Math.min(3, distIdx));
            this.danmakuSpanIdx = Math.max(0, Math.min(3, spanIdx));
            this.danmakuVOut = vOut;
            if (spanIdx == 3) {
                this.spanX = 1.45f;
            } else if (spanIdx == 2) {
                this.spanX = 2.25f;
            } else if (spanIdx == 1) {
                this.spanX = 1.55f;
            } else {
                this.spanX = 1.0f;
            }
            this.spanY = vOut ? 1.6f : 1.0f;
            this.scaleGeneration++;
            this.wasCanvasEmpty = false;
            updateCanvasAspect();
            clearActiveTracks();
        }
    }

    private void resetLastDrawnState() {
        lastDrawnSec = Float.NaN;
        lastDrawnActiveCount = -1;
        lastDrawnCcText = null;
        lastDrawnScaleGen = -1;
        lastDrawnSpanIdx = -1;
        lastDrawnDensityIdx = -1;
        lastDrawnDistIdx = -1;
        lastDrawnVOut = false;
        lastDrawnSubScale = -1f;
        lastDrawnFixed = true;
        lastDrawnWidth = 0;
        lastDrawnHeight = 0;
        prevCpuBottomPx = canvasH;
        for (int i = 0; i < recentBottomPx.length; i++) recentBottomPx[i] = canvasH;
    }

    private void mpvPollLoop() {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND);
        } catch (Throwable ignored) {}
        long lastFileMs = 0L;
        while (running) {
            long nowMs = SystemClock.uptimeMillis();
            if (nowMs - lastFileMs > 1500L) {
                lastFileMs = nowMs;
                aspectCheckPending = true;
                triggerBackgroundFileCheck();
            }
            long handle = VrState.mpvHandle;
            if (handle != 0L) {
                if (currentCid <= 0L) {
                    polledIsLive = VrState.isLiveStream(handle);
                    polledLiveKnown = true;
                }
                String pauseStr = MpvBridge.getProp(handle, "pause");
                polledPaused = "yes".equalsIgnoreCase(pauseStr);
                String speedStr = MpvBridge.getProp(handle, "speed");
                if (speedStr != null) {
                    try {
                        double sp = Double.parseDouble(speedStr.trim());
                        if (sp > 0.1 && sp < 8.0) polledSpeed = sp;
                    } catch (Throwable ignored) {}
                }
                if (!isLiveStreamMode) {
                    String posStr = MpvBridge.getProp(handle, "time-pos");
                    long atNs = SystemClock.elapsedRealtimeNanos();
                    if (posStr != null && !posStr.isEmpty()) {
                        try {
                            polledMpvSec = Double.parseDouble(posStr.trim());
                            polledMpvAtNs = atNs;
                        } catch (Throwable ignored) {}
                    }
                }
                String subText = MpvBridge.getProp(handle, "sub-text");
                if (subText != null && !isLiveStreamMode) {
                    subText = subText.trim();
                    polledSubText = subText.isEmpty() ? null : subText;
                } else {
                    polledSubText = null;
                }
                polledMpvSeq++;
            }
            long sleepMs = (polledMpvSec < 0.0 && !isLiveStreamMode) ? 50L : 200L;
            try {
                Thread.sleep(sleepMs);
            } catch (InterruptedException ie) {
                if (!running) break;
            }
        }
    }

    private void workerLoop() {
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY);
        } catch (Throwable ignored) {}
        while (running) {
            long newSeq = AiDepthNative.waitForXrFrame(lastXrSeq, 50);
            if (!running) break;
            if (newSeq <= lastXrSeq) {
                try {
                    Thread.sleep(16L);
                } catch (InterruptedException ie) {
                    if (!running) break;
                }
                continue;
            }
            lastXrSeq = newSeq;
            if (!running) break;
            long dispNs = AiDepthNative.getTargetDisplayNs();
            try {
                tick(dispNs);
            } catch (Throwable t) {
                Log.w(TAG, "tick error: " + t.getMessage());
            }
        }
    }

    private void triggerBackgroundFileCheck() {
        java.util.concurrent.ExecutorService ex = this.ioExecutor;
        if (ex == null || ex.isShutdown() || ioCheckPending) return;
        ioCheckPending = true;
        ex.execute(new Runnable() {
            @Override
            public void run() {
                try {
                    checkFilesAndReload();
                } catch (Throwable ignored) {
                } finally {
                    ioCheckPending = false;
                }
            }
        });
    }

    private void switchToLiveMode() {
        switchToLiveMode(-1L);
    }

    private void switchToLiveMode(long targetCid) {
        synchronized (liveLock) {
            if (isLiveStreamMode) {
                if (targetCid == currentCid) return;
                if (currentCid == -1L && targetCid < -1L) {
                    // Refine provisional live mode (-1L) with the real room cid without clearing queue.
                    currentCid = targetCid;
                    return;
                }
            }
            Log.i(TAG, "Entering LIVE stream mode (cid=" + targetCid + ", prev=" + currentCid + "): clearing previous danmaku & subtitles");
            isLiveStreamMode = true;
            currentCid = (targetCid < 0L) ? targetCid : -1L;
            xmlFallbackAttempted = true;
            File liveFile = new File(context.getFilesDir(), "vr_live_danmaku.tsv");
            // Ignore the previous file bridge's backlog; the native feed starts
            // immediately in live mode and owns the queue from here on.
            lastLiveTsvOffset = liveFile.exists() ? liveFile.length() : 0L;
            lastLiveTsvGrowthMs = SystemClock.uptimeMillis();
            renderVideoSec = 0.0;
            lastRenderNs = 0L;
            clockDriftRate = 0.0;
            playbackSpeed = 1.0;
            liveQueue.clear();
        }
        synchronized (renderLock) {
            synchronized (allDanmaku) {
                allDanmaku.clear();
                segmentBuckets.clear();
                cachedMpvSubText = null;
                polledSubText = null;
                clearActiveTracks();
                nextDanmakuIdx = 0;
            }
            synchronized (subtitleCues) {
                subtitleCues.clear();
            }
        }
        clearSegmentState();
    }

    private void exitLiveMode() {
        synchronized (liveLock) {
            if (!isLiveStreamMode) return;
            Log.i(TAG, "Exiting LIVE stream mode back to VOD mode");
            stopNativeLiveFeed();
            isLiveStreamMode = false;
            liveRoomId = 0L;
            lastLiveTsvOffset = 0L;
            lastLiveTsvGrowthMs = SystemClock.uptimeMillis();
            liveQueue.clear();
        }
        synchronized (renderLock) {
            clearActiveTracks();
        }
        clearSegmentState();
    }

    private void readLiveDanmakuTsv() {
        // The 2D Flutter live-room WebSocket keeps writing this file whenever
        // the 2D engine is alive (VrState.engineDetached == false). It carries
        // the full, login-scoped comment stream, so it is ALWAYS the primary
        // source -- even while the native WSS fallback is connected.
        synchronized (liveLock) {
            File liveFile = new File(context.getFilesDir(), "vr_live_danmaku.tsv");
            // Only skip the bridge when the 2D engine is really gone AND the
            // fallback owns the stream; otherwise the 2D writer is still active.
            if (VrState.engineDetached && nativeLiveClient != null) return;
            if (!liveFile.exists()) {
                lastLiveTsvOffset = 0L;
                return;
            }
            long fileLen = liveFile.length();
            if (fileLen < lastLiveTsvOffset) {
                lastLiveTsvOffset = 0L;
                lastLiveTsvGrowthMs = SystemClock.uptimeMillis();
            }
            if (fileLen == lastLiveTsvOffset) {
                return;
            }
            lastLiveTsvGrowthMs = SystemClock.uptimeMillis();
            // The bridge is alive again. Retire the fallback so a single source
            // stays in charge and no comment is shown twice.
            if (nativeLiveClient != null) {
                Log.i(TAG, "Live TSV bridge resumed; stopping native WSS fallback");
                stopNativeLiveFeed();
                lastNativeLiveCheckMs = lastLiveTsvGrowthMs + 5000L;
            }
            java.io.RandomAccessFile raf = null;
            try {
                raf = new java.io.RandomAccessFile(liveFile, "r");
                raf.seek(lastLiveTsvOffset);
                String line;
                while ((line = raf.readLine()) != null) {
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    byte[] raw = line.getBytes(StandardCharsets.ISO_8859_1);
                    String utfLine = new String(raw, StandardCharsets.UTF_8).trim();
                    if (utfLine.isEmpty()) continue;
                    String[] parts = utfLine.split("\t", 3);
                    if (parts.length < 3) continue;
                    try {
                        int mode = Integer.parseInt(parts[0].trim());
                        int color = Integer.parseInt(parts[1].trim());
                        String text = parts[2].trim();
                        if (!text.isEmpty()) {
                            DanmakuItem item = new DanmakuItem(0f, mode, color, 25f, text);
                            liveQueue.offer(item);
                            liveTsvReadCount++;
                        }
                    } catch (Throwable ignored) {}
                }
                lastLiveTsvOffset = raf.getFilePointer();
            } catch (Throwable t) {
                Log.w(TAG, "readLiveDanmakuTsv error: " + t.getMessage());
            } finally {
                if (raf != null) {
                    try { raf.close(); } catch (Throwable ignored) {}
                }
            }
        }
    }

    private void stopNativeLiveFeed() {
        LiveDanmakuClient client = nativeLiveClient;
        nativeLiveClient = null;
        if (client != null) client.stop();
    }

    /**
     * The 2D live-room WebSocket writes vr_live_danmaku.tsv and stays alive
     * unless the FlutterEngine really detaches. That bridge carries the full,
     * login-scoped stream; the anonymous native WSS only receives a sampled
     * subset. So the bridge is always primary and the native feed is started
     * only as a fallback for a detached engine or a bridge gone quiet.
     */
    private void maybeStartNativeLiveFeed(long nowMs) {
        if (!isLiveStreamMode || liveRoomId <= 0L || nativeLiveClient != null
                || !danmakuEnabled || !running) return;
        if (nowMs - lastNativeLiveCheckMs < 1000L) return;
        lastNativeLiveCheckMs = nowMs;
        File liveFile = new File(context.getFilesDir(), "vr_live_danmaku.tsv");
        boolean bridgeAlive = liveFile.exists()
                && (nowMs - lastLiveTsvGrowthMs) < 5000L;
        if (!VrState.engineDetached && bridgeAlive) {
            return;
        }
        // Keep the current TSV offset. The bridge may resume at any moment and
        // must hand the full stream back without losing a chunk of comments.
        Log.i(TAG, "Starting native live danmaku fallback for room=" + liveRoomId
                + " (engineDetached=" + VrState.engineDetached
                + ", bridgeIdleMs=" + (nowMs - lastLiveTsvGrowthMs) + ")");
        final LiveDanmakuClient[] holder = new LiveDanmakuClient[1];
        final long roomIdSnapshot = liveRoomId;
        holder[0] = new LiveDanmakuClient(roomIdSnapshot,
                new LiveDanmakuClient.Sink() {
                    @Override
                    public void onDanmaku(int mode, int color, String text) {
                        if (text == null || text.isEmpty()) return;
                        if (!isLiveStreamMode || liveRoomId != roomIdSnapshot) return;
                        if (liveQueue.size() < 600) {
                            liveQueue.offer(new DanmakuItem(0f, mode, color, 25f, text));
                        }
                    }

                    @Override
                    public void onLog(String message) {
                        Log.i(TAG, "native live: " + message);
                    }

                    @Override
                    public void onClosed() {
                        if (nativeLiveClient == holder[0]) nativeLiveClient = null;
                        long now = SystemClock.uptimeMillis();
                        // Let the 2D bridge take over before another fallback
                        // connection is attempted. Do NOT advance the TSV
                        // offset here: the 2D WebSocket may have kept writing
                        // while this fallback was connected, and those lines
                        // must still be read on the next pass.
                        lastNativeLiveCheckMs = now + 5000L;
                        lastLiveTsvGrowthMs = now;
                    }
                });
        nativeLiveClient = holder[0];
        holder[0].start();
    }

    public void setProjectionMode(int mode) {
        synchronized (renderLock) {
            this.projectionMode = mode;
            updateCanvasAspect();
        }
    }

    private void updateCanvasAspect() {
        int vw = 1920;
        int vh = 1080;
        int[] sz = VrState.videoSize();
        if (sz != null && sz[0] > 0 && sz[1] > 0) {
            vw = sz[0];
            vh = sz[1];
        }
        float aspect = (float) vw / (float) vh;
        if (projectionMode > 0) {
            // When in VR180 or VR360, each eye views an undistorted hemisphere/sphere.
            // Using standard 16:9 aspect guarantees completely distortion-free subtitles and danmaku.
            aspect = 1.7777778f;
        } else {
            if (aspect < 0.35f) aspect = 0.35f;
            if (aspect > 3.20f) aspect = 3.20f;
        }

        if (Math.abs(this.videoAspect - aspect) > 0.03f) {
            this.videoAspect = aspect;
            this.scaleGeneration++;
        } else {
            this.videoAspect = aspect;
        }

        if (danmakuSpanIdx == 3) {
            float trackSpan = computeTrackSpanX(aspect);
            if (Math.abs(this.spanX - trackSpan) > 0.02f) {
                this.spanX = trackSpan;
                clearActiveTracks();
            } else {
                this.spanX = trackSpan;
            }
        }

        float desiredAspect = aspect * (spanX / spanY);
        int targetW, targetH;
        if (danmakuSpanIdx == 3) {
            // Stereo mode (2D video only): over-under layout (targetH = 2 * eyeH).
            // Use 720p per eye on portrait videos (still well below 2304px width) to keep scaled-down
            // glyphs crisp while staying within the 2400x1440 OpenXR swapchain.
            int portraitEyeH = (projectionMode == 0 && aspect < 0.99f) ? 720 : 648;
            if (desiredAspect >= (2304f / portraitEyeH)) {
                targetW = 2304;
                int eyeH = Math.max(324, Math.round(2304f / desiredAspect)) & ~1;
                targetH = eyeH * 2;
            } else {
                int eyeH = portraitEyeH;
                targetH = eyeH * 2;
                targetW = Math.max(256, Math.min(2304, Math.round(eyeH * desiredAspect))) & ~1;
            }
        } else if (desiredAspect >= (2048f / 1152f)) {
            targetW = 2048;
            targetH = Math.max(384, Math.round(2048f / desiredAspect)) & ~1;
            if (targetH > 1152) targetH = 1152;
        } else {
            targetH = Math.min(1152, Math.round(2048f / desiredAspect)) & ~1;
            targetW = Math.round(targetH * desiredAspect) & ~1;
            if (targetW > 2048) targetW = 2048;
        }

        if (bitmap == null || targetW != canvasW || targetH != canvasH) {
            canvasW = targetW;
            canvasH = targetH;
            bitmap = Bitmap.createBitmap(canvasW, canvasH, Bitmap.Config.ARGB_8888);
            canvas = new Canvas(bitmap);
            wasCanvasEmpty = false;
            resetLastDrawnState();
            Log.i(TAG, "Adapted Danmaku/Subtitle canvas to " + canvasW + "x" + canvasH + " (video " + vw + "x" + vh + ", mode=" + danmakuMode + ", span=" + spanX + "x" + spanY + ")");
        }
    }

    private void tick(long dispNs) {
        if (!running || suspended) return;
        XrBridge bridge = this.xr;
        if (bridge == null) return;

        if (aspectCheckPending) {
            aspectCheckPending = false;
            synchronized (renderLock) {
                updateCanvasAspect();
            }
        }

        long handle = VrState.mpvHandle;
        if (handle == 0L) return;

        long nowMs = SystemClock.uptimeMillis();
        long nowNs = SystemClock.elapsedRealtimeNanos();

        if (isLiveStreamMode && (nowMs - lastLiveReadMs >= 150L)) {
            lastLiveReadMs = nowMs;
            readLiveDanmakuTsv();
            maybeStartNativeLiveFeed(nowMs);
            if (nowMs - lastLiveStatsMs >= 10000L) {
                lastLiveStatsMs = nowMs;
                Log.i(TAG, "live stats room=" + liveRoomId
                        + " tsvReads=" + liveTsvReadCount
                        + " queue=" + liveQueue.size()
                        + " native=" + (nativeLiveClient != null)
                        + " detached=" + VrState.engineDetached
                        + " paused=" + isPaused);
            }
        }

        // 1. Derive dt from compositor predicted display timestamp delta when active,
        // so slow/fast OS wakeups never inject judder into scroll positions.
        double dtSec;
        if (dispNs > 0L && lastDispNs > 0L && dispNs > lastDispNs && (dispNs - lastDispNs) < 100_000_000L) {
            dtSec = (dispNs - lastDispNs) / 1_000_000_000.0;
        } else {
            dtSec = (lastRenderNs > 0L) ? ((nowNs - lastRenderNs) / 1_000_000_000.0) : 0.011111;
        }
        if (dispNs > 0L) lastDispNs = dispNs;
        lastRenderNs = nowNs;
        if (dtSec < 0.0 || dtSec > 0.1) dtSec = 0.011111;

        if (isLiveStreamMode && renderVideoSec < 0.0) {
            renderVideoSec = 0.0;
        }

        // 2. Consume latest background-polled mpv state (zero libmpv mutex contention here).
        long pollSeq = this.polledMpvSeq;
        if (pollSeq != appliedMpvSeq || renderVideoSec < 0.0) {
            appliedMpvSeq = pollSeq;
            if (currentCid <= 0L && polledLiveKnown) {
                boolean isLive = this.polledIsLive;
                if (isLive && !isLiveStreamMode) {
                    switchToLiveMode(-1L);
                } else if (!isLive && isLiveStreamMode && currentCid == -1L) {
                    exitLiveMode();
                }
            }
            isPaused = this.polledPaused;
            playbackSpeed = this.polledSpeed;
            cachedMpvSubText = isLiveStreamMode ? null : this.polledSubText;

            if (isLiveStreamMode) {
                clockDriftRate = 0.0;
            } else {
                double pSec = this.polledMpvSec;
                long pAtNs = this.polledMpvAtNs;
                if (pSec >= 0.0 && pAtNs > 0L) {
                    double pollAgeSec = !isPaused
                            ? Math.max(0.0, Math.min(0.4, (nowNs - pAtNs) / 1_000_000_000.0))
                            : 0.0;
                    double mpvSec = pSec + pollAgeSec * playbackSpeed;
                    if (renderVideoSec < 0.0 || Math.abs(mpvSec - renderVideoSec) > 0.75) {
                        boolean seeked = (renderVideoSec >= 0.0 && Math.abs(mpvSec - renderVideoSec) > 0.75);
                        renderVideoSec = mpvSec;
                        clockDriftRate = 0.0;
                        if (seeked) {
                            synchronized (renderLock) {
                                clearActiveTracks();
                                seekDanmakuIndex((float) renderVideoSec);
                                resetLastDrawnState();
                            }
                        }
                    } else if (!isPaused) {
                        double drift = mpvSec - renderVideoSec;
                        clockDriftRate = Math.max(-0.03, Math.min(0.03, drift * 0.25));
                    } else {
                        renderVideoSec = pSec;
                        clockDriftRate = 0.0;
                    }
                }
            }
        }

        // 3. Monotonic forward advance on every rendered OpenXR frame
        if (!isPaused && renderVideoSec >= 0.0) {
            double effectiveSpeed = Math.max(0.0, playbackSpeed + clockDriftRate);
            renderVideoSec += dtSec * effectiveSpeed;
        }

        if (renderVideoSec < 0.0) return;
        float curSec = (float) renderVideoSec;

        ensureSegmentsForTime(curSec);

        synchronized (renderLock) {
            if (!running) return;

            if (segmentDataUpdated) {
                segmentDataUpdated = false;
                resetLastDrawnState();
            }

            int gen = this.scaleGeneration;
            if (gen != appliedScaleGeneration) {
                appliedScaleGeneration = gen;
                float sr_ = getDanmakuScaleRatio();
                for (int i = 0; i < activeList.size(); i++) {
                    ActiveDanmaku ad = activeList.get(i);
                    ad.sprite = null;
                    ensureSprite(ad);
                    if (ad.item.mode != 4 && ad.item.mode != 5) {
                        if (danmakuSpanIdx == 3) {
                            float Wv_ = (spanX > 1.01f) ? (canvasW / spanX) : canvasW;
                            float trackR_ = 100f * sr_;
                            float Ltotal_ = 2f * Wv_ + 2f * (float)(Math.PI * trackR_);
                            ad.speedPxPerSec = Ltotal_ / 10.5f;
                        } else {
                            ad.speedPxPerSec = calcMonoScrollSpeed(ad.width, sr_);
                        }
                    }
                }
                resetLastDrawnState();
            }

            if (danmakuEnabled) {
                updateActiveDanmaku(curSec);
            } else {
                if (isLiveStreamMode) {
                    liveQueue.clear();
                }
                if (!activeList.isEmpty()) {
                    clearActiveTracks();
                }
            }

            String ccText = subtitleEnabled ? resolveSubtitleText(curSec) : null;
            boolean hasDanmaku = (danmakuEnabled && !activeList.isEmpty());
            boolean hasCc = (ccText != null && !ccText.isEmpty());
            boolean hasContent = hasDanmaku || hasCc;

            if (!hasContent) {
                if (!wasCanvasEmpty && canvas != null && bitmap != null) {
                    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
                    AiDepthNative.setDanmakuUploadHeight(canvasH);
                    bridge.setDanmakuBitmap(bitmap);
                    wasCanvasEmpty = true;
                    resetLastDrawnState();
                }
                bridge.setDanmakuVisible(false);
                return;
            }

            boolean sameCc = (lastDrawnCcText == null) ? (ccText == null) : lastDrawnCcText.equals(ccText);
            if (!wasCanvasEmpty
                    && !Float.isNaN(lastDrawnSec)
                    && Math.abs(curSec - lastDrawnSec) < 0.0001f
                    && activeList.size() == lastDrawnActiveCount
                    && sameCc
                    && appliedScaleGeneration == lastDrawnScaleGen
                    && danmakuSpanIdx == lastDrawnSpanIdx
                    && densityIdx == lastDrawnDensityIdx
                    && danmakuDistIdx == lastDrawnDistIdx
                    && danmakuVOut == lastDrawnVOut
                    && Math.abs(subtitleScale - lastDrawnSubScale) < 0.001f
                    && fixedDanmakuEnabled == lastDrawnFixed
                    && canvasW == lastDrawnWidth
                    && canvasH == lastDrawnHeight) {
                bridge.setDanmakuVisible(true);
                return;
            }

            bridge.setDanmakuVisible(true);
            if (canvas != null && bitmap != null) {
                int clearH = (danmakuSpanIdx == 3 || hasCc || prevCpuBottomPx <= 0 || prevCpuBottomPx > canvasH)
                        ? canvasH : prevCpuBottomPx;
                if (clearH < canvasH) {
                    canvas.save();
                    canvas.clipRect(0, 0, canvasW, clearH);
                    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
                    canvas.restore();
                } else {
                    canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);
                }

                curFrameMaxBottomPx = 0;
                if (hasDanmaku) {
                    drawActiveDanmaku(curSec);
                }

                if (hasCc) {
                    drawCcSubtitle(ccText);
                    curFrameMaxBottomPx = canvasH;
                }

                int frameBottom = (danmakuSpanIdx == 3 || hasCc)
                        ? canvasH
                        : Math.min(canvasH, Math.max(64, (curFrameMaxBottomPx + 15) & ~15));
                prevCpuBottomPx = frameBottom;
                recentBottomPx[recentBottomIdx] = frameBottom;
                recentBottomIdx = (recentBottomIdx + 1) % recentBottomPx.length;
                int uploadH = frameBottom;
                for (int i = 0; i < recentBottomPx.length; i++) {
                    if (recentBottomPx[i] > uploadH) uploadH = recentBottomPx[i];
                }
                if (uploadH > canvasH) uploadH = canvasH;
                AiDepthNative.setDanmakuUploadHeight(uploadH);
                bridge.setDanmakuBitmap(bitmap);
                wasCanvasEmpty = false;
                lastDrawnSec = curSec;
                lastDrawnActiveCount = activeList.size();
                lastDrawnCcText = ccText;
                lastDrawnScaleGen = appliedScaleGeneration;
                lastDrawnSpanIdx = danmakuSpanIdx;
                lastDrawnDensityIdx = densityIdx;
                lastDrawnDistIdx = danmakuDistIdx;
                lastDrawnVOut = danmakuVOut;
                lastDrawnSubScale = subtitleScale;
                lastDrawnFixed = fixedDanmakuEnabled;
                lastDrawnWidth = canvasW;
                lastDrawnHeight = canvasH;
            }
        }
    }

    private void ensureSprite(ActiveDanmaku ad) {
        float mr = getDanmakuScaleRatio();
        float scale = this.danmakuScale * mr;
        if (ad.sprite != null && !ad.sprite.isRecycled() && Math.abs(ad.spriteScale - scale) < 0.01f) {
            return;
        }
        ad.sprite = null;
        float fontSize = computeFontSize(ad.item.baseFontSize) * scale;
        fillPaint.setTextSize(fontSize);
        strokePaint.setTextSize(fontSize);
        strokePaint.setStrokeWidth(Math.max(1.2f, fontSize * 0.095f));

        String text = ad.item.text;
        float textW;
        if (danmakuSpanIdx == 3) {
            int textLen = text.length();
            ad.charWidths = new float[textLen];
            ad.charMidOffsets = new float[textLen];
            float curOffset = 0f;
            for (int c = 0; c < textLen; c++) {
                float cw = fillPaint.measureText(text, c, c + 1);
                ad.charWidths[c] = cw;
                ad.charMidOffsets[c] = curOffset + cw * 0.5f;
                curOffset += cw;
            }
            textW = curOffset;
        } else {
            textW = fillPaint.measureText(text);
        }
        Paint.FontMetrics fm = fillPaint.getFontMetrics();
        int bmpW = Math.max(8, (int) Math.ceil(textW + 14f * mr));
        int bmpH = Math.max(8, (int) Math.ceil(fm.descent - fm.ascent + 12f * mr));
        if (bmpW > canvasW * 2) bmpW = canvasW * 2;

        Bitmap sp = Bitmap.createBitmap(bmpW, bmpH, Bitmap.Config.ARGB_8888);
        Canvas sc = new Canvas(sp);
        float drawX = 6f * mr;
        float baseline = drawX - fm.ascent;
        fillPaint.setColor(ad.item.color);
        sc.drawText(ad.item.text, drawX, baseline, strokePaint);
        sc.drawText(ad.item.text, drawX, baseline, fillPaint);

        ad.sprite = sp;
        ad.width = bmpW;
        ad.spriteScale = scale;
        ad.spritePadX = drawX;
        ad.spriteCenterOffsetY = baseline + (fm.ascent + fm.descent) * 0.5f;
        if (danmakuSpanIdx == 3) {
            int textLen = text.length();
            if (ad.charSrcLeft == null || ad.charSrcLeft.length != textLen) {
                ad.charSrcLeft = new int[textLen];
                ad.charSrcRight = new int[textLen];
            }
            for (int c = 0; c < textLen; c++) {
                ad.charSrcLeft[c] = (c == 0) ? 0 : ad.charSrcRight[c - 1];
                if (c == textLen - 1) {
                    ad.charSrcRight[c] = bmpW;
                } else {
                    float rightEdge = drawX + ad.charMidOffsets[c] + ad.charWidths[c] * 0.5f;
                    int rPx = Math.round(rightEdge);
                    if (rPx < ad.charSrcLeft[c]) rPx = ad.charSrcLeft[c];
                    if (rPx > bmpW) rPx = bmpW;
                    ad.charSrcRight[c] = rPx;
                }
            }
        }
    }

    private void clearActiveSprites() {
        for (int i = 0; i < activeList.size(); i++) {
            ActiveDanmaku ad = activeList.get(i);
            ad.sprite = null;
        }
        activeList.clear();
    }

    private void clearActiveTracks() {
        clearActiveSprites();
        pendingScroll.clear();
        pendingTop.clear();
        pendingBottom.clear();
        lastScrollSpawnSec = -100f;
        for (int i = 0; i < scrollTracks.length; i++) scrollTracks[i] = null;
        for (int i = 0; i < topTracks.length; i++) topTracks[i] = null;
        for (int i = 0; i < bottomTracks.length; i++) bottomTracks[i] = null;
    }

    private float calcMonoScrollSpeed(float w, float scaleRatio) {
        float baseSpeed = (canvasW + 240f * scaleRatio) / SCROLL_DURATION_SEC;
        return Math.min(baseSpeed * 1.12f, baseSpeed * 0.95f + (w / SCROLL_DURATION_SEC) * 0.18f);
    }

    private void enqueuePendingItem(DanmakuItem item) {
        if (item == null) return;
        if (item.mode == 5) {
            if (fixedDanmakuEnabled && pendingTop.size() < 12) pendingTop.addLast(item);
        } else if (item.mode == 4) {
            if (fixedDanmakuEnabled && pendingBottom.size() < 10) pendingBottom.addLast(item);
        } else {
            if (pendingScroll.size() < MAX_PENDING_SCROLL) pendingScroll.addLast(item);
        }
    }

    private void seekDanmakuIndex(float targetSec) {
        float windowStart = Math.max(0f, targetSec - 2.0f);
        synchronized (allDanmaku) {
            int lo = 0;
            int hi = allDanmaku.size() - 1;
            int ans = allDanmaku.size();
            while (lo <= hi) {
                int mid = (lo + hi) >>> 1;
                if (allDanmaku.get(mid).timeSec >= windowStart) {
                    ans = mid;
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            nextDanmakuIdx = ans;
        }
    }

    private void updateActiveDanmaku(float curSec) {
        for (int i = activeList.size() - 1; i >= 0; i--) {
            ActiveDanmaku ad = activeList.get(i);
            if (curSec > ad.endSec || curSec < ad.startSec - 0.5f) {
                ad.sprite = null;
                activeList.remove(i);
                if (ad.item.mode == 5 && ad.track >= 0 && ad.track < topTracks.length && topTracks[ad.track] == ad) {
                    topTracks[ad.track] = null;
                } else if (ad.item.mode == 4 && ad.track >= 0 && ad.track < bottomTracks.length && bottomTracks[ad.track] == ad) {
                    bottomTracks[ad.track] = null;
                } else if (ad.track >= 0 && ad.track < scrollTracks.length && scrollTracks[ad.track] == ad) {
                    scrollTracks[ad.track] = null;
                }
            }
        }

        int dIdx = Math.max(0, Math.min(3, densityIdx));
        if (danmakuSpanIdx == 3) {
            // Cap surround-track danmaku strictly to 25% density (index 0).
            dIdx = 0;
        }
        int maxActive = DENSITY_MAX_ACTIVE[dIdx];
        if (danmakuSpanIdx != 3 && spanX > 1.05f) {
            maxActive = Math.min(MAX_ACTIVE_DANMAKU, Math.round(maxActive * spanX));
        }

        if (isLiveStreamMode && !liveQueue.isEmpty()) {
            int pulledLive = 0;
            DanmakuItem liveItem;
            while (pulledLive < 32 && (liveItem = liveQueue.poll()) != null) {
                enqueuePendingItem(new DanmakuItem(curSec, liveItem.mode, liveItem.color, liveItem.baseFontSize, liveItem.text));
                pulledLive++;
            }
        }

        synchronized (allDanmaku) {
            while (nextDanmakuIdx < allDanmaku.size()) {
                DanmakuItem item = allDanmaku.get(nextDanmakuIdx);
                if (item.timeSec > curSec + 0.05f) {
                    break;
                }
                nextDanmakuIdx++;
                if (item.timeSec < curSec - 4.5f) {
                    continue;
                }
                enqueuePendingItem(item);
            }
        }

        while (!pendingScroll.isEmpty() && pendingScroll.peekFirst().timeSec < curSec - 4.5f) {
            pendingScroll.pollFirst();
        }
        while (!pendingTop.isEmpty() && pendingTop.peekFirst().timeSec < curSec - 3.5f) {
            pendingTop.pollFirst();
        }
        while (!pendingBottom.isEmpty() && pendingBottom.peekFirst().timeSec < curSec - 3.5f) {
            pendingBottom.pollFirst();
        }

        if (curSec < lastScrollSpawnSec) {
            lastScrollSpawnSec = curSec - 1.0f;
        }

        float scaleRatio = getDanmakuScaleRatio();
        float trackH = 40f * danmakuScale * scaleRatio;
        float areaH = (danmakuSpanIdx == 3) ? (canvasH * 0.5f) : canvasH;
        int estTracks = Math.min(MAX_SCROLL_TRACKS, Math.max(2, (int) ((areaH * DENSITY_HEIGHT_FRAC[dIdx]) / trackH)));
        float minLaunchIntervalSec = (danmakuSpanIdx == 3)
                ? 0.075f
                : Math.max(0.014f, 0.25f / Math.max(2, estTracks));

        int maxPerTick = (danmakuSpanIdx == 3) ? 1 : (pendingScroll.size() > estTracks ? 2 : 1);
        int launchedScroll = 0;
        while (launchedScroll < maxPerTick && !pendingScroll.isEmpty() && activeList.size() < maxActive) {
            if (launchedScroll == 0 && (curSec - lastScrollSpawnSec) < minLaunchIntervalSec) {
                break;
            }
            DanmakuItem head = pendingScroll.peekFirst();
            if (head != null && trySpawnDanmaku(head, curSec, dIdx)) {
                pendingScroll.pollFirst();
                lastScrollSpawnSec = curSec;
                launchedScroll++;
            } else {
                break;
            }
        }

        if (fixedDanmakuEnabled && activeList.size() < maxActive && !pendingTop.isEmpty()) {
            DanmakuItem head = pendingTop.peekFirst();
            if (head != null && trySpawnDanmaku(head, curSec, dIdx)) {
                pendingTop.pollFirst();
            }
        }
        if (fixedDanmakuEnabled && activeList.size() < maxActive && !pendingBottom.isEmpty()) {
            DanmakuItem head = pendingBottom.peekFirst();
            if (head != null && trySpawnDanmaku(head, curSec, dIdx)) {
                pendingBottom.pollFirst();
            }
        }
    }

    private float computeFontSize(float rawSize) {
        if (rawSize <= 18f) return 26f;
        if (rawSize >= 30f) return 36f;
        return 31f;
    }

    private boolean trySpawnDanmaku(DanmakuItem item, float curSec, int dIdx) {
        if (!fixedDanmakuEnabled && isFixedDanmakuMode(item.mode)) {
            return false;
        }
        ActiveDanmaku ad = new ActiveDanmaku();
        ad.item = item;
        ad.startSec = curSec;
        ensureSprite(ad);
        float w = ad.width;
        int maxPinTracks = Math.min(MAX_TOP_TRACKS, DENSITY_PIN_TRACKS[dIdx]);

        if (item.mode == 5) {
            for (int t = 0; t < maxPinTracks; t++) {
                if (topTracks[t] == null || curSec >= topTracks[t].endSec) {
                    ad.track = t;
                    ad.speedPxPerSec = 0f;
                    ad.endSec = curSec + FIXED_DURATION_SEC;
                    topTracks[t] = ad;
                    activeList.add(ad);
                    return true;
                }
            }
            ad.sprite = null;
            return false;
        } else if (item.mode == 4) {
            int maxBotTracks = Math.min(MAX_BOTTOM_TRACKS, maxPinTracks);
            for (int t = 0; t < maxBotTracks; t++) {
                if (bottomTracks[t] == null || curSec >= bottomTracks[t].endSec) {
                    ad.track = t;
                    ad.speedPxPerSec = 0f;
                    ad.endSec = curSec + FIXED_DURATION_SEC;
                    bottomTracks[t] = ad;
                    activeList.add(ad);
                    return true;
                }
            }
            ad.sprite = null;
            return false;
        } else if (danmakuSpanIdx == 3) {
            float scaleRatio = getDanmakuScaleRatio();
            float Wv = (spanX > 1.01f) ? (canvasW / spanX) : canvasW;
            float trackR = 100f * scaleRatio;
            float L1 = (float) (Math.PI * trackR);
            float Ltotal = 2f * Wv + 2f * L1;
            float loopDuration = 10.5f;
            float speed = Ltotal / loopDuration;

            float heightFrac = DENSITY_HEIGHT_FRAC[dIdx];
            float minGap = (DENSITY_MIN_GAP[dIdx] + 44f) * scaleRatio;
            float videoH = canvasH * 0.5f;
            float trackH = 40f * danmakuScale * scaleRatio;
            int maxTracks = Math.min(MAX_SCROLL_TRACKS, Math.max(2, (int) ((videoH * heightFrac) / trackH)));
            for (int t = 0; t < maxTracks; t++) {
                ActiveDanmaku prev = scrollTracks[t];
                if (prev == null || curSec >= prev.endSec) {
                    ad.track = t;
                    ad.speedPxPerSec = speed;
                    ad.startSec = curSec;
                    ad.endSec = curSec + loopDuration;
                    scrollTracks[t] = ad;
                    activeList.add(ad);
                    return true;
                }
                float prevElapsed = curSec - prev.startSec;
                float prevDist = prevElapsed * prev.speedPxPerSec;
                if (prevDist - prev.width > minGap) {
                    ad.track = t;
                    ad.speedPxPerSec = speed;
                    ad.startSec = curSec;
                    ad.endSec = curSec + loopDuration;
                    scrollTracks[t] = ad;
                    activeList.add(ad);
                    return true;
                }
            }
            ad.sprite = null;
            return false;
        } else {
            float scaleRatio = getDanmakuScaleRatio();
            float speed = calcMonoScrollSpeed(w, scaleRatio);
            float duration = (canvasW + w) / speed;
            float heightFrac = DENSITY_HEIGHT_FRAC[dIdx];
            float minGap = DENSITY_MIN_GAP[dIdx] * scaleRatio;
            float videoH = canvasH;
            float trackH = 40f * danmakuScale * scaleRatio;
            int maxTracks = Math.min(MAX_SCROLL_TRACKS, Math.max(2, (int) ((videoH * heightFrac) / trackH)));
            for (int t = 0; t < maxTracks; t++) {
                ActiveDanmaku prev = scrollTracks[t];
                if (prev == null || curSec >= prev.endSec) {
                    ad.track = t;
                    ad.speedPxPerSec = speed;
                    ad.endSec = curSec + duration;
                    scrollTracks[t] = ad;
                    activeList.add(ad);
                    return true;
                }
                float prevElapsed = curSec - prev.startSec;
                float prevRightEdge = canvasW - prevElapsed * prev.speedPxPerSec + prev.width;
                if (prevRightEdge < canvasW - minGap) {
                    float prevRemainSec = prev.endSec - curSec;
                    float gap = canvasW - prevRightEdge;
                    if (speed <= prev.speedPxPerSec || (gap / (speed - prev.speedPxPerSec)) > prevRemainSec) {
                        ad.track = t;
                        ad.speedPxPerSec = speed;
                        ad.endSec = curSec + duration;
                        scrollTracks[t] = ad;
                        activeList.add(ad);
                        return true;
                    }
                }
            }
            ad.sprite = null;
            return false;
        }
    }

    private void drawActiveDanmaku(float curSec) {
        float scaleRatio = getDanmakuScaleRatio();
        float modeScaleRatio = getModeScaleRatio();
        float trackHeight = 40f * danmakuScale * scaleRatio;
        float topPadding = 12f * scaleRatio;

        if (danmakuSpanIdx != 3) {
            // Normal mono rendering (modes 0, 1, 2)
            float videoTop = (spanY > 1.01f) ? (canvasH * (0.5f - 0.5f / spanY)) : 0f;
            float videoBottom = (spanY > 1.01f) ? (canvasH * (0.5f + 0.5f / spanY)) : canvasH;
            float fadeDist = Math.max(90f * scaleRatio, canvasW * 0.08f);

            canvas.save();
            canvas.clipRect(2f, 0f, canvasW - 2f, canvasH);
            for (int i = 0; i < activeList.size(); i++) {
                ActiveDanmaku ad = activeList.get(i);
                if (ad.sprite == null || ad.sprite.isRecycled()) {
                    ensureSprite(ad);
                }
                if (ad.sprite == null) continue;
                float x;
                float y;
                float alphaFactor = 1.0f;
                if (ad.item.mode == 5) {
                    x = (canvasW - ad.width) * 0.5f;
                    y = videoTop + topPadding + ad.track * trackHeight;
                    float elapsed = curSec - ad.startSec;
                    float remain = ad.endSec - curSec;
                    if (elapsed < 0.35f) alphaFactor = Math.min(alphaFactor, Math.max(0.0f, elapsed / 0.35f));
                    if (remain < 0.35f) alphaFactor = Math.min(alphaFactor, Math.max(0.0f, remain / 0.35f));
                } else if (ad.item.mode == 4) {
                    x = (canvasW - ad.width) * 0.5f;
                    y = videoBottom - Math.max(110f * scaleRatio, 100f * modeScaleRatio) - (ad.track + 1) * trackHeight;
                    float elapsed = curSec - ad.startSec;
                    float remain = ad.endSec - curSec;
                    if (elapsed < 0.35f) alphaFactor = Math.min(alphaFactor, Math.max(0.0f, elapsed / 0.35f));
                    if (remain < 0.35f) alphaFactor = Math.min(alphaFactor, Math.max(0.0f, remain / 0.35f));
                } else {
                    float elapsed = curSec - ad.startSec;
                    x = canvasW - elapsed * ad.speedPxPerSec;
                    y = topPadding + ad.track * trackHeight;
                    if (x > canvasW - fadeDist) {
                        alphaFactor = Math.min(alphaFactor, Math.max(0.0f, (canvasW - x) / fadeDist));
                    }
                    if (x + ad.width < fadeDist) {
                        alphaFactor = Math.min(alphaFactor, Math.max(0.0f, (x + ad.width) / fadeDist));
                    }
                }
                int alpha = Math.max(0, Math.min(255, Math.round(255f * alphaFactor)));
                spritePaint.setAlpha(alpha);
                canvas.drawBitmap(ad.sprite, x, y, spritePaint);
                int bot = (int) Math.ceil(y + ad.sprite.getHeight()) + 4;
                if (bot > curFrameMaxBottomPx) curFrameMaxBottomPx = bot;
            }
            canvas.restore();
            spritePaint.setAlpha(255);
            return;
        }

        // =========================================================================
        // 3D Stadium Track: True Stereoscopic Over-Under + Conformal Surface Bending
        // Geometry and occlusion are precomputed ONCE per frame and replayed via
        // cached sprite blits / slices for both eyes (zero per-char drawText rasterization).
        // =========================================================================
        int eyeH = canvasH / 2;
        float Wv = canvasW / spanX;
        float Xc = canvasW * 0.5f;
        float Yc = eyeH * 0.5f;
        float Xl = Xc - Wv * 0.5f;
        float Xr = Xc + Wv * 0.5f;
        float trackR = 100f * scaleRatio;
        float L1 = (float) (Math.PI * trackR);
        float L2 = Wv;
        float L3 = L1;
        float L4 = Wv;
        float Ltotal = 2f * Wv + 2f * L1;
        float virtualD = 2.2f * Math.max(Wv, eyeH * 1.4f);

        float videoTop = (spanY > 1.01f) ? (eyeH * (0.5f - 0.5f / spanY)) : 0f;
        float videoBottom = (spanY > 1.01f) ? (eyeH * (0.5f + 0.5f / spanY)) : eyeH;

        // Dynamic stereo disparity derived from HUD "弹幕距离" setting (danmakuDistIdx),
        // scaled proportionally with getModeScaleRatio() to preserve exact angular VR depth.
        float frontDisparity;
        float backDisparity;
        if (danmakuDistIdx == 0) {
            frontDisparity = 0f;
            backDisparity = -14f * scaleRatio;
        } else if (danmakuDistIdx == 1) {
            frontDisparity = 6f * scaleRatio;
            backDisparity = -10f * scaleRatio;
        } else if (danmakuDistIdx == 2) {
            frontDisparity = 14f * scaleRatio;
            backDisparity = -8f * scaleRatio;
        } else {
            frontDisparity = 24f * scaleRatio;
            backDisparity = -6f * scaleRatio;
        }

        float depthFactor = (danmakuDistIdx == 0) ? 0.99f : ((danmakuDistIdx == 1) ? 0.88f : ((danmakuDistIdx == 2) ? 0.76f : 0.64f));
        float vidWMat = 2.0f * Math.max(0.2f, screenViewDist) * (float) Math.tan(Math.max(0.05f, screenViewFov) * 0.5f);
        float ipdShiftPx = ((0.032f * (1.0f - depthFactor)) / Math.max(0.05f, depthFactor * vidWMat)) * Wv;
        float maxShift = Math.abs(backDisparity) + ipdShiftPx;

        passCount[0] = 0;
        passCount[1] = 0;

        for (int i = 0; i < activeList.size(); i++) {
            ActiveDanmaku ad = activeList.get(i);
            float elapsed = curSec - ad.startSec;

            // Mode 5 (top) & Mode 4 (bottom) are pinned on-screen: drawn once in front pass (pass 1)
            if (ad.item.mode == 5) {
                if (ad.sprite == null || ad.sprite.isRecycled()) ensureSprite(ad);
                if (ad.sprite == null) continue;
                float x = (canvasW - ad.width) * 0.5f;
                float localY = videoTop + topPadding + ad.track * trackHeight;
                float remain = ad.endSec - curSec;
                float af = 1.0f;
                if (elapsed < 0.35f) af = Math.min(af, Math.max(0.0f, elapsed / 0.35f));
                if (remain < 0.35f) af = Math.min(af, Math.max(0.0f, remain / 0.35f));
                int alpha = Math.max(0, Math.min(255, Math.round(255f * af)));
                appendWholeSpriteOp(1, ad.sprite, x, 0f, localY, alpha);
                continue;
            } else if (ad.item.mode == 4) {
                if (ad.sprite == null || ad.sprite.isRecycled()) ensureSprite(ad);
                if (ad.sprite == null) continue;
                float x = (canvasW - ad.width) * 0.5f;
                float localY = videoBottom - Math.max(110f * scaleRatio, 100f * modeScaleRatio) - (ad.track + 1) * trackHeight;
                float remain = ad.endSec - curSec;
                float af = 1.0f;
                if (elapsed < 0.35f) af = Math.min(af, Math.max(0.0f, elapsed / 0.35f));
                if (remain < 0.35f) af = Math.min(af, Math.max(0.0f, remain / 0.35f));
                int alpha = Math.max(0, Math.min(255, Math.round(255f * af)));
                appendWholeSpriteOp(1, ad.sprite, x, 0f, localY, alpha);
                continue;
            }

            if (ad.charWidths == null || ad.charMidOffsets == null
                    || ad.charSrcLeft == null || ad.charSrcRight == null
                    || ad.sprite == null || ad.sprite.isRecycled()) {
                ensureSprite(ad);
            }
            if (ad.sprite == null) continue;

            float sHead = elapsed * ad.speedPxPerSec;
            float sTail = sHead - ad.width;
            if (sHead < 0f || sTail >= Ltotal) continue;

            int spH = ad.sprite.getHeight();
            float halfTextH = Math.max(4f * scaleRatio, (spH - 12f * scaleRatio) * 0.5f);
            float baseY = (danmakuVOut ? 0f : videoTop) + topPadding + ad.track * trackHeight + halfTextH;

            // Fast path: whole sprite is strictly on the uncurved front straight (worldZ == +trackR, k == 1, scaleU == 1)
            if (sTail >= L1 && sHead <= L1 + L2) {
                float spriteLeftMono = (Xr - (sHead - L1)) - ad.spritePadX;
                float spriteTopLocal = baseY - ad.spriteCenterOffsetY;
                appendWholeSpriteOp(1, ad.sprite, spriteLeftMono, frontDisparity, spriteTopLocal, 255);
                continue;
            }

            int textLen = ad.charWidths.length;
            if (textLen == 0) continue;

            float gcy = ad.spriteCenterOffsetY;
            for (int c = 0; c < textLen; c++) {
                float sChar = sHead - ad.charMidOffsets[c];
                if (sChar < 0f || sChar >= Ltotal) continue;

                int srcL = ad.charSrcLeft[c];
                int srcR = ad.charSrcRight[c];
                if (srcR <= srcL) continue;

                float worldX, worldZ;
                boolean isMirrored;
                float scaleU = 1.0f;

                if (sChar < L1) {
                    float u = sChar / L1;
                    float alpha = (float) (u * Math.PI);
                    worldX = Xr + (float) (trackR * Math.sin(alpha));
                    worldZ = (float) (-trackR * Math.cos(alpha));
                    isMirrored = (worldZ < 0f);
                    scaleU = 0.45f + 0.55f * Math.abs((float) Math.cos(alpha));
                } else if (sChar < L1 + L2) {
                    float u = (sChar - L1) / L2;
                    worldX = Xr - u * Wv;
                    worldZ = trackR;
                    isMirrored = false;
                    scaleU = 1.0f;
                } else if (sChar < L1 + L2 + L3) {
                    float u = (sChar - L1 - L2) / L3;
                    float beta = (float) (u * Math.PI);
                    worldX = Xl - (float) (trackR * Math.sin(beta));
                    worldZ = (float) (trackR * Math.cos(beta));
                    isMirrored = (worldZ < 0f);
                    scaleU = 0.45f + 0.55f * Math.abs((float) Math.cos(beta));
                } else {
                    float u = (sChar - L1 - L2 - L3) / L4;
                    worldX = Xl + u * Wv;
                    worldZ = -trackR;
                    isMirrored = true;
                    scaleU = 1.0f;
                }

                float relZ = worldZ - trackR;
                float k = virtualD / (virtualD - relZ);
                float tDepth = (worldZ + trackR) / (2f * trackR);
                float disp = backDisparity + tDepth * (frontDisparity - backDisparity);

                float monoCenterX = Xc + (worldX - Xc) * k;
                float localY = Yc + (baseY - Yc) * k;
                float gcx = ad.spritePadX + ad.charMidOffsets[c];
                float sx = scaleU * k;
                float sy = k;

                // Projected occlusion check: skip slices deep inside video bounds before drawing,
                // while clipOutRect during eye replay cuts partially emerging slices right at the edge.
                boolean vCovered = !danmakuVOut || ((localY - gcy * sy) >= videoTop && (localY + (spH - gcy) * sy) <= videoBottom);
                float charHalfW = Math.max(ad.charWidths[c] * sx * 0.65f, 10f * scaleRatio);
                if (worldZ < 0f && vCovered && (monoCenterX - charHalfW - maxShift) >= Xl && (monoCenterX + charHalfW + maxShift) <= Xr) {
                    continue;
                }

                float alphaFactor = 1.0f;
                if (worldZ < 0f) {
                    float edgeDist = (monoCenterX > Xr) ? (monoCenterX - Xr) : ((monoCenterX < Xl) ? (Xl - monoCenterX) : 0f);
                    if (!vCovered && monoCenterX >= Xl && monoCenterX <= Xr) {
                        alphaFactor = 0.45f;
                    } else {
                        float fadeBand = 28f * scaleRatio;
                        alphaFactor = 0.62f + 0.38f * Math.min(1.0f, edgeDist / fadeBand);
                    }
                }
                if (alphaFactor <= 0.02f) continue;

                int alpha = Math.max(0, Math.min(255, Math.round(255f * alphaFactor)));
                int pass = (worldZ < 0f) ? 0 : 1;
                appendSliceOp(pass, ad.sprite, srcL, srcR, monoCenterX, disp, localY,
                        (srcL - gcx) * sx, -gcy * sy, (srcR - gcx) * sx, (spH - gcy) * sy,
                        alpha, isMirrored);
            }
        }

        int lastAlpha = -1;
        for (int eye = 0; eye < 2; eye++) {
            float eyeOffsetY = eye * eyeH;
            float eyeSign = (eye == 0) ? 1.0f : -1.0f;
            float eyeVidShift = (eye == 0) ? -ipdShiftPx : ipdShiftPx;
            float clipL = Xl + eyeVidShift;
            float clipR = Xr + eyeVidShift;
            for (int pass = 0; pass < SURROUND_PASSES; pass++) {
                int count = passCount[pass];
                if (count <= 0) continue;
                Bitmap[] spArr = passSprite[pass];
                int[] srcLArr = passSrcL[pass];
                int[] srcRArr = passSrcR[pass];
                float[] monoXArr = passMonoX[pass];
                float[] dispArr = passDisp[pass];
                float[] localYArr = passLocalY[pass];
                float[] relLArr = passRelL[pass];
                float[] relTArr = passRelT[pass];
                float[] relRArr = passRelR[pass];
                float[] relBArr = passRelB[pass];
                int[] alphaArr = passAlpha[pass];
                boolean[] mirArr = passMirrored[pass];

                canvas.save();
                canvas.clipRect(2f, eyeOffsetY, canvasW - 2f, eyeOffsetY + eyeH);
                if (pass == 0) {
                    canvas.clipOutRect(clipL, eyeOffsetY + videoTop, clipR, eyeOffsetY + videoBottom);
                }

                for (int k = 0; k < count; k++) {
                    Bitmap sp = spArr[k];
                    int a = alphaArr[k];
                    if (a != lastAlpha) {
                        spritePaint.setAlpha(a);
                        lastAlpha = a;
                    }
                    float dx = monoXArr[k] + eyeSign * dispArr[k];
                    float dy = eyeOffsetY + localYArr[k];
                    int sL = srcLArr[k];
                    if (sL < 0) {
                        canvas.drawBitmap(sp, dx, dy, spritePaint);
                    } else {
                        charSrcRect.set(sL, 0, srcRArr[k], sp.getHeight());
                        charDstRect.set(dx + relLArr[k], dy + relTArr[k], dx + relRArr[k], dy + relBArr[k]);
                        if (mirArr[k]) {
                            canvas.save();
                            canvas.scale(-1f, 1f, dx, dy);
                            canvas.drawBitmap(sp, charSrcRect, charDstRect, spritePaint);
                            canvas.restore();
                        } else {
                            canvas.drawBitmap(sp, charSrcRect, charDstRect, spritePaint);
                        }
                    }
                }
                canvas.restore();
            }
        }

        for (int pass = 0; pass < SURROUND_PASSES; pass++) {
            int count = passCount[pass];
            if (count > 0 && passSprite[pass] != null) {
                java.util.Arrays.fill(passSprite[pass], 0, count, null);
            }
            passCount[pass] = 0;
        }
        spritePaint.setAlpha(255);
    }

    private int allocSurroundOp(int pass) {
        int idx = passCount[pass];
        int cap = passCap[pass];
        if (idx >= cap) {
            int newCap = Math.max(64, Math.max(cap * 2, idx + 32));
            Bitmap[] newSp = new Bitmap[newCap];
            int[] newSrcL = new int[newCap];
            int[] newSrcR = new int[newCap];
            float[] newMonoX = new float[newCap];
            float[] newDisp = new float[newCap];
            float[] newLocalY = new float[newCap];
            float[] newRelL = new float[newCap];
            float[] newRelT = new float[newCap];
            float[] newRelR = new float[newCap];
            float[] newRelB = new float[newCap];
            int[] newAlpha = new int[newCap];
            boolean[] newMir = new boolean[newCap];
            if (cap > 0) {
                System.arraycopy(passSprite[pass], 0, newSp, 0, cap);
                System.arraycopy(passSrcL[pass], 0, newSrcL, 0, cap);
                System.arraycopy(passSrcR[pass], 0, newSrcR, 0, cap);
                System.arraycopy(passMonoX[pass], 0, newMonoX, 0, cap);
                System.arraycopy(passDisp[pass], 0, newDisp, 0, cap);
                System.arraycopy(passLocalY[pass], 0, newLocalY, 0, cap);
                System.arraycopy(passRelL[pass], 0, newRelL, 0, cap);
                System.arraycopy(passRelT[pass], 0, newRelT, 0, cap);
                System.arraycopy(passRelR[pass], 0, newRelR, 0, cap);
                System.arraycopy(passRelB[pass], 0, newRelB, 0, cap);
                System.arraycopy(passAlpha[pass], 0, newAlpha, 0, cap);
                System.arraycopy(passMirrored[pass], 0, newMir, 0, cap);
            }
            passSprite[pass] = newSp;
            passSrcL[pass] = newSrcL;
            passSrcR[pass] = newSrcR;
            passMonoX[pass] = newMonoX;
            passDisp[pass] = newDisp;
            passLocalY[pass] = newLocalY;
            passRelL[pass] = newRelL;
            passRelT[pass] = newRelT;
            passRelR[pass] = newRelR;
            passRelB[pass] = newRelB;
            passAlpha[pass] = newAlpha;
            passMirrored[pass] = newMir;
            passCap[pass] = newCap;
        }
        passCount[pass] = idx + 1;
        return idx;
    }

    private void appendWholeSpriteOp(int pass, Bitmap sp, float monoX, float disp, float localY, int alpha) {
        int idx = allocSurroundOp(pass);
        passSprite[pass][idx] = sp;
        passSrcL[pass][idx] = -1;
        passMonoX[pass][idx] = monoX;
        passDisp[pass][idx] = disp;
        passLocalY[pass][idx] = localY;
        passAlpha[pass][idx] = alpha;
    }

    private void appendSliceOp(int pass, Bitmap sp, int srcL, int srcR,
                               float monoX, float disp, float localY,
                               float relL, float relT, float relR, float relB,
                               int alpha, boolean mirrored) {
        int idx = allocSurroundOp(pass);
        passSprite[pass][idx] = sp;
        passSrcL[pass][idx] = srcL;
        passSrcR[pass][idx] = srcR;
        passMonoX[pass][idx] = monoX;
        passDisp[pass][idx] = disp;
        passLocalY[pass][idx] = localY;
        passRelL[pass][idx] = relL;
        passRelT[pass][idx] = relT;
        passRelR[pass][idx] = relR;
        passRelB[pass][idx] = relB;
        passAlpha[pass][idx] = alpha;
        passMirrored[pass][idx] = mirrored;
    }

    private String resolveSubtitleText(float curSec) {
        if (isLiveStreamMode) {
            return null;
        }
        if (cachedMpvSubText != null && !cachedMpvSubText.isEmpty()) {
            return cachedMpvSubText;
        }
        synchronized (subtitleCues) {
            if (!subtitleCues.isEmpty()) {
                StringBuilder sb = null;
                for (int i = 0; i < subtitleCues.size(); i++) {
                    SubtitleCue cue = subtitleCues.get(i);
                    if (curSec >= cue.fromSec && curSec <= cue.toSec) {
                        if (sb == null) {
                            sb = new StringBuilder(cue.content);
                        } else {
                            sb.append('\n').append(cue.content);
                        }
                    }
                }
                if (sb != null) return sb.toString();
            }
        }
        return null;
    }

    private void drawCcSubtitle(String text) {
        String[] lines = text.split("\n");
        float scaleRatio = getModeScaleRatio();
        float fontSize = 34f * subtitleScale * scaleRatio;
        subTextPaint.setTextSize(fontSize);
        subStrokePaint.setTextSize(fontSize);
        subStrokePaint.setStrokeWidth(Math.max(1.5f, fontSize * 0.095f));

        float lineHeight = fontSize * 1.28f;
        float maxW = 0f;
        for (String line : lines) {
            float w = subTextPaint.measureText(line.trim());
            if (w > maxW) maxW = w;
        }
        if (maxW <= 0f) return;

        int eyeCount = (danmakuSpanIdx == 3) ? 2 : 1;
        int eyeH = (danmakuSpanIdx == 3) ? (canvasH / 2) : canvasH;

        float totalH = lines.length * lineHeight + 18f * scaleRatio;
        float padH = 26f * scaleRatio;
        float visibleVideoW = (spanX > 1.01f) ? (canvasW / spanX) : canvasW;
        float boxW = Math.min(visibleVideoW - 48f * scaleRatio, maxW + padH * 2f);
        float cx = canvasW * 0.5f;
        float videoBottom = (spanY > 1.01f) ? (eyeH * (0.5f + 0.5f / spanY)) : eyeH;
        float baseBottomY = videoBottom - Math.max(16f * scaleRatio, (eyeH / spanY) * 0.035f);
        float baseTopY = baseBottomY - totalH;

        for (int eye = 0; eye < eyeCount; eye++) {
            float eyeOffsetY = eye * eyeH;
            float topY = baseTopY + eyeOffsetY;
            float bottomY = baseBottomY + eyeOffsetY;

            subBoxRect.set(cx - boxW * 0.5f, topY, cx + boxW * 0.5f, bottomY);
            canvas.drawRoundRect(subBoxRect, 14f * scaleRatio, 14f * scaleRatio, subBgPaint);

            Paint.FontMetrics fm = subTextPaint.getFontMetrics();
            for (int i = 0; i < lines.length; i++) {
                String line = lines[i].trim();
                float lineCenterY = topY + 9f * scaleRatio + (i + 0.5f) * lineHeight;
                float baseline = lineCenterY - (fm.ascent + fm.descent) * 0.5f;
                canvas.drawText(line, cx, baseline, subStrokePaint);
                canvas.drawText(line, cx, baseline, subTextPaint);
            }
        }
    }

    private void checkFilesAndReload() {
        File filesDir = context.getFilesDir();
        long newCid = currentCid;
        File cidFile = new File(filesDir, "vr_current_cid.txt");
        if (cidFile.exists()) {
            try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(cidFile), StandardCharsets.UTF_8))) {
                String line = br.readLine();
                if (line != null && !line.trim().isEmpty()) {
                    newCid = Long.parseLong(line.trim());
                }
            } catch (Throwable ignored) {}
        }

        if (newCid < 0L) {
            long roomId = -newCid;
            boolean roomChanged;
            synchronized (liveLock) {
                roomChanged = (liveRoomId != roomId);
                if (roomChanged) {
                    liveRoomId = roomId;
                    stopNativeLiveFeed();
                    lastNativeLiveCheckMs = 0L;
                    lastLiveTsvGrowthMs = SystemClock.uptimeMillis();
                }
            }
            if (!isLiveStreamMode || roomChanged || currentCid != newCid) {
                switchToLiveMode(newCid);
            }
            return;
        }

        if (isLiveStreamMode) {
            // A positive/zero cid means the user left the live room. The old
            // early return here made live mode sticky and blocked every later
            // VOD danmaku load for the rest of the VR session.
            stopNativeLiveFeed();
            exitLiveMode();
            currentCid = 0L;
            liveQueue.clear();
            lastTsvMtime = 0L;
            lastTsvLen = 0L;
            lastSubFileMtime = 0L;
            lastSubFileLen = 0L;
            xmlFallbackAttempted = false;
            synchronized (allDanmaku) {
                segmentBuckets.clear();
                allDanmaku.clear();
            }
            clearSegmentState();
        }

        if (newCid != 0L && newCid != currentCid) {
            Log.i(TAG, "Detected new video cid=" + newCid + " (prev=" + currentCid + ")");
            currentCid = newCid;
            synchronized (renderLock) {
                synchronized (allDanmaku) {
                    allDanmaku.clear();
                    segmentBuckets.clear();
                    clearActiveTracks();
                    nextDanmakuIdx = 0;
                }
                synchronized (subtitleCues) {
                    subtitleCues.clear();
                }
                resetLastDrawnState();
            }
            clearSegmentState();
            lastTsvMtime = 0L;
            lastTsvLen = 0L;
            lastSubFileMtime = 0L;
            lastSubFileLen = 0L;
            xmlFallbackAttempted = false;
        }

        File tsvFile = new File(filesDir, "vr_danmaku.tsv");
        if (tsvFile.exists()) {
            long mtime = tsvFile.lastModified();
            long len = tsvFile.length();
            if (mtime != lastTsvMtime || len != lastTsvLen) {
                lastTsvMtime = mtime;
                lastTsvLen = len;
                loadDanmakuTsv(tsvFile);
            }
        }

        boolean needXml;
        synchronized (allDanmaku) {
            needXml = !isLiveStreamMode && allDanmaku.isEmpty() && currentCid > 0L && !xmlFallbackAttempted && !tsvFile.exists();
        }
        if (needXml) {
            xmlFallbackAttempted = true;
            fetchXmlDanmakuFallback(currentCid);
        }

        File subFile = new File(filesDir, "vr_cc_subtitle.vtt");
        if (subFile.exists()) {
            long mtime = subFile.lastModified();
            long len = subFile.length();
            if (mtime != lastSubFileMtime || len != lastSubFileLen) {
                lastSubFileMtime = mtime;
                lastSubFileLen = len;
                loadWebVttFile(subFile);
            }
        } else {
            synchronized (subtitleCues) {
                if (!subtitleCues.isEmpty()) {
                    subtitleCues.clear();
                }
            }
        }
    }

    private void loadDanmakuTsv(File tsvFile) {
        List<DanmakuItem> parsed = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(tsvFile), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.isEmpty() || line.startsWith("#")) {
                    if (line.startsWith("#cid=-")) {
                        long parsedCid = -1L;
                        try { parsedCid = Long.parseLong(line.substring(5).trim()); } catch (Throwable ignored) {}
                        switchToLiveMode(parsedCid);
                        return;
                    }
                    if (line.startsWith("#live=")) {
                        switchToLiveMode(-1L);
                        return;
                    }
                    if (line.startsWith("#cid=")) {
                        try {
                            long fileCid = Long.parseLong(line.substring(5).trim());
                            if (currentCid > 0L && fileCid != currentCid) {
                                return;
                            }
                            if (currentCid == 0L) currentCid = fileCid;
                        } catch (Throwable ignored) {}
                    }
                    continue;
                }
                String[] parts = line.split("\t", 6);
                if (parts.length < 6) continue;
                try {
                    float timeSec = Integer.parseInt(parts[0]) / 1000.0f;
                    int mode = Integer.parseInt(parts[1]);
                    int color = Integer.parseInt(parts[2]);
                    float fontSize = Float.parseFloat(parts[3]);
                    String text = parts[5].trim();
                    if (!text.isEmpty()) {
                        parsed.add(new DanmakuItem(timeSec, mode, color, fontSize, text));
                    }
                } catch (Throwable ignored) {}
            }
        } catch (Throwable t) {
            Log.w(TAG, "loadDanmakuTsv error: " + t.getMessage());
        }

        if (!parsed.isEmpty()) {
            java.util.Map<Integer, List<DanmakuItem>> bySeg = new java.util.HashMap<>();
            for (int i = 0; i < parsed.size(); i++) {
                DanmakuItem it = parsed.get(i);
                int seg = Math.max(1, (int) (it.timeSec / SEG_DURATION_SEC) + 1);
                List<DanmakuItem> bucket = bySeg.get(seg);
                if (bucket == null) {
                    bucket = new ArrayList<>();
                    bySeg.put(seg, bucket);
                }
                bucket.add(it);
            }
            java.util.Set<Integer> netSnapshot;
            synchronized (segLock) {
                netSnapshot = new java.util.HashSet<>(netFetchedSegments);
            }
            synchronized (allDanmaku) {
                for (java.util.Map.Entry<Integer, List<DanmakuItem>> entry : bySeg.entrySet()) {
                    Integer segKey = entry.getKey();
                    if (!netSnapshot.contains(segKey)) {
                        segmentBuckets.put(segKey, entry.getValue());
                    }
                }
                rebuildAllDanmakuLocked();
            }
            synchronized (segLock) {
                loadedSegments.addAll(bySeg.keySet());
            }
            segmentDataUpdated = true;
            Log.i(TAG, "Loaded " + parsed.size() + " danmaku items from TSV across segments " + bySeg.keySet() + " for cid=" + currentCid);
        }
    }

    private void rebuildAllDanmakuLocked() {
        allDanmaku.clear();
        for (List<DanmakuItem> bucket : segmentBuckets.values()) {
            if (bucket != null && !bucket.isEmpty()) {
                allDanmaku.addAll(bucket);
            }
        }
        Collections.sort(allDanmaku, new Comparator<DanmakuItem>() {
            @Override
            public int compare(DanmakuItem a, DanmakuItem b) {
                return Float.compare(a.timeSec, b.timeSec);
            }
        });
        double cur = renderVideoSec;
        if (cur >= 0.0) {
            seekDanmakuIndex((float) cur);
        } else {
            nextDanmakuIdx = 0;
        }
    }

    private void clearSegmentState() {
        synchronized (segLock) {
            loadedSegments.clear();
            netFetchedSegments.clear();
            inFlightSegments.clear();
            failedSegmentRetryMs.clear();
            fastSegHitA = -1;
            fastSegHitB = -1;
        }
    }

    private void ensureSegmentsForTime(float curSec) {
        long cid = currentCid;
        if (isLiveStreamMode || cid <= 0L || curSec < 0f) return;
        int seg = Math.max(1, (int) (curSec / SEG_DURATION_SEC) + 1);
        if (seg != fastSegHitA && seg != fastSegHitB) {
            requestSegmentIfMissing(cid, seg);
        }
        float inSeg = curSec - (seg - 1) * SEG_DURATION_SEC;
        if (inSeg < 10.0f && seg > 1) {
            int prevSeg = seg - 1;
            if (prevSeg != fastSegHitA && prevSeg != fastSegHitB) {
                requestSegmentIfMissing(cid, prevSeg);
            }
        } else if (inSeg > 320.0f) {
            int nextSeg = seg + 1;
            if (nextSeg != fastSegHitA && nextSeg != fastSegHitB) {
                requestSegmentIfMissing(cid, nextSeg);
            }
        }
    }

    private void requestSegmentIfMissing(final long cid, final int segIdx) {
        if (!running || cid <= 0L || segIdx < 1 || isLiveStreamMode) return;
        java.util.concurrent.ExecutorService ex = this.dmFetchExecutor;
        if (ex == null || ex.isShutdown()) return;
        synchronized (segLock) {
            if (loadedSegments.contains(segIdx) || netFetchedSegments.contains(segIdx)) {
                fastSegHitB = fastSegHitA;
                fastSegHitA = segIdx;
                return;
            }
            if (inFlightSegments.contains(segIdx)) {
                return;
            }
            Long retryAt = failedSegmentRetryMs.get(segIdx);
            if (retryAt != null && SystemClock.uptimeMillis() < retryAt) {
                return;
            }
            inFlightSegments.add(segIdx);
        }
        try {
            ex.execute(new Runnable() {
                @Override
                public void run() {
                    fetchAndMergeSegment(cid, segIdx);
                }
            });
        } catch (Throwable t) {
            synchronized (segLock) {
                inFlightSegments.remove(segIdx);
            }
        }
    }

    private void fetchAndMergeSegment(long cid, int segIdx) {
        if (!running || isLiveStreamMode || cid != currentCid) {
            synchronized (segLock) {
                inFlightSegments.remove(segIdx);
            }
            return;
        }
        HttpURLConnection conn = null;
        boolean succeeded = false;
        boolean unexpectedFailure = false;
        List<DanmakuItem> items = null;
        try {
            URL url = new URL("https://api.bilibili.com/x/v2/dm/web/seg.so?type=1&oid=" + cid + "&segment_index=" + segIdx);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(5000);
            conn.setReadTimeout(6000);
            conn.setRequestProperty("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
            conn.setRequestProperty("Referer", "https://www.bilibili.com/");
            int code = conn.getResponseCode();
            if (code == 304) {
                items = new ArrayList<>();
                succeeded = true;
            } else if (code == 200) {
                InputStream in = conn.getInputStream();
                ByteArrayOutputStream bos = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) != -1) {
                    bos.write(buf, 0, n);
                }
                items = parseDanmakuSegProtobuf(bos.toByteArray());
                succeeded = true;
            } else {
                unexpectedFailure = true;
                Log.w(TAG, "seg.so HTTP " + code + " for cid=" + cid + " seg=" + segIdx);
            }
        } catch (Throwable t) {
            unexpectedFailure = true;
            Log.w(TAG, "fetchAndMergeSegment error cid=" + cid + " seg=" + segIdx + ": " + t.getMessage());
        } finally {
            if (conn != null) {
                try { conn.disconnect(); } catch (Throwable ignored) {}
            }
        }

        if (succeeded && items != null) {
            if (!isLiveStreamMode && cid == currentCid) {
                synchronized (allDanmaku) {
                    if (!isLiveStreamMode && cid == currentCid) {
                        segmentBuckets.put(segIdx, items);
                        rebuildAllDanmakuLocked();
                    }
                }
                synchronized (segLock) {
                    inFlightSegments.remove(segIdx);
                    loadedSegments.add(segIdx);
                    netFetchedSegments.add(segIdx);
                    failedSegmentRetryMs.remove(segIdx);
                    fastSegHitB = fastSegHitA;
                    fastSegHitA = segIdx;
                }
                segmentDataUpdated = true;
                Log.i(TAG, "Fetched seg=" + segIdx + " (" + items.size() + " items) for cid=" + cid);
                return;
            }
        }

        synchronized (segLock) {
            inFlightSegments.remove(segIdx);
            if (!succeeded) {
                failedSegmentRetryMs.put(segIdx, SystemClock.uptimeMillis() + 15000L);
            }
        }
        if (unexpectedFailure && !isLiveStreamMode && cid == currentCid && !xmlFallbackAttempted) {
            boolean empty;
            synchronized (allDanmaku) {
                empty = allDanmaku.isEmpty();
            }
            if (empty) {
                xmlFallbackAttempted = true;
                fetchXmlDanmakuFallback(cid);
            }
        }
    }

    private static List<DanmakuItem> parseDanmakuSegProtobuf(byte[] data) {
        List<DanmakuItem> out = new ArrayList<>();
        if (data == null || data.length == 0) return out;
        int pos = 0;
        final int total = data.length;
        while (pos < total) {
            long tag = 0L;
            int shift = 0;
            boolean ok = false;
            while (pos < total && shift < 64) {
                int b = data[pos++] & 0xFF;
                tag |= (long) (b & 0x7F) << shift;
                if ((b & 0x80) == 0) {
                    ok = true;
                    break;
                }
                shift += 7;
            }
            if (!ok) break;
            int fieldNum = (int) (tag >>> 3);
            int wireType = (int) (tag & 7);
            if (wireType == 0) {
                while (pos < total && (data[pos] & 0x80) != 0) pos++;
                if (pos < total) pos++;
                else break;
            } else if (wireType == 1) {
                pos += 8;
            } else if (wireType == 5) {
                pos += 4;
            } else if (wireType == 2) {
                int len = 0;
                int lShift = 0;
                boolean lOk = false;
                while (pos < total && lShift < 32) {
                    int b = data[pos++] & 0xFF;
                    len |= (b & 0x7F) << lShift;
                    if ((b & 0x80) == 0) {
                        lOk = true;
                        break;
                    }
                    lShift += 7;
                }
                if (!lOk || len < 0 || pos + len > total) break;
                if (fieldNum == 1 && len > 0) {
                    DanmakuItem item = parseDanmakuElemProtobuf(data, pos, pos + len);
                    if (item != null) {
                        out.add(item);
                    }
                }
                pos += len;
            } else {
                break;
            }
        }
        return out;
    }

    private static DanmakuItem parseDanmakuElemProtobuf(byte[] buf, int start, int end) {
        int p = start;
        int progressMs = -1;
        int mode = 0;
        float fontSize = 25f;
        int color = 0xFFFFFF;
        String text = null;
        while (p < end) {
            long tag = 0L;
            int shift = 0;
            boolean ok = false;
            while (p < end && shift < 64) {
                int b = buf[p++] & 0xFF;
                tag |= (long) (b & 0x7F) << shift;
                if ((b & 0x80) == 0) {
                    ok = true;
                    break;
                }
                shift += 7;
            }
            if (!ok) break;
            int fieldNum = (int) (tag >>> 3);
            int wireType = (int) (tag & 7);
            if (wireType == 0) {
                long val = 0L;
                int vShift = 0;
                boolean vOk = false;
                while (p < end && vShift < 64) {
                    int b = buf[p++] & 0xFF;
                    val |= (long) (b & 0x7F) << vShift;
                    if ((b & 0x80) == 0) {
                        vOk = true;
                        break;
                    }
                    vShift += 7;
                }
                if (!vOk) break;
                if (fieldNum == 2) progressMs = (int) val;
                else if (fieldNum == 3) mode = (int) val;
                else if (fieldNum == 4) fontSize = (float) val;
                else if (fieldNum == 5) color = (int) (val & 0x00FFFFFFL);
            } else if (wireType == 1) {
                p += 8;
            } else if (wireType == 5) {
                p += 4;
            } else if (wireType == 2) {
                int len = 0;
                int lShift = 0;
                boolean lOk = false;
                while (p < end && lShift < 32) {
                    int b = buf[p++] & 0xFF;
                    len |= (b & 0x7F) << lShift;
                    if ((b & 0x80) == 0) {
                        lOk = true;
                        break;
                    }
                    lShift += 7;
                }
                if (!lOk || len < 0 || p + len > end) break;
                if (fieldNum == 7 && len > 0) {
                    text = new String(buf, p, len, StandardCharsets.UTF_8).trim();
                }
                p += len;
            } else {
                break;
            }
        }
        if (progressMs < 0 || text == null || text.isEmpty()) return null;
        if (mode < 1 || mode > 5) return null;
        return new DanmakuItem(progressMs / 1000.0f, mode, color, fontSize, text);
    }

    private void fetchXmlDanmakuFallback(final long cid) {
        HttpURLConnection conn = null;
        try {
            URL url = new URL("https://comment.bilibili.com/" + cid + ".xml");
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(5000);
            conn.setReadTimeout(5000);
            conn.setRequestProperty("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
            InputStream rawIn = conn.getInputStream();
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = rawIn.read(buf)) != -1) {
                bos.write(buf, 0, n);
            }
            byte[] rawBytes = bos.toByteArray();
            String xmlContent = decodeDeflateOrUtf8(rawBytes);
            if (xmlContent == null || xmlContent.isEmpty()) return;

            List<DanmakuItem> parsed = new ArrayList<>();
            XmlPullParserFactory factory = XmlPullParserFactory.newInstance();
            XmlPullParser parser = factory.newPullParser();
            parser.setInput(new StringReader(xmlContent));
            int eventType = parser.getEventType();
            while (eventType != XmlPullParser.END_DOCUMENT) {
                if (eventType == XmlPullParser.START_TAG && "d".equals(parser.getName())) {
                    String pAttr = parser.getAttributeValue(null, "p");
                    String text = parser.nextText();
                    if (pAttr != null && text != null && !text.trim().isEmpty()) {
                        String[] pParts = pAttr.split(",");
                        if (pParts.length >= 4) {
                            try {
                                float timeSec = Float.parseFloat(pParts[0]);
                                int mode = Integer.parseInt(pParts[1]);
                                float fontSize = Float.parseFloat(pParts[2]);
                                int color = (int) Long.parseLong(pParts[3]);
                                parsed.add(new DanmakuItem(timeSec, mode, color, fontSize, text.trim()));
                            } catch (Throwable ignored) {}
                        }
                    }
                }
                eventType = parser.next();
            }

            if (!parsed.isEmpty()) {
                Collections.sort(parsed, new Comparator<DanmakuItem>() {
                    @Override
                    public int compare(DanmakuItem a, DanmakuItem b) {
                        return Float.compare(a.timeSec, b.timeSec);
                    }
                });
                int loadedCount = 0;
                synchronized (allDanmaku) {
                    if (!isLiveStreamMode && allDanmaku.isEmpty() && cid == currentCid) {
                        for (int i = 0; i < parsed.size(); i++) {
                            DanmakuItem it = parsed.get(i);
                            if (it.mode < 1 || it.mode > 5) continue;
                            int seg = Math.max(1, (int) (it.timeSec / SEG_DURATION_SEC) + 1);
                            List<DanmakuItem> bucket = segmentBuckets.get(seg);
                            if (bucket == null) {
                                bucket = new ArrayList<>();
                                segmentBuckets.put(seg, bucket);
                            }
                            bucket.add(it);
                        }
                        rebuildAllDanmakuLocked();
                        loadedCount = allDanmaku.size();
                    }
                }
                if (loadedCount > 0) {
                    segmentDataUpdated = true;
                    Log.i(TAG, "Loaded " + loadedCount + " danmaku items from XML fallback for cid=" + cid);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "fetchXmlDanmakuFallback failed: " + t.getMessage());
        } finally {
            if (conn != null) {
                try { conn.disconnect(); } catch (Throwable ignored) {}
            }
        }
    }

    private static String decodeDeflateOrUtf8(byte[] data) {
        try {
            Inflater inflater = new Inflater(true);
            InflaterInputStream iis = new InflaterInputStream(new java.io.ByteArrayInputStream(data), inflater);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = iis.read(buf)) != -1) {
                out.write(buf, 0, n);
            }
            iis.close();
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (Throwable ignored) {}
        try {
            return new String(data, StandardCharsets.UTF_8);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private void loadWebVttFile(File vttFile) {
        List<SubtitleCue> cues = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(vttFile), StandardCharsets.UTF_8))) {
            String firstLine = br.readLine();
            if (firstLine != null && firstLine.startsWith("#cid=")) {
                try {
                    long fileCid = Long.parseLong(firstLine.substring(5).trim());
                    if (currentCid > 0L && fileCid != currentCid) {
                        return;
                    }
                } catch (Throwable ignored) {}
            }
            String line;
            float cueFrom = -1f;
            float cueTo = -1f;
            StringBuilder textBuf = new StringBuilder();
            while ((line = br.readLine()) != null) {
                String trimmed = line.trim();
                if (trimmed.contains("-->")) {
                    if (cueFrom >= 0f && textBuf.length() > 0) {
                        cues.add(new SubtitleCue(cueFrom, cueTo, textBuf.toString().trim()));
                    }
                    textBuf.setLength(0);
                    String[] times = trimmed.split("-->");
                    if (times.length >= 2) {
                        cueFrom = parseVttTimestamp(times[0].trim());
                        cueTo = parseVttTimestamp(times[1].trim().split(" ")[0]);
                    } else {
                        cueFrom = -1f;
                    }
                } else if (trimmed.isEmpty()) {
                    if (cueFrom >= 0f && textBuf.length() > 0) {
                        cues.add(new SubtitleCue(cueFrom, cueTo, textBuf.toString().trim()));
                        cueFrom = -1f;
                        textBuf.setLength(0);
                    }
                } else if (cueFrom >= 0f) {
                    if (textBuf.length() > 0) textBuf.append('\n');
                    textBuf.append(trimmed);
                }
            }
            if (cueFrom >= 0f && textBuf.length() > 0) {
                cues.add(new SubtitleCue(cueFrom, cueTo, textBuf.toString().trim()));
            }
        } catch (Throwable t) {
            Log.w(TAG, "loadWebVttFile error: " + t.getMessage());
        }
        int cueCount;
        synchronized (subtitleCues) {
            subtitleCues.clear();
            subtitleCues.addAll(cues);
            cueCount = subtitleCues.size();
        }
        Log.i(TAG, "Loaded " + cueCount + " CC subtitle cues from WebVTT");
    }

    private static float parseVttTimestamp(String ts) {
        try {
            String[] parts = ts.replace(',', '.').split(":");
            if (parts.length == 3) {
                return Integer.parseInt(parts[0]) * 3600f
                        + Integer.parseInt(parts[1]) * 60f
                        + Float.parseFloat(parts[2]);
            } else if (parts.length == 2) {
                return Integer.parseInt(parts[0]) * 60f
                        + Float.parseFloat(parts[1]);
            }
        } catch (Throwable ignored) {}
        return -1f;
    }
}
