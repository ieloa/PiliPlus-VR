// SPDX-License-Identifier: AGPL-3.0-only
#include "xr_session.h"
#include "spatializer.h"
#include "log.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <string>

namespace p5m {
namespace {

const char *XrResultStr(XrInstance instance, XrResult res)
{
	static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
	if(instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, res, buf)))
		return buf;
	snprintf(buf, sizeof(buf), "XrResult(%d)", (int)res);
	return buf;
}

#define XR_CHECK(instance, expr, what)                                              \
	do {                                                                            \
		XrResult _res = (expr);                                                     \
		if(XR_FAILED(_res)) {                                                       \
			LOGE("%s failed: %s", (what), XrResultStr((instance), _res));           \
			return false;                                                           \
		}                                                                           \
	} while(0)

// Tamanho do painel de ajuda, em pixels de textura. Largo o bastante para uma
// linha de comando por vez sem quebrar, e pequeno o bastante para o envio por
// frame nao pesar.
constexpr int32_t kHudWidth = 1024;
constexpr int32_t kHudHeight = 640;
constexpr int32_t kSettingsWidth = 1024;
constexpr int32_t kSettingsHeight = 440;
constexpr int32_t kOsdWidth = 1024;
constexpr int32_t kOsdHeight = 140;
constexpr int32_t kResWidth = 384;
constexpr int32_t kResHeight = 128;
constexpr int32_t kDanmakuWidth = 2400;
constexpr int32_t kDanmakuHeight = 1440;
constexpr int32_t kCursorSize = 64;

constexpr float kHudDistance = 1.45f;
constexpr float kHudWidth1M = 0.88f;
constexpr float kHudHeightOffset = -0.3f;
// HUD / settings / OSD overlays are pinned at a FIXED distance, so their
// apparent size no longer changes when the virtual screen is pushed away or
// pulled closer. kFixedSettingsW keeps the settings card at exactly the same
// angular width it had with the default 2.5 m screen (old settings_dist 2.3 m),
// so nothing changes visually until the user actually moves the screen.
constexpr float kFixedUiDist = 1.4f;
constexpr float kFixedSettingsW = 0.95f * kFixedUiDist / 2.3f;
// The settings card sits above the video's top edge. kSettingsUpExtra is
// measured from that edge to the card's centre and keeps a little breathing
// room above the picture. The raycast hit test uses the same constant so the
// visible card and its hit box stay aligned.
constexpr float kSettingsUpExtra = 0.19f;

XrQuaternionf QuatFromYaw(float yaw)
{
	return XrQuaternionf{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

/** Gira um vetor pelo inverso de um quaternion unitario. */
XrVector3f RotateByInverse(const XrQuaternionf &q, const XrVector3f &v)
{
	// v' = q* v q, com q* = (-x, -y, -z, w). Escrito como duas produtos
	// vetoriais em vez de montar a matriz: sao menos operacoes e nao ha matriz
	// para manter em lugar nenhum.
	const XrVector3f u{-q.x, -q.y, -q.z};
	const XrVector3f t{
		2.0f * (u.y * v.z - u.z * v.y),
		2.0f * (u.z * v.x - u.x * v.z),
		2.0f * (u.x * v.y - u.y * v.x)};
	return XrVector3f{
		v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
		v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
		v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

/** Gira um vetor por um quaternion unitario. */
XrVector3f RotateVector(const XrQuaternionf &q, const XrVector3f &v)
{
	const XrVector3f u{q.x, q.y, q.z};
	const XrVector3f t{
		2.0f * (u.y * v.z - u.z * v.y),
		2.0f * (u.z * v.x - u.x * v.z),
		2.0f * (u.x * v.y - u.y * v.x)};
	return XrVector3f{
		v.x + q.w * t.x + (u.y * t.z - u.z * t.y),
		v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
		v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// So o yaw interessa na recentragem: inclinar a tela junto com a cabeca
// enjoa e nao ajuda em nada.
float YawFromQuat(const XrQuaternionf &q)
{
	return std::atan2(2.0f * (q.w * q.y + q.x * q.z),
	                  1.0f - 2.0f * (q.y * q.y + q.x * q.x));
}

float PitchFromQuat(const XrQuaternionf &q)
{
	float sin_p = 2.0f * (q.x * q.w - q.y * q.z);
	if(sin_p > 1.0f) sin_p = 1.0f;
	if(sin_p < -1.0f) sin_p = -1.0f;
	float cos_p = std::sqrt(std::max(0.0f, 1.0f - sin_p * sin_p));
	return std::atan2(sin_p, cos_p);
}

XrQuaternionf QuatFromYawPitch(float yaw, float pitch)
{
	float cy = std::cos(yaw * 0.5f);
	float sy = std::sin(yaw * 0.5f);
	float cp = std::cos(pitch * 0.5f);
	float sp = std::sin(pitch * 0.5f);
	return XrQuaternionf{ cy * sp, sy * cp, -sy * sp, cy * cp };
}

XrQuaternionf QuatMul(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return XrQuaternionf{
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
}

// Keep 3D surround-track side wings proportional to screen height so vertical
// videos retain full semicircle turnaround room without edge compression.
float ComputeTrackSpanX(float videoAspect)
{
	float a = std::clamp(videoAspect, 0.35f, 3.20f);
	return std::clamp(1.0f + 0.70f / a, 1.45f, 2.50f);
}

} // namespace

XrVideoSession::~XrVideoSession()
{
	Destroy();
}

bool XrVideoSession::Create(JavaVM *vm, jobject activity)
{
	vm_ = vm;

	JNIEnv *env = nullptr;
	if(vm_->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		LOGE("GetEnv failed while creating the session");
		return false;
	}
	activity_ = env->NewGlobalRef(activity);

	// No Android o loader precisa da VM e do Context antes de qualquer outra
	// chamada OpenXR, senao nao localiza o runtime do Horizon OS.
	PFN_xrInitializeLoaderKHR pfnInitializeLoader = nullptr;
	if(XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			(PFN_xrVoidFunction *)&pfnInitializeLoader)) && pfnInitializeLoader)
	{
		XrLoaderInitInfoAndroidKHR loader_init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		loader_init.applicationVM = vm_;
		loader_init.applicationContext = activity_;
		pfnInitializeLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loader_init);
	}
	else
	{
		LOGW("xrInitializeLoaderKHR unavailable; carrying on anyway");
	}

	uint32_t ext_count = 0;
	if(XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr)))
	{
		LOGE("Could not enumerate OpenXR extensions");
		return false;
	}
	std::vector<XrExtensionProperties> available(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, available.data());

	// A lista inteira, e nao so as que ligamos.
	//
	// Ate aqui o log dizia apenas "Extensao habilitada: X" para cada uma das
	// nossas, o que responde "o que pedimos" e nunca "o que existe". Toda
	// pergunta sobre o que mais este aparelho oferece terminava em suposicao.
	//
	// Quebrada em linhas de oito nomes: o log do Android corta mensagem em
	// torno de 4 KB, e uma lista de cem extensoes passa disso com folga --
	// sairia truncada exatamente onde comeca a ficar interessante.
	{
		LOGI("Extensions the runtime offers: %u", ext_count);
		std::string line;
		int on_line = 0;
		for(uint32_t i = 0; i < ext_count; i++)
		{
			line += "  ";
			line += available[i].extensionName;
			if(++on_line == 8 || i + 1 == ext_count)
			{
				LOGI("  available:%s", line.c_str());
				line.clear();
				on_line = 0;
			}
		}
	}

	auto has_ext = [&available](const char *name) {
		for(const auto &e : available)
			if(strcmp(e.extensionName, name) == 0)
				return true;
		return false;
	};

	std::vector<const char *> extensions;
	// Obrigatorias: sem qualquer uma delas o projeto nao existe.
	const char *required[] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
		XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
	};
	for(const char *name : required)
	{
		if(!has_ext(name))
		{
			last_error_ = std::string("Required OpenXR extension missing: ") + name;
			LOGE("%s", last_error_.c_str());
			return false;
		}
		extensions.push_back(name);
	}
	// Opcional: sem passthrough o app cai para ambiente escuro.
	if(has_ext(XR_FB_PASSTHROUGH_EXTENSION_NAME))
	{
		extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
		passthrough_supported_ = true;
	}
	else
	{
		LOGW("XR_FB_passthrough unavailable; mixed mode disabled");
	}

	// Opcionais de qualidade. Nenhuma delas e critica: se faltar, o app roda
	// igual, so mais macio, com cor menos exata ou com judder.
	// A Meta trata esta como obrigatoria na pratica: a especificacao dela diz
	// que XrAndroidSurfaceSwapchainCreateInfoFB *must* estar no next chain do
	// XrSwapchainCreateInfo ao chamar xrCreateSwapchainAndroidSurfaceKHR. Sem
	// ela o swapchain e criado, o MediaCodec decodifica normalmente, e o
	// runtime morre ao compor a camada.
	if(has_ext(XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME))
	{
		surface_swapchain_create_supported_ = true;
		extensions.push_back(XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME);
	}
	else
		LOGW("XR_FB_android_surface_swapchain_create unavailable");

	// A imagem chega espelhada verticalmente: o BufferQueue do Android tem
	// origem no canto superior esquerdo e o compositor amostra pela convencao
	// do GL, com origem embaixo. Esta extensao existe exatamente para isso, e e
	// a unica correcao que nao custa uma passada de GPU -- espelhamento nao se
	// faz com pose, porque rotacao rigida nunca produz reflexo.
	if(has_ext(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME))
	{
		image_layout_supported_ = true;
		extensions.push_back(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
	}
	else
		LOGW("XR_FB_composition_layer_image_layout unavailable; the image will be mirrored");

	if(has_ext(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME))
	{
		extensions.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
		layer_settings_supported_ = true;
	}
	if(has_ext(XR_FB_COLOR_SPACE_EXTENSION_NAME))
	{
		extensions.push_back(XR_FB_COLOR_SPACE_EXTENSION_NAME);
		color_space_supported_ = true;
	}
	if(has_ext(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
	{
		extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
		refresh_rate_supported_ = true;
	}

	// Atenuacao local do painel.
	//
	// O Quest 3 tem retroiluminacao em zonas, e por padrao ela fica no modo
	// conservador porque uma interface comum tem branco espalhado pela tela
	// inteira. Uma camada de video em quarto escuro e o caso oposto: quase tudo
	// e preto, e sem atenuacao local esse preto sai cinza -- que e o que faz uma
	// cena noturna parecer lavada.
	//
	// So vale no modo escuro. Com passthrough ligado o compositor mostra o
	// quarto, o painel tem conteudo claro em toda parte, e ligar isto nao muda
	// nada -- por isso o modo e escolhido por frame, e nao uma vez na abertura.
	if(has_ext(XR_META_LOCAL_DIMMING_EXTENSION_NAME))
	{
		extensions.push_back(XR_META_LOCAL_DIMMING_EXTENSION_NAME);
		local_dimming_supported_ = true;
	}
	else
		LOGW("XR_META_local_dimming unavailable; black stays panel black");

	// Declara ao sistema qual thread desenha. Sem isso a thread do frame loop e
	// tratada como qualquer outra: pode ser escalonada num nucleo pequeno e
	// perder a fatia de tempo bem na hora de submeter o frame, o que aparece
	// como engasgo esporadico e nao como lentidao constante.
	if(has_ext(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
	{
		extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
		thread_settings_supported_ = true;
	}
	else
		LOGW("XR_KHR_android_thread_settings unavailable; no render priority");

	// Deixa o runtime escolher o filtro de cada camada em vez de impor um.
	//
	// E o caminho certo depois do que o MQSR fez aqui: pedir explicitamente o
	// sharpening de qualidade produziu um disco claro acompanhando a cabeca, nas
	// duas formas de tela. Esta extensao inverte a decisao -- o compositor sabe
	// o tamanho aparente da camada, a densidade do painel e o que esta em volta,
	// e nos nao sabemos nada disso do lado de ca.
	if(has_ext(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME))
	{
		extensions.push_back(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME);
		auto_filter_supported_ = true;
	}

	// O runtime sabe dizer em que resolucao a camada deveria estar para casar
	// com a densidade do painel no tamanho em que ela aparece. Nao da para mudar
	// a resolucao da fonte -- 1080p e o teto do Remote Play --, mas da para
	// dizer ao usuario se a tela em que ele esta jogando desperdica pixels ou
	// pede mais do que existe, que e a informacao que falta para escolher o
	// tamanho com criterio em vez de por gosto.
	if(has_ext(XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME))
	{
		extensions.push_back(XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME);
		recommended_resolution_supported_ = true;
	}

	// Brilho e contraste da camada, aplicados pelo compositor.
	//
	// O caminho direto nao tem shader por definicao -- e o que o torna direto --,
	// entao ate agora nao havia nenhuma forma de mexer na imagem sem sair dele.
	// Isto e escala e viés por canal, feito na composicao: nao custa passada de
	// GPU, nao custa copia, e serve exatamente para amansar os brancos altos que
	// os 10 bits trazem.
	//
	// Nao substitui o mapeamento de tons: escala e reta, e curva PQ nao. Ela
	// abaixa a imagem inteira em vez de comprimir so as altas luzes. Ameniza, e
	// e o que da para ter de graca.
	if(has_ext(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME))
	{
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
		color_scale_supported_ = true;
	}

	if(has_ext(XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME))
	{
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME);
		equirect2_supported_ = true;
		LOGI("XR_KHR_composition_layer_equirect2 supported and enabled");
	}
	if(has_ext(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME))
	{
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
		equirect_supported_ = true;
		LOGI("XR_KHR_composition_layer_equirect supported and enabled");
	}
	if(has_ext(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
	{
		extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
		hand_tracking_supported_ = true;
		if(has_ext(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME))
		{
			extensions.push_back(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME);
			hand_aim_supported_ = true;
		}
		if(has_ext(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME))
		{
			extensions.push_back(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
			hand_data_source_supported_ = true;
		}
		if(has_ext(XR_META_HAND_TRACKING_FREQUENCY_HINT_EXTENSION_NAME))
		{
			extensions.push_back(XR_META_HAND_TRACKING_FREQUENCY_HINT_EXTENSION_NAME);
			hand_freq_hint_supported_ = true;
		}
		LOGI("Hand tracking extensions enabled (aim=%d dataSrc=%d freqHint=%d)",
				(int)hand_aim_supported_, (int)hand_data_source_supported_, (int)hand_freq_hint_supported_);
	}

	// XR_EXT_local_floor existe neste aparelho e fica de fora de proposito.
	//
	// Com ele a origem passaria a ser o chao em vez da cabeca na abertura, e a
	// altura salva -- que hoje significa "em relacao aos seus olhos" -- passaria
	// a significar "em relacao ao chao". A tela iria parar nos pes na primeira
	// abertura, e a unica saida seria zerar o ajuste de todo mundo. O ganho
	// seria uma altura estavel entre sessoes; o custo, quebrar a que ja esta
	// ajustada. Nao compensa.
	//
	// Saber quando o headset sai da cabeca. Por enquanto so registra: pausar o
	// stream sozinho seria decidir pelo usuario que tirar o aparelho por dez
	// segundos significa parar de jogar, e nem sempre significa.
	if(has_ext(XR_EXT_USER_PRESENCE_EXTENSION_NAME))
		extensions.push_back(XR_EXT_USER_PRESENCE_EXTENSION_NAME);

	// Nivel de CPU e GPU declarado, em vez de deixado ao governador.
	//
	// Sem isto o sistema decide pela carga observada, e o padrao dele e
	// conservador: um app que gasta pouca GPU -- que e o nosso caso, o
	// compositor faz quase tudo -- e lido como app que pode rodar em nucleo
	// pequeno e relogio baixo. O custo aparece como engasgo esporadico quando a
	// decodificacao e a submissao caem no mesmo instante.
	if(has_ext(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
	{
		extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
		perf_settings_supported_ = true;
	}

	// Contadores do compositor: quadros descartados, tempo de quadro, uso de
	// GPU. E a metade da cadeia que nunca teve numero nenhum -- do lado da rede
	// o chiaki ja contava pacotes, mas do compositor para ca so havia
	// impressao.
	if(has_ext(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME))
	{
		extensions.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
		perf_metrics_supported_ = true;
	}

	XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_info.applicationVM = vm_;
	android_info.applicationActivity = activity_;

	XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
	instance_info.next = &android_info;
	instance_info.enabledExtensionCount = (uint32_t)extensions.size();
	instance_info.enabledExtensionNames = extensions.data();
	strcpy(instance_info.applicationInfo.applicationName, "P5M");
	instance_info.applicationInfo.applicationVersion = 1;
	strcpy(instance_info.applicationInfo.engineName, "P5M");
	instance_info.applicationInfo.engineVersion = 1;
	instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	for(const char *name : extensions)
		LOGI("Extension enabled: %s", name);

	XR_CHECK(XR_NULL_HANDLE, xrCreateInstance(&instance_info, &instance_), "xrCreateInstance");

	XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(instance_, xrGetSystem(instance_, &system_info, &system_id_), "xrGetSystem");

	{
		XrSystemPassthroughProperties2FB pt_props{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
		XrSystemHandTrackingPropertiesEXT ht_props{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
		XrSystemProperties sys_props{XR_TYPE_SYSTEM_PROPERTIES};
		if(passthrough_supported_)
		{
			sys_props.next = &pt_props;
			if(hand_tracking_supported_)
				pt_props.next = &ht_props;
		}
		else if(hand_tracking_supported_)
		{
			sys_props.next = &ht_props;
		}
		if(XR_SUCCEEDED(xrGetSystemProperties(instance_, system_id_, &sys_props)))
		{
			if(passthrough_supported_)
				passthrough_supported_ = (pt_props.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) != 0;
			if(hand_tracking_supported_)
			{
				hand_tracking_supported_ = (ht_props.supportsHandTracking == XR_TRUE);
				if(!hand_tracking_supported_)
					hand_aim_supported_ = false;
				LOGI("System hand tracking support=%d (aim=%d)",
						(int)hand_tracking_supported_, (int)hand_aim_supported_);
			}
			if(sys_props.graphicsProperties.maxSwapchainImageWidth > 0)
				max_swapchain_width_ = sys_props.graphicsProperties.maxSwapchainImageWidth;
			if(sys_props.graphicsProperties.maxSwapchainImageHeight > 0)
				max_swapchain_height_ = sys_props.graphicsProperties.maxSwapchainImageHeight;
			LOGI("System graphics caps: maxSwapchain=%ux%u maxLayers=%u",
					sys_props.graphicsProperties.maxSwapchainImageWidth,
					sys_props.graphicsProperties.maxSwapchainImageHeight,
					sys_props.graphicsProperties.maxLayerCount);
		}
	}

	if(!LoadExtensionFunctions())
		return false;

	// A extensao GL ES exige que xrGetOpenGLESGraphicsRequirementsKHR seja
	// chamada antes de xrCreateSession, mesmo que ignoremos o resultado.
	PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetReq = nullptr;
	xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
			(PFN_xrVoidFunction *)&pfnGetReq);
	if(pfnGetReq)
	{
		XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
		pfnGetReq(instance_, system_id_, &reqs);
	}

	if(!InitEgl())
		return false;

	XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	binding.display = egl_display_;
	binding.config = egl_config_;
	binding.context = egl_context_;

	XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
	session_info.next = &binding;
	session_info.systemId = system_id_;
	XR_CHECK(instance_, xrCreateSession(instance_, &session_info, &session_), "xrCreateSession");

	XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation = XrQuaternionf{0, 0, 0, 1};
	space_info.poseInReferenceSpace.position = XrVector3f{0, 0, 0};
	XR_CHECK(instance_, xrCreateReferenceSpace(session_, &space_info, &app_space_),
			"xrCreateReferenceSpace(LOCAL)");

	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(instance_, xrCreateReferenceSpace(session_, &space_info, &view_space_),
			"xrCreateReferenceSpace(VIEW)");

	// O app nao renderiza geometria, entao em tese nao precisa saber nada sobre
	// as views. Mas todo app OpenXR faz este handshake, e um runtime pode muito
	// bem so montar seu estado de view quando ele acontece -- e estado de view
	// faltando e exatamente o tipo de coisa que faz o xrEndFrame saltar para um
	// ponteiro nulo. Custa uma chamada na criacao.
	{
		uint32_t count = 0;
		XrResult res = xrEnumerateViewConfigurationViews(instance_, system_id_,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
		if(XR_SUCCEEDED(res) && count > 0)
		{
			std::vector<XrViewConfigurationView> config(count,
					{XR_TYPE_VIEW_CONFIGURATION_VIEW});
			res = xrEnumerateViewConfigurationViews(instance_, system_id_,
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, config.data());
			if(XR_SUCCEEDED(res))
			{
				view_count_ = count;
				views_.assign(count, XrView{XR_TYPE_VIEW});
				LOGI("Views: %u, recommended %ux%u", count,
						config[0].recommendedImageRectWidth,
						config[0].recommendedImageRectHeight);
			}
		}
		if(view_count_ == 0)
			LOGW("xrEnumerateViewConfigurationViews failed: %s", XrResultStr(instance_, res));
	}

	ApplyColorSpace();

	if(passthrough_supported_ && !InitPassthrough())
	{
		LOGW("Passthrough failed to initialize; carrying on without it");
		passthrough_supported_ = false;
	}

	if(hand_tracking_supported_ && pfnCreateHandTrackerEXT_)
	{
		for(int h = 0; h < 2; h++)
		{
			XrHandTrackingDataSourceEXT req_sources[1] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
			XrHandTrackingDataSourceInfoEXT ds_info{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
			ds_info.requestedDataSourceCount = 1;
			ds_info.requestedDataSources = req_sources;

			XrHandTrackerCreateInfoEXT ht_info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
			ht_info.next = hand_data_source_supported_ ? &ds_info : nullptr;
			ht_info.hand = (h == 0) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
			ht_info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
			XrResult hres = pfnCreateHandTrackerEXT_(session_, &ht_info, &hand_trackers_[h]);
			if(XR_FAILED(hres) && hand_data_source_supported_)
			{
				ht_info.next = nullptr;
				hres = pfnCreateHandTrackerEXT_(session_, &ht_info, &hand_trackers_[h]);
			}
			if(XR_FAILED(hres))
			{
				LOGW("xrCreateHandTrackerEXT(hand=%d) failed: %s", h, XrResultStr(instance_, hres));
				hand_trackers_[h] = XR_NULL_HANDLE;
			}
		}
		if(hand_trackers_[0] == XR_NULL_HANDLE && hand_trackers_[1] == XR_NULL_HANDLE)
		{
			hand_tracking_supported_ = false;
			hand_aim_supported_ = false;
		}
		else
		{
			if(hand_freq_hint_supported_ && pfnSetHandTrackingFrequencyHintMETA_)
			{
				XrResult fres = pfnSetHandTrackingFrequencyHintMETA_(
						session_, XR_HAND_TRACKING_FREQUENCY_HINT_HIGH_META);
				LOGI("Hand tracking frequency hint HIGH: %s", XrResultStr(instance_, fres));
			}
			LOGI("Hand trackers created (L=%d, R=%d)",
					hand_trackers_[0] != XR_NULL_HANDLE, hand_trackers_[1] != XR_NULL_HANDLE);
		}
	}

	// Antes de soltar o contexto, e nao depois: o swapchain do painel e de
	// textura GL, e o runtime precisa de um contexto corrente para aloca-la.
	// Criado depois da soltura, ele falhava com XR_ERROR_RUNTIME_FAILURE em
	// qualquer combinacao de usage e formato -- a mensagem nao dizia nada sobre
	// contexto, e o swapchain de video escapava por ser Surface e nao GL.
	if(!InitHudSwapchain())
		LOGW("Help panel unavailable; tuning mode will have no caption");
	if(!CreateSettingsSwapchain())
		LOGW("Settings panel unavailable");
	if(!InitOsdSwapchain())
		LOGW("OSD panel unavailable");
	if(!InitResSwapchain())
		LOGW("Res badge panel unavailable");
	if(!InitDanmakuSwapchain())
		LOGW("Danmaku panel unavailable");
	if(!InitCursorSwapchain())
		LOGW("Cursor panel unavailable");
	if(!InitCornerSwapchain())
		LOGW("Corner resize handle unavailable");

	// Solta o contexto nesta thread. Ele foi criado e ficou corrente aqui para
	// abrir a sessao, mas quem vai usa-lo e a thread do frame loop -- e o EGL
	// so permite um contexto corrente em uma thread por vez. Sem isto o
	// eglMakeCurrent do loop falha com EGL_BAD_ACCESS e a thread que chama
	// xrEndFrame fica sem contexto nenhum.
	if(!eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
		LOGW("eglMakeCurrent(release) failed: 0x%x", eglGetError());

	LOGI("OpenXR session created (passthrough=%d)", (int)passthrough_supported_);
	return true;
}

bool XrVideoSession::LoadExtensionFunctions()
{
	auto get = [this](const char *name, PFN_xrVoidFunction *out) {
		XrResult res = xrGetInstanceProcAddr(instance_, name, out);
		if(XR_FAILED(res))
			LOGW("xrGetInstanceProcAddr(%s) failed", name);
		return XR_SUCCEEDED(res);
	};

	if(!get("xrCreateSwapchainAndroidSurfaceKHR",
			(PFN_xrVoidFunction *)&pfnCreateSwapchainAndroidSurfaceKHR))
	{
		LOGE("xrCreateSwapchainAndroidSurfaceKHR unavailable");
		return false;
	}

	if(color_space_supported_ && !get("xrSetColorSpaceFB", (PFN_xrVoidFunction *)&pfnSetColorSpaceFB))
		color_space_supported_ = false;
	if(refresh_rate_supported_)
	{
		get("xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&pfnEnumerateDisplayRefreshRatesFB);
		get("xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&pfnRequestDisplayRefreshRateFB);
		if(!pfnEnumerateDisplayRefreshRatesFB || !pfnRequestDisplayRefreshRateFB)
			refresh_rate_supported_ = false;
	}
	if(thread_settings_supported_)
	{
		get("xrSetAndroidApplicationThreadKHR",
				(PFN_xrVoidFunction *)&pfnSetAndroidApplicationThreadKHR);
		if(!pfnSetAndroidApplicationThreadKHR)
			thread_settings_supported_ = false;
	}
	if(recommended_resolution_supported_)
	{
		get("xrGetRecommendedLayerResolutionMETA",
				(PFN_xrVoidFunction *)&pfnGetRecommendedLayerResolutionMETA);
		if(!pfnGetRecommendedLayerResolutionMETA)
			recommended_resolution_supported_ = false;
	}

	if(perf_settings_supported_)
	{
		get("xrPerfSettingsSetPerformanceLevelEXT",
				(PFN_xrVoidFunction *)&pfnPerfSettingsSetPerformanceLevelEXT);
		get("xrThermalGetTemperatureTrendEXT",
				(PFN_xrVoidFunction *)&pfnThermalGetTemperatureTrendEXT);
		if(!pfnPerfSettingsSetPerformanceLevelEXT)
			perf_settings_supported_ = false;
	}

	if(perf_metrics_supported_)
	{
		get("xrEnumeratePerformanceMetricsCounterPathsMETA",
				(PFN_xrVoidFunction *)&pfnEnumeratePerformanceMetricsCounterPathsMETA);
		get("xrSetPerformanceMetricsStateMETA",
				(PFN_xrVoidFunction *)&pfnSetPerformanceMetricsStateMETA);
		get("xrQueryPerformanceMetricsCounterMETA",
				(PFN_xrVoidFunction *)&pfnQueryPerformanceMetricsCounterMETA);
		if(!pfnSetPerformanceMetricsStateMETA || !pfnQueryPerformanceMetricsCounterMETA
				|| !pfnEnumeratePerformanceMetricsCounterPathsMETA)
			perf_metrics_supported_ = false;
	}

	if(passthrough_supported_)
	{
		get("xrPassthroughLayerSetStyleFB", (PFN_xrVoidFunction *)&pfnPassthroughLayerSetStyleFB);
		get("xrCreatePassthroughFB", (PFN_xrVoidFunction *)&pfnCreatePassthroughFB);
		get("xrDestroyPassthroughFB", (PFN_xrVoidFunction *)&pfnDestroyPassthroughFB);
		get("xrPassthroughStartFB", (PFN_xrVoidFunction *)&pfnPassthroughStartFB);
		get("xrPassthroughPauseFB", (PFN_xrVoidFunction *)&pfnPassthroughPauseFB);
		get("xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&pfnCreatePassthroughLayerFB);
		get("xrDestroyPassthroughLayerFB", (PFN_xrVoidFunction *)&pfnDestroyPassthroughLayerFB);
		get("xrPassthroughLayerResumeFB", (PFN_xrVoidFunction *)&pfnPassthroughLayerResumeFB);
		get("xrPassthroughLayerPauseFB", (PFN_xrVoidFunction *)&pfnPassthroughLayerPauseFB);

		if(!pfnCreatePassthroughFB || !pfnCreatePassthroughLayerFB || !pfnPassthroughStartFB)
			passthrough_supported_ = false;
	}

	if(hand_tracking_supported_)
	{
		get("xrCreateHandTrackerEXT", (PFN_xrVoidFunction *)&pfnCreateHandTrackerEXT_);
		get("xrDestroyHandTrackerEXT", (PFN_xrVoidFunction *)&pfnDestroyHandTrackerEXT_);
		get("xrLocateHandJointsEXT", (PFN_xrVoidFunction *)&pfnLocateHandJointsEXT_);
		if(hand_freq_hint_supported_)
		{
			xrGetInstanceProcAddr(instance_, "xrSetHandTrackingFrequencyHintMETA",
					(PFN_xrVoidFunction *)&pfnSetHandTrackingFrequencyHintMETA_);
		}
		if(!pfnCreateHandTrackerEXT_ || !pfnDestroyHandTrackerEXT_ || !pfnLocateHandJointsEXT_)
		{
			hand_tracking_supported_ = false;
			hand_aim_supported_ = false;
		}
	}
	return true;
}

bool XrVideoSession::InitEgl()
{
	// O OpenXR exige um binding grafico para abrir a sessao. Como nao
	// desenhamos nada, este contexto so existe para satisfazer o runtime:
	// uma pbuffer 1x1 basta.
	egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if(egl_display_ == EGL_NO_DISPLAY)
	{
		LOGE("eglGetDisplay failed");
		return false;
	}
	EGLint major = 0, minor = 0;
	if(!eglInitialize(egl_display_, &major, &minor))
	{
		LOGE("eglInitialize failed");
		return false;
	}

	const EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
		EGL_NONE
	};
	EGLint num_configs = 0;
	if(!eglChooseConfig(egl_display_, config_attribs, &egl_config_, 1, &num_configs) || num_configs < 1)
	{
		LOGE("eglChooseConfig returned no valid configuration");
		return false;
	}

	const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, context_attribs);
	if(egl_context_ == EGL_NO_CONTEXT)
	{
		LOGE("eglCreateContext failed");
		return false;
	}

	const EGLint pbuffer_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
	egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config_, pbuffer_attribs);
	if(egl_surface_ == EGL_NO_SURFACE)
	{
		LOGE("eglCreatePbufferSurface failed");
		return false;
	}
	if(!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
	{
		LOGE("eglMakeCurrent failed");
		return false;
	}

	// Shared EGL context for the AI depth stage thread. Sharing the main
	// context lets depth_tex_ be uploaded off the XR frame loop while the
	// non-shareable objects (FBO, VAO) stay with the renderer.
	stage_egl_context_ = eglCreateContext(egl_display_, egl_config_,
			egl_context_, context_attribs);
	stage_egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config_,
			pbuffer_attribs);
	if(stage_egl_context_ == EGL_NO_CONTEXT || stage_egl_surface_ == EGL_NO_SURFACE)
	{
		LOGW("AI stage EGL unavailable (0x%x); depth stays on the frame loop",
				eglGetError());
		if(stage_egl_surface_ != EGL_NO_SURFACE)
			eglDestroySurface(egl_display_, stage_egl_surface_);
		if(stage_egl_context_ != EGL_NO_CONTEXT)
			eglDestroyContext(egl_display_, stage_egl_context_);
		stage_egl_surface_ = EGL_NO_SURFACE;
		stage_egl_context_ = EGL_NO_CONTEXT;
	}
	else if(!tone_mapper_.InitStageEgl(egl_display_, stage_egl_surface_,
			stage_egl_context_))
	{
		LOGW("AI stage thread could not start; depth stays on the frame loop");
	}
	else
	{
		LOGI("AI stage shared EGL context created");
	}

	LogGlCapabilities();
	return true;
}

/**
 * A lista de extensoes do GL, pelo mesmo motivo da lista do OpenXR.
 *
 * Tudo que se discutiu sobre upscaler e sintese de quadros na Adreno --
 * GL_QCOM_motion_estimation, processamento de imagem, o que mais houver -- e
 * documentacao da Qualcomm ate alguem perguntar a este aparelho. E ninguem
 * tinha perguntado: o contexto GL existe desde o primeiro commit e nunca
 * dissemos uma linha sobre o que ele oferece.
 *
 * Pelo indice, e nao pelo glGetString(GL_EXTENSIONS) de uma string so: em ES 3
 * a forma indexada e a correta, e a string unica pode vir truncada ou nula
 * conforme o driver.
 */
void XrVideoSession::LogGlCapabilities()
{
	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	LOGI("GL: %s / %s / %s", vendor ? vendor : "?", renderer ? renderer : "?",
			version ? version : "?");

	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	if(count <= 0)
	{
		LOGW("GL did not return extensions by index");
		return;
	}

	LOGI("GL extensions available: %d", (int)count);
	std::string line;
	int on_line = 0;
	for(GLint i = 0; i < count; i++)
	{
		const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if(!name)
			continue;
		line += "  ";
		line += name;
		// Quatro por linha: nome de extensao GL e mais longo que o de OpenXR, e
		// o log do Android corta perto de 4 KB.
		if(++on_line == 4 || i + 1 == count)
		{
			LOGI("  gl:%s", line.c_str());
			line.clear();
			on_line = 0;
		}
	}
}

void XrVideoSession::DestroyEgl()
{
	if(egl_display_ != EGL_NO_DISPLAY)
	{
		if(danmaku_clear_fbo_ != 0)
		{
			if(eglGetCurrentContext() != EGL_NO_CONTEXT)
				glDeleteFramebuffers(1, &danmaku_clear_fbo_);
			danmaku_clear_fbo_ = 0;
		}
		eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if(egl_surface_ != EGL_NO_SURFACE)
			eglDestroySurface(egl_display_, egl_surface_);
		if(egl_context_ != EGL_NO_CONTEXT)
			eglDestroyContext(egl_display_, egl_context_);
		if(stage_egl_surface_ != EGL_NO_SURFACE)
			eglDestroySurface(egl_display_, stage_egl_surface_);
		if(stage_egl_context_ != EGL_NO_CONTEXT)
			eglDestroyContext(egl_display_, stage_egl_context_);
		eglTerminate(egl_display_);
	}
	egl_display_ = EGL_NO_DISPLAY;
	egl_surface_ = EGL_NO_SURFACE;
	egl_context_ = EGL_NO_CONTEXT;
	stage_egl_surface_ = EGL_NO_SURFACE;
	stage_egl_context_ = EGL_NO_CONTEXT;
}

bool XrVideoSession::InitPassthrough()
{
	XrPassthroughCreateInfoFB pt_info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
	XR_CHECK(instance_, pfnCreatePassthroughFB(session_, &pt_info, &passthrough_),
			"xrCreatePassthroughFB");

	XrPassthroughLayerCreateInfoFB layer_info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
	layer_info.passthrough = passthrough_;
	layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	XR_CHECK(instance_, pfnCreatePassthroughLayerFB(session_, &layer_info, &passthrough_layer_),
			"xrCreatePassthroughLayerFB");

	// Criado ligado, mas so entra na composicao quando o usuario pedir.
	XR_CHECK(instance_, pfnPassthroughStartFB(passthrough_), "xrPassthroughStartFB");
	if(pfnPassthroughLayerPauseFB)
		pfnPassthroughLayerPauseFB(passthrough_layer_);
	return true;
}

void XrVideoSession::ApplyColorSpace()
{
	if(!color_space_supported_)
		return;

	// O padrao do Horizon OS e Display P3, mais amplo que qualquer coisa que o
	// Remote Play envie. Sem fixar, vermelho e verde saem estourados: parece
	// "vivo", mas e cor errada.
	//
	// Qual espaco fixar depende da profundidade. Em 8 bits o console manda
	// Rec.709. Em 10 bits vem o perfil HDR, e com ele o gamut BT.2020 -- que
	// continua BT.2020 mesmo depois de o decodificador mapear a curva para SDR,
	// porque mapeamento de tons trata de luminancia, nao de primarias. Declarar
	// 709 sobre conteudo 2020 faz o compositor esticar as cores para um gamut
	// que nao e o delas, e o resultado e imagem clara e lavada.
	const bool wide = wide_color_.load();
	const XrColorSpaceFB space = wide ? XR_COLOR_SPACE_REC2020_FB : XR_COLOR_SPACE_REC709_FB;
	XrResult res = pfnSetColorSpaceFB(session_, space);
	if(XR_FAILED(res))
		LOGW("xrSetColorSpaceFB(%s) failed: %s", wide ? "REC2020" : "REC709",
				XrResultStr(instance_, res));
	else
		LOGI("Color space pinned to %s", wide ? "Rec.2020" : "Rec.709");
}

void XrVideoSession::SetWideColor(bool wide)
{
	if(wide_color_.exchange(wide) == wide)
		return;
	if(session_ != XR_NULL_HANDLE)
		ApplyColorSpace();
}

bool XrVideoSession::CreateQuadSwapchain(int32_t width, int32_t height,
		XrSwapchain *out_swapchain, std::vector<XrSwapchainImageOpenGLESKHR> *out_images,
		const char *tag)
{
	uint32_t format_count = 0;
	if(XR_FAILED(xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr))
			|| format_count == 0)
		return false;
	std::vector<int64_t> formats(format_count);
	if(XR_FAILED(xrEnumerateSwapchainFormats(session_, format_count, &format_count,
			formats.data())))
		return false;

	const int64_t kSrgb8Alpha8 = 0x8C43;
	const int64_t kRgba8 = 0x8058;
	int64_t chosen = 0;
	for(int64_t candidate : { kSrgb8Alpha8, kRgba8 })
	{
		for(int64_t f : formats)
		{
			if(f == candidate)
			{
				chosen = candidate;
				break;
			}
		}
		if(chosen != 0)
			break;
	}
	if(chosen == 0)
	{
		std::string list;
		for(int64_t f : formats)
		{
			char buf[16];
			snprintf(buf, sizeof(buf), "0x%llx ", (unsigned long long)f);
			list += buf;
		}
		LOGW("No 8-bit RGBA format for %s panel. Available: %s", tag, list.c_str());
		return false;
	}

	const struct { XrSwapchainUsageFlags usage; const char *name; } kUsages[] = {
		{ XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
				"SAMPLED|COLOR_ATTACHMENT" },
		{ XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
				| XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, "SAMPLED|COLOR_ATTACHMENT|TRANSFER_DST" },
		{ XR_SWAPCHAIN_USAGE_SAMPLED_BIT, "SAMPLED" },
		{ XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT" },
	};

	LOGI("%s panel: format 0x%llx chosen out of %u available", tag,
			(unsigned long long)chosen, format_count);

	XrResult res = XR_ERROR_RUNTIME_FAILURE;
	for(const auto &attempt : kUsages)
	{
		XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		info.usageFlags = attempt.usage;
		info.format = chosen;
		info.sampleCount = 1;
		info.width = (uint32_t)width;
		info.height = (uint32_t)height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		res = xrCreateSwapchain(session_, &info, out_swapchain);
		if(XR_SUCCEEDED(res))
		{
			LOGI("%s panel: usage %s accepted", tag, attempt.name);
			break;
		}
		LOGW("%s panel: usage %s refused (%s)", tag, attempt.name,
				XrResultStr(instance_, res));
		*out_swapchain = XR_NULL_HANDLE;
	}
	if(XR_FAILED(res))
		return false;

	uint32_t image_count = 0;
	if(XR_FAILED(xrEnumerateSwapchainImages(*out_swapchain, 0, &image_count, nullptr))
			|| image_count == 0)
		return false;
	out_images->assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
	if(XR_FAILED(xrEnumerateSwapchainImages(*out_swapchain, image_count, &image_count,
			reinterpret_cast<XrSwapchainImageBaseHeader *>(out_images->data()))))
		return false;

	LOGI("%s panel: %dx%d, format 0x%llx, %u images", tag, width, height,
			(unsigned long long)chosen, image_count);
	return true;
}

bool XrVideoSession::InitHudSwapchain()
{
	return CreateQuadSwapchain(kHudWidth, kHudHeight, &hud_swapchain_, &hud_images_, "HUD");
}

bool XrVideoSession::CreateSettingsSwapchain()
{
	return CreateQuadSwapchain(kSettingsWidth, kSettingsHeight, &settings_swapchain_, &settings_images_, "Settings");
}

bool XrVideoSession::InitOsdSwapchain()
{
	return CreateQuadSwapchain(kOsdWidth, kOsdHeight, &osd_swapchain_, &osd_images_, "OSD");
}

bool XrVideoSession::InitResSwapchain()
{
	return CreateQuadSwapchain(kResWidth, kResHeight, &res_swapchain_, &res_images_, "ResBadge");
}

bool XrVideoSession::InitDanmakuSwapchain()
{
	danmaku_pixels_.reserve(kDanmakuWidth * kDanmakuHeight * 4);
	bool ok1 = CreateQuadSwapchain(kDanmakuWidth, kDanmakuHeight, &danmaku_swapchain_, &danmaku_images_, "Danmaku");
	bool ok2 = CreateQuadSwapchain(kDanmakuWidth, kDanmakuHeight, &danmaku_swapchain_right_, &danmaku_images_right_, "DanmakuRight");
	danmaku_img_clear_gen_.assign(danmaku_images_.size(), 0);
	danmaku_img_clear_gen_right_.assign(danmaku_images_right_.size(), 0);
	danmaku_clear_gen_ = 1;
	danmaku_last_up_w_ = 0;
	danmaku_last_up_h_ = 0;
	danmaku_last_up_stereo_ = false;
	danmaku_right_has_image_.store(false);
	return ok1 && ok2;
}

bool XrVideoSession::InitCursorSwapchain()
{
	return CreateQuadSwapchain(kCursorSize, kCursorSize, &cursor_swapchain_, &cursor_images_, "Cursor");
}

bool XrVideoSession::InitCornerSwapchain()
{
	return CreateQuadSwapchain(64, 64, &corner_swapchain_, &corner_images_, "CornerHandle");
}

void XrVideoSession::SetSettingsBitmap(const uint8_t *pixels, int32_t width, int32_t height)
{
	if(!pixels || width <= 0 || height <= 0)
		return;
	{
		std::lock_guard<std::mutex> lock(settings_mutex_);
		settings_width_ = width;
		settings_height_ = height;
		settings_pixels_.assign(pixels, pixels + (size_t)width * height * 4);
	}
	settings_dirty_count_.store((int)std::max((size_t)3, settings_images_.size()));
}

bool XrVideoSession::UpdateSettingsLayer(XrCompositionLayerQuad &layer, float yaw, float pitch, const ScreenParams &params, const XrVector3f &recenter_pos)
{
	if(settings_swapchain_ == XR_NULL_HANDLE || settings_images_.empty())
		return false;

	if(settings_dirty_count_.load() > 0)
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(settings_swapchain_, &acquire, &index)))
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000;
			if(XR_SUCCEEDED(xrWaitSwapchainImage(settings_swapchain_, &wait)))
			{
				std::lock_guard<std::mutex> lock(settings_mutex_);
				if(!settings_pixels_.empty() && settings_width_ > 0 && settings_height_ > 0 && settings_width_ <= kSettingsWidth && settings_height_ <= kSettingsHeight)
				{
					glBindTexture(GL_TEXTURE_2D, settings_images_[index].image);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, settings_width_, settings_height_,
							GL_RGBA, GL_UNSIGNED_BYTE, settings_pixels_.data());
					glBindTexture(GL_TEXTURE_2D, 0);
					settings_has_image_.store(true);
				}
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(settings_swapchain_, &release);
				settings_dirty_count_.fetch_sub(1);
			}
			else
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(settings_swapchain_, &release);
			}
		}
	}

	if(!settings_has_image_.load())
		return false;

	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = app_space_;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = settings_swapchain_;
	layer.subImage.imageRect.offset = XrOffset2Di{0, 0};
	layer.subImage.imageRect.extent = XrExtent2Di{kSettingsWidth, kSettingsHeight};
	layer.subImage.imageArrayIndex = 0;

	float half_v_angle = std::atan(std::tan(params.centralAngle * 0.5f) / std::max(0.2f, params.aspectRatio));
	float height_angle = (params.radius > 0.1f) ? std::atan2(params.heightOffset, params.radius) : 0.0f;
	float up_angle = half_v_angle + kSettingsUpExtra;
	float settings_pitch = pitch + height_angle + up_angle;
	const float settings_dist = kFixedUiDist;

	const float settings_w = kFixedSettingsW;
	float settings_h = settings_w * (float)kSettingsHeight / (float)kSettingsWidth;
	layer.size = XrExtent2Df{settings_w, settings_h};

	float cos_p = std::cos(settings_pitch);
	float sin_p = std::sin(settings_pitch);
	layer.pose.orientation = QuatFromYawPitch(yaw, settings_pitch);
	layer.pose.position = XrVector3f{
		recenter_pos.x - std::sin(yaw) * cos_p * settings_dist,
		recenter_pos.y + sin_p * settings_dist,
		recenter_pos.z - std::cos(yaw) * cos_p * settings_dist
	};
	return true;
}

void XrVideoSession::SetHudBitmap(const uint8_t *pixels, int32_t width, int32_t height)
{
	if(!pixels || width <= 0 || height <= 0)
		return;
	{
		std::lock_guard<std::mutex> lock(hud_mutex_);
		hud_width_ = width;
		hud_height_ = height;
		hud_pixels_.assign(pixels, pixels + (size_t)width * height * 4);
	}
	hud_dirty_count_.store((int)std::max((size_t)3, hud_images_.size()));
}

bool XrVideoSession::UpdateHudLayer(XrCompositionLayerQuad &layer, float yaw, float pitch, const ScreenParams &params, const XrVector3f &recenter_pos)
{
	if(hud_swapchain_ == XR_NULL_HANDLE || hud_images_.empty())
		return false;

	if(hud_dirty_count_.load() > 0)
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(hud_swapchain_, &acquire, &index)))
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000; // 10ms timeout
			if(XR_SUCCEEDED(xrWaitSwapchainImage(hud_swapchain_, &wait)))
			{
				std::lock_guard<std::mutex> lock(hud_mutex_);
				if(!hud_pixels_.empty() && hud_width_ > 0 && hud_height_ > 0 && hud_width_ <= kHudWidth && hud_height_ <= kHudHeight)
				{
					glBindTexture(GL_TEXTURE_2D, hud_images_[index].image);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hud_width_, hud_height_,
							GL_RGBA, GL_UNSIGNED_BYTE, hud_pixels_.data());
					glBindTexture(GL_TEXTURE_2D, 0);
					hud_has_image_.store(true);
				}
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(hud_swapchain_, &release);
				hud_dirty_count_.fetch_sub(1);
			}
			else
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(hud_swapchain_, &release);
			}
		}
	}

	if(!hud_has_image_.load())
		return false;

	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = app_space_;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = hud_swapchain_;
	layer.subImage.imageRect.offset = XrOffset2Di{0, 0};
	layer.subImage.imageRect.extent = XrExtent2Di{kHudWidth, kHudHeight};
	layer.subImage.imageArrayIndex = 0;

	// While a video is opening, the HUD swapchain carries a centered loading
	// card instead of the control panel. Keep it straight ahead and close
	// enough to read even if the virtual screen is moved or not ready yet.
	if(loading_visible_.load())
	{
		const float forward_yaw = yaw;
		const float loading_dist = 1.4f;
		const float loading_w = 0.90f;
		const float loading_h = loading_w * (float)kHudHeight / (float)kHudWidth;
		layer.size = XrExtent2Df{loading_w, loading_h};
		layer.pose.orientation = QuatFromYaw(forward_yaw);
		layer.pose.position = XrVector3f{
			recenter_pos.x - std::sin(forward_yaw) * loading_dist,
			recenter_pos.y,
			recenter_pos.z - std::cos(forward_yaw) * loading_dist
		};
		return true;
	}


	const float hud_dist = kFixedUiDist;
	float hud_scale = hud_dist / 1.45f;
	layer.size = XrExtent2Df{kHudWidth1M * hud_scale, kHudWidth1M * hud_scale * (float)kHudHeight / (float)kHudWidth};
	float half_v_angle = std::atan(std::tan(params.centralAngle * 0.5f) / std::max(0.2f, params.aspectRatio));
	float height_angle = (params.radius > 0.1f) ? std::atan2(params.heightOffset, params.radius) : 0.0f;
	float down_angle = std::clamp(half_v_angle + 0.08f, 0.22f, 0.55f);
	float hud_pitch = pitch + height_angle - down_angle;
	float cos_p = std::cos(hud_pitch);
	float sin_p = std::sin(hud_pitch);
	layer.pose.orientation = QuatFromYawPitch(yaw, hud_pitch);
	layer.pose.position = XrVector3f{
		recenter_pos.x - std::sin(yaw) * cos_p * hud_dist,
		recenter_pos.y + sin_p * hud_dist,
		recenter_pos.z - std::cos(yaw) * cos_p * hud_dist
	};
	return true;
}

void XrVideoSession::SetOsdBitmap(const uint8_t *pixels, int32_t width, int32_t height)
{
	if(!pixels || width <= 0 || height <= 0)
		return;
	{
		std::lock_guard<std::mutex> lock(osd_mutex_);
		osd_width_ = width;
		osd_height_ = height;
		osd_pixels_.assign(pixels, pixels + (size_t)width * height * 4);
	}
	osd_dirty_count_.store((int)std::max((size_t)3, osd_images_.size()));
}

bool XrVideoSession::UpdateOsdLayer(XrCompositionLayerQuad &layer, float yaw, float pitch, const ScreenParams &params, const XrVector3f &recenter_pos)
{
	if(osd_swapchain_ == XR_NULL_HANDLE || osd_images_.empty())
		return false;

	if(osd_dirty_count_.load() > 0)
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(osd_swapchain_, &acquire, &index)))
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000; // 10ms timeout
			if(XR_SUCCEEDED(xrWaitSwapchainImage(osd_swapchain_, &wait)))
			{
				std::lock_guard<std::mutex> lock(osd_mutex_);
				if(!osd_pixels_.empty() && osd_width_ > 0 && osd_height_ > 0 && osd_width_ <= kOsdWidth && osd_height_ <= kOsdHeight)
				{
					glBindTexture(GL_TEXTURE_2D, osd_images_[index].image);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, osd_width_, osd_height_,
							GL_RGBA, GL_UNSIGNED_BYTE, osd_pixels_.data());
					glBindTexture(GL_TEXTURE_2D, 0);
					osd_has_image_.store(true);
				}
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(osd_swapchain_, &release);
				osd_dirty_count_.fetch_sub(1);
			}
			else
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(osd_swapchain_, &release);
			}
		}
	}

	if(!osd_has_image_.load())
		return false;

	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = app_space_;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = osd_swapchain_;
	layer.subImage.imageRect.offset = XrOffset2Di{0, 0};
	layer.subImage.imageRect.extent = XrExtent2Di{kOsdWidth, kOsdHeight};
	layer.subImage.imageArrayIndex = 0;

	// Toast sits just ABOVE the four circular buttons in the HUD card, i.e.
	// well clear of the seekbar. The HUD bitmap is 1024x640 and the circles
	// start at y=343, so the toast's bottom edge goes to y=326. Convert that
	// pixel offset into an angle at the fixed HUD distance; since it follows
	// hud_pitch, it tracks both screen size and source aspect ratio.
	const float hud_dist = kFixedUiDist;
	float hud_scale = hud_dist / 1.45f;
	float hud_w = kHudWidth1M * hud_scale;
	float hud_h = hud_w * (float)kHudHeight / (float)kHudWidth;
	float half_v_angle = std::atan(std::tan(params.centralAngle * 0.5f) / std::max(0.2f, params.aspectRatio));
	float height_angle = (params.radius > 0.1f)
			? std::atan2(params.heightOffset, params.radius) : 0.0f;
	float down_angle = std::clamp(half_v_angle + 0.08f, 0.22f, 0.55f);
	float hud_pitch = pitch + height_angle - down_angle;

	float osd_w = 0.65f * hud_scale;
	float osd_h = osd_w * (float)kOsdHeight / (float)kOsdWidth;
	const float kToastBottomPx = 326.0f;
	float px_to_m = hud_h / (float)kHudHeight;
	float bottom_m = ((float)kHudHeight * 0.5f - kToastBottomPx) * px_to_m;
	float osd_pitch = hud_pitch + std::atan2(bottom_m + osd_h * 0.5f, hud_dist);
	float cos_p = std::cos(osd_pitch);
	float sin_p = std::sin(osd_pitch);

	layer.size = XrExtent2Df{osd_w, osd_h};
	layer.pose.orientation = QuatFromYawPitch(yaw, osd_pitch);
	layer.pose.position = XrVector3f{
		recenter_pos.x - std::sin(yaw) * cos_p * hud_dist,
		recenter_pos.y + sin_p * hud_dist,
		recenter_pos.z - std::cos(yaw) * cos_p * hud_dist
	};
	return true;
}

void XrVideoSession::SetResBitmap(const uint8_t *pixels, int32_t width, int32_t height)
{
	if(!pixels || width <= 0 || height <= 0)
		return;
	{
		std::lock_guard<std::mutex> lock(res_mutex_);
		res_width_ = width;
		res_height_ = height;
		res_pixels_.assign(pixels, pixels + (size_t)width * height * 4);
	}
	res_dirty_count_.store((int)std::max((size_t)3, res_images_.size()));
}

bool XrVideoSession::UpdateResLayer(XrCompositionLayerQuad &layer, float yaw, float pitch, const ScreenParams &params, const XrVector3f &recenter_pos)
{
	if(res_swapchain_ == XR_NULL_HANDLE || res_images_.empty())
		return false;

	if(res_dirty_count_.load() > 0)
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(res_swapchain_, &acquire, &index)))
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000;
			if(XR_SUCCEEDED(xrWaitSwapchainImage(res_swapchain_, &wait)))
			{
				std::lock_guard<std::mutex> lock(res_mutex_);
				if(!res_pixels_.empty() && res_width_ > 0 && res_height_ > 0 && res_width_ <= kResWidth && res_height_ <= kResHeight)
				{
					glBindTexture(GL_TEXTURE_2D, res_images_[index].image);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, res_width_, res_height_,
							GL_RGBA, GL_UNSIGNED_BYTE, res_pixels_.data());
					glBindTexture(GL_TEXTURE_2D, 0);
					res_has_image_.store(true);
				}
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(res_swapchain_, &release);
				res_dirty_count_.fetch_sub(1);
			}
			else
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(res_swapchain_, &release);
			}
		}
	}

	if(!res_has_image_.load())
		return false;

	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = app_space_;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = res_swapchain_;
	layer.subImage.imageRect.offset = XrOffset2Di{0, 0};
	layer.subImage.imageRect.extent = XrExtent2Di{kResWidth, kResHeight};
	layer.subImage.imageArrayIndex = 0;

	// OSD Resolution Badge at bottom-left corner of video screen in world space
	float total_yaw = yaw;
	float total_pitch = pitch;
	float cos_sp = std::cos(total_pitch);
	float sin_sp = std::sin(total_pitch);
	XrQuaternionf scr_ori = QuatFromYawPitch(total_yaw, total_pitch);
	float dist = std::max(0.5f, params.radius);
	XrVector3f scr_center{
		recenter_pos.x - std::sin(total_yaw) * cos_sp * dist,
		recenter_pos.y + params.heightOffset + sin_sp * dist,
		recenter_pos.z - std::cos(total_yaw) * cos_sp * dist
	};
	float scr_w = 2.0f * dist * std::tan(params.centralAngle * 0.5f);
	float scr_h = scr_w / std::max(0.2f, params.aspectRatio);

	// Quad size for the badge
	float res_w = 0.30f * (dist / 2.5f);
	float res_h = res_w * (float)kResHeight / (float)kResWidth;
	layer.size = XrExtent2Df{res_w, res_h};

	// Local screen offset: -X is left, -Y is bottom, +Z is towards viewer (into the room)
	// Place badge inset from bottom-left corner
	float local_x = -0.5f * scr_w + res_w * 0.60f;
	float local_y = -0.5f * scr_h + res_h * 0.70f;
	float local_z = 0.03f; // 3cm in front of screen to prevent Z-fighting

	XrVector3f offset_world = RotateVector(scr_ori, XrVector3f{local_x, local_y, local_z});
	layer.pose.orientation = scr_ori;
	layer.pose.position = XrVector3f{
		scr_center.x + offset_world.x,
		scr_center.y + offset_world.y,
		scr_center.z + offset_world.z
	};
	return true;
}

// Frame-tagged 2-slot danmaku queue. The worker wakes at the START of XR frame N
// (WaitForXrFrame returns seq N) and draws the bitmap for frame N+1's display time.
// Under danmaku_mutex_, SetDanmakuBitmap places the frame into one of two slots
// tagged with wake seq N, and UpdateDanmakuLayer only consumes slots with tag < cur_seq.
struct DanmakuSlot
{
	std::vector<uint8_t> pixels;
	int32_t w = 0;
	int32_t h = 0;
	int32_t copy_h = 0;
	uint64_t tag_seq = 0;
	bool valid = false;
};
static DanmakuSlot g_dm_slots[2];
static std::atomic<uint64_t> g_dm_wake_seq{0};
static std::atomic<int32_t> g_dm_upload_h_hint{0};
namespace {
std::atomic<uint64_t> g_xr_frame_seq{0};
} // namespace
void SetDanmakuUploadHeightHint(int32_t height)
{
	g_dm_upload_h_hint.store(height, std::memory_order_release);
}

void XrVideoSession::SetDanmakuBitmap(const uint8_t *pixels, int32_t width, int32_t height)
{
	if(!pixels || width <= 0 || height <= 0)
		return;
	const int32_t hint_h = g_dm_upload_h_hint.load(std::memory_order_acquire);
	const int32_t copy_h = (hint_h > 0 && hint_h < height) ? hint_h : height;
	const size_t byteCount = (size_t)width * copy_h * 4;
	const uint64_t wake_seq = g_dm_wake_seq.load(std::memory_order_acquire);
	std::lock_guard<std::mutex> writeLock(danmaku_write_mutex_);
	danmaku_staging_pixels_.assign(pixels, pixels + byteCount);
	{
		std::lock_guard<std::mutex> lock(danmaku_mutex_);
		int target = -1;
		for(int i = 0; i < 2; i++)
		{
			if(!g_dm_slots[i].valid)
			{
				target = i;
				break;
			}
		}
		if(target < 0)
		{
			target = (g_dm_slots[0].tag_seq <= g_dm_slots[1].tag_seq) ? 0 : 1;
		}
		g_dm_slots[target].pixels.swap(danmaku_staging_pixels_);
		g_dm_slots[target].w = width;
		g_dm_slots[target].h = height;
		g_dm_slots[target].copy_h = copy_h;
		g_dm_slots[target].tag_seq = wake_seq;
		g_dm_slots[target].valid = true;
	}
}

static std::atomic<float> g_danmaku_depth_factor{0.99f};
static std::atomic<float> g_danmaku_span_x{1.0f};
static std::atomic<float> g_danmaku_span_y{1.0f};
static std::atomic<bool> g_danmaku_is_stereo{false};
static std::atomic<float> g_screen_pitch_offset{0.0f};
static std::atomic<float> g_screen_yaw_offset{0.0f};

namespace {
std::mutex g_xr_frame_mtx;
std::condition_variable g_xr_frame_cv;
std::atomic<int64_t> g_xr_target_display_ns{0};
std::atomic<bool> g_xr_frame_active{false};
} // namespace

uint64_t WaitForXrFrame(uint64_t lastSeq, int32_t timeoutMs)
{
	std::unique_lock<std::mutex> lock(g_xr_frame_mtx);
	const int32_t clampedMs = std::clamp(timeoutMs, 1, 100);
	g_xr_frame_cv.wait_for(lock, std::chrono::milliseconds(clampedMs), [&]() {
		return g_xr_frame_seq.load(std::memory_order_acquire) > lastSeq
				|| !g_xr_frame_active.load(std::memory_order_acquire);
	});
	const uint64_t seq = g_xr_frame_seq.load(std::memory_order_acquire);
	g_dm_wake_seq.store(seq, std::memory_order_release);
	return seq;
}

int64_t GetXrTargetDisplayNs()
{
	return g_xr_target_display_ns.load(std::memory_order_acquire);
}
// Java writes these directly so there is NO extra frame of latency. To keep
// all composition layers consistent, RenderFrame() takes one local snapshot
// at the top of the frame and every layer (video/HUD/OSD/settings/cursor) uses
// that snapshot instead of re-reading the atomics. Neither a two-stage pending
// buffer (which added a whole frame of lag) nor a low-pass follower is used:
// both made the screen trail the hand and showed up as a ghost edge.

// Grip drag uses the exact aim yaw/pitch that PiliPlus reads from
// TouchInput state[12]/[13]. Reconstructing yaw from the ray direction here
// produced the opposite left/right sign on Quest's aim pose convention.
static std::mutex g_screen_drag_mutex;
static bool g_grip_down = false;
static float g_anchor_ray_yaw = 0.0f;
static float g_anchor_ray_pitch = 0.0f;
static float g_anchor_screen_yaw = 0.0f;
static float g_anchor_screen_pitch = 0.0f;

static float WrappedAngleDelta(float now, float anchor)
{
	constexpr float kPi = 3.14159265f;
	constexpr float kTwoPi = 6.2831853f;
	float d = now - anchor;
	while(d > kPi) d -= kTwoPi;
	while(d < -kPi) d += kTwoPi;
	return d;
}

// Aim-pose tremor is typically well under a degree. A small angular dead band
// keeps a resting hand from translating the whole virtual screen; subtracting
// the band (instead of hard-gating) keeps the motion continuous when the drag
// actually starts. No low-pass: the screen must track the newest sample on the
// very next XR frame or it reads as a trailing ghost.
// 0.012 rad (~0.7 deg) still let a resting hand translate the screen; 0.020
// rad (~1.15 deg) covers normal grip tremor without making a deliberate drag
// feel sticky. Subtracting the band keeps motion continuous once it starts.
constexpr float kGripDeadzoneYaw = 0.020f;   // rad, ~1.15 deg
constexpr float kGripDeadzonePitch = 0.020f;

static float ApplyDeadzone(float delta, float band)
{
	const float mag = std::fabs(delta);
	if(mag <= band) return 0.0f;
	return delta > 0.0f ? (delta - band) : (delta + band);
}

void XrVideoSession::SetAimDragSample(float yaw, float pitch, bool grip)
{
	std::lock_guard<std::mutex> lock(g_screen_drag_mutex);
	if(grip && !g_grip_down)
	{
		g_grip_down = true;
		g_anchor_ray_yaw = yaw;
		g_anchor_ray_pitch = pitch;
		g_anchor_screen_yaw = g_screen_yaw_offset.load();
		g_anchor_screen_pitch = g_screen_pitch_offset.load();
	}
	else if(!grip && g_grip_down)
	{
		g_grip_down = false;
	}
	else if(grip && g_grip_down)
	{
		const float deltaYaw = WrappedAngleDelta(yaw, g_anchor_ray_yaw);
		const float deltaPitch = pitch - g_anchor_ray_pitch;
		g_screen_yaw_offset.store(g_anchor_screen_yaw
				+ ApplyDeadzone(deltaYaw, kGripDeadzoneYaw));
		g_screen_pitch_offset.store(std::clamp(g_anchor_screen_pitch
				+ ApplyDeadzone(deltaPitch, kGripDeadzonePitch), -0.65f, 1.35f));
	}
}

void XrVideoSession::SetScreenOffset(float pitch, float yaw)
{
	std::lock_guard<std::mutex> lock(g_screen_drag_mutex);
	g_screen_pitch_offset.store(pitch);
	g_screen_yaw_offset.store(yaw);
	g_grip_down = false;
}

void XrVideoSession::GetScreenOffset(float *outYaw, float *outPitch) const
{
	if(outYaw) *outYaw = g_screen_yaw_offset.load();
	if(outPitch) *outPitch = g_screen_pitch_offset.load();
}

void XrVideoSession::SetLoadingVisible(bool visible)
{
	loading_requested_.store(visible);
	loading_ready_.store(false);
	loading_dismissed_.store(false);
	loading_focused_frames_.store(0);
	loading_first_focused_ns_.store(0);
	loading_visible_.store(visible);
	if(visible)
		video_layer_enabled_.store(false);
	LOGI("VR launch splash %s", visible ? "armed" : "force hidden");
}

void XrVideoSession::SetLoadingReady(bool ready)
{
	loading_ready_.store(ready);
	// RenderFrame() normally dismisses the splash and enables the video layer
	// atomically. These fallbacks only cover unusual ordering (readiness after
	// a forced dismissal, or the splash never being armed), so a decoded frame
	// can never leave the immersive view stuck on black.
	if(ready && (loading_dismissed_.load() || !loading_requested_.load()))
		video_layer_enabled_.store(true);
}

bool XrVideoSession::UpdateDanmakuLayer(XrCompositionLayerCylinderKHR &cyl, XrCompositionLayerCylinderKHR &cyl_right, XrCompositionLayerQuad &quad, XrCompositionLayerQuad &quad_right, bool &out_is_quad, bool &out_is_stereo, float yaw, float pitch, const ScreenParams &params, const XrVector3f &recenter_pos)
{
	out_is_stereo = false;
	if(danmaku_swapchain_ == XR_NULL_HANDLE || danmaku_images_.empty())
		return false;

	const bool is_stereo = g_danmaku_is_stereo.load() && (danmaku_swapchain_right_ != XR_NULL_HANDLE);
	const float span_x = is_stereo ? ComputeTrackSpanX(params.aspectRatio) : std::max(0.5f, g_danmaku_span_x.load());

	int32_t active_w = kDanmakuWidth;
	int32_t active_h = kDanmakuHeight;
	{
		std::lock_guard<std::mutex> lock(danmaku_mutex_);
		if(danmaku_width_ > 0 && danmaku_height_ > 0)
		{
			active_w = std::min(danmaku_width_, kDanmakuWidth);
			active_h = std::min(danmaku_height_, kDanmakuHeight);
			if(is_stereo && active_h > 1)
				active_h = active_h / 2;
		}
	}

	auto clearSwapchainTexIfNeeded = [&](GLuint texId, std::vector<uint32_t> &gens, uint32_t idx, uint32_t targetGen) {
		if(idx >= gens.size() || gens[idx] == targetGen)
			return;
		if(danmaku_clear_fbo_ == 0)
			glGenFramebuffers(1, &danmaku_clear_fbo_);
		GLint prevFbo = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
		GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
		GLfloat prevClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
		glBindFramebuffer(GL_FRAMEBUFFER, danmaku_clear_fbo_);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
		if(prevScissor) glDisable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
		if(prevScissor) glEnable(GL_SCISSOR_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
		gens[idx] = targetGen;
	};

	int32_t up_w = 0;
	int32_t up_h = 0;
	int32_t up_copy_h = 0;
	{
		const uint64_t cur_seq = g_xr_frame_seq.load(std::memory_order_acquire);
		std::lock_guard<std::mutex> lock(danmaku_mutex_);
		int best = -1;
		for(int i = 0; i < 2; i++)
		{
			if(g_dm_slots[i].valid && g_dm_slots[i].tag_seq < cur_seq)
			{
				if(best < 0 || g_dm_slots[i].tag_seq > g_dm_slots[best].tag_seq)
					best = i;
			}
		}
		if(best >= 0)
		{
			if(!g_dm_slots[best].pixels.empty() && g_dm_slots[best].w > 0 && g_dm_slots[best].h > 0
					&& g_dm_slots[best].w <= kDanmakuWidth && g_dm_slots[best].h <= kDanmakuHeight)
			{
				danmaku_upload_pixels_.swap(g_dm_slots[best].pixels);
				up_w = g_dm_slots[best].w;
				up_h = g_dm_slots[best].h;
				up_copy_h = std::clamp(g_dm_slots[best].copy_h, 1, up_h);
				danmaku_width_ = up_w;
				danmaku_height_ = up_h;
			}
			g_dm_slots[best].valid = false;
			for(int i = 0; i < 2; i++)
			{
				if(g_dm_slots[i].valid && g_dm_slots[i].tag_seq < cur_seq)
					g_dm_slots[i].valid = false;
			}
		}
	}

	if(up_w > 0 && up_h > 0 && !danmaku_upload_pixels_.empty())
	{
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(!danmaku_swapchain_acquired_)
		{
			if(XR_SUCCEEDED(xrAcquireSwapchainImage(danmaku_swapchain_, &acquire, &danmaku_acquired_idx_)))
				danmaku_swapchain_acquired_ = true;
		}
		if(danmaku_swapchain_acquired_)
		{
			uint32_t index = danmaku_acquired_idx_;
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000;
			XrResult wait_res = xrWaitSwapchainImage(danmaku_swapchain_, &wait);
			if(wait_res == XR_SUCCESS)
			{
				active_w = up_w;
				active_h = up_h;
				int32_t check_h = (is_stereo && active_h > 1) ? (active_h / 2) : active_h;
				if(active_w != danmaku_last_up_w_ || check_h != danmaku_last_up_h_ || is_stereo != danmaku_last_up_stereo_)
				{
					danmaku_last_up_w_ = active_w;
					danmaku_last_up_h_ = check_h;
					danmaku_last_up_stereo_ = is_stereo;
					danmaku_clear_gen_++;
				}
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				if(is_stereo && active_h > 1)
				{
					int32_t eye_h = active_h / 2;
					clearSwapchainTexIfNeeded(danmaku_images_[index].image, danmaku_img_clear_gen_, index, danmaku_clear_gen_);
					glBindTexture(GL_TEXTURE_2D, danmaku_images_[index].image);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, active_w, eye_h,
							GL_RGBA, GL_UNSIGNED_BYTE, danmaku_upload_pixels_.data());
					glBindTexture(GL_TEXTURE_2D, 0);

					if(danmaku_swapchain_right_ != XR_NULL_HANDLE && !danmaku_images_right_.empty())
					{
						if(!danmaku_right_swapchain_acquired_)
						{
							if(XR_SUCCEEDED(xrAcquireSwapchainImage(danmaku_swapchain_right_, &acquire, &danmaku_right_acquired_idx_)))
								danmaku_right_swapchain_acquired_ = true;
						}
						if(danmaku_right_swapchain_acquired_)
						{
							uint32_t idx_r = danmaku_right_acquired_idx_;
							if(xrWaitSwapchainImage(danmaku_swapchain_right_, &wait) == XR_SUCCESS)
							{
								clearSwapchainTexIfNeeded(danmaku_images_right_[idx_r].image, danmaku_img_clear_gen_right_, idx_r, danmaku_clear_gen_);
								const uint8_t *right_ptr = danmaku_upload_pixels_.data() + ((size_t)active_w * eye_h * 4);
								glBindTexture(GL_TEXTURE_2D, danmaku_images_right_[idx_r].image);
								glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
								glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
								glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, active_w, eye_h,
										GL_RGBA, GL_UNSIGNED_BYTE, right_ptr);
								glBindTexture(GL_TEXTURE_2D, 0);
								danmaku_right_has_image_.store(true);
								XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
								xrReleaseSwapchainImage(danmaku_swapchain_right_, &rel);
								danmaku_right_swapchain_acquired_ = false;
							}
						}
					}
					active_h = eye_h;
				}
				else
				{
					danmaku_right_has_image_.store(false);
					clearSwapchainTexIfNeeded(danmaku_images_[index].image, danmaku_img_clear_gen_, index, danmaku_clear_gen_);
					glBindTexture(GL_TEXTURE_2D, danmaku_images_[index].image);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					const int32_t partial_h = std::min(up_copy_h, active_h);
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, active_w, partial_h,
							GL_RGBA, GL_UNSIGNED_BYTE, danmaku_upload_pixels_.data());
					glBindTexture(GL_TEXTURE_2D, 0);
				}
				danmaku_has_image_.store(true);
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(danmaku_swapchain_, &release);
				danmaku_swapchain_acquired_ = false;
			}
			else if(wait_res != XR_TIMEOUT_EXPIRED)
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(danmaku_swapchain_, &release);
				danmaku_swapchain_acquired_ = false;
			}
		}
	}

	if(!danmaku_has_image_.load() || (is_stereo && !danmaku_right_has_image_.load()))
		return false;

	const float span_y = std::max(0.5f, g_danmaku_span_y.load());
	const float dm_aspect = (active_h > 0) ? ((float)active_w / (float)active_h) : (std::max(0.2f, params.aspectRatio) * (span_x / span_y));
	const bool force_cylinder = (span_x > 1.2f && !is_stereo);

	const float depth_factor = std::clamp(g_danmaku_depth_factor.load(), 0.45f, 0.995f);
	const float distance = std::max(0.15f, params.radius * depth_factor);
	const float cos_pitch = std::cos(pitch);
	const float sin_pitch = std::sin(pitch);

	if(layer_shape_.load() == LayerShape::Cylinder || force_cylinder)
	{
		out_is_quad = false;
		cyl.type = XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
		cyl.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		cyl.space = app_space_;
		cyl.eyeVisibility = is_stereo ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
		cyl.subImage.swapchain = danmaku_swapchain_;
		cyl.subImage.imageRect.offset = XrOffset2Di{0, 0};
		cyl.subImage.imageRect.extent = XrExtent2Di{active_w, active_h};
		cyl.subImage.imageArrayIndex = 0;

		float curvature = params.curvature > 0.001f ? params.curvature : 0.1f;
		if(!is_stereo)
		{
			if(span_x > 1.8f)
				curvature = std::max(curvature, 0.90f);
			else if(span_x > 1.2f)
				curvature = std::max(curvature, 0.65f);
		}

		const float arc_radius = distance / curvature;
		const float axis_back = arc_radius - distance;

		cyl.pose.orientation = QuatFromYawPitch(yaw, pitch);
		cyl.pose.position = XrVector3f{
			recenter_pos.x + std::sin(yaw) * cos_pitch * axis_back,
			recenter_pos.y + params.heightOffset - sin_pitch * axis_back,
			recenter_pos.z + std::cos(yaw) * cos_pitch * axis_back
		};
		cyl.radius = arc_radius;
		float base_half_tan = std::tan(std::min(params.centralAngle, 3.05f) * 0.5f);
		cyl.centralAngle = std::min(5.5f, 2.0f * curvature * base_half_tan * span_x);
		cyl.aspectRatio = dm_aspect;

		if(is_stereo)
		{
			cyl_right = cyl;
			cyl_right.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
			cyl_right.subImage.swapchain = danmaku_swapchain_right_;
			out_is_stereo = true;
		}
		return true;
	}
	else
	{
		out_is_quad = true;
		quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.space = app_space_;
		quad.eyeVisibility = is_stereo ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = danmaku_swapchain_;
		quad.subImage.imageRect.offset = XrOffset2Di{0, 0};
		quad.subImage.imageRect.extent = XrExtent2Di{active_w, active_h};
		quad.subImage.imageArrayIndex = 0;

		float width = 2.0f * distance * std::tan(params.centralAngle * 0.5f) * span_x;
		quad.size = XrExtent2Df{width, width / dm_aspect};
		quad.pose.orientation = QuatFromYawPitch(yaw, pitch);
		quad.pose.position = XrVector3f{
			recenter_pos.x - std::sin(yaw) * cos_pitch * distance,
			recenter_pos.y + params.heightOffset + sin_pitch * distance,
			recenter_pos.z - std::cos(yaw) * cos_pitch * distance
		};

		if(is_stereo)
		{
			quad_right = quad;
			quad_right.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
			quad_right.subImage.swapchain = danmaku_swapchain_right_;
			out_is_stereo = true;
		}
		return true;
	}
}

bool XrVideoSession::UpdateCursorLayer(XrCompositionLayerQuad &layer, int slot)
{
	if(slot < 0 || slot > 1 || cursor_swapchain_ == XR_NULL_HANDLE || cursor_images_.empty())
		return false;

	if(!cursor_has_image_.load())
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(cursor_swapchain_, &acquire, &index)))
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000;
			if(XR_SUCCEEDED(xrWaitSwapchainImage(cursor_swapchain_, &wait)))
			{
				std::vector<uint8_t> pixels(kCursorSize * kCursorSize * 4, 0);
				const float center = (kCursorSize - 1) * 0.5f;
				for(int y = 0; y < kCursorSize; y++)
				{
					for(int x = 0; x < kCursorSize; x++)
					{
						float dx = (float)x - center;
						float dy = (float)y - center;
						float r = std::sqrt(dx * dx + dy * dy);
						uint8_t R = 0, G = 0, B = 0, A = 0;
						if(r <= 11.5f)
						{
							R = 255; G = 255; B = 255;
							A = 255;
						}
						else if(r <= 13.5f)
						{
							float t = (r - 11.5f) / 2.0f;
							uint8_t v = (uint8_t)(255.0f * (1.0f - t) + 140.0f * t);
							R = v; G = v; B = v;
							A = 250;
						}
						else if(r <= 17.5f)
						{
							R = 140; G = 140; B = 144;
							A = 235;
						}
						else if(r <= 20.5f)
						{
							float t = std::clamp((20.5f - r) / 3.0f, 0.0f, 1.0f);
							A = (uint8_t)(235.0f * t);
							R = (uint8_t)(140.0f * t);
							G = (uint8_t)(140.0f * t);
							B = (uint8_t)(144.0f * t);
						}
						size_t idx = (size_t)(y * kCursorSize + x) * 4;
						pixels[idx + 0] = R;
						pixels[idx + 1] = G;
						pixels[idx + 2] = B;
						pixels[idx + 3] = A;
					}
				}
				glBindTexture(GL_TEXTURE_2D, cursor_images_[index].image);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCursorSize, kCursorSize,
						GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
				glBindTexture(GL_TEXTURE_2D, 0);
				cursor_has_image_.store(true);
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(cursor_swapchain_, &release);
			}
			else
			{
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(cursor_swapchain_, &release);
			}
		}
	}

	if(!cursor_has_image_.load() || !cursor_visible_[slot].load())
		return false;

	if(hand_cursor_active_.load() && hand_cursor_hidden_.load())
		return false;

	if(!hud_visible_.load() && !settings_visible_.load())
		return false;

	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = app_space_;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = cursor_swapchain_;
	layer.subImage.imageRect.offset = XrOffset2Di{0, 0};
	layer.subImage.imageRect.extent = XrExtent2Di{kCursorSize, kCursorSize};
	layer.subImage.imageArrayIndex = 0;

	std::lock_guard<std::mutex> lock(cursor_mutex_);
	layer.size = XrExtent2Df{cursor_size_[slot], cursor_size_[slot]};
	layer.pose.position = cursor_pos_[slot];
	layer.pose.orientation = cursor_ori_[slot];
	return true;
}

int XrVideoSession::UpdateControllerRay(float ox, float oy, float oz, float dx, float dy, float dz, int buttons, float *outU, float *outV)
{
	return UpdatePointerRay(0, ox, oy, oz, dx, dy, dz, buttons, outU, outV, true);
}

int XrVideoSession::UpdatePointerRay(int slot, float ox, float oy, float oz, float dx, float dy, float dz, int buttons, float *outU, float *outV, bool is_controller)
{
	if(slot < 0 || slot > 1) slot = 0;
	if(outU) *outU = 0.0f;
	if(outV) *outV = 0.0f;
	float dirLenSq = dx * dx + dy * dy + dz * dz;
	if(dirLenSq < 0.1f)
	{
		if(!hand_cursor_active_.load())
			cursor_visible_[slot].store(false);
		return 0;
	}

	if(is_controller && slot == 0)
	{
		float moveDelta = std::fabs(dx - last_ray_dx_) + std::fabs(dy - last_ray_dy_) + std::fabs(dz - last_ray_dz_);
		last_ray_dx_ = dx;
		last_ray_dy_ = dy;
		last_ray_dz_ = dz;
		const float wakeThresh = hand_cursor_active_.load() ? 0.018f : 0.003f;
		if(moveDelta > wakeThresh || buttons != 0)
		{
			auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
			last_controller_active_ms_.store(now_ms);
		}
	}

	ScreenParams params;
	XrQuaternionf recenter;
	XrVector3f recenter_pos;
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		params = params_;
		recenter = recenter_orientation_;
		recenter_pos = recenter_position_;
	}

	const float total_yaw = YawFromQuat(recenter) + params.yawOffset + g_screen_yaw_offset.load();
	const float total_pitch = PitchFromQuat(recenter) + g_screen_pitch_offset.load();
	const bool hud_vis = hud_visible_.load();
	const bool set_vis = settings_visible_.load();

	// 0. Check intersection with upper Settings Quad when Settings is open
	if(set_vis)
	{
		float half_v_angle = std::atan(std::tan(params.centralAngle * 0.5f) / std::max(0.2f, params.aspectRatio));
		float height_angle = (params.radius > 0.1f) ? std::atan2(params.heightOffset, params.radius) : 0.0f;
		float up_angle = half_v_angle + kSettingsUpExtra;
		float settings_pitch = total_pitch + height_angle + up_angle;
		const float settings_dist = kFixedUiDist;
		float cos_set_p = std::cos(settings_pitch);
		float sin_set_p = std::sin(settings_pitch);
		XrQuaternionf settings_ori = QuatFromYawPitch(total_yaw, settings_pitch);
		XrVector3f settings_pos{
			recenter_pos.x - std::sin(total_yaw) * cos_set_p * settings_dist,
			recenter_pos.y + sin_set_p * settings_dist,
			recenter_pos.z - std::cos(total_yaw) * cos_set_p * settings_dist
		};
		const float set_w = kFixedSettingsW;
		float set_h = set_w * (float)kSettingsHeight / (float)kSettingsWidth;

		XrVector3f p_set = RotateByInverse(settings_ori, XrVector3f{ox - settings_pos.x, oy - settings_pos.y, oz - settings_pos.z});
		XrVector3f d_set = RotateByInverse(settings_ori, XrVector3f{dx, dy, dz});
		if(d_set.z < -0.02f)
		{
			float t = -p_set.z / d_set.z;
			if(t > 0.05f)
			{
				float hx = p_set.x + t * d_set.x;
				float hy = p_set.y + t * d_set.y;
				float u = (hx / set_w) + 0.5f;
				float v = 0.5f - (hy / set_h);
				if(u >= -0.03f && u <= 1.03f && v >= -0.03f && v <= 1.03f)
				{
					float cdist = std::max(0.2f, t - 0.008f);
					{
						std::lock_guard<std::mutex> lock(cursor_mutex_);
						cursor_pos_[slot] = XrVector3f{ox + cdist * dx, oy + cdist * dy, oz + cdist * dz};
						cursor_ori_[slot] = settings_ori;
						cursor_size_[slot] = 0.024f * (cdist / 1.5f);
					}
					cursor_visible_[slot].store(true);
					if(is_controller && slot == 0 && buttons != 0)
						last_controller_active_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now().time_since_epoch()).count());
					if(outU) *outU = std::clamp(u, 0.0f, 1.0f);
					if(outV) *outV = std::clamp(v, 0.0f, 1.0f);
					return 3; // Hit Settings panel
				}
			}
		}
	}

	// 1. Check intersection with HUD Quad when HUD is visible
	if(hud_vis)
	{
		const float hud_dist = kFixedUiDist;
		float hud_scale = hud_dist / 1.45f;
		float half_v_angle = std::atan(std::tan(params.centralAngle * 0.5f) / std::max(0.2f, params.aspectRatio));
		float height_angle = (params.radius > 0.1f) ? std::atan2(params.heightOffset, params.radius) : 0.0f;
		float down_angle = std::clamp(half_v_angle + 0.08f, 0.22f, 0.55f);
		float hud_pitch = total_pitch + height_angle - down_angle;
		float cos_hp = std::cos(hud_pitch);
		float sin_hp = std::sin(hud_pitch);
		XrQuaternionf hud_ori = QuatFromYawPitch(total_yaw, hud_pitch);
		XrVector3f hud_pos{
			recenter_pos.x - std::sin(total_yaw) * cos_hp * hud_dist,
			recenter_pos.y + sin_hp * hud_dist,
			recenter_pos.z - std::cos(total_yaw) * cos_hp * hud_dist
		};
		float hud_w = kHudWidth1M * hud_scale;
		float hud_h = hud_w * (float)kHudHeight / (float)kHudWidth;

		XrVector3f p_hud = RotateByInverse(hud_ori, XrVector3f{ox - hud_pos.x, oy - hud_pos.y, oz - hud_pos.z});
		XrVector3f d_hud = RotateByInverse(hud_ori, XrVector3f{dx, dy, dz});
		if(d_hud.z < -0.02f)
		{
			float t = -p_hud.z / d_hud.z;
			if(t > 0.05f)
			{
				float hx = p_hud.x + t * d_hud.x;
				float hy = p_hud.y + t * d_hud.y;
				float u = (hx / hud_w) + 0.5f;
				float v = 0.5f - (hy / hud_h);
				if(u >= -0.02f && u <= 1.02f && v >= -0.02f && v <= 1.02f)
				{
					bool hit_opaque = false;
					{
						std::lock_guard<std::mutex> hlock(hud_mutex_);
						if(!hud_pixels_.empty() && hud_width_ > 0 && hud_height_ > 0)
						{
							int px = std::clamp((int)(u * hud_width_), 0, hud_width_ - 1);
							int py = std::clamp((int)(v * hud_height_), 0, hud_height_ - 1);
							for(int dy_px = -18; dy_px <= 18 && !hit_opaque; dy_px += 9)
							{
								int sy = std::clamp(py + dy_px, 0, hud_height_ - 1);
								for(int dx_px = -18; dx_px <= 18; dx_px += 9)
								{
									int sx = std::clamp(px + dx_px, 0, hud_width_ - 1);
									uint8_t alpha = hud_pixels_[((size_t)sy * hud_width_ + sx) * 4 + 3];
									if(alpha > 15)
									{
										hit_opaque = true;
										break;
									}
								}
							}
						}
					}
					if(hit_opaque)
					{
						float cdist = std::max(0.2f, t - 0.008f);
						{
							std::lock_guard<std::mutex> lock(cursor_mutex_);
							cursor_pos_[slot] = XrVector3f{ox + cdist * dx, oy + cdist * dy, oz + cdist * dz};
							cursor_ori_[slot] = hud_ori;
							cursor_size_[slot] = 0.024f * (cdist / 1.5f);
						}
						cursor_visible_[slot].store(true);
						if(is_controller && slot == 0 && buttons != 0)
							last_controller_active_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
									std::chrono::steady_clock::now().time_since_epoch()).count());
						if(outU) *outU = std::clamp(u, 0.0f, 1.0f);
						if(outV) *outV = std::clamp(v, 0.0f, 1.0f);
						return 2; // Hit HUD
					}
				}
			}
		}
	}

	// 2. Check intersection with Screen surface (Cylinder in Cinema curved mode, Quad otherwise)
	float cos_sp = std::cos(total_pitch);
	float sin_sp = std::sin(total_pitch);
	XrQuaternionf scr_ori = QuatFromYawPitch(total_yaw, total_pitch);
	float dist = std::max(0.5f, params.radius);

	if(projection_mode_.load() == 0 && layer_shape_.load() == LayerShape::Cylinder)
	{
		const float curvature = params.curvature > 0.001f ? params.curvature : 0.1f;
		const float arc_radius = dist / curvature;
		const float axis_back = arc_radius - dist;
		const float cyl_angle = std::min(5.5f, 2.0f * curvature * std::tan(std::min(params.centralAngle, 3.05f) * 0.5f));
		const float arc_len = arc_radius * cyl_angle;
		const float scr_h = arc_len / std::max(0.2f, params.aspectRatio);
		const XrVector3f cyl_pos{
			recenter_pos.x + std::sin(total_yaw) * cos_sp * axis_back,
			recenter_pos.y + params.heightOffset - sin_sp * axis_back,
			recenter_pos.z + std::cos(total_yaw) * cos_sp * axis_back
		};
		XrVector3f p_cyl = RotateByInverse(scr_ori, XrVector3f{ox - cyl_pos.x, oy - cyl_pos.y, oz - cyl_pos.z});
		XrVector3f d_cyl = RotateByInverse(scr_ori, XrVector3f{dx, dy, dz});
		float A = d_cyl.x * d_cyl.x + d_cyl.z * d_cyl.z;
		if(A > 1e-4f)
		{
			float B = p_cyl.x * d_cyl.x + p_cyl.z * d_cyl.z;
			float C = p_cyl.x * p_cyl.x + p_cyl.z * p_cyl.z - arc_radius * arc_radius;
			float disc = B * B - A * C;
			if(disc >= 0.0f)
			{
				float sqrt_disc = std::sqrt(disc);
				float t = (C <= 0.0f) ? ((-B + sqrt_disc) / A) : ((-B - sqrt_disc) / A);
				if(t > 0.05f)
				{
					float hx = p_cyl.x + t * d_cyl.x;
					float hy = p_cyl.y + t * d_cyl.y;
					float hz = p_cyl.z + t * d_cyl.z;
					float theta = std::atan2(hx, -hz);
					float u = (theta / std::max(0.01f, cyl_angle)) + 0.5f;
					float v = 0.5f - (hy / std::max(0.05f, scr_h));
					if(u >= -0.25f && u <= 1.25f && v >= -0.20f && v <= 1.85f)
					{
						float cdist = std::max(0.35f, t - 0.015f);
						XrQuaternionf tangent_ori = QuatMul(scr_ori, QuatFromYawPitch(-theta, 0.0f));
						{
							std::lock_guard<std::mutex> lock(cursor_mutex_);
							cursor_pos_[slot] = XrVector3f{ox + cdist * dx, oy + cdist * dy, oz + cdist * dz};
							cursor_ori_[slot] = tangent_ori;
							cursor_size_[slot] = 0.024f * (cdist / 1.5f);
						}
						cursor_visible_[slot].store(!(hand_cursor_active_.load() && hand_cursor_hidden_.load()));
						if(hud_visible_.load() && projection_mode_.load() == 0 && u >= 0.84f && u <= 1.12f && v >= 0.80f && v <= 1.12f)
						{
							corner_hovered_.store(true);
							if(outU) *outU = std::clamp(u, 0.0f, 1.0f);
							if(outV) *outV = std::clamp(v, 0.0f, 1.0f);
							return 4; // Hit Bottom-Right Resize Corner
						}
						if(slot == 0)
							corner_hovered_.store(false);
						if(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
						{
							if(outU) *outU = u;
							if(outV) *outV = v;
							return 1; // Hit Screen
						}
						return 0;
					}
				}
			}
		}
	}
	else
	{
		XrVector3f scr_pos{
			recenter_pos.x - std::sin(total_yaw) * cos_sp * dist,
			recenter_pos.y + params.heightOffset + sin_sp * dist,
			recenter_pos.z - std::cos(total_yaw) * cos_sp * dist
		};
		float scr_w = 2.0f * dist * std::tan(params.centralAngle * 0.5f);
		float scr_h = scr_w / std::max(0.2f, params.aspectRatio);

		XrVector3f p_scr = RotateByInverse(scr_ori, XrVector3f{ox - scr_pos.x, oy - scr_pos.y, oz - scr_pos.z});
		XrVector3f d_scr = RotateByInverse(scr_ori, XrVector3f{dx, dy, dz});
		if(d_scr.z < -0.05f)
		{
			float t = -p_scr.z / d_scr.z;
			if(t > 0.05f)
			{
				float hx = p_scr.x + t * d_scr.x;
				float hy = p_scr.y + t * d_scr.y;
				float u = (hx / scr_w) + 0.5f;
				float v = 0.5f - (hy / scr_h);
				if(u >= -0.25f && u <= 1.25f && v >= -0.20f && v <= 1.85f)
				{
					// Place cursor directly on the screen surface so there is zero stereo parallax
					float cdist = std::max(0.35f, t - 0.015f);
					{
						std::lock_guard<std::mutex> lock(cursor_mutex_);
						cursor_pos_[slot] = XrVector3f{ox + cdist * dx, oy + cdist * dy, oz + cdist * dz};
						cursor_ori_[slot] = scr_ori;
						cursor_size_[slot] = 0.024f * (cdist / 1.5f);
					}
					cursor_visible_[slot].store(!(hand_cursor_active_.load() && hand_cursor_hidden_.load()));
					if(hud_visible_.load() && projection_mode_.load() == 0 && u >= 0.84f && u <= 1.12f && v >= 0.80f && v <= 1.12f)
					{
						corner_hovered_.store(true);
						if(outU) *outU = std::clamp(u, 0.0f, 1.0f);
						if(outV) *outV = std::clamp(v, 0.0f, 1.0f);
						return 4; // Hit Bottom-Right Resize Corner
					}
					if(slot == 0)
						corner_hovered_.store(false);
					if(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)
					{
						if(outU) *outU = u;
						if(outV) *outV = v;
						return 1; // Hit Screen
					}
					return 0;
				}
			}
		}
	}

	if(slot == 0)
		corner_hovered_.store(false);
	cursor_visible_[slot].store(false);
	return 0;
}

void XrVideoSession::SetHandCursorHidden(bool hidden)
{
	hand_cursor_hidden_.store(hidden);
	if(hidden && hand_cursor_active_.load())
	{
		cursor_visible_[0].store(false);
		cursor_visible_[1].store(false);
	}
}

void XrVideoSession::SyncBaseScreenState(float baseFov, float screenDist)
{
	if(hand_is_manipulating_.load())
		return;
	if(baseFov >= 0.04f && baseFov <= 3.10f)
		hand_base_fov_.store(baseFov);
	if(screenDist >= 0.15f && screenDist <= 30.0f)
		hand_screen_dist_.store(screenDist);
}

void XrVideoSession::SetHandTrackingEnabled(bool enabled)
{
	hand_tracking_enabled_.store(enabled);
	if(!enabled)
	{
		if(hand_cursor_active_.exchange(false))
		{
			cursor_visible_[0].store(false);
			cursor_visible_[1].store(false);
		}
		hand_cursor_hidden_.store(false);
		corner_hovered_.store(false);
		corner_resizing_.store(false);
		if(hand_is_manipulating_.exchange(false))
		{
			std::lock_guard<std::mutex> lock(hand_mutex_);
			pending_save_prefs_ = true;
		}
		hand_gesture_mode_ = HandGestureMode::None;
		hand_active_idx_ = -1;
		for(int h = 0; h < 2; h++)
		{
			smooth_ray_valid_[h] = false;
			smooth_depth_valid_[h] = false;
			hand_pinch_down_[h] = false;
			hand_pinch_drop_since_ms_[h] = 0;
			hand_open_confirm_frames_[h] = 0;
			hand_last_track_ok_ms_[h] = 0;
			hand_prev_ray_src_[h] = 0;
		}
		std::lock_guard<std::mutex> lock(hand_mutex_);
		for(int h = 0; h < 2; h++)
		{
			out_hand_active_[h] = false;
			out_hand_pinch_[h] = false;
			out_hand_hit_type_[h] = 0;
		}
		out_two_hand_span_ = 0.0f;
	}
}

void XrVideoSession::PollHandTracking(float outState[26])
{
	if(!outState) return;
	for(int i = 0; i < 26; i++) outState[i] = 0.0f;
	std::lock_guard<std::mutex> lock(hand_mutex_);
	for(int h = 0; h < 2; h++)
	{
		int base = h * 8;
		outState[base + 0] = out_hand_active_[h] ? 1.0f : 0.0f;
		outState[base + 1] = out_hand_pinch_[h] ? 1.0f : 0.0f;
		outState[base + 2] = (float)out_hand_hit_type_[h];
		outState[base + 3] = out_hand_hit_u_[h];
		outState[base + 4] = out_hand_hit_v_[h];
		outState[base + 5] = out_hand_ray_yaw_[h];
		outState[base + 6] = out_hand_ray_pitch_[h];
		outState[base + 7] = out_hand_depth_[h];
	}
	outState[16] = (float)pending_hand_toggle_event_;
	pending_hand_toggle_event_ = 0;
	outState[17] = hand_tracking_enabled_.load() ? 0.0f : 1.0f; // 1.0 = paused
	outState[18] = out_two_hand_span_;
	outState[19] = hand_is_manipulating_.load() ? 1.0f : 0.0f;
	outState[20] = hand_base_fov_.load();
	outState[21] = hand_screen_dist_.load();
	outState[22] = g_screen_yaw_offset.load();
	outState[23] = g_screen_pitch_offset.load();
	outState[24] = (float)pending_tap_hit_type_;
	pending_tap_hit_type_ = 0;
	outState[25] = pending_save_prefs_ ? 1.0f : 0.0f;
	pending_save_prefs_ = false;
	outState[26] = (float)pending_microgesture_event_;
	pending_microgesture_event_ = 0;
}

void XrVideoSession::UpdateHandTracking(XrTime display_time)
{
	if(!hand_tracking_supported_ || !pfnLocateHandJointsEXT_ || app_space_ == XR_NULL_HANDLE)
		return;

	auto rotVec = [](const XrQuaternionf &q, const XrVector3f &v) -> XrVector3f {
		float tx = 2.0f * (q.y * v.z - q.z * v.y);
		float ty = 2.0f * (q.z * v.x - q.x * v.z);
		float tz = 2.0f * (q.x * v.y - q.y * v.x);
		return XrVector3f{
			v.x + q.w * tx + (q.y * tz - q.z * ty),
			v.y + q.w * ty + (q.z * tx - q.x * tz),
			v.z + q.w * tz + (q.x * ty - q.y * tx)
		};
	};

	XrVector3f head_pos{0.0f, 0.0f, 0.0f};
	if(views_.size() >= 2)
	{
		head_pos.x = 0.5f * (views_[0].pose.position.x + views_[1].pose.position.x);
		head_pos.y = 0.5f * (views_[0].pose.position.y + views_[1].pose.position.y);
		head_pos.z = 0.5f * (views_[0].pose.position.z + views_[1].pose.position.z);
	}
	else if(!views_.empty())
	{
		head_pos = views_[0].pose.position;
	}

	bool tracked[2] = {false, false};
	bool pinch[2] = {false, false};
	bool aim_valid[2] = {false, false};
	bool hand_grace_hold[2] = {false, false};
	int ray_src[2] = {0, 0};
	XrVector3f ray_orig[2]{};
	XrVector3f ray_dir[2]{};
	bool left_palm_facing_head = false;
	int local_micro_event = 0;
	auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();

	for(int h = 0; h < 2; h++)
	{
		if(hand_trackers_[h] == XR_NULL_HANDLE)
			continue;

		const bool actively_manip_h = (hand_gesture_mode_ == HandGestureMode::TwoHandResize)
				|| ((hand_gesture_mode_ == HandGestureMode::SingleDrag || hand_gesture_mode_ == HandGestureMode::CornerResize)
						&& hand_active_idx_ == h && hand_drag_moved_);

		XrHandJointsLocateInfoEXT locate_info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
		locate_info.baseSpace = app_space_;
		locate_info.time = display_time;

		XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT]{};
		XrHandTrackingAimStateFB aim_state{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
		XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
		locations.next = hand_aim_supported_ ? &aim_state : nullptr;
		locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
		locations.jointLocations = joints;

		if(XR_FAILED(pfnLocateHandJointsEXT_(hand_trackers_[h], &locate_info, &locations)) || !locations.isActive)
		{
			if(hand_pinch_down_[h] && actively_manip_h && (now_ms - hand_last_track_ok_ms_[h]) < 110)
			{
				pinch[h] = true;
				hand_grace_hold[h] = true;
			}
			else
			{
				hand_pinch_down_[h] = false;
				hand_pinch_drop_since_ms_[h] = 0;
				hand_open_confirm_frames_[h] = 0;
				hand_prev_ray_src_[h] = 0;
			}
			micro_tap_armed_[h] = false;
			continue;
		}
		tracked[h] = true;

		float tip_gap = -1.0f;
		{
			const auto &thumb = joints[XR_HAND_JOINT_THUMB_TIP_EXT];
			const auto &index = joints[XR_HAND_JOINT_INDEX_TIP_EXT];
			if((thumb.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
			   (index.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
			{
				float dx = thumb.pose.position.x - index.pose.position.x;
				float dy = thumb.pose.position.y - index.pose.position.y;
				float dz = thumb.pose.position.z - index.pose.position.z;
				tip_gap = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
		}

		// Official Meta OpenXR pinch hysteresis: trigger on binary aim bit / >= 0.85 strength,
		// and hold via continuous pinchStrengthIndex + fingertip distance with drag-release debounce.
		const bool has_aim_computed = hand_aim_supported_ && ((aim_state.status & XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB) != 0);
		bool pinch_raw = false;
		bool clear_open = false;
		if(has_aim_computed)
		{
			const bool pinch_bit = (aim_state.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) != 0;
			const float strength = aim_state.pinchStrengthIndex;
			if(!hand_pinch_down_[h])
			{
				pinch_raw = pinch_bit || (strength >= 0.85f);
			}
			else if(actively_manip_h)
			{
				pinch_raw = pinch_bit || (strength >= 0.38f) || (tip_gap >= 0.0f && tip_gap < 0.028f);
				clear_open = !pinch_bit && (strength < 0.28f) && (tip_gap < 0.0f || tip_gap > 0.035f);
			}
			else
			{
				pinch_raw = pinch_bit || (strength >= 0.42f) || (tip_gap >= 0.0f && tip_gap < 0.028f);
				clear_open = !pinch_bit && (strength < 0.30f);
			}
		}
		else if(tip_gap >= 0.0f)
		{
			if(!hand_pinch_down_[h])
			{
				pinch_raw = (tip_gap < 0.022f);
			}
			else if(actively_manip_h)
			{
				pinch_raw = (tip_gap < 0.028f);
				clear_open = (tip_gap > 0.035f);
			}
			else
			{
				pinch_raw = (tip_gap < 0.028f);
				clear_open = (tip_gap > 0.034f);
			}
		}
		else
		{
			clear_open = !actively_manip_h;
		}

		if(!hand_pinch_down_[h])
		{
			if(pinch_raw)
			{
				hand_pinch_down_[h] = true;
				hand_pinch_drop_since_ms_[h] = 0;
				hand_open_confirm_frames_[h] = 0;
			}
		}
		else if(pinch_raw)
		{
			hand_pinch_drop_since_ms_[h] = 0;
			hand_open_confirm_frames_[h] = 0;
		}
		else if(!actively_manip_h || clear_open)
		{
			hand_pinch_down_[h] = false;
			hand_pinch_drop_since_ms_[h] = 0;
			hand_open_confirm_frames_[h] = 0;
		}
		else
		{
			if(hand_pinch_drop_since_ms_[h] == 0)
				hand_pinch_drop_since_ms_[h] = now_ms;
			if((now_ms - hand_pinch_drop_since_ms_[h]) >= 50)
			{
				hand_pinch_down_[h] = false;
				hand_pinch_drop_since_ms_[h] = 0;
			}
		}
		pinch[h] = hand_pinch_down_[h];

		// Fist microgestures (thumb tap + left/right thumb swipe) sampled from 26-joint
		// skeletal poses when not pinching or manipulating the screen.
		if(hand_tracking_enabled_.load() && !hand_pinch_down_[h]
				&& hand_gesture_mode_ == HandGestureMode::None)
		{
			auto jointDist = [&](XrHandJointEXT a, XrHandJointEXT b, float &outDist) -> bool {
				const auto &ja = joints[a];
				const auto &jb = joints[b];
				if(!(ja.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
				   !(jb.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
					return false;
				float dx = ja.pose.position.x - jb.pose.position.x;
				float dy = ja.pose.position.y - jb.pose.position.y;
				float dz = ja.pose.position.z - jb.pose.position.z;
				outDist = std::sqrt(dx * dx + dy * dy + dz * dz);
				return true;
			};
			float d_mid = 1.0f, d_ring = 1.0f, d_lit = 1.0f, d_idx = 1.0f, d_tap = 1.0f;
			bool have_joints = jointDist(XR_HAND_JOINT_MIDDLE_TIP_EXT, XR_HAND_JOINT_PALM_EXT, d_mid)
					&& jointDist(XR_HAND_JOINT_RING_TIP_EXT, XR_HAND_JOINT_PALM_EXT, d_ring)
					&& jointDist(XR_HAND_JOINT_LITTLE_TIP_EXT, XR_HAND_JOINT_PALM_EXT, d_lit)
					&& jointDist(XR_HAND_JOINT_INDEX_TIP_EXT, XR_HAND_JOINT_PALM_EXT, d_idx)
					&& jointDist(XR_HAND_JOINT_THUMB_TIP_EXT, XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT, d_tap);
			const bool fist_pose = have_joints
					&& d_mid < 0.068f && d_ring < 0.068f && d_lit < 0.068f && d_idx < 0.075f
					&& (tip_gap < 0.0f || tip_gap > 0.024f);
			if(fist_pose && d_tap < 0.060f)
			{
				const bool cooldown_ok = (now_ms - micro_last_fire_ms_[h]) > 550;

				if(d_tap >= 0.027f)
				{
					micro_tap_armed_[h] = true;
				}
				else if(micro_tap_armed_[h] && d_tap <= 0.019f && cooldown_ok && local_micro_event == 0)
				{
					local_micro_event = (h == 0) ? 1 : 2;
					micro_last_fire_ms_[h] = now_ms;
					micro_tap_armed_[h] = false;
				}
			}
			else
			{
				micro_tap_armed_[h] = false;
			}
		}
		else
		{
			micro_tap_armed_[h] = false;
		}

		// Aim ray: prefer XR_FB_hand_tracking_aim aimPose, fall back to wrist -> index knuckle
		if(hand_aim_supported_ && (aim_state.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB))
		{
			ray_orig[h] = aim_state.aimPose.position;
			ray_dir[h] = rotVec(aim_state.aimPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
			aim_valid[h] = true;
			ray_src[h] = 1;
		}
		else
		{
			const auto &wrist = joints[XR_HAND_JOINT_WRIST_EXT];
			const auto &knuckle = joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT];
			if((wrist.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
			   (knuckle.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
			{
				float dx = knuckle.pose.position.x - wrist.pose.position.x;
				float dy = knuckle.pose.position.y - wrist.pose.position.y;
				float dz = knuckle.pose.position.z - wrist.pose.position.z;
				float len = std::sqrt(dx * dx + dy * dy + dz * dz);
				if(len > 0.01f)
				{
					ray_orig[h] = knuckle.pose.position;
					ray_dir[h] = XrVector3f{dx / len, dy / len, dz / len};
					aim_valid[h] = true;
					ray_src[h] = 2;
				}
			}
		}

		if(aim_valid[h])
		{
			hand_last_track_ok_ms_[h] = now_ms;
		}
		else if(hand_pinch_down_[h] && actively_manip_h && (now_ms - hand_last_track_ok_ms_[h]) < 220)
		{
			hand_grace_hold[h] = true;
		}

		// Left hand (h == 0): check if palm faces the viewer's head (-Y of palm pose)
		if(h == 0)
		{
			const auto &palm = joints[XR_HAND_JOINT_PALM_EXT];
			if((palm.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
			   (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			{
				XrVector3f palm_out = rotVec(palm.pose.orientation, XrVector3f{0.0f, -1.0f, 0.0f});
				float hx = head_pos.x - palm.pose.position.x;
				float hy = head_pos.y - palm.pose.position.y;
				float hz = head_pos.z - palm.pose.position.z;
				float hlen = std::sqrt(hx * hx + hy * hy + hz * hz);
				if(hlen > 0.05f)
				{
					float dot = (palm_out.x * hx + palm_out.y * hy + palm_out.z * hz) / hlen;
					left_palm_facing_head = (dot > 0.55f);
				}
			}
		}
	}

	// Left palm toward self + pinch toggles hand tracking interaction ON/OFF (only when idle)
	bool left_pinch_rising = (pinch[0] && !prev_left_pinch_);
	prev_left_pinch_ = pinch[0];
	if(!pinch[0])
		left_pinch_swallowed_ = false;

	if(hand_gesture_mode_ == HandGestureMode::None && left_pinch_rising && left_palm_facing_head)
	{
		bool next_enabled = !hand_tracking_enabled_.load();
		hand_tracking_enabled_.store(next_enabled);
		left_pinch_swallowed_ = true;
		{
			std::lock_guard<std::mutex> lock(hand_mutex_);
			pending_hand_toggle_event_ = next_enabled ? 1 : -1;
		}
	if(!next_enabled)
		{
			if(hand_cursor_active_.exchange(false))
			{
				cursor_visible_[0].store(false);
				cursor_visible_[1].store(false);
			}
			hand_cursor_hidden_.store(false);
			corner_hovered_.store(false);
			corner_resizing_.store(false);
			if(hand_is_manipulating_.exchange(false))
			{
				std::lock_guard<std::mutex> lock(hand_mutex_);
				pending_save_prefs_ = true;
			}
			hand_gesture_mode_ = HandGestureMode::None;
			hand_active_idx_ = -1;
			for(int h = 0; h < 2; h++)
			{
				smooth_ray_valid_[h] = false;
				smooth_depth_valid_[h] = false;
				hand_pinch_down_[h] = false;
				hand_pinch_drop_since_ms_[h] = 0;
				hand_open_confirm_frames_[h] = 0;
				hand_prev_ray_src_[h] = 0;
			}
		}
		LOGI("Hand tracking toggled via left-palm pinch: %s", next_enabled ? "ON" : "PAUSED");
	}

	auto cancelNativeGesture = [&](bool save) {
		if(hand_gesture_mode_ != HandGestureMode::None)
		{
			hand_gesture_mode_ = HandGestureMode::None;
			hand_active_idx_ = -1;
			hand_is_manipulating_.store(false);
			hand_cursor_hidden_.store(false);
			corner_resizing_.store(false);
			for(int i = 0; i < 2; i++)
			{
				hand_pinch_down_[i] = false;
				hand_pinch_drop_since_ms_[i] = 0;
				hand_open_confirm_frames_[i] = 0;
				hand_prev_ray_src_[i] = 0;
			}
			if(save)
			{
				std::lock_guard<std::mutex> lock(hand_mutex_);
				pending_save_prefs_ = true;
			}
		}
	};

	if(!hand_tracking_enabled_.load())
	{
		cancelNativeGesture(true);
		if(hand_cursor_active_.exchange(false))
		{
			cursor_visible_[0].store(false);
			cursor_visible_[1].store(false);
		}
		std::lock_guard<std::mutex> lock(hand_mutex_);
		for(int h = 0; h < 2; h++)
		{
			out_hand_active_[h] = false;
			out_hand_pinch_[h] = false;
			out_hand_hit_type_[h] = 0;
		}
		out_two_hand_span_ = 0.0f;
		return;
	}

	int64_t ctrl_ms = last_controller_active_ms_.load();
	if(now_ms - ctrl_ms < 400)
	{
		cancelNativeGesture(true);
		if(hand_cursor_active_.exchange(false))
			cursor_visible_[1].store(false);
		std::lock_guard<std::mutex> lock(hand_mutex_);
		for(int h = 0; h < 2; h++)
		{
			out_hand_active_[h] = false;
			out_hand_pinch_[h] = false;
			out_hand_hit_type_[h] = 0;
		}
		out_two_hand_span_ = 0.0f;
		return;
	}

	// Effective pinch per hand (swallow left toggle pinch until release)
	bool eff_pinch[2] = {pinch[0] && !left_pinch_swallowed_, pinch[1]};

	if(!aim_valid[0] && !aim_valid[1] && !hand_grace_hold[0] && !hand_grace_hold[1])
	{
		cancelNativeGesture(true);
		if(hand_cursor_active_.exchange(false))
		{
			cursor_visible_[0].store(false);
			cursor_visible_[1].store(false);
		}
		corner_hovered_.store(false);
		for(int h = 0; h < 2; h++)
		{
			smooth_ray_valid_[h] = false;
			smooth_depth_valid_[h] = false;
			hand_prev_ray_src_[h] = 0;
		}
		std::lock_guard<std::mutex> lock(hand_mutex_);
		for(int h = 0; h < 2; h++)
		{
			out_hand_active_[h] = false;
			out_hand_pinch_[h] = false;
			out_hand_hit_type_[h] = 0;
		}
		out_two_hand_span_ = 0.0f;
		return;
	}

	hand_cursor_active_.store(true);
	bool any_corner = false;
	bool h_active[2] = {false, false};
	int h_hit[2] = {0, 0};
	float h_u[2] = {0.0f, 0.0f}, h_v[2] = {0.0f, 0.0f};
	float h_yaw[2] = {0.0f, 0.0f}, h_pitch[2] = {0.0f, 0.0f}, h_depth[2] = {0.0f, 0.0f};

	const float a = 0.45f;
	for(int h = 0; h < 2; h++)
	{
		if(!aim_valid[h])
		{
			if(hand_grace_hold[h])
			{
				h_yaw[h] = hand_prev_yaw_[h];
				h_pitch[h] = hand_prev_pitch_[h];
				h_depth[h] = smooth_depth_valid_[h] ? smooth_hand_depth_[h] : h_anchor_depth_;
				h_active[h] = true;
				cursor_visible_[h].store(false);
				continue;
			}
			smooth_ray_valid_[h] = false;
			smooth_depth_valid_[h] = false;
			hand_prev_ray_src_[h] = 0;
			cursor_visible_[h].store(false);
			continue;
		}

		if(!smooth_ray_valid_[h])
		{
			smooth_ray_ox_[h] = ray_orig[h].x;
			smooth_ray_oy_[h] = ray_orig[h].y;
			smooth_ray_oz_[h] = ray_orig[h].z;
			smooth_ray_dx_[h] = ray_dir[h].x;
			smooth_ray_dy_[h] = ray_dir[h].y;
			smooth_ray_dz_[h] = ray_dir[h].z;
			smooth_ray_valid_[h] = true;
		}
		else
		{
			smooth_ray_ox_[h] += a * (ray_orig[h].x - smooth_ray_ox_[h]);
			smooth_ray_oy_[h] += a * (ray_orig[h].y - smooth_ray_oy_[h]);
			smooth_ray_oz_[h] += a * (ray_orig[h].z - smooth_ray_oz_[h]);
			smooth_ray_dx_[h] += a * (ray_dir[h].x - smooth_ray_dx_[h]);
			smooth_ray_dy_[h] += a * (ray_dir[h].y - smooth_ray_dy_[h]);
			smooth_ray_dz_[h] += a * (ray_dir[h].z - smooth_ray_dz_[h]);
			float len = std::sqrt(smooth_ray_dx_[h] * smooth_ray_dx_[h]
					+ smooth_ray_dy_[h] * smooth_ray_dy_[h]
					+ smooth_ray_dz_[h] * smooth_ray_dz_[h]);
			if(len > 1e-4f)
			{
				smooth_ray_dx_[h] /= len;
				smooth_ray_dy_[h] /= len;
				smooth_ray_dz_[h] /= len;
			}
		}

		h_hit[h] = UpdatePointerRay(h,
				smooth_ray_ox_[h], smooth_ray_oy_[h], smooth_ray_oz_[h],
				smooth_ray_dx_[h], smooth_ray_dy_[h], smooth_ray_dz_[h],
				eff_pinch[h] ? 0x40 : 0, &h_u[h], &h_v[h], false);
		if(h_hit[h] == 4)
			any_corner = true;

		h_yaw[h] = std::atan2(-smooth_ray_dx_[h], -smooth_ray_dz_[h]);
		h_pitch[h] = std::asin(std::clamp(smooth_ray_dy_[h], -1.0f, 1.0f));
		if(hand_gesture_mode_ != HandGestureMode::None && hand_active_idx_ == h
				&& hand_prev_ray_src_[h] != 0 && ray_src[h] != hand_prev_ray_src_[h])
		{
			h_anchor_yaw_ += (h_yaw[h] - hand_prev_yaw_[h]);
			h_anchor_pitch_ += (h_pitch[h] - hand_prev_pitch_[h]);
		}
		hand_prev_ray_src_[h] = ray_src[h];
		hand_prev_yaw_[h] = h_yaw[h];
		hand_prev_pitch_[h] = h_pitch[h];

		float dhx = ray_orig[h].x - head_pos.x;
		float dhy = ray_orig[h].y - head_pos.y;
		float dhz = ray_orig[h].z - head_pos.z;
		float raw_depth = std::sqrt(dhx * dhx + dhy * dhy + dhz * dhz);
		if(!smooth_depth_valid_[h])
		{
			smooth_hand_depth_[h] = raw_depth;
			smooth_depth_valid_[h] = true;
		}
		else
		{
			smooth_hand_depth_[h] += 0.45f * (raw_depth - smooth_hand_depth_[h]);
		}
		h_depth[h] = smooth_hand_depth_[h];
		h_active[h] = true;
	}
	corner_hovered_.store(hud_visible_.load() && any_corner);

	float span = 0.0f;
	if(h_active[0] && h_active[1])
	{
		float sx = smooth_ray_ox_[1] - smooth_ray_ox_[0];
		float sy = smooth_ray_oy_[1] - smooth_ray_oy_[0];
		float sz = smooth_ray_oz_[1] - smooth_ray_oz_[0];
		span = std::sqrt(sx * sx + sy * sy + sz * sz);
	}

	// Apply hand screen drag / zoom / corner resize natively at OpenXR VSync so screen
	// motion has zero UI-thread lag or phase-beat stutter against head tracking & passthrough.
	auto applyFovDistLocked = [&](float baseFov, float dist) {
		std::lock_guard<std::mutex> lock(params_mutex_);
		float distRatio = std::sqrt(2.5f / std::max(0.2f, dist));
		float effFov = 2.0f * std::atan(std::tan(baseFov * 0.5f) * distRatio);
		effFov = std::clamp(effFov, 0.05f, 3.00f);
		float fov = (params_.aspectRatio > 0.01f && params_.aspectRatio < 1.0f)
				? (effFov * params_.aspectRatio) : effFov;
		params_.radius = dist;
		params_.centralAngle = fov;
	};

	bool pinch_now[2] = {h_active[0] && eff_pinch[0], h_active[1] && eff_pinch[1]};
	static bool prev_eff_pinch[2] = {false, false};
	bool pressed[2] = {pinch_now[0] && !prev_eff_pinch[0], pinch_now[1] && !prev_eff_pinch[1]};
	prev_eff_pinch[0] = pinch_now[0];
	prev_eff_pinch[1] = pinch_now[1];

	int local_tap_hit = 0;
	bool local_save_prefs = false;

	if(pinch_now[0] && pinch_now[1] && span > 0.04f)
	{
		if(hand_gesture_mode_ != HandGestureMode::TwoHandResize)
		{
			hand_gesture_mode_ = HandGestureMode::TwoHandResize;
			hand_active_idx_ = -1;
			corner_resizing_.store(false);
			hand_is_manipulating_.store(true);
			hand_cursor_hidden_.store(true);
			cursor_visible_[0].store(false);
			cursor_visible_[1].store(false);
			h_anchor_span_ = span;
			h_anchor_fov_ = hand_base_fov_.load();
		}
		else if(!hand_grace_hold[0] && !hand_grace_hold[1])
		{
			float scale = span / std::max(0.06f, h_anchor_span_);
			float next_fov = std::clamp(h_anchor_fov_ * scale, 0.05f, 3.00f);
			hand_base_fov_.store(next_fov);
			applyFovDistLocked(next_fov, hand_screen_dist_.load());
		}
	}
	else if(hand_gesture_mode_ == HandGestureMode::TwoHandResize)
	{
		local_save_prefs = true;
		int rem = pinch_now[1] ? 1 : (pinch_now[0] ? 0 : -1);
		if(rem >= 0)
		{
			hand_gesture_mode_ = HandGestureMode::SingleDrag;
			hand_active_idx_ = rem;
			hand_drag_moved_ = true;
			hand_pinch_start_ms_ = now_ms - 500;
			h_anchor_yaw_ = h_yaw[rem];
			h_anchor_pitch_ = h_pitch[rem];
			h_anchor_depth_ = h_depth[rem];
			h_anchor_scr_yaw_ = g_screen_yaw_offset.load();
			h_anchor_scr_pitch_ = g_screen_pitch_offset.load();
			h_anchor_dist_ = hand_screen_dist_.load();
			hand_is_manipulating_.store(true);
			hand_cursor_hidden_.store(true);
			cursor_visible_[0].store(false);
			cursor_visible_[1].store(false);
		}
		else
		{
			hand_gesture_mode_ = HandGestureMode::None;
			hand_active_idx_ = -1;
			hand_is_manipulating_.store(false);
			hand_cursor_hidden_.store(false);
		}
	}
	else
	{
		if(hand_gesture_mode_ == HandGestureMode::None)
		{
			int h = pressed[1] ? 1 : (pressed[0] ? 0 : -1);
			if(h >= 0)
			{
				const bool ui_hit = (hud_visible_.load() && h_hit[h] == 2)
						|| (settings_visible_.load() && hud_visible_.load() && h_hit[h] == 3);
				if(!ui_hit)
				{
					if(hud_visible_.load() && h_hit[h] == 4)
					{
						hand_gesture_mode_ = HandGestureMode::CornerResize;
						hand_active_idx_ = h;
						corner_resizing_.store(true);
						hand_is_manipulating_.store(true);
						hand_cursor_hidden_.store(false);
						h_anchor_yaw_ = h_yaw[h];
						h_anchor_pitch_ = h_pitch[h];
						h_anchor_fov_ = hand_base_fov_.load();
					}
					else
					{
						hand_gesture_mode_ = HandGestureMode::SingleDrag;
						hand_active_idx_ = h;
						hand_drag_moved_ = false;
						hand_pinch_start_ms_ = now_ms;
						hand_start_hit_type_ = h_hit[h];
						hand_is_manipulating_.store(true);
						hand_cursor_hidden_.store(true);
						cursor_visible_[0].store(false);
						cursor_visible_[1].store(false);
						h_anchor_yaw_ = h_yaw[h];
						h_anchor_pitch_ = h_pitch[h];
						h_anchor_depth_ = h_depth[h];
						h_anchor_scr_yaw_ = g_screen_yaw_offset.load();
						h_anchor_scr_pitch_ = g_screen_pitch_offset.load();
						h_anchor_dist_ = hand_screen_dist_.load();
					}
				}
			}
		}
		else if(hand_active_idx_ >= 0 && pinch_now[hand_active_idx_]
				&& (hand_gesture_mode_ != HandGestureMode::CornerResize || hud_visible_.load()))
		{
			int h = hand_active_idx_;
			if(hand_gesture_mode_ == HandGestureMode::CornerResize)
			{
				float dYaw = h_yaw[h] - h_anchor_yaw_;
				float dPitch = h_pitch[h] - h_anchor_pitch_;
				float diag = (-dYaw - dPitch) * 0.75f;
				float next_fov = std::clamp(h_anchor_fov_ * std::exp(diag * 1.8f), 0.05f, 3.00f);
				hand_base_fov_.store(next_fov);
				applyFovDistLocked(next_fov, hand_screen_dist_.load());
			}
			else if(hand_gesture_mode_ == HandGestureMode::SingleDrag)
			{
				cursor_visible_[0].store(false);
				cursor_visible_[1].store(false);
				if(!hand_grace_hold[h])
				{
					float dYaw = h_yaw[h] - h_anchor_yaw_;
					float dPitch = h_pitch[h] - h_anchor_pitch_;
					float dDepth = (h_anchor_depth_ > 0.05f && h_depth[h] > 0.05f) ? (h_depth[h] - h_anchor_depth_) : 0.0f;
					if(std::fabs(dYaw) > 0.014f || std::fabs(dPitch) > 0.014f || std::fabs(dDepth) > 0.020f
							|| (now_ms - hand_pinch_start_ms_) > 160)
					{
						hand_drag_moved_ = true;
					}
					g_screen_yaw_offset.store(h_anchor_scr_yaw_ + dYaw);
					g_screen_pitch_offset.store(std::clamp(h_anchor_scr_pitch_ + dPitch, -0.65f, 1.35f));
					float next_dist = (h_anchor_depth_ > 0.05f && h_depth[h] > 0.05f)
							? std::clamp(h_anchor_dist_ * std::exp(dDepth * 6.8f), 0.2f, 25.0f)
							: h_anchor_dist_;
					hand_screen_dist_.store(next_dist);
					applyFovDistLocked(hand_base_fov_.load(), next_dist);
				}
			}
		}
		else
		{
			if(hand_gesture_mode_ == HandGestureMode::CornerResize)
			{
				local_save_prefs = true;
			}
			else if(hand_gesture_mode_ == HandGestureMode::SingleDrag)
			{
				if(hand_drag_moved_)
				{
					local_save_prefs = true;
				}
				else
				{
					g_screen_yaw_offset.store(h_anchor_scr_yaw_);
					g_screen_pitch_offset.store(h_anchor_scr_pitch_);
					hand_screen_dist_.store(h_anchor_dist_);
					applyFovDistLocked(hand_base_fov_.load(), h_anchor_dist_);
					local_tap_hit = (hand_start_hit_type_ > 0) ? hand_start_hit_type_ : 1;
				}
			}
			hand_gesture_mode_ = HandGestureMode::None;
			hand_active_idx_ = -1;
			hand_is_manipulating_.store(false);
			hand_cursor_hidden_.store(false);
			corner_resizing_.store(false);
		}
	}

	std::lock_guard<std::mutex> lock(hand_mutex_);
	for(int h = 0; h < 2; h++)
	{
		out_hand_active_[h] = h_active[h];
		out_hand_pinch_[h] = eff_pinch[h];
		out_hand_hit_type_[h] = h_hit[h];
		out_hand_hit_u_[h] = h_u[h];
		out_hand_hit_v_[h] = h_v[h];
		out_hand_ray_yaw_[h] = h_yaw[h];
		out_hand_ray_pitch_[h] = h_pitch[h];
		out_hand_depth_[h] = h_depth[h];
	}
	out_two_hand_span_ = span;
	if(local_tap_hit > 0)
		pending_tap_hit_type_ = local_tap_hit;
	if(local_micro_event > 0)
		pending_microgesture_event_ = local_micro_event;
	if(local_save_prefs)
		pending_save_prefs_ = true;
}

bool XrVideoSession::UpdateCornerLayer(XrCompositionLayerQuad &layer)
{
	if(corner_swapchain_ == XR_NULL_HANDLE || corner_images_.empty())
		return false;
	if(projection_mode_.load() != 0)
		return false;

	if(!corner_has_image_.load())
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(corner_swapchain_, &acquire, &index)))
		{
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 10000000;
			if(XR_SUCCEEDED(xrWaitSwapchainImage(corner_swapchain_, &wait)))
			{
				const int S = 64;
				std::vector<uint8_t> pixels(S * S * 4, 0);
				// Crisp MoonlightXR / visionOS rounded bottom-right resize bracket "⌟" in GL bottom-up coords
				for(int y = 0; y < S; y++)
				{
					for(int x = 0; x < S; x++)
					{
						float fx = (float)x + 0.5f;
						float fy = (float)y + 0.5f;
						// Horizontal arm (16..42, y=10), Vertical arm (x=54, 22..48), Fillet arc (42, 22, r=12)
						float dh = std::hypot(std::max({16.0f - fx, fx - 42.0f, 0.0f}), std::fabs(fy - 10.0f));
						float dv = std::hypot(std::fabs(fx - 54.0f), std::max({22.0f - fy, fy - 48.0f, 0.0f}));
						float darc = (fx >= 42.0f && fy <= 22.0f)
								? std::fabs(std::hypot(fx - 42.0f, fy - 22.0f) - 12.0f)
								: 1e5f;
						float d = std::min({dh, dv, darc});
						size_t idx = (size_t)(y * S + x) * 4;
						if(d <= 3.1f)
						{
							pixels[idx + 0] = 255;
							pixels[idx + 1] = 255;
							pixels[idx + 2] = 255;
							pixels[idx + 3] = 250;
						}
						else if(d <= 4.3f)
						{
							float t = std::clamp((4.3f - d) / 1.2f, 0.0f, 1.0f);
							uint8_t c = (uint8_t)(255.0f * t + 16.0f * (1.0f - t));
							pixels[idx + 0] = c;
							pixels[idx + 1] = c;
							pixels[idx + 2] = (uint8_t)(255.0f * t + 28.0f * (1.0f - t));
							pixels[idx + 3] = (uint8_t)(250.0f * t + 190.0f * (1.0f - t));
						}
						else if(d <= 6.2f)
						{
							float t = std::clamp((6.2f - d) / 1.9f, 0.0f, 1.0f);
							pixels[idx + 0] = 16;
							pixels[idx + 1] = 22;
							pixels[idx + 2] = 34;
							pixels[idx + 3] = (uint8_t)(190.0f * t);
						}
					}
				}
				glBindTexture(GL_TEXTURE_2D, corner_images_[index].image);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
				glBindTexture(GL_TEXTURE_2D, 0);
				corner_has_image_.store(true);
			}
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			xrReleaseSwapchainImage(corner_swapchain_, &release);
		}
	}

	const bool show = hud_visible_.load() && (corner_hovered_.load() || corner_resizing_.load());
	if(!corner_has_image_.load() || !show)
		return false;

	ScreenParams params;
	XrQuaternionf recenter;
	XrVector3f recenter_pos;
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		params = params_;
		recenter = recenter_orientation_;
		recenter_pos = recenter_position_;
	}

	const float total_yaw = YawFromQuat(recenter) + params.yawOffset + g_screen_yaw_offset.load();
	const float total_pitch = PitchFromQuat(recenter) + g_screen_pitch_offset.load();
	const float cos_sp = std::cos(total_pitch);
	const float sin_sp = std::sin(total_pitch);
	const XrQuaternionf scr_ori = QuatFromYawPitch(total_yaw, total_pitch);
	const float dist = std::max(0.5f, params.radius);

	XrVector3f corner_world_pos{};
	XrQuaternionf corner_ori = scr_ori;
	float handle_size = 0.16f;

	if(layer_shape_.load() == LayerShape::Cylinder)
	{
		const float curvature = params.curvature > 0.001f ? params.curvature : 0.1f;
		const float arc_radius = dist / curvature;
		const float axis_back = arc_radius - dist;
		const float cyl_angle = std::min(5.5f, 2.0f * curvature * std::tan(std::min(params.centralAngle, 3.05f) * 0.5f));
		const float half_cyl_angle = cyl_angle * 0.5f;
		const float arc_len = arc_radius * cyl_angle;
		const float scr_h = arc_len / std::max(0.2f, params.aspectRatio);
		handle_size = std::clamp(std::min(dist * 0.085f, scr_h * 0.18f), 0.06f, 0.38f);

		const XrVector3f cyl_pos{
			recenter_pos.x + std::sin(total_yaw) * cos_sp * axis_back,
			recenter_pos.y + params.heightOffset - sin_sp * axis_back,
			recenter_pos.z + std::cos(total_yaw) * cos_sp * axis_back
		};
		// Exact bottom-right corner on the curved cylinder surface
		const XrVector3f edge_cyl{
			arc_radius * std::sin(half_cyl_angle),
			-0.5f * scr_h,
			-arc_radius * std::cos(half_cyl_angle)
		};
		const XrVector3f edge_world = RotateVector(scr_ori, edge_cyl);
		corner_ori = QuatMul(scr_ori, QuatFromYawPitch(-half_cyl_angle, 0.0f));
		// Align bracket outer corner flush with the screen corner, slightly in front (+Z)
		const XrVector3f tangent_off = RotateVector(corner_ori,
				XrVector3f{-0.32f * handle_size, 0.32f * handle_size, 0.03f});
		corner_world_pos = XrVector3f{
			cyl_pos.x + edge_world.x + tangent_off.x,
			cyl_pos.y + edge_world.y + tangent_off.y,
			cyl_pos.z + edge_world.z + tangent_off.z
		};
	}
	else
	{
		const XrVector3f scr_pos{
			recenter_pos.x - std::sin(total_yaw) * cos_sp * dist,
			recenter_pos.y + params.heightOffset + sin_sp * dist,
			recenter_pos.z - std::cos(total_yaw) * cos_sp * dist
		};
		const float scr_w = 2.0f * dist * std::tan(params.centralAngle * 0.5f);
		const float scr_h = scr_w / std::max(0.2f, params.aspectRatio);
		handle_size = std::clamp(std::min(dist * 0.085f, scr_h * 0.18f), 0.06f, 0.38f);
		const XrVector3f local_off{
			0.5f * scr_w - 0.32f * handle_size,
			-0.5f * scr_h + 0.32f * handle_size,
			0.03f
		};
		const XrVector3f world_off = RotateVector(scr_ori, local_off);
		corner_world_pos = XrVector3f{
			scr_pos.x + world_off.x,
			scr_pos.y + world_off.y,
			scr_pos.z + world_off.z
		};
	}

	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	layer.space = app_space_;
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = corner_swapchain_;
	layer.subImage.imageRect.offset = XrOffset2Di{0, 0};
	layer.subImage.imageRect.extent = XrExtent2Di{64, 64};
	layer.subImage.imageArrayIndex = 0;
	layer.size = XrExtent2Df{handle_size, handle_size};
	layer.pose.position = corner_world_pos;
	layer.pose.orientation = corner_ori;
	return true;
}

bool XrVideoSession::CreateGlVideoSwapchain(int32_t width, int32_t height, bool want_stereo)
{
	// MoonlightXR pattern: use a single double-wide SBS swapchain (2W x H) when
	// stereo is active and within runtime swapchain width limits, eliminating the
	// second swapchain's xrAcquire/Wait/Release calls and per-eye FBO/uniform setup.
	// If 2*W exceeds maxSwapchainImageWidth (e.g. 4K on a 4096 cap), fall back to
	// two W x H swapchains rather than downscaling per-eye resolution.
	uint32_t format_count = 0;
	if(XR_FAILED(xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr))
			|| format_count == 0)
		return false;
	std::vector<int64_t> formats(format_count);
	if(XR_FAILED(xrEnumerateSwapchainFormats(session_, format_count, &format_count,
			formats.data())))
		return false;

	const int64_t kSrgb8Alpha8 = 0x8C43;
	const int64_t kRgba8 = 0x8058;
	int64_t chosen = 0;
	for(int64_t candidate : { kSrgb8Alpha8, kRgba8 })
	{
		for(int64_t f : formats)
			if(f == candidate) { chosen = candidate; break; }
		if(chosen != 0)
			break;
	}
	if(chosen == 0)
	{
		LOGE("No 8-bit RGBA format for the video");
		return false;
	}

	// Cap per-eye OpenXR swapchain textures at 4096px (matching the pre-P0/P1
	// ceiling). Even when the runtime reports maxSwapchain >= 8192, rendering 8K
	// through a 7680x4320 / 8192x4096 swapchain multiplies fillrate by ~3.5-4x
	// and starves 8K MediaCodec bandwidth, while keep SurfaceTexture supersampled.
	constexpr int32_t kMaxGlSwapchainEdge = 4096;
	const int32_t maxW = std::min(kMaxGlSwapchainEdge,
			(int32_t)(max_swapchain_width_ > 0 ? max_swapchain_width_ : kMaxGlSwapchainEdge));
	const int32_t maxH = std::min(kMaxGlSwapchainEdge,
			(int32_t)(max_swapchain_height_ > 0 ? max_swapchain_height_ : kMaxGlSwapchainEdge));
	int32_t eyeW = width;
	int32_t eyeH = height;
	if(eyeW > maxW || eyeH > maxH)
	{
		float scale = std::min((float)maxW / (float)eyeW, (float)maxH / (float)eyeH);
		eyeW = std::max(2, ((int32_t)(eyeW * scale)) & ~1);
		eyeH = std::max(2, ((int32_t)(eyeH * scale)) & ~1);
		LOGI("Clamping per-eye OpenXR swapchain to %dx%d (max %dx%d)", eyeW, eyeH, maxW, maxH);
	}

	bool use_wide_sbs = want_stereo && (eyeW * 2 <= maxW);
	int32_t scW = use_wide_sbs ? (eyeW * 2) : eyeW;
	int32_t scH = eyeH;

	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	info.format = chosen;
	info.sampleCount = 1;
	info.width = (uint32_t)scW;
	info.height = (uint32_t)scH;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	XrResult res = xrCreateSwapchain(session_, &info, &video_swapchain_);
	if(XR_FAILED(res) && use_wide_sbs)
	{
		LOGW("Wide SBS swapchain %dx%d refused (%s), falling back to dual %dx%d",
				scW, scH, XrResultStr(instance_, res), eyeW, eyeH);
		use_wide_sbs = false;
		scW = eyeW;
		info.width = (uint32_t)scW;
		res = xrCreateSwapchain(session_, &info, &video_swapchain_);
	}
	if(XR_FAILED(res))
	{
		LOGE("xrCreateSwapchain(video GL): %s", XrResultStr(instance_, res));
		video_swapchain_ = XR_NULL_HANDLE;
		return false;
	}

	uint32_t image_count = 0;
	if(XR_FAILED(xrEnumerateSwapchainImages(video_swapchain_, 0, &image_count, nullptr))
			|| image_count == 0)
	{
		xrDestroySwapchain(video_swapchain_);
		video_swapchain_ = XR_NULL_HANDLE;
		return false;
	}
	video_images_.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
	if(XR_FAILED(xrEnumerateSwapchainImages(video_swapchain_, image_count, &image_count,
			reinterpret_cast<XrSwapchainImageBaseHeader *>(video_images_.data()))))
	{
		xrDestroySwapchain(video_swapchain_);
		video_swapchain_ = XR_NULL_HANDLE;
		video_images_.clear();
		return false;
	}

	for(uint32_t i = 0; i < image_count; i++)
	{
		if(video_images_[i].image == 0)
		{
			LOGE("The GL video swapchain returned a zeroed texture at index %u: "
					"there is no EGL context current on this thread", i);
			xrDestroySwapchain(video_swapchain_);
			video_swapchain_ = XR_NULL_HANDLE;
			video_images_.clear();
			return false;
		}
	}

	video_gl_format_ = chosen;
	eye_width_ = eyeW;
	eye_height_ = eyeH;
	video_width_ = scW;
	video_height_ = scH;
	wide_sbs_swapchain_ = use_wide_sbs;
	if(eyeW > 0 && eyeH > 0)
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		params_.aspectRatio = (float)eyeW / (float)eyeH;
	}

	if(want_stereo && !use_wide_sbs)
	{
		XrResult res2 = xrCreateSwapchain(session_, &info, &video_swapchain_right_);
		if(XR_SUCCEEDED(res2))
		{
			uint32_t direita = 0;
			xrEnumerateSwapchainImages(video_swapchain_right_, 0, &direita, nullptr);
			video_images_right_.assign(direita, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
			if(direita == 0 || XR_FAILED(xrEnumerateSwapchainImages(video_swapchain_right_,
					direita, &direita,
					reinterpret_cast<XrSwapchainImageBaseHeader *>(video_images_right_.data()))))
			{
				LOGW("The right eye swapchain returned no images");
				xrDestroySwapchain(video_swapchain_right_);
				video_swapchain_right_ = XR_NULL_HANDLE;
				video_images_right_.clear();
			}
			else
			{
				LOGI("Dual-swapchain 3D fallback: right eye %dx%d, %u images",
						eyeW, eyeH, direita);
			}
		}
		else
		{
			LOGW("xrCreateSwapchain(right eye) failed (%s), stereo fallback limited",
					XrResultStr(instance_, res2));
			video_swapchain_right_ = XR_NULL_HANDLE;
		}
	}

	tone_mapper_.ResetFboCache();
	LOGI("GL video swapchain: %dx%d (eye %dx%d, wide_sbs=%d, dual=%d), format 0x%llx, %u images",
			scW, scH, eyeW, eyeH, (int)use_wide_sbs,
			(int)(video_swapchain_right_ != XR_NULL_HANDLE),
			(unsigned long long)chosen, image_count);
	return true;
}

bool XrVideoSession::AttachSurfaceTexture(JNIEnv *env, jobject surface_texture)
{
	if(render_path_ != RenderPath::ToneMapped)
	{
		LOGW("SurfaceTexture handed over on the direct path; ignoring");
		return false;
	}
	ASurfaceTexture *native = ASurfaceTexture_fromSurfaceTexture(env, surface_texture);
	if(!native)
	{
		last_error_ = "ASurfaceTexture_fromSurfaceTexture failed";
		LOGE("%s", last_error_.c_str());
		return false;
	}
	// Guardada para a thread do frame loop: a textura externa e o anexo
	// pertencem ao contexto EGL de la, nao ao desta chamada.
	pending_surface_texture_ = native;
	return true;
}

bool XrVideoSession::RenderVideoThroughShader()
{
	int stereo_eff_mode = 0;
	const int proj = projection_mode_.load();
	if (proj == 2 || proj == 5) {
		stereo_eff_mode = 2; // SBS (VR180 SBS / VR360 SBS)
	} else if (proj == 3 || proj == 6) {
		stereo_eff_mode = 3; // OU (VR180 OU / VR360 OU)
	} else if (stereo_synthetic_.load() && stereo_mode_.load() != 0) {
		stereo_eff_mode = 1; // AI 3D
	} else if (stereo_mode_.load() == 1) {
		stereo_eff_mode = 2; // SBS
	} else if (stereo_mode_.load() == 2) {
		stereo_eff_mode = 3; // OU
	}
	const bool want_stereo = (stereo_eff_mode != 0);

	int32_t req_w = 0;
	int32_t req_h = 0;
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		req_w = pending_gl_width_;
		req_h = pending_gl_height_;
		pending_gl_width_ = 0;
		pending_gl_height_ = 0;
	}
	const bool size_changed = req_w > 0 && req_h > 0
			&& (req_w != requested_eye_w_ || req_h != requested_eye_h_);
	if(req_w > 0 && req_h > 0)
	{
		requested_eye_w_ = req_w;
		requested_eye_h_ = req_h;
	}

	int32_t target_eye_w = requested_eye_w_;
	int32_t target_eye_h = requested_eye_h_;
	if(target_eye_w > 0 && target_eye_h > 0)
	{
		// Quest 3 panel width is ~2064px/eye. Capping 4K stereo GL output at 2048
		// keeps SurfaceTexture full 4K supersampled, fits both eyes into ONE single
		// wide_sbs swapchain (<=4096), and cuts stereo fillrate by ~71% (16.6M -> 4.7M px).
		constexpr int32_t kMaxStereoGlEdge = 2048;
		const int32_t max_dim = std::max(target_eye_w, target_eye_h);
		const int cur_proj = projection_mode_.load();
		const bool cap_ai = (stereo_eff_mode == 1) && (max_dim > kMaxStereoGlEdge);
		const bool cap_cinema_packed = ((stereo_eff_mode == 2 || stereo_eff_mode == 3)
				&& cur_proj == 0 && target_eye_w > kMaxStereoGlEdge && target_eye_h > 1200);
		if((cap_ai || cap_cinema_packed) && max_dim > kMaxStereoGlEdge)
		{
			const float s = (float)kMaxStereoGlEdge / (float)max_dim;
			target_eye_w = std::max(2, ((int32_t)std::lround((double)target_eye_w * s)) & ~1);
			target_eye_h = std::max(2, ((int32_t)std::lround((double)target_eye_h * s)) & ~1);
		}
	}

	const bool have_stereo_sc = wide_sbs_swapchain_ || (video_swapchain_right_ != XR_NULL_HANDLE);
	const bool layout_changed = (want_stereo != have_stereo_sc);
	const bool gl_size_changed = target_eye_w > 0 && target_eye_h > 0
			&& (target_eye_w != active_gl_req_w_ || target_eye_h != active_gl_req_h_);

	if(target_eye_w > 0 && target_eye_h > 0
			&& (video_swapchain_ == XR_NULL_HANDLE || size_changed || gl_size_changed || layout_changed))
	{
		float saved_aspect = 0.0f;
		if(!size_changed)
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			saved_aspect = params_.aspectRatio;
		}
		if(video_swapchain_ != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(video_swapchain_);
			video_swapchain_ = XR_NULL_HANDLE;
			video_images_.clear();
		}
		if(video_swapchain_right_ != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(video_swapchain_right_);
			video_swapchain_right_ = XR_NULL_HANDLE;
			video_images_right_.clear();
		}
		wide_sbs_swapchain_ = false;
		video_swapchain_rendered_ = false;
		tone_mapper_.ResetFboCache();
		if(!CreateGlVideoSwapchain(target_eye_w, target_eye_h, want_stereo))
		{
			last_error_ = "Could not create the video swapchain for the shader";
			LOGE("%s", last_error_.c_str());
			return false;
		}
		active_gl_req_w_ = target_eye_w;
		active_gl_req_h_ = target_eye_h;
		if(!size_changed && saved_aspect > 0.01f)
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params_.aspectRatio = saved_aspect;
		}
	}
	if(video_swapchain_ == XR_NULL_HANDLE)
		return false;

	if(pending_surface_texture_ && !tone_mapper_ready_)
	{
		if(!tone_mapper_.Init() || !tone_mapper_.Attach(pending_surface_texture_))
		{
			LOGE("Tone mapper unavailable; nothing will be displayed");
			pending_surface_texture_ = nullptr;
			return false;
		}
		tone_mapper_ready_ = true;
		pending_surface_texture_ = nullptr;
		tone_mapper_.EnableExtrapolation((GLenum)video_gl_format_);
	}
	if(!tone_mapper_ready_ || video_images_.empty())
		return false;

	float sharpen = 0.0f;
	bool extrapolate = false;
	Sharpness nitidez = Sharpness::Off;
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		sharpen = quality_.sharpenAmount;
		extrapolate = quality_.frameExtrapolation;
		nitidez = quality_.sharpness;
	}

	if(stereo_mode_.load() != 0 && !stereo_synthetic_.load()
			&& (nitidez == Sharpness::Mqsr || nitidez == Sharpness::Auto))
	{
		sharpen = 1.0f;
		if(!logged_stereo_sharpen_)
		{
			logged_stereo_sharpen_ = true;
			LOGI("Packed 3D: the compositor filter does not apply to a layer that is "
					"half of a texture, so sharpening falls back to the shader");
		}
	}

	float tm_strength = 0.0f;
	float tm_conv = 0.0f;
	if(stereo_eff_mode == 1)
	{
		tm_strength = stereo_strength_.load();
		if(tm_strength > 0.05f) tm_strength = 0.05f;
		tm_conv = stereo_convergence_.load();
		tone_mapper_.SetStereo(1, tm_strength, tm_conv, true);
	}
	else if(stereo_eff_mode == 2)
	{
		tone_mapper_.SetStereo(2, 0.0f, 0.0f, true);
	}
	else if(stereo_eff_mode == 3)
	{
		tone_mapper_.SetStereo(3, 0.0f, 0.0f, true);
	}
	else
	{
		tone_mapper_.SetStereo(0, 0.0f, 0.0f, true);
	}

	tone_mapper_.SetAiManipulating(
			hand_is_manipulating_.load(std::memory_order_acquire)
			|| corner_resizing_.load(std::memory_order_acquire)
			|| java_manipulating_.load(std::memory_order_acquire));

	const ToneMapper::FrameAction frameAction = tone_mapper_.PrepareFrame(
			eye_width_, eye_height_, stereo_eff_mode, tm_strength, tm_conv,
			tone_map_pq_.load(), sharpen, video_swapchain_rendered_);
	if(frameAction == ToneMapper::FrameAction::kNoFrame)
		return false;
	if(frameAction == ToneMapper::FrameAction::kReuseLast && video_swapchain_rendered_)
	{
		right_layer_ready_ = want_stereo && (wide_sbs_swapchain_ || video_swapchain_right_ != XR_NULL_HANDLE);
		return true;
	}

	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if(XR_FAILED(xrAcquireSwapchainImage(video_swapchain_, &acquire, &index)))
		return false;

	XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wait.timeout = XR_INFINITE_DURATION;
	bool drawn = false;

	uint32_t index_right = 0;
	bool right_acquired = false;
	right_layer_ready_ = false;
	if(!wide_sbs_swapchain_ && video_swapchain_right_ != XR_NULL_HANDLE && want_stereo)
	{
		XrSwapchainImageAcquireInfo acquire_right{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		if(XR_SUCCEEDED(xrAcquireSwapchainImage(video_swapchain_right_, &acquire_right,
				&index_right)))
		{
			XrSwapchainImageWaitInfo wait_right{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait_right.timeout = XR_INFINITE_DURATION;
			right_acquired = XR_SUCCEEDED(xrWaitSwapchainImage(video_swapchain_right_,
					&wait_right));
			if(!right_acquired)
			{
				XrSwapchainImageReleaseInfo solta{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(video_swapchain_right_, &solta);
			}
		}
	}

	if(XR_SUCCEEDED(xrWaitSwapchainImage(video_swapchain_, &wait)))
	{
		drawn = tone_mapper_.RenderPrepared(video_images_[index].image, eye_width_, eye_height_,
				tone_map_pq_.load(), sharpen, false, extrapolate,
				right_acquired ? video_images_right_[index_right].image : 0,
				wide_sbs_swapchain_);
	}

	XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	xrReleaseSwapchainImage(video_swapchain_, &release);
	if(wide_sbs_swapchain_)
	{
		video_swapchain_rendered_ |= drawn;
		right_layer_ready_ = drawn;
	}
	else if(right_acquired)
	{
		XrSwapchainImageReleaseInfo release_right{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		xrReleaseSwapchainImage(video_swapchain_right_, &release_right);
		video_swapchain_rendered_ |= drawn;
		right_layer_ready_ = drawn;
	}
	else if(drawn && !want_stereo)
	{
		video_swapchain_rendered_ = true;
	}
	return drawn;
}

jobject XrVideoSession::CreateVideoSurface(JNIEnv *env, int32_t width, int32_t height)
{
	if(session_ == XR_NULL_HANDLE)
	{
		LOGE("CreateVideoSurface called out of order");
		return nullptr;
	}

	// No caminho com shader quem produz o Surface e o Java, a partir de uma
	// SurfaceTexture: aqui so o swapchain de destino precisa existir.
	if(render_path_ == RenderPath::ToneMapped)
	{
		// O swapchain nao pode nascer aqui. Esta chamada vem da thread do Java,
		// e a essa altura o Create() ja soltou o contexto EGL para entrega-lo ao
		// frame loop -- o runtime criaria as texturas sem contexto nenhum e
		// devolveria zeros. Ficam so as medidas; quem cria e o frame loop, no
		// primeiro quadro, com o contexto na mao.
		std::lock_guard<std::mutex> lock(params_mutex_);
		pending_gl_width_ = width;
		pending_gl_height_ = height;
		params_.aspectRatio = (float)width / (float)height;
		return nullptr;
	}

	if(video_swapchain_ != XR_NULL_HANDLE || pending_gl_width_ > 0)
	{
		LOGE("CreateVideoSurface called out of order");
		return nullptr;
	}

	// Ver o comentario na selecao de extensoes: sem este elo, o swapchain nasce
	// pela metade e o runtime salta para ponteiro nulo dentro do xrEndFrame na
	// primeira submissao que o referencia.
	//
	// createFlags fica em zero de proposito. SYNCHRONOUS faria o compositor
	// enfileirar buffers em vez de sempre pegar o mais recente, e USE_TIMESTAMPS
	// o faria esperar o buffer com timestamp certo: os dois trocam latencia por
	// sincronia de A/V, que e o oposto do que este projeto quer.
	XrAndroidSurfaceSwapchainCreateInfoFB surface_info{
			XR_TYPE_ANDROID_SURFACE_SWAPCHAIN_CREATE_INFO_FB};
	surface_info.createFlags = 0;

	XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	if(surface_swapchain_create_supported_)
		info.next = &surface_info;
	// A especificacao de XR_KHR_android_surface_swapchain e explicita:
	//
	//   "The format, sampleCount, faceCount, arraySize and mipCount members of
	//    the structure passed as the info parameter must be zero."
	//
	// Nao sao ignorados, sao *obrigatoriamente zero*. Passar 1 -- o valor que
	// faria sentido num swapchain de textura comum -- devolve
	// XR_ERROR_VALIDATION_FAILURE. Aqui quem produz as imagens e o MediaCodec
	// pelo lado Surface, entao o runtime nao tem formato nem niveis para alocar.
	info.createFlags = 0;
	info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = 0;
	info.sampleCount = 0;
	info.width = (uint32_t)width;
	info.height = (uint32_t)height;
	info.faceCount = 0;
	info.arraySize = 0;
	info.mipCount = 0;

	jobject surface = nullptr;
	XrResult res = pfnCreateSwapchainAndroidSurfaceKHR(session_, &info, &video_swapchain_, &surface);
	if(XR_FAILED(res) || surface == nullptr)
	{
		last_error_ = std::string("xrCreateSwapchainAndroidSurfaceKHR: ")
				+ XrResultStr(instance_, res)
				+ (surface == nullptr ? " (surface nula)" : "");
		LOGE("%s", last_error_.c_str());
		video_swapchain_ = XR_NULL_HANDLE;
		return nullptr;
	}

	video_width_ = width;
	video_height_ = height;
	eye_width_ = width;
	eye_height_ = height;
	wide_sbs_swapchain_ = false;
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		params_.aspectRatio = (float)width / (float)height;
	}

	LOGI("Video swapchain created: %dx%d (create_info_fb=%d)", width, height,
			(int)surface_swapchain_create_supported_);
	// Devolvemos referencia global: o Surface vai viver no lado Kotlin.
	return env->NewGlobalRef(surface);
}

void XrVideoSession::SetScreenParams(const ScreenParams &params)
{
	std::lock_guard<std::mutex> lock(params_mutex_);
	float aspect = params_.aspectRatio;
	params_ = params;
	if(params.yawOffset >= 1000.0f && params.yawOffset < 2000.0f)
	{
		int code = (int)(params.yawOffset + 0.5f);
		int rem = code - 1000;
		int spanIdx = rem / 100;
		int vOut = (rem % 100) / 10;
		int distIdx = rem % 10;

		float sx = 1.0f;
		if(spanIdx == 1) sx = 1.55f;
		else if(spanIdx == 2) sx = 2.25f;
		else if(spanIdx == 3) sx = ComputeTrackSpanX(aspect);

		float sy = (vOut == 1) ? 1.6f : 1.0f;

		float d = 0.99f;
		if(distIdx == 1) d = 0.88f;
		else if(distIdx == 2) d = 0.76f;
		else if(distIdx == 3) d = 0.64f;

		g_danmaku_is_stereo.store(spanIdx == 3);
		g_danmaku_span_x.store(sx);
		g_danmaku_span_y.store(sy);
		g_danmaku_depth_factor.store(d);
		params_.yawOffset = 0.0f;
	}
	else if(params.yawOffset > 0.1f && params.yawOffset < 10.0f)
	{
		int mode = (int)std::floor(params.yawOffset + 0.005f);
		float depth = params.yawOffset - (float)mode;
		if(depth < 0.1f || depth > 1.0f) depth = 0.99f;
		float sx = 1.0f;
		if(mode == 1) sx = 1.55f;
		else if(mode == 2) sx = 2.25f;
		g_danmaku_is_stereo.store(false);
		g_danmaku_span_x.store(sx);
		g_danmaku_span_y.store(1.0f);
		g_danmaku_depth_factor.store(std::clamp(depth, 0.45f, 0.995f));
		params_.yawOffset = 0.0f;
	}
	g_screen_pitch_offset.store(params.heightOffset);
	params_.heightOffset = 0.0f;
	// O aspecto vem do video, nao da UI.
	if(aspect > 0.01f)
		params_.aspectRatio = aspect;
}

void XrVideoSession::SetQualityParams(const QualityParams &params)
{
	{
		std::lock_guard<std::mutex> lock(params_mutex_);
		quality_ = params;
	}
	if(passthrough_layer_ != XR_NULL_HANDLE && pfnPassthroughLayerSetStyleFB)
	{
		XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
		style.textureOpacityFactor = params.passthroughOpacity;
		style.edgeColor = XrColor4f{0.0f, 0.0f, 0.0f, 0.0f};

		// So encadeia quando ha o que mudar. Nos valores neutros o elo nao
		// acrescenta nada, e um runtime que nao conheca a estrutura recusaria o
		// estilo inteiro -- perder a opacidade por causa de um pedido que nao
		// pedia nada seria troca ruim.
		XrPassthroughBrightnessContrastSaturationFB grade{
				XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB};
		const bool graded = params.passthroughBrightness != 0.0f
				|| params.passthroughContrast != 1.0f
				|| params.passthroughSaturation != 1.0f;
		if(graded)
		{
			grade.brightness = params.passthroughBrightness;
			grade.contrast = params.passthroughContrast;
			grade.saturation = params.passthroughSaturation;
			style.next = &grade;
		}

		XrResult res = pfnPassthroughLayerSetStyleFB(passthrough_layer_, &style);
		if(XR_FAILED(res))
		{
			LOGW("xrPassthroughLayerSetStyleFB%s: %s", graded ? " (with cinema mode)" : "",
					XrResultStr(instance_, res));
			// Se o runtime recusou por causa do elo novo, o estilo simples ainda
			// vale: melhor perder o modo cinema do que a opacidade junto.
			if(graded)
			{
				style.next = nullptr;
				pfnPassthroughLayerSetStyleFB(passthrough_layer_, &style);
			}
		}
		else if(graded && !logged_cinema_)
		{
			logged_cinema_ = true;
			LOGI("Cinema mode accepted: brightness %.2f contrast %.2f saturation %.2f",
					(double)grade.brightness, (double)grade.contrast,
					(double)grade.saturation);
		}
	}
}

float XrVideoSession::SelectDisplayRefreshRate(int sourceFps)
{
	// Pedido cedo demais nao vale nada: enquanto a sessao nao passa pelo
	// xrBeginSession o runtime da Meta responde XR_SUCCESS com lista vazia, e
	// era exatamente isso que o log mostrava -- "devolveu 0 taxas: XR_SUCCESS".
	// Entao o pedido fica guardado e o frame loop o cumpre assim que a sessao
	// comeca de fato.
	if(!session_running_)
	{
		pending_refresh_fps_.store(sourceFps);
		LOGI("Panel rate deferred until the session starts (%d fps source)", sourceFps);
		return 0.0f;
	}

	// Cada saida antecipada com seu motivo. Antes as tres devolviam zero em
	// silencio, e o log ficava sem nenhuma linha sobre taxa -- indistinguivel de
	// a funcao nem ter sido chamada. O painel podia estar nos 72 Hz de fabrica
	// desde sempre, sem nada denunciar.
	if(!refresh_rate_supported_)
	{
		LOGW("XR_FB_display_refresh_rate unavailable; the panel stays at the system rate");
		return 0.0f;
	}
	if(session_ == XR_NULL_HANDLE)
	{
		LOGW("Panel rate requested with no session");
		return 0.0f;
	}

	uint32_t count = 0;
	XrResult res = pfnEnumerateDisplayRefreshRatesFB(session_, 0, &count, nullptr);
	if(XR_FAILED(res) || count == 0)
	{
		LOGW("xrEnumerateDisplayRefreshRatesFB returned %u rates: %s", count,
				XrResultStr(instance_, res));

		// Lista vazia com sucesso declarado, mesmo depois do xrBeginSession:
		// nao e questao de chamar cedo demais, ja tentamos. Sobra pedir sem
		// consultar. A especificacao nao exige que a taxa pedida tenha vindo da
		// enumeracao -- ela recusa com XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB
		// se nao servir --, e uma lista vazia pode ser apenas o runtime nao
		// publicando as taxas estendidas. A recusa e barata e o log diz qual foi.
		const float kBlind[] = { 120.0f, 90.0f };
		for(float rate : kBlind)
		{
			res = pfnRequestDisplayRefreshRateFB(session_, rate);
			if(XR_SUCCEEDED(res))
			{
				SetCurrentDisplayRefreshHz(rate);
				LOGI("Panel at %.1f Hz, asked for blind (the list came back empty)", rate);
				return rate;
			}
			LOGW("Blind request for %.1f Hz refused: %s", rate,
					XrResultStr(instance_, res));
		}
		return 0.0f;
	}
	std::vector<float> rates(count, 0.0f);
	res = pfnEnumerateDisplayRefreshRatesFB(session_, count, &count, rates.data());
	if(XR_FAILED(res))
	{
		LOGW("xrEnumerateDisplayRefreshRatesFB(lista): %s", XrResultStr(instance_, res));
		return 0.0f;
	}

	// A lista inteira no log: com as taxas estendidas do Horizon OS v2.7 o Quest 3
	// aceita qualquer inteiro de 72 a 207 Hz, e saber o que o runtime realmente
	// oferece e o que separa "escolhemos 120" de "so existia 72".
	{
		std::string list;
		for(float r : rates)
		{
			char buf[16];
			snprintf(buf, sizeof(buf), "%.0f ", r);
			list += buf;
		}
		LOGI("Rates the panel offers: %s", list.c_str());
	}

	// Preferimos o maior multiplo inteiro do framerate da fonte: assim cada
	// frame do console ocupa um numero inteiro de frames do painel e a cadencia
	// fica uniforme. So se nenhum multiplo existir e que caimos na maior taxa.
	float best = 0.0f;
	for(float rate : rates)
	{
		float ratio = rate / (float)sourceFps;
		bool integral = std::fabs(ratio - std::round(ratio)) < 0.01f && ratio >= 1.0f;
		if(integral && rate > best)
			best = rate;
	}
	if(best <= 0.0f)
	{
		for(float rate : rates)
			if(rate > best)
				best = rate;
		LOGW("No rate that is a multiple of %d Hz; using %.1f Hz and accepting judder",
				sourceFps, best);
	}

	res = pfnRequestDisplayRefreshRateFB(session_, best);
	if(XR_FAILED(res))
	{
		LOGW("xrRequestDisplayRefreshRateFB(%.1f) failed: %s", best, XrResultStr(instance_, res));
		return 0.0f;
	}
	SetCurrentDisplayRefreshHz(best);
	ResetDisplayPacing();
	LOGI("Panel at %.1f Hz for a %d fps source (%.0f panel frames per source frame)",
			best, sourceFps, best / (float)sourceFps);
	return best;
}

void XrVideoSession::ApplyPerformanceLevels()
{
	if(!perf_settings_supported_ || !pfnPerfSettingsSetPerformanceLevelEXT)
	{
		LOGW("XR_EXT_performance_settings unavailable; clocks are left to the system");
		return;
	}

	// SUSTAINED_HIGH nos dois dominios, e nao BOOST.
	//
	// BOOST existe para rajadas de poucos segundos -- carregar uma cena, passar
	// por uma transicao -- e a propria especificacao avisa que pode nao ser
	// sustentavel. Uma sessao de jogo dura horas: pedir BOOST aqui compraria
	// alguns minutos rapidos e depois o termico cobraria a conta de volta, com
	// juros, no meio da partida. SUSTAINED_HIGH e o teto que o aparelho aguenta
	// segurar, e consistencia vale mais que pico num stream de 60 fps.
	struct { XrPerfSettingsDomainEXT domain; const char *name; } domains[] = {
		{ XR_PERF_SETTINGS_DOMAIN_CPU_EXT, "CPU" },
		{ XR_PERF_SETTINGS_DOMAIN_GPU_EXT, "GPU" },
	};
	for(const auto &d : domains)
	{
		XrResult res = pfnPerfSettingsSetPerformanceLevelEXT(session_, d.domain,
				XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
		if(XR_FAILED(res))
			LOGW("Performance level for %s refused: %s", d.name, XrResultStr(instance_, res));
		else
			LOGI("%s at sustained high level", d.name);
	}
}

void XrVideoSession::InitPerformanceMetrics()
{
	if(!perf_metrics_supported_)
	{
		LOGW("XR_META_performance_metrics unavailable; no numbers from the compositor");
		return;
	}

	XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
	state.enabled = XR_TRUE;
	XrResult res = pfnSetPerformanceMetricsStateMETA(session_, &state);
	if(XR_FAILED(res))
	{
		LOGW("xrSetPerformanceMetricsStateMETA: %s", XrResultStr(instance_, res));
		perf_metrics_supported_ = false;
		return;
	}

	// Os caminhos vem enumerados, e nao escritos a mao.
	//
	// A alternativa seria chutar as strings da documentacao da Meta, e chute
	// que erra devolve XR_ERROR_PATH_UNSUPPORTED -- indistinguivel, no log, de
	// "este aparelho nao tem esse contador". Enumerar e casar por trecho diz as
	// duas coisas e sobrevive a mudanca de nome numa versao futura.
	uint32_t path_count = 0;
	if(XR_FAILED(pfnEnumeratePerformanceMetricsCounterPathsMETA(instance_, 0, &path_count, nullptr))
			|| path_count == 0)
	{
		LOGW("No performance counter enumerated");
		perf_metrics_supported_ = false;
		return;
	}
	std::vector<XrPath> paths(path_count, XR_NULL_PATH);
	if(XR_FAILED(pfnEnumeratePerformanceMetricsCounterPathsMETA(instance_, path_count,
			&path_count, paths.data())))
	{
		perf_metrics_supported_ = false;
		return;
	}

	LOGI("Performance counters offered: %u", path_count);
	for(XrPath path : paths)
	{
		uint32_t len = 0;
		if(XR_FAILED(xrPathToString(instance_, path, 0, &len, nullptr)) || len == 0)
			continue;
		std::string name(len, '\0');
		if(XR_FAILED(xrPathToString(instance_, path, len, &len, &name[0])))
			continue;
		name.resize(len > 0 ? len - 1 : 0);
		LOGI("  counter: %s", name.c_str());

		// Casado por trecho do caminho real, e nao pelo nome que eu supunha.
		//
		// A primeira versao procurava "gpu_time" e o caminho e "gpu_frametime":
		// os dois contadores de tempo de GPU nunca casavam, e o painel
		// simplesmente nao mostrava a linha -- que, pela regra de so mostrar o
		// que existe, era indistinguivel de "este aparelho nao tem". Foi a
		// lista enumerada que denunciou.
		if(name.find("dropped_frame") != std::string::npos)
			counter_dropped_frames_ = path;
		else if(name.find("gpu_utilization") != std::string::npos)
			counter_gpu_utilization_ = path;
		else if(name.find("motion_to_photon") != std::string::npos)
			counter_motion_to_photon_ = path;
		else if(name.find("cpu_utilization_average") != std::string::npos)
			counter_cpu_utilization_ = path;
		else if(name.find("app") != std::string::npos
				&& name.find("gpu_frametime") != std::string::npos)
			counter_app_gpu_time_ = path;
		else if(name.find("compositor") != std::string::npos
				&& name.find("gpu_frametime") != std::string::npos)
			counter_comp_gpu_time_ = path;
	}
}

bool XrVideoSession::ReadCounter(XrPath path, float *out) const
{
	if(path == XR_NULL_PATH || !pfnQueryPerformanceMetricsCounterMETA)
		return false;
	XrPerformanceMetricsCounterMETA counter{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
	if(XR_FAILED(pfnQueryPerformanceMetricsCounterMETA(session_, path, &counter)))
		return false;
	if(counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)
		*out = counter.floatValue;
	else if(counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META)
		*out = (float)counter.uintValue;
	else
		return false;
	return true;
}

PerformanceSnapshot XrVideoSession::ReadPerformance() const
{
	PerformanceSnapshot snap;
	snap.droppedFrames = ReadCounter(counter_dropped_frames_, &snap.droppedFrameCount) ? 1 : 0;
	snap.hasGpuUtilization = ReadCounter(counter_gpu_utilization_, &snap.gpuUtilization);
	snap.hasAppGpuTime = ReadCounter(counter_app_gpu_time_, &snap.appGpuTimeMs);
	snap.hasCompositorGpuTime = ReadCounter(counter_comp_gpu_time_, &snap.compositorGpuTimeMs);
	snap.hasMotionToPhoton = ReadCounter(counter_motion_to_photon_, &snap.motionToPhotonMs);
	snap.hasCpuUtilization = ReadCounter(counter_cpu_utilization_, &snap.cpuUtilization);

	if(pfnThermalGetTemperatureTrendEXT)
	{
		XrPerfSettingsNotificationLevelEXT level = XR_PERF_SETTINGS_NOTIF_LEVEL_NORMAL_EXT;
		float headroom = 0.0f, slope = 0.0f;
		if(XR_SUCCEEDED(pfnThermalGetTemperatureTrendEXT(session_,
				XR_PERF_SETTINGS_DOMAIN_GPU_EXT, &level, &headroom, &slope)))
		{
			snap.hasThermal = true;
			snap.thermalHeadroom = headroom;
			snap.thermalSlope = slope;
		}
	}
	return snap;
}

void XrVideoSession::SetPassthroughEnabled(bool enabled)
{
	if(!passthrough_supported_)
		return;
	const bool prev = passthrough_enabled_.exchange(enabled);
	if(enabled && pfnPassthroughLayerResumeFB)
		pfnPassthroughLayerResumeFB(passthrough_layer_);
	else if(!enabled && pfnPassthroughLayerPauseFB)
		pfnPassthroughLayerPauseFB(passthrough_layer_);
	if(prev != enabled)
	{
		ResetDisplayPacing();
		reapply_perf_requested_.store(true);
	}
}

void XrVideoSession::Recenter()
{
	recenter_requested_ = true;
}

void XrVideoSession::StartFrameLoop()
{
	if(running_.exchange(true))
		return;
	{
		std::lock_guard<std::mutex> lk(g_xr_frame_mtx);
		g_xr_frame_active.store(true, std::memory_order_relaxed);
	}
	// Quem chama isto e a activity, na thread principal do app -- e este e o
	// unico ponto em que temos o tid dela. O frame loop precisa dele para
	// declara-la ao runtime, e la dentro `gettid()` ja devolveria outra coisa.
	main_thread_tid_.store((uint32_t)gettid());
	frame_thread_ = std::thread(&XrVideoSession::FrameLoop, this);
}

void XrVideoSession::StopFrameLoop()
{
	if(!running_.exchange(false))
		return;
	{
		std::lock_guard<std::mutex> lk(g_xr_frame_mtx);
		g_xr_frame_active.store(false, std::memory_order_relaxed);
	}
	g_xr_frame_cv.notify_all();
	if(frame_thread_.joinable())
		frame_thread_.join();

	// Terminar a sessao aqui, e nao por sorte de agendamento.
	//
	// O xrEndSession so acontece quando o STOPPING e lido, e quem lia eventos
	// era a thread que acabou de morrer. Ate aqui funcionava por um fio: o
	// runtime enfileirava o STOPPING no mesmo instante do onPause e a ultima
	// passada do loop calhava de pega-lo. Quando nao calhasse, a sessao seria
	// destruida ainda rodando, sem um xrEndSession sequer, e sem uma linha no
	// diario dizendo por que.
	//
	// A espera e curta de proposito: isto roda na thread principal, dentro do
	// onPause. Duzentos milissegundos e mais do que o runtime leva e menos do
	// que o Android reclama.
	if(session_running_ && !exit_requested_)
	{
		const auto limite = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
		while(session_running_ && !exit_requested_
				&& std::chrono::steady_clock::now() < limite)
		{
			PollEvents();
			if(!session_running_)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		if(session_running_)
			LOGW("The OpenXR session was still running 200 ms after the loop stopped: "
					"STOPPING never arrived, and xrEndSession never happened");
	}
}

void XrVideoSession::FrameLoop()
{
	// O contexto EGL foi criado na thread de origem, que o soltou ao fim do
	// Create justamente para esta assumi-lo. Se isto falhar, a thread fica sem
	// contexto e o runtime morre dentro do xrEndFrame com pc=0, sem dizer por
	// que -- entao a falha e reportada aqui, onde ainda da para entender.
	if(!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
	{
		EGLint err = eglGetError();
		LOGE("eglMakeCurrent on the frame loop thread failed: 0x%x", err);
		last_error_ = "eglMakeCurrent on the render thread failed (0x"
				+ std::to_string(err) + ")";
		running_ = false;
		return;
	}
	LOGI("EGL context current on the frame loop thread (%s)",
			eglGetCurrentContext() == egl_context_ ? "confirmado" : "OUTRO CONTEXTO");

	// Esta e a thread que submete os frames. Declarada como RENDERER_MAIN, o
	// sistema a mantem nos nucleos grandes e fora da fila comum -- sem isso ela
	// concorre com qualquer trabalho de fundo justamente no instante em que o
	// compositor espera o frame, e o efeito e engasgo esporadico, o pior tipo
	// de defeito de latencia porque nao aparece em media nenhuma.
	if(thread_settings_supported_)
	{
		const uint32_t tid = (uint32_t)gettid();
		XrResult res = pfnSetAndroidApplicationThreadKHR(session_,
				XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, tid);
		if(XR_FAILED(res))
			LOGW("xrSetAndroidApplicationThreadKHR(RENDERER_MAIN, %u): %s", tid,
					XrResultStr(instance_, res));
		else
			LOGI("Frame loop thread declared as RENDERER_MAIN (tid %u)", tid);

		// A thread principal do app tambem, e por um motivo diferente.
		//
		// Ela nao desenha, mas e ela que recebe entrada, roda o ciclo de vida da
		// activity e entrega os quadros do decodificador. Se o sistema a puser
		// num nucleo pequeno, o atraso nao aparece no frame loop -- aparece
		// antes dele, como quadro que chegou tarde, e procurar isso no lugar
		// errado ja custou tempo aqui. Declarar as duas e o que a documentacao
		// do Horizon pede; declarar so uma e meio caminho.
		const uint32_t main_tid = main_thread_tid_.load();
		if(main_tid != 0)
		{
			XrResult mres = pfnSetAndroidApplicationThreadKHR(session_,
					XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, main_tid);
			if(XR_FAILED(mres))
				LOGW("xrSetAndroidApplicationThreadKHR(APPLICATION_MAIN, %u): %s", main_tid,
						XrResultStr(instance_, mres));
			else
				LOGI("App main thread declared as APPLICATION_MAIN (tid %u)", main_tid);
		}
	}

	while(running_ && !exit_requested_)
	{
		PollEvents();
		if(!session_running_)
		{
			// Sessao ociosa (headset na cabeca ainda nao, ou app em background):
			// nao ocupamos CPU girando.
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		if(reapply_perf_requested_.exchange(false))
			ApplyPerformanceLevels();
		RenderFrame();
	}

	if(danmaku_clear_fbo_ != 0)
	{
		if(eglGetCurrentContext() != EGL_NO_CONTEXT)
			glDeleteFramebuffers(1, &danmaku_clear_fbo_);
		danmaku_clear_fbo_ = 0;
	}
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void XrVideoSession::PollEvents()
{
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while(true)
	{
		event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
		XrResult res = xrPollEvent(instance_, &event);
		if(res != XR_SUCCESS)
			break;

		switch(event.type)
		{
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			{
				const auto *changed = reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
				HandleSessionStateChange(changed->state);
				break;
			}
			case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT:
			{
				const auto *presence =
						reinterpret_cast<const XrEventDataUserPresenceChangedEXT *>(&event);
				LOGI("Headset %s", presence->isUserPresent ? "on the head"
						: "off the head (the stream carries on)");
				break;
			}
			case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT:
			{
				// O runtime avisa antes de cortar relogio, e este e o unico
				// aviso que existe: quando o corte chega, ele aparece como
				// engasgo e nao como mensagem.
				const auto *perf =
						reinterpret_cast<const XrEventDataPerfSettingsEXT *>(&event);
				LOGW("Performance notice: domain %d, subdomain %d, level %d -> %d",
						(int)perf->domain, (int)perf->subDomain,
						(int)perf->fromLevel, (int)perf->toLevel);
				break;
			}
			case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
			{
				const auto *rateEv =
						reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB *>(&event);
				SetCurrentDisplayRefreshHz(rateEv->toDisplayRefreshRate);
				LOGI("Display refresh rate changed: %.1f -> %.1f Hz",
						(double)rateEv->fromDisplayRefreshRate,
						(double)rateEv->toDisplayRefreshRate);
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				LOGW("The OpenXR instance will be lost; ending the loop");
				exit_requested_ = true;
				break;
			default:
				break;
		}
	}
}

// Nome legivel do estado, so para o diario.
//
// Sem isto o runtime podia mandar a sessao para EXITING e o loop de quadro
// parava em silencio: o diario nao registrava uma linha sequer, e do lado de
// dentro do headset nao ha como olhar de fora.
static const char *XrSessionStateStr(XrSessionState state)
{
	switch(state)
	{
		case XR_SESSION_STATE_IDLE: return "IDLE";
		case XR_SESSION_STATE_READY: return "READY";
		case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
		case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
		case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
		case XR_SESSION_STATE_STOPPING: return "STOPPING";
		case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
		case XR_SESSION_STATE_EXITING: return "EXITING";
		default: return "?";
	}
}

void XrVideoSession::HandleSessionStateChange(XrSessionState state)
{
	LOGI("OpenXR session state: %s", XrSessionStateStr(state));
	session_state_ = state;
	switch(state)
	{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
			begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if(XR_SUCCEEDED(xrBeginSession(session_, &begin)))
			{
				session_running_ = true;
				int fps = pending_refresh_fps_.exchange(0);
				if(fps > 0)
					SelectDisplayRefreshRate(fps);
				ApplyPerformanceLevels();
				InitPerformanceMetrics();
			}
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			session_running_ = false;
			xrEndSession(session_);
			break;
		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			LOGW("The runtime asked to end the session (%s); the frame loop stops here",
					XrSessionStateStr(state));
			session_running_ = false;
			exit_requested_ = true;
			break;
		default:
			break;
	}
}

void XrVideoSession::RenderFrame()
{
	XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	if(XR_FAILED(xrWaitFrame(session_, &wait_info, &frame_state)))
		return;
	latest_predicted_display_time_.store(frame_state.predictedDisplayTime);
	const bool activePacing = session_running_.load()
			&& session_state_ >= XR_SESSION_STATE_VISIBLE
			&& video_layer_enabled_.load()
			&& frame_state.shouldRender;
	if(activePacing)
	{
		if(frame_state.predictedDisplayPeriod > 1000000LL
				&& frame_state.predictedDisplayPeriod < 50000000LL)
		{
			SetCurrentDisplayRefreshHz(1000000000.0f / (float)frame_state.predictedDisplayPeriod);
		}
		if(prev_predicted_display_time_ns_ > 0
				&& frame_state.predictedDisplayTime > prev_predicted_display_time_ns_
				&& frame_state.predictedDisplayPeriod > 0)
		{
			const int64_t deltaNs = frame_state.predictedDisplayTime - prev_predicted_display_time_ns_;
			const bool missed = deltaNs > (frame_state.predictedDisplayPeriod * 27LL) / 20LL;
			RecordDisplayPacingSample(missed);
		}
		prev_predicted_display_time_ns_ = frame_state.predictedDisplayTime;
	}
	else
	{
		prev_predicted_display_time_ns_ = 0;
	}

	XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
	if(XR_FAILED(xrBeginFrame(session_, &begin_info)))
		return;

	// Wake the danmaku worker at the START of the frame. It draws the bitmap for
	// the NEXT frame's display time (predictedDisplayTime + period) and gets
	// roughly a full period plus this frame's render time to finish, because the
	// frame-tagged bitmap is only consumed by the next frame's UpdateDanmakuLayer.
	if(frame_state.shouldRender)
	{
		const int64_t periodNs = (frame_state.predictedDisplayPeriod > 1000000LL
				&& frame_state.predictedDisplayPeriod < 50000000LL)
				? frame_state.predictedDisplayPeriod : 11111111LL;
		{
			std::lock_guard<std::mutex> lk(g_xr_frame_mtx);
			g_xr_target_display_ns.store(frame_state.predictedDisplayTime + periodNs,
					std::memory_order_relaxed);
			g_xr_frame_seq.fetch_add(1, std::memory_order_release);
		}
		g_xr_frame_cv.notify_all();
	}

	// Locate views and run OpenXR hand tracking BEFORE snapshotting screen pose so
	// every composition layer reflects this frame's exact predicted hand motion (0-frame lag).
	if(view_count_ > 0 && frame_state.shouldRender)
	{
		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime = frame_state.predictedDisplayTime;
		locate.space = app_space_;
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		uint32_t located = 0;
		xrLocateViews(session_, &locate, &view_state, view_count_, &located, views_.data());

		if(located >= 2 && !logged_ipd_ &&
				(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
		{
			const XrVector3f &a = views_[0].pose.position;
			const XrVector3f &b = views_[1].pose.position;
			float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
			float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
			if(ipd > 0.03f && ipd < 0.09f)
			{
				logged_ipd_ = true;
				ipd_meters_.store(ipd);
				LOGI("Interpupillary distance in use: %.1f mm (read from the runtime, "
						"not asked for)", (double)(ipd * 1000.0f));
			}
		}
	}

	if(hand_tracking_supported_ && frame_state.shouldRender)
	{
		UpdateHandTracking(frame_state.predictedDisplayTime);
	}

	// ONE snapshot for every composition layer in this frame.
	const float frame_screen_yaw = g_screen_yaw_offset.load();
	const float frame_screen_pitch = g_screen_pitch_offset.load();

	// Immersive launch splash. Java arms it AND uploads the first spinner
	// bitmap before the frame loop starts, so the very first renderable XR
	// frame shows a centered card instead of a black void. The video layer is
	// held back until this splash is dismissed; both transitions happen in
	// this SAME frame, so the card can never overlay playing video and the
	// video can never appear before the card has been shown.
	if(loading_requested_.load() && !loading_dismissed_.load())
	{
		const bool renderable_frame = session_running_.load()
				&& session_state_ >= XR_SESSION_STATE_VISIBLE
				&& frame_state.shouldRender;
		if(renderable_frame)
		{
			const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
			int64_t first_ns = loading_first_focused_ns_.load();
			if(first_ns == 0)
			{
				first_ns = now_ns;
				loading_first_focused_ns_.store(first_ns);
			}
			const int frames = loading_focused_frames_.fetch_add(1) + 1;
			// A few composited frames guarantee the card really reached the
			// compositor; a short floor avoids a one-frame flash on files that
			// decode instantly. There is deliberately NO long fixed delay --
			// the splash must disappear the moment the picture is ready.
			const bool composited = frames >= 3;
			const bool minimum_visible = now_ns - first_ns >= 250000000LL;
			if(loading_ready_.load() && composited && minimum_visible)
			{
				loading_visible_.store(false);
				// Hide the shared HUD layer for the few frames until Java
				// replaces the spinner bitmap with the normal HUD. Otherwise
				// the card would briefly reappear at the HUD's lower position.
				hud_visible_.store(false);
				loading_dismissed_.store(true);
				video_layer_enabled_.store(true);
				LOGI("VR splash dismissed after %d composited frames / %.0f ms; video layer enabled",
						frames, (double)(now_ns - first_ns) / 1000000.0);
			}
			else
			{
				loading_visible_.store(true);
			}
		}
		else
		{
			loading_visible_.store(true);
		}
	}

	// The transient 2D startup page can be closed once the runtime lets us
	// submit an actual immersive frame. Java polls this once per input frame.
	if(!session_renderable_.load() && session_running_.load()
			&& session_state_ >= XR_SESSION_STATE_VISIBLE
			&& frame_state.shouldRender)
	{
		session_renderable_.store(true);
		LOGI("VR session submitted its first renderable frame");
	}

	// Onde a tela esta em relacao a cabeca, para os alto-falantes virtuais.
	// Publicado a cada quadro; quem le e a thread de audio, por atomicos.
	if(view_count_ > 0 && !views_.empty())
	{
		ScreenParams params;
		XrQuaternionf recenter;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			recenter = recenter_orientation_;
		}
		const float screen_yaw = YawFromQuat(recenter) + params.yawOffset;
		const float screen_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		const float cos_sp = std::cos(screen_pitch);
		const XrVector3f center{
			-std::sin(screen_yaw) * cos_sp * params.radius,
			params.heightOffset + std::sin(screen_pitch) * params.radius,
			-std::cos(screen_yaw) * cos_sp * params.radius};

		// A cabeca fica entre os dois olhos; usar so o esquerdo deslocaria o
		// palco alguns centimetros para o lado, o suficiente para a imagem nao
		// ficar centrada quando ela deveria.
		const XrPosef &eye = views_[0].pose;
		XrVector3f head = eye.position;
		if(views_.size() > 1)
		{
			head.x = (head.x + views_[1].pose.position.x) * 0.5f;
			head.y = (head.y + views_[1].pose.position.y) * 0.5f;
			head.z = (head.z + views_[1].pose.position.z) * 0.5f;
		}

		const XrVector3f to_screen{center.x - head.x, center.y - head.y, center.z - head.z};
		const XrVector3f local = RotateByInverse(eye.orientation, to_screen);
		// x para a direita, -z para a frente: e a convencao do OpenXR, e o
		// azimute sai direto dela.
		Spatializer::Instance().SetGeometry(std::atan2(local.x, -local.z),
				params.centralAngle * 0.5f);
	}

	if(recenter_requested_.load())
	{
		XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
		if(XR_SUCCEEDED(xrLocateSpace(view_space_, app_space_, frame_state.predictedDisplayTime, &loc)))
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			if(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			{
				float yaw = YawFromQuat(loc.pose.orientation);
				float pitch = PitchFromQuat(loc.pose.orientation);
				recenter_orientation_ = QuatFromYawPitch(yaw, pitch);
				g_screen_pitch_offset.store(0.0f);
				g_screen_yaw_offset.store(0.0f);
				recenter_requested_.store(false);
			}
			if(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			{
				recenter_position_ = loc.pose.position;
			}
		}
	}

	std::vector<const XrCompositionLayerBaseHeader *> layers;

	XrCompositionLayerPassthroughFB passthrough_layer_info{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
	if(passthrough_supported_ && passthrough_enabled_)
	{
		passthrough_layer_info.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		passthrough_layer_info.space = XR_NULL_HANDLE;
		passthrough_layer_info.layerHandle = passthrough_layer_;
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&passthrough_layer_info));
	}

	XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	// Copias para o olho direito, quando a imagem carrega os dois. Vivem aqui,
	// e nao dentro do bloco, porque o xrEndFrame le as estruturas depois: uma
	// copia local dentro de um escopo que fecha viraria ponteiro morto na hora
	// exata em que o runtime for lê-la.
	XrCompositionLayerCylinderKHR cylinder_right{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
	XrCompositionLayerQuad quad_right{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerEquirect2KHR equirect{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
	XrCompositionLayerEquirect2KHR equirect_right{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
	XrCompositionLayerEquirectKHR equirect1{XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR};
	XrCompositionLayerEquirectKHR equirect1_right{XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR};
	XrCompositionLayerSettingsFB layer_settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
	XrCompositionLayerImageLayoutFB image_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	XrCompositionLayerColorScaleBiasKHR color_scale{
			XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
	// No caminho com shader a imagem precisa ser produzida antes de a camada ser
	// montada; no direto o MediaCodec ja escreveu sozinho.
	bool video_ready = video_layer_enabled_.load() && frame_state.shouldRender;
	if(render_path_ == RenderPath::ToneMapped)
	{
		ScreenParams cparams;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			cparams = params_;
		}
		tone_mapper_.SetCornerParams(projection_mode_.load() == 0 ? 0.032f : 0.0f, cparams.aspectRatio);
		// No caminho com shader o swapchain pode ainda nem existir: e aqui
		// dentro que ele nasce, no primeiro quadro depois da conexao.
		video_ready = video_ready && RenderVideoThroughShader();
	}
	else
		video_ready = video_ready && video_swapchain_ != XR_NULL_HANDLE;

	if(video_ready)
	{
		ScreenParams params;
		QualityParams quality;
		XrQuaternionf recenter;
		XrVector3f recenter_pos;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			quality = quality_;
			recenter = recenter_orientation_;
			recenter_pos = recenter_position_;
		}

		// Os quatro primeiros degraus sao do compositor no caminho direto, e do
		// shader no caminho com shader: pedir os dois somaria dois realces sobre
		// a mesma imagem, e o degrau mais baixo sairia mais forte que o mais
		// alto do outro caminho.
		//
		// MQSR e automatico sao diferentes: nao ha equivalente no shader, e a
		// camada e composta pelo compositor nos dois caminhos. Entao esses dois
		// valem sempre, e la o shader nao afia nada (a tabela de intensidades
		// tem zero nas duas posicoes).
		// O que tira o filtro do compositor de cena e o recorte, e nao o 3D.
		//
		// Quando a camada e metade de uma textura maior, o MQSR e o automatico
		// desenharam um X discreto atravessando a tela -- medido em hardware.
		// O filtro nao foi feito para uma camada que e um recorte de outra.
		//
		// Isso hoje acontece so no estereo empacotado, em que o conteudo ja
		// chega com os dois olhos numa imagem so e nao ha como nao recortar. No
		// estereo que nos sintetizamos cada olho tem swapchain proprio e entra
		// inteiro, entao o filtro vale como vale no mono.
		const bool recorta_a_camada = wide_sbs_swapchain_
				|| (stereo_mode_.load() != 0 && !stereo_synthetic_.load());
		const bool compositor_owns_filter = !recorta_a_camada
				&& (render_path_ == RenderPath::Direct
						|| quality.sharpness == Sharpness::Mqsr
						|| quality.sharpness == Sharpness::Auto);
		if(compositor_owns_filter)
		{
			// O bit de sharpening tem forca fixa. Os degraus intermediarios
			// saem de liga-lo junto com o supersampling, que amacia e devolve
			// parte do realce -- e do supersampling sozinho, que fica um passo
			// abaixo de nao filtrar nada.
			switch(quality.sharpness)
			{
				case Sharpness::Off:
					break;
				case Sharpness::Light:
					layer_settings.layerFlags |=
							XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
					break;
				case Sharpness::Medium:
					layer_settings.layerFlags |=
							XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB
							| XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
					break;
				case Sharpness::Strong:
					layer_settings.layerFlags |=
							XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
					break;
				case Sharpness::Mqsr:
					// Compatibilidade com o indice antigo. O MQSR explicito fez
					// um disco acompanhar a cabeca; so participa como candidato.
				case Sharpness::Auto:
					// O bit automatico exige um conjunto de candidatos junto: a
					// especificacao manda o runtime devolver
					// XR_ERROR_VALIDATION_FAILURE do xrEndFrame se ele vier
					// sozinho. Nao e "escolha o que quiser", e "escolha entre
					// estes".
					if(auto_filter_supported_)
						layer_settings.layerFlags |=
								XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META
								| XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB
								| XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB
								| XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
					else
						layer_settings.layerFlags |=
								XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
					break;
			}
		}

		// Sem XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT: o video e
		// opaco, e com swapchain-Surface quem escolhe o formato e o runtime --
		// pedir mistura por alfa sobre um formato que pode nao ter canal alfa e
		// pedir problema.
		// The intro curtain draws alpha-0 pixels so the passthrough / void
		// behind the screen shows through. Enable source-alpha blending ONLY
		// while it is active; normal playback stays fully opaque (flags=0).
		// In opaque Theater mode (!passthrough) pixels outside rounded corners are pure black,
		// so only enable source-alpha blending on the video layer during intro curtain or passthrough.
		cylinder.layerFlags = (tone_mapper_.CurtainActive()
				|| (passthrough_enabled_.load() && projection_mode_.load() == 0 && render_path_ == RenderPath::ToneMapped))
				? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
		if(layer_settings_supported_ && layer_settings.layerFlags != 0)
		{
			cylinder.next = &layer_settings;
			if(!logged_layer_config_)
				LOGI("Layer with XrCompositionLayerSettingsFB, flags=0x%x",
						(unsigned)layer_settings.layerFlags);
		}

		// Brilho da camada, pelo compositor.
		//
		// Entra antes dos outros elos porque encadear e so encadear: a ordem no
		// next chain nao importa para o runtime. Fica fora quando nao muda nada,
		// pelo mesmo motivo do modo cinema -- elo inutil e superficie a mais
		// para um runtime recusar.
		if(color_scale_supported_ && quality.videoBrightness != 1.0f)
		{
			color_scale.colorScale = XrColor4f{quality.videoBrightness,
					quality.videoBrightness, quality.videoBrightness, 1.0f};
			color_scale.colorBias = XrColor4f{0.0f, 0.0f, 0.0f, 0.0f};
			color_scale.next = const_cast<void *>(cylinder.next);
			cylinder.next = &color_scale;
		}

		// Entra na frente do que ja estiver encadeado: a ordem no next chain
		// nao importa para o runtime, e assim o elo do flip nao depende de o
		// filtro estar ligado.
		// So no caminho direto. Com shader, a matriz da SurfaceTexture ja
		// orientou a imagem, e inverter de novo desfaria o acerto.
		if(render_path_ == RenderPath::Direct && image_layout_supported_
				&& vertical_flip_.load())
		{
			image_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
			image_layout.next = const_cast<void *>(cylinder.next);
			cylinder.next = &image_layout;
		}

		cylinder.space = app_space_;
		cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		cylinder.subImage.swapchain = video_swapchain_;
		cylinder.subImage.imageRect.offset = XrOffset2Di{0, 0};
		const int32_t eyeW = eye_width_ > 0 ? eye_width_ : video_width_;
		const int32_t eyeH = eye_height_ > 0 ? eye_height_ : video_height_;
		int32_t sub_w = sub_image_extent_w_.load();
		int32_t sub_h = sub_image_extent_h_.load();
		if(sub_w <= 0 || sub_w > eyeW) sub_w = eyeW;
		if(sub_h <= 0 || sub_h > eyeH) sub_h = eyeH;
		cylinder.subImage.imageRect.extent = XrExtent2Di{sub_w, sub_h};
		cylinder.subImage.imageArrayIndex = 0;

		// A tela e um arco: a pose marca o eixo do cilindro, e o arco visivel
		// fica centrado em -Z dessa pose. Yaw da recentragem + ajuste do usuario.
		float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
		float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		float cos_pitch = std::cos(total_pitch);
		float sin_pitch = std::sin(total_pitch);
		cylinder.pose.orientation = QuatFromYawPitch(total_yaw, total_pitch);
		// Curvatura separada da distancia.
		//
		// Com a pose na origem, o olho fica no centro do cilindro e a tela sai
		// com a curvatura maxima possivel -- toda a superficie a mesma distancia,
		// abracando quem assiste. Fica exagerado.
		//
		// Para suavizar, o cilindro precisa de um raio maior que a distancia de
		// visao, com o eixo recuado para tras do olho: a superficie continua a
		// `radius` metros, mas descreve um arco de raio R = radius / curvature.
		// Com curvature = 1 o eixo volta a origem e a curva e a maxima; quanto
		// menor, mais plana, e no limite vira a tela chata.
		const float curvature = params.curvature > 0.001f ? params.curvature : 0.1f;
		const float distance = params.radius;
		const float arc_radius = distance / curvature;
		const float axis_back = arc_radius - distance;

		cylinder.pose.position = XrVector3f{
			recenter_pos.x + std::sin(total_yaw) * cos_pitch * axis_back,
			recenter_pos.y + params.heightOffset - sin_pitch * axis_back,
			recenter_pos.z + std::cos(total_yaw) * cos_pitch * axis_back
		};
		cylinder.radius = arc_radius;
		// O arco tem de encolher junto com a curvatura, senao a tela cresceria
		// ao ser aplainada: o que se quer manter e a largura aparente, nao o
		// angulo do cilindro.
		cylinder.centralAngle = std::min(5.5f, 2.0f * curvature * std::tan(std::min(params.centralAngle, 3.05f) * 0.5f));
		cylinder.aspectRatio = params.aspectRatio;

		// Quantos pixels o compositor gostaria de ter nesta camada, do jeito que
		// ela esta agora. Antes do bloco de log logo abaixo, e nao depois: na
		// primeira versao a consulta vinha depois, e a linha de log do primeiro
		// quadro reportava "nao devolveu valor valido" sobre um atomico que
		// ainda estava em zero -- a consulta funcionava, o relato e que mentia.
		//
		// Consultado a cada quadro, e nao uma vez por sessao como antes. O
		// numero muda junto com o tamanho e a distancia da tela, e so serve para
		// escolher o tamanho se puder ser visto *enquanto* se escolhe -- num log
		// lido depois ele conta o que ja passou. Custa uma chamada por quadro e
		// vai para atomicos, que e de onde o painel de ajuste o le.
		if(recommended_resolution_supported_ && pfnGetRecommendedLayerResolutionMETA
				&& !hand_is_manipulating_.load())
		{
			auto now_rec_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
			if(!logged_recommended_resolution_ || (now_rec_ms - last_rec_res_query_ms_) >= 2000)
			{
				last_rec_res_query_ms_ = now_rec_ms;
				XrRecommendedLayerResolutionGetInfoMETA info{
						XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META};
				info.layer = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder);
				info.predictedDisplayTime = frame_state.predictedDisplayTime;
				XrRecommendedLayerResolutionMETA rec{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
				if(XR_SUCCEEDED(pfnGetRecommendedLayerResolutionMETA(session_, &info, &rec))
						&& rec.isValid)
				{
					recommended_width_.store((int)rec.recommendedImageDimensions.width);
					recommended_height_.store((int)rec.recommendedImageDimensions.height);
				}
			}
		}

		// Depois das atribuicoes, nao antes: a versao anterior deste log rodava
		// com a struct ainda zerada e reportava raio, arco e aspecto todos em
		// zero, o que parecia defeito de configuracao e nao era.
		if(!logged_layer_config_)
		{
			logged_layer_config_ = true;
			LOGI("First submission: shape=%s eye=%dx%d swapchain=%dx%d (wide_sbs=%d) radius=%.2f arc=%.2f aspect=%.2f "
					"passthrough=%d chain=%d",
					layer_shape_.load() == LayerShape::Cylinder ? "cilindro" : "quad",
					eyeW, eyeH, video_width_, video_height_, (int)wide_sbs_swapchain_,
					cylinder.radius, cylinder.centralAngle,
					cylinder.aspectRatio, (int)(passthrough_supported_ && passthrough_enabled_),
					(int)(cylinder.next != nullptr));
			if(recommended_resolution_supported_ && !logged_recommended_resolution_)
			{
				logged_recommended_resolution_ = true;
				if(recommended_width_.load() > 0)
					LOGI("Recommended resolution for this screen: %dx%d (the source delivers %dx%d)",
							recommended_width_.load(), recommended_height_.load(),
							eyeW, eyeH);
				else
					LOGW("xrGetRecommendedLayerResolutionMETA returned no valid value");
			}

			// "Disponivel", e nao "ativo".
			//
			// A versao anterior dizia "ativo" sempre que a extensao estivesse
			// habilitada, o que e outra coisa: o bit AUTO_LAYER_FILTER so entra
			// no degrau "automatica" da nitidez. A linha vinha afirmando, em
			// todo log, que um recurso estava em uso quando nao estava.
			LOGI("Automatic layer filter: %s",
					auto_filter_supported_ ? "available (the 'automatic' step)"
					: "unavailable");
			LOGI("Vertical flip: %s", (image_layout_supported_ && vertical_flip_.load())
					? "corrected by the compositor"
					: (image_layout_supported_ ? "off" : "NOT SUPPORTED"));
		}

		// Estereo: duas camadas, uma por olho. A geometria nao muda -- a tela
		// continua do mesmo tamanho e no mesmo lugar --, so muda o que cada olho
		// enxerga.
		//
		// Duas formas de chegar la. No estereo que nos sintetizamos cada olho ja
		// tem um swapchain proprio, e a camada leva a imagem inteira dele. No
		// empacotado ha uma imagem so com os dois olhos dentro, e a camada leva
		// metade dela.
		XrCompositionLayerSettingsFB layer_settings_right{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
		XrCompositionLayerColorScaleBiasKHR color_scale_right{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
		XrCompositionLayerImageLayoutFB image_layout_right{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};

		const int stereo = stereo_mode_.load();
		const int proj = projection_mode_.load();
		const bool is_180 = (proj == 1 || proj == 2 || proj == 3);
		const bool is_sbs = (proj == 2 || proj == 5);
		const bool is_ou = (proj == 3 || proj == 6);
		const bool is_stereo_proj = is_sbs || is_ou;
		const bool has_stereo = (stereo != 0) || is_stereo_proj;
		const bool has_wide = has_stereo && wide_sbs_swapchain_ && right_layer_ready_;
		const bool use_dual_layers = has_wide
				|| (has_stereo && (video_swapchain_right_ != XR_NULL_HANDLE) && right_layer_ready_);
		const XrSwapchain right_sc = has_wide ? video_swapchain_ : video_swapchain_right_;
		const int32_t right_off_x = has_wide ? eyeW : 0;

		cylinder.subImage.imageRect.offset = XrOffset2Di{0, 0};
		cylinder.subImage.imageRect.extent = XrExtent2Di{sub_w, sub_h};

		if(use_dual_layers)
		{
			cylinder.eyeVisibility = XR_EYE_VISIBILITY_LEFT;

			cylinder_right = cylinder;
			cylinder_right.next = nullptr;
			if(layer_settings_supported_ && layer_settings.layerFlags != 0)
			{
				layer_settings_right = layer_settings;
				layer_settings_right.next = nullptr;
				cylinder_right.next = &layer_settings_right;
			}
			if(color_scale_supported_ && quality.videoBrightness != 1.0f)
			{
				color_scale_right = color_scale;
				color_scale_right.next = const_cast<void *>(cylinder_right.next);
				cylinder_right.next = &color_scale_right;
			}
			if(render_path_ == RenderPath::Direct && image_layout_supported_ && vertical_flip_.load())
			{
				image_layout_right = image_layout;
				image_layout_right.next = const_cast<void *>(cylinder_right.next);
				cylinder_right.next = &image_layout_right;
			}

			cylinder_right.subImage.swapchain = right_sc;
			cylinder_right.subImage.imageRect.offset = XrOffset2Di{right_off_x, 0};
			cylinder_right.subImage.imageRect.extent = XrExtent2Di{sub_w, sub_h};
			cylinder_right.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
		}
		else
		{
			cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		}

		if(proj > 0)
		{
			int32_t eff_w = sub_w;
			int32_t eff_h = sub_h;

			if(equirect2_supported_)
			{
				equirect.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR;
				equirect.next = cylinder.next;
				equirect.layerFlags = cylinder.layerFlags;
				equirect.space = cylinder.space;
				equirect.eyeVisibility = use_dual_layers ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
				equirect.subImage = cylinder.subImage;
				equirect.subImage.imageRect.offset = XrOffset2Di{0, 0};
				equirect.subImage.imageRect.extent = XrExtent2Di{eff_w, eff_h};

				equirect.pose.orientation = QuatFromYawPitch(total_yaw, total_pitch);
				equirect.pose.position = recenter_pos;
				equirect.radius = 0.0f;
				equirect.centralHorizontalAngle = is_180 ? 3.14159265f : 6.28318530f;
				equirect.upperVerticalAngle = 1.57079632f;
				equirect.lowerVerticalAngle = -1.57079632f;

				layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&equirect));

				if(use_dual_layers)
				{
					equirect_right = equirect;
					equirect_right.next = cylinder_right.next;
					equirect_right.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
					equirect_right.subImage.swapchain = right_sc;
					equirect_right.subImage.imageRect.offset = XrOffset2Di{right_off_x, 0};
					equirect_right.subImage.imageRect.extent = XrExtent2Di{eff_w, eff_h};
					layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&equirect_right));
				}
			}
			else if(equirect_supported_)
			{
				equirect1.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR;
				equirect1.next = cylinder.next;
				equirect1.layerFlags = cylinder.layerFlags;
				equirect1.space = cylinder.space;
				equirect1.eyeVisibility = use_dual_layers ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
				equirect1.subImage = cylinder.subImage;
				equirect1.subImage.imageRect.offset = XrOffset2Di{0, 0};
				equirect1.subImage.imageRect.extent = XrExtent2Di{eff_w, eff_h};

				equirect1.pose.orientation = QuatFromYawPitch(total_yaw, total_pitch);
				equirect1.pose.position = recenter_pos;
				equirect1.radius = 0.0f;
				equirect1.scale = XrVector2f{is_180 ? 0.5f : 1.0f, 1.0f};
				equirect1.bias = XrVector2f{0.0f, 0.0f};

				layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&equirect1));

				if(use_dual_layers)
				{
					equirect1_right = equirect1;
					equirect1_right.next = cylinder_right.next;
					equirect1_right.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
					equirect1_right.subImage.swapchain = right_sc;
					equirect1_right.subImage.imageRect.offset = XrOffset2Di{right_off_x, 0};
					equirect1_right.subImage.imageRect.extent = XrExtent2Di{eff_w, eff_h};
					layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&equirect1_right));
				}
			}
		}
		else if(layer_shape_.load() == LayerShape::Cylinder)
		{
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder));
			if(use_dual_layers)
				layers.push_back(
						reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder_right));
		}
		else
		{
			// Quad equivalente: largura que subtende o mesmo arco a mesma
			// distancia, para a troca de forma nao mudar o tamanho aparente.
			quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			quad.next = cylinder.next;
			quad.layerFlags = cylinder.layerFlags;
			quad.space = cylinder.space;
			quad.eyeVisibility = use_dual_layers ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_BOTH;
			quad.subImage = cylinder.subImage;
			quad.subImage.imageRect.offset = XrOffset2Di{0, 0};
			quad.subImage.imageRect.extent = XrExtent2Di{sub_w, sub_h};

			float width = 2.0f * params.radius * std::tan(params.centralAngle * 0.5f);
			float height = width / params.aspectRatio;
			quad.size = XrExtent2Df{width, height};

			// A pose do cilindro marca o eixo; a do quad marca o centro da tela,
			// entao ele precisa ser empurrado para a frente pelo raio.
			float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
			float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
			float cos_pitch = std::cos(total_pitch);
			float sin_pitch = std::sin(total_pitch);

			quad.pose.orientation = QuatFromYawPitch(total_yaw, total_pitch);
			quad.pose.position = XrVector3f{
				recenter_pos.x - std::sin(total_yaw) * cos_pitch * params.radius,
				recenter_pos.y + params.heightOffset + sin_pitch * params.radius,
				recenter_pos.z - std::cos(total_yaw) * cos_pitch * params.radius
			};

			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
			if(use_dual_layers)
			{
				quad_right = quad;
				quad_right.next = cylinder_right.next;
				quad_right.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
				quad_right.subImage.swapchain = right_sc;
				quad_right.subImage.imageRect.offset = XrOffset2Di{right_off_x, 0};
				quad_right.subImage.imageRect.extent = XrExtent2Di{sub_w, sub_h};
				layers.push_back(
						reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad_right));
			}
		}
	}

	const size_t danmaku_insert_idx = layers.size();
	XrCompositionLayerCylinderKHR danmaku_cyl{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
	XrCompositionLayerCylinderKHR danmaku_cyl_right{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
	XrCompositionLayerQuad danmaku_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerQuad danmaku_quad_right{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerImageLayoutFB danmaku_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	XrCompositionLayerImageLayoutFB danmaku_layout_right{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	XrCompositionLayerSettingsFB danmaku_settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
	XrCompositionLayerSettingsFB danmaku_settings_right{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};

	XrCompositionLayerQuad settings_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerImageLayoutFB settings_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	if(settings_visible_.load())
	{
		ScreenParams params;
		XrQuaternionf recenter;
		XrVector3f set_recenter_pos;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			recenter = recenter_orientation_;
			set_recenter_pos = recenter_position_;
		}
		float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
		float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		if(UpdateSettingsLayer(settings_layer, total_yaw, total_pitch, params, set_recenter_pos))
		{
			if(image_layout_supported_)
			{
				settings_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
				settings_layer.next = &settings_layout;
			}
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&settings_layer));
		}
	}

	// O painel entra por ultimo, para ficar por cima da tela do console.
	XrCompositionLayerQuad hud_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerImageLayoutFB hud_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	// The launch splash shares the HUD swapchain. Always submit its layer while
	// armed, even if a normal HUD refresh has not run yet (Java uploads the
	// first spinner frame before the frame loop starts).
	if(hud_visible_.load() || loading_visible_.load())
	{
		ScreenParams params;
		XrQuaternionf recenter;
		XrVector3f hud_recenter_pos;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			recenter = recenter_orientation_;
			hud_recenter_pos = recenter_position_;
		}
		float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
		float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		if(UpdateHudLayer(hud_layer, total_yaw, total_pitch, params, hud_recenter_pos))
		{
			if(loading_visible_.load())
			{
				static bool logged_splash_layer = false;
				if(!logged_splash_layer)
				{
					logged_splash_layer = true;
					LOGI("VR splash layer submitted: centered %dx%d card, image=%d",
							kHudWidth, kHudHeight, (int)hud_has_image_.load());
				}
			}
			if(image_layout_supported_)
			{
				hud_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
				hud_layer.next = &hud_layout;
			}
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&hud_layer));
		}
	}

	XrCompositionLayerQuad osd_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerImageLayoutFB osd_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	if(osd_visible_.load())
	{
		ScreenParams params;
		XrQuaternionf recenter;
		XrVector3f osd_recenter_pos;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			recenter = recenter_orientation_;
			osd_recenter_pos = recenter_position_;
		}
		float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
		float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		if(UpdateOsdLayer(osd_layer, total_yaw, total_pitch, params, osd_recenter_pos))
		{
			if(image_layout_supported_)
			{
				osd_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
				osd_layer.next = &osd_layout;
			}
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&osd_layer));
		}
	}

	XrCompositionLayerQuad res_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerImageLayoutFB res_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
	if(res_visible_.load())
	{
		ScreenParams params;
		XrQuaternionf recenter;
		XrVector3f res_recenter_pos;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			recenter = recenter_orientation_;
			res_recenter_pos = recenter_position_;
		}
		float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
		float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		if(UpdateResLayer(res_layer, total_yaw, total_pitch, params, res_recenter_pos))
		{
			if(image_layout_supported_)
			{
				res_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
				res_layer.next = &res_layout;
			}
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&res_layer));
		}
	}

	XrCompositionLayerQuad corner_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(UpdateCornerLayer(corner_layer))
	{
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&corner_layer));
	}

	XrCompositionLayerQuad cursor_layer_0{XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(UpdateCursorLayer(cursor_layer_0, 0))
	{
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cursor_layer_0));
	}
	XrCompositionLayerQuad cursor_layer_1{XR_TYPE_COMPOSITION_LAYER_QUAD};
	if(UpdateCursorLayer(cursor_layer_1, 1))
	{
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cursor_layer_1));
	}

	// Update and insert the danmaku layer AFTER all video & UI layers so the danmaku
	// worker gets the full frame interval, and wake the worker immediately once consumed
	// (before xrEndFrame IPC blocks) so the next frame never misses its deadline or overwrites.
	if(danmaku_visible_.load())
	{
		ScreenParams params;
		XrQuaternionf recenter;
		XrVector3f dm_recenter_pos;
		{
			std::lock_guard<std::mutex> lock(params_mutex_);
			params = params_;
			recenter = recenter_orientation_;
			dm_recenter_pos = recenter_position_;
		}
		float total_yaw = YawFromQuat(recenter) + params.yawOffset + frame_screen_yaw;
		float total_pitch = PitchFromQuat(recenter) + frame_screen_pitch;
		bool dm_is_quad = false;
		bool dm_is_stereo = false;
		if(UpdateDanmakuLayer(danmaku_cyl, danmaku_cyl_right, danmaku_quad, danmaku_quad_right, dm_is_quad, dm_is_stereo, total_yaw, total_pitch, params, dm_recenter_pos))
		{
			if(image_layout_supported_)
			{
				danmaku_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
				if(dm_is_quad)
				{
					danmaku_quad.next = &danmaku_layout;
					if(dm_is_stereo)
					{
						danmaku_layout_right.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
						danmaku_quad_right.next = &danmaku_layout_right;
					}
				}
				else
				{
					danmaku_cyl.next = &danmaku_layout;
					if(dm_is_stereo)
					{
						danmaku_layout_right.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
						danmaku_cyl_right.next = &danmaku_layout_right;
					}
				}
			}
			// Compositor hardware sharpening tightens scrolled glyphs without extra app GPU/CPU cost.
			if(layer_settings_supported_)
			{
				danmaku_settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
				if(dm_is_quad)
				{
					danmaku_settings.next = const_cast<void *>(danmaku_quad.next);
					danmaku_quad.next = &danmaku_settings;
					if(dm_is_stereo)
					{
						danmaku_settings_right.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
						danmaku_settings_right.next = const_cast<void *>(danmaku_quad_right.next);
						danmaku_quad_right.next = &danmaku_settings_right;
					}
				}
				else
				{
					danmaku_settings.next = const_cast<void *>(danmaku_cyl.next);
					danmaku_cyl.next = &danmaku_settings;
					if(dm_is_stereo)
					{
						danmaku_settings_right.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
						danmaku_settings_right.next = const_cast<void *>(danmaku_cyl_right.next);
						danmaku_cyl_right.next = &danmaku_settings_right;
					}
				}
			}
			const XrCompositionLayerBaseHeader *dm_layers[2];
			size_t dm_count = 0;
			if(dm_is_quad)
			{
				dm_layers[dm_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&danmaku_quad);
				if(dm_is_stereo)
					dm_layers[dm_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&danmaku_quad_right);
			}
			else
			{
				dm_layers[dm_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&danmaku_cyl);
				if(dm_is_stereo)
					dm_layers[dm_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&danmaku_cyl_right);
			}
			layers.insert(layers.begin() + danmaku_insert_idx, dm_layers, dm_layers + dm_count);
		}
	}

	if(frame_state.shouldRender)
	{
		// (The danmaku worker wake-up is signalled at the start of the frame now.)
	}

	XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = (uint32_t)layers.size();
	end_info.layers = layers.empty() ? nullptr : layers.data();

	XrLocalDimmingFrameEndInfoMETA dimming{XR_TYPE_LOCAL_DIMMING_FRAME_END_INFO_META};
	if(local_dimming_supported_)
	{
		dimming.localDimmingMode = passthrough_enabled_.load()
				? XR_LOCAL_DIMMING_MODE_OFF_META
				: XR_LOCAL_DIMMING_MODE_ON_META;
		dimming.next = end_info.next;
		end_info.next = &dimming;
		if(!logged_local_dimming_)
		{
			logged_local_dimming_ = true;
			LOGI("Local dimming attached to the frame (off while passthrough is on)");
		}
	}
	XrResult end_res = xrEndFrame(session_, &end_info);
	if(XR_FAILED(end_res))
	{
		static int error_count = 0;
		if(error_count++ < 20)
		{
			LOGE("xrEndFrame failed: %d, layerCount=%u", (int)end_res, (unsigned)layers.size());
		}
	}

	static auto pacing_last = std::chrono::steady_clock::now();
	static int pacing_frames = 0;
	if(frame_state.shouldRender) pacing_frames++;
	auto pacing_now = std::chrono::steady_clock::now();
	auto pacing_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			pacing_now - pacing_last).count();
	if(pacing_ms >= 3000)
	{
		LOGI("xr frame pacing: %.1f fps, layers=%u, splash=%d, screenYaw=%.3f, screenPitch=%.3f",
				(double)pacing_frames * 1000.0 / (double)pacing_ms,
				(unsigned)layers.size(), (int)loading_visible_.load(),
				(double)frame_screen_yaw, (double)frame_screen_pitch);
		pacing_frames = 0;
		pacing_last = pacing_now;
	}
}

void XrVideoSession::Destroy()
{
	StopFrameLoop();
	session_renderable_.store(false);

	// Sem sessao imersiva nao ha cabeca para rastrear, e o filtro de audio tem
	// de voltar a deixar o som passar como veio.
	Spatializer::Instance().Clear();

	if(passthrough_layer_ != XR_NULL_HANDLE && pfnDestroyPassthroughLayerFB)
		pfnDestroyPassthroughLayerFB(passthrough_layer_);
	if(passthrough_ != XR_NULL_HANDLE && pfnDestroyPassthroughFB)
		pfnDestroyPassthroughFB(passthrough_);
	passthrough_layer_ = XR_NULL_HANDLE;
	passthrough_ = XR_NULL_HANDLE;

	if(video_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(video_swapchain_);
	video_swapchain_ = XR_NULL_HANDLE;
	if(video_swapchain_right_ != XR_NULL_HANDLE)
		xrDestroySwapchain(video_swapchain_right_);
	video_swapchain_right_ = XR_NULL_HANDLE;

	tone_mapper_.Destroy();
	tone_mapper_ready_ = false;
	video_images_.clear();
	video_images_right_.clear();
	pending_gl_width_ = 0;
	pending_gl_height_ = 0;
	requested_eye_w_ = 0;
	requested_eye_h_ = 0;
	active_gl_req_w_ = 0;
	active_gl_req_h_ = 0;
	eye_width_ = 0;
	eye_height_ = 0;
	video_swapchain_rendered_ = false;
	wide_sbs_swapchain_ = false;
	prev_predicted_display_time_ns_ = 0;
	SetCurrentDisplayRefreshHz(72.0f);
	ResetDisplayPacing();

	if(settings_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(settings_swapchain_);
	settings_swapchain_ = XR_NULL_HANDLE;
	settings_images_.clear();
	settings_has_image_.store(false);
	settings_dirty_count_.store(0);
	if(hud_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(hud_swapchain_);
	hud_swapchain_ = XR_NULL_HANDLE;
	hud_images_.clear();
	hud_has_image_.store(false);
	hud_dirty_count_.store(0);
	if(osd_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(osd_swapchain_);
	osd_swapchain_ = XR_NULL_HANDLE;
	osd_images_.clear();
	osd_has_image_.store(false);
	osd_dirty_count_.store(0);
	if(res_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(res_swapchain_);
	res_swapchain_ = XR_NULL_HANDLE;
	res_images_.clear();
	res_has_image_.store(false);
	res_dirty_count_.store(0);
	if(danmaku_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(danmaku_swapchain_);
	danmaku_swapchain_ = XR_NULL_HANDLE;
	danmaku_swapchain_acquired_ = false;
	danmaku_images_.clear();
	if(danmaku_swapchain_right_ != XR_NULL_HANDLE)
		xrDestroySwapchain(danmaku_swapchain_right_);
	danmaku_swapchain_right_ = XR_NULL_HANDLE;
	danmaku_right_swapchain_acquired_ = false;
	danmaku_images_right_.clear();
	if(danmaku_clear_fbo_ != 0)
	{
		if(eglGetCurrentContext() != EGL_NO_CONTEXT)
			glDeleteFramebuffers(1, &danmaku_clear_fbo_);
		danmaku_clear_fbo_ = 0;
	}
	danmaku_img_clear_gen_.clear();
	danmaku_img_clear_gen_right_.clear();
	danmaku_has_image_.store(false);
	danmaku_right_has_image_.store(false);
	danmaku_dirty_count_.store(0);
	{
		std::lock_guard<std::mutex> lock(danmaku_mutex_);
		g_dm_slots[0].valid = false;
		g_dm_slots[1].valid = false;
	}
	g_dm_upload_h_hint.store(0);
	if(cursor_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(cursor_swapchain_);
	cursor_swapchain_ = XR_NULL_HANDLE;
	cursor_images_.clear();
	cursor_has_image_.store(false);
	cursor_visible_[0].store(false);
	cursor_visible_[1].store(false);
	hand_cursor_active_.store(false);
	if(corner_swapchain_ != XR_NULL_HANDLE)
		xrDestroySwapchain(corner_swapchain_);
	corner_swapchain_ = XR_NULL_HANDLE;
	corner_images_.clear();
	corner_has_image_.store(false);
	corner_hovered_.store(false);
	corner_resizing_.store(false);

	if(pfnDestroyHandTrackerEXT_)
	{
		for(int h = 0; h < 2; h++)
		{
			if(hand_trackers_[h] != XR_NULL_HANDLE)
			{
				pfnDestroyHandTrackerEXT_(hand_trackers_[h]);
				hand_trackers_[h] = XR_NULL_HANDLE;
			}
		}
	}

	if(app_space_ != XR_NULL_HANDLE)
		xrDestroySpace(app_space_);
	if(view_space_ != XR_NULL_HANDLE)
		xrDestroySpace(view_space_);
	app_space_ = XR_NULL_HANDLE;
	view_space_ = XR_NULL_HANDLE;

	if(session_ != XR_NULL_HANDLE)
		xrDestroySession(session_);
	session_ = XR_NULL_HANDLE;

	DestroyEgl();

	if(instance_ != XR_NULL_HANDLE)
		xrDestroyInstance(instance_);
	instance_ = XR_NULL_HANDLE;

	if(activity_ != nullptr && vm_ != nullptr)
	{
		JNIEnv *env = nullptr;
		if(vm_->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK)
			env->DeleteGlobalRef(activity_);
		activity_ = nullptr;
	}
}

} // namespace p5m
