package com.example.piliplus.vr;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.BatteryManager;
import android.text.TextPaint;
import android.text.TextUtils;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Renders the 1024x640 VR HUD overlay and 1024x440 expandable Settings popup
 * using Apple-style G2 continuous-curvature squircles, lightweight flat translucent
 * glass fills, and grid-aligned keycaps.
 */
public final class VrHudRenderer {
    public static final int HUD_W = 1024;
    public static final int HUD_H = 640;
    public static final int SETTINGS_W = 1024;
    public static final int SETTINGS_H = 440;

    public static final class QualityOption {
        public final int code;
        public final String shortDesc;
        public final String fullDesc;
        public final int width;
        public final int height;
        public final String edlUri;

        public QualityOption(int code, String shortDesc, String fullDesc, int width, int height, String edlUri) {
            this.code = code;
            this.shortDesc = shortDesc;
            this.fullDesc = fullDesc;
            this.width = width;
            this.height = height;
            this.edlUri = edlUri;
        }
    }

    private final Context context;
    private Bitmap hudBitmap;
    private Canvas hudCanvas;
    private Bitmap settingsBitmap;
    private Canvas settingsCanvas;

    private final Paint cardBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint cardBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dividerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint keyBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint keyBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint keyActiveBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint heroKeyBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint iconStrokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint iconFillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final TextPaint textPaint = new TextPaint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
    private final Paint barBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint barFillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint optBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint optBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint optActivePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint segTrackBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final SimpleDateFormat clockFormat = new SimpleDateFormat("HH:mm", Locale.ENGLISH);

    private final Path scratchPath = new Path();

    // Live ZipDepth throughput for the top-left 3D badge
    public boolean depthBadgeActive;
    public boolean depthEnabled;
    public float depthMapsPerSec;
    public int depthEffectiveMapsPerSec = 30;
    public int depthTargetMapsPerSec = 30;

    // Symmetrical visionOS Media Ornament grid on 1024x640 HUD canvas
    private static final float MAIN_CARD_L = 112f;
    private static final float MAIN_CARD_T = 412f;
    private static final float MAIN_CARD_R = 912f;
    private static final float MAIN_CARD_B = 588f;
    private static final float INNER_LEFT = 138f;
    private static final float INNER_RIGHT = 886f;

    private final RectF[] topSquircleRects = new RectF[4];
    private final RectF btnMuteRect = new RectF(138f, 480f, 186f, 524f);
    private final RectF btnSubRect = new RectF(196f, 480f, 244f, 524f);
    private final RectF btnPrevRect = new RectF(410f, 479f, 458f, 525f);
    private final RectF btnPlayRect = new RectF(474f, 472f, 550f, 532f);
    private final RectF btnNextRect = new RectF(566f, 479f, 614f, 525f);
    private final RectF btnSettingsRect = new RectF(712f, 480f, 760f, 524f);
    private final RectF btnStereoRect = new RectF(770f, 480f, 886f, 524f);
    private final RectF seekBarHitRect = new RectF(214f, 536f, 810f, 582f);
    private static final float SEEK_X0 = 222f;
    private static final float SEEK_X1 = 802f;
    private volatile boolean liveMode = false;

    // Settings card geometry: outer [16..1008] x [12..428], inner rails [40..984]
    private static final float SET_INNER_L = 40f;
    private static final float SET_INNER_R = 984f;
    private static final float ROW_X0 = 180f;
    private static final float ROW_X1 = 984f;
    private static final float ROW_GAP = 10f;

    private static final float TAB0_Y0 = 108f;
    private static final float TAB0_STEP = 68f;
    private static final float TAB1_Y0 = 104f;
    private static final float TAB1_STEP = 56f;
    private static final float TAB2_Y0 = 132f;
    private static final float TAB2_STEP = 88f;
    private static final float TAB3_Y0 = 100f;
    private static final float TAB3_STEP = 48f;

    public void setLiveMode(boolean live) {
        this.liveMode = live;
    }

    public VrHudRenderer(Context context) {
        this.context = context.getApplicationContext();
        this.hudBitmap = Bitmap.createBitmap(HUD_W, HUD_H, Bitmap.Config.ARGB_8888);
        this.hudCanvas = new Canvas(this.hudBitmap);

        Typeface boldSans = Typeface.create("sans-serif-medium", Typeface.BOLD);

        cardBgPaint.setStyle(Paint.Style.FILL);
        cardBgPaint.setColor(0xE8171B24);

        cardBorderPaint.setStyle(Paint.Style.STROKE);
        cardBorderPaint.setStrokeWidth(1.5f);
        cardBorderPaint.setColor(0x36FFFFFF);

        dividerPaint.setStyle(Paint.Style.STROKE);
        dividerPaint.setStrokeWidth(1.0f);
        dividerPaint.setColor(0x1CFFFFFF);

        keyBgPaint.setStyle(Paint.Style.FILL);
        keyBgPaint.setColor(0xD4232836);

        keyBorderPaint.setStyle(Paint.Style.STROKE);
        keyBorderPaint.setStrokeWidth(1.3f);
        keyBorderPaint.setColor(0x28FFFFFF);

        keyActiveBorderPaint.setStyle(Paint.Style.STROKE);
        keyActiveBorderPaint.setStrokeWidth(1.8f);
        keyActiveBorderPaint.setColor(0xFFFB7299);

        heroKeyBgPaint.setStyle(Paint.Style.FILL);
        heroKeyBgPaint.setColor(0xE631384A);

        iconStrokePaint.setStyle(Paint.Style.STROKE);
        iconStrokePaint.setStrokeWidth(2.4f);
        iconStrokePaint.setStrokeCap(Paint.Cap.ROUND);
        iconStrokePaint.setStrokeJoin(Paint.Join.ROUND);
        iconStrokePaint.setColor(Color.WHITE);

        iconFillPaint.setStyle(Paint.Style.FILL);
        iconFillPaint.setColor(Color.WHITE);

        textPaint.setTypeface(boldSans);
        textPaint.setColor(Color.WHITE);

        barBgPaint.setStyle(Paint.Style.FILL);
        barBgPaint.setColor(0x36FFFFFF);

        barFillPaint.setStyle(Paint.Style.FILL);
        barFillPaint.setColor(0xFFFB7299);

        optBgPaint.setStyle(Paint.Style.FILL);
        optBgPaint.setColor(0xCE232836);

        optBorderPaint.setStyle(Paint.Style.STROKE);
        optBorderPaint.setStrokeWidth(1.1f);
        optBorderPaint.setColor(0x20FFFFFF);

        optActivePaint.setStyle(Paint.Style.FILL);
        optActivePaint.setColor(0xFFFB7299);

        segTrackBgPaint.setStyle(Paint.Style.FILL);
        segTrackBgPaint.setColor(0x780B0D14);

        // Top 4 floating visionOS glass action squircles: 60x60, centered on x = 512
        float[] cx = {342f, 454f, 566f, 678f};
        float cy = 368f;
        float half = 30f;
        for (int i = 0; i < 4; i++) {
            topSquircleRects[i] = new RectF(cx[i] - half, cy - half, cx[i] + half, cy + half);
        }
    }

    /**
     * Builds an Apple G2 continuous-corner squircle path (iOS / visionOS
     * continuousCornerRadius curvature ease-in without tangent curvature jumps).
     */
    public static Path addSquirclePath(Path dst, RectF rect, float radius) {
        dst.reset();
        float l = rect.left;
        float t = rect.top;
        float r = rect.right;
        float b = rect.bottom;
        float w = r - l;
        float h = b - t;
        if (w <= 0f || h <= 0f) return dst;
        if (radius <= 0f) {
            dst.addRect(rect, Path.Direction.CW);
            return dst;
        }
        float p = Math.min(radius * 1.52f, Math.min(w, h) * 0.5f);
        float c1 = 0.64f * p;
        float d = 0.195f * p;
        float c2 = 2f * d;

        dst.moveTo(l + p, t);
        if (r - p > l + p) {
            dst.lineTo(r - p, t);
        }
        dst.cubicTo(r - c1, t, r - c2, t, r - d, t + d);
        dst.cubicTo(r, t + c2, r, t + c1, r, t + p);

        if (b - p > t + p) {
            dst.lineTo(r, b - p);
        }
        dst.cubicTo(r, b - c1, r, b - c2, r - d, b - d);
        dst.cubicTo(r - c2, b, r - c1, b, r - p, b);

        if (r - p > l + p) {
            dst.lineTo(l + p, b);
        }
        dst.cubicTo(l + c1, b, l + c2, b, l + d, b - d);
        dst.cubicTo(l, b - c2, l, b - c1, l, b - p);

        if (b - p > t + p) {
            dst.lineTo(l, t + p);
        }
        dst.cubicTo(l, t + c1, l, t + c2, l + d, t + d);
        dst.cubicTo(l + c2, t, l + c1, t, l + p, t);
        dst.close();
        return dst;
    }

    private void drawSquircleCard(Canvas c, RectF bounds, float radius) {
        addSquirclePath(scratchPath, bounds, radius);
        c.drawPath(scratchPath, cardBgPaint);
        c.drawPath(scratchPath, cardBorderPaint);
    }

    private void drawKeycapSquircle(Canvas c, RectF bounds, float radius, boolean active, int accentBorderColor, int accentBgColor) {
        addSquirclePath(scratchPath, bounds, radius);
        if (active) {
            iconFillPaint.setColor(accentBgColor);
            c.drawPath(scratchPath, iconFillPaint);
            keyActiveBorderPaint.setColor(accentBorderColor);
            keyActiveBorderPaint.setStrokeWidth(1.6f);
            c.drawPath(scratchPath, keyActiveBorderPaint);
        } else {
            c.drawPath(scratchPath, keyBgPaint);
            c.drawPath(scratchPath, keyBorderPaint);
        }
    }

    public Bitmap render(
            String videoTitle,
            boolean isPaused,
            boolean isMuted,
            boolean danmakuEnabled,
            boolean subtitleEnabled,
            boolean passthrough,
            int stereoMode,
            int synth3dLevel,
            double positionSec,
            double durationSec,
            boolean settingsOpen,
            int settingsTab,
            List<QualityOption> qualities,
            int currentQualityCode,
            boolean limit4k,
            int screenCurveIdx,
            int screenPitchIdx,
            int danmakuDistIdx,
            int danmakuSpanIdx,
            boolean danmakuVOut,
            int danmakuDensityIdx,
            boolean fixedDanmakuEnabled,
            int danmakuSizeIdx,
            int subtitleSizeIdx,
            int subProtIdx) {

        if (hudBitmap == null || hudBitmap.isRecycled()) {
            hudBitmap = Bitmap.createBitmap(HUD_W, HUD_H, Bitmap.Config.ARGB_8888);
            hudCanvas = new Canvas(hudBitmap);
        }
        Canvas c = hudCanvas;
        c.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);

        // 1. Top 4 Floating Apple Squircle Buttons
        drawTopSquircles(c, danmakuEnabled, passthrough);

        // 2. Main Control Card (visionOS concentric glass ornament)
        RectF mainCard = new RectF(MAIN_CARD_L, MAIN_CARD_T, MAIN_CARD_R, MAIN_CARD_B);
        drawSquircleCard(c, mainCard, 30f);

        // 2a. Header Row (y = 442): 3D throughput squircle badge + aligned video title
        float titleStartX = drawDepthBadge(c);
        String title = (videoTitle != null && !videoTitle.trim().isEmpty()) ? videoTitle.trim() : "PiliPlus VR Cinema";
        textPaint.setTextSize(18.5f);
        textPaint.setColor(0xFFE5E8F0);
        textPaint.setTextAlign(Paint.Align.LEFT);
        float titleMaxW = Math.max(40f, INNER_RIGHT - titleStartX);
        CharSequence ellipsized = TextUtils.ellipsize(title, textPaint, titleMaxW, TextUtils.TruncateAt.END);
        Paint.FontMetrics titleFm = textPaint.getFontMetrics();
        c.drawText(ellipsized, 0, ellipsized.length(), titleStartX, 442f - (titleFm.ascent + titleFm.descent) * 0.5f, textPaint);

        c.drawLine(INNER_LEFT, 468f, INNER_RIGHT, 468f, dividerPaint);

        // 2b. Aligned Keycap Buttons Row (cy = 502)
        drawKeycapSquircle(c, btnMuteRect, 14f, isMuted, 0xFFEF4444, 0x30EF4444);
        drawSpeakerIcon(c, btnMuteRect.centerX(), btnMuteRect.centerY(), isMuted);

        drawKeycapSquircle(c, btnSubRect, 14f, subtitleEnabled, 0xCCFB7299, 0x26FB7299);
        drawSubtitleIcon(c, btnSubRect.centerX(), btnSubRect.centerY(), subtitleEnabled);

        drawKeycapSquircle(c, btnPrevRect, 15f, false, 0, 0);
        drawSkipIcon(c, btnPrevRect.centerX(), btnPrevRect.centerY(), false);

        addSquirclePath(scratchPath, btnPlayRect, 22f);
        c.drawPath(scratchPath, heroKeyBgPaint);
        keyActiveBorderPaint.setColor(0x44FFFFFF);
        keyActiveBorderPaint.setStrokeWidth(1.4f);
        c.drawPath(scratchPath, keyActiveBorderPaint);
        drawPlayPauseIcon(c, btnPlayRect.centerX(), btnPlayRect.centerY(), isPaused);

        drawKeycapSquircle(c, btnNextRect, 15f, false, 0, 0);
        drawSkipIcon(c, btnNextRect.centerX(), btnNextRect.centerY(), true);

        drawKeycapSquircle(c, btnSettingsRect, 14f, settingsOpen, 0xCCFB7299, 0x26FB7299);
        drawGearIcon(c, btnSettingsRect.centerX(), btnSettingsRect.centerY(), settingsOpen);

        drawKeycapSquircle(c, btnStereoRect, 14f, stereoMode != 0, 0xCCFB7299, 0x26FB7299);
        drawCubeButtonContent(c, btnStereoRect, stereoMode, synth3dLevel);

        // 2c. Seekbar & Flush-Aligned Timestamps (cy = 558)
        if (liveMode) {
            textPaint.setTextAlign(Paint.Align.CENTER);
            textPaint.setTextSize(17.5f);
            textPaint.setColor(0xFFE5E8F0);
            Paint.FontMetrics lfm = textPaint.getFontMetrics();
            c.drawText("正在直播", 512f, 558f - (lfm.ascent + lfm.descent) * 0.5f, textPaint);
        } else {
            float progress = 0f;
            if (durationSec > 0.5) {
                progress = (float) Math.max(0.0, Math.min(1.0, positionSec / durationSec));
            }
            textPaint.setTextSize(15.5f);
            textPaint.setColor(0xFFBDC3D0);
            Paint.FontMetrics tfm = textPaint.getFontMetrics();
            float tBase = 558f - (tfm.ascent + tfm.descent) * 0.5f;

            textPaint.setTextAlign(Paint.Align.LEFT);
            c.drawText(formatTime(positionSec), INNER_LEFT, tBase, textPaint);

            textPaint.setTextAlign(Paint.Align.RIGHT);
            c.drawText(formatTime(durationSec), INNER_RIGHT, tBase, textPaint);

            RectF trackBg = new RectF(SEEK_X0, 555f, SEEK_X1, 561f);
            c.drawRoundRect(trackBg, 3f, 3f, barBgPaint);

            float thumbX = SEEK_X0 + (SEEK_X1 - SEEK_X0) * progress;
            if (thumbX > SEEK_X0 + 1f) {
                RectF trackFill = new RectF(SEEK_X0, 555f, thumbX, 561f);
                c.drawRoundRect(trackFill, 3f, 3f, barFillPaint);
            }

            iconFillPaint.setColor(Color.WHITE);
            c.drawCircle(thumbX, 558f, 7.2f, iconFillPaint);
        }

        // 3. Bottom Battery + Clock Squircle Pill
        drawBottomStatusPill(c);

        return hudBitmap;
    }

    private float drawDepthBadge(Canvas c) {
        if (!depthBadgeActive) return INNER_LEFT;
        String label;
        int accent;
        if (!depthEnabled) {
            label = "原版深度 · AI 关";
            accent = 0xFF949CA9;
        } else {
            label = String.format(Locale.ENGLISH, "AI 3D · %.1f/%d", depthMapsPerSec, depthEffectiveMapsPerSec);
            accent = 0xFFFB7299;
        }

        textPaint.setTextSize(14.5f);
        textPaint.setTextAlign(Paint.Align.LEFT);
        float textW = textPaint.measureText(label);
        final float left = INNER_LEFT;
        final float top = 428f;
        final float height = 28f;
        final float cy = top + height * 0.5f;
        final float textX = left + 28f;
        final float padRight = 14f;
        float pillW = (textX - left) + textW + padRight;
        RectF pill = new RectF(left, top, left + pillW, top + height);

        addSquirclePath(scratchPath, pill, 9f);
        iconFillPaint.setColor(depthEnabled ? 0x26FB7299 : 0x452A303E);
        c.drawPath(scratchPath, iconFillPaint);
        keyActiveBorderPaint.setColor(depthEnabled ? 0xAAFB7299 : 0x32FFFFFF);
        keyActiveBorderPaint.setStrokeWidth(1.1f);
        c.drawPath(scratchPath, keyActiveBorderPaint);

        iconFillPaint.setColor(accent);
        c.drawCircle(left + 14f, cy, 4.5f, iconFillPaint);
        textPaint.setColor(accent);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        c.drawText(label, textX, cy - (fm.ascent + fm.descent) * 0.5f, textPaint);

        return left + pillW + 12f;
    }

    private void drawTopSquircles(Canvas c, boolean danmakuEnabled, boolean passthrough) {
        for (int i = 0; i < 4; i++) {
            RectF r = topSquircleRects[i];
            boolean active = (i == 1 && danmakuEnabled) || (i == 3 && passthrough);

            addSquirclePath(scratchPath, r, 18f);
            if (active) {
                iconFillPaint.setColor(0xEC2E1924);
                c.drawPath(scratchPath, iconFillPaint);
                keyActiveBorderPaint.setColor(0xFFFB7299);
                keyActiveBorderPaint.setStrokeWidth(2.0f);
                c.drawPath(scratchPath, keyActiveBorderPaint);
            } else {
                c.drawPath(scratchPath, keyBgPaint);
                c.drawPath(scratchPath, cardBorderPaint);
            }

            float cx = r.centerX();
            float cy = r.centerY();
            if (i == 0) {
                iconStrokePaint.setColor(0xFFF3F4F6);
                iconStrokePaint.setStrokeWidth(2.8f);
                c.drawLine(cx - 8.5f, cy - 8.5f, cx + 8.5f, cy + 8.5f, iconStrokePaint);
                c.drawLine(cx + 8.5f, cy - 8.5f, cx - 8.5f, cy + 8.5f, iconStrokePaint);
            } else if (i == 1) {
                textPaint.setTextSize(21f);
                textPaint.setColor(danmakuEnabled ? 0xFFFF86AE : Color.WHITE);
                textPaint.setTextAlign(Paint.Align.CENTER);
                Paint.FontMetrics fm = textPaint.getFontMetrics();
                c.drawText("弹", cx, cy - (fm.ascent + fm.descent) * 0.5f, textPaint);
            } else if (i == 2) {
                iconStrokePaint.setColor(Color.WHITE);
                iconStrokePaint.setStrokeWidth(2.3f);
                RectF eyeOval = new RectF(cx - 12.5f, cy - 7.5f, cx + 12.5f, cy + 7.5f);
                c.drawOval(eyeOval, iconStrokePaint);
                iconFillPaint.setColor(Color.WHITE);
                c.drawCircle(cx, cy, 4.0f, iconFillPaint);
            } else if (i == 3) {
                int col = passthrough ? 0xFFFF86AE : Color.WHITE;
                iconStrokePaint.setColor(col);
                iconStrokePaint.setStrokeWidth(2.2f);
                RectF box = new RectF(cx - 11.5f, cy - 8.5f, cx + 11.5f, cy + 8.5f);
                addSquirclePath(scratchPath, box, 4.5f);
                c.drawPath(scratchPath, iconStrokePaint);
                Path mountain = new Path();
                mountain.moveTo(cx - 8f, cy + 5.5f);
                mountain.lineTo(cx - 2.5f, cy - 2f);
                mountain.lineTo(cx + 2f, cy + 3.5f);
                mountain.lineTo(cx + 6f, cy - 0.5f);
                mountain.lineTo(cx + 9f, cy + 5.5f);
                c.drawPath(mountain, iconStrokePaint);
            }
        }
    }

    private void drawSpeakerIcon(Canvas c, float cx, float cy, boolean muted) {
        int col = muted ? 0xFFEF4444 : Color.WHITE;
        iconFillPaint.setColor(col);
        iconStrokePaint.setColor(col);
        iconStrokePaint.setStrokeWidth(2.2f);

        float ox = cx - 1f;
        Path p = new Path();
        p.moveTo(ox - 8.5f, cy - 3.5f);
        p.lineTo(ox - 4f, cy - 3.5f);
        p.lineTo(ox + 1.5f, cy - 8.5f);
        p.lineTo(ox + 1.5f, cy + 8.5f);
        p.lineTo(ox - 4f, cy + 3.5f);
        p.lineTo(ox - 8.5f, cy + 3.5f);
        p.close();
        c.drawPath(p, iconFillPaint);

        if (muted) {
            c.drawLine(ox + 5f, cy - 4.5f, ox + 11f, cy + 4.5f, iconStrokePaint);
            c.drawLine(ox + 11f, cy - 4.5f, ox + 5f, cy + 4.5f, iconStrokePaint);
        } else {
            RectF arc1 = new RectF(ox - 1f, cy - 5.5f, ox + 7f, cy + 5.5f);
            c.drawArc(arc1, -45f, 90f, false, iconStrokePaint);
            RectF arc2 = new RectF(ox - 4f, cy - 9f, ox + 11f, cy + 9f);
            c.drawArc(arc2, -45f, 90f, false, iconStrokePaint);
        }
    }

    private void drawSubtitleIcon(Canvas c, float cx, float cy, boolean enabled) {
        int col = enabled ? 0xFFFF86AE : 0xFFBDC3D0;
        iconStrokePaint.setColor(col);
        iconStrokePaint.setStrokeWidth(2.0f);
        RectF r = new RectF(cx - 13f, cy - 9.5f, cx + 13f, cy + 9.5f);
        addSquirclePath(scratchPath, r, 4.5f);
        c.drawPath(scratchPath, iconStrokePaint);

        textPaint.setTextSize(12.5f);
        textPaint.setColor(col);
        textPaint.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        c.drawText("CC", cx, cy - (fm.ascent + fm.descent) * 0.5f, textPaint);
    }

    private void drawSkipIcon(Canvas c, float cx, float cy, boolean forward) {
        iconFillPaint.setColor(Color.WHITE);
        iconStrokePaint.setColor(Color.WHITE);
        iconStrokePaint.setStrokeWidth(2.5f);

        if (forward) {
            Path tri = new Path();
            tri.moveTo(cx - 7.5f, cy - 8f);
            tri.lineTo(cx + 3.5f, cy);
            tri.lineTo(cx - 7.5f, cy + 8f);
            tri.close();
            c.drawPath(tri, iconFillPaint);
            c.drawLine(cx + 6.5f, cy - 7f, cx + 6.5f, cy + 7f, iconStrokePaint);
        } else {
            c.drawLine(cx - 6.5f, cy - 7f, cx - 6.5f, cy + 7f, iconStrokePaint);
            Path tri = new Path();
            tri.moveTo(cx + 7.5f, cy - 8f);
            tri.lineTo(cx - 3.5f, cy);
            tri.lineTo(cx + 7.5f, cy + 8f);
            tri.close();
            c.drawPath(tri, iconFillPaint);
        }
    }

    private void drawPlayPauseIcon(Canvas c, float cx, float cy, boolean paused) {
        iconFillPaint.setColor(Color.WHITE);
        if (paused) {
            Path tri = new Path();
            tri.moveTo(cx - 7.5f, cy - 12f);
            tri.lineTo(cx + 11.5f, cy);
            tri.lineTo(cx - 7.5f, cy + 12f);
            tri.close();
            c.drawPath(tri, iconFillPaint);
        } else {
            RectF b1 = new RectF(cx - 9f, cy - 11.5f, cx - 2.5f, cy + 11.5f);
            RectF b2 = new RectF(cx + 2.5f, cy - 11.5f, cx + 9f, cy + 11.5f);
            addSquirclePath(scratchPath, b1, 2.5f);
            c.drawPath(scratchPath, iconFillPaint);
            addSquirclePath(scratchPath, b2, 2.5f);
            c.drawPath(scratchPath, iconFillPaint);
        }
    }

    private void drawGearIcon(Canvas c, float cx, float cy, boolean open) {
        int col = open ? 0xFFFF86AE : Color.WHITE;
        iconStrokePaint.setColor(col);
        iconStrokePaint.setStrokeWidth(2.2f);
        c.drawCircle(cx, cy, 6.0f, iconStrokePaint);
        for (int i = 0; i < 8; i++) {
            double ang = i * Math.PI * 0.25;
            float x0 = cx + (float) Math.cos(ang) * 7.2f;
            float y0 = cy + (float) Math.sin(ang) * 7.2f;
            float x1 = cx + (float) Math.cos(ang) * 10.2f;
            float y1 = cy + (float) Math.sin(ang) * 10.2f;
            c.drawLine(x0, y0, x1, y1, iconStrokePaint);
        }
    }

    private void drawCubeButtonContent(Canvas c, RectF rect, int stereoMode, int synthLevel) {
        int col = (stereoMode != 0) ? 0xFFFF86AE : Color.WHITE;
        iconStrokePaint.setColor(col);
        iconStrokePaint.setStrokeWidth(1.9f);

        float cx = rect.left + 25f;
        float cy = rect.centerY();
        float r = 9.0f;
        Path hex = new Path();
        hex.moveTo(cx, cy - r);
        hex.lineTo(cx + r * 0.866f, cy - r * 0.5f);
        hex.lineTo(cx + r * 0.866f, cy + r * 0.5f);
        hex.lineTo(cx, cy + r);
        hex.lineTo(cx - r * 0.866f, cy + r * 0.5f);
        hex.lineTo(cx - r * 0.866f, cy - r * 0.5f);
        hex.close();
        c.drawPath(hex, iconStrokePaint);
        c.drawLine(cx, cy, cx, cy + r, iconStrokePaint);
        c.drawLine(cx, cy, cx + r * 0.866f, cy - r * 0.5f, iconStrokePaint);
        c.drawLine(cx, cy, cx - r * 0.866f, cy - r * 0.5f, iconStrokePaint);

        String tag = "2D";
        if (stereoMode == 1) tag = "SBS";
        else if (stereoMode == 2) tag = "OU";
        else if (stereoMode == 3) {
            if (synthLevel == 1) tag = "3D 弱";
            else if (synthLevel == 2) tag = "3D 中";
            else tag = "3D 强";
        }
        textPaint.setTextSize(14.5f);
        textPaint.setColor(col);
        textPaint.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float textCx = rect.left + 68f;
        c.drawText(tag, textCx, cy - (fm.ascent + fm.descent) * 0.5f, textPaint);
    }

    private void drawBottomStatusPill(Canvas c) {
        int bat = getBatteryPercentage();
        String timeStr = clockFormat.format(new Date());
        String status = bat + "%  ·  " + timeStr;

        textPaint.setTextSize(14.5f);
        float textW = textPaint.measureText(status);
        float iconW = 19f;
        float iconGap = 8f;
        float padX = 20f;
        float totalContentW = iconW + iconGap + textW;
        float pillW = Math.max(156f, totalContentW + padX * 2f);
        RectF pill = new RectF(512f - pillW * 0.5f, 596f, 512f + pillW * 0.5f, 630f);
        drawSquircleCard(c, pill, 13f);

        float cy = pill.centerY();
        float contentStartX = 512f - totalContentW * 0.5f;
        iconStrokePaint.setColor(Color.WHITE);
        iconStrokePaint.setStrokeWidth(1.7f);
        RectF bBody = new RectF(contentStartX, cy - 5.5f, contentStartX + 16f, cy + 5.5f);
        addSquirclePath(scratchPath, bBody, 2.5f);
        c.drawPath(scratchPath, iconStrokePaint);
        iconFillPaint.setColor(Color.WHITE);
        c.drawRect(contentStartX + 16.5f, cy - 2.5f, contentStartX + 18.8f, cy + 2.5f, iconFillPaint);
        float fillW = Math.max(2f, 12f * (bat / 100f));
        if (bat <= 20) iconFillPaint.setColor(0xFFEF4444);
        c.drawRect(contentStartX + 2f, cy - 3.5f, contentStartX + 2f + fillW, cy + 3.5f, iconFillPaint);
        iconFillPaint.setColor(Color.WHITE);

        textPaint.setColor(0xFFE5E7EB);
        textPaint.setTextAlign(Paint.Align.LEFT);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        c.drawText(status, contentStartX + iconW + iconGap, cy - (fm.ascent + fm.descent) * 0.5f, textPaint);
    }

    private void drawRowLabel(Canvas c, String label, float x, float cy) {
        textPaint.setTextSize(16f);
        textPaint.setColor(0xFFDCE1EC);
        textPaint.setTextAlign(Paint.Align.LEFT);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        c.drawText(label, x, cy - (fm.ascent + fm.descent) * 0.5f, textPaint);
    }

    public Bitmap renderSettingsCard(
            int settingsTab,
            List<QualityOption> qualities,
            int currentQualityCode,
            boolean limit4k,
            int screenCurveIdx,
            int screenPitchIdx,
            int danmakuDistIdx,
            int danmakuSpanIdx,
            boolean danmakuVOut,
            int danmakuDensityIdx,
            boolean fixedDanmakuEnabled,
            int danmakuSizeIdx,
            int subtitleSizeIdx,
            int stereoMode,
            int synth3dLevel,
            int subProtIdx,
            int projModeIdx,
            int stereoArrangement,
            int aiEdgeEnhanceIdx,
            boolean sbsUncompressed,
            boolean defaultPassthrough) {

        if (settingsBitmap == null || settingsBitmap.isRecycled()) {
            settingsBitmap = Bitmap.createBitmap(SETTINGS_W, SETTINGS_H, Bitmap.Config.ARGB_8888);
            settingsCanvas = new Canvas(settingsBitmap);
        }
        Canvas c = settingsCanvas;
        c.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR);

        RectF card = new RectF(16f, 12f, 1008f, 428f);
        drawSquircleCard(c, card, 26f);

        // --- 1. Apple Segmented Control Tabs (4 separated categories) + Close Squircle ---
        RectF segTrack = new RectF(SET_INNER_L, 22f, 922f, 62f);
        addSquirclePath(scratchPath, segTrack, 13f);
        c.drawPath(scratchPath, segTrackBgPaint);
        c.drawPath(scratchPath, optBorderPaint);

        String[] tabs = {"📺 画面设置", "💬 弹幕字幕", "🎬 视频3D/全景", "✨ AI 3D"};
        float segPad = 4f;
        float segGap = 6f;
        float tabW = (segTrack.width() - segPad * 2f - segGap * 3f) / 4f;
        float tabY = segTrack.centerY();
        for (int i = 0; i < 4; i++) {
            float tx = segTrack.left + segPad + i * (tabW + segGap);
            RectF tr = new RectF(tx, segTrack.top + segPad, tx + tabW, segTrack.bottom - segPad);
            boolean isTabSel = (settingsTab == i);
            addSquirclePath(scratchPath, tr, 10f);
            if (isTabSel) {
                c.drawPath(scratchPath, optActivePaint);
            }
            textPaint.setTextSize(14.5f);
            textPaint.setColor(isTabSel ? 0xFF101319 : 0xFFD4D9E3);
            textPaint.setTextAlign(Paint.Align.CENTER);
            Paint.FontMetrics fm = textPaint.getFontMetrics();
            c.drawText(tabs[i], tr.centerX(), tabY - (fm.ascent + fm.descent) * 0.5f, textPaint);
        }

        // Close [X] squircle flush with SET_INNER_R
        RectF closeBtn = new RectF(934f, 22f, SET_INNER_R, 62f);
        addSquirclePath(scratchPath, closeBtn, 12f);
        c.drawPath(scratchPath, optBgPaint);
        c.drawPath(scratchPath, optBorderPaint);
        iconStrokePaint.setColor(Color.WHITE);
        iconStrokePaint.setStrokeWidth(2.6f);
        c.drawLine(closeBtn.centerX() - 7.5f, closeBtn.centerY() - 7.5f, closeBtn.centerX() + 7.5f, closeBtn.centerY() + 7.5f, iconStrokePaint);
        c.drawLine(closeBtn.centerX() + 7.5f, closeBtn.centerY() - 7.5f, closeBtn.centerX() - 7.5f, closeBtn.centerY() + 7.5f, iconStrokePaint);

        c.drawLine(SET_INNER_L, 74f, SET_INNER_R, 74f, dividerPaint);

        // --- 2. Tab Contents ---
        if (settingsTab == 0) {
            String[] rowLabels = {"\u89c6\u9891\u5206\u8fa8\u7387", "\u5206\u8fa8\u7387\u9650\u5236", "\u753b\u9762\u66f2\u7387", "\u753b\u9762\u4fef\u4ef0", "\u8fdb\u5165\u80cc\u666f"};
            for (int r = 0; r < 5; r++) {
                float cy = TAB0_Y0 + r * TAB0_STEP;
                drawRowLabel(c, rowLabels[r], SET_INNER_L, cy);
                if (r == 0) {
                    int count = (qualities != null) ? Math.min(6, qualities.size()) : 0;
                    if (count == 0) {
                        drawRowPills(c, cy, new String[]{"\u81ea\u52a8 (\u5f53\u524d\u539f\u753b)"}, 0, 40f, 11.5f);
                    } else {
                        String[] labels = new String[count];
                        int sel = -1;
                        for (int i = 0; i < count; i++) {
                            QualityOption q = qualities.get(i);
                            labels[i] = q.shortDesc;
                            if (q.code == currentQualityCode
                                    || (currentQualityCode == 0 && i == 0)) sel = i;
                        }
                        drawRowPills(c, cy, labels, sel, 40f, 11.5f);
                    }
                } else if (r == 1) {
                    drawRowPills(c, cy,
                            new String[]{"\u4e0d\u9650\u5236 (\u539f\u753b\u81ea\u9002\u5e94)", "\u9650\u5236 2.5K (\u9632\u53d1\u70ed)"},
                            limit4k ? 1 : 0, 40f, 11.5f);
                } else if (r == 2) {
                    drawRowPills(c, cy,
                            new String[]{"\u5e73\u9762 (0%)", "\u5fae\u66f2 (40%)", "\u6807\u51c6\u66f2\u9762 (70%)", "\u73af\u5e55 (100%)"},
                            screenCurveIdx, 40f, 11.5f);
                } else if (r == 3) {
                    drawRowPills(c, cy,
                            new String[]{"\u5e73\u89c6 (0\u00b0)", "\u5fae\u4ef0 (15\u00b0)", "\u8212\u670d (30\u00b0)", "\u534a\u8eba (45\u00b0)", "\u5367\u59ff (65\u00b0)"},
                            screenPitchIdx, 40f, 11.5f);
                } else if (r == 4) {
                    drawRowPills(c, cy,
                            new String[]{"\u900f\u89c6 (\u9ed8\u8ba4\u5b9e\u666f)", "\u5168\u9ed1 (\u6c89\u6d78\u5f71\u9662)"},
                            defaultPassthrough ? 0 : 1, 40f, 11.5f);
                }
            }
        } else if (settingsTab == 1) {
            String[] rowLabels = {"\u51fa\u5c4f\u8ddd\u79bb", "\u6c34\u5e73\u8303\u56f4", "\u4e0a\u4e0b\u51fa\u5c4f", "\u53d1\u5c04\u5bc6\u5ea6", "\u56fa\u5b9a\u5f39\u5e55", "\u5b57\u53f7\u8c03\u8282"};
            boolean is3dVideo = (projModeIdx == 0) ? (stereoMode != 0) : (projModeIdx == 2 || projModeIdx == 4);
            for (int r = 0; r < 6; r++) {
                float cy = TAB1_Y0 + r * TAB1_STEP;
                drawRowLabel(c, rowLabels[r], SET_INNER_L, cy);
                if (r == 0) {
                    drawRowPills(c, cy,
                            new String[]{"\u8d34\u5c4f (99%)", "\u5fae\u6d6e (88%)", "\u51fa\u5c4f (76%)", "\u8d85\u8fd1\u773c\u524d (64%)"},
                            danmakuDistIdx, 36f, 10.5f);
                } else if (r == 1) {
                    drawRowPills(c, cy,
                            new String[]{"\u5e38\u89c4\u5e73\u5e55", "\u5e7f\u89d2 130\u00b0", "180\u00b0 \u5305\u56f4", is3dVideo ? "3D\u8dd1\u9053(\u4ec52D)" : "3D \u73af\u7ed5\u8dd1\u9053"},
                            danmakuSpanIdx, 36f, 10.5f, is3dVideo ? 0b0111 : 0b1111);
                } else if (r == 2) {
                    drawRowPills(c, cy,
                            new String[]{"\u5173\u95ed (\u4ec5\u89c6\u9891\u9ad8\u5ea6)", "\u5f00\u542f (\u5929\u5e55\u5730\u9762\u5ef6\u5c55)"},
                            danmakuVOut ? 1 : 0, 36f, 10.5f);
                } else if (r == 3) {
                    int densityMask = (danmakuSpanIdx == 3 || is3dVideo) ? 0b0001 : 0b1111;
                    drawRowPills(c, cy,
                            new String[]{"\u7a00\u5c11 25%", "\u9002\u4e2d 50%", "\u6807\u51c6 75%", "\u6ee1\u5c4f 100%"},
                            danmakuDensityIdx, 36f, 10.5f, densityMask);
                } else if (r == 4) {
                    drawRowPills(c, cy,
                            new String[]{"\u5173\u95ed (\u4ec5\u663e\u793a\u6eda\u52a8)", "\u5f00\u542f (\u663e\u793a\u9876\u90e8/\u5e95\u90e8)"},
                            fixedDanmakuEnabled ? 1 : 0, 36f, 10.5f);
                } else if (r == 5) {
                    textPaint.setTextSize(14.5f);
                    textPaint.setColor(0xFFA8B0C0);
                    textPaint.setTextAlign(Paint.Align.LEFT);
                    Paint.FontMetrics fm = textPaint.getFontMetrics();
                    float labelY = cy - (fm.ascent + fm.descent) * 0.5f;
                    c.drawText("\u5f39\u5e55", ROW_X0, labelY, textPaint);
                    c.drawText("\u5b57\u5e55", 592f, labelY, textPaint);
                    String[] szShort = {"\u5c0f", "\u4e2d", "\u5927", "\u7279"};
                    for (int i = 0; i < 4; i++) {
                        drawOptionPill(c, 228f + i * 88f, cy, 80f, 36f, 10.5f, szShort[i], danmakuSizeIdx == i);
                        drawOptionPill(c, 640f + i * 88f, cy, 80f, 36f, 10.5f, szShort[i], subtitleSizeIdx == i);
                    }
                }
            }
        } else if (settingsTab == 2) {
            // Native video 3D & panorama format settings (separated from AI 3D)
            int rowCount = (projModeIdx == 0) ? 3 : 2;
            String[] rowLabels = {"\u64ad\u653e\u683c\u5f0f", "\u89c6\u98913D\u683c\u5f0f", "SBS \u753b\u9762\u6bd4\u4f8b"};
            for (int r = 0; r < rowCount; r++) {
                float cy = TAB2_Y0 + r * TAB2_STEP;
                drawRowLabel(c, rowLabels[r], SET_INNER_L, cy);
                if (r == 0) {
                    drawRowPills(c, cy,
                            new String[]{"\u5de8\u5e55\u5f71\u9662", "VR180", "\u53cc\u76ee180", "VR360", "\u53cc\u76ee360"},
                            projModeIdx, 44f, 12f);
                } else if (r == 1) {
                    if (projModeIdx == 0) {
                        int nativeSel = (stereoMode >= 0 && stereoMode <= 2) ? stereoMode : -1;
                        drawRowPills(c, cy,
                                new String[]{"2D \u5e38\u89c4", "\u5de6\u53f3 3D (SBS)", "\u4e0a\u4e0b 3D (OU)"},
                                nativeSel, 44f, 12f);
                    } else if (projModeIdx == 2 || projModeIdx == 4) {
                        drawRowPills(c, cy,
                                new String[]{"\u5de6\u53f3\u53cc\u76ee (SBS \u5e38\u7528)", "\u4e0a\u4e0b\u53cc\u76ee (OU)"},
                                stereoArrangement, 44f, 12f);
                    } else {
                        drawRowPills(c, cy, new String[]{"\u5355\u76ee\u5168\u666f (\u4e24\u773c\u540c\u753b)"}, 0, 44f, 12f);
                    }
                } else if (r == 2) {
                    int sbsSel = (stereoMode == 1) ? (sbsUncompressed ? 1 : 0) : -1;
                    drawRowPills(c, cy,
                            new String[]{"\u538b\u7f29 SBS (\u534a\u5bbd)", "\u4e0d\u538b\u7f29 SBS (\u5168\u5bbd)"},
                            sbsSel, 44f, 12f);
                }
            }
        } else if (settingsTab == 3) {
            // Dedicated AI 3D depth settings
            String[] rowLabels = {"AI 3D \u5f00\u5173", "AI 3D \u5f3a\u5ea6", "\u5e95\u90e8\u96323D\u533a",
                    "AI \u6df1\u5ea6\u6765\u6e90", "\u6df1\u5ea6\u56fe\u8c03\u8bd5", "\u6df1\u5ea6\u5e27\u7387\u4e0a\u9650", "AI \u8fb9\u7f18\u589e\u5f3a"};
            boolean ai3dActive = (projModeIdx == 0 && stereoMode == 3);
            for (int r = 0; r < 7; r++) {
                float cy = TAB3_Y0 + r * TAB3_STEP;
                drawRowLabel(c, rowLabels[r], SET_INNER_L, cy);
                if (r == 0) {
                    drawRowPills(c, cy,
                            new String[]{"\u5173\u95ed (2D \u753b\u9762)", "\u5f00\u542f (AI \u5408\u6210 3D)"},
                            ai3dActive ? 1 : 0, 35f, 10f);
                } else if (r == 1) {
                    drawRowPills(c, cy,
                            new String[]{"\u5f31", "\u4e2d", "\u5f3a"},
                            ai3dActive ? synth3dLevel - 1 : -1, 35f, 10f);
                } else if (r == 2) {
                    drawRowPills(c, cy,
                            new String[]{"\u5173\u95ed (\u51683D)", "\u5c0f 10%", "\u6807\u51c6 16%", "\u5927 22%", "\u7279\u5927 28%"},
                            subProtIdx, 35f, 10f);
                } else if (r == 3) {
                    drawRowPills(c, cy,
                            new String[]{"\u539f\u7248 (\u5185\u7f6e\u666f\u6df1)",
                                    ai3dActive ? "ZipDepth AI" : "ZipDepth AI (\u9700\u5f00\u542f AI 3D)"},
                            VrState.aiDepthEnabled ? 1 : 0, 35f, 10f);
                } else if (r == 4) {
                    drawRowPills(c, cy,
                            new String[]{"\u5173\u95ed (\u6b63\u5e38\u753b\u9762)",
                                    ai3dActive ? "\u663e\u793a\u6df1\u5ea6\u56fe (\u7ea2\u8fd1\u84dd\u8fdc)" : "\u663e\u793a\u6df1\u5ea6\u56fe (\u9700\u5f00\u542f AI 3D)"},
                            (ai3dActive && VrState.showDepthMap) ? 1 : 0, 35f, 10f);
                } else if (r == 5) {
                    drawRowPills(c, cy,
                            new String[]{"\u6807\u51c6 20 \u5f20/\u79d2", "\u5b9e\u9a8c 30 \u5f20/\u79d2"},
                            depthTargetMapsPerSec >= 30 ? 1 : 0, 35f, 10f);
                } else if (r == 6) {
                    drawRowPills(c, cy,
                            new String[]{"\u5173\u95ed (\u67d4\u548c)", "\u5747\u8861 (\u63a8\u8350)", "\u5f3a\u9510\u5316"},
                            aiEdgeEnhanceIdx, 35f, 10f);
                }
            }
        }
        return settingsBitmap;
    }

    private static float rowPillW(int count) {
        return (ROW_X1 - ROW_X0 - (count - 1) * ROW_GAP) / count;
    }

    private static float rowPillX(int i, int count) {
        return ROW_X0 + i * (rowPillW(count) + ROW_GAP);
    }

    private static int pickRowPill(float px, int count) {
        float w = rowPillW(count);
        for (int i = 0; i < count; i++) {
            float x0 = rowPillX(i, count);
            if (px >= x0 && px <= x0 + w) return i;
        }
        return -1;
    }

    private void drawRowPills(Canvas c, float cy, String[] labels, int selected, float h, float cornerR) {
        drawRowPills(c, cy, labels, selected, h, cornerR, ~0);
    }

    private void drawRowPills(Canvas c, float cy, String[] labels, int selected, float h, float cornerR, int enabledMask) {
        int n = labels.length;
        float w = rowPillW(n);
        for (int i = 0; i < n; i++) {
            boolean enabled = ((enabledMask & (1 << i)) != 0);
            drawOptionPill(c, rowPillX(i, n), cy, w, h, cornerR, labels[i], i == selected, enabled);
        }
    }

    private void drawOptionPill(Canvas c, float x, float cy, float w, float h, float cornerR, String label, boolean selected) {
        drawOptionPill(c, x, cy, w, h, cornerR, label, selected, true);
    }

    private void drawOptionPill(Canvas c, float x, float cy, float w, float h, float cornerR, String label, boolean selected, boolean enabled) {
        RectF r = new RectF(x, cy - h * 0.5f, x + w, cy + h * 0.5f);
        addSquirclePath(scratchPath, r, cornerR);
        if (selected && enabled) {
            c.drawPath(scratchPath, optActivePaint);
        } else {
            c.drawPath(scratchPath, optBgPaint);
            c.drawPath(scratchPath, optBorderPaint);
        }

        float size = 14.5f;
        textPaint.setTextSize(size);
        float textW = textPaint.measureText(label);
        float availW = Math.max(24f, w - 16f);
        if (textW > availW) {
            size = Math.max(11.5f, size * (availW / textW));
            textPaint.setTextSize(size);
        }
        textPaint.setColor(!enabled ? 0x558B94A6 : (selected ? 0xFF101319 : 0xFFE6E9F2));
        textPaint.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        c.drawText(label, r.centerX(), cy - (fm.ascent + fm.descent) * 0.5f, textPaint);
    }

    public boolean handleSettingsClick(float normX, float normY, VrPlayerActivity activity, List<QualityOption> qualities) {
        float px = normX * SETTINGS_W;
        float py = normY * SETTINGS_H;

        // 1. Close [X] squircle button
        if (px >= 926f && px <= 994f && py >= 16f && py <= 68f) {
            activity.toggleSettingsPanel();
            return true;
        }

        // 2. Category Tabs (4 segmented controls spanning [40..922])
        if (py >= 16f && py <= 68f && px >= SET_INNER_L && px <= 922f) {
            float tabSpan = 922f - SET_INNER_L;
            int tabIdx = Math.max(0, Math.min(3, (int) (((px - SET_INNER_L) / tabSpan) * 4f)));
            activity.setSettingsTab(tabIdx);
            return true;
        }

        int currentTab = activity.getSettingsTab();
        if (currentTab == 0) {
            int row = Math.round((py - TAB0_Y0) / TAB0_STEP);
            if (row < 0 || row >= 5 || Math.abs(py - (TAB0_Y0 + row * TAB0_STEP)) > 26f) {
                return false;
            }
            int idx;
            if (row == 0) {
                int count = (qualities != null) ? Math.min(6, qualities.size()) : 0;
                idx = count > 0 ? pickRowPill(px, count) : -1;
                if (idx >= 0) { activity.selectVideoQuality(qualities.get(idx)); return true; }
            } else if (row == 1) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setLimit4kFromSettings(idx == 1); return true; }
            } else if (row == 2) {
                idx = pickRowPill(px, 4);
                if (idx >= 0) { activity.setScreenCurveIdx(idx); return true; }
            } else if (row == 3) {
                idx = pickRowPill(px, 5);
                if (idx >= 0) { activity.setScreenPitchPresetIdx(idx); return true; }
            } else if (row == 4) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setDefaultPassthroughFromSettings(idx == 0); return true; }
            }
        } else if (currentTab == 1) {
            int row = Math.round((py - TAB1_Y0) / TAB1_STEP);
            if (row < 0 || row >= 6 || Math.abs(py - (TAB1_Y0 + row * TAB1_STEP)) > 24f) {
                return false;
            }
            int idx;
            if (row == 0) {
                idx = pickRowPill(px, 4);
                if (idx >= 0) { activity.setDanmakuDistIdx(idx); return true; }
            } else if (row == 1) {
                idx = pickRowPill(px, 4);
                if (idx >= 0) { activity.setDanmakuSpanIdx(idx); return true; }
            } else if (row == 2) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setDanmakuVOut(idx == 1); return true; }
            } else if (row == 3) {
                idx = pickRowPill(px, 4);
                if (idx >= 0) { activity.setDanmakuDensityIdx(idx); return true; }
            } else if (row == 4) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setFixedDanmakuFromSettings(idx == 1); return true; }
            } else if (row == 5) {
                for (int i = 0; i < 4; i++) {
                    float x0 = 228f + i * 88f;
                    if (px >= x0 && px <= x0 + 80f) { activity.setDanmakuSizeIdx(i); return true; }
                    float sx0 = 640f + i * 88f;
                    if (px >= sx0 && px <= sx0 + 80f) { activity.setSubtitleSizeIdx(i); return true; }
                }
            }
        } else if (currentTab == 2) {
            int row = Math.round((py - TAB2_Y0) / TAB2_STEP);
            int pIdx = activity.getProjModeIdx();
            int maxRows = (pIdx == 0) ? 3 : 2;
            if (row < 0 || row >= maxRows || Math.abs(py - (TAB2_Y0 + row * TAB2_STEP)) > 30f) {
                return false;
            }
            int idx;
            if (row == 0) {
                idx = pickRowPill(px, 5);
                if (idx >= 0) { activity.setProjectionModeFromSettings(idx); return true; }
            } else if (row == 1) {
                if (pIdx == 0) {
                    idx = pickRowPill(px, 3);
                    if (idx >= 0) { activity.setStereoModeFromSettings(idx); return true; }
                } else if (pIdx == 2 || pIdx == 4) {
                    idx = pickRowPill(px, 2);
                    if (idx >= 0) { activity.setStereoArrangementFromSettings(idx); return true; }
                }
            } else if (row == 2 && pIdx == 0) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setSbsUncompressedFromSettings(idx == 1); return true; }
            }
        } else if (currentTab == 3) {
            int row = Math.round((py - TAB3_Y0) / TAB3_STEP);
            if (row < 0 || row >= 7 || Math.abs(py - (TAB3_Y0 + row * TAB3_STEP)) > 22f) {
                return false;
            }
            int idx;
            if (row == 0) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) {
                    activity.setStereoModeFromSettings(idx == 1 ? 3 : 0);
                    return true;
                }
            } else if (row == 1) {
                idx = pickRowPill(px, 3);
                if (idx >= 0) { activity.setSynth3dLevelFromSettings(idx + 1); return true; }
            } else if (row == 2) {
                idx = pickRowPill(px, 5);
                if (idx >= 0) { activity.setSubProtIdx(idx); return true; }
            } else if (row == 3) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setAiDepthFromSettings(idx == 1); return true; }
            } else if (row == 4) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setShowDepthFromSettings(idx == 1); return true; }
            } else if (row == 5) {
                idx = pickRowPill(px, 2);
                if (idx >= 0) { activity.setDepthRateCapFromSettings(idx == 1 ? 30 : 20); return true; }
            } else if (row == 6) {
                idx = pickRowPill(px, 3);
                if (idx >= 0) { activity.setAiEdgeEnhanceFromSettings(idx); return true; }
            }
        }
        return false;
    }

    public void handleClick(float normX, float normY, VrPlayerActivity activity, boolean settingsOpen, List<QualityOption> qualities) {
        float px = normX * HUD_W;
        float py = normY * HUD_H;

        // 1. Top 4 floating squircle buttons
        for (int i = 0; i < 4; i++) {
            RectF r = topSquircleRects[i];
            if (px >= r.left - 6f && px <= r.right + 6f && py >= r.top - 6f && py <= r.bottom + 6f) {
                if (i == 0) activity.exitVr();
                else if (i == 1) activity.toggleDanmaku();
                else if (i == 2) activity.toggleHudVisibility();
                else if (i == 3) activity.togglePassthrough();
                return;
            }
        }

        // 2. Seekbar click (disabled for live streams)
        if (!liveMode && seekBarHitRect.contains(px, py)) {
            float ratio = (px - SEEK_X0) / (SEEK_X1 - SEEK_X0);
            if (ratio < 0f) ratio = 0f;
            if (ratio > 1f) ratio = 1f;
            activity.seekToFraction(ratio);
            return;
        }

        // 3. Aligned Keycaps Row
        if (btnMuteRect.contains(px, py)) {
            activity.toggleMute();
        } else if (btnSubRect.contains(px, py)) {
            activity.toggleSubtitle();
        } else if (btnPrevRect.contains(px, py)) {
            activity.seekRelative(-10);
        } else if (btnPlayRect.contains(px, py)) {
            activity.togglePlayPause();
        } else if (btnNextRect.contains(px, py)) {
            activity.seekRelative(10);
        } else if (btnSettingsRect.contains(px, py)) {
            activity.toggleSettingsPanel();
        } else if (btnStereoRect.contains(px, py)) {
            activity.cycleStereoModeHud();
        }
    }

    private int getBatteryPercentage() {
        try {
            IntentFilter ifilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            Intent batteryStatus = context.registerReceiver(null, ifilter);
            if (batteryStatus != null) {
                int level = batteryStatus.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                int scale = batteryStatus.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
                if (level >= 0 && scale > 0) {
                    return Math.round((level * 100f) / scale);
                }
            }
        } catch (Throwable ignored) {}
        return 100;
    }

    private static String formatTime(double sec) {
        if (sec < 0 || Double.isNaN(sec) || Double.isInfinite(sec)) sec = 0;
        long total = Math.round(sec);
        long h = total / 3600;
        long m = (total % 3600) / 60;
        long s = total % 60;
        return String.format(Locale.ENGLISH, "%02d:%02d:%02d", h, m, s);
    }
}
