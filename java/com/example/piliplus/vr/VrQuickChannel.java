package com.example.piliplus.vr;

import android.app.Activity;
import android.content.SharedPreferences;
import android.util.Log;

/**
 * Bridge for the two VR buttons that live inside the Flutter widget tree next
 * to the "关注" button.
 *
 * A MethodChannel cannot be used here: this APK was R8-shrunk and no longer
 * contains io.flutter.plugin.common.MethodChannel (nor the original vr_bridge
 * handler). Instead both sides share Android's "FlutterSharedPreferences"
 * file, which the shared_preferences plugin uses with a "flutter." key prefix.
 * A native listener reacts immediately to writes from Dart.
 */
public final class VrQuickChannel {
    private static final String TAG = "PiliVR";
    private static final String FILE = "FlutterSharedPreferences";
    private static final String KEY_AUTO = "flutter.vr_auto_enter";
    private static final String KEY_ENTER = "flutter.vr_quick_enter";

    private static SharedPreferences prefs;
    private static SharedPreferences.OnSharedPreferenceChangeListener listener;

    private VrQuickChannel() {}

    public static synchronized void register(final Activity activity) {
        if (activity == null) return;
        try {
            if (listener != null && prefs != null) {
                prefs.unregisterOnSharedPreferenceChangeListener(listener);
            }
            final SharedPreferences sp =
                    activity.getSharedPreferences(FILE, Activity.MODE_PRIVATE);

            // Publish VrState's value so the Dart widgets and the VR settings
            // card always agree. Done before registering so it cannot echo.
            sp.edit().putBoolean(KEY_AUTO, VrState.vrAutoEnter).apply();
            prefs = sp;

            listener = new SharedPreferences.OnSharedPreferenceChangeListener() {
                @Override
                public void onSharedPreferenceChanged(SharedPreferences shared, String key) {
                    if (KEY_ENTER.equals(key)) {
                        VrState.enterVR(activity);
                    } else if (KEY_AUTO.equals(key)) {
                        boolean on = shared.getBoolean(KEY_AUTO, VrState.vrAutoEnter);
                        // setAutoEnter writes the same key back; comparing
                        // against the live value stops the loop.
                        if (on != VrState.vrAutoEnter) {
                            VrState.setAutoEnter(activity, on);
                        }
                    }
                }
            };
            sp.registerOnSharedPreferenceChangeListener(listener);
            Log.i(TAG, "vr_quick prefs bridge registered");
        } catch (Throwable t) {
            Log.w(TAG, "vr_quick prefs bridge failed", t);
        }
    }
}
