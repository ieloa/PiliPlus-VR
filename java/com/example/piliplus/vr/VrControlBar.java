package com.example.piliplus.vr;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.media.AudioManager;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class VrControlBar {

    public interface Callback {
        void onTogglePassthrough();
        void onRecenter();
        void onExitVr();
    }

    public static View create(final Activity activity, final Callback callback) {
        final Context ctx = activity;

        final LinearLayout bar = new LinearLayout(ctx);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER);

        // Semi-transparent rounded background
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(200, 20, 20, 25));
        bg.setCornerRadius(32f);
        bg.setStroke(2, Color.argb(100, 255, 255, 255));
        bar.setBackground(bg);
        int padH = dp(ctx, 20);
        int padV = dp(ctx, 10);
        bar.setPadding(padH, padV, padH, padV);

        // Play/Pause button
        final Button btnPlayPause = createButton(ctx, "⏸ 暂停");
        btnPlayPause.setOnClickListener(new View.OnClickListener() {
            private boolean isPaused = false;
            @Override
            public void onClick(View v) {
                sendMediaKey(ctx, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE);
                isPaused = !isPaused;
                btnPlayPause.setText(isPaused ? "▶ 播放" : "⏸ 暂停");
            }
        });
        bar.addView(btnPlayPause);

        // -10s Rewind button
        Button btnRewind = createButton(ctx, "⏪ -10s");
        btnRewind.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendMediaKey(ctx, KeyEvent.KEYCODE_MEDIA_REWIND);
            }
        });
        bar.addView(btnRewind);

        // +10s Fast forward button
        Button btnForward = createButton(ctx, "⏩ +10s");
        btnForward.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendMediaKey(ctx, KeyEvent.KEYCODE_MEDIA_FAST_FORWARD);
            }
        });
        bar.addView(btnForward);

        // Passthrough button
        Button btnPass = createButton(ctx, "🌌 透视");
        btnPass.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (callback != null) callback.onTogglePassthrough();
            }
        });
        bar.addView(btnPass);

        // Recenter button
        Button btnRecenter = createButton(ctx, "🎯 居中");
        btnRecenter.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (callback != null) callback.onRecenter();
            }
        });
        bar.addView(btnRecenter);

        // Exit VR button
        Button btnExit = createButton(ctx, "🔙 退出VR");
        btnExit.setBackgroundColor(Color.argb(180, 180, 40, 40));
        btnExit.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (callback != null) callback.onExitVr();
            }
        });
        bar.addView(btnExit);

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
        );
        lp.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
        lp.bottomMargin = dp(ctx, 40);
        bar.setLayoutParams(lp);

        return bar;
    }

    private static Button createButton(Context ctx, String text) {
        Button btn = new Button(ctx);
        btn.setText(text);
        btn.setTextColor(Color.WHITE);
        btn.setTextSize(14f);
        btn.setAllCaps(false);
        btn.setBackgroundColor(Color.argb(120, 50, 50, 60));
        int pad = dp(ctx, 8);
        btn.setPadding(pad * 2, pad, pad * 2, pad);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        lp.setMargins(pad, 0, pad, 0);
        btn.setLayoutParams(lp);
        return btn;
    }

    public static void sendMediaKey(Context ctx, int keyCode) {
        try {
            AudioManager am = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
            long now = SystemClock.uptimeMillis();
            KeyEvent down = new KeyEvent(now, now, KeyEvent.ACTION_DOWN, keyCode, 0);
            KeyEvent up = new KeyEvent(now, now, KeyEvent.ACTION_UP, keyCode, 0);
            if (am != null) {
                am.dispatchMediaKeyEvent(down);
                am.dispatchMediaKeyEvent(up);
            }
        } catch (Throwable t) {
            // ignore
        }
    }

    private static int dp(Context ctx, int val) {
        float d = ctx.getResources().getDisplayMetrics().density;
        return Math.round(val * d);
    }
}
