package com.example.piliplus.vr

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.*
import android.graphics.drawable.GradientDrawable
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.net.Uri
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.TextUtils
import android.util.LruCache
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.Surface
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.*
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.*
import java.util.concurrent.Executors

class VrPlayerActivity : Activity(), SensorEventListener {

    private lateinit var glSurfaceView: GLSurfaceView
    private lateinit var renderer: VrRenderer

    private var videoMediaPlayer: MediaPlayer? = null
    private var audioMediaPlayer: MediaPlayer? = null
    private var surface: Surface? = null

    private lateinit var sensorManager: SensorManager
    private var rotationSensor: Sensor? = null

    private var initialYaw = 0f
    private var hasCalibratedInitialYaw = false
    private val rawRotationMatrix = FloatArray(16)
    private val sensorMatrix = FloatArray(16)

    // Touch dragging fallback
    private var lastTouchX = 0f
    private var lastTouchY = 0f

    // Current video state
    private var currentBvid: String = ""
    private var currentAid: Long = 0L
    private var currentCid: Long = 0L
    private var currentTitle: String = ""
    private var currentDesc: String = ""
    private var currentOwnerName: String = ""
    private var currentOwnerFace: String = ""
    private var currentViewCount: String = ""
    private var currentLikeCount: String = ""
    private var cookieString: String = ""

    // UI Overlay elements
    private lateinit var overlayContainer: FrameLayout
    private lateinit var topBarLayout: LinearLayout
    private lateinit var controlsLayout: LinearLayout
    private lateinit var titleTextView: TextView
    private lateinit var playPauseBtn: Button
    private lateinit var timeTextView: TextView
    private lateinit var seekBar: SeekBar
    private lateinit var loadingIndicator: ProgressBar

    // Left Window (Title, Uploader, Description, Comments)
    private lateinit var leftWindowLayout: FrameLayout
    private lateinit var leftScrollView: ScrollView
    private lateinit var leftContentLayout: LinearLayout
    private lateinit var videoTitleView: TextView
    private lateinit var ownerAvatarView: ImageView
    private lateinit var ownerNameView: TextView
    private lateinit var videoStatsView: TextView
    private lateinit var descTextView: TextView
    private lateinit var commentCountView: TextView
    private lateinit var commentsContainer: LinearLayout
    private lateinit var loadMoreCommentsBtn: Button
    private lateinit var commentsProgressBar: ProgressBar
    private var isLeftWindowVisible = true
    private var commentPage = 1
    private var isLoadingComments = false

    // Right Window (Related Videos)
    private lateinit var rightWindowLayout: FrameLayout
    private lateinit var rightScrollView: ScrollView
    private lateinit var relatedContainer: LinearLayout
    private lateinit var relatedProgressBar: ProgressBar
    private var isRightWindowVisible = true
    private var isLoadingRelated = false

    // Control toggles
    private lateinit var toggleLeftBtn: Button
    private lateinit var toggleRightBtn: Button

    private val handler = Handler(Looper.getMainLooper())
    private var isControlsVisible = true
    private var isTrackingTouch = false

    private val executor = Executors.newFixedThreadPool(4)
    private val imageCache = object : LruCache<String, Bitmap>(30 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap): Int {
            return value.byteCount
        }
    }

    private val hideControlsRunnable = Runnable {
        hideControls()
    }

    private val updateProgressRunnable = object : Runnable {
        override fun run() {
            val player = videoMediaPlayer
            if (player != null && player.isPlaying && !isTrackingTouch) {
                val current = player.currentPosition
                val total = player.duration
                if (total > 0) {
                    seekBar.max = total
                    seekBar.progress = current
                    timeTextView.text = "${formatTime(current)} / ${formatTime(total)}"
                }
            }
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Fullscreen & Keep Screen On for VR headset
        window.addFlags(
            WindowManager.LayoutParams.FLAG_FULLSCREEN or
                    WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
        )

        val rootLayout = FrameLayout(this).apply {
            setBackgroundColor(Color.BLACK)
        }

        // Initialize OpenGL Surface View
        renderer = VrRenderer { surfaceTexture ->
            runOnUiThread {
                onSurfaceTextureReady(surfaceTexture)
            }
        }

        glSurfaceView = GLSurfaceView(this).apply {
            setEGLContextClientVersion(2)
            setRenderer(renderer)
            renderMode = GLSurfaceView.RENDERMODE_CONTINUOUSLY
        }
        rootLayout.addView(glSurfaceView, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)

        // Parse intent extras
        currentBvid = intent.getStringExtra("bvid") ?: ""
        currentAid = intent.getLongExtra("aid", 0L)
        currentCid = intent.getLongExtra("cid", 0L)
        currentTitle = intent.getStringExtra("video_title") ?: "Bilibili VR 视频"
        currentDesc = intent.getStringExtra("desc") ?: ""
        currentOwnerName = intent.getStringExtra("owner_name") ?: ""
        currentOwnerFace = intent.getStringExtra("owner_face") ?: ""
        currentViewCount = intent.getStringExtra("view_count") ?: ""
        currentLikeCount = intent.getStringExtra("like_count") ?: ""
        cookieString = intent.getStringExtra("cookie") ?: ""

        val initialModeStr = intent.getStringExtra("vr_mode") ?: "cinema"
        renderer.currentMode = when (initialModeStr.lowercase()) {
            "sphere_360", "360" -> ProjectionMode.SPHERE_360
            "dome_180", "180" -> ProjectionMode.DOME_180
            "sbs_3d", "3d_sbs" -> ProjectionMode.SBS_3D
            "ou_3d", "3d_ou", "top_bottom" -> ProjectionMode.TOP_BOTTOM_3D
            else -> ProjectionMode.CINEMA_2D
        }

        // Initialize Sensors for head tracking
        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        rotationSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
            ?: sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)

        // Build YouTube VR style Spatial Overlay (Left Info/Comments + Right Related + Center Controls)
        buildUiOverlay(rootLayout)
        setContentView(rootLayout)

        scheduleHideControls(6000)

        // Initial fetch for comments and related videos
        if (currentAid > 0) {
            fetchComments(currentAid, 1, append = false)
        }
        if (currentBvid.isNotEmpty()) {
            fetchRelatedVideos(currentBvid)
        }
    }

    private fun buildUiOverlay(root: FrameLayout) {
        overlayContainer = FrameLayout(this)

        // 1. Top Bar
        topBarLayout = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(20), dp(12), dp(20), dp(12))
            background = GradientDrawable(
                GradientDrawable.Orientation.TOP_BOTTOM,
                intArrayOf(Color.argb(210, 10, 10, 15), Color.TRANSPARENT)
            )
            isClickable = true
        }

        val backBtn = Button(this).apply {
            text = "◀ 返回"
            setTextColor(Color.WHITE)
            textSize = 13f
            background = createRoundButtonBg(Color.argb(160, 40, 40, 48))
            setPadding(dp(16), dp(6), dp(16), dp(6))
            setOnClickListener { finishWithResult() }
        }
        topBarLayout.addView(backBtn)

        titleTextView = TextView(this).apply {
            text = currentTitle
            setTextColor(Color.WHITE)
            textSize = 15f
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            setPadding(dp(16), 0, dp(16), 0)
        }
        topBarLayout.addView(titleTextView, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        toggleLeftBtn = Button(this).apply {
            text = "📖 简介与评论"
            setTextColor(Color.WHITE)
            textSize = 12f
            background = createRoundButtonBg(Color.argb(180, 251, 114, 153))
            setPadding(dp(14), dp(6), dp(14), dp(6))
            setOnClickListener {
                isLeftWindowVisible = !isLeftWindowVisible
                leftWindowLayout.visibility = if (isLeftWindowVisible) View.VISIBLE else View.GONE
                background = createRoundButtonBg(
                    if (isLeftWindowVisible) Color.argb(180, 251, 114, 153) else Color.argb(120, 50, 50, 55)
                )
                cancelHideControls()
            }
        }
        val toggleLeftLp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            marginEnd = dp(8)
        }
        topBarLayout.addView(toggleLeftBtn, toggleLeftLp)

        toggleRightBtn = Button(this).apply {
            text = "📺 相关视频"
            setTextColor(Color.WHITE)
            textSize = 12f
            background = createRoundButtonBg(Color.argb(180, 251, 114, 153))
            setPadding(dp(14), dp(6), dp(14), dp(6))
            setOnClickListener {
                isRightWindowVisible = !isRightWindowVisible
                rightWindowLayout.visibility = if (isRightWindowVisible) View.VISIBLE else View.GONE
                background = createRoundButtonBg(
                    if (isRightWindowVisible) Color.argb(180, 251, 114, 153) else Color.argb(120, 50, 50, 55)
                )
                cancelHideControls()
            }
        }
        topBarLayout.addView(toggleRightBtn)

        val topParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply { gravity = Gravity.TOP }
        overlayContainer.addView(topBarLayout, topParams)

        // 2. Left Window (Video Info & Comments)
        buildLeftWindow()
        val leftParams = FrameLayout.LayoutParams(dp(380), FrameLayout.LayoutParams.MATCH_PARENT).apply {
            gravity = Gravity.START or Gravity.TOP
            marginStart = dp(16)
            topMargin = dp(60)
            bottomMargin = dp(80)
        }
        overlayContainer.addView(leftWindowLayout, leftParams)

        // 3. Right Window (Related Videos)
        buildRightWindow()
        val rightParams = FrameLayout.LayoutParams(dp(350), FrameLayout.LayoutParams.MATCH_PARENT).apply {
            gravity = Gravity.END or Gravity.TOP
            marginEnd = dp(16)
            topMargin = dp(60)
            bottomMargin = dp(80)
        }
        overlayContainer.addView(rightWindowLayout, rightParams)

        // 4. Loading Indicator in center
        loadingIndicator = ProgressBar(this).apply {
            isIndeterminate = true
            visibility = View.VISIBLE
        }
        val centerParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply { gravity = Gravity.CENTER }
        overlayContainer.addView(loadingIndicator, centerParams)

        // 5. Bottom Controls
        buildBottomControls()
        val bottomParams = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.WRAP_CONTENT
        ).apply { gravity = Gravity.BOTTOM }
        overlayContainer.addView(controlsLayout, bottomParams)

        root.addView(overlayContainer, FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT)
    }

    // Build Left Window (Video Description, Uploader, and scrolling down into Comments)
    private fun buildLeftWindow() {
        leftWindowLayout = FrameLayout(this).apply {
            background = createCardBg()
            isClickable = true
        }

        leftScrollView = ScrollView(this).apply {
            isVerticalScrollBarEnabled = true
            setOnTouchListener { _, _ ->
                cancelHideControls()
                false
            }
        }

        leftContentLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }

        // Title
        videoTitleView = TextView(this).apply {
            text = currentTitle
            setTextColor(Color.WHITE)
            textSize = 15f
            setTypeface(null, Typeface.BOLD)
            setTextIsSelectable(true)
        }
        leftContentLayout.addView(videoTitleView)

        // Uploader & Stats Row
        val ownerRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(12), 0, dp(10))
        }

        ownerAvatarView = ImageView(this).apply {
            scaleType = ImageView.ScaleType.CENTER_CROP
        }
        val avatarLp = LinearLayout.LayoutParams(dp(36), dp(36)).apply {
            marginEnd = dp(10)
        }
        ownerRow.addView(ownerAvatarView, avatarLp)
        if (currentOwnerFace.isNotEmpty()) {
            loadImage(currentOwnerFace, ownerAvatarView, isCircle = true)
        }

        val ownerCol = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        ownerNameView = TextView(this).apply {
            text = if (currentOwnerName.isNotEmpty()) currentOwnerName else "哔哩哔哩 UP主"
            setTextColor(Color.WHITE)
            textSize = 13f
            setTypeface(null, Typeface.BOLD)
        }
        ownerCol.addView(ownerNameView)

        videoStatsView = TextView(this).apply {
            text = "${if (currentViewCount.isNotEmpty()) "$currentViewCount 播放" else ""}  ${if (currentLikeCount.isNotEmpty()) "· $currentLikeCount 点赞" else ""}"
            setTextColor(Color.parseColor("#9E9E9E"))
            textSize = 11f
        }
        ownerCol.addView(videoStatsView)

        ownerRow.addView(ownerCol)
        leftContentLayout.addView(ownerRow)

        // Description
        descTextView = TextView(this).apply {
            text = if (currentDesc.isNotEmpty()) currentDesc else "暂无视频简介"
            setTextColor(Color.parseColor("#CCCCCC"))
            textSize = 12f
            setLineSpacing(0f, 1.25f)
            setTextIsSelectable(true)
        }
        leftContentLayout.addView(descTextView)

        // Divider
        val divider = View(this).apply {
            setBackgroundColor(Color.argb(40, 255, 255, 255))
        }
        val divLp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1)).apply {
            topMargin = dp(14)
            bottomMargin = dp(14)
        }
        leftContentLayout.addView(divider, divLp)

        // Comments Section Header
        val commentHeader = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        val commentTitle = TextView(this).apply {
            text = "💬 评论区"
            setTextColor(Color.WHITE)
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
        }
        commentHeader.addView(commentTitle)

        commentCountView = TextView(this).apply {
            text = ""
            setTextColor(Color.parseColor("#888888"))
            textSize = 11f
            setPadding(dp(6), 0, 0, 0)
        }
        commentHeader.addView(commentCountView, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        val refreshCommentsBtn = TextView(this).apply {
            text = "🔄 刷新"
            setTextColor(Color.parseColor("#FB7299"))
            textSize = 12f
            setPadding(dp(8), dp(4), dp(8), dp(4))
            setOnClickListener {
                if (currentAid > 0) {
                    commentPage = 1
                    fetchComments(currentAid, 1, append = false)
                }
            }
        }
        commentHeader.addView(refreshCommentsBtn)
        leftContentLayout.addView(commentHeader)

        // Comments Loading Indicator
        commentsProgressBar = ProgressBar(this).apply {
            isIndeterminate = true
            visibility = View.VISIBLE
        }
        val pbLp = LinearLayout.LayoutParams(dp(28), dp(28)).apply {
            gravity = Gravity.CENTER_HORIZONTAL
            topMargin = dp(16)
            bottomMargin = dp(16)
        }
        leftContentLayout.addView(commentsProgressBar, pbLp)

        // Comments Items Container
        commentsContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        leftContentLayout.addView(commentsContainer)

        // Load More Comments Button
        loadMoreCommentsBtn = Button(this).apply {
            text = "加载更多评论"
            setTextColor(Color.parseColor("#FB7299"))
            textSize = 12f
            background = createRoundButtonBg(Color.argb(80, 251, 114, 153))
            visibility = View.GONE
            setOnClickListener {
                if (!isLoadingComments && currentAid > 0) {
                    commentPage++
                    fetchComments(currentAid, commentPage, append = true)
                }
            }
        }
        val lmParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(10)
            bottomMargin = dp(10)
        }
        leftContentLayout.addView(loadMoreCommentsBtn, lmParams)

        leftScrollView.addView(leftContentLayout)
        leftWindowLayout.addView(leftScrollView)
    }

    // Build Right Window (Related Videos)
    private fun buildRightWindow() {
        rightWindowLayout = FrameLayout(this).apply {
            background = createCardBg()
            isClickable = true
        }

        val rightRoot = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(14), dp(14), dp(14))
        }

        // Header
        val rightHeader = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, 0, 0, dp(10))
        }

        val relatedTitle = TextView(this).apply {
            text = "📺 相关推荐"
            setTextColor(Color.WHITE)
            textSize = 14f
            setTypeface(null, Typeface.BOLD)
        }
        rightHeader.addView(relatedTitle, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))

        val refreshRelatedBtn = TextView(this).apply {
            text = "🔄 换一换"
            setTextColor(Color.parseColor("#FB7299"))
            textSize = 12f
            setPadding(dp(8), dp(4), dp(8), dp(4))
            setOnClickListener {
                if (currentBvid.isNotEmpty()) {
                    fetchRelatedVideos(currentBvid)
                }
            }
        }
        rightHeader.addView(refreshRelatedBtn)
        rightRoot.addView(rightHeader)

        // Related Progress
        relatedProgressBar = ProgressBar(this).apply {
            isIndeterminate = true
            visibility = View.VISIBLE
        }
        val pbLp = LinearLayout.LayoutParams(dp(28), dp(28)).apply {
            gravity = Gravity.CENTER_HORIZONTAL
            topMargin = dp(20)
            bottomMargin = dp(20)
        }
        rightRoot.addView(relatedProgressBar, pbLp)

        // Scrollable List
        rightScrollView = ScrollView(this).apply {
            isVerticalScrollBarEnabled = true
            setOnTouchListener { _, _ ->
                cancelHideControls()
                false
            }
        }

        relatedContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        rightScrollView.addView(relatedContainer)
        rightRoot.addView(rightScrollView, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT))

        rightWindowLayout.addView(rightRoot)
    }

    // Bottom Playback Controls
    private fun buildBottomControls() {
        controlsLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(12), dp(20), dp(16))
            background = GradientDrawable(
                GradientDrawable.Orientation.BOTTOM_TOP,
                intArrayOf(Color.argb(230, 10, 10, 15), Color.TRANSPARENT)
            )
            isClickable = true
        }

        // Progress Bar & Time Row
        val progressRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        timeTextView = TextView(this).apply {
            text = "00:00 / 00:00"
            setTextColor(Color.LTGRAY)
            textSize = 12f
        }

        seekBar = SeekBar(this).apply {
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                    if (fromUser) {
                        timeTextView.text = "${formatTime(progress)} / ${formatTime(sb?.max ?: 0)}"
                    }
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {
                    isTrackingTouch = true
                    cancelHideControls()
                }
                override fun onStopTrackingTouch(sb: SeekBar?) {
                    isTrackingTouch = false
                    val targetMs = sb?.progress ?: 0
                    videoMediaPlayer?.seekTo(targetMs)
                    audioMediaPlayer?.seekTo(targetMs)
                    scheduleHideControls(5000)
                }
            })
        }

        val seekParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        progressRow.addView(seekBar, seekParams)
        progressRow.addView(timeTextView)
        controlsLayout.addView(progressRow)

        // Button Controls Row
        val btnRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(8), 0, 0)
        }

        playPauseBtn = Button(this).apply {
            text = "⏸ 暂停"
            setTextColor(Color.WHITE)
            textSize = 13f
            background = createRoundButtonBg(Color.argb(180, 251, 114, 153))
            setPadding(dp(16), dp(6), dp(16), dp(6))
            setOnClickListener {
                togglePlayPause()
                scheduleHideControls(5000)
            }
        }
        btnRow.addView(playPauseBtn)

        val spacer = View(this)
        btnRow.addView(spacer, LinearLayout.LayoutParams(0, 1, 1f))

        // Mode switch buttons
        val modeCinemaBtn = createModeBtn("🎬 巨幕", ProjectionMode.CINEMA_2D)
        val mode360Btn = createModeBtn("🌐 360°", ProjectionMode.SPHERE_360)
        val mode180Btn = createModeBtn("🥽 180°VR", ProjectionMode.DOME_180)
        val modeSbsBtn = createModeBtn("👓 3D左右", ProjectionMode.SBS_3D)
        val modeOuBtn = createModeBtn("👓 3D上下", ProjectionMode.TOP_BOTTOM_3D)

        btnRow.addView(modeCinemaBtn)
        btnRow.addView(mode360Btn)
        btnRow.addView(mode180Btn)
        btnRow.addView(modeSbsBtn)
        btnRow.addView(modeOuBtn)

        // Recenter Button
        val recenterBtn = Button(this).apply {
            text = "🎯 置中"
            setTextColor(Color.WHITE)
            textSize = 12f
            background = createRoundButtonBg(Color.argb(140, 60, 60, 68))
            setPadding(dp(12), dp(6), dp(12), dp(6))
            setOnClickListener {
                recenterView()
                scheduleHideControls(5000)
            }
        }
        btnRow.addView(recenterBtn)

        controlsLayout.addView(btnRow)
    }

    private fun createCardBg(): GradientDrawable {
        return GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(18).toFloat()
            setColor(Color.argb(225, 20, 20, 26)) // Dark translucent glassmorphism
            setStroke(dp(1), Color.argb(45, 255, 255, 255))
        }
    }

    private fun createRoundButtonBg(color: Int): GradientDrawable {
        return GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(16).toFloat()
            setColor(color)
        }
    }

    private fun createModeBtn(label: String, mode: ProjectionMode): Button {
        return Button(this).apply {
            text = label
            setTextColor(Color.WHITE)
            textSize = 12f
            background = createRoundButtonBg(
                if (renderer.currentMode == mode) Color.argb(180, 251, 114, 153)
                else Color.argb(120, 50, 50, 55)
            )
            setPadding(dp(10), dp(6), dp(10), dp(6))
            val lp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                marginEnd = dp(6)
            }
            layoutParams = lp
            setOnClickListener {
                renderer.currentMode = mode
                scheduleHideControls(5000)
            }
        }
    }

    private fun dp(value: Int): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            value.toFloat(),
            resources.displayMetrics
        ).toInt()
    }

    private fun recenterView() {
        renderer.touchPitch = 0f
        renderer.touchYaw = 0f
        hasCalibratedInitialYaw = false
        Toast.makeText(this, "视角已置中", Toast.LENGTH_SHORT).show()
    }

    private fun togglePlayPause() {
        val player = videoMediaPlayer ?: return
        if (player.isPlaying) {
            player.pause()
            audioMediaPlayer?.pause()
            playPauseBtn.text = "▶ 播放"
        } else {
            player.start()
            audioMediaPlayer?.start()
            playPauseBtn.text = "⏸ 暂停"
        }
    }

    // Toggle controls & windows visibility on screen tap
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.action) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = event.x
                lastTouchY = event.y
                toggleControls()
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - lastTouchX
                val dy = event.y - lastTouchY
                // Drag to rotate view
                renderer.touchYaw += dx * 0.15f
                renderer.touchPitch += dy * 0.15f
                lastTouchX = event.x
                lastTouchY = event.y
            }
        }
        return true
    }

    private fun toggleControls() {
        if (isControlsVisible) {
            hideControls()
        } else {
            showControls()
            scheduleHideControls(6000)
        }
    }

    private fun showControls() {
        isControlsVisible = true
        topBarLayout.visibility = View.VISIBLE
        controlsLayout.visibility = View.VISIBLE
        if (isLeftWindowVisible) leftWindowLayout.visibility = View.VISIBLE
        if (isRightWindowVisible) rightWindowLayout.visibility = View.VISIBLE
    }

    private fun hideControls() {
        isControlsVisible = false
        topBarLayout.visibility = View.GONE
        controlsLayout.visibility = View.GONE
        leftWindowLayout.visibility = View.GONE
        rightWindowLayout.visibility = View.GONE
    }

    private fun scheduleHideControls(delayMs: Long = 6000) {
        cancelHideControls()
        handler.postDelayed(hideControlsRunnable, delayMs)
    }

    private fun cancelHideControls() {
        handler.removeCallbacks(hideControlsRunnable)
    }

    // ==================== Bilibili Data Fetching & UI Rendering ====================

    // Fetch related videos
    private fun fetchRelatedVideos(bvid: String) {
        isLoadingRelated = true
        relatedProgressBar.visibility = View.VISIBLE
        val url = "https://api.bilibili.com/x/web-interface/archive/related?bvid=$bvid"
        executor.execute {
            try {
                val json = httpGetJson(url)
                val code = json.optInt("code", -1)
                if (code == 0) {
                    val dataArr = json.optJSONArray("data")
                    val list = ArrayList<RelatedVideoItem>()
                    if (dataArr != null) {
                        for (i in 0 until dataArr.length()) {
                            val item = dataArr.optJSONObject(i) ?: continue
                            val rBvid = item.optString("bvid")
                            val rAid = item.optLong("aid")
                            val rCid = item.optLong("cid")
                            val rTitle = item.optString("title")
                            val rPic = item.optString("pic")
                            val rDuration = item.optInt("duration")
                            val ownerObj = item.optJSONObject("owner")
                            val rOwnerName = ownerObj?.optString("name") ?: ""
                            val statObj = item.optJSONObject("stat")
                            val rView = statObj?.optLong("view") ?: 0L
                            val rDanmaku = statObj?.optLong("danmaku") ?: 0L

                            list.add(
                                RelatedVideoItem(
                                    bvid = rBvid,
                                    aid = rAid,
                                    cid = rCid,
                                    title = rTitle,
                                    pic = rPic,
                                    duration = rDuration,
                                    ownerName = rOwnerName,
                                    viewCount = rView,
                                    danmakuCount = rDanmaku
                                )
                            )
                        }
                    }
                    runOnUiThread {
                        isLoadingRelated = false
                        relatedProgressBar.visibility = View.GONE
                        displayRelatedVideos(list)
                    }
                } else {
                    runOnUiThread {
                        isLoadingRelated = false
                        relatedProgressBar.visibility = View.GONE
                    }
                }
            } catch (e: Exception) {
                e.printStackTrace()
                runOnUiThread {
                    isLoadingRelated = false
                    relatedProgressBar.visibility = View.GONE
                }
            }
        }
    }

    private fun displayRelatedVideos(list: List<RelatedVideoItem>) {
        relatedContainer.removeAllViews()
        for (item in list) {
            val card = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(8), dp(8), dp(8), dp(8))
                background = createRoundButtonBg(Color.argb(70, 40, 40, 48))
                isClickable = true
                setOnClickListener {
                    playRelatedVideo(item)
                }
            }
            val cardLp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dp(8)
            }

            // Thumbnail container
            val thumbFrame = FrameLayout(this)
            val thumbView = ImageView(this).apply {
                scaleType = ImageView.ScaleType.CENTER_CROP
            }
            val thumbLp = FrameLayout.LayoutParams(dp(110), dp(64))
            thumbFrame.addView(thumbView, thumbLp)

            val durationView = TextView(this).apply {
                text = formatDuration(item.duration)
                setTextColor(Color.WHITE)
                textSize = 9f
                setPadding(dp(4), dp(2), dp(4), dp(2))
                background = createRoundButtonBg(Color.argb(180, 0, 0, 0))
            }
            val durLp = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.BOTTOM or Gravity.END
                marginEnd = dp(4)
                bottomMargin = dp(4)
            }
            thumbFrame.addView(durationView, durLp)

            val cardThumbLp = LinearLayout.LayoutParams(dp(110), dp(64)).apply {
                marginEnd = dp(10)
            }
            card.addView(thumbFrame, cardThumbLp)

            if (item.pic.isNotEmpty()) {
                loadImage(item.pic, thumbView, cornerRadiusPx = dp(8).toFloat())
            }

            // Info Column
            val infoCol = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
            }

            val titleView = TextView(this).apply {
                text = item.title
                setTextColor(Color.WHITE)
                textSize = 12f
                maxLines = 2
                ellipsize = TextUtils.TruncateAt.END
                setTypeface(null, Typeface.BOLD)
            }
            infoCol.addView(titleView)

            val upView = TextView(this).apply {
                text = item.ownerName
                setTextColor(Color.parseColor("#AAAAAA"))
                textSize = 10.5f
                setPadding(0, dp(2), 0, dp(1))
            }
            infoCol.addView(upView)

            val statView = TextView(this).apply {
                text = "▶ ${formatCount(item.viewCount)}   💬 ${formatCount(item.danmakuCount)}"
                setTextColor(Color.parseColor("#888888"))
                textSize = 10f
            }
            infoCol.addView(statView)

            card.addView(infoCol, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            relatedContainer.addView(card, cardLp)
        }
    }

    // Fetch video comments
    private fun fetchComments(aid: Long, page: Int, append: Boolean) {
        isLoadingComments = true
        commentsProgressBar.visibility = View.VISIBLE
        val url = "https://api.bilibili.com/x/v2/reply?oid=$aid&type=1&pn=$page&ps=20&sort=2"
        executor.execute {
            try {
                val json = httpGetJson(url)
                val code = json.optInt("code", -1)
                if (code == 0) {
                    val dataObj = json.optJSONObject("data")
                    val repliesArr = dataObj?.optJSONArray("replies")
                    val totalCount = dataObj?.optJSONObject("page")?.optInt("count") ?: 0
                    val commentList = ArrayList<CommentItem>()

                    if (repliesArr != null) {
                        for (i in 0 until repliesArr.length()) {
                            val r = repliesArr.optJSONObject(i) ?: continue
                            val member = r.optJSONObject("member")
                            val uname = member?.optString("uname") ?: "用户"
                            val avatar = member?.optString("avatar") ?: ""
                            val level = member?.optJSONObject("level_info")?.optInt("current_level") ?: 0
                            val message = r.optJSONObject("content")?.optString("message") ?: ""
                            val like = r.optLong("like")
                            val ctime = r.optLong("ctime")

                            val subReplies = ArrayList<Pair<String, String>>()
                            val subArr = r.optJSONArray("replies")
                            if (subArr != null) {
                                for (j in 0 until Math.min(subArr.length(), 2)) {
                                    val sub = subArr.optJSONObject(j) ?: continue
                                    val subUname = sub.optJSONObject("member")?.optString("uname") ?: ""
                                    val subMsg = sub.optJSONObject("content")?.optString("message") ?: ""
                                    if (subUname.isNotEmpty() && subMsg.isNotEmpty()) {
                                        subReplies.add(Pair(subUname, subMsg))
                                    }
                                }
                            }
                            commentList.add(CommentItem(uname, avatar, level, message, like, ctime, subReplies))
                        }
                    }
                    runOnUiThread {
                        isLoadingComments = false
                        commentsProgressBar.visibility = View.GONE
                        displayComments(commentList, totalCount, append)
                    }
                } else {
                    runOnUiThread {
                        isLoadingComments = false
                        commentsProgressBar.visibility = View.GONE
                    }
                }
            } catch (e: Exception) {
                e.printStackTrace()
                runOnUiThread {
                    isLoadingComments = false
                    commentsProgressBar.visibility = View.GONE
                }
            }
        }
    }

    private fun displayComments(list: List<CommentItem>, totalCount: Int, append: Boolean) {
        if (!append) {
            commentsContainer.removeAllViews()
            commentCountView.text = if (totalCount > 0) "($totalCount)" else ""
        }

        for (item in list) {
            val itemLayout = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(0, dp(10), 0, dp(10))
            }

            // Avatar
            val avatarView = ImageView(this).apply {
                scaleType = ImageView.ScaleType.CENTER_CROP
            }
            val avLp = LinearLayout.LayoutParams(dp(32), dp(32)).apply {
                marginEnd = dp(10)
                topMargin = dp(2)
            }
            itemLayout.addView(avatarView, avLp)
            if (item.avatar.isNotEmpty()) {
                loadImage(item.avatar, avatarView, isCircle = true)
            }

            // Body
            val bodyCol = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
            }

            // User info row (Name, Level, Time, Likes)
            val nameRow = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }

            val nameView = TextView(this).apply {
                text = item.uname
                setTextColor(Color.parseColor("#FB7299"))
                textSize = 12f
                setTypeface(null, Typeface.BOLD)
            }
            nameRow.addView(nameView)

            val levelView = TextView(this).apply {
                text = " LV${item.level} "
                setTextColor(Color.WHITE)
                textSize = 9f
                background = createRoundButtonBg(Color.parseColor("#E06A3B"))
                setPadding(dp(3), 0, dp(3), 0)
            }
            val lvlLp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            ).apply {
                marginStart = dp(6)
            }
            nameRow.addView(levelView, lvlLp)

            val spacer = View(this)
            nameRow.addView(spacer, LinearLayout.LayoutParams(0, 1, 1f))

            val timeView = TextView(this).apply {
                text = formatCtime(item.ctime)
                setTextColor(Color.parseColor("#777777"))
                textSize = 10f
            }
            nameRow.addView(timeView)

            val likeView = TextView(this).apply {
                text = "  👍 ${formatCount(item.like)}"
                setTextColor(Color.parseColor("#888888"))
                textSize = 10f
            }
            nameRow.addView(likeView)

            bodyCol.addView(nameRow)

            // Message Content
            val msgView = TextView(this).apply {
                text = item.message
                setTextColor(Color.WHITE)
                textSize = 12.5f
                setLineSpacing(0f, 1.2f)
                setPadding(0, dp(4), 0, 0)
                setTextIsSelectable(true)
            }
            bodyCol.addView(msgView)

            // Sub-replies Preview
            if (item.subReplies.isNotEmpty()) {
                val subBox = LinearLayout(this).apply {
                    orientation = LinearLayout.VERTICAL
                    background = createRoundButtonBg(Color.argb(90, 35, 35, 42))
                    setPadding(dp(8), dp(6), dp(8), dp(6))
                }
                val sbLp = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply {
                    topMargin = dp(6)
                }

                for (sub in item.subReplies) {
                    val subText = TextView(this).apply {
                        text = "${sub.first}: ${sub.second}"
                        setTextColor(Color.parseColor("#CCCCCC"))
                        textSize = 11f
                        maxLines = 2
                        ellipsize = TextUtils.TruncateAt.END
                        setPadding(0, dp(1), 0, dp(1))
                    }
                    subBox.addView(subText)
                }
                bodyCol.addView(subBox, sbLp)
            }

            // Divider under each comment
            val itemDivider = View(this).apply {
                setBackgroundColor(Color.argb(20, 255, 255, 255))
            }
            val idLp = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(1)).apply {
                topMargin = dp(10)
            }

            bodyCol.addView(itemDivider, idLp)

            itemLayout.addView(bodyCol, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            commentsContainer.addView(itemLayout)
        }

        loadMoreCommentsBtn.visibility = if (list.isNotEmpty()) View.VISIBLE else View.GONE
    }

    // Switch video when a related video is clicked inside VR
    private fun playRelatedVideo(item: RelatedVideoItem) {
        loadingIndicator.visibility = View.VISIBLE
        currentBvid = item.bvid
        currentAid = item.aid
        currentTitle = item.title

        titleTextView.text = item.title
        videoTitleView.text = item.title
        ownerNameView.text = item.ownerName
        videoStatsView.text = "${formatCount(item.viewCount)} 播放"
        descTextView.text = "正在加载视频简介..."
        commentsContainer.removeAllViews()

        executor.execute {
            try {
                // 1. Fetch view info to get cid, desc, owner face
                val viewUrl = "https://api.bilibili.com/x/web-interface/view?bvid=${item.bvid}"
                val viewJson = httpGetJson(viewUrl)
                val viewData = viewJson.optJSONObject("data")
                val cid = viewData?.optLong("cid") ?: item.cid
                currentCid = cid
                val desc = viewData?.optString("desc") ?: ""
                val ownerObj = viewData?.optJSONObject("owner")
                val ownerName = ownerObj?.optString("name") ?: item.ownerName
                val ownerFace = ownerObj?.optString("face") ?: ""
                val statObj = viewData?.optJSONObject("stat")
                val viewCount = statObj?.optLong("view") ?: item.viewCount
                val likeCount = statObj?.optLong("like") ?: 0L

                // 2. Fetch playurl
                val playUrlApi = "https://api.bilibili.com/x/player/playurl?bvid=${item.bvid}&cid=$cid&qn=64&platform=html5"
                val playJson = httpGetJson(playUrlApi)
                val durlArr = playJson.optJSONObject("data")?.optJSONArray("durl")
                val videoUrl = durlArr?.optJSONObject(0)?.optString("url")

                if (!videoUrl.isNullOrEmpty()) {
                    runOnUiThread {
                        currentDesc = desc
                        currentOwnerName = ownerName
                        currentOwnerFace = ownerFace

                        videoTitleView.text = item.title
                        ownerNameView.text = ownerName
                        videoStatsView.text = "${formatCount(viewCount)} 播放 · ${formatCount(likeCount)} 点赞"
                        descTextView.text = if (desc.isNotEmpty()) desc else "暂无简介"
                        if (ownerFace.isNotEmpty()) {
                            loadImage(ownerFace, ownerAvatarView, isCircle = true)
                        }

                        // Switch stream
                        switchStream(videoUrl)

                        // Load comments
                        commentPage = 1
                        fetchComments(item.aid, 1, append = false)

                        // Load related videos for new video
                        fetchRelatedVideos(item.bvid)
                    }
                } else {
                    runOnUiThread {
                        loadingIndicator.visibility = View.GONE
                        Toast.makeText(this@VrPlayerActivity, "无法获取视频流", Toast.LENGTH_SHORT).show()
                    }
                }
            } catch (e: Exception) {
                e.printStackTrace()
                runOnUiThread {
                    loadingIndicator.visibility = View.GONE
                    Toast.makeText(this@VrPlayerActivity, "加载视频失败: ${e.message}", Toast.LENGTH_SHORT).show()
                }
            }
        }
    }

    private fun switchStream(videoUrl: String) {
        val surf = surface ?: return
        val headers = HashMap<String, String>().apply {
            put("Referer", "https://www.bilibili.com")
            put("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
        }

        try {
            audioMediaPlayer?.release()
            audioMediaPlayer = null

            videoMediaPlayer?.release()
            videoMediaPlayer = MediaPlayer().apply {
                setSurface(surf)
                setDataSource(this@VrPlayerActivity, Uri.parse(videoUrl), headers)
                setAudioAttributes(
                    AudioAttributes.Builder()
                        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .build()
                )
                setOnPreparedListener { mp ->
                    loadingIndicator.visibility = View.GONE
                    mp.start()
                    handler.post(updateProgressRunnable)
                }
                setOnBufferingUpdateListener { _, percent ->
                    seekBar.secondaryProgress = (seekBar.max * (percent / 100f)).toInt()
                }
                setOnCompletionListener {
                    finishWithResult()
                }
                setOnErrorListener { _, what, extra ->
                    Toast.makeText(this@VrPlayerActivity, "播放异常 ($what, $extra)", Toast.LENGTH_SHORT).show()
                    true
                }
                prepareAsync()
            }
        } catch (e: Exception) {
            e.printStackTrace()
            loadingIndicator.visibility = View.GONE
            Toast.makeText(this, "切换视频失败: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    private fun httpGetJson(urlStr: String): JSONObject {
        val url = URL(urlStr)
        val conn = url.openConnection() as HttpURLConnection
        conn.requestMethod = "GET"
        conn.connectTimeout = 8000
        conn.readTimeout = 8000
        conn.setRequestProperty("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
        conn.setRequestProperty("Referer", "https://www.bilibili.com")
        if (cookieString.isNotEmpty()) {
            conn.setRequestProperty("Cookie", cookieString)
        }

        val reader = BufferedReader(InputStreamReader(conn.inputStream))
        val sb = java.lang.StringBuilder()
        var line: String? = reader.readLine()
        while (line != null) {
            sb.append(line)
            line = reader.readLine()
        }
        reader.close()
        conn.disconnect()
        return JSONObject(sb.toString())
    }

    // High performance asynchronous image loader
    private fun loadImage(url: String, imageView: ImageView, isCircle: Boolean = false, cornerRadiusPx: Float = 0f) {
        val cacheKey = "$url-$isCircle-$cornerRadiusPx"
        val cached = imageCache.get(cacheKey)
        if (cached != null) {
            imageView.setImageBitmap(cached)
            return
        }

        imageView.tag = cacheKey
        executor.execute {
            try {
                val conn = URL(url).openConnection() as HttpURLConnection
                conn.setRequestProperty("User-Agent", "Mozilla/5.0")
                conn.setRequestProperty("Referer", "https://www.bilibili.com")
                conn.connectTimeout = 6000
                conn.readTimeout = 6000
                val stream = conn.inputStream
                var bitmap = BitmapFactory.decodeStream(stream)
                stream.close()
                conn.disconnect()

                if (bitmap != null) {
                    if (isCircle) {
                        bitmap = createCircularBitmap(bitmap)
                    } else if (cornerRadiusPx > 0f) {
                        bitmap = createRoundedBitmap(bitmap, cornerRadiusPx)
                    }

                    imageCache.put(cacheKey, bitmap)
                    runOnUiThread {
                        if (imageView.tag == cacheKey) {
                            imageView.setImageBitmap(bitmap)
                        }
                    }
                }
            } catch (_: Exception) {}
        }
    }

    private fun createCircularBitmap(src: Bitmap): Bitmap {
        val size = Math.min(src.width, src.height)
        val output = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(output)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        val rect = Rect((src.width - size) / 2, (src.height - size) / 2, (src.width + size) / 2, (src.height + size) / 2)
        val rectF = RectF(0f, 0f, size.toFloat(), size.toFloat())
        canvas.drawOval(rectF, paint)
        paint.xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC_IN)
        canvas.drawBitmap(src, rect, rectF, paint)
        return output
    }

    private fun createRoundedBitmap(src: Bitmap, cornerRadiusPx: Float): Bitmap {
        val output = Bitmap.createBitmap(src.width, src.height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(output)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        val rectF = RectF(0f, 0f, src.width.toFloat(), src.height.toFloat())
        canvas.drawRoundRect(rectF, cornerRadiusPx, cornerRadiusPx, paint)
        paint.xfermode = PorterDuffXfermode(PorterDuff.Mode.SRC_IN)
        canvas.drawBitmap(src, 0f, 0f, paint)
        return output
    }

    // ==================== Playback & Sensor Lifecycle ====================

    private fun onSurfaceTextureReady(st: SurfaceTexture) {
        surface = Surface(st)
        startPlayback()
    }

    private fun startPlayback() {
        val surf = surface ?: return
        val videoUrl = intent.getStringExtra("video_url") ?: return
        val audioUrl = intent.getStringExtra("audio_url")
        val startPos = intent.getLongExtra("start_position", 0L)

        val headers = HashMap<String, String>().apply {
            put("Referer", "https://www.bilibili.com")
            put("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")
        }

        try {
            videoMediaPlayer?.release()
            videoMediaPlayer = MediaPlayer().apply {
                setSurface(surf)
                setDataSource(this@VrPlayerActivity, Uri.parse(videoUrl), headers)
                if (audioUrl == null) {
                    setAudioAttributes(
                        AudioAttributes.Builder()
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .build()
                    )
                } else {
                    setVolume(0f, 0f)
                }
                setOnPreparedListener { mp ->
                    loadingIndicator.visibility = View.GONE
                    if (startPos > 0) mp.seekTo(startPos.toInt())
                    mp.start()
                    handler.post(updateProgressRunnable)
                }
                setOnBufferingUpdateListener { _, percent ->
                    seekBar.secondaryProgress = (seekBar.max * (percent / 100f)).toInt()
                }
                setOnCompletionListener {
                    finishWithResult()
                }
                setOnErrorListener { _, what, extra ->
                    Toast.makeText(this@VrPlayerActivity, "播放异常 ($what, $extra)", Toast.LENGTH_SHORT).show()
                    true
                }
                prepareAsync()
            }

            // Audio track for DASH
            if (!audioUrl.isNullOrEmpty() && audioUrl != videoUrl) {
                audioMediaPlayer?.release()
                audioMediaPlayer = MediaPlayer().apply {
                    setDataSource(this@VrPlayerActivity, Uri.parse(audioUrl), headers)
                    setAudioAttributes(
                        AudioAttributes.Builder()
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .build()
                    )
                    setOnPreparedListener { mp ->
                        if (startPos > 0) mp.seekTo(startPos.toInt())
                        mp.start()
                    }
                    prepareAsync()
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
            Toast.makeText(this, "视频初始化失败: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (event.sensor.type == Sensor.TYPE_GAME_ROTATION_VECTOR ||
            event.sensor.type == Sensor.TYPE_ROTATION_VECTOR
        ) {
            SensorManager.getRotationMatrixFromVector(rawRotationMatrix, event.values)
            SensorManager.remapCoordinateSystem(
                rawRotationMatrix,
                SensorManager.AXIS_X,
                SensorManager.AXIS_Z,
                sensorMatrix
            )

            if (!hasCalibratedInitialYaw) {
                hasCalibratedInitialYaw = true
                Matrix.setIdentityM(renderer.headRotationMatrix, 0)
            }

            System.arraycopy(sensorMatrix, 0, renderer.headRotationMatrix, 0, 16)
        }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    private fun finishWithResult() {
        val pos = videoMediaPlayer?.currentPosition?.toLong() ?: 0L
        val data = Intent().apply {
            putExtra("last_position", pos)
            putExtra("last_bvid", currentBvid)
        }
        setResult(RESULT_OK, data)
        finish()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        finishWithResult()
    }

    override fun onResume() {
        super.onResume()
        glSurfaceView.onResume()
        rotationSensor?.let {
            sensorManager.registerListener(this, it, SensorManager.SENSOR_DELAY_FASTEST)
        }
    }

    override fun onPause() {
        super.onPause()
        glSurfaceView.onPause()
        sensorManager.unregisterListener(this)
        videoMediaPlayer?.pause()
        audioMediaPlayer?.pause()
    }

    override fun onDestroy() {
        super.onDestroy()
        handler.removeCallbacksAndMessages(null)
        videoMediaPlayer?.release()
        audioMediaPlayer?.release()
        videoMediaPlayer = null
        audioMediaPlayer = null
        surface?.release()
        surface = null
        renderer.release()
        executor.shutdown()
    }

    // Format helpers
    private fun formatTime(ms: Int): String {
        val totalSec = ms / 1000
        val min = totalSec / 60
        val sec = totalSec % 60
        return String.format(Locale.getDefault(), "%02d:%02d", min, sec)
    }

    private fun formatDuration(sec: Int): String {
        val min = sec / 60
        val s = sec % 60
        return String.format(Locale.getDefault(), "%02d:%02d", min, s)
    }

    private fun formatCount(count: Long): String {
        return when {
            count >= 100_000_000 -> String.format(Locale.getDefault(), "%.1f亿", count / 100_000_000f)
            count >= 10_000 -> String.format(Locale.getDefault(), "%.1f万", count / 10_000f)
            else -> count.toString()
        }
    }

    private fun formatCtime(ctimeSec: Long): String {
        if (ctimeSec <= 0) return ""
        val now = System.currentTimeMillis() / 1000
        val diff = now - ctimeSec
        return when {
            diff < 60 -> "刚刚"
            diff < 3600 -> "${diff / 60}分钟前"
            diff < 86400 -> "${diff / 3600}小时前"
            diff < 86400 * 30 -> "${diff / 86400}天前"
            else -> {
                val sdf = SimpleDateFormat("yyyy-MM-dd", Locale.getDefault())
                sdf.format(Date(ctimeSec * 1000))
            }
        }
    }

    data class RelatedVideoItem(
        val bvid: String,
        val aid: Long,
        val cid: Long,
        val title: String,
        val pic: String,
        val duration: Int,
        val ownerName: String,
        val viewCount: Long,
        val danmakuCount: Long
    )

    data class CommentItem(
        val uname: String,
        val avatar: String,
        val level: Int,
        val message: String,
        val like: Long,
        val ctime: Long,
        val subReplies: List<Pair<String, String>>
    )
}
