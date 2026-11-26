package com.example.piliplus.vr;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.SurfaceTexture;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Surface;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;

import com.picacg.picacomic.TouchInput;
import io.github.gblandro.p5m.XrBridge;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;

public final class VrPlayerActivity extends Activity implements Runnable {
    private static final String TAG = "PiliVR";

    /** Unlimited long edge cap for the VR render target. */
    private static final int MAX_RENDER_LONG_EDGE = 4096;
    /** Minimum dimension floor. */
    private static final int MIN_RENDER_LONG_EDGE = 2;

    private int renderWidth = 1920;
    private int renderHeight = 1080;
    /** Long edge the compositor actually needs for the current screen geometry, 0 = unknown. */
    private int renderCapLongEdge = 0;
    private long lastInteractionAt = 0;
    private float panelRate = 0f;
    private int lastRequestedPanelHz = 0;

    // The runtime's recommended layer resolution is logged, never used to size a swapchain: it
    // swings several-fold with the screen size the user asks for (663x1179 small vs 3794x2134
    // large on the same device), so remembering one value and reusing it makes pictures soft.

    private int videoW = 0;
    private int videoH = 0;

    private XrBridge xr;
    private ZipDepthSource zipDepthSource;
    private SurfaceTexture vrTexture;
    private Surface vrSurface;
    private long vrWid = 0;
    private long currentMpvHandle = 0;
    private Handler handler;
    private volatile boolean isRunning = false;
    private volatile boolean initializing = false;
    private int initGen = 0;
    private View blackView;
    private PowerManager.WakeLock wakeLock;
    private boolean touchActive = false;
    /** Default VR background on entry (true = passthrough, false = pitch black), configurable in HUD settings. */
    private boolean defaultPassthrough = true;
    private boolean passthrough = true;
    private boolean isPaused = false;
    private boolean userExplicitlyPaused = false;
    private float screenYawOffset = 0.0f;
    private float lastPushedYaw = 0.0f;
    private boolean isGripDown = false;
    private boolean isScreenDragging = false;
    private float anchorRayYaw = 0.0f;
    private float anchorRayPitch = 0.0f;
    private float anchorScreenYaw = 0.0f;
    private float anchorScreenPitch = 0.0f;
    private boolean resBadgeShown = false;
    private boolean exited = false;
    private int stereoMode = 0; // 0=2D, 1=SBS, 2=OU, 3=Synthetic 3D
    private int synth3dLevel = 1; // 1=Weak, 2=Medium, 3=Strong
    private boolean isHudVisible = true;
    // Intro curtain: freeze the first video frame behind two parting panels
    // while the AI depth model loads. Matches kCurtainDurationMs in native.
    // Cold start needs the full reveal to cover the ~3 s GPU delegate build;
    // once the model is cached, a short reveal is enough.
    private static final long CURTAIN_MS_COLD = 3600L;
    private static final long CURTAIN_MS_CACHED = 1000L;
    private long curtainDurationMs = CURTAIN_MS_COLD;
    private long curtainUntilMs = 0L;
    private boolean curtainStarted = false;
    private boolean curtainDanmakuHidden = false;
    private int xrSessionGen = 0;
    private Runnable curtainRevealRunnable;
    private Runnable zipDepthDelayRunnable;
    private boolean danmakuEnabled = true;
    private boolean subtitleEnabled = true;
    // Depth-map capture cap chosen in the 3D settings card (20 / 30 maps/s).
    // 30 is the default on entering VR: the higher map rate is what keeps the
    // parallax in step with a panning subject. The user can still pick 20.
    private int depthRateCap = ZipDepthSource.EXPERIMENTAL_MAPS_PER_SECOND;
    // P2 edge enhancement preset: 0=Off(soft), 1=Balanced(Cubic without step snapping), 2=Strong(Cubic+gentle edge snap).
    private int aiEdgeEnhanceIdx = 1;
    private static final float[] AI_EDGE_SHARP_VALUES = {0.0f, 0.0f, 0.25f};
    private static final boolean[] AI_EDGE_CUBIC_VALUES = {false, true, true};
    private static final String[] AI_EDGE_NAMES = {"\u5173\u95ed (\u67d4\u548c)", "\u5747\u8861 (\u63a8\u8350)", "\u5f3a\u9510\u5316"};
    // Button Y: short press cycles 3D strength, long press toggles the depth
    // debug view. The OpenXR button bitmask has no long-press event, so the
    // hold duration is measured here inside the poll loop.
    private static final long Y_LONG_PRESS_MS = 600L;
    private long yPressAtMs = 0L;
    private boolean yLongFired = false;
    private boolean isMuted = false;
    private boolean settingsOpen = false;
    private int screenCurveIdx = 1;     // 0=Flat(0%), 1=Light(40%), 2=Standard(70%), 3=Wrap(100%)
    private int danmakuDistIdx = 1;     // 0=贴屏(0.99), 1=微浮(0.88), 2=出屏(0.76), 3=超近眼前(0.64)
    private int danmakuSpanIdx = 0;     // 0=仅视频, 1=广角130, 2=180全景包围, 3=3D环绕跑道
    private boolean danmakuVOut = false; // false=仅视频高度, true=天幕地面延展
    private int lastVOutOrientSign = 0; // -1=竖屏, 1=横屏
    private int settingsTab = 0;        // 0=画面与分辨率, 1=弹幕与字幕, 2=3D立体效果
    private int projectionMode = 0;     // 0=Cinema, 1=VR180, 2=VR180_SBS, 3=VR180_OU, 4=VR360, 5=VR360_SBS, 6=VR360_OU
    private int stereoArrangement = 0;  // 0=SBS (左右), 1=OU (上下)
    private boolean sbsUncompressed = false; // true = Full-SBS (uncompressed), false = Half-SBS (compressed)
    private final float[] handState = new float[28];
    private final boolean[] isHandPinchDown = new boolean[2];
    private boolean prevNativeManipulating = false;
    private boolean lastJavaManipulating = false;
    private float prevNativeFov = DEFAULT_FOV;
    private final Path osdPillPath = new Path();
    private String lastRenderedOsdText = "";
    private boolean isCtrlCornerResizing = false;
    private float ctrlResizeAnchorYaw = 0.0f;
    private float ctrlResizeAnchorPitch = 0.0f;
    private float ctrlResizeAnchorFov = DEFAULT_FOV;
    private long lastAttachAttemptTime = 0L;
    private float screenPitchOffset = 0.0f; // in radians, [-0.65f, 1.35f]
    private int screenPitchPresetIdx = 0;   // 0=0, 1=15, 2=30, 3=45, 4=65 deg
    private static final float[] PITCH_PRESETS_RAD = {0.0f, 0.2618f, 0.5236f, 0.7854f, 1.1345f};
    private int danmakuDensityIdx = 2;  // 0=25%, 1=50%, 2=75%, 3=100%
    private boolean fixedDanmakuEnabled = true; // true=show top/bottom fixed, false=scroll only
    private int danmakuSizeIdx = 1;     // 0=75%, 1=100%, 2=125%, 3=150%
    private int subtitleSizeIdx = 1;    // 0=75%, 1=100%, 2=125%, 3=150%
    private int subProtIdx = 2;         // 0=Off, 1=10%, 2=16%, 3=22%, 4=28%
    private static final float[] CURVE_VALUES = {0.0f, 0.40f, 0.70f, 1.00f};
    private static final float[] DANMAKU_DEPTH_FACTORS = {0.99f, 0.88f, 0.76f, 0.64f};
    private static final float[] SIZE_SCALES = {0.75f, 1.0f, 1.25f, 1.50f};
    private static final int[] SUB_PROT_PCTS = {0, 10, 16, 22, 28};
    private static final float[] SYNTH_3D_DISP = {0.007f, 0.010f, 0.018f};
    private static final String[] SYNTH_3D_NAMES = {"弱", "中", "强"};

    private VrDanmakuEngine danmakuEngine;
    private VrHudRenderer hudRenderer;
    private final List<VrHudRenderer.QualityOption> qualityOptions = new ArrayList<>();
    private int currentQualityCode = 0;
    private String videoTitle = "";
    private volatile long liveTitleFetchRoom = 0L;
    private volatile long lastLiveTitleFetchMs = 0L;
    private double positionSec = 0.0;
    private double durationSec = 0.0;
    private long lastHudPollMs = 0L;
    private long lastQualitiesMtime = 0L;
    private long lastQualitiesLen = 0L;

    // OSD HUD overlay
    private String currentOsdText = null;
    private long osdExpireTime = 0L;
    private boolean isOsdVisible = false;
    private Bitmap currentOsdBitmap = null;
    private long resBadgeExpireTime = 0L;
    private boolean isResBadgeVisible = false;
    private Bitmap currentResBitmap = null;
    private final Paint osdBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint osdBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint osdTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Bitmap[] hudCache = new Bitmap[64];
    private long resumeGuardUntil = 0;
    private long lastResumeNudgeAt = 0;
    private double pendingQualityResumePos = -1.0;
    private long pendingQualityResumeAtMs = 0L;
    private long pendingQualityResumeUntilMs = 0L;
    private long lastQualityResumeCheckMs = 0L;
    private long lastQualityFallbackSeekMs = 0L;
    private int qualityFallbackSeekAttempts = 0;
    private boolean pendingQualitySawReload = false;
    private String pendingQualityPrevPath = null;

    // Laser pointer & Touch state
    private final float[] touchState = new float[16];
    private float pointerX = 960f;
    private float pointerY = 540f;
    private boolean pointerValid = false;
    private int pointerHitType = 0;
    private boolean isTriggerDown = false;
    private int previousButtons = 0;
    private boolean stickWasEngaged = false;
    private long lastStickTime = 0;
    private long stickEngageStartTime = 0;
    private long lastStickRepeatTime = 0;
    private int stickRepeatCount = 0;
    private long lastHudActionTime = 0;
    private long hudHideTimeoutMs = 5000L;

    // Screen size / distance settings (persisted)
    private static final String PREFS = "pili_vr";
    private static final int PREFS_VERSION = 4;
    private static final float DEFAULT_FOV = 0.7f;      // radians, ~40 deg
    private static final float DEFAULT_DIST = 2.5f;     // metres
    private float screenFov = DEFAULT_FOV;
    private float screenDist = DEFAULT_DIST;
    private boolean sizeStickEngaged = false;
    private boolean distStickEngaged = false;
    private long lastTouchAtMs = 0;
    private long lastScreenApplyAt = 0;
    private float lastPushedFov = 0f;
    private float lastPushedDist = 0f;
    private float lastPushedPitch = 0f;
    private int lastPushedCurveIdx = -1;
    private int lastPushedDistIdx = -1;
    private int lastPushedSpanIdx = -1;
    private boolean lastPushedVOut = false;
    private long dbgSspN, dbgSspNs, dbgSspMaxNs;
    private long dbgOsdN, dbgOsdNs, dbgOsdMaxNs;
    private long dbgPollN, dbgPollNs, dbgPollMaxNs;
    private long lastSizeOsdAt = 0;
    private long lastDistOsdAt = 0;
    private long lastColorCheckAt = 0;

    // Frame watchdog: media_kit's Dart videoParams listener re-applies mpv's wid to the
    // Flutter SurfaceTexture whenever video parameters change (resolution/rotate), which
    // silently steals the video output away from our VR SurfaceTexture and freezes the
    // picture. We detect "no new frame on the VR texture" and take the wid back.
    private final AtomicInteger frameCount = new AtomicInteger(0);
    private volatile long lastFrameAt = 0;
    private volatile long lastWidAssertAt = 0;
    private volatile int widAssertCount = 0;
    private long lastStatsAt = 0;
    private int lastStatsFrames = 0;

    // Adaptive resolution / aspect
    private long lastSizeProbeAt = 0;
    private long lastRebuildAt = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        VrState.vrActive = true;
        VrState.vrStarting = false;
        VrState.vrActivity = this;

        super.onCreate(savedInstanceState);
        lastHudActionTime = SystemClock.uptimeMillis();
        hudHideTimeoutMs = 5000L;
        try { overridePendingTransition(0, 0); } catch (Throwable ignored) {}

        // Transparent placeholder window with permanent KEEP_SCREEN_ON
        blackView = new View(this);
        blackView.setBackgroundColor(Color.TRANSPARENT);
        blackView.setKeepScreenOn(true);
        setContentView(blackView);

        // Keep screen on and force full window brightness via WakeLock + Window flags
        try {
            PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
            if (pm != null) {
                wakeLock = pm.newWakeLock(PowerManager.SCREEN_BRIGHT_WAKE_LOCK | PowerManager.ON_AFTER_RELEASE, "piliplus:vr_bright");
                wakeLock.acquire();
            }
        } catch (Throwable t) {
            Log.w(TAG, "wakeLock acquire failed", t);
        }
        applyWindowBrightness();

        handler = new Handler(Looper.getMainLooper());

        // Restore screen size / distance chosen during a previous session.
        try {
            SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
            if (sp.getInt("prefs_version", 0) < PREFS_VERSION) {
                screenFov = DEFAULT_FOV;
                screenDist = DEFAULT_DIST;
                sp.edit()
                        .putInt("prefs_version", PREFS_VERSION)
                        .putFloat("screen_fov", screenFov)
                        .putFloat("screen_dist", screenDist)
                        .apply();
            } else {
                screenFov = clamp(sp.getFloat("screen_fov", DEFAULT_FOV), MIN_FOV, MAX_FOV);
                screenDist = clamp(sp.getFloat("screen_dist", DEFAULT_DIST), MIN_DIST, MAX_DIST);
            }
            danmakuEnabled = sp.getBoolean("danmaku_enabled", true);
            subtitleEnabled = sp.getBoolean("subtitle_enabled", true);
            defaultPassthrough = sp.getBoolean("default_passthrough", true);
            passthrough = defaultPassthrough;
            VrState.aiDepthEnabled = sp.getBoolean("ai_depth_enabled", false);
            VrState.showDepthMap = sp.getBoolean("show_depth_map", false);
            VrState.vrAutoEnter = sp.getBoolean("auto_enter_vr", true);
            screenCurveIdx = clampi(sp.getInt("screen_curve_idx", 1), 0, 3);
            danmakuDistIdx = clampi(sp.getInt("danmaku_dist_idx", 1), 0, 3);
            danmakuSpanIdx = clampi(sp.getInt("danmaku_span_idx", 0), 0, 3);
            danmakuVOut = sp.getBoolean("danmaku_vout", false);
            danmakuDensityIdx = clampi(sp.getInt("danmaku_density_idx", 2), 0, 3);
            fixedDanmakuEnabled = sp.getBoolean("fixed_danmaku_enabled", true);
            danmakuSizeIdx = clampi(sp.getInt("danmaku_size_idx", 1), 0, 3);
            subtitleSizeIdx = clampi(sp.getInt("subtitle_size_idx", 1), 0, 3);
            subProtIdx = clampi(sp.getInt("sub_prot_idx", 2), 0, 4);
            aiEdgeEnhanceIdx = clampi(sp.getInt("ai_edge_enhance_idx", 1), 0, 2);
            sbsUncompressed = sp.getBoolean("sbs_uncompressed", false);
            stereoMode = clampi(sp.getInt("stereo_mode", 0), 0, 3);
            synth3dLevel = clampi(sp.getInt("synth_3d_level", 1), 1, 3);
            if (is3dVideoActive() && danmakuSpanIdx == 3) {
                danmakuSpanIdx = 0;
                sp.edit().putInt("danmaku_span_idx", 0).apply();
            }
        } catch (Throwable ignored) {}
        applyAiEdgeEnhance();
        hudRenderer = new VrHudRenderer(this);

        // Resolution/aspect requested by VrState (the size mpv is decoding right now).
        try {
            Intent it = getIntent();
            if (it != null) {
                videoW = it.getIntExtra("video_w", 0);
                videoH = it.getIntExtra("video_h", 0);
            }
        } catch (Throwable ignored) {}

        osdBgPaint.setColor(0xEB151822); // Frosted obsidian glass matching HUD
        osdBgPaint.setStyle(Paint.Style.FILL);
        osdBorderPaint.setColor(0x38FFFFFF); // Subtle specular hairline border
        osdBorderPaint.setStyle(Paint.Style.STROKE);
        osdBorderPaint.setStrokeWidth(1.8f);
        osdTextPaint.setColor(Color.WHITE);
        osdTextPaint.setTextAlign(Paint.Align.CENTER);
        osdTextPaint.setFakeBoldText(true);

        Log.i(TAG, "VrPlayerActivity created, video=" + videoW + "x" + videoH
                + ", initializing OpenXR cinema layer (RenderPath 1 ToneMapper)");

        beginInit();

        isRunning = true;
        handler.removeCallbacks(this);
        handler.post(this);
    }

    private void applyWindowBrightness() {
        try {
            android.view.Window window = getWindow();
            if (window != null) {
                window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                WindowManager.LayoutParams lp = window.getAttributes();
                // Toggle slightly to ensure WindowManager sees a change and forces backlight refresh
                lp.screenBrightness = 0.999f;
                window.setAttributes(lp);
                lp.screenBrightness = 1.0f;
                window.setAttributes(lp);
            }
            if (blackView != null) {
                blackView.setKeepScreenOn(true);
                blackView.postInvalidate();
            }
        } catch (Throwable t) {
            Log.w(TAG, "applyWindowBrightness failed", t);
        }
    }

    // ---------------------------------------------------------------
    // OpenXR lifecycle
    // ---------------------------------------------------------------

    /** Waits until mpv knows the video size (usually already ready from 2D), then builds the XR session. */
    private void beginInit() {
        if (initializing || xr != null) return;
        if (videoW <= 0 || videoH <= 0) {
            int[] sz = VrState.videoSize();
            if (sz != null) {
                videoW = sz[0];
                videoH = sz[1];
            }
        }
        if (videoW > 0 && videoH > 0) {
            initXr();
            return;
        }
        initializing = true;
        resumeMpv("init poll");
        final int gen = ++initGen;
        pollVideoSize(gen, 0);
    }

    private void pollVideoSize(final int gen, final int attempt) {
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (gen != initGen || !initializing || isFinishing()) return;
                if (videoW <= 0 || videoH <= 0) {
                    int[] sz = VrState.videoSize();
                    if (sz != null) {
                        videoW = sz[0];
                        videoH = sz[1];
                    }
                }
                if (videoW > 0 && videoH > 0) {
                    initXr();
                    return;
                }
                if (attempt < 12) {
                    resumeMpv("init poll");
                    pollVideoSize(gen, attempt + 1);
                } else {
                    videoW = 1920;
                    videoH = 1080;
                    initXr();
                }
            }
        }, 60);
    }

    private void initXr() {
        initializing = false;
        if (isFinishing()) return;

        xrSessionGen++;
        if (curtainRevealRunnable != null && handler != null) {
            handler.removeCallbacks(curtainRevealRunnable);
            curtainRevealRunnable = null;
        }
        curtainStarted = false;
        curtainUntilMs = 0L;
        curtainDanmakuHidden = false;

        try {
            xr = new XrBridge(this);
            if (!xr.create()) {
                String err = xr.getLastError();
                Log.e(TAG, "Failed to create OpenXR session: " + err);
                failInit();
                return;
            }

            // ZipDepth is initialised asynchronously AFTER the video is on
            // screen (see startZipDepthAsync). Opening the 12 MB model and
            // its GPU delegate here used to block xr.start() for seconds,
            // which showed up as a black screen on every VR entry.

            // 1. Set RenderPath 1 (ToneMapper pipeline via SurfaceTexture)
            xr.setRenderPath(1, false);
            curtainDurationMs = ZipDepthSource.isModelCached(getCacheDir()) ? CURTAIN_MS_CACHED : CURTAIN_MS_COLD;
            try {
                AiDepthNative.setCurtainDuration((int) curtainDurationMs);
            } catch (Throwable t) {
                Log.w(TAG, "curtain duration JNI failed", t);
            }
            // The JNI session exists from xr.create(); restore the saved debug
            // view before the first frame is drawn.
            syncShowDepth();
            xr.setLayerShape(0); // 0 = cinema layer
            xr.setVerticalFlip(true);
            xr.setWideColor(false);

            // Geometry before the surface so the runtime has distance/FOV to work with.
            applyScreenParams();

            // 2. Size the render target from what a previous screen measured, scaled to today's
            //    FOV. Rendering at the full source size costs several times the pixel budget the
            //    optics can resolve, and that sustained load is what makes the headset throttle
            //    and dim. Never rebuild a live session to change this: it reads as a flash.
            renderCapLongEdge = learnedCapLongEdge();
            int[] dims = fitDims(videoW, videoH, renderCapLongEdge);
            renderWidth = dims[0];
            renderHeight = dims[1];

            // The quad aspect ratio is fixed here from the decoded video size, so the
            // cinema screen matches the real aspect (16:9, 4:3, 21:9, portrait, ...).
            xr.createVideoSurface(renderWidth, renderHeight);

            // And again now that the true aspect is known: portrait screens shrink their
            // angular width so the long edge keeps the size the user picked.
            applyScreenParams();

            // 3. Create detached SurfaceTexture for ToneMapper
            vrTexture = new SurfaceTexture(false);
            vrTexture.setDefaultBufferSize(renderWidth, renderHeight);
            vrTexture.setOnFrameAvailableListener(new SurfaceTexture.OnFrameAvailableListener() {
                @Override
                public void onFrameAvailable(SurfaceTexture st) {
                    frameCount.incrementAndGet();
                    lastFrameAt = SystemClock.uptimeMillis();
                    if (!curtainStarted) startCurtainFreeze();
                }
            }, handler);
            boolean attached = xr.attachSurfaceTexture(vrTexture);
            Log.i(TAG, "OpenXR attachSurfaceTexture returned: " + attached);
            applyStereoMode();

            // 4. Create consumer Surface for mpv
            vrSurface = new Surface(vrTexture);

            // brightness, contrast, saturation ... all neutral: keep the picture exactly
            // as decoded so it does not dim a few seconds after the session starts.
            xr.setQuality(1, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, false);
            // Ask for 90 Hz when passthrough + AI 3D are both active (leaving Adreno GPU
            // headroom for ZipDepth inference), and 120 Hz otherwise.
            xr.setPassthrough(passthrough);
            alignPanelRate();
            xr.setVideoLayerEnabled(true);

            touchActive = TouchInput.attach(xr.getNativeHandle());

            xr.start();
            xr.recenter();

            if (danmakuEngine != null) {
                try { danmakuEngine.stop(); } catch (Throwable ignored) {}
            }
            danmakuEngine = new VrDanmakuEngine(this);
            danmakuEngine.setFixedDanmakuEnabled(fixedDanmakuEnabled);
            danmakuEngine.start(xr, danmakuEnabled, subtitleEnabled,
                    SIZE_SCALES[danmakuSizeIdx], SIZE_SCALES[subtitleSizeIdx],
                    effectiveDanmakuDensityIdx(), danmakuSpanIdx);
            danmakuEngine.setDanmakuParams(danmakuDistIdx, danmakuSpanIdx, danmakuVOut);
            applyPortraitDanmakuVOutDefault(videoW, videoH);
            // No danmaku (or CC overlay) until the intro curtain has fully
            // parted. The reveal callback restores it, unless the AI resource
            // policy wants it suspended anyway.
            curtainDanmakuHidden = true;
            refreshDanmakuSuspension();
            Log.i(TAG, "Intro curtain: overlay hidden until reveal");
            reloadQualitiesTsv(true);
            pollHudState(SystemClock.uptimeMillis(), true);

            lastRebuildAt = SystemClock.uptimeMillis();
            // Learn the real pixel need once the runtime has published it, for the next entry.
            handler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    learnRecommended();
                }
            }, 3500);

            Log.i(TAG, "OpenXR started " + renderWidth + "x" + renderHeight
                    + " (video " + videoW + "x" + videoH
                    + ", cap=" + renderCapLongEdge + "), touchActive=" + touchActive);

            isHudVisible = true;
            hudHideTimeoutMs = 5000L;
            lastHudActionTime = SystemClock.uptimeMillis();
            updateHud();

            // Direct mpv video output to the fresh vrSurface
            attachMpvToVrSurface();

            // Show initial resolution badge in the top-left corner
            handler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    if (isRunning && xr != null) {
                        showResolutionBadge(renderWidth, renderHeight);
                    }
                }
            }, 600);

            isRunning = true;
            handler.removeCallbacks(this);
            handler.post(this);
        } catch (Throwable t) {
            Log.e(TAG, "XrBridge init error", t);
            failInit();
        }
    }

    private void failInit() {
        initializing = false;
        try {
            Toast.makeText(this, "VR 启动失败，已返回 2D", Toast.LENGTH_LONG).show();
        } catch (Throwable ignored) {}
        exitVr();
    }

    /**
     * Full XR teardown. The OpenXR session must be destroyed (not just stopped) whenever
     * this activity leaves the foreground: a stopped-but-alive session blocks the next
     * XrBridge.create() with "session not created", which showed up as a black screen
     * after backgrounding the app and entering VR again.
     */
    private void teardownXrOnly(boolean restoreFlutterFirst) {
        xrSessionGen++;
        if (curtainRevealRunnable != null && handler != null) {
            handler.removeCallbacks(curtainRevealRunnable);
            curtainRevealRunnable = null;
        }
        curtainStarted = false;
        curtainUntilMs = 0L;
        curtainDanmakuHidden = false;

        // Stop the OpenXR frame loop BEFORE tearing down AI depth or releasing
        // surfaces so no heuristic 3D fallback frame renders during exit/pause.
        if (xr != null) {
            try { xr.stop(); } catch (Throwable ignored) {}
        }

        stopZipDepth();

        if (restoreFlutterFirst) releaseMpvToFlutterSoft();

        if (danmakuEngine != null) {
            try { danmakuEngine.stop(); } catch (Throwable ignored) {}
            danmakuEngine = null;
        }

        isOsdVisible = false;
        currentOsdText = null;
        currentOsdBitmap = null;
        isResBadgeVisible = false;
        currentResBitmap = null;

        if (touchActive) {
            try { TouchInput.detach(); } catch (Throwable ignored) {}
            touchActive = false;
        }

        if (vrWid != 0) {
            try { MpvBridge.deleteGlobalRef(vrWid); } catch (Throwable ignored) {}
            vrWid = 0;
        }

        if (vrSurface != null) {
            try { vrSurface.release(); } catch (Throwable ignored) {}
            vrSurface = null;
        }

        if (vrTexture != null) {
            try { vrTexture.release(); } catch (Throwable ignored) {}
            vrTexture = null;
        }

        if (xr != null) {
            try { xr.destroy(); } catch (Throwable ignored) {}
            xr = null;
        }

        currentMpvHandle = 0;
        lastFrameAt = 0;
        lastWidAssertAt = 0;
        lastRequestedPanelHz = 0;
        panelRate = 0f;
        lastJavaManipulating = false;
    }

    /** Rebuilds the session with a new quad aspect (e.g. next episode is 4:3). */
    private void rebuildForVideoSize(int w, int h) {
        Log.i(TAG, "Video aspect changed, rebuilding OpenXR session with " + w + "x" + h);
        boolean wasRunning = isRunning;
        isRunning = false;
        if (xr != null) {
            try { xr.stop(); } catch (Throwable ignored) {}
        }
        releaseMpvToFlutterSoft();
        teardownXrOnly(false);
        videoW = w;
        videoH = h;
        initXr();
        if (wasRunning && xr != null) {
            isRunning = true;
            handler.removeCallbacks(this);
            handler.post(this);
        }
    }

    public boolean isLimit4k() {
        try {
            File flag = new File(getFilesDir(), "vr_limit_4k");
            if (flag.exists()) {
                return true;
            }
        } catch (Throwable ignored) {}
        try {
            Boolean hiveVal = VrState.readHiveBool(this, "vrLimit4k");
            if (hiveVal != null) {
                return hiveVal.booleanValue();
            }
        } catch (Throwable ignored) {}
        try {
            SharedPreferences sp = getSharedPreferences("vr_player_prefs", MODE_PRIVATE);
            if (sp.contains("limit_4k")) {
                return sp.getBoolean("limit_4k", false);
            }
        } catch (Throwable ignored) {}
        return false;
    }

    public void setLimit4k(boolean limit) {
        try {
            File flag = new File(getFilesDir(), "vr_limit_4k");
            if (limit) {
                if (!flag.exists()) flag.createNewFile();
            } else {
                if (flag.exists()) flag.delete();
            }
        } catch (Throwable ignored) {}
        try {
            SharedPreferences sp = getSharedPreferences("vr_player_prefs", MODE_PRIVATE);
            sp.edit().putBoolean("limit_4k", limit).apply();
        } catch (Throwable ignored) {}
    }

    private void toggleLimit4k() {
        boolean newVal = !isLimit4k();
        setLimit4k(newVal);
        showVrOsd(newVal ? "4K分辨率限制: 已开启 (2560防发热)" : "4K分辨率限制: 已关闭 (无上限自适应)");
        renderCapLongEdge = learnedCapLongEdge();
        lastSizeProbeAt = 0;
        maybeAdaptToVideoSize(SystemClock.uptimeMillis());
        updateHud();
    }

    private static int[] fitDims(int w, int h) {
        return fitDims(w, h, 0);
    }

    /**
     * Unlimited adaptive resolution: directly matches the decoded video resolution (1:1 pixel mapping)
     * without any artificial cap, preserving full 4K, 8K, or custom resolutions.
     */
    private static int[] fitDims(int w, int h, int capLongEdge) {
        if (w <= 0 || h <= 0) return new int[]{1920, 1080};
        int longEdge = Math.max(w, h);
        int nw = w;
        int nh = h;
        if (capLongEdge > 0 && longEdge > capLongEdge) {
            float s = (float) capLongEdge / (float) longEdge;
            nw = Math.round(w * s);
            nh = Math.round(h * s);
        }
        if ((nw & 1) != 0) nw--;
        if ((nh & 1) != 0) nh--;
        return new int[]{Math.max(2, nw), Math.max(2, nh)};
    }

    /** True while the user is pushing a stick or pressed a button very recently. */
    private boolean interacting(long now) {
        return sizeStickEngaged || distStickEngaged || (now - lastInteractionAt) < 1200;
    }

    /**
     * Cap for a new swapchain. Flat 2K: already past what the optics resolve at a normal screen
     * size, and ~2.2x cheaper than the 3840 that pushed the runtime into its
     * sustained-performance throttling.
     */
    private int learnedCapLongEdge() {
        if (isLimit4k()) {
            return 2560;
        }
        return MAX_RENDER_LONG_EDGE;
    }

    private static int clampi(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /**
     * libp5m paces its loop to whatever we ask for. If the panel will not deliver that rate it
     * falls back and explicitly accepts judder, so ask for the ceiling, then re-assert with the
     * rate the runtime actually returned to keep request and reality identical.
     */
    private int desiredPanelRefreshHz() {
        // AI 3D + ZipDepth runs at 72 Hz in both theater and passthrough so Adreno GPU
        // and the Quest compositor keep ample headroom during screen move/zoom without ASW drops.
        if (VrState.aiDepthEnabled && stereoMode == 3 && projectionMode == 0) {
            return 72;
        }
        if (stereoMode != 0 && projectionMode == 0) {
            return 90;
        }
        return 120;
    }

    private void alignPanelRate() {
        if (xr == null) return;
        int wantHz = desiredPanelRefreshHz();
        if (lastRequestedPanelHz == wantHz && panelRate > 24f) return;
        lastRequestedPanelHz = wantHz;
        try {
            float got = xr.selectDisplayRefreshRate(wantHz);
            Log.i(TAG, "Panel rate request " + wantHz + " -> " + got
                    + " (passthrough=" + passthrough + ", stereo=" + stereoMode + ")");
            if (got > 24f && Math.abs(got - (float) wantHz) > 0.5f) {
                float again = xr.selectDisplayRefreshRate((int) (got + 0.5f));
                Log.i(TAG, "Panel rate re-assert " + (int) (got + 0.5f) + " -> " + again);
                panelRate = got;
            } else {
                panelRate = got;
            }
        } catch (Throwable t) {
            lastRequestedPanelHz = 0;
            Log.w(TAG, "alignPanelRate failed", t);
        }
    }

    private void learnRecommended() {
        if (xr == null) return;
        int rec = recommendedLongEdge();
        if (rec <= 0) return;
        Log.i(TAG, "This screen would want " + rec + "px long edge; we render "
                + Math.max(renderWidth, renderHeight) + "px (cap " + renderCapLongEdge + ")");
    }

    /** Pixels the compositor will show for the current screen geometry, 0 when unknown. */
    private int recommendedLongEdge() {
        if (xr == null) return 0;
        try {
            int[] rec = xr.getRecommendedResolution();
            if (rec == null || rec.length < 2) return 0;
            if (rec[0] <= 0 || rec[1] <= 0) return 0;
            Log.i(TAG, "Recommended layer resolution: " + rec[0] + "x" + rec[1]);
            return Math.max(rec[0], rec[1]);
        } catch (Throwable t) {
            Log.w(TAG, "getRecommendedResolution failed", t);
            return 0;
        }
    }

    /** Freeze the very first decoded frame; the native curtain starts on the
     *  same frame. Playback resumes only after the panels fully part. */
    private void startCurtainFreeze() {
        curtainStarted = true;
        final long handle = VrState.mpvHandle;
        if (handle == 0) { // adb-only launch with no video source
            curtainDanmakuHidden = false;
            refreshDanmakuSuspension();
            return;
        }
        try {
            final int freezeGen = xrSessionGen;
            if (curtainRevealRunnable != null && handler != null) {
                handler.removeCallbacks(curtainRevealRunnable);
            }
            curtainDanmakuHidden = true;
            refreshDanmakuSuspension();
            MpvBridge.setProp(handle, "pause", "yes");
            curtainUntilMs = SystemClock.uptimeMillis() + curtainDurationMs;
            resumeGuardUntil = 0L; // guard must NOT undo the freeze
            lastResumeNudgeAt = SystemClock.uptimeMillis();
            curtainRevealRunnable = new Runnable() {
                @Override
                public void run() {
                    curtainRevealRunnable = null;
                    if (isFinishing() || curtainUntilMs == 0L || freezeGen != xrSessionGen) return;
                    curtainUntilMs = 0L;
                    curtainDanmakuHidden = false;
                    refreshDanmakuSuspension();
                    Log.i(TAG, "Intro curtain: overlay restored");
                    long h = VrState.mpvHandle;
                    if (h != 0) {
                        if (userExplicitlyPaused) {
                            MpvBridge.setProp(h, "pause", "yes");
                            resumeGuardUntil = 0L;
                            isPaused = true;
                        } else {
                            MpvBridge.setProp(h, "pause", "no");
                            resumeGuardUntil = SystemClock.uptimeMillis() + 3000L;
                            lastResumeNudgeAt = SystemClock.uptimeMillis();
                            resumeMpv("curtain reveal");
                            isPaused = false;
                        }
                    }
                    try { updateHud(); } catch (Throwable ignored) {}
                    Log.i(TAG, "Intro curtain revealed, userExplicitlyPaused=" + userExplicitlyPaused);
                }
            };
            handler.postDelayed(curtainRevealRunnable, curtainDurationMs);
            Log.i(TAG, "Intro curtain: first frame frozen (" + curtainDurationMs + " ms)");
        } catch (Throwable t) {
            Log.w(TAG, "curtain freeze failed", t);
        }
    }

    private void attachMpvToVrSurface() {
        if (vrSurface == null || !vrSurface.isValid()) {
            Log.e(TAG, "vrSurface is null or invalid, cannot attach mpv");
            return;
        }
        final long handle = VrState.mpvHandle;
        if (handle == 0) {
            Log.w(TAG, "mpvHandle is 0, waiting for video output creation");
            return;
        }

        try {
            if (vrWid == 0) {
                vrWid = MpvBridge.newGlobalRef(vrSurface);
            }
            if (vrWid != 0) {
                currentMpvHandle = handle;
                Log.i(TAG, "Redirecting mpv to OpenXR vrSurface (SurfaceTexture), wid=0x" + Long.toHexString(vrWid) + ", handle=0x" + Long.toHexString(handle));

                // 1. Point mpv VO to vrSurface
                int r1 = MpvBridge.setProp(handle, "wid", String.valueOf(vrWid));
                int r2 = MpvBridge.setProp(handle, "android-surface-size", renderWidth + "x" + renderHeight);
                Log.i(TAG, "mpv set wid result=" + r1 + ", android-surface-size result=" + r2);

                // 2. Do NOT seek here: a seek forces a video reconfiguration, which makes
                //    media_kit's Dart videoParams listener re-apply the Flutter wid and
                //    steal the output right after we set it. mpv redraws the current frame
                //    on VO re-creation anyway.
                lastFrameAt = SystemClock.uptimeMillis();
                lastWidAssertAt = lastFrameAt;
                userExplicitlyPaused = false;
                isPaused = false;
                // Let mpv paint the first frame onto the new VO. The freeze
                // starts from onFrameAvailable(), so the curtain and the
                // frozen frame are guaranteed to exist together.
                MpvBridge.setProp(handle, "pause", "no");
                resumeGuardUntil = lastFrameAt + 7000L;
                lastResumeNudgeAt = lastFrameAt;
                resumeMpv("VR attach");
            } else {
                Log.e(TAG, "Failed to create JNI global ref for vrSurface");
            }
        } catch (Throwable t) {
            Log.e(TAG, "attachMpvToVrSurface error", t);
        }
    }

    public void reloadVideo() {
        reassertVrQuality();
        long handle = VrState.mpvHandle;
        if (handle != 0) {
            try { MpvBridge.setProp(handle, "brightness", "0"); } catch (Throwable ignored) {}
            Log.i(TAG, "Manual video reload requested");
            MpvBridge.command(handle, "seek 0 relative");
        }
    }

    private void detachMpvFromVrSurface() {
        // Stop OpenXR rendering before restoring mpv's Flutter surface or
        // releasing VR surfaces so the frame loop cannot flash stale/warped frames.
        if (xr != null) {
            try { xr.stop(); } catch (Throwable ignored) {}
        }
        // Nothing to hand back if onPause already tore the VR surface down.
        if (vrWid == 0 && vrSurface == null) return;
        long handle = VrState.mpvHandle;
        clearPendingQualityResume(handle);
        if (handle != 0) {
            if (VrState.engineDetached) {
                VrState.stopOrphanedOrPauseOnVrExit(handle, isPaused);
            } else {
                long flutterWid = VrState.flutterWid();
                if (flutterWid != 0) {
                    Log.i(TAG, "Restoring mpv wid to Flutter surface: 0x" + Long.toHexString(flutterWid));
                    MpvBridge.setProp(handle, "wid", String.valueOf(flutterWid));
                    MpvBridge.setProp(handle, "android-surface-size", "");
                } else {
                    Log.i(TAG, "flutterWid is 0, invoking VideoOutput.a() to recreate Flutter surface");
                    try {
                        Object vo = VrState.videoOutput;
                        if (vo != null) {
                            vo.getClass().getMethod("a").invoke(vo);
                            long newWid = VrState.flutterWid();
                            if (newWid != 0) {
                                MpvBridge.setProp(handle, "wid", String.valueOf(newWid));
                                MpvBridge.setProp(handle, "android-surface-size", "");
                            }
                        }
                    } catch (Throwable t) {
                        Log.w(TAG, "Recreate flutter surface failed", t);
                    }
                }
                VrState.stopOrphanedOrPauseOnVrExit(handle, isPaused);
            }
        }

        if (vrWid != 0) {
            try {
                MpvBridge.deleteGlobalRef(vrWid);
            } catch (Throwable ignored) {}
            vrWid = 0;
        }

        if (vrSurface != null) {
            try { vrSurface.release(); } catch (Throwable ignored) {}
            vrSurface = null;
        }

        if (vrTexture != null) {
            try { vrTexture.release(); } catch (Throwable ignored) {}
            vrTexture = null;
        }

        currentMpvHandle = 0;
    }

    private void updateHud() {
        if (xr == null || hudRenderer == null) return;
        final long vrCid = readVrCurrentCid();
        final boolean isLive = vrCid < 0L;
        if (isLive) {
            applyLiveTitle(SystemClock.uptimeMillis());
        }
        hudRenderer.setLiveMode(isLive);
        try {
            // Live 3D depth throughput badge, drawn as part of the HUD card.
            hudRenderer.depthBadgeActive = (stereoMode == 3 && getProjModeIdx() == 0);
            hudRenderer.depthEnabled = VrState.aiDepthEnabled;
            hudRenderer.depthMapsPerSec = zipDepthSource != null
                    ? zipDepthSource.getMeasuredMapsPerSecond() : 0f;
            hudRenderer.depthEffectiveMapsPerSec = zipDepthSource != null
                    ? zipDepthSource.getEffectiveMapsPerSecond() : depthRateCap;
            hudRenderer.depthTargetMapsPerSec = depthRateCap;
            Bitmap bmp = hudRenderer.render(
                    videoTitle,
                    isPaused,
                    isMuted,
                    danmakuEnabled,
                    subtitleEnabled,
                    passthrough,
                    stereoMode,
                    synth3dLevel,
                    positionSec,
                    durationSec,
                    settingsOpen,
                    settingsTab,
                    qualityOptions,
                    currentQualityCode,
                    isLimit4k(),
                    screenCurveIdx,
                    screenPitchPresetIdx,
                    danmakuDistIdx,
                    danmakuSpanIdx,
                    danmakuVOut,
                    effectiveDanmakuDensityIdx(),
                    fixedDanmakuEnabled,
                    danmakuSizeIdx,
                    subtitleSizeIdx,
                    subProtIdx);
            xr.setHudBitmap(bmp);
            xr.setHudVisible(isHudVisible);
            if (settingsOpen) {
                updateSettingsPanel();
            } else {
                xr.setSettingsVisible(false);
            }
        } catch (Throwable t) {
            Log.w(TAG, "updateHud failed: " + t.getMessage());
        }
    }

    public void toggleDanmaku() {
        danmakuEnabled = !danmakuEnabled;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("danmaku_enabled", danmakuEnabled)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setDanmakuEnabled(danmakuEnabled);
        }
        showVrOsd(danmakuEnabled ? "弹幕: 开启" : "弹幕: 关闭");
        updateHud();
        Log.i(TAG, "Toggled danmaku: " + danmakuEnabled);
    }

    public void toggleSubtitle() {
        subtitleEnabled = !subtitleEnabled;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("subtitle_enabled", subtitleEnabled)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setSubtitleEnabled(subtitleEnabled);
        }
        showVrOsd(subtitleEnabled ? "CC字幕: 开启" : "CC字幕: 关闭");
        updateHud();
        Log.i(TAG, "Toggled CC subtitle: " + subtitleEnabled);
    }

    public void toggleMute() {
        long handle = VrState.mpvHandle;
        if (handle != 0) {
            isMuted = !isMuted;
            MpvBridge.setProp(handle, "mute", isMuted ? "yes" : "no");
            showVrOsd(isMuted ? "🔇 静音: 开启" : "🔊 声音: 开启");
            updateHud();
        }
    }

    public void toggleSettingsPanel() {
        settingsOpen = !settingsOpen;
        if (settingsOpen) {
            applyLiveTitle(SystemClock.uptimeMillis());
            reloadQualitiesTsv(true);
            // The forced quality reload must never restore a stale VOD title.
            applyLiveTitle(SystemClock.uptimeMillis());
            updateSettingsPanel();
        } else if (xr != null) {
            xr.setSettingsVisible(false);
        }
        updateHud();
    }

    public void updateSettingsPanel() {
        if (xr == null || hudRenderer == null) return;
        if (!settingsOpen) {
            xr.setSettingsVisible(false);
            return;
        }
        try {
            Bitmap bmp = hudRenderer.renderSettingsCard(
                    settingsTab,
                    qualityOptions,
                    currentQualityCode,
                    isLimit4k(),
                    screenCurveIdx,
                    screenPitchPresetIdx,
                    danmakuDistIdx,
                    danmakuSpanIdx,
                    danmakuVOut,
                    effectiveDanmakuDensityIdx(),
                    fixedDanmakuEnabled,
                    danmakuSizeIdx,
                    subtitleSizeIdx,
                    stereoMode,
                    synth3dLevel,
                    subProtIdx,
                    getProjModeIdx(),
                    stereoArrangement,
                    aiEdgeEnhanceIdx,
                    sbsUncompressed,
                    defaultPassthrough);
            xr.setSettingsBitmap(bmp);
            xr.setSettingsVisible(true);
        } catch (Throwable t) {
            Log.w(TAG, "updateSettingsPanel failed: " + t.getMessage());
        }
    }

    public int getProjModeIdx() {
        if (projectionMode == 0) return 0; // 巨幕影院
        if (projectionMode == 1) return 1; // VR180
        if (projectionMode == 2 || projectionMode == 3) return 2; // 双目180
        if (projectionMode == 4) return 3; // VR360
        if (projectionMode == 5 || projectionMode == 6) return 4; // 双目360
        return 0;
    }

    public int getStereoArrangement() {
        return stereoArrangement;
    }

    public void setProjectionModeFromSettings(int idx) {
        if (idx == 0) {
            projectionMode = 0;
            showVrOsd("播放模式: 巨幕影院");
        } else if (idx == 1) {
            projectionMode = 1;
            showVrOsd("播放模式: VR180 全景 (单目)");
        } else if (idx == 2) {
            projectionMode = (stereoArrangement == 1) ? 3 : 2;
            showVrOsd("播放模式: 双目180 3D (" + (stereoArrangement == 1 ? "上下" : "左右") + ")");
        } else if (idx == 3) {
            projectionMode = 4;
            showVrOsd("播放模式: VR360 全景 (单目)");
        } else if (idx == 4) {
            projectionMode = (stereoArrangement == 1) ? 6 : 5;
            showVrOsd("播放模式: 双目360 3D (" + (stereoArrangement == 1 ? "上下" : "左右") + ")");
        }
        if (xr != null) {
            xr.setProjectionMode(projectionMode);
        }
        if (danmakuEngine != null) {
            danmakuEngine.setProjectionMode(projectionMode);
        }
        enforceDanmaku3dConstraints();
        syncAiDepth();
        syncShowDepth();
        updateHud();
        updateSettingsPanel();
    }

    public void setStereoArrangementFromSettings(int arrIdx) {
        stereoArrangement = clampi(arrIdx, 0, 1);
        if (projectionMode == 2 || projectionMode == 3) {
            projectionMode = (stereoArrangement == 1) ? 3 : 2;
        } else if (projectionMode == 5 || projectionMode == 6) {
            projectionMode = (stereoArrangement == 1) ? 6 : 5;
        }
        if (xr != null) {
            xr.setProjectionMode(projectionMode);
        }
        showVrOsd("双目排列: " + (stereoArrangement == 1 ? "上下 (OU)" : "左右 (SBS)"));
        updateHud();
        updateSettingsPanel();
    }

    public void setSbsUncompressedFromSettings(boolean uncompressed) {
        sbsUncompressed = uncompressed;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("sbs_uncompressed", sbsUncompressed)
                    .apply();
        } catch (Throwable ignored) {}
        if (stereoMode != 1) {
            stereoMode = 1;
            applyStereoMode();
        } else {
            pushScreenParams(true);
        }
        showVrOsd(uncompressed ? "SBS 比例: 不压缩 (全宽)" : "SBS 比例: 压缩 (半宽)");
        updateHud();
        updateSettingsPanel();
    }

    public void setLimit4kFromSettings(boolean limit) {
        setLimit4k(limit);
        showVrOsd(limit ? "分辨率限制: 已开启 (2.5K防发热)" : "分辨率限制: 已关闭 (原画自适应)");
        renderCapLongEdge = learnedCapLongEdge();
        lastSizeProbeAt = 0;
        maybeAdaptToVideoSize(SystemClock.uptimeMillis());
        updateHud();
    }

    public void setScreenCurveIdx(int idx) {
        screenCurveIdx = clampi(idx, 0, 3);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("screen_curve_idx", screenCurveIdx)
                    .apply();
        } catch (Throwable ignored) {}
        applyScreenParams();
        String[] names = {"平面 (0%)", "微曲 (40%)", "标准曲面 (70%)", "环幕 (100%)"};
        showVrOsd("画面与弹幕曲率: " + names[screenCurveIdx]);
        updateHud();
    }

    public void setDanmakuDistIdx(int idx) {
        danmakuDistIdx = clampi(idx, 0, 3);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("danmaku_dist_idx", danmakuDistIdx)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setDanmakuParams(danmakuDistIdx, danmakuSpanIdx, danmakuVOut);
            applyPortraitDanmakuVOutDefault(videoW, videoH);
        }
        applyScreenParams();
        String[] names = {"贴屏 (99%)", "微浮 (88%)", "出屏 (76%)", "超近眼前 (64%)"};
        showVrOsd("弹幕距离: " + names[danmakuDistIdx]);
        updateHud();
    }

    public boolean is3dVideoActive() {
        if (projectionMode == 0) return stereoMode != 0;
        return projectionMode == 2 || projectionMode == 3 || projectionMode == 5 || projectionMode == 6;
    }

    private boolean enforceDanmaku3dConstraints() {
        boolean spanReset = false;
        if (is3dVideoActive() && danmakuSpanIdx == 3) {
            danmakuSpanIdx = 0;
            spanReset = true;
            try {
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .putInt("danmaku_span_idx", 0)
                        .apply();
            } catch (Throwable ignored) {}
        }
        if (danmakuEngine != null) {
            if (spanReset) {
                danmakuEngine.setDanmakuParams(danmakuDistIdx, danmakuSpanIdx, danmakuVOut);
            applyPortraitDanmakuVOutDefault(videoW, videoH);
            }
            danmakuEngine.setDanmakuDensity(effectiveDanmakuDensityIdx());
        }
        if (spanReset) {
            applyScreenParams();
        }
        return spanReset;
    }

    private int effectiveDanmakuDensityIdx() {
        if (danmakuSpanIdx == 3 || is3dVideoActive()) return 0;
        return danmakuDensityIdx;
    }

    public void setDanmakuSpanIdx(int idx) {
        int clamped = clampi(idx, 0, 3);
        if (clamped == 3 && is3dVideoActive()) {
            showVrOsd("仅限不开 3D 时开启 3D 环绕跑道");
            updateHud();
            return;
        }
        danmakuSpanIdx = clamped;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("danmaku_span_idx", danmakuSpanIdx)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setDanmakuParams(danmakuDistIdx, danmakuSpanIdx, danmakuVOut);
            applyPortraitDanmakuVOutDefault(videoW, videoH);
            danmakuEngine.setDanmakuDensity(effectiveDanmakuDensityIdx());
        }
        applyScreenParams();
        String[] names = {"常规平幕", "广角 130°", "180° 全景包围", "3D 环绕跑道"};
        if (danmakuSpanIdx == 3) {
            showVrOsd("弹幕形态: 3D 环绕跑道 (密度限制 25%)");
        } else {
            showVrOsd("弹幕形态: " + names[danmakuSpanIdx]);
        }
        updateHud();
    }

    public void setDanmakuVOut(boolean enabled) {
        danmakuVOut = enabled;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("danmaku_vout", danmakuVOut)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setDanmakuParams(danmakuDistIdx, danmakuSpanIdx, danmakuVOut);
            applyPortraitDanmakuVOutDefault(videoW, videoH);
        }
        applyScreenParams();
        showVrOsd("弹幕上下出屏: " + (danmakuVOut ? "开启 (天幕地面延展)" : "关闭 (仅视频高度)"));
        updateHud();
    }

    public void setScreenPitchPresetIdx(int idx) {
        screenPitchPresetIdx = clampi(idx, 0, 4);
        screenPitchOffset = PITCH_PRESETS_RAD[screenPitchPresetIdx];
        applyScreenParams();
        String[] names = {"平视 (0°)", "微仰 (15°)", "舒服 (30°)", "半躺 (45°)", "卧姿 (65°)"};
        showVrOsd("画面俯仰: " + names[screenPitchPresetIdx]);
        updateHud();
    }

    public int getSettingsTab() { return settingsTab; }

    public void setSettingsTab(int tab) {
        settingsTab = clampi(tab, 0, 3);
        updateHud();
    }

    public void setStereoModeFromSettings(int mode) {
        int next = clampi(mode, 0, 3);
        if (next == 3 && projectionMode != 0) {
            setProjectionModeFromSettings(0);
        }
        stereoMode = next;
        applyStereoMode();
        updateHud();
    }

    public void setSynth3dLevelFromSettings(int level) {
        synth3dLevel = clampi(level, 1, 3);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("synth_3d_level", synth3dLevel)
                    .apply();
        } catch (Throwable ignored) {}
        if (stereoMode == 3) {
            applyStereoMode();
        } else {
            showVrOsd("AI 3D 强度: " + SYNTH_3D_NAMES[synth3dLevel - 1] + " (切至 AI 3D 生效)");
        }
        updateHud();
    }

    public void setDanmakuDensityIdx(int idx) {
        int clamped = clampi(idx, 0, 3);
        if (danmakuSpanIdx == 3 && clamped > 0) {
            if (danmakuEngine != null) {
                danmakuEngine.setDanmakuDensity(0);
            }
            showVrOsd("3D 环绕跑道模式下弹幕密度上限为 25%");
            updateHud();
            return;
        }
        if (is3dVideoActive() && clamped > 0) {
            if (danmakuEngine != null) {
                danmakuEngine.setDanmakuDensity(0);
            }
            showVrOsd("开启 3D 时所有弹幕密度上限为 25%");
            updateHud();
            return;
        }
        danmakuDensityIdx = clamped;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("danmaku_density_idx", danmakuDensityIdx)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setDanmakuDensity(effectiveDanmakuDensityIdx());
        }
        String[] names = {"稀少 (25%)", "适中 (50%)", "标准 (75%)", "满屏 (100%)"};
        showVrOsd("弹幕密度: " + names[danmakuDensityIdx]);
        updateHud();
    }

    public void setFixedDanmakuFromSettings(boolean enabled) {
        fixedDanmakuEnabled = enabled;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("fixed_danmaku_enabled", fixedDanmakuEnabled)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setFixedDanmakuEnabled(fixedDanmakuEnabled);
        }
        showVrOsd("固定弹幕: " + (fixedDanmakuEnabled ? "开启 (显示顶部/底部)" : "关闭 (仅显示滚动)"));
        updateHud();
        updateSettingsPanel();
    }

    public void setDanmakuSizeIdx(int idx) {
        danmakuSizeIdx = clampi(idx, 0, 3);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("danmaku_size_idx", danmakuSizeIdx)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setDanmakuScale(SIZE_SCALES[danmakuSizeIdx]);
        }
        String[] names = {"小 (75%)", "标准 (100%)", "大 (125%)", "特大 (150%)"};
        showVrOsd("弹幕大小: " + names[danmakuSizeIdx]);
        updateHud();
    }

    public void setSubtitleSizeIdx(int idx) {
        subtitleSizeIdx = clampi(idx, 0, 3);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("subtitle_size_idx", subtitleSizeIdx)
                    .apply();
        } catch (Throwable ignored) {}
        if (danmakuEngine != null) {
            danmakuEngine.setSubtitleScale(SIZE_SCALES[subtitleSizeIdx]);
        }
        String[] names = {"小 (75%)", "标准 (100%)", "大 (125%)", "特大 (150%)"};
        showVrOsd("CC字幕大小: " + names[subtitleSizeIdx]);
        updateHud();
    }

    public void setSubProtIdx(int idx) {
        subProtIdx = clampi(idx, 0, 4);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("sub_prot_idx", subProtIdx)
                    .apply();
        } catch (Throwable ignored) {}
        applyStereoMode();
        String[] names = {"关闭 (全屏3D)", "小 (底部10%)", "标准 (底部16%)", "大 (底部22%)", "特大 (底部28%)"};
        showVrOsd("底部防3D扭曲区域: " + names[subProtIdx]);
    }

    private boolean isLivePlayback() {
        return readVrCurrentCid() < 0L;
    }

    private void clearPendingQualityResume(long handle) {
        pendingQualityResumePos = -1.0;
        pendingQualityResumeAtMs = 0L;
        pendingQualityResumeUntilMs = 0L;
        lastQualityResumeCheckMs = 0L;
        lastQualityFallbackSeekMs = 0L;
        qualityFallbackSeekAttempts = 0;
        pendingQualitySawReload = false;
        pendingQualityPrevPath = null;
        if (handle != 0) {
            try { MpvBridge.setProp(handle, "start", "none"); } catch (Throwable ignored) {}
        }
    }

    private void checkPendingQualityResume(long now) {
        if (pendingQualityResumePos <= 0.5) return;
        final long handle = VrState.mpvHandle;
        if (handle == 0 || now >= pendingQualityResumeUntilMs) {
            Log.w(TAG, "Quality resume finished/expired: target=" + pendingQualityResumePos
                    + " attempts=" + qualityFallbackSeekAttempts);
            clearPendingQualityResume(handle);
            return;
        }
        if (now - lastQualityResumeCheckMs < 180L) return;
        lastQualityResumeCheckMs = now;

        try {
            String durStr = MpvBridge.getProp(handle, "duration");
            double dur = -1.0;
            if (durStr != null && !durStr.isEmpty()) {
                try { dur = Double.parseDouble(durStr.trim()); } catch (Throwable ignored) {}
            }
            String posStr = MpvBridge.getProp(handle, "time-pos");
            double curPos = -1.0;
            if (posStr != null && !posStr.isEmpty()) {
                try { curPos = Double.parseDouble(posStr.trim()); } catch (Throwable ignored) {}
            }
            String curPath = MpvBridge.getProp(handle, "path");

            if (curPos < 0.0 || dur <= 0.5) {
                pendingQualitySawReload = true;
                return;
            }
            if (pendingQualityPrevPath != null && curPath != null && !curPath.equals(pendingQualityPrevPath)) {
                pendingQualitySawReload = true;
            }
            if (curPos < pendingQualityResumePos - 5.0) {
                pendingQualitySawReload = true;
            }

            long elapsed = now - pendingQualityResumeAtMs;
            if (!pendingQualitySawReload && elapsed < 1500L) {
                return;
            }

            double target = Math.min(Math.max(0.0, dur - 0.5), pendingQualityResumePos);
            double acceptFloor = (qualityFallbackSeekAttempts > 0)
                    ? Math.max(2.0, target - 15.0)
                    : Math.max(0.5, target - 5.0);
            if (curPos >= acceptFloor && curPos <= target + 25.0) {
                Log.i(TAG, "Quality resume verified: curPos=" + String.format(Locale.ENGLISH, "%.2f", curPos)
                        + " target=" + String.format(Locale.ENGLISH, "%.2f", target)
                        + " attempts=" + qualityFallbackSeekAttempts);
                positionSec = curPos;
                clearPendingQualityResume(handle);
                return;
            }

            if (curPos < acceptFloor && (now - lastQualityFallbackSeekMs >= 750L) && qualityFallbackSeekAttempts < 6) {
                lastQualityFallbackSeekMs = now;
                qualityFallbackSeekAttempts++;
                String seekTarget = String.format(Locale.ENGLISH, "%.2f", target);
                int rs = MpvBridge.command(handle, "seek " + seekTarget + " absolute");
                Log.i(TAG, "Quality resume fallback seek #" + qualityFallbackSeekAttempts
                        + ": curPos=" + String.format(Locale.ENGLISH, "%.2f", curPos)
                        + " -> target=" + seekTarget + " res=" + rs);
                positionSec = target;
            }
        } catch (Throwable t) {
            Log.w(TAG, "checkPendingQualityResume error", t);
        }
    }

    public void selectVideoQuality(final VrHudRenderer.QualityOption opt) {
        if (opt == null || isLivePlayback()) return;
        final long handle = VrState.mpvHandle;
        if (handle == 0 || opt.edlUri == null || opt.edlUri.isEmpty()) return;
        try {
            final long now = SystemClock.uptimeMillis();
            String posStr = MpvBridge.getProp(handle, "time-pos");
            double pos = positionSec;
            if (posStr != null && !posStr.isEmpty()) {
                try {
                    double parsedPos = Double.parseDouble(posStr.trim());
                    if (!Double.isNaN(parsedPos) && parsedPos >= 0.0) {
                        pos = parsedPos;
                    }
                } catch (Throwable ignored) {}
            }
            if (pendingQualityResumePos > 0.5 && now < pendingQualityResumeUntilMs && pos < 1.5) {
                pos = pendingQualityResumePos;
            }

            String prevPath = null;
            try { prevPath = MpvBridge.getProp(handle, "path"); } catch (Throwable ignored) {}

            int rStart = 0;
            if (pos > 0.5) {
                String startVal = String.format(Locale.ENGLISH, "%.2f", pos);
                rStart = MpvBridge.setProp(handle, "start", startVal);
                pendingQualityResumePos = pos;
                pendingQualityResumeAtMs = now;
                pendingQualityResumeUntilMs = now + 20000L;
                lastQualityResumeCheckMs = 0L;
                lastQualityFallbackSeekMs = 0L;
                qualityFallbackSeekAttempts = 0;
                pendingQualitySawReload = false;
                pendingQualityPrevPath = prevPath;
                positionSec = pos;
            } else {
                clearPendingQualityResume(handle);
            }

            String escapedUri = opt.edlUri.replace("'", "\\'");
            int rLoad = MpvBridge.command(handle, "loadfile '" + escapedUri + "' replace");
            Log.i(TAG, "selectVideoQuality: code=" + opt.code + " pos="
                    + String.format(Locale.ENGLISH, "%.2f", pos)
                    + " rStart=" + rStart + " rLoad=" + rLoad);
            currentQualityCode = opt.code;
            if (!userExplicitlyPaused) {
                resumeGuardUntil = now + 7000;
            }
            lastSizeProbeAt = 0;
            lastFrameAt = now;
            showVrOsd("切换视频分辨率: " + opt.fullDesc);
            updateHud();
        } catch (Throwable t) {
            Log.w(TAG, "selectVideoQuality failed", t);
        }
    }

    public void seekToFraction(float ratio) {
        if (isLivePlayback()) return;
        long handle = VrState.mpvHandle;
        if (handle != 0 && durationSec > 1.0) {
            double target = Math.max(0.0, Math.min(durationSec - 0.5, durationSec * ratio));
            positionSec = target;
            MpvBridge.command(handle, "seek " + String.format(Locale.ENGLISH, "%.2f", target) + " absolute");
            int total = (int) Math.round(target);
            String ts = String.format(Locale.ENGLISH, "%02d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
            showVrOsd("跳转至 " + ts);
            updateHud();
        }
    }

    private void reloadQualitiesTsv(boolean force) {
        final boolean liveSession = readVrCurrentCid() < 0L;
        if (liveSession) {
            if (!qualityOptions.isEmpty()) {
                qualityOptions.clear();
            }
            currentQualityCode = 0;
            lastQualitiesMtime = 0L;
            lastQualitiesLen = 0L;
            return;
        }
        try {
            File qFile = new File(getFilesDir(), "vr_qualities.tsv");
            if (!qFile.exists()) return;
            long mtime = qFile.lastModified();
            long len = qFile.length();
            if (!force && mtime == lastQualitiesMtime && len == lastQualitiesLen) return;
            lastQualitiesMtime = mtime;
            lastQualitiesLen = len;

            List<VrHudRenderer.QualityOption> parsed = new ArrayList<>();
            boolean currentWas8k = (currentQualityCode == 127) || (Math.max(videoW, videoH) > 4096);
            VrHudRenderer.QualityOption best4k = null;
            try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(qFile), StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) {
                    if (line.startsWith("#")) {
                        String[] meta = line.substring(1).split("\t");
                        for (String m : meta) {
                            if (m.startsWith("current=")) {
                                try { currentQualityCode = Integer.parseInt(m.substring(8).trim()); } catch (Throwable ignored) {}
                            } else if (!liveSession && m.startsWith("title=")) {
                                String t = m.substring(6).trim();
                                if (!t.isEmpty()) videoTitle = t;
                            }
                        }
                        if (currentQualityCode == 127) currentWas8k = true;
                        continue;
                    }
                    String[] parts = line.split("\t", 6);
                    if (parts.length >= 6) {
                        try {
                            int code = Integer.parseInt(parts[0].trim());
                            String sDesc = parts[1].trim();
                            String fDesc = parts[2].trim();
                            int w = Integer.parseInt(parts[3].trim());
                            int h = Integer.parseInt(parts[4].trim());
                            String edl = parts[5].trim();
                            boolean is8k = (code == 127)
                                    || Math.max(w, h) > 4096
                                    || sDesc.toUpperCase(Locale.ENGLISH).contains("8K")
                                    || fDesc.toUpperCase(Locale.ENGLISH).contains("8K");
                            if (is8k) {
                                if (code == currentQualityCode) currentWas8k = true;
                                continue;
                            }
                            VrHudRenderer.QualityOption opt = new VrHudRenderer.QualityOption(code, sDesc, fDesc, w, h, edl);
                            parsed.add(opt);
                            if (edl != null && !edl.isEmpty()) {
                                int optEdge = Math.max(w, h);
                                int bestEdge = (best4k != null) ? Math.max(best4k.width, best4k.height) : -1;
                                if (best4k == null
                                        || (code == 120 && best4k.code != 120)
                                        || (best4k.code != 120 && optEdge > bestEdge)) {
                                    best4k = opt;
                                }
                            }
                        } catch (Throwable ignored) {}
                    }
                }
            }
            qualityOptions.clear();
            qualityOptions.addAll(parsed);
            if (currentWas8k && best4k != null && VrState.mpvHandle != 0L && currentQualityCode != best4k.code) {
                Log.i(TAG, "8K stream detected (current=" + currentQualityCode + ", video=" + videoW + "x" + videoH + "), auto-switching to 4K: " + best4k.fullDesc);
                selectVideoQuality(best4k);
            }
        } catch (Throwable t) {
            Log.w(TAG, "reloadQualitiesTsv failed: " + t.getMessage());
        }
    }

    private long readVrCurrentCid() {
        File cidFile = new File(getFilesDir(), "vr_current_cid.txt");
        if (!cidFile.exists()) return 0L;
        try (BufferedReader br = new BufferedReader(new InputStreamReader(
                new FileInputStream(cidFile), StandardCharsets.UTF_8))) {
            String line = br.readLine();
            if (line != null && !line.trim().isEmpty()) {
                return Long.parseLong(line.trim());
            }
        } catch (Throwable ignored) {
        }
        return 0L;
    }

    /** Live rooms never rewrite vr_qualities.tsv, so their title must not come from the last VOD. */
    private boolean applyLiveTitle(long now) {
        long cid = readVrCurrentCid();
        if (cid >= 0L) {
            liveTitleFetchRoom = 0L;
            return false;
        }
        final long roomId = -cid;
        videoTitle = "直播间 " + roomId;
        File titleFile = new File(getFilesDir(), "vr_live_title.tsv");
        if (titleFile.exists()) {
            try (BufferedReader br = new BufferedReader(new InputStreamReader(
                    new FileInputStream(titleFile), StandardCharsets.UTF_8))) {
                String line = br.readLine();
                if (line != null) {
                    String[] parts = line.split("\t", 2);
                    if (parts.length == 2 && Long.parseLong(parts[0].trim()) == roomId) {
                        String t = parts[1].trim();
                        if (!t.isEmpty()) {
                            videoTitle = t;
                            return true;
                        }
                    }
                }
            } catch (Throwable ignored) {
            }
        }
        if (liveTitleFetchRoom != roomId || now - lastLiveTitleFetchMs > 15000L) {
            fetchLiveTitleAsync(roomId, now);
        }
        return true;
    }

    private void fetchLiveTitleAsync(final long roomId, long now) {
        liveTitleFetchRoom = roomId;
        lastLiveTitleFetchMs = now;
        new Thread(new Runnable() {
            @Override
            public void run() {
                HttpURLConnection conn = null;
                InputStream in = null;
                try {
                    conn = (HttpURLConnection) new URL(
                            "https://api.live.bilibili.com/room/v1/Room/get_info?room_id="
                                    + roomId).openConnection();
                    conn.setRequestMethod("GET");
                    conn.setConnectTimeout(6000);
                    conn.setReadTimeout(6000);
                    conn.setRequestProperty("User-Agent", "Mozilla/5.0 (Linux; Android 12; Quest 3)");
                    conn.setRequestProperty("Referer", "https://live.bilibili.com/");
                    int code = conn.getResponseCode();
                    if (code < 200 || code >= 300) return;
                    in = conn.getInputStream();
                    ByteArrayOutputStream buffer = new ByteArrayOutputStream();
                    byte[] chunk = new byte[4096];
                    int n;
                    while ((n = in.read(chunk)) > 0) buffer.write(chunk, 0, n);
                    JSONObject root = new JSONObject(new String(buffer.toByteArray(),
                            StandardCharsets.UTF_8));
                    JSONObject data = root.optJSONObject("data");
                    if (data == null) return;
                    String title = data.optString("title", "")
                            .replace('\r', ' ').replace('\n', ' ').replace('\t', ' ').trim();
                    if (title.isEmpty()) return;
                    byte[] bytes = (roomId + "\t" + title).getBytes(StandardCharsets.UTF_8);
                    File dir = getFilesDir();
                    File tmp = new File(dir, "vr_live_title.tsv.tmp");
                    File dst = new File(dir, "vr_live_title.tsv");
                    try (FileOutputStream fos = new FileOutputStream(tmp, false)) {
                        fos.write(bytes);
                        fos.flush();
                    }
                    if (dst.exists()) dst.delete();
                    if (!tmp.renameTo(dst)) {
                        try (FileOutputStream fos = new FileOutputStream(dst, false)) {
                            fos.write(bytes);
                            fos.flush();
                        }
                    }
                    Log.i(TAG, "live title resolved room=" + roomId + " title=" + title);
                } catch (Throwable t) {
                    Log.w(TAG, "live title fetch failed: " + t.getMessage());
                } finally {
                    if (in != null) try { in.close(); } catch (Throwable ignored) {}
                    if (conn != null) conn.disconnect();
                }
            }
        }, "PiliVRLiveTitle").start();
    }

    private void pollHudState(long now, boolean force) {
        if (!force && !isHudVisible) {
            if (!userExplicitlyPaused && now < resumeGuardUntil) {
                long handle = VrState.mpvHandle;
                if (handle != 0) {
                    try {
                        if ("yes".equalsIgnoreCase(MpvBridge.getProp(handle, "pause"))) {
                            MpvBridge.setProp(handle, "pause", "no");
                            isPaused = false;
                        }
                    } catch (Throwable ignored) {}
                }
            }
            return;
        }
        if (!force && (now - lastHudPollMs < 950L)) return;
        lastHudPollMs = now;
        final boolean liveSession = applyLiveTitle(now);
        reloadQualitiesTsv(false);
        if (liveSession) {
            durationSec = 0.0;
        }

        long handle = VrState.mpvHandle;
        if (handle != 0) {
            try {
                String posStr = MpvBridge.getProp(handle, "time-pos");
                if (posStr != null && !posStr.isEmpty()) {
                    double parsedPos = Double.parseDouble(posStr.trim());
                    if (pendingQualityResumePos <= 0.5 || parsedPos >= pendingQualityResumePos - 5.0) {
                        positionSec = parsedPos;
                    }
                }
                if (!liveSession) {
                    String durStr = MpvBridge.getProp(handle, "duration");
                    if (durStr != null && !durStr.isEmpty()) {
                        durationSec = Double.parseDouble(durStr.trim());
                    }
                }
                String p = MpvBridge.getProp(handle, "pause");
                if (curtainUntilMs != 0L && now < curtainUntilMs) {
                    // Intro curtain: intentionally frozen first frame. Show the user's
                    // intended post-curtain pause state and do not unfreeze early.
                    isPaused = userExplicitlyPaused;
                } else if (userExplicitlyPaused) {
                    isPaused = "yes".equalsIgnoreCase(p);
                } else {
                    if ("yes".equalsIgnoreCase(p)) {
                        if (now < resumeGuardUntil) {
                            MpvBridge.setProp(handle, "pause", "no");
                            isPaused = false;
                        } else {
                            isPaused = true;
                        }
                    } else {
                        isPaused = false;
                    }
                }
                String m = MpvBridge.getProp(handle, "mute");
                isMuted = "yes".equalsIgnoreCase(m);
                if (videoTitle == null || videoTitle.isEmpty()) {
                    String mt = MpvBridge.getProp(handle, "media-title");
                    if (mt != null && !mt.trim().isEmpty() && !mt.startsWith("edl://")) {
                        videoTitle = mt.trim();
                    }
                }
            } catch (Throwable ignored) {}
        }
        if (isHudVisible) {
            updateHud();
        }
    }

    public void toggleHudVisibility() {
        isHudVisible = !isHudVisible;
        if (!isHudVisible) {
            settingsOpen = false;
            hideResizeOsd();
        } else {
            hudHideTimeoutMs = 7000L;
            lastHudActionTime = SystemClock.uptimeMillis();
        }
        if (xr != null) {
            xr.setHudVisible(isHudVisible);
        }
        // Always redraw so the next show starts from a fresh frame.
        updateHud();
        showVrOsd(isHudVisible ? "HUD 悬浮面板: 显示" : "HUD 悬浮面板: 隐藏");
        Log.i(TAG, "Toggled HUD visibility: " + isHudVisible);
    }

    private void cycleSbsOuFormat() {
        if (stereoMode == 0 || stereoMode == 3) {
            stereoMode = 1; // 3D 左右 (SBS)
        } else if (stereoMode == 1) {
            stereoMode = 2; // 3D 上下 (OU)
        } else {
            stereoMode = 0; // 2D (关闭 3D格式)
        }
        applyStereoMode();
    }

    private void cycleSynth3dStrength() {
        if (stereoMode != 3) {
            stereoMode = 3;
            synth3dLevel = 1;
        } else {
            if (synth3dLevel < 3) {
                synth3dLevel++;
            } else {
                stereoMode = 0;
            }
        }
        applyStereoMode();
    }

    public void cycleStereoModeHud() {
        if (stereoMode == 0) {
            stereoMode = 3;
            synth3dLevel = 1;
        } else if (stereoMode == 3) {
            if (synth3dLevel < 3) {
                synth3dLevel++;
            } else {
                stereoMode = 1; // 3D 左右
            }
        } else if (stereoMode == 1) {
            stereoMode = 2; // 3D 上下
        } else {
            stereoMode = 0; // 2D
        }
        applyStereoMode();
    }

    private void applyStereoMode() {
        if (xr == null) return;
        String modeName;
        switch (stereoMode) {
            case 1:
                xr.setStereoMode(1, false);
                modeName = "3D 左右 (SBS)";
                break;
            case 2:
                xr.setStereoMode(2, false);
                modeName = "3D 上下 (OU)";
                break;
            case 3:
                xr.setStereoMode(1, true);
                int lvlIdx = clampi(synth3dLevel, 1, 3) - 1;
                synth3dLevel = lvlIdx + 1;
                float disp = SYNTH_3D_DISP[lvlIdx];
                int protPct = SUB_PROT_PCTS[clampi(subProtIdx, 0, 4)];
                float convParam = (protPct > 0) ? (protPct + 0.35f) : 0.35f;
                xr.setStereoTuning(disp, convParam);
                modeName = "AI 3D 景深: " + SYNTH_3D_NAMES[lvlIdx];
                break;
            case 0:
            default:
                stereoMode = 0;
                xr.setStereoMode(0, false);
                modeName = "2D (普通画面)";
                break;
        }
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("stereo_mode", stereoMode)
                    .putInt("synth_3d_level", synth3dLevel)
                    .apply();
        } catch (Throwable ignored) {}
        boolean spanReset = enforceDanmaku3dConstraints();
        String osdSuffix = "";
        if (spanReset) {
            osdSuffix = " · 弹幕已切常规平幕 (限25%)";
        } else if (is3dVideoActive() && danmakuDensityIdx > 0) {
            osdSuffix = " · 弹幕密度已限 25%";
        }
        showVrOsd("播放模式: " + modeName + osdSuffix);
        Log.i(TAG, "Switched stereo mode to: " + modeName + " (id=" + stereoMode + ", level=" + synth3dLevel + ", subProt=" + SUB_PROT_PCTS[clampi(subProtIdx, 0, 4)] + "%, dmSpan=" + danmakuSpanIdx + ", dmEffDens=" + effectiveDanmakuDensityIdx() + ")");
        pushScreenParams(true);
        updateHud();
        syncAiDepth();
    }

    /** Settings-card toggle for the depth source. The stock 3D stays available. */
    public void setAiDepthFromSettings(boolean on) {
        VrState.aiDepthEnabled = on;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("ai_depth_enabled", on).apply();
        } catch (Throwable t) {
            Log.w(TAG, "ai depth pref write failed", t);
        }
        syncAiDepth();
        showVrOsd(on ? "AI 深度来源: ZipDepth" : "AI 深度来源: 原版 (内置)");
        updateHud();
        updateSettingsPanel();
    }

    /** Overlay is hidden only while the intro curtain runs. */
    private void refreshDanmakuSuspension() {
        if (danmakuEngine != null) {
            danmakuEngine.setSuspended(curtainDanmakuHidden);
        }
    }

    private void applyAiResourcePolicy(boolean active) {
        // Shared-EGL staging and dual-thread ZipDepth keep the frame loop under ~1%
        // overhead, and adaptive timer pacing protects frame budget even above 1080p,
        // so both full stream resolution and danmaku/subtitles stay enabled.
    }

    private void applyAiEdgeEnhance() {
        int idx = clampi(aiEdgeEnhanceIdx, 0, 2);
        aiEdgeEnhanceIdx = idx;
        try {
            AiDepthNative.setEdgeEnhance(AI_EDGE_SHARP_VALUES[idx], AI_EDGE_CUBIC_VALUES[idx]);
        } catch (Throwable ignored) {}
    }

    public void setAiEdgeEnhanceFromSettings(int idx) {
        aiEdgeEnhanceIdx = clampi(idx, 0, 2);
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putInt("ai_edge_enhance_idx", aiEdgeEnhanceIdx).apply();
        } catch (Throwable ignored) {}
        applyAiEdgeEnhance();
        showVrOsd("AI 边缘增强: " + AI_EDGE_NAMES[aiEdgeEnhanceIdx]);
        updateHud();
        updateSettingsPanel();
        Log.i(TAG, "AI edge enhance -> idx=" + aiEdgeEnhanceIdx
                + " sharp=" + AI_EDGE_SHARP_VALUES[aiEdgeEnhanceIdx]
                + " cubic=" + AI_EDGE_CUBIC_VALUES[aiEdgeEnhanceIdx]);
    }

    private void syncAiDepth() {
        alignPanelRate();
        if (zipDepthSource == null) {
            if (xr == null) return;
            startZipDepthAsync();
            return; // the desired mode is applied when the model is ready
        }
        // Only synthetic 3D in cinema mode uses a depth map. Panoramas and SBS/OU/2D keep native behaviour.
        boolean active = VrState.aiDepthEnabled && stereoMode == 3 && projectionMode == 0;
        zipDepthSource.setEnabled(active);
        if (active) {
            zipDepthSource.onEnvironmentChanged();
        }
        applyAiResourcePolicy(active);
        syncShowDepth();
        Log.i(TAG, "AI depth source: " + (active ? "ZipDepth" : "stock heuristic"));
    }

    private void stopZipDepth() {
        if (zipDepthDelayRunnable != null && handler != null) {
            handler.removeCallbacks(zipDepthDelayRunnable);
            zipDepthDelayRunnable = null;
        }
        ZipDepthSource src = zipDepthSource;
        zipDepthSource = null;
        if (src != null) {
            try { src.stop(); } catch (Throwable ignored) {}
        }
    }

    /**
     * Open the AI depth model off the main thread, after the first video
     * frames are visible. Until the interpreter is ready the native renderer
     * keeps its stock heuristic depth, so the picture shows immediately
     * instead of a multi-second black screen.
     */
    private void startZipDepthAsync() {
        final int sessionGen = xrSessionGen;
        if (zipDepthDelayRunnable != null && handler != null) {
            handler.removeCallbacks(zipDepthDelayRunnable);
            zipDepthDelayRunnable = null;
        }
        final boolean cached = ZipDepthSource.isModelCached(getCacheDir());
        curtainDurationMs = cached ? CURTAIN_MS_CACHED : CURTAIN_MS_COLD;
        // native uses the SAME value; set it before the first rendered frame.
        try {
            AiDepthNative.setCurtainDuration((int) curtainDurationMs);
        } catch (Throwable t) {
            Log.w(TAG, "curtain duration JNI failed", t);
        }
        applyAiEdgeEnhance();
        Log.i(TAG, "Intro curtain duration " + curtainDurationMs
                + " ms (model cached=" + cached + ")");
        final ZipDepthSource src = ZipDepthSource.get(getAssets(), getCacheDir());
        src.setHighPriority(true);
        src.setFastEma(getIntent() == null
                || !getIntent().getBooleanExtra("keep_ema", false));
        src.setTargetMapsPerSecond(depthRateCap);
        zipDepthSource = src; // volatile-ish: updateHud() reads it right away
        // Give xr.start() and the first decoded frames a head start before
        // the GPU delegate competes for Adreno during shader compilation.
        zipDepthDelayRunnable = new Runnable() {
            @Override
            public void run() {
                zipDepthDelayRunnable = null;
                if (isFinishing() || xr == null || sessionGen != xrSessionGen || zipDepthSource != src) return;
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            src.start(); // seconds of work, off the UI thread
                        } catch (Throwable t) {
                            Log.w(TAG, "ZipDepth async init failed", t);
                            return;
                        }
                        handler.post(new Runnable() {
                            @Override
                            public void run() {
                                if (isFinishing() || xr == null || sessionGen != xrSessionGen || zipDepthSource != src) return;
                                boolean active = VrState.aiDepthEnabled && stereoMode == 3 && projectionMode == 0;
                                src.setEnabled(active);
                                applyAiResourcePolicy(active);
                                syncShowDepth();
                                updateHud();
                                Log.i(TAG, "AI depth source ready: "
                                        + (active ? "ZipDepth" : "stock heuristic"));
                            }
                        });
                    }
                }, "ZipDepth-Init").start();
            }
        };
        handler.postDelayed(zipDepthDelayRunnable, 400);
    }

    /**
     * Greyscale/heat-map depth debug view. Only meaningful while synthetic 3D in cinema mode
     * is producing a depth texture; in panoramas or 2D/SBS/OU there is no depth map to show.
     */
    private boolean isShowDepthActive() {
        return VrState.showDepthMap && stereoMode == 3 && projectionMode == 0;
    }

    private void syncShowDepth() {
        boolean show = isShowDepthActive();
        try {
            AiDepthNative.setShowDepth(show);
        } catch (Throwable t) {
            Log.w(TAG, "setShowDepth failed", t);
        }
    }

    private void saveShowDepthPref(boolean on) {
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("show_depth_map", on).apply();
        } catch (Throwable t) {
            Log.w(TAG, "show depth pref write failed", t);
        }
    }

    /** Settings-card / long-press Y toggle for the depth-map debug view. */
    public void setShowDepthFromSettings(boolean on) {
        if (on) {
            if (projectionMode != 0) {
                VrState.showDepthMap = false;
                syncShowDepth();
                showVrOsd("当前全景模式不支持深度图 (需巨幕模式)");
                updateHud();
                updateSettingsPanel();
                return;
            }
            if (stereoMode == 0) {
                VrState.showDepthMap = true;
                saveShowDepthPref(true);
                stereoMode = 3;
                applyStereoMode();
                showVrOsd("深度图: 开 (已自动开启 AI 3D)");
                updateHud();
                updateSettingsPanel();
                return;
            }
            if (stereoMode != 3) {
                VrState.showDepthMap = false;
                syncShowDepth();
                showVrOsd("请先切换至「AI合成 3D」再开启深度图");
                updateHud();
                updateSettingsPanel();
                return;
            }
        }
        VrState.showDepthMap = on;
        saveShowDepthPref(on);
        syncShowDepth();
        Log.i(TAG, "Show depth map: " + on);
        showVrOsd(on ? "深度图: 开 (近红远蓝)" : "深度图: 关 (正常画面)");
        updateHud();
        updateSettingsPanel();
    }

    /** Settings-card cap for depth-map captures: 20 (stable) or 30 (experimental). */
    public void setDepthRateCapFromSettings(int mapsPerSecond) {
        int cap = mapsPerSecond >= ZipDepthSource.EXPERIMENTAL_MAPS_PER_SECOND
                ? ZipDepthSource.EXPERIMENTAL_MAPS_PER_SECOND
                : ZipDepthSource.DEFAULT_MAPS_PER_SECOND;
        depthRateCap = cap;
        if (zipDepthSource != null) {
            zipDepthSource.setTargetMapsPerSecond(cap);
        }
        showVrOsd(cap == 30 ? "深度帧率上限: 30 张/秒 (实验)" : "深度帧率上限: 20 张/秒");
        updateHud();
        updateSettingsPanel();
        Log.i(TAG, "depth rate cap=" + cap);
    }

    // ---------------------------------------------------------------
    // Screen size / distance
    // ---------------------------------------------------------------

    private void applyScreenParams() {
        pushScreenParams(true);
    }

    /**
     * Throttled, change-gated hand-off to libp5m. Measured on device this call costs ~0.01 ms, so
     * it is pushed on every frame of the stick loop: rate limiting it made the screen grow in
     * 60ms steps, which reads as jank even though the video frames were fine.
     */
    private void pushScreenParams(boolean force) {
        if (xr == null) return;
        float aspect = renderHeight > 0 ? (float) renderWidth / (float) renderHeight : 1.7778f;
        if (projectionMode == 0 && stereoMode == 1 && sbsUncompressed) {
            aspect *= 0.5f;
        }
        // Perspective apparent FOV: screen size inversely scales with physical viewing distance relative to reference 2.5m
        float distRatio = (float) Math.pow(DEFAULT_DIST / screenDist, 0.5);
        float effectiveFov = 2.0f * (float) Math.atan(Math.tan(screenFov * 0.5f) * distRatio);
        effectiveFov = clamp(effectiveFov, MIN_FOV, MAX_FOV);
        float fov = aspect < 1.0f ? effectiveFov * aspect : effectiveFov;
        xr.setAspectRatio(aspect);
        if (!force && !paramsMoved(fov)) return;
        long t0 = System.nanoTime();
        int cIdx = clampi(screenCurveIdx, 0, 3);
        int dIdx = clampi(danmakuDistIdx, 0, 3);
        int sIdx = clampi(danmakuSpanIdx, 0, 3);
        xr.setLayerShape(cIdx == 0 ? 1 : 0);
        float curve = (cIdx == 0) ? 0.05f : CURVE_VALUES[cIdx];
        float dmParam = 1000.0f + (sIdx * 100f) + (danmakuVOut ? 10f : 0f) + dIdx;
        xr.setScreenParams(screenDist, fov, dmParam, screenPitchOffset, curve);
        xr.setScreenOffset(screenPitchOffset, screenYawOffset);
        AiDepthNative.syncBaseScreenState(screenFov, screenDist);
        if (danmakuEngine != null) {
            danmakuEngine.setScreenViewParams(screenDist, fov);
        }
        lastPushedFov = fov;
        lastPushedDist = screenDist;
        lastPushedPitch = screenPitchOffset;
        lastPushedYaw = screenYawOffset;
        lastPushedCurveIdx = cIdx;
        lastPushedDistIdx = dIdx;
        lastPushedSpanIdx = sIdx;
        lastPushedVOut = danmakuVOut;
        recordSsp(System.nanoTime() - t0);
    }

    private boolean paramsMoved(float fov) {
        if (lastPushedFov <= 0f) return true;
        return Math.abs(fov - lastPushedFov) > 1e-4f
                || Math.abs(screenDist - lastPushedDist) > 1e-4f
                || Math.abs(screenPitchOffset - lastPushedPitch) > 1e-4f
                || Math.abs(screenYawOffset - lastPushedYaw) > 1e-4f
                || lastPushedCurveIdx != screenCurveIdx
                || lastPushedDistIdx != danmakuDistIdx
                || lastPushedSpanIdx != danmakuSpanIdx
                || lastPushedVOut != danmakuVOut;
    }

    private void recordSsp(long ns) {
        dbgSspN++;
        dbgSspNs += ns;
        if (ns > dbgSspMaxNs) dbgSspMaxNs = ns;
    }

    private void recordOsd(long ns) {
        dbgOsdN++;
        dbgOsdNs += ns;
        if (ns > dbgOsdMaxNs) dbgOsdMaxNs = ns;
    }

    private void recordPoll(long ns) {
        dbgPollN++;
        dbgPollNs += ns;
        if (ns > dbgPollMaxNs) dbgPollMaxNs = ns;
    }

    private static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    private static final float MIN_FOV = 0.05f;
    private static final float MAX_FOV = 3.00f;
    private static final float MIN_DIST = 0.2f;
    private static final float MAX_DIST = 25.0f;

    private static String fmtDist(float d) {
        return d < 1.0f
                ? String.format(Locale.ENGLISH, "%.2f", d)
                : String.format(Locale.ENGLISH, "%.1f", d);
    }

    /**
     * Dead-zoned stick magnitude with a smoothstep ramp: nothing happens until the stick
     * passes 0.30, and small pushes stay small so the adjustment feels stepless without
     * accidental activation when tilting the stick horizontally.
     */
    private static float analogMag(float v) {
        float a = Math.abs(v);
        if (a < 0.15f) return 0f;
        a = (a - 0.15f) / 0.85f;
        if (a > 1f) a = 1f;
        return a * a * (3f - 2f * a);
    }

    /** Continuous (stepless) right-stick screen size: rate follows how far it is pushed. */
    private void updateScreenSizeFromStick(float v, float dtSec, long now) {
        float mag = analogMag(v);
        if (mag <= 0f) {
            if (sizeStickEngaged) {
                sizeStickEngaged = false;
                applyScreenParams();
                saveScreenPrefs();
                applyWindowBrightness();
                showResizeOsd("\u753B\u9762\u5927\u5C0F: " + Math.round(screenFov / DEFAULT_FOV * 100f) + "%");
                Log.i(TAG, "Screen size settled at " + String.format(Locale.ENGLISH, "%.3f", screenFov) + " rad");
            }
            return;
        }
        if (!sizeStickEngaged) {
            applyWindowBrightness();
        }
        sizeStickEngaged = true;
        lastInteractionAt = now;
        float dir = v > 0 ? 1f : -1f;
        screenFov = clamp(screenFov * (float) Math.exp(dir * (0.15f + 0.85f * mag) * dtSec), MIN_FOV, MAX_FOV);
        pushScreenParams(false);
        if (now - lastSizeOsdAt > 120) {
            lastSizeOsdAt = now;
            showResizeOsd("\u753B\u9762\u5927\u5C0F: " + Math.round(screenFov / DEFAULT_FOV * 100f) + "%");
        }
    }

    /** Continuous (stepless) left-stick viewing distance. */
    private void updateScreenDistanceFromStick(float v, float dtSec, long now) {
        float mag = analogMag(v);
        if (mag <= 0f) {
            if (distStickEngaged) {
                distStickEngaged = false;
                applyScreenParams();
                saveScreenPrefs();
                applyWindowBrightness();
                showResizeOsd("\u89C6\u8DDD: " + fmtDist(screenDist) + " m");
                Log.i(TAG, "Screen distance settled at " + String.format(Locale.ENGLISH, "%.2f", screenDist) + " m");
            }
            return;
        }
        if (!distStickEngaged) {
            applyWindowBrightness();
        }
        distStickEngaged = true;
        lastInteractionAt = now;
        // Stick UP (v > 0) = push farther (+), stick DOWN (v < 0) = pull closer (-)
        float speed = Math.max(0.3f, screenDist * 0.65f); // Progressive speed: fine at 0.2m, fast at 25m
        float delta = (v > 0 ? 1f : -1f) * (speed * mag) * dtSec;
        screenDist = clamp(screenDist + delta, MIN_DIST, MAX_DIST);
        pushScreenParams(false);
        if (now - lastDistOsdAt > 120) {
            lastDistOsdAt = now;
            showResizeOsd("\u89C6\u8DDD: " + fmtDist(screenDist) + " m");
        }
    }

    private void saveScreenPrefs() {
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putFloat("screen_fov", screenFov)
                    .putFloat("screen_dist", screenDist)
                    .apply();
        } catch (Throwable ignored) {}
    }

    private String getResolutionText() {
        int[] sz = VrState.videoSize();
        if (sz != null && sz[0] > 0 && sz[1] > 0) {
            videoW = sz[0];
            videoH = sz[1];
        }
        if (videoW > 0 && videoH > 0) {
            renderCapLongEdge = learnedCapLongEdge();
            int[] dims = fitDims(videoW, videoH, renderCapLongEdge);
            return "分辨率：" + dims[0] + "*" + dims[1];
        }
        if (renderWidth > 0 && renderHeight > 0) {
            return "分辨率：" + renderWidth + "*" + renderHeight;
        }
        return null;
    }

    private void scheduleResolutionOsd(final int attempt) {
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!isRunning || xr == null) return;
                long now = SystemClock.uptimeMillis();
                if (interacting(now)) {
                    if (attempt < 10) {
                        scheduleResolutionOsd(attempt + 1);
                    }
                    return;
                }
                String res = getResolutionText();
                if (res != null) {
                    showVrOsd(res);
                } else if (attempt < 6) {
                    scheduleResolutionOsd(attempt + 1);
                }
            }
        }, attempt == 0 ? 3500 : 800);
    }

    /**
     * Draws the settings bar into the video with mpv's OSD, so it shows up on the
     * VR screen right below the title. libp5m's HUD layer is not composited in this
     * build, so the video OSD is the only overlay surface we control.
     */
    private void showResizeOsd(String status) {
        if (!isHudVisible) return;
        showVrOsd(status);
    }

    private void hideResizeOsd() {
        if (isOsdVisible && currentOsdText != null
                && (currentOsdText.startsWith("屏幕大小:")
                        || currentOsdText.startsWith("\u753B\u9762\u5927\u5C0F:")
                        || currentOsdText.startsWith("\u89C6\u8DDD:"))) {
            isOsdVisible = false;
            currentOsdText = null;
            osdExpireTime = 0L;
            lastRenderedOsdText = "";
            if (xr != null) {
                xr.setOsdVisible(false);
            }
        }
    }

    private void showVrOsd(String status) {
        if (status == null) return;
        final String text = status.trim();
        if (text.isEmpty()) return;

        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    showVrOsdUi(text);
                } catch (Throwable t) {
                    Log.w(TAG, "showVrOsdUi error: " + t);
                }
            }
        });
    }

    private void showVrOsdUi(String text) {
        long now = SystemClock.uptimeMillis();
        currentOsdText = text;
        osdExpireTime = now + 1600L;

        if (xr == null) return;
        if (isOsdVisible && text.equals(lastRenderedOsdText)) {
            return;
        }
        lastRenderedOsdText = text;

        int w = 1024;
        int h = 140;
        if (currentOsdBitmap == null || currentOsdBitmap.isRecycled()) {
            currentOsdBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        } else {
            currentOsdBitmap.eraseColor(Color.TRANSPARENT);
        }

        Canvas c = new Canvas(currentOsdBitmap);
        float textSize = 38f;
        osdTextPaint.setTextSize(textSize);
        osdTextPaint.setTextAlign(Paint.Align.CENTER);
        float textWidth = osdTextPaint.measureText(text);

        float padX = 40f;
        float pillW = Math.max(textWidth + padX * 2f, 280f);
        float pillH = 72f;

        float cx = w * 0.5f;
        float cy = h * 0.5f;
        RectF pill = new RectF(cx - pillW * 0.5f, cy - pillH * 0.5f, cx + pillW * 0.5f, cy + pillH * 0.5f);
        VrHudRenderer.addSquirclePath(osdPillPath, pill, 22f);

        c.drawPath(osdPillPath, osdBgPaint);
        c.drawPath(osdPillPath, osdBorderPaint);

        Paint.FontMetrics fm = osdTextPaint.getFontMetrics();
        float baseline = cy - (fm.ascent + fm.descent) * 0.5f;
        c.drawText(text, cx, baseline, osdTextPaint);

        xr.setOsdBitmap(currentOsdBitmap);
        xr.setOsdVisible(true);
        isOsdVisible = true;
    }

    public void showResolutionBadge(int width, int height) {
        if (width <= 0 || height <= 0) return;
        final String badge = width + "*" + height;
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    showResolutionBadgeUi(badge);
                } catch (Throwable t) {
                    Log.w(TAG, "showResolutionBadgeUi error: " + t);
                }
            }
        });
    }

    private void showResolutionBadgeUi(String text) {
        long now = SystemClock.uptimeMillis();
        resBadgeExpireTime = now + 4000L;

        if (xr == null) return;

        int w = 384;
        int h = 128;
        if (currentResBitmap == null || currentResBitmap.isRecycled()) {
            currentResBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        } else {
            currentResBitmap.eraseColor(Color.TRANSPARENT);
        }

        Canvas c = new Canvas(currentResBitmap);
        Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextSize(40f);
        textPaint.setTypeface(Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
        textPaint.setTextAlign(Paint.Align.CENTER);

        float textWidth = textPaint.measureText(text);
        float padX = 36f;
        float boxW = textWidth + padX * 2f;
        float boxH = 68f;

        float cx = w * 0.5f;
        float cy = h * 0.5f;
        RectF box = new RectF(cx - boxW * 0.5f, cy - boxH * 0.5f, cx + boxW * 0.5f, cy + boxH * 0.5f);
        Path boxPath = VrHudRenderer.addSquirclePath(new Path(), box, 18f);

        Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        bgPaint.setColor(0xEB151822); // Frosted obsidian glass matching HUD
        bgPaint.setStyle(Paint.Style.FILL);
        c.drawPath(boxPath, bgPaint);

        Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        borderPaint.setColor(0x45FFFFFF); // Crisp hairline specular border
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(2.4f);
        c.drawPath(boxPath, borderPaint);

        // Exact vertical center for text
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float baseline = cy - (fm.descent + fm.ascent) * 0.5f;
        c.drawText(text, cx, baseline, textPaint);

        xr.setResolutionBadgeBitmap(currentResBitmap);
        xr.setResolutionBadgeVisible(true);
        isResBadgeVisible = true;
    }

    private Bitmap createHudBitmap(boolean paused, boolean pass) {
        int w = 1024;
        int h = 140;
        Bitmap bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(bmp);

        // Background pill
        Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        bgPaint.setColor(Color.argb(220, 18, 18, 24));
        RectF pill = new RectF(10, 10, w - 10, h - 10);
        c.drawRoundRect(pill, 40, 40, bgPaint);

        // Border
        Paint borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(2.5f);
        borderPaint.setColor(Color.argb(120, 255, 255, 255));
        c.drawRoundRect(pill, 40, 40, borderPaint);

        String modeLabel = "👓 2D [Y]";
        if (stereoMode == 1) modeLabel = "👓 3D左右";
        else if (stereoMode == 2) modeLabel = "👓 3D上下";
        else if (stereoMode == 3) {
            if (synth3dLevel == 1) modeLabel = "👓 AI:弱 [Y]";
            else if (synth3dLevel == 2) modeLabel = "👓 AI:中 [Y]";
            else modeLabel = "👓 AI:强 [Y]";
        }

        // 6 buttons: Play/Pause [A], 3D [Y], Danmaku, Hide HUD [R3], Passthrough [B], Exit [X]
        String[] labels = {
            paused ? "\u25B6 \u64AD\u653E [A]" : "\u23F8 \u6682\u505C [A]",
            modeLabel,
            danmakuEnabled ? "\uD83D\uDCAC \u5F39\u5E55:\u5F00" : "\uD83D\uDCAC \u5F39\u5E55:\u5173",
            "\uD83D\uDC41 \u9690\u85CF [R3]",
            pass ? "\uD83C\uDF0C \u573A\u666F [B]" : "\uD83C\uDF0C \u900F\u89C6 [B]",
            "\uD83D\uDD19 \u9000\u51FA [X]"
        };

        Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextSize(20f);
        textPaint.setFakeBoldText(true);
        textPaint.setTextAlign(Paint.Align.CENTER);

        // Uniform dark neutral button styling (no colored rainbow buttons)
        Paint btnBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        btnBgPaint.setColor(Color.argb(170, 44, 46, 56));

        Paint btnBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        btnBorderPaint.setStyle(Paint.Style.STROKE);
        btnBorderPaint.setStrokeWidth(1.5f);
        btnBorderPaint.setColor(Color.argb(70, 255, 255, 255));

        float btnW = 150f;
        float btnH = 76f;
        float spacing = 12f;
        float totalW = 6 * btnW + 5 * spacing;
        float startX = (w - totalW) / 2f;
        float startY = (h - btnH) / 2f;

        for (int i = 0; i < 6; i++) {
            float x = startX + i * (btnW + spacing);
            RectF r = new RectF(x, startY, x + btnW, startY + btnH);
            c.drawRoundRect(r, 18, 18, btnBgPaint);
            c.drawRoundRect(r, 18, 18, btnBorderPaint);

            Paint.FontMetrics fm = textPaint.getFontMetrics();
            float baseline = r.centerY() - (fm.ascent + fm.descent) / 2f;
            c.drawText(labels[i], r.centerX(), baseline, textPaint);
        }
        return bmp;
    }

    public void togglePlayPause() {
        long handle = VrState.mpvHandle;
        if (handle != 0) {
            if (curtainUntilMs != 0L) {
                userExplicitlyPaused = !userExplicitlyPaused;
                isPaused = userExplicitlyPaused;
                if (isPaused) {
                    resumeGuardUntil = 0L;
                }
                MpvBridge.setProp(handle, "pause", "yes");
                updateHud();
                showVrOsd(isPaused ? "⏸ 暂停" : "▶ 播放");
                Log.i(TAG, "Toggled post-curtain pause state, userExplicit=" + userExplicitlyPaused);
                return;
            }
            MpvBridge.command(handle, "cycle pause");
            String p = MpvBridge.getProp(handle, "pause");
            isPaused = "yes".equalsIgnoreCase(p);
            userExplicitlyPaused = isPaused;
            if (isPaused) {
                resumeGuardUntil = 0;
            } else {
                resumeGuardUntil = SystemClock.uptimeMillis() + 3000L;
            }
            updateHud();
            showVrOsd(isPaused ? "⏸ 暂停" : "▶ 播放");
            Log.i(TAG, "Toggled play/pause, isPaused=" + isPaused + ", userExplicit=" + userExplicitlyPaused);
        }
    }

    /**
     * media_kit's Dart side pauses the player when the 2D window goes hidden, which used to
     * leave VR staring at a frozen frame. Take the pause back whenever we own the output.
     */
    private void resumeMpv(String why) {
        long handle = VrState.mpvHandle;
        if (handle == 0) return;
        try {
            if ("yes".equalsIgnoreCase(MpvBridge.getProp(handle, "pause"))) {
                MpvBridge.setProp(handle, "pause", "no");
                Log.i(TAG, "Resumed mpv (" + why + ")");
            }
        } catch (Throwable t) {
            Log.w(TAG, "resumeMpv failed", t);
        }
    }

    /** Dart keeps re-applying its pause for a moment after the activity switch, so keep nudging. */
    private void playResumeGuard(long now) {
        if (userExplicitlyPaused || now >= resumeGuardUntil) return;
        if (now - lastResumeNudgeAt < 700) return;
        lastResumeNudgeAt = now;
        resumeMpv("resume guard");
    }

    public void seekRelative(int seconds) {
        if (isLivePlayback()) {
            showVrOsd("直播暂不支持快进/快退");
            return;
        }
        long handle = VrState.mpvHandle;
        if (handle != 0) {
            MpvBridge.command(handle, "seek " + seconds + " relative");
            positionSec = Math.max(0.0, positionSec + seconds);
            if (durationSec > 0) positionSec = Math.min(durationSec, positionSec);
            updateHud();
            showVrOsd(seconds > 0 ? ("快进 +" + seconds + "s") : ("快退 " + seconds + "s"));
            Log.i(TAG, "Seek " + seconds + "s");
        }
    }

    public void setDefaultPassthroughFromSettings(boolean passthroughDefault) {
        defaultPassthrough = passthroughDefault;
        try {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putBoolean("default_passthrough", defaultPassthrough)
                    .apply();
        } catch (Throwable ignored) {}
        if (passthrough != defaultPassthrough) {
            passthrough = defaultPassthrough;
            if (xr != null) {
                xr.setPassthrough(passthrough);
            }
            alignPanelRate();
            if (zipDepthSource != null) {
                zipDepthSource.onEnvironmentChanged();
            }
        }
        showVrOsd(defaultPassthrough ? "进入VR默认: 透视" : "进入VR默认: 全黑");
        updateHud();
        Log.i(TAG, "Default VR passthrough -> " + defaultPassthrough);
    }

    public void togglePassthrough() {
        passthrough = !passthrough;
        if (xr != null) {
            xr.setPassthrough(passthrough);
        }
        alignPanelRate();
        if (zipDepthSource != null) {
            zipDepthSource.onEnvironmentChanged();
        }
        showVrOsd(passthrough ? "透视模式: 开启" : "透视模式: 关闭");
        updateHud();
        Log.i(TAG, "Passthrough toggled: " + passthrough);
    }

    public void recenter() {
        screenPitchOffset = 0.0f;
        screenYawOffset = 0.0f;
        lastPushedYaw = 0.0f;
        if (xr != null) {
            xr.recenter();
            xr.setScreenOffset(0.0f, 0.0f);
            reassertVrQuality();
            showVrOsd("画面已居中 (Recenter)");
            Log.i(TAG, "Recenter triggered");
        }
        pushScreenParams(true);
    }

    public void exitVr() {
        if (exited) return;
        exited = true;
        isRunning = false;
        initializing = false;
        initGen++;
        if (handler != null) handler.removeCallbacks(this);
        Log.i(TAG, "Exiting VR mode, restoring mpv and returning to 2D");

        try {
            detachMpvFromVrSurface();
        } catch (Throwable t) {
            Log.w(TAG, "detachMpvFromVrSurface failed", t);
        }

        teardownXrOnly(false);

        VrState.onVrExited(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (exited) return;
        VrState.vrActive = true;
        VrState.vrStarting = false;
        VrState.vrActivity = this;
        if (wakeLock != null && !wakeLock.isHeld()) {
            try { wakeLock.acquire(); } catch (Throwable ignored) {}
        }
        reassertVrQuality();

        // Returning from background: the whole XR session was destroyed in onPause,
        // so build a brand-new one instead of resuming a stale session.
        if (xr == null && !initializing) {
            int[] sz = VrState.videoSize();
            if (sz != null) {
                videoW = sz[0];
                videoH = sz[1];
            }
            beginInit();
        } else if (xr != null) {
            attachMpvToVrSurface();
        }

        isRunning = true;
        if (handler != null) {
            handler.removeCallbacks(this);
            handler.post(this);
        }
    }

    @Override
    protected void onPause() {
        isRunning = false;
        initializing = false;
        initGen++;
        if (handler != null) handler.removeCallbacks(this);
        if (wakeLock != null && wakeLock.isHeld()) {
            try { wakeLock.release(); } catch (Throwable ignored) {}
        }

        // Hand mpv back to Flutter and drop the XR session entirely. Keeping a stopped
        // session around makes the next create() fail with "session not created", which
        // is the black-screen-after-background bug.
        if (!exited) {
            teardownXrOnly(true);
            VrState.vrActive = false;
        }
        super.onPause();
    }

    /** Points mpv back at the Flutter surface. */
    private void releaseMpvToFlutterSoft() {
        try {
            long handle = VrState.mpvHandle;
            clearPendingQualityResume(handle);
            if (handle != 0) {
                if (VrState.engineDetached) {
                    VrState.stopOrphanedOrPauseOnVrExit(handle, isPaused);
                } else {
                    long fw = VrState.flutterWid();
                    if (fw != 0) {
                        Log.i(TAG, "VR paused, mpv wid back to Flutter surface 0x" + Long.toHexString(fw));
                        MpvBridge.setProp(handle, "wid", String.valueOf(fw));
                        MpvBridge.setProp(handle, "android-surface-size", "");
                    }
                    VrState.stopOrphanedOrPauseOnVrExit(handle, isPaused);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "releaseMpvToFlutterSoft failed", t);
        }
    }

    @Override
    protected void onDestroy() {
        if (wakeLock != null && wakeLock.isHeld()) {
            try { wakeLock.release(); } catch (Throwable ignored) {}
        }
        if (!exited) {
            // Destroyed by the system while backgrounded: release everything quietly,
            // do not steal focus back to the 2D player.
            exited = true;
            isRunning = false;
            initializing = false;
            initGen++;
            if (handler != null) handler.removeCallbacks(this);
            try {
                detachMpvFromVrSurface();
            } catch (Throwable ignored) {}
            teardownXrOnly(false);
            VrState.vrActive = false;
            VrState.vrStarting = false;
            VrState.vrActivity = null;
            VrState.suppressAutoEnter(2000);
        }
        stopZipDepth();
        hudRenderer = null;
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        exitVr();
    }

    @Override
    public void run() {
        if (!isRunning) return;

        // Auto-reconnect if mpvHandle changed (e.g. next episode or video switch)
        long currentHandle = VrState.mpvHandle;
        if (currentHandle != 0 && currentHandle != currentMpvHandle && vrSurface != null && vrSurface.isValid()) {
            attachMpvToVrSurface();
        }

        if (!touchActive && xr != null && xr.getNativeHandle() != 0) {
            long now = SystemClock.uptimeMillis();
            if (now - lastAttachAttemptTime > 1000L) {
                lastAttachAttemptTime = now;
                touchActive = TouchInput.attach(xr.getNativeHandle());
                if (touchActive) {
                    Log.i(TAG, "TouchInput attached successfully on retry");
                }
            }
        }
        if (touchActive) {
            handleTouch();
        }
        boolean javaManipulating = isScreenDragging || isCtrlCornerResizing || sizeStickEngaged || distStickEngaged;
        if (javaManipulating != lastJavaManipulating) {
            lastJavaManipulating = javaManipulating;
            try { AiDepthNative.setScreenManipulating(javaManipulating); } catch (Throwable ignored) {}
        }

        long now = SystemClock.uptimeMillis();
        if (isOsdVisible && now >= osdExpireTime) {
            isOsdVisible = false;
            currentOsdText = null;
            if (xr != null) {
                xr.setOsdVisible(false);
            }
        }
        if (isResBadgeVisible && now >= resBadgeExpireTime) {
            isResBadgeVisible = false;
            if (xr != null) {
                xr.setResolutionBadgeVisible(false);
            }
        }
        if (isHudVisible && !settingsOpen && (now - lastHudActionTime > hudHideTimeoutMs)) {
            isHudVisible = false;
            hideResizeOsd();
            if (xr != null) {
                xr.setHudVisible(false);
            }
            updateHud(); // keep Java state in sync with the hidden layer
        }
        boolean idle = !interacting(now);
        if (idle) {
            maybeAdaptToVideoSize(now);
        }
        pollHudState(now, false);
        checkPendingQualityResume(now);
        brightnessWatchdog(now);
        mpvOutputWatchdog(now);
        playResumeGuard(now);
        logFrameStats(now);

        if (isRunning && handler != null) {
            handler.postDelayed(this, 11);
        }
    }

    private void applyPortraitDanmakuVOutDefault(int vw, int vh) {
        if (projectionMode != 0 || vw <= 0 || vh <= 0) return;
        int orient = (vw < vh) ? -1 : 1;
        if (orient == lastVOutOrientSign) return;
        lastVOutOrientSign = orient;
        if (orient < 0 && danmakuVOut) {
            danmakuVOut = false;
            if (danmakuEngine != null) {
                danmakuEngine.setDanmakuParams(danmakuDistIdx, danmakuSpanIdx, danmakuVOut);
            }
            applyScreenParams();
            updateHud();
        }
    }

    /** Follows resolution / aspect changes coming from the 2D player's quality menu. */
    private void maybeAdaptToVideoSize(long now) {
        if (xr == null || vrSurface == null || !vrSurface.isValid()) return;
        if (now - lastSizeProbeAt < 1000) return;
        lastSizeProbeAt = now;

        int[] sz = VrState.videoSize();
        if (sz == null || sz[0] <= 0 || sz[1] <= 0) return;

        videoW = sz[0];
        videoH = sz[1];
        applyPortraitDanmakuVOutDefault(videoW, videoH);
        renderCapLongEdge = learnedCapLongEdge();
        int[] dims = fitDims(sz[0], sz[1], renderCapLongEdge);
        if (dims[0] != renderWidth || dims[1] != renderHeight) {
            Log.i(TAG, "Video size changed " + renderWidth + "x" + renderHeight
                    + " -> " + dims[0] + "x" + dims[1]
                    + " (raw " + sz[0] + "x" + sz[1] + ", cap=" + renderCapLongEdge
                    + "), updating swapchain in-place");
            renderWidth = dims[0];
            renderHeight = dims[1];
            showResolutionBadge(renderWidth, renderHeight);
            try {
                xr.createVideoSurface(renderWidth, renderHeight);
                applyScreenParams();
            } catch (Throwable t) {
                Log.w(TAG, "in-place createVideoSurface failed", t);
            }
            if (vrTexture != null) {
                try {
                    vrTexture.setDefaultBufferSize(renderWidth, renderHeight);
                } catch (Throwable ignored) {}
            }
            long handle = VrState.mpvHandle;
            if (handle != 0) {
                try {
                    MpvBridge.setProp(handle, "android-surface-size", renderWidth + "x" + renderHeight);
                } catch (Throwable t) {
                    Log.w(TAG, "update android-surface-size failed", t);
                }
            }
        }
    }

    /**
     * media_kit Dart side re-applies mpv's "wid" to the Flutter SurfaceTexture on every
     * videoParams change (and during initial load). After that, mpv keeps rendering into
     * the invisible 2D texture: audio continues, but the VR layer shows a stale frame.
     * Detect a stalled frame flow on our texture and re-point mpv to vrSurface.
     */
    private void mpvOutputWatchdog(long now) {
        if (vrWid == 0 || vrSurface == null || !vrSurface.isValid() || xr == null) return;

        final long handle = VrState.mpvHandle;
        if (handle == 0) return;

        // Frames are flowing, nothing to do.
        if (lastFrameAt != 0 && now - lastFrameAt < 900) return;
        // Rate-limit: each re-assert re-creates mpv's VO, so don't thrash.
        if (now - lastWidAssertAt < 1600) return;

        try {
            if ("yes".equalsIgnoreCase(MpvBridge.getProp(handle, "pause"))) return;
            if ("yes".equalsIgnoreCase(MpvBridge.getProp(handle, "eof-reached"))) return;
            // Only act once the video has real parameters; otherwise we would fight the
            // player while it is still opening the file.
            String w = MpvBridge.getProp(handle, "width");
            int vw = 0;
            if (w != null) {
                try { vw = Integer.parseInt(w.trim()); } catch (Throwable ignored) {}
            }
            if (vw <= 0) return;

            lastWidAssertAt = now;
            widAssertCount++;
            int r = MpvBridge.setProp(handle, "wid", String.valueOf(vrWid));
            MpvBridge.setProp(handle, "android-surface-size", renderWidth + "x" + renderHeight);
            Log.w(TAG, "Watchdog: no new frames for " + (now - lastFrameAt) + "ms, re-asserted mpv wid=0x"
                    + Long.toHexString(vrWid) + " result=" + r + " (attempt " + widAssertCount + ")");
        } catch (Throwable t) {
            Log.w(TAG, "mpvOutputWatchdog failed", t);
        }
    }

    private void logFrameStats(long now) {
        if (now - lastStatsAt < 3000) return;
        int total = frameCount.get();
        float scale = 1000000f;
        Log.i(TAG, "VR stats: frames/3s=" + (total - lastStatsFrames) + " total=" + total
                + " asserts=" + widAssertCount
                + " lastFrameAge=" + (lastFrameAt == 0 ? -1 : (now - lastFrameAt)) + "ms"
                + " | ssp n=" + dbgSspN
                + " avg=" + fmt(dbgSspN == 0 ? 0 : dbgSspNs / dbgSspN / scale)
                + " max=" + fmt(dbgSspMaxNs / scale)
                + " | osd n=" + dbgOsdN
                + " avg=" + fmt(dbgOsdN == 0 ? 0 : dbgOsdNs / dbgOsdN / scale)
                + " max=" + fmt(dbgOsdMaxNs / scale)
                + " | poll n=" + dbgPollN
                + " avg=" + fmt(dbgPollN == 0 ? 0 : dbgPollNs / dbgPollN / scale)
                + " max=" + fmt(dbgPollMaxNs / scale)
                + " | panelRate=" + panelRate
                + " cap=" + renderCapLongEdge + " render=" + renderWidth + "x" + renderHeight);
        dbgSspN = 0; dbgSspNs = 0; dbgSspMaxNs = 0;
        dbgOsdN = 0; dbgOsdNs = 0; dbgOsdMaxNs = 0;
        dbgPollN = 0; dbgPollNs = 0; dbgPollMaxNs = 0;
        lastStatsFrames = total;
        lastStatsAt = now;
    }

    private static String fmt(float ms) {
        return String.format(Locale.ENGLISH, "%.2fms", ms);
    }

    /**
     * The 2D player owns the mpv instance and can move its colour controls while we render, which
     * is the one dimming path we can still reach. Every mpv get is synchronous against the video
     * output thread, so this reads a single property and only every five seconds.
     */
    private void brightnessWatchdog(long now) {
        if (interacting(now)) return;
        if (now - lastColorCheckAt < 8000) return;
        lastColorCheckAt = now;
        reassertVrQuality();

        final long handle = VrState.mpvHandle;
        if (handle == 0) return;
        try {
            String b = MpvBridge.getProp(handle, "brightness");
            if (b != null) {
                float v = Float.parseFloat(b.trim());
                if (Math.abs(v) > 0.01f) {
                    Log.w(TAG, "mpv brightness=" + b + " (neutral is 0), restoring 0");
                    MpvBridge.setProp(handle, "brightness", "0");
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "brightnessWatchdog failed", t);
        }
    }

    /** Re-applies the neutral VR picture settings; used where a visual reset already happens. */
    private void reassertVrQuality() {
        if (xr == null) return;
        try {
            xr.setQuality(1, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, false);
            applyWindowBrightness();
            if (wakeLock != null && !wakeLock.isHeld()) {
                wakeLock.acquire();
            }
        } catch (Throwable t) {
            Log.w(TAG, "reassertVrQuality failed", t);
        }
    }

    private void handleHandTracking() {
        if (xr == null) return;
        long now = SystemClock.uptimeMillis();

        AiDepthNative.pollHandTracking(handState);

        int toggleEv = Math.round(handState[16]);
        if (toggleEv > 0) {
            showVrOsd("手势追踪: 开启");
        } else if (toggleEv < 0) {
            isHandPinchDown[0] = false;
            isHandPinchDown[1] = false;
            prevNativeManipulating = false;
            showVrOsd("手势追踪: 暂停");
        }

        boolean nativeManipulating = handState[19] > 0.5f;
        if (nativeManipulating) {
            screenFov = clamp(handState[20], MIN_FOV, MAX_FOV);
            screenDist = clamp(handState[21], MIN_DIST, MAX_DIST);
            screenYawOffset = handState[22];
            screenPitchOffset = handState[23];
            lastInteractionAt = now;
            lastHudActionTime = now;
            hudHideTimeoutMs = 7000L;
            if (!prevNativeManipulating) {
                prevNativeManipulating = true;
                prevNativeFov = screenFov;
            } else if (Math.abs(screenFov - prevNativeFov) > 0.015f && (now - lastSizeOsdAt) > 280L) {
                lastSizeOsdAt = now;
                prevNativeFov = screenFov;
                showResizeOsd("屏幕大小: " + Math.round(Math.toDegrees(screenFov)) + "°");
            }
        } else if (prevNativeManipulating) {
            prevNativeManipulating = false;
            screenFov = clamp(handState[20], MIN_FOV, MAX_FOV);
            screenDist = clamp(handState[21], MIN_DIST, MAX_DIST);
            screenYawOffset = handState[22];
            screenPitchOffset = handState[23];
        }

        int tapHit = Math.round(handState[24]);
        if (tapHit > 0) {
            screenFov = clamp(handState[20], MIN_FOV, MAX_FOV);
            screenDist = clamp(handState[21], MIN_DIST, MAX_DIST);
            screenYawOffset = handState[22];
            screenPitchOffset = handState[23];
            lastInteractionAt = now;
            applyWindowBrightness();
            if (!isHudVisible) {
                isHudVisible = true;
                hudHideTimeoutMs = 7000L;
                lastHudActionTime = now;
                xr.setHudVisible(true);
                updateHud();
            } else if (tapHit == 1) {
                isHudVisible = false;
                settingsOpen = false;
                hideResizeOsd();
                xr.setSettingsVisible(false);
                xr.setHudVisible(false);
            }
        }

        if (handState[25] > 0.5f) {
            screenFov = clamp(handState[20], MIN_FOV, MAX_FOV);
            screenDist = clamp(handState[21], MIN_DIST, MAX_DIST);
            screenYawOffset = handState[22];
            screenPitchOffset = handState[23];
            saveScreenPrefs();
        }

        int microEv = Math.round(handState[26]);
        if (microEv > 0) {
            lastInteractionAt = now;
            applyWindowBrightness();
            if (microEv == 1) {
                cycleSynth3dStrength();
            } else if (microEv == 2) {
                togglePlayPause();
            }
        }

        boolean active0 = handState[0] > 0.5f;
        boolean active1 = handState[8] > 0.5f;
        if (!active0 && !active1) {
            isHandPinchDown[0] = false;
            isHandPinchDown[1] = false;
            return;
        }

        boolean pinch0 = active0 && (handState[1] > 0.5f);
        boolean pinch1 = active1 && (handState[9] > 0.5f);
        boolean pressed0 = (!isHandPinchDown[0] && pinch0);
        boolean pressed1 = (!isHandPinchDown[1] && pinch1);
        isHandPinchDown[0] = pinch0;
        isHandPinchDown[1] = pinch1;

        if (!nativeManipulating && (pressed1 || pressed0) && isHudVisible) {
            int ch = pressed1 ? 1 : 0;
            int cb = ch * 8;
            int hit = Math.round(handState[cb + 2]);
            if (hit == 3 && settingsOpen) {
                lastInteractionAt = now;
                lastHudActionTime = now;
                hudHideTimeoutMs = 7000L;
                applyWindowBrightness();
                if (hudRenderer.handleSettingsClick(handState[cb + 3], handState[cb + 4], this, qualityOptions)) {
                    updateSettingsPanel();
                    updateHud();
                }
            } else if (hit == 2) {
                lastInteractionAt = now;
                lastHudActionTime = now;
                hudHideTimeoutMs = 7000L;
                applyWindowBrightness();
                handleHudClick(handState[cb + 3], handState[cb + 4]);
            }
        }
    }

    private void handleTouch() {
        handleHandTracking();
        long t0 = System.nanoTime();
        boolean polled = TouchInput.poll(touchState);
        recordPoll(System.nanoTime() - t0);
        if (!polled) {
            return;
        }

        int currentButtons = (int) touchState[0];
        int pressed = (~previousButtons) & currentButtons;
        int released = previousButtons & ~currentButtons;
        previousButtons = currentButtons;

        long now = SystemClock.uptimeMillis();
        if (pressed != 0) {
            lastInteractionAt = now;
            applyWindowBrightness();
        }

        // Check laser pointer intersection (2 = HUD Quad, 1 = Cinema Screen, 0 = Miss)
        pointerHitType = Math.round(touchState[7]);
        pointerValid = (pointerHitType > 0);
        if (pointerValid) {
            pointerX = touchState[8];
            pointerY = touchState[9];
        }

        // Trigger state tracking & brightness wake
        boolean triggerCurrent = ((currentButtons & 0x60) != 0) || (touchState[5] > 0.35f) || (touchState[6] > 0.35f);
        boolean triggerPressed = (!isTriggerDown && triggerCurrent);
        isTriggerDown = triggerCurrent;
        boolean bothTriggers = ((currentButtons & 0x60) == 0x60);
        if (bothTriggers || triggerPressed) {
            reassertVrQuality();
        }

        if (triggerPressed) {
            lastInteractionAt = now;
            if (isHudVisible && pointerHitType == 4) {
                isCtrlCornerResizing = true;
                AiDepthNative.setCornerResizing(true);
                ctrlResizeAnchorYaw = touchState[12];
                ctrlResizeAnchorPitch = touchState[13];
                ctrlResizeAnchorFov = screenFov;
            } else if (!isHudVisible) {
                // Wake HUD on trigger click anywhere
                isHudVisible = true;
                hudHideTimeoutMs = 7000L;
                lastHudActionTime = now;
                if (xr != null) {
                    xr.setHudVisible(true);
                }
                updateHud();
            } else {
                lastHudActionTime = now;
                hudHideTimeoutMs = 7000L;
                if (pointerHitType == 3 && settingsOpen) {
                    boolean handled = hudRenderer.handleSettingsClick(pointerX, pointerY, this, qualityOptions);
                    if (handled) {
                        updateSettingsPanel();
                        updateHud();
                    }
                } else if (pointerHitType == 2) {
                    handleHudClick(pointerX, pointerY);
                } else if (pointerHitType == 1) {
                    isHudVisible = false;
                    settingsOpen = false;
                    hideResizeOsd();
                    if (xr != null) {
                        xr.setSettingsVisible(false);
                        xr.setHudVisible(false);
                    }
                }
            }
        }
        if (isCtrlCornerResizing) {
            if (isHudVisible && (isTriggerDown || isGripDown)) {
                lastHudActionTime = now;
                hudHideTimeoutMs = 7000L;
                float dYaw = touchState[12] - ctrlResizeAnchorYaw;
                float dPitch = touchState[13] - ctrlResizeAnchorPitch;
                float diagDelta = (-dYaw - dPitch) * 0.75f;
                screenFov = clamp(ctrlResizeAnchorFov * (float) Math.exp(diagDelta * 1.8f), MIN_FOV, MAX_FOV);
                pushScreenParams(false);
                if (now - lastSizeOsdAt > 280) {
                    lastSizeOsdAt = now;
                    showResizeOsd("屏幕大小: " + Math.round(Math.toDegrees(screenFov)) + "°");
                }
            } else {
                isCtrlCornerResizing = false;
                AiDepthNative.setCornerResizing(false);
                saveScreenPrefs();
            }
        }

        // Stick inputs:
        // - Right stick horizontal (X): seek relative (-10s / +10s) with hold-to-repeat
        // - Right stick vertical (Y): screen size (FOV)
        // - Left stick vertical (Y): viewing distance (Distance)
        float leftX = touchState[1];
        float leftY = touchState[2];
        float rightX = touchState[3];
        float rightY = touchState[4];

        long dtMs = now - lastTouchAtMs;
        if (dtMs <= 0 || dtMs > 250) dtMs = 16;
        lastTouchAtMs = now;
        float dtSec = dtMs / 1000f;

        // Grip button screen dragging (direct 6DOF grab & move)
        boolean gripCurrent = ((currentButtons & 0x600) != 0) || (touchState[10] > 0.40f) || (touchState[11] > 0.40f);
        boolean gripPressed = (!isGripDown && gripCurrent);
        boolean gripReleased = (isGripDown && !gripCurrent);
        isGripDown = gripCurrent;

        float rayYaw = touchState[12];
        float rayPitch = touchState[13];

        if (gripPressed) {
            if (isHudVisible && pointerHitType == 4) {
                isCtrlCornerResizing = true;
                AiDepthNative.setCornerResizing(true);
                ctrlResizeAnchorYaw = rayYaw;
                ctrlResizeAnchorPitch = rayPitch;
                ctrlResizeAnchorFov = screenFov;
            } else {
                isScreenDragging = true;
                anchorRayYaw = rayYaw;
                anchorRayPitch = rayPitch;
                anchorScreenYaw = screenYawOffset;
                anchorScreenPitch = screenPitchOffset;
            }
            lastHudActionTime = now;
        } else if (gripReleased) {
            isScreenDragging = false;
            lastHudActionTime = now;
        } else if (isScreenDragging && isGripDown) {
            lastHudActionTime = now;
            hudHideTimeoutMs = 7000L;
            float deltaYaw = rayYaw - anchorRayYaw;
            float deltaPitch = rayPitch - anchorRayPitch;
            screenYawOffset = anchorScreenYaw + deltaYaw;
            screenPitchOffset = Math.max(-0.65f, Math.min(1.35f, anchorScreenPitch + deltaPitch));

            // While gripping, moving thumbstick Y pushes / pulls screen distance smoothly
            float gripStickDist = Math.abs(leftY) > 0.15f ? leftY : (Math.abs(rightY) > 0.15f ? rightY : 0f);
            if (Math.abs(gripStickDist) > 0.15f) {
                float distSpeed = Math.max(0.4f, screenDist * 0.8f);
                screenDist = clamp(screenDist + (gripStickDist > 0 ? 1f : -1f) * distSpeed * (dtMs / 1000f), MIN_DIST, MAX_DIST);
            }
            pushScreenParams(false);
        }

        boolean horizDominantForSeek = Math.abs(rightX) > Math.abs(rightY) * 1.3f;
        if (horizDominantForSeek && !sizeStickEngaged && (rightX > 0.55f || rightX < -0.55f)) {
            lastHudActionTime = now;
            hudHideTimeoutMs = 7000L;
            int dir = rightX > 0 ? 1 : -1;
            if (!stickWasEngaged && (now - lastStickTime > 300)) {
                stickWasEngaged = true;
                lastStickTime = now;
                stickEngageStartTime = now;
                lastStickRepeatTime = now;
                stickRepeatCount = 0;
                seekRelative(dir * 10);
            } else if (stickWasEngaged && !isLivePlayback()) {
                long holdTime = now - stickEngageStartTime;
                if (holdTime > 350) {
                    int step = 10;
                    long interval = 220;
                    if (holdTime > 2800) {
                        step = 30;
                        interval = 140;
                    } else if (holdTime > 1400) {
                        step = 20;
                        interval = 180;
                    }
                    if (now - lastStickRepeatTime >= interval) {
                        lastStickRepeatTime = now;
                        stickRepeatCount++;
                        seekRelative(dir * step);
                    }
                }
            }
        } else if (Math.abs(rightX) < 0.25f) {
            stickWasEngaged = false;
            stickRepeatCount = 0;
        }

        // Screen pitch control via RT/LT or Grip held + stick Y
        boolean pitchModifierHeld = ((currentButtons & (0x20 | 0x40 | 0x200 | 0x400)) != 0);
        float stickPitch = (Math.abs(rightY) > Math.abs(leftY)) ? rightY : leftY;
        if (pitchModifierHeld && Math.abs(stickPitch) > 0.15f) {
            lastHudActionTime = now;
            hudHideTimeoutMs = 7000L;
            screenPitchOffset += stickPitch * dtSec * 0.9f;
            screenPitchOffset = Math.max(-0.65f, Math.min(1.35f, screenPitchOffset));
            applyScreenParams();
            // Grip + stick pitch is a continuous gesture. Suppress the
            // repeating "画面俯仰: xx°" OSD; the pitch change stays visible.
        }

        // Right stick Y: screen size, guarded against seek cooldown
        boolean seekActiveOrCooldown = stickWasEngaged || (now - lastStickTime < 450);
        float rightRatioReq = sizeStickEngaged ? 1.1f : 1.4f;
        float rightMaxHoriz = sizeStickEngaged ? 0.45f : 0.35f;
        float filteredRightY = (!seekActiveOrCooldown
                && Math.abs(rightX) < rightMaxHoriz
                && Math.abs(rightY) > Math.abs(rightX) * rightRatioReq) ? rightY : 0f;

        // Left stick Y: viewing distance (no horizontal seek conflict on left stick)
        float filteredLeftY = (Math.abs(leftY) > 0.15f) ? leftY : 0f;

        if (!pitchModifierHeld && !isScreenDragging) {
            updateScreenSizeFromStick(filteredRightY, dtSec, now);
            updateScreenDistanceFromStick(filteredLeftY, dtSec, now);
        }

        // Button A (0x1) - Play/Pause
        if ((pressed & 0x1) != 0) {
            lastHudActionTime = now;
            togglePlayPause();
        }

        // Menu (0x10) - Recenter
        if ((pressed & 0x10) != 0) {
            lastHudActionTime = now;
            recenter();
        }

        // Button B (0x2) - Exit VR (system convention)
        if ((pressed & 0x2) != 0) {
            exitVr();
        }

        // Button X (0x4) - Passthrough toggle
        if ((pressed & 0x4) != 0) {
            lastHudActionTime = now;
            togglePassthrough();
        }

        // Button Y (0x8): short press cycles AI 3D strength, long press
        // toggles the depth-map debug view. The short action waits for the
        // release so a long hold never also changes the 3D strength.
        if ((pressed & 0x8) != 0) {
            yPressAtMs = now;
            yLongFired = false;
            lastHudActionTime = now;
        }
        if (yPressAtMs > 0L && !yLongFired && (currentButtons & 0x8) != 0
                && now - yPressAtMs >= Y_LONG_PRESS_MS) {
            yLongFired = true;
            setShowDepthFromSettings(!isShowDepthActive());
        }
        if ((released & 0x8) != 0) {
            if (yPressAtMs > 0L && !yLongFired) {
                cycleSynth3dStrength();
            }
            yPressAtMs = 0L;
            yLongFired = false;
        }

        // Left Stick Click (0x800) - Cycle 3D format (SBS / OU / 2D)
        if ((pressed & 0x800) != 0) {
            lastHudActionTime = now;
            cycleSbsOuFormat();
        }

        // Right Stick Click (0x1000) - Toggle HUD visibility
        if ((pressed & 0x1000) != 0) {
            toggleHudVisibility();
        }
    }

    private void handleHudClick(float normX, float normY) {
        if (!isHudVisible || hudRenderer == null) return;
        hudRenderer.handleClick(normX, normY, this, settingsOpen, qualityOptions);
    }
}
