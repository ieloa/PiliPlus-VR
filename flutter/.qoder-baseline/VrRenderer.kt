package com.example.piliplus.vr

import android.graphics.SurfaceTexture
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.opengl.Matrix
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.math.cos
import kotlin.math.sin

enum class ProjectionMode {
    CINEMA_2D,
    SPHERE_360,
    DOME_180,
    SBS_3D,
    TOP_BOTTOM_3D
}

class VrRenderer(
    private val onSurfaceCreatedCallback: (SurfaceTexture) -> Unit
) : GLSurfaceView.Renderer, SurfaceTexture.OnFrameAvailableListener {

    private var surfaceTexture: SurfaceTexture? = null
    private var textureId: Int = 0
    private var updateSurface = false

    private val mtx = FloatArray(16)
    private val mvpMatrix = FloatArray(16)
    private val projectionMatrix = FloatArray(16)
    private val viewMatrix = FloatArray(16)
    private val modelMatrix = FloatArray(16)
    val headRotationMatrix = FloatArray(16)

    // Manual drag rotation offsets
    var touchPitch = 0f
    var touchYaw = 0f
    var fov = 75f

    var currentMode: ProjectionMode = ProjectionMode.CINEMA_2D
        set(value) {
            field = value
            rebuildGeometry()
        }

    private var program = 0
    private var uMVPMatrixHandle = 0
    private var uSTMatrixHandle = 0
    private var uTexOffsetScaleHandle = 0
    private var aPositionHandle = 0
    private var aTextureCoordHandle = 0

    // Geometries
    private var vertexBuffer: FloatBuffer? = null
    private var uvBuffer: FloatBuffer? = null
    private var vertexCount = 0

    init {
        Matrix.setIdentityM(headRotationMatrix, 0)
        Matrix.setIdentityM(modelMatrix, 0)
        Matrix.setIdentityM(viewMatrix, 0)
        rebuildGeometry()
    }

    private fun rebuildGeometry() {
        when (currentMode) {
            ProjectionMode.SPHERE_360, ProjectionMode.SBS_3D, ProjectionMode.TOP_BOTTOM_3D -> {
                buildSphere(radius = 50f, lats = 40, longs = 40, isDome = false)
            }
            ProjectionMode.DOME_180 -> {
                buildSphere(radius = 50f, lats = 30, longs = 30, isDome = true)
            }
            ProjectionMode.CINEMA_2D -> {
                buildCurvedScreen(radius = 4f, widthAngleDeg = 90f, height = 2.4f, segments = 32)
            }
        }
    }

    private fun buildCurvedScreen(radius: Float, widthAngleDeg: Float, height: Float, segments: Int) {
        val vertices = ArrayList<Float>()
        val uvs = ArrayList<Float>()

        val halfAngleRad = Math.toRadians((widthAngleDeg / 2).toDouble()).toFloat()
        val angleStep = (2 * halfAngleRad) / segments
        val halfH = height / 2f

        for (i in 0 until segments) {
            val a0 = -halfAngleRad + i * angleStep
            val a1 = a0 + angleStep

            val x0 = radius * sin(a0)
            val z0 = -radius * cos(a0)
            val x1 = radius * sin(a1)
            val z1 = -radius * cos(a1)

            val u0 = i.toFloat() / segments
            val u1 = (i + 1).toFloat() / segments

            // Quad as 2 Triangles: (p0, p1, p2) & (p1, p3, p2)
            // Top: +halfH, Bottom: -halfH
            // T1
            vertices.addAll(listOf(x0, halfH, z0,  x0, -halfH, z0,  x1, halfH, z1))
            uvs.addAll(listOf(u0, 0f,  u0, 1f,  u1, 0f))

            // T2
            vertices.addAll(listOf(x1, halfH, z1,  x0, -halfH, z0,  x1, -halfH, z1))
            uvs.addAll(listOf(u1, 0f,  u0, 1f,  u1, 1f))
        }

        fillBuffers(vertices, uvs)
    }

    private fun buildSphere(radius: Float, lats: Int, longs: Int, isDome: Boolean) {
        val vertices = ArrayList<Float>()
        val uvs = ArrayList<Float>()

        val maxLat = if (isDome) Math.PI / 2 else Math.PI
        val minLat = if (isDome) -Math.PI / 2 else -Math.PI / 2

        val maxLong = if (isDome) Math.PI else 2 * Math.PI
        val startLong = if (isDome) -Math.PI / 2 else -Math.PI

        for (i in 0 until lats) {
            val theta0 = minLat + (i.toDouble() / lats) * (maxLat - minLat)
            val theta1 = minLat + ((i + 1).toDouble() / lats) * (maxLat - minLat)

            val v0 = 1f - (i.toFloat() / lats)
            val v1 = 1f - ((i + 1).toFloat() / lats)

            for (j in 0 until longs) {
                val phi0 = startLong + (j.toDouble() / longs) * maxLong
                val phi1 = startLong + ((j + 1).toDouble() / longs) * maxLong

                val u0 = j.toFloat() / longs
                val u1 = (j + 1).toFloat() / longs

                val p0 = spherePoint(radius, theta0, phi0)
                val p1 = spherePoint(radius, theta1, phi0)
                val p2 = spherePoint(radius, theta0, phi1)
                val p3 = spherePoint(radius, theta1, phi1)

                // Triangle 1
                vertices.addAll(listOf(p0[0], p0[1], p0[2],  p1[0], p1[1], p1[2],  p2[0], p2[1], p2[2]))
                uvs.addAll(listOf(u0, v0,  u0, v1,  u1, v0))

                // Triangle 2
                vertices.addAll(listOf(p2[0], p2[1], p2[2],  p1[0], p1[1], p1[2],  p3[0], p3[1], p3[2]))
                uvs.addAll(listOf(u1, v0,  u0, v1,  u1, v1))
            }
        }

        fillBuffers(vertices, uvs)
    }

    private fun spherePoint(r: Float, theta: Double, phi: Double): FloatArray {
        val cosT = cos(theta).toFloat()
        val sinT = sin(theta).toFloat()
        val cosP = cos(phi).toFloat()
        val sinP = sin(phi).toFloat()
        return floatArrayOf(
            r * cosT * sinP,
            r * sinT,
            -r * cosT * cosP
        )
    }

    private fun fillBuffers(vertices: List<Float>, uvs: List<Float>) {
        vertexCount = vertices.size / 3

        vertexBuffer = ByteBuffer.allocateDirect(vertices.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer().apply {
                for (v in vertices) put(v)
                position(0)
            }

        uvBuffer = ByteBuffer.allocateDirect(uvs.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer().apply {
                for (u in uvs) put(u)
                position(0)
            }
    }

    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        textureId = textures[0]

        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)
        GLES20.glTexParameterf(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR.toFloat())
        GLES20.glTexParameterf(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR.toFloat())
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        val st = SurfaceTexture(textureId)
        st.setOnFrameAvailableListener(this)
        surfaceTexture = st
        onSurfaceCreatedCallback(st)

        initShaders()
    }

    private fun initShaders() {
        val vShader = """
            uniform mat4 uMVPMatrix;
            uniform mat4 uSTMatrix;
            attribute vec4 aPosition;
            attribute vec4 aTextureCoord;
            varying vec2 vTextureCoord;
            void main() {
                gl_Position = uMVPMatrix * aPosition;
                vTextureCoord = (uSTMatrix * aTextureCoord).xy;
            }
        """.trimIndent()

        val fShader = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTextureCoord;
            uniform samplerExternalOES sTexture;
            uniform vec4 uTexOffsetScale; // x=offset_x, y=offset_y, z=scale_x, w=scale_y
            void main() {
                vec2 uv = vec2(
                    vTextureCoord.x * uTexOffsetScale.z + uTexOffsetScale.x,
                    vTextureCoord.y * uTexOffsetScale.w + uTexOffsetScale.y
                );
                gl_FragColor = texture2D(sTexture, uv);
            }
        """.trimIndent()

        program = createProgram(vShader, fShader)
        aPositionHandle = GLES20.glGetAttribLocation(program, "aPosition")
        aTextureCoordHandle = GLES20.glGetAttribLocation(program, "aTextureCoord")
        uMVPMatrixHandle = GLES20.glGetUniformLocation(program, "uMVPMatrix")
        uSTMatrixHandle = GLES20.glGetUniformLocation(program, "uSTMatrix")
        uTexOffsetScaleHandle = GLES20.glGetUniformLocation(program, "uTexOffsetScale")
    }

    private fun createProgram(vertexSource: String, fragmentSource: String): Int {
        val vs = loadShader(GLES20.GL_VERTEX_SHADER, vertexSource)
        val fs = loadShader(GLES20.GL_FRAGMENT_SHADER, fragmentSource)
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, vs)
        GLES20.glAttachShader(p, fs)
        GLES20.glLinkProgram(p)
        return p
    }

    private fun loadShader(type: Int, source: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, source)
        GLES20.glCompileShader(shader)
        return shader
    }

    private var viewWidth = 1920
    private var viewHeight = 1080

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        viewWidth = width
        viewHeight = height
        GLES20.glViewport(0, 0, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        synchronized(this) {
            if (updateSurface) {
                surfaceTexture?.updateTexImage()
                surfaceTexture?.getTransformMatrix(mtx)
                updateSurface = false
            }
        }

        GLES20.glClearColor(0.02f, 0.02f, 0.04f, 1.0f)
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT or GLES20.GL_DEPTH_BUFFER_BIT)
        GLES20.glEnable(GLES20.GL_DEPTH_TEST)

        // Stereoscopic rendering if SBS_3D or TOP_BOTTOM_3D
        if (currentMode == ProjectionMode.SBS_3D) {
            // Left eye: left viewport, sample left half [0..0.5]
            val halfW = viewWidth / 2
            GLES20.glViewport(0, 0, halfW, viewHeight)
            drawEye(0f, 0f, 0.5f, 1f, halfW.toFloat() / viewHeight)

            // Right eye: right viewport, sample right half [0.5..1.0]
            GLES20.glViewport(halfW, 0, halfW, viewHeight)
            drawEye(0.5f, 0f, 0.5f, 1f, halfW.toFloat() / viewHeight)
        } else if (currentMode == ProjectionMode.TOP_BOTTOM_3D) {
            // Left eye: left viewport, top half [0..0.5]
            val halfW = viewWidth / 2
            GLES20.glViewport(0, 0, halfW, viewHeight)
            drawEye(0f, 0f, 1f, 0.5f, halfW.toFloat() / viewHeight)

            // Right eye: right viewport, bottom half [0.5..1.0]
            GLES20.glViewport(halfW, 0, halfW, viewHeight)
            drawEye(0f, 0.5f, 1f, 0.5f, halfW.toFloat() / viewHeight)
        } else {
            // Full viewport mono (Cinema or Sphere 360)
            GLES20.glViewport(0, 0, viewWidth, viewHeight)
            drawEye(0f, 0f, 1f, 1f, viewWidth.toFloat() / viewHeight)
        }
    }

    private fun drawEye(offsetX: Float, offsetY: Float, scaleX: Float, scaleY: Float, aspect: Float) {
        val vb = vertexBuffer ?: return
        val ub = uvBuffer ?: return

        GLES20.glUseProgram(program)

        // Projection
        Matrix.perspectiveM(projectionMatrix, 0, fov, aspect, 0.1f, 200f)

        // View: combines manual touch pitch/yaw and Sensor head-tracking matrix
        val touchMatrix = FloatArray(16)
        Matrix.setIdentityM(touchMatrix, 0)
        Matrix.rotateM(touchMatrix, 0, touchPitch, 1f, 0f, 0f)
        Matrix.rotateM(touchMatrix, 0, touchYaw, 0f, 1f, 0f)

        val combinedRotation = FloatArray(16)
        Matrix.multiplyMM(combinedRotation, 0, touchMatrix, 0, headRotationMatrix, 0)

        // Camera at origin (0,0,0) looking down -Z
        val baseCamera = FloatArray(16)
        Matrix.setLookAtM(baseCamera, 0, 0f, 0f, 0f, 0f, 0f, -1f, 0f, 1f, 0f)

        Matrix.multiplyMM(viewMatrix, 0, baseCamera, 0, combinedRotation, 0)

        // MVP = Projection * View * Model
        val vpMatrix = FloatArray(16)
        Matrix.multiplyMM(vpMatrix, 0, projectionMatrix, 0, viewMatrix, 0)
        Matrix.multiplyMM(mvpMatrix, 0, vpMatrix, 0, modelMatrix, 0)

        GLES20.glUniformMatrix4fv(uMVPMatrixHandle, 1, false, mvpMatrix, 0)
        GLES20.glUniformMatrix4fv(uSTMatrixHandle, 1, false, mtx, 0)
        GLES20.glUniform4f(uTexOffsetScaleHandle, offsetX, offsetY, scaleX, scaleY)

        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, textureId)

        vb.position(0)
        GLES20.glVertexAttribPointer(aPositionHandle, 3, GLES20.GL_FLOAT, false, 0, vb)
        GLES20.glEnableVertexAttribArray(aPositionHandle)

        ub.position(0)
        GLES20.glVertexAttribPointer(aTextureCoordHandle, 2, GLES20.GL_FLOAT, false, 0, ub)
        GLES20.glEnableVertexAttribArray(aTextureCoordHandle)

        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, vertexCount)

        GLES20.glDisableVertexAttribArray(aPositionHandle)
        GLES20.glDisableVertexAttribArray(aTextureCoordHandle)
    }

    override fun onFrameAvailable(st: SurfaceTexture?) {
        synchronized(this) {
            updateSurface = true
        }
    }

    fun release() {
        surfaceTexture?.release()
        surfaceTexture = null
    }
}
