package com.example.piliplus.vr;

import android.content.res.AssetFileDescriptor;
import android.content.res.AssetManager;
import android.util.Log;

import org.tensorflow.lite.DataType;
import org.tensorflow.lite.Interpreter;
import org.tensorflow.lite.Tensor;
import org.tensorflow.lite.gpu.CompatibilityList;
import org.tensorflow.lite.gpu.GpuDelegate;
import org.tensorflow.lite.gpu.GpuDelegateFactory;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.util.Locale;

/**
 * ZipDepth monocular depth estimation, running beside the OpenXR frame loop.
 *
 * MoonlightXR-aligned zero-copy contract with native C++ (ToneMapper):
 *   1. Open via AssetFileDescriptor + FileChannel.map (mmap, zero heap copy)
 *   2. GpuDelegateFactory.Options enables kernel serialization into cacheDir/depth-kernels
 *   3. CPU fallback switches to zipdepth_256_int8dr.tflite with XNNPACK
 *   4. Double-buffered DirectByteBuffers shared with native C++:
 *      acquireInputSlot() waits on native condvar (no 8 ms polling, no pixel loops),
 *      interpreter.run(in[slot], out[slot]), submitOutputSlot(slot) wakes StageThread.
 */
public final class ZipDepthSource {

    private static final String TAG = "PiliVR-ZipDepth";
    private static final String MODEL_ASSET_GPU = "zipdepth_512x288_fp16.tflite";
    private static final String MODEL_ASSET_CPU = "zipdepth_256_int8dr.tflite";
    private static final String KERNEL_CACHE_DIR = "depth-kernels";

    public static final int DEFAULT_MAPS_PER_SECOND = 20;
    public static final int EXPERIMENTAL_MAPS_PER_SECOND = 30;
    private static final int MIN_ADAPTIVE_GPU_MAPS = 12;
    private static final int MIN_ADAPTIVE_CPU_MAPS = 8;
    private static final long ADAPT_EVAL_INTERVAL_NS = 2_000_000_000L;
    private static final long ADAPT_HOLD_BASE_NS = 6_000_000_000L;
    private static final long ADAPT_HOLD_MAX_NS = 24_000_000_000L;
    private static final boolean USE_GPU_DELEGATE = true;

    private AssetManager assets;
    private File cacheDir;
    private final Object stateLock = new Object();
    private final Object initLock = new Object();
    private long sessionEpoch = 0L;

    private volatile boolean running = false;
    private volatile boolean enabled = false;
    private volatile int targetMapsPerSecond = EXPERIMENTAL_MAPS_PER_SECOND;
    private volatile int effectiveMapsPerSecond = EXPERIMENTAL_MAPS_PER_SECOND;
    private volatile boolean fastEma = true;
    private Thread worker;

    private volatile boolean initStarted = false;
    private volatile boolean initInProgress = false;
    private volatile boolean modelReady = false;
    private volatile boolean modelCompiled = false;
    private volatile boolean pendingEnabled = false;
    private volatile boolean stopped = false;
    private volatile boolean highPriority = false;

    private Interpreter interpreter;
    private GpuDelegate gpuDelegate;
    private ByteBuffer modelBuffer;
    private ByteBuffer[] inputBuffers;
    private ByteBuffer[] outputBuffers;
    private DataType inputType = DataType.FLOAT32;
    private DataType outputType = DataType.FLOAT32;
    private int frameW;
    private int frameH;
    private int inputW;
    private int inputH;
    private int outputW;
    private int outputH;
    private int outputCount;
    private String activeAsset = MODEL_ASSET_GPU;

    private long lastLogAtNs = 0;
    private long inferTotalNs = 0;
    private int inferSamples = 0;
    private int mapsDone = 0;
    private boolean usingGpu = false;
    private boolean kernelCached = false;

    private static final long RATE_WINDOW_NS = 500_000_000L;
    private long rateWindowStartNs = 0L;
    private int rateWindowMaps = 0;
    private volatile float measuredMapsPerSec = 0f;

    // P2 adaptive depth rate state (driven by real XR missed refreshes + inference latency).
    private long adaptLastEvalNs = 0L;
    private long adaptHeadroomStartNs = 0L;
    private long adaptLastDropNs = 0L;
    private long adaptUpHoldNs = ADAPT_HOLD_BASE_NS;
    private int adaptDropStreak = 0;
    private int adaptInferLagStreak = 0;
    private boolean adaptPendingDropVerify = false;
    private int adaptDropOriginRate = EXPERIMENTAL_MAPS_PER_SECOND;
    private float adaptDropOriginMissedPct = 0f;
    private boolean adaptDropLocked = false;
    private long adaptInferWindowNs = 0L;
    private int adaptInferWindowSamples = 0;
    private volatile float lastWarpGpuMs = -1f;
    private volatile float lastDisplayHz = 72f;
    private volatile float lastMissedPct = -1f;

    public ZipDepthSource(AssetManager assets) {
        this(assets, null);
    }

    public ZipDepthSource(AssetManager assets, File cacheDir) {
        this.assets = assets;
        this.cacheDir = cacheDir;
    }

    private static ZipDepthSource sInstance;

    /** True when a compiled interpreter is in memory or GPU delegate kernels are cached on disk. */
    public static boolean isModelCached(File cacheDir) {
        if (isModelCached()) return true;
        if (cacheDir == null) return false;
        try {
            File dir = new File(cacheDir, KERNEL_CACHE_DIR);
            File[] files = dir.listFiles();
            if (files != null) {
                for (File f : files) {
                    if (f != null && f.isFile() && f.length() > 0L) {
                        return true;
                    }
                }
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    /** True when a compiled interpreter is already cached for this process. */
    public static synchronized boolean isModelCached() {
        return sInstance != null && sInstance.modelCompiled && sInstance.interpreter != null;
    }

    public static synchronized ZipDepthSource get(AssetManager assets) {
        return get(assets, null);
    }

    public static synchronized ZipDepthSource get(AssetManager assets, File cacheDir) {
        if (sInstance == null) {
            sInstance = new ZipDepthSource(assets, cacheDir);
        } else {
            sInstance.assets = assets;
            if (cacheDir != null) {
                sInstance.cacheDir = cacheDir;
            }
        }
        return sInstance;
    }

    public void start() {
        final long callEpoch;
        synchronized (stateLock) {
            if (initStarted) return;
            initStarted = true;
            stopped = false;
            initInProgress = true;
            callEpoch = sessionEpoch;
        }
        try {
            synchronized (initLock) {
                final boolean cancelledBeforeLoad;
                final boolean reused;
                synchronized (stateLock) {
                    cancelledBeforeLoad = stopped || callEpoch != sessionEpoch;
                    reused = modelCompiled && interpreter != null;
                }
                if (cancelledBeforeLoad) {
                    return;
                }
                boolean ok;
                if (reused) {
                    resetSessionState();
                    ok = registerSlotBuffers();
                    Log.i(TAG, "reusing cached ZipDepth interpreter (registered=" + ok + ")");
                } else {
                    ok = openModel();
                }
                if (!ok) {
                    Log.w(TAG, "model unavailable; stock 3D depth stays active");
                    synchronized (stateLock) {
                        modelReady = false;
                        initStarted = false;
                    }
                    return;
                }
                synchronized (stateLock) {
                    if (stopped || callEpoch != sessionEpoch) {
                        modelReady = false;
                        initStarted = false;
                        Log.i(TAG, "init finished after stop; compiled model cached for next VR session");
                        return;
                    }
                    modelReady = true;
                    running = true;
                    AiDepthNative.setFastEma(fastEma);
                    worker = new Thread(new Runnable() {
                        @Override
                        public void run() {
                            loop();
                        }
                    }, "ZipDepth");
                    worker.setPriority(Thread.NORM_PRIORITY);
                    worker.start();
                    if (pendingEnabled) {
                        enabled = true;
                        effectiveMapsPerSecond = targetMapsPerSecond;
                        AiDepthNative.setMode(true, effectiveMapsPerSecond);
                    }
                }
            }
        } finally {
            synchronized (stateLock) {
                initInProgress = false;
            }
        }
        Log.i(TAG, "started: asset=" + activeAsset
                + " frame=" + frameW + "x" + frameH
                + " input=" + inputW + "x" + inputH + "/" + inputType
                + " output=" + outputW + "x" + outputH + "/" + outputType
                + " gpu=" + usingGpu + " kernelCache=" + kernelCached);
    }

    public void setHighPriority(boolean on) {
        if (highPriority == on) return;
        highPriority = on;
        Log.i(TAG, "depth priority boost -> " + on);
    }

    public void setFastEma(boolean on) {
        fastEma = on;
        try {
            AiDepthNative.setFastEma(on);
        } catch (Throwable ignored) {
        }
        Log.i(TAG, "fast depth EMA -> " + on);
    }

    private void resetSessionState() {
        synchronized (stateLock) {
            rateWindowStartNs = 0L;
            rateWindowMaps = 0;
            measuredMapsPerSec = 0f;
            resetAdaptiveStateLocked();
            inferTotalNs = 0L;
            inferSamples = 0;
            lastLogAtNs = 0L;
        }
    }

    private void resetAdaptiveStateLocked() {
        adaptLastEvalNs = 0L;
        adaptHeadroomStartNs = 0L;
        adaptLastDropNs = 0L;
        adaptUpHoldNs = ADAPT_HOLD_BASE_NS;
        adaptDropStreak = 0;
        adaptInferLagStreak = 0;
        adaptPendingDropVerify = false;
        adaptDropOriginRate = targetMapsPerSecond;
        adaptDropOriginMissedPct = 0f;
        adaptDropLocked = false;
        adaptInferWindowNs = 0L;
        adaptInferWindowSamples = 0;
    }

    /**
     * Reset adaptive rate throttling when passthrough or display cadence changes
     * so transient compositor transition misses never carry over a lowered floor.
     */
    public void onEnvironmentChanged() {
        synchronized (stateLock) {
            resetAdaptiveStateLocked();
            if (enabled && effectiveMapsPerSecond != targetMapsPerSecond) {
                effectiveMapsPerSecond = targetMapsPerSecond;
                AiDepthNative.setCaptureRate(effectiveMapsPerSecond);
            }
        }
    }

    public void setEnabled(boolean on) {
        synchronized (stateLock) {
            pendingEnabled = on;
            if (!on) {
                enabled = false;
                resetRateWindow();
                resetAdaptiveStateLocked();
                AiDepthNative.setMode(false, effectiveMapsPerSecond);
                return;
            }
            if (!modelReady) {
                Log.i(TAG, "enable deferred until ZipDepth model is ready");
                return;
            }
            enabled = true;
            effectiveMapsPerSecond = targetMapsPerSecond;
            resetRateWindow();
            resetAdaptiveStateLocked();
            AiDepthNative.setFastEma(fastEma);
            AiDepthNative.setMode(true, effectiveMapsPerSecond);
        }
    }

    public boolean isEnabled() {
        return enabled;
    }

    public int getTargetMapsPerSecond() {
        return targetMapsPerSecond;
    }

    public int getEffectiveMapsPerSecond() {
        return effectiveMapsPerSecond;
    }

    public void setTargetMapsPerSecond(int maps) {
        int next = maps >= EXPERIMENTAL_MAPS_PER_SECOND
                ? EXPERIMENTAL_MAPS_PER_SECOND : DEFAULT_MAPS_PER_SECOND;
        synchronized (stateLock) {
            if (next == targetMapsPerSecond) return;
            targetMapsPerSecond = next;
            effectiveMapsPerSecond = next;
            resetRateWindow();
            resetAdaptiveStateLocked();
            AiDepthNative.setCaptureRate(next);
        }
        Log.i(TAG, "target maps/s -> " + next + " effective="
                + effectiveMapsPerSecond + " enabled=" + enabled);
    }

    public float getMeasuredMapsPerSecond() {
        return enabled ? measuredMapsPerSec : 0f;
    }

    private void noteMapCompleted(long nowNs, long inferNs) {
        adaptInferWindowNs += inferNs;
        adaptInferWindowSamples++;
        if (rateWindowStartNs == 0L) {
            rateWindowStartNs = nowNs;
            rateWindowMaps = 0;
            return;
        }
        rateWindowMaps++;
        long span = nowNs - rateWindowStartNs;
        if (span >= RATE_WINDOW_NS) {
            float instant = rateWindowMaps * 1_000_000_000f / span;
            measuredMapsPerSec = measuredMapsPerSec <= 0f
                    ? instant : measuredMapsPerSec * 0.6f + instant * 0.4f;
            rateWindowStartNs = nowNs;
            rateWindowMaps = 0;
        }
        if (adaptLastEvalNs == 0L) {
            adaptLastEvalNs = nowNs;
            return;
        }
        if (nowNs - adaptLastEvalNs >= ADAPT_EVAL_INTERVAL_NS) {
            adaptLastEvalNs = nowNs;
            evaluateAdaptiveRate(nowNs);
        }
    }

    private void evaluateAdaptiveRate(long nowNs) {
        float gpuMs;
        float hz;
        float missedPct;
        try {
            gpuMs = AiDepthNative.getWarpGpuMs();
            hz = AiDepthNative.getDisplayRefreshHz();
            missedPct = AiDepthNative.getMissedRefreshPct();
        } catch (Throwable ignored) {
            return;
        }
        if (hz < 60f || hz > 144f) hz = 72f;
        lastWarpGpuMs = gpuMs;
        lastDisplayHz = hz;
        lastMissedPct = missedPct;

        final float modelAvgMs = adaptInferWindowSamples > 0
                ? (float) adaptInferWindowNs / (float) adaptInferWindowSamples / 1_000_000f
                : 0f;
        adaptInferWindowNs = 0L;
        adaptInferWindowSamples = 0;

        if (!enabled) return;
        final int cap = targetMapsPerSecond;
        final int cur = effectiveMapsPerSecond;
        final int floor = (usingGpu || modelAvgMs <= 95f) ? MIN_ADAPTIVE_GPU_MAPS : MIN_ADAPTIVE_CPU_MAPS;
        final float budgetMs = 1000f / hz;

        // If the previous frame-drop step-down did not reduce missed refreshes
        // once we are already near the floor, lock so per-frame overhead is not throttled uselessly.
        // Never restore/lock at high rates while XR pacing is severely missed (>= 12%).
        if (adaptPendingDropVerify && missedPct >= 0f) {
            adaptPendingDropVerify = false;
            if (cur <= floor + 4
                    && missedPct >= adaptDropOriginMissedPct * 0.85f && missedPct >= 5.0f
                    && missedPct < 12.0f
                    && adaptDropOriginRate > cur && adaptDropOriginRate <= cap) {
                synchronized (stateLock) {
                    if (!enabled || targetMapsPerSecond != cap) return;
                    effectiveMapsPerSecond = adaptDropOriginRate;
                    AiDepthNative.setCaptureRate(adaptDropOriginRate);
                }
                adaptDropLocked = true;
                adaptHeadroomStartNs = 0L;
                Log.i(TAG, String.format(Locale.ENGLISH,
                        "depth adaptive restore: %d -> %d (missed=%.1f%% unchanged vs %.1f%%, locking floor)",
                        cur, adaptDropOriginRate, missedPct, adaptDropOriginMissedPct));
                return;
            }
        }

        // Real XR missed-refresh pressure: missedPct reflects actual xrEndFrame / VSync misses,
        // including OpenCL TFLite GPU delegate & compositor contention not shown in GL warpGpuMs.
        final boolean realFrameDrop = (missedPct >= 10.0f)
                || (missedPct >= 5.0f && (gpuMs < 0f || gpuMs > budgetMs * 0.72f));
        // With async GPU OpenCL inference and a latest-frame-wins 2-slot mailbox,
        // lowering capture below ~2x GPU latency starves slots unnecessarily; only
        // trigger inferSaturated for severe stalls or CPU fallback.
        final float inferThreshMs = usingGpu
                ? Math.max(110f, (1000f / (float) cur) * 2.5f)
                : (1000f / (float) cur) * 1.45f;
        final boolean inferSaturated = (modelAvgMs > 0f)
                && (cur > floor)
                && (modelAvgMs > inferThreshMs);

        int next = cur;
        String reason = null;
        if (!adaptDropLocked && realFrameDrop && cur > floor) {
            adaptInferLagStreak = 0;
            adaptHeadroomStartNs = 0L;
            adaptDropStreak++;
            if (adaptDropStreak >= 2 || missedPct >= 12.0f) {
                adaptDropStreak = 0;
                final int step = (missedPct >= 25.0f) ? 6 : ((missedPct >= 12.0f) ? 4 : 2);
                next = Math.max(floor, cur - step);
                if (next < cur) {
                    adaptDropOriginRate = cur;
                    adaptDropOriginMissedPct = missedPct;
                    adaptPendingDropVerify = true;
                    reason = "missed";
                }
            }
        } else if (!adaptDropLocked && inferSaturated) {
            adaptDropStreak = 0;
            adaptInferLagStreak++;
            adaptHeadroomStartNs = 0L;
            if (adaptInferLagStreak >= 2) {
                adaptInferLagStreak = 0;
                final int modelCap = Math.max(floor, Math.round(1000f / (modelAvgMs * 1.15f)));
                next = Math.max(modelCap, cur - 4);
                if (next < cur) {
                    reason = "inferLag";
                } else {
                    next = cur;
                }
            }
        } else {
            adaptDropStreak = 0;
            adaptInferLagStreak = 0;
            final boolean pacingClean = (missedPct < 0f || missedPct < 3.0f)
                    && (gpuMs < 0f || gpuMs < budgetMs * 0.90f);
            final int probeUp = Math.min(cap, cur + 2);
            final boolean inferHeadroom = usingGpu || (modelAvgMs <= 0f)
                    || (modelAvgMs < (1000f / (float) Math.max(1, probeUp)) * 1.20f);
            if (pacingClean && inferHeadroom) {
                if (adaptDropLocked && adaptHeadroomStartNs == 0L) {
                    adaptHeadroomStartNs = nowNs;
                } else if (adaptDropLocked && (nowNs - adaptHeadroomStartNs >= 10_000_000_000L)) {
                    adaptDropLocked = false;
                }
                if (cur < cap) {
                    if (adaptHeadroomStartNs == 0L) {
                        adaptHeadroomStartNs = nowNs;
                    } else if (nowNs - adaptHeadroomStartNs >= adaptUpHoldNs) {
                        next = probeUp;
                        adaptHeadroomStartNs = nowNs;
                        reason = "recover";
                    }
                } else if (adaptLastDropNs > 0L && nowNs - adaptLastDropNs > adaptUpHoldNs * 2L) {
                    adaptUpHoldNs = ADAPT_HOLD_BASE_NS;
                }
            } else {
                adaptHeadroomStartNs = 0L;
            }
        }

        if (next != cur) {
            synchronized (stateLock) {
                if (!enabled || targetMapsPerSecond != cap) return;
                effectiveMapsPerSecond = next;
                AiDepthNative.setCaptureRate(next);
            }
            if (next < cur) {
                if (adaptLastDropNs > 0L && (nowNs - adaptLastDropNs) < ADAPT_HOLD_MAX_NS) {
                    adaptUpHoldNs = Math.min(ADAPT_HOLD_MAX_NS, adaptUpHoldNs * 2L);
                }
                adaptLastDropNs = nowNs;
            }
            Log.i(TAG, String.format(Locale.ENGLISH,
                    "depth adaptive rate (%s): %d -> %d (cap=%d missed=%.1f%% gpu=%.2f/%.2fms hz=%.0f infer=%.1fms hold=%ds)",
                    reason, cur, next, cap, missedPct, gpuMs, budgetMs, hz, modelAvgMs,
                    (int) (adaptUpHoldNs / 1_000_000_000L)));
        }
    }

    private void resetRateWindow() {
        rateWindowStartNs = 0L;
        rateWindowMaps = 0;
        measuredMapsPerSec = 0f;
        adaptInferWindowNs = 0L;
        adaptInferWindowSamples = 0;
    }

    public void stop() {
        Thread toJoin = null;
        synchronized (stateLock) {
            sessionEpoch++;
            stopped = true;
            running = false;
            enabled = false;
            pendingEnabled = false;
            modelReady = false;
            initStarted = false;
            toJoin = worker;
            worker = null;
            AiDepthNative.setMode(false, targetMapsPerSecond);
        }
        if (toJoin != null) {
            toJoin.interrupt();
            try {
                toJoin.join(1200);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        synchronized (stateLock) {
            final boolean loading = initInProgress;
            final boolean safe = !loading && (toJoin == null || !toJoin.isAlive());
            if (loading) {
                Log.i(TAG, "model still loading at teardown; deferred cleanup");
            } else if (safe) {
                Log.i(TAG, "compiled model kept for the next VR session");
            } else {
                Log.w(TAG, "depth thread still alive; dropping cached model");
                closeModel();
            }
        }
    }

    private static int mapDataType(DataType dt) {
        if (dt == DataType.FLOAT32) return AiDepthNative.DTYPE_FLOAT32;
        if (dt == DataType.UINT8) return AiDepthNative.DTYPE_UINT8;
        if (dt == DataType.INT8) return AiDepthNative.DTYPE_INT8;
        throw new IllegalStateException("unsupported tensor dtype: " + dt);
    }

    private boolean registerSlotBuffers() {
        if (inputBuffers == null || outputBuffers == null
                || inputBuffers.length < 2 || outputBuffers.length < 2
                || inputBuffers[0] == null || inputBuffers[1] == null
                || outputBuffers[0] == null || outputBuffers[1] == null) {
            return false;
        }
        return AiDepthNative.registerBuffers(
                inputBuffers[0], outputBuffers[0],
                inputBuffers[1], outputBuffers[1],
                inputW, inputH, mapDataType(inputType),
                outputW, outputH, mapDataType(outputType));
    }

    private boolean openModel() {
        try {
            activeAsset = MODEL_ASSET_GPU;
            modelBuffer = loadModel(MODEL_ASSET_GPU);
            Interpreter.Options options = new Interpreter.Options();
            options.setNumThreads(2);
            kernelCached = false;

            if (USE_GPU_DELEGATE) {
                try {
                    CompatibilityList compat = new CompatibilityList();
                    Log.i(TAG, "GPU allowlist says "
                            + compat.isDelegateSupportedOnThisDevice()
                            + ", trying the delegate anyway");
                } catch (Throwable ignored) {
                }
                try {
                    GpuDelegateFactory.Options gpu = new GpuDelegateFactory.Options();
                    gpu.setPrecisionLossAllowed(true);
                    gpu.setInferencePreference(
                            GpuDelegateFactory.Options.INFERENCE_PREFERENCE_SUSTAINED_SPEED);
                    if (cacheDir != null) {
                        File kdir = new File(cacheDir, KERNEL_CACHE_DIR);
                        if (kdir.isDirectory() || kdir.mkdirs()) {
                            String token = MODEL_ASSET_GPU + "-" + modelBuffer.capacity();
                            gpu.setSerializationParams(kdir.getAbsolutePath(), token);
                            kernelCached = true;
                        }
                    }
                    gpuDelegate = new GpuDelegate(gpu);
                    options.addDelegate(gpuDelegate);
                    usingGpu = true;
                } catch (Throwable t) {
                    Log.w(TAG, "GPU delegate creation failed, using CPU", t);
                    if (gpuDelegate != null) {
                        try { gpuDelegate.close(); } catch (Throwable ignored) {}
                    }
                    gpuDelegate = null;
                    usingGpu = false;
                    kernelCached = false;
                }
            }

            if (usingGpu) {
                try {
                    interpreter = new Interpreter(modelBuffer, options);
                } catch (Throwable gpuFailure) {
                    Log.w(TAG, "GPU interpreter failed, switching to CPU route", gpuFailure);
                    if (gpuDelegate != null) {
                        try { gpuDelegate.close(); } catch (Throwable ignored) {}
                    }
                    gpuDelegate = null;
                    usingGpu = false;
                    kernelCached = false;
                }
            }

            if (interpreter == null) {
                // MoonlightXR CPU fallback route: use smaller 256 int8dr model with XNNPACK.
                try {
                    modelBuffer = loadModel(MODEL_ASSET_CPU);
                    activeAsset = MODEL_ASSET_CPU;
                } catch (IOException cpuAssetErr) {
                    Log.w(TAG, "CPU model " + MODEL_ASSET_CPU + " unavailable, keeping "
                            + MODEL_ASSET_GPU, cpuAssetErr);
                    activeAsset = MODEL_ASSET_GPU;
                }
                Interpreter.Options cpu = new Interpreter.Options();
                cpu.setNumThreads(2);
                cpu.setUseXNNPACK(true);
                interpreter = new Interpreter(modelBuffer, cpu);
            }

            Tensor in = interpreter.getInputTensor(0);
            int[] inShape = in.shape();
            inputH = inShape[1];
            inputW = inShape[2];
            inputType = in.dataType();
            Tensor out = interpreter.getOutputTensor(0);
            int[] outShape = out.shape();
            outputH = outShape[1];
            outputW = outShape[2];
            outputType = out.dataType();
            outputCount = out.numElements();
            if (inputW < 8 || inputH < 8 || outputW < 8 || outputH < 8
                    || outputCount < outputW * outputH) {
                throw new IllegalStateException("unexpected model shapes");
            }

            inputBuffers = new ByteBuffer[]{
                    ByteBuffer.allocateDirect(in.numBytes()).order(ByteOrder.nativeOrder()),
                    ByteBuffer.allocateDirect(in.numBytes()).order(ByteOrder.nativeOrder()),
            };
            outputBuffers = new ByteBuffer[]{
                    ByteBuffer.allocateDirect(out.numBytes()).order(ByteOrder.nativeOrder()),
                    ByteBuffer.allocateDirect(out.numBytes()).order(ByteOrder.nativeOrder()),
            };

            frameW = AiDepthNative.frameWidth();
            frameH = AiDepthNative.frameHeight();
            if (frameW < 8 || frameH < 8) {
                frameW = inputW;
                frameH = inputH;
            }
            if (!registerSlotBuffers()) {
                throw new IllegalStateException("native registerBuffers failed");
            }
            modelCompiled = true;
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "failed to open ZipDepth model", t);
            closeModel();
            return false;
        }
    }

    private ByteBuffer loadModel(String assetName) throws IOException {
        try {
            AssetFileDescriptor afd = assets.openFd(assetName);
            try (FileInputStream fis = new FileInputStream(afd.getFileDescriptor());
                 FileChannel ch = fis.getChannel()) {
                return ch.map(FileChannel.MapMode.READ_ONLY,
                        afd.getStartOffset(), afd.getDeclaredLength());
            } finally {
                afd.close();
            }
        } catch (IOException mmapFallback) {
            Log.w(TAG, "mmap unavailable for " + assetName + ", streaming into DirectByteBuffer", mmapFallback);
            InputStream in = assets.open(assetName);
            try {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] chunk = new byte[65536];
                int n;
                while ((n = in.read(chunk)) > 0) {
                    out.write(chunk, 0, n);
                }
                byte[] bytes = out.toByteArray();
                ByteBuffer bb = ByteBuffer.allocateDirect(bytes.length).order(ByteOrder.nativeOrder());
                bb.put(bytes);
                bb.rewind();
                return bb;
            } finally {
                in.close();
            }
        }
    }

    private void closeModel() {
        modelCompiled = false;
        try {
            AiDepthNative.unregisterBuffers();
        } catch (Throwable ignored) {
        }
        if (interpreter != null) {
            try {
                interpreter.close();
            } catch (Throwable ignored) {
            }
        }
        interpreter = null;
        if (gpuDelegate != null) {
            try {
                gpuDelegate.close();
            } catch (Throwable ignored) {
            }
        }
        gpuDelegate = null;
        modelBuffer = null;
        inputBuffers = null;
        outputBuffers = null;
    }

    private void loop() {
        boolean lastBoost = false;
        while (running) {
            final boolean boost = highPriority;
            if (boost != lastBoost) {
                android.os.Process.setThreadPriority(boost
                        ? android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY
                        : android.os.Process.THREAD_PRIORITY_DEFAULT);
                lastBoost = boost;
                Log.i(TAG, "worker nice=" + (boost ? "URGENT_DISPLAY" : "DEFAULT"));
            }
            if (!enabled || interpreter == null || inputBuffers == null || outputBuffers == null) {
                sleepMs(25);
                continue;
            }
            final int slot = AiDepthNative.acquireInputSlot(50);
            if (slot < 0 || slot > 1) {
                continue;
            }
            boolean submitted = false;
            try {
                final ByteBuffer inBuf = inputBuffers[slot];
                final ByteBuffer outBuf = outputBuffers[slot];
                inBuf.rewind();
                outBuf.clear();
                final long t0 = System.nanoTime();
                interpreter.run(inBuf, outBuf);
                final long t1 = System.nanoTime();
                AiDepthNative.submitOutputSlot(slot);
                submitted = true;

                final long inferNs = t1 - t0;
                inferTotalNs += inferNs;
                inferSamples++;
                mapsDone++;
                noteMapCompleted(t1, inferNs);
                maybeLog();
            } catch (Throwable t) {
                Log.e(TAG, "inference failed on slot " + slot, t);
                sleepMs(50);
            } finally {
                if (!submitted) {
                    AiDepthNative.releaseInputSlot(slot);
                }
            }
        }
    }

    private void maybeLog() {
        final long now = System.nanoTime();
        if (now - lastLogAtNs < 2_000_000_000L) {
            return;
        }
        lastLogAtNs = now;
        int n = inferSamples > 0 ? inferSamples : 1;
        long modelMs = inferTotalNs / n / 1_000_000L;
        Log.i(TAG, "maps=" + mapsDone
                + " modelMs=" + modelMs
                + " measuredMapsPerSec=" + String.format(Locale.ENGLISH, "%.1f",
                        measuredMapsPerSec)
                + " target=" + targetMapsPerSecond
                + " effective=" + effectiveMapsPerSecond
                + " warpGpuMs=" + String.format(Locale.ENGLISH, "%.2f", lastWarpGpuMs)
                + " missedPct=" + String.format(Locale.ENGLISH, "%.1f", lastMissedPct)
                + " hz=" + Math.round(lastDisplayHz)
                + " gpu=" + usingGpu + " kernelCache=" + kernelCached
                + " asset=" + activeAsset + " enabled=" + enabled);
        inferTotalNs = 0L;
        inferSamples = 0;
    }

    private static void sleepMs(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
