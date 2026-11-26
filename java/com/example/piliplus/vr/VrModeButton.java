package com.example.piliplus.vr;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;

import java.lang.ref.WeakReference;

/**
 * Installs the native side of the VR entry points.
 *
 * In the 2D Activity we no longer add floating overlay views: the two buttons
 * are real Flutter widgets inside the video intro Row (see VrQuickButtons in
 * the Dart sources), so they line up with "关注" and disappear in fullscreen.
 * Here we only register the MethodChannel those widgets talk to.
 *
 * The VR Activity still gets its own top-right "退出 VR" button.
 */
public final class VrModeButton {
    private static final String TAG_BUTTON = "pili_vr_toggle_button";
    private static final String TAG_AUTO = "pili_vr_auto_button";
    private static final Handler UI_HANDLER = new Handler(Looper.getMainLooper());
    private static WeakReference<Button> buttonRef = null;

    private VrModeButton() {}

    /** Kept for VrState.onVideoOutput(); harmless when there is no 2D button. */
    public static void setVisible(final boolean visible) {
        UI_HANDLER.post(new Runnable() {
            @Override
            public void run() {
                Button btn = (buttonRef != null) ? buttonRef.get() : null;
                if (btn != null) {
                    btn.setVisibility(visible ? View.VISIBLE : View.GONE);
                }
            }
        });
    }

    public static void updateVisibility() {
        setVisible(VrState.isVideoActive());
    }

    public static void install(final Activity activity) {
        if (activity == null) return;

        final ViewGroup root = (ViewGroup) activity.findViewById(android.R.id.content);
        if (root == null) return;

        final boolean inVr = (activity instanceof VrPlayerActivity);
        if (inVr) {
            installVrExitButton(activity, root);
            return;
        }

        // The 2D Activity is where a video is opened, so that is where the
        // saved preference matters.
        VrState.loadAutoEnter(activity);
        VrQuickChannel.register(activity);

        // Remove overlay buttons left over from an older build in the same
        // decor hierarchy; the Dart widgets replace them.
        for (int i = root.getChildCount() - 1; i >= 0; i--) {
            View child = root.getChildAt(i);
            Object tag = child.getTag();
            if (TAG_BUTTON.equals(tag) || TAG_AUTO.equals(tag)) {
                root.removeViewAt(i);
            }
        }
        buttonRef = null;
    }

    private static void installVrExitButton(final Activity activity, ViewGroup root) {
        for (int i = 0; i < root.getChildCount(); i++) {
            if (TAG_BUTTON.equals(root.getChildAt(i).getTag())) {
                buttonRef = new WeakReference<Button>((Button) root.getChildAt(i));
                return;
            }
        }

        Button btn = new Button(activity);
        btn.setTag(TAG_BUTTON);
        btn.setText("退出 VR");
        btn.setTextColor(Color.WHITE);
        btn.setTextSize(12f);
        btn.setAllCaps(false);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(200, 200, 50, 50));
        bg.setCornerRadius(24f);
        btn.setBackground(bg);
        int pad = dp(activity, 8);
        btn.setPadding(pad * 2, pad, pad * 2, pad);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.TOP | Gravity.END;
        lp.topMargin = dp(activity, 48);
        lp.rightMargin = dp(activity, 16);
        btn.setLayoutParams(lp);
        btn.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                VrState.switchTo2D(activity);
            }
        });
        buttonRef = new WeakReference<Button>(btn);
        root.addView(btn);
    }

    private static int dp(Activity activity, int val) {
        float d = activity.getResources().getDisplayMetrics().density;
        return Math.round(val * d);
    }
}
