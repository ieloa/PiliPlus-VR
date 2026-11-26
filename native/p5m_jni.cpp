// SPDX-License-Identifier: AGPL-3.0-only
//
// Ponte JNI entre XrBridge.kt e a sessao OpenXR.
#include <jni.h>
#include <android/bitmap.h>
#include <android/surface_texture_jni.h>
#include "spatializer.h"
#include "tone_mapper.h"
#include "xr_session.h"
#include "log.h"

#include <atomic>
#include <mutex>
#include <vector>

using p5m::XrVideoSession;
using p5m::ScreenParams;

namespace {

JavaVM *g_vm = nullptr;
// The active session, so AiDepthNative needs no pointer and XrBridge.smali
// needs no edits. Set on nativeCreate, cleared on nativeDestroy.
std::atomic<XrVideoSession *> g_session{nullptr};

inline XrVideoSession *Handle(jlong ptr)
{
	return reinterpret_cast<XrVideoSession *>(ptr);
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *)
{
	g_vm = vm;
	return JNI_VERSION_1_6;
}

extern "C" {

#define JNI_FCN(name) Java_io_github_gblandro_p5m_XrBridge_##name

JNIEXPORT jlong JNICALL JNI_FCN(nativeCreate)(JNIEnv *env, jobject, jobject activity)
{
	auto *session = new XrVideoSession();
	if(!session->Create(g_vm, activity))
	{
		delete session;
		return 0;
	}
	g_session.store(session, std::memory_order_release);
	return reinterpret_cast<jlong>(session);
}

JNIEXPORT jobject JNICALL JNI_FCN(nativeCreateVideoSurface)(JNIEnv *env, jobject, jlong ptr,
		jint width, jint height)
{
	if(!ptr)
		return nullptr;
	return Handle(ptr)->CreateVideoSurface(env, width, height);
}

JNIEXPORT jstring JNICALL JNI_FCN(nativeLastError)(JNIEnv *env, jobject, jlong ptr)
{
	if(!ptr)
		return env->NewStringUTF("");
	return env->NewStringUTF(Handle(ptr)->LastError());
}

JNIEXPORT void JNICALL JNI_FCN(nativeStart)(JNIEnv *, jobject, jlong ptr)
{
	if(ptr)
		Handle(ptr)->StartFrameLoop();
}

JNIEXPORT void JNICALL JNI_FCN(nativeStop)(JNIEnv *, jobject, jlong ptr)
{
	if(ptr)
		Handle(ptr)->StopFrameLoop();
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetScreenParams)(JNIEnv *, jobject, jlong ptr,
		jfloat radius, jfloat centralAngle, jfloat yawOffset, jfloat heightOffset,
		jfloat curvature)
{
	if(!ptr)
		return;
	ScreenParams params;
	params.radius = radius;
	params.centralAngle = centralAngle;
	params.curvature = curvature;
	params.yawOffset = yawOffset;
	params.heightOffset = heightOffset;
	Handle(ptr)->SetScreenParams(params);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetAspectRatio)(JNIEnv *, jobject, jlong ptr, jfloat aspect)
{
	if(ptr && aspect > 0.01f)
		Handle(ptr)->SetAspectRatio((float)aspect);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetSubImageExtent)(JNIEnv *, jobject, jlong ptr, jint w, jint h)
{
	if(ptr)
		Handle(ptr)->SetSubImageExtent(w, h);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetProjectionMode)(JNIEnv *, jobject, jlong ptr, jint mode)
{
	if(ptr)
		Handle(ptr)->SetProjectionMode((int)mode);
}

extern "C" __attribute__((visibility("default")))
void p5m_set_projection_mode(p5m::XrVideoSession* session, int mode)
{
	if(!session) return;
	session->SetProjectionMode(mode);
}

extern "C" __attribute__((visibility("default")))
void p5m_get_screen_geometry(p5m::XrVideoSession* session,
                             XrQuaternionf* outRecenterOri,
                             XrVector3f* outRecenterPos,
                             float* outDistance,
                             float* outCentralAngle,
                             float* outAspectRatio,
                             float* outHeightOffset)
{
	if(!session) return;
	session->GetScreenGeometry(outRecenterOri, outRecenterPos, outDistance, outCentralAngle, outAspectRatio, outHeightOffset);
}

extern "C" __attribute__((visibility("default")))
XrTime p5m_get_predicted_display_time(p5m::XrVideoSession* session)
{
	if(!session) return 0;
	return session->GetLatestPredictedDisplayTime();
}

extern "C" __attribute__((visibility("default")))
int p5m_update_controller_ray(p5m::XrVideoSession* session,
                              float ox, float oy, float oz,
                              float dx, float dy, float dz,
                              int buttons,
                              float* outU, float* outV)
{
	if(!session) return 0;
	return session->UpdateControllerRay(ox, oy, oz, dx, dy, dz, buttons, outU, outV);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetQuality)(JNIEnv *, jobject, jlong ptr,
		jint sharpness, jfloat sharpenAmount, jfloat passthroughOpacity,
		jfloat cinemaBrightness, jfloat cinemaContrast, jfloat cinemaSaturation,
		jfloat videoBrightness, jboolean frameExtrapolation)
{
	if(!ptr)
		return;
	p5m::QualityParams params;
	switch(sharpness)
	{
		case 1: params.sharpness = p5m::Sharpness::Light; break;
		case 2: params.sharpness = p5m::Sharpness::Medium; break;
		case 3: params.sharpness = p5m::Sharpness::Strong; break;
		case 4: params.sharpness = p5m::Sharpness::Auto; break;
		case 5: params.sharpness = p5m::Sharpness::Auto; break;
		default: params.sharpness = p5m::Sharpness::Off; break;
	}
	params.sharpenAmount = sharpenAmount;
	params.passthroughOpacity = passthroughOpacity;
	params.passthroughBrightness = cinemaBrightness;
	params.passthroughContrast = cinemaContrast;
	params.passthroughSaturation = cinemaSaturation;
	params.videoBrightness = videoBrightness;
	params.frameExtrapolation = frameExtrapolation == JNI_TRUE;
	Handle(ptr)->SetQualityParams(params);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetLayerShape)(JNIEnv *, jobject, jlong ptr, jint shape)
{
	if(ptr)
		Handle(ptr)->SetLayerShape(shape == 1 ? p5m::LayerShape::Quad
				: p5m::LayerShape::Cylinder);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetVideoLayerEnabled)(JNIEnv *, jobject, jlong ptr,
		jboolean enabled)
{
	if(ptr)
		Handle(ptr)->SetVideoLayerEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetRenderPath)(JNIEnv *, jobject, jlong ptr,
		jint path, jboolean sourceIsPq)
{
	if(!ptr)
		return;
	Handle(ptr)->SetRenderPath(path == 1 ? p5m::RenderPath::ToneMapped
			: p5m::RenderPath::Direct);
	Handle(ptr)->SetSourceIsPq(sourceIsPq == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL JNI_FCN(nativeAttachSurfaceTexture)(JNIEnv *env, jobject, jlong ptr,
		jobject surfaceTexture)
{
	if(!ptr || !surfaceTexture)
		return JNI_FALSE;
	return Handle(ptr)->AttachSurfaceTexture(env, surfaceTexture) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetWideColor)(JNIEnv *, jobject, jlong ptr,
		jboolean wide)
{
	if(ptr)
		Handle(ptr)->SetWideColor(wide == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetVerticalFlip)(JNIEnv *, jobject, jlong ptr,
		jboolean enabled)
{
	if(ptr)
		Handle(ptr)->SetVerticalFlip(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetStereoMode)(JNIEnv *, jobject, jlong ptr,
		jint mode, jboolean synthetic)
{
	if(ptr)
		Handle(ptr)->SetStereoMode((int)mode, synthetic == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetStereoTuning)(JNIEnv *, jobject, jlong ptr,
		jfloat strength, jfloat convergence)
{
	if(ptr)
		Handle(ptr)->SetStereoTuning((float)strength, (float)convergence);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetScreenOffset)(JNIEnv *, jobject, jlong ptr,
		jfloat pitch, jfloat yaw)
{
	if(ptr)
		Handle(ptr)->SetScreenOffset((float)pitch, (float)yaw);
}

JNIEXPORT void JNICALL JNI_FCN(nativeGetScreenOffset)(JNIEnv *env, jobject, jlong ptr,
		jfloatArray out)
{
	if(!ptr || !out || env->GetArrayLength(out) < 2)
		return;
	float yaw = 0.0f;
	float pitch = 0.0f;
	Handle(ptr)->GetScreenOffset(&yaw, &pitch);
	jfloat values[2] = {(jfloat)yaw, (jfloat)pitch};
	env->SetFloatArrayRegion(out, 0, 2, values);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetAimDragSample)(JNIEnv *, jobject, jlong ptr,
		jfloat yaw, jfloat pitch, jboolean grip)
{
	if(ptr)
		Handle(ptr)->SetAimDragSample((float)yaw, (float)pitch, grip == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetSettingsBitmap)(JNIEnv *env, jobject, jlong ptr,
		jobject bitmap)
{
	if(!ptr || !bitmap)
		return;

	AndroidBitmapInfo info{};
	if(AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_getInfo failed for settings");
		return;
	}
	if(info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
	{
		LOGE("The settings panel needs ARGB_8888, got format %d", (int)info.format);
		return;
	}

	void *pixels = nullptr;
	if(AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_lockPixels failed for settings");
		return;
	}
	Handle(ptr)->SetSettingsBitmap(static_cast<const uint8_t *>(pixels), (int32_t)info.width,
			(int32_t)info.height);
	AndroidBitmap_unlockPixels(env, bitmap);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetSettingsVisible)(JNIEnv *, jobject, jlong ptr,
		jboolean visible)
{
	if(ptr)
		Handle(ptr)->SetSettingsVisible(visible != JNI_FALSE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetHudBitmap)(JNIEnv *env, jobject, jlong ptr,
		jobject bitmap)
{
	if(!ptr || !bitmap)
		return;

	AndroidBitmapInfo info{};
	if(AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_getInfo failed");
		return;
	}
	if(info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
	{
		LOGE("The help panel needs ARGB_8888, got format %d", (int)info.format);
		return;
	}

	void *pixels = nullptr;
	if(AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_lockPixels failed");
		return;
	}
	Handle(ptr)->SetHudBitmap(static_cast<const uint8_t *>(pixels), (int32_t)info.width,
			(int32_t)info.height);
	AndroidBitmap_unlockPixels(env, bitmap);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetHudVisible)(JNIEnv *, jobject, jlong ptr,
		jboolean visible)
{
	if(ptr)
		Handle(ptr)->SetHudVisible(visible == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetLoadingVisible)(JNIEnv *, jobject, jlong ptr,
		jboolean visible)
{
	if(ptr)
		Handle(ptr)->SetLoadingVisible(visible == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetLoadingReady)(JNIEnv *, jobject, jlong ptr,
		jboolean ready)
{
	if(ptr)
		Handle(ptr)->SetLoadingReady(ready == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL JNI_FCN(nativeIsLoadingVisible)(JNIEnv *, jobject, jlong ptr)
{
	return ptr && Handle(ptr)->IsLoadingVisible() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL JNI_FCN(nativeHasRenderedFrame)(JNIEnv *, jobject, jlong ptr)
{
	return ptr && Handle(ptr)->IsSessionRendering() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetOsdBitmap)(JNIEnv *env, jobject, jlong ptr,
		jobject bitmap)
{
	if(!ptr || !bitmap)
		return;

	AndroidBitmapInfo info{};
	if(AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_getInfo failed for OSD");
		return;
	}
	if(info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
	{
		LOGE("The OSD needs ARGB_8888, got format %d", (int)info.format);
		return;
	}

	void *pixels = nullptr;
	if(AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_lockPixels failed for OSD");
		return;
	}
	Handle(ptr)->SetOsdBitmap(static_cast<const uint8_t *>(pixels), (int32_t)info.width,
			(int32_t)info.height);
	AndroidBitmap_unlockPixels(env, bitmap);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetOsdVisible)(JNIEnv *, jobject, jlong ptr,
		jboolean visible)
{
	if(ptr)
		Handle(ptr)->SetOsdVisible(visible == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetResolutionBadgeBitmap)(JNIEnv *env, jobject, jlong ptr,
		jobject bitmap)
{
	if(!ptr || !bitmap)
		return;

	AndroidBitmapInfo info{};
	if(AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_getInfo failed for Resolution Badge");
		return;
	}
	if(info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
	{
		LOGE("The Resolution Badge needs ARGB_8888, got format %d", (int)info.format);
		return;
	}

	void *pixels = nullptr;
	if(AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		LOGE("AndroidBitmap_lockPixels failed for Resolution Badge");
		return;
	}
	Handle(ptr)->SetResBitmap(static_cast<const uint8_t *>(pixels), (int32_t)info.width,
			(int32_t)info.height);
	AndroidBitmap_unlockPixels(env, bitmap);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetResolutionBadgeVisible)(JNIEnv *, jobject, jlong ptr,
		jboolean visible)
{
	if(ptr)
		Handle(ptr)->SetResVisible(visible == JNI_TRUE);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetDanmakuBitmap)(JNIEnv *env, jobject, jlong ptr,
		jobject bitmap)
{
	if(!ptr || !bitmap)
		return;

	AndroidBitmapInfo info{};
	if(AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
		return;
	if(info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
		return;

	void *pixels = nullptr;
	if(AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
		return;
	Handle(ptr)->SetDanmakuBitmap(static_cast<const uint8_t *>(pixels), (int32_t)info.width,
			(int32_t)info.height);
	AndroidBitmap_unlockPixels(env, bitmap);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetDanmakuVisible)(JNIEnv *, jobject, jlong ptr,
		jboolean visible)
{
	if(ptr)
		Handle(ptr)->SetDanmakuVisible(visible == JNI_TRUE);
}

JNIEXPORT jfloat JNICALL JNI_FCN(nativeSelectDisplayRefreshRate)(JNIEnv *, jobject, jlong ptr,
		jint sourceFps)
{
	if(!ptr)
		return 0.0f;
	return Handle(ptr)->SelectDisplayRefreshRate(sourceFps);
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetPassthrough)(JNIEnv *, jobject, jlong ptr, jboolean enabled)
{
	if(ptr)
		Handle(ptr)->SetPassthroughEnabled(enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL JNI_FCN(nativeIsPassthroughSupported)(JNIEnv *, jobject, jlong ptr)
{
	return (ptr && Handle(ptr)->PassthroughSupported()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FCN(nativeSetSpatialAudio)(JNIEnv *, jobject, jfloat strength)
{
	// Sem ponteiro de sessao: o espacializador e um so no processo, porque quem
	// o chama e a thread de audio do chiaki, que nao sabe nada da sessao OpenXR.
	p5m::Spatializer::Instance().SetStrength(strength);
	if(strength > 0.0f)
		LOGI("Virtual speakers on, strength %.2f", (double)strength);
	else
		LOGI("Virtual speakers off: the audio passes through as it came");
}

/**
 * Retrato do desempenho num array de floats, para o painel.
 *
 * Array e nao objeto: isto e lido uma vez por segundo com o painel aberto, e um
 * objeto por leitura seria uma alocacao e uma classe a manter dos dois lados
 * para carregar oito numeros. O layout esta documentado no lado Kotlin, que e
 * quem os le.
 */
JNIEXPORT jfloatArray JNICALL JNI_FCN(nativeReadPerformance)(JNIEnv *env, jobject, jlong ptr)
{
	if(!ptr)
		return nullptr;
	p5m::PerformanceSnapshot snap = Handle(ptr)->ReadPerformance();
	jfloat values[] = {
		snap.droppedFrames ? 1.0f : 0.0f, snap.droppedFrameCount,
		snap.hasGpuUtilization ? 1.0f : 0.0f, snap.gpuUtilization,
		snap.hasAppGpuTime ? 1.0f : 0.0f, snap.appGpuTimeMs,
		snap.hasCompositorGpuTime ? 1.0f : 0.0f, snap.compositorGpuTimeMs,
		snap.hasThermal ? 1.0f : 0.0f, snap.thermalHeadroom, snap.thermalSlope,
		snap.hasMotionToPhoton ? 1.0f : 0.0f, snap.motionToPhotonMs,
		snap.hasCpuUtilization ? 1.0f : 0.0f, snap.cpuUtilization,
	};
	const jsize count = (jsize)(sizeof(values) / sizeof(values[0]));
	jfloatArray out = env->NewFloatArray(count);
	if(!out)
		return nullptr;
	env->SetFloatArrayRegion(out, 0, count, values);
	return out;
}

/** Largura nos 32 bits altos, altura nos baixos. Zero quando nao ha valor. */
JNIEXPORT jlong JNICALL JNI_FCN(nativeRecommendedResolution)(JNIEnv *, jobject, jlong ptr)
{
	if(!ptr)
		return 0;
	int w = 0, h = 0;
	Handle(ptr)->RecommendedResolution(&w, &h);
	return ((jlong)w << 32) | (jlong)(uint32_t)h;
}

JNIEXPORT void JNICALL JNI_FCN(nativeRecenter)(JNIEnv *, jobject, jlong ptr)
{
	if(ptr)
		Handle(ptr)->Recenter();
}

JNIEXPORT void JNICALL JNI_FCN(nativeDestroy)(JNIEnv *, jobject, jlong ptr)
{
	if(!ptr)
		return;
	auto *session = Handle(ptr);
	g_session.store(nullptr, std::memory_order_release);
	session->Destroy();
	delete session;
}


// -- Filtro do modo janela ------------------------------------------------
//
// No modo janela nao ha camada nossa no compositor, e portanto nao ha bits de
// sharpening para pedir: quem compoe e o Horizon OS, e o app e um painel comum
// como qualquer outro. A nitidez so pode ser nossa, entao o mesmo conversor de
// tons do modo imersivo roda aqui, desenhando no framebuffer da GLSurfaceView
// em vez de num swapchain.
//
// E de proposito que e o mesmo ToneMapper, e nao um shader gemeo do lado Java:
// dois shaders que deveriam concordar acabam divergindo, e a nitidez media de
// um modo deixaria de ser a nitidez media do outro.

#undef JNI_FCN
#define JNI_FCN(name) Java_io_github_gblandro_p5m_AudioRoute_##name

JNIEXPORT void JNICALL JNI_FCN(nativeSetPrefersSystemMixer)(JNIEnv *, jobject,
		jboolean prefers)
{
	p5m::SetPrefersSystemMixer(prefers == JNI_TRUE);
	LOGI("Audio output through the %s", prefers == JNI_TRUE
			? "system mixer (Horizon OS can position the sound)"
			: "direct path to the hardware (lower latency)");
}

#undef JNI_FCN
#define JNI_FCN(name) Java_io_github_gblandro_p5m_VideoFilter_##name

JNIEXPORT jlong JNICALL JNI_FCN(nativeCreate)(JNIEnv *, jobject)
{
	return reinterpret_cast<jlong>(new p5m::ToneMapper());
}

JNIEXPORT jboolean JNICALL JNI_FCN(nativeInit)(JNIEnv *env, jobject, jlong ptr,
		jobject surface_texture)
{
	if(!ptr)
		return JNI_FALSE;
	auto *mapper = reinterpret_cast<p5m::ToneMapper *>(ptr);
	if(!mapper->Init())
		return JNI_FALSE;
	ASurfaceTexture *native = ASurfaceTexture_fromSurfaceTexture(env, surface_texture);
	if(!native)
	{
		LOGE("ASurfaceTexture_fromSurfaceTexture failed in window mode");
		return JNI_FALSE;
	}
	return mapper->Attach(native) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL JNI_FCN(nativeDraw)(JNIEnv *, jobject, jlong ptr,
		jint width, jint height, jboolean pq, jfloat sharpen)
{
	if(!ptr)
		return JNI_FALSE;
	// Alvo zero: o framebuffer padrao da GLSurfaceView. E encode ligado, porque
	// a janela e RGBA8 cru e nao aplica a curva do sRGB na escrita como faz o
	// swapchain do compositor.
	return reinterpret_cast<p5m::ToneMapper *>(ptr)->Render(0, width, height,
			pq == JNI_TRUE, sharpen, true) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL JNI_FCN(nativeDestroy)(JNIEnv *, jobject, jlong ptr)
{
	if(!ptr)
		return;
	auto *mapper = reinterpret_cast<p5m::ToneMapper *>(ptr);
	mapper->Destroy();
	delete mapper;
}

} // extern "C"

namespace {

std::mutex g_ai_ref_mutex;
jobject g_ai_in_ref[2] = {nullptr, nullptr};
jobject g_ai_out_ref[2] = {nullptr, nullptr};

void DeleteAiGlobalRefsLocked(JNIEnv *env)
{
	for(int i = 0; i < 2; i++)
	{
		if(g_ai_in_ref[i]) { env->DeleteGlobalRef(g_ai_in_ref[i]); g_ai_in_ref[i] = nullptr; }
		if(g_ai_out_ref[i]) { env->DeleteGlobalRef(g_ai_out_ref[i]); g_ai_out_ref[i] = nullptr; }
	}
}

} // namespace

// -- AI depth source -------------------------------------------------------
// Declared on our own Java class so the stock XrBridge.smali is untouched.
// Double-buffered DirectByteBuffers eliminate per-frame JNI array copies and
// heap allocations; GL work stays on the frame loop and shared-EGL stage thread.
extern "C" {

#define AI_FCN(name) Java_com_example_piliplus_vr_AiDepthNative_##name

JNIEXPORT void JNICALL AI_FCN(setMode)(JNIEnv *, jclass, jboolean on, jint mapsPerSecond)
{
	auto *s = g_session.load(std::memory_order_acquire);
	if(s)
		s->SetAiDepthMode(on == JNI_TRUE, (int32_t)mapsPerSecond);
}

JNIEXPORT void JNICALL AI_FCN(setCaptureRate)(JNIEnv *, jclass, jint mapsPerSecond)
{
	auto *s = g_session.load(std::memory_order_acquire);
	if(s)
		s->SetAiCaptureRate((int32_t)mapsPerSecond);
}

JNIEXPORT void JNICALL AI_FCN(setEdgeEnhance)(JNIEnv *, jclass, jfloat sharp, jboolean cubic)
{
	p5m::SetAiEdgeEnhance((float)sharp, cubic == JNI_TRUE);
}

JNIEXPORT jfloat JNICALL AI_FCN(getWarpGpuMs)(JNIEnv *, jclass)
{
	return (jfloat)p5m::GetLastWarpGpuMs();
}

JNIEXPORT jfloat JNICALL AI_FCN(getDisplayRefreshHz)(JNIEnv *, jclass)
{
	return (jfloat)p5m::GetCurrentDisplayRefreshHz();
}

JNIEXPORT jfloat JNICALL AI_FCN(getMissedRefreshPct)(JNIEnv *, jclass)
{
	return (jfloat)p5m::GetMissedRefreshPct();
}

JNIEXPORT jlong JNICALL AI_FCN(waitForXrFrame)(JNIEnv *, jclass, jlong lastSeq, jint timeoutMs)
{
	return (jlong)p5m::WaitForXrFrame((uint64_t)(lastSeq < 0 ? 0 : lastSeq), (int32_t)timeoutMs);
}

JNIEXPORT jlong JNICALL AI_FCN(getTargetDisplayNs)(JNIEnv *, jclass)
{
	return (jlong)p5m::GetXrTargetDisplayNs();
}

JNIEXPORT void JNICALL AI_FCN(setDanmakuUploadHeight)(JNIEnv *, jclass, jint height)
{
	p5m::SetDanmakuUploadHeightHint((int32_t)height);
}

JNIEXPORT void JNICALL AI_FCN(setCurtainDuration)(JNIEnv *, jclass, jint millis)
{
	p5m::SetCurtainDurationMs((int32_t)millis);
}

JNIEXPORT void JNICALL AI_FCN(setShowDepth)(JNIEnv *, jclass, jboolean on)
{
	auto *s = g_session.load(std::memory_order_acquire);
	if(s)
		s->SetShowDepth(on == JNI_TRUE);
}

JNIEXPORT void JNICALL AI_FCN(setFastEma)(JNIEnv *, jclass, jboolean on)
{
	p5m::SetAiFastEma(on == JNI_TRUE);
}

JNIEXPORT jint JNICALL AI_FCN(frameWidth)(JNIEnv *, jclass)
{
	auto *s = g_session.load(std::memory_order_acquire);
	return s ? s->AiFrameWidth() : 0;
}

JNIEXPORT jint JNICALL AI_FCN(frameHeight)(JNIEnv *, jclass)
{
	auto *s = g_session.load(std::memory_order_acquire);
	return s ? s->AiFrameHeight() : 0;
}

JNIEXPORT jboolean JNICALL AI_FCN(registerBuffers)(JNIEnv *env, jclass,
		jobject in0, jobject out0, jobject in1, jobject out1,
		jint inW, jint inH, jint inType,
		jint outW, jint outH, jint outType)
{
	if(!in0 || !out0 || !in1 || !out1)
		return JNI_FALSE;
	void *pIn0 = env->GetDirectBufferAddress(in0);
	jlong cIn0 = env->GetDirectBufferCapacity(in0);
	void *pOut0 = env->GetDirectBufferAddress(out0);
	jlong cOut0 = env->GetDirectBufferCapacity(out0);
	void *pIn1 = env->GetDirectBufferAddress(in1);
	jlong cIn1 = env->GetDirectBufferCapacity(in1);
	void *pOut1 = env->GetDirectBufferAddress(out1);
	jlong cOut1 = env->GetDirectBufferCapacity(out1);
	if(!pIn0 || !pOut0 || !pIn1 || !pOut1)
		return JNI_FALSE;

	std::lock_guard<std::mutex> lock(g_ai_ref_mutex);
	p5m::UnregisterAiSlotBuffers();
	DeleteAiGlobalRefsLocked(env);

	g_ai_in_ref[0] = env->NewGlobalRef(in0);
	g_ai_out_ref[0] = env->NewGlobalRef(out0);
	g_ai_in_ref[1] = env->NewGlobalRef(in1);
	g_ai_out_ref[1] = env->NewGlobalRef(out1);

	if(!p5m::RegisterAiSlotBuffers(
			pIn0, (int64_t)cIn0, pOut0, (int64_t)cOut0,
			pIn1, (int64_t)cIn1, pOut1, (int64_t)cOut1,
			(int32_t)inW, (int32_t)inH, (int32_t)inType,
			(int32_t)outW, (int32_t)outH, (int32_t)outType))
	{
		p5m::UnregisterAiSlotBuffers();
		DeleteAiGlobalRefsLocked(env);
		return JNI_FALSE;
	}
	return JNI_TRUE;
}

JNIEXPORT void JNICALL AI_FCN(unregisterBuffers)(JNIEnv *env, jclass)
{
	std::lock_guard<std::mutex> lock(g_ai_ref_mutex);
	p5m::UnregisterAiSlotBuffers();
	DeleteAiGlobalRefsLocked(env);
}

JNIEXPORT jint JNICALL AI_FCN(acquireInputSlot)(JNIEnv *, jclass, jint timeoutMs)
{
	return (jint)p5m::AcquireAiInputSlot((int32_t)timeoutMs);
}

JNIEXPORT void JNICALL AI_FCN(releaseInputSlot)(JNIEnv *, jclass, jint slot)
{
	p5m::ReleaseAiInputSlot((int32_t)slot);
}

JNIEXPORT void JNICALL AI_FCN(submitOutputSlot)(JNIEnv *, jclass, jint slot)
{
	p5m::SubmitAiOutputSlot((int32_t)slot);
}

JNIEXPORT jboolean JNICALL AI_FCN(pollHandTracking)(JNIEnv *env, jclass, jfloatArray outState)
{
	XrVideoSession *session = g_session.load(std::memory_order_acquire);
	if(!session || !outState)
		return JNI_FALSE;
	jfloat buf[28]{};
	session->PollHandTracking(buf);
	jsize len = env->GetArrayLength(outState);
	if(len > 0)
	{
		env->SetFloatArrayRegion(outState, 0, len < 28 ? len : 28, buf);
	}
	return (buf[0] > 0.5f || buf[8] > 0.5f || buf[16] != 0.0f || buf[24] > 0.5f || buf[25] > 0.5f || buf[26] > 0.5f) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL AI_FCN(setHandTrackingEnabled)(JNIEnv *, jclass, jboolean enabled)
{
	XrVideoSession *session = g_session.load(std::memory_order_acquire);
	if(session)
		session->SetHandTrackingEnabled(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL AI_FCN(setHandCursorHidden)(JNIEnv *, jclass, jboolean hidden)
{
	XrVideoSession *session = g_session.load(std::memory_order_acquire);
	if(session)
		session->SetHandCursorHidden(hidden == JNI_TRUE);
}

JNIEXPORT void JNICALL AI_FCN(syncBaseScreenState)(JNIEnv *, jclass, jfloat baseFov, jfloat screenDist)
{
	XrVideoSession *session = g_session.load(std::memory_order_acquire);
	if(session)
		session->SyncBaseScreenState((float)baseFov, (float)screenDist);
}

JNIEXPORT void JNICALL AI_FCN(setCornerResizing)(JNIEnv *, jclass, jboolean resizing)
{
	XrVideoSession *session = g_session.load(std::memory_order_acquire);
	if(session)
		session->SetCornerResizing(resizing == JNI_TRUE);
}

JNIEXPORT void JNICALL AI_FCN(setScreenManipulating)(JNIEnv *, jclass, jboolean manipulating)
{
	XrVideoSession *session = g_session.load(std::memory_order_acquire);
	if(session)
		session->SetScreenManipulating(manipulating == JNI_TRUE);
}

} // extern "C"
