package com.example.piliplus.vr

import android.app.Activity
import android.content.Intent
import android.os.Build
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.PluginRegistry

class VrBridgePlugin(private val activity: Activity) : MethodChannel.MethodCallHandler, PluginRegistry.ActivityResultListener {

    private var pendingResult: MethodChannel.Result? = null

    companion object {
        private const val CHANNEL_NAME = "com.example.piliplus/vr_bridge"
        private const val REQUEST_CODE_VR = 9182

        fun registerWith(flutterEngine: FlutterEngine, activity: Activity) {
            val channel = MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL_NAME)
            val plugin = VrBridgePlugin(activity)
            channel.setMethodCallHandler(plugin)
        }
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "openVrPlayer" -> {
                val videoUrl = call.argument<String>("videoUrl")
                if (videoUrl.isNullOrEmpty()) {
                    result.error("INVALID_URL", "Video URL is required", null)
                    return
                }

                val audioUrl = call.argument<String>("audioUrl")
                val title = call.argument<String>("title") ?: "Bilibili VR"
                val positionMs = (call.argument<Number>("positionMs"))?.toLong() ?: 0L
                val vrMode = call.argument<String>("vrMode") ?: "cinema"

                val bvid = call.argument<String>("bvid") ?: ""
                val aid = (call.argument<Number>("aid"))?.toLong() ?: 0L
                val cid = (call.argument<Number>("cid"))?.toLong() ?: 0L
                val desc = call.argument<String>("desc") ?: ""
                val ownerName = call.argument<String>("ownerName") ?: ""
                val ownerFace = call.argument<String>("ownerFace") ?: ""
                val viewCount = call.argument<String>("viewCount") ?: ""
                val likeCount = call.argument<String>("likeCount") ?: ""
                val cookie = call.argument<String>("cookie") ?: ""

                pendingResult = result

                val intent = Intent(activity, VrPlayerActivity::class.java).apply {
                    putExtra("video_url", videoUrl)
                    putExtra("audio_url", audioUrl)
                    putExtra("video_title", title)
                    putExtra("start_position", positionMs)
                    putExtra("vr_mode", vrMode)
                    putExtra("bvid", bvid)
                    putExtra("aid", aid)
                    putExtra("cid", cid)
                    putExtra("desc", desc)
                    putExtra("owner_name", ownerName)
                    putExtra("owner_face", ownerFace)
                    putExtra("view_count", viewCount)
                    putExtra("like_count", likeCount)
                    putExtra("cookie", cookie)
                }

                activity.startActivityForResult(intent, REQUEST_CODE_VR)
            }
            "isQuestDevice" -> {
                val isQuest = Build.MANUFACTURER.contains("Oculus", ignoreCase = true) ||
                        Build.BRAND.contains("oculus", ignoreCase = true) ||
                        Build.MODEL.contains("Quest", ignoreCase = true)
                result.success(isQuest)
            }
            else -> {
                result.notImplemented()
            }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (requestCode == REQUEST_CODE_VR) {
            val lastPos = data?.getLongExtra("last_position", 0L) ?: 0L
            val lastBvid = data?.getStringExtra("last_bvid")
            val map = HashMap<String, Any?>()
            map["lastPosition"] = lastPos
            map["lastBvid"] = lastBvid
            pendingResult?.success(map)
            pendingResult = null
            return true
        }
        return false
    }
}
