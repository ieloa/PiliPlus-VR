package com.example.piliplus.vr

import android.content.Context
import android.content.SharedPreferences

/**
 * Cross-activity handoff for the VR playback session.
 *
 * VrPlayerActivity is declared with `launchMode="singleTask"` and its own `taskAffinity`,
 * i.e. it lives in a different task than MainActivity. Android does not deliver
 * `startActivityForResult` results across tasks — the caller gets RESULT_CANCELED
 * immediately — so the exit position can never travel back through the intent.
 * This SharedPreferences-backed state is the channel that actually works: the activity
 * publishes its progress while it lives, and Flutter polls [read] until the session ends.
 */
object VrSession {
    private const val PREFS = "vr_session"
    private const val KEY_ACTIVE = "active"
    private const val KEY_POSITION = "position_ms"
    private const val KEY_BVID = "bvid"
    private const val KEY_UPDATED = "updated_at"

    private fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Called by VrPlayerActivity when the session starts. */
    fun begin(context: Context, bvid: String, positionMs: Long = 0L) {
        prefs(context).edit()
            .putBoolean(KEY_ACTIVE, true)
            .putString(KEY_BVID, bvid)
            .putLong(KEY_POSITION, positionMs)
            .putLong(KEY_UPDATED, System.currentTimeMillis())
            .apply()
    }

    /** Called periodically while playing, so a killed process still leaves a usable position. */
    fun update(context: Context, bvid: String, positionMs: Long) {
        prefs(context).edit()
            .putString(KEY_BVID, bvid)
            .putLong(KEY_POSITION, positionMs)
            .putLong(KEY_UPDATED, System.currentTimeMillis())
            .apply()
    }

    /** Called by VrPlayerActivity when the session is over. */
    fun end(context: Context, bvid: String, positionMs: Long) {
        prefs(context).edit()
            .putBoolean(KEY_ACTIVE, false)
            .putString(KEY_BVID, bvid)
            .putLong(KEY_POSITION, positionMs)
            .putLong(KEY_UPDATED, System.currentTimeMillis())
            .apply()
    }

    /** Snapshot for the Flutter side. `active` is false when no VR session is running. */
    fun read(context: Context): Map<String, Any> {
        val p = prefs(context)
        return HashMap<String, Any>().apply {
            put("active", p.getBoolean(KEY_ACTIVE, false))
            put("lastPosition", p.getLong(KEY_POSITION, 0L))
            put("lastBvid", p.getString(KEY_BVID, "") ?: "")
            put("updatedAt", p.getLong(KEY_UPDATED, 0L))
        }
    }
}
