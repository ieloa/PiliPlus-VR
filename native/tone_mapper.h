// SPDX-License-Identifier: AGPL-3.0-only
//
// Converte o vÃ­deo HDR do console para SDR, na GPU.
//
// Existe porque o decodificador do Quest 3 ignora o pedido de mapeamento de
// tons: ele entrega P010 com curva PQ intacta (color-transfer=6), e uma curva
// PQ lida como se fosse SDR estoura os brancos em qualquer cena clara.
//
// O custo Ã© uma passada de GPU e o fim do caminho sem cÃ³pia: em vez de o
// MediaCodec escrever direto no swapchain do compositor, ele escreve numa
// SurfaceTexture que este cÃ³digo lÃª, converte e desenha num swapchain comum.
// Ã‰ por isso que o modo direto continua existindo â€?quem quer latÃªncia mÃ­nima
// e nÃ£o se incomoda com faixas em gradiente fica com ele.
#pragma once

#include <chrono>

#include <android/surface_texture.h>
// ASurfaceTexture_fromSurfaceTexture vive no header JNI, nao no principal.
#include <android/surface_texture_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
// GL_TEXTURE_EXTERNAL_OES e extensao OES: nao esta no gl3.h.
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace p5m {

// Intro curtain length. Java picks 3600 ms for a cold model build (the GPU
// delegate needs ~3 s to compile) and 1000 ms once that interpreter is cached
// for the process. Shared through a static native so no XrBridge.smali edit
// is needed.
void SetCurtainDurationMs(int32_t ms);
int32_t CurtainDurationMs();

// Zero-copy DirectByteBuffer bridge shared with ZipDepthSource.
bool RegisterAiSlotBuffers(void *in0, int64_t inBytes0, void *out0, int64_t outBytes0,
		void *in1, int64_t inBytes1, void *out1, int64_t outBytes1,
		int32_t inW, int32_t inH, int32_t inType,
		int32_t outW, int32_t outH, int32_t outType);
void UnregisterAiSlotBuffers();
void SetAiFastEma(bool on);
int32_t AcquireAiInputSlot(int32_t timeoutMs);
void ReleaseAiInputSlot(int32_t slot);
void SubmitAiOutputSlot(int32_t slot);

// P2 edge enhancement (bilateral Sigmoid sharpen + warp 4-tap B-spline) & GPU timing telemetry.
void SetAiEdgeEnhance(float sharp, bool cubic);
float AiEdgeSharp();
bool AiDepthCubic();
float GetLastWarpGpuMs();
void SetCurrentDisplayRefreshHz(float hz);
float GetCurrentDisplayRefreshHz();
void RecordDisplayPacingSample(bool missed);
void ResetDisplayPacing();
float GetMissedRefreshPct();

class ToneMapper
{
public:
	/**
	 * Compila o programa e cria a textura externa.
	 *
	 * Precisa rodar na thread do frame loop, com o contexto EGL corrente: a
	 * textura externa pertence ao contexto em que nasce.
	 */
	bool Init();

	/** Liga a SurfaceTexture do Java ao contexto GL desta thread. */
	bool Attach(ASurfaceTexture *texture);

	/**
	 * Consome o frame mais recente e desenha no destino.
	 *
	 * `target` e a textura do swapchain, ja adquirida, ou zero para desenhar no
	 * framebuffer padrao -- o caso da janela, onde nao ha swapchain.
	 *
	 * `sharpen` e a intensidade da mascara de nitidez, 0 para nenhuma. Chega
	 * como numero, e nao como degrau, porque quem escolhe os degraus e a
	 * interface -- aqui so existe a manopla.
	 *
	 * `encode` diz se o shader precisa aplicar a curva do sRGB antes de
	 * escrever. Falso quando o alvo e um swapchain sRGB, que a aplica sozinho.
	 *
	 * `pq` e true quando a fonte e PQ (10 bits), false para SDR em 8 bits.
	 *
	 * Devolve false quando nao havia frame novo nem antigo para desenhar.
	 */
	/** True while the intro curtain still needs alpha blending. */
	bool CurtainActive() const;

	enum class FrameAction
	{
		kNoFrame,
		kReuseLast,
		kNeedDraw,
	};

	/** Update video frame & AI depth state before acquiring any swapchain image. */
	FrameAction PrepareFrame(int32_t eyeWidth, int32_t eyeHeight, int stereoEffMode,
			float stereoStrength, float stereoConv, bool pq, float sharpen,
			bool swapchainReady);

	bool RenderPrepared(GLuint target, int32_t width, int32_t height, bool pq,
			float sharpen, bool encode, bool extrapolate = false,
			GLuint target_right = 0, bool wide_sbs = false);

	bool Render(GLuint target, int32_t width, int32_t height, bool pq, float sharpen,
			bool encode, bool extrapolate = false, GLuint target_right = 0,
			bool wide_sbs = false);

	/** Reset cached FBO target validation after swapchain recreation. */
	void ResetFboCache()
	{
		validated_count_ = 0;
		last_rendered_valid_ = false;
	}

	/** Signal interactive screen drag/zoom so AI GPU compute yields VSync headroom. */
	void SetAiManipulating(bool manipulating)
	{
		ai_manipulating_.store(manipulating, std::memory_order_release);
	}

	/**
	 * Liga o olho sintetizado.
	 *
	 * `strength` e a disparidade maxima em fracao da largura da fonte, e
	 * `convergence` a profundidade que fica no plano da tela. `groundDown`
	 * diz se a borda de baixo do alvo e a parte de baixo da cena -- a pista
	 * mais forte da estimativa depende disso, e o compositor pode estar
	 * invertendo a imagem.
	 *
	 * Cada olho tem um alvo proprio, do tamanho da fonte, passado ao Render
	 * como `target` e `target_right`. Ja foi um alvo unico do dobro da largura,
	 * com cada olho numa metade: funcionava, mas obrigava a submeter cada
	 * camada como recorte de uma textura maior, e sobre um recorte o filtro de
	 * nitidez do compositor produziu um X atravessando a tela. Dois alvos
	 * inteiros custam o mesmo numero de pixels e devolvem o MQSR ao 3D.
	 */
	void SetStereo(int mode, float strength, float convergence, bool groundDown)
	{
		stereo_mode_ = mode;
		stereo_strength_ = strength;
		convergence_ = convergence;
		ground_sign_ = groundDown ? 1.0f : -1.0f;
	}

	/**
	 * Liga o histÃ³rico usado pela extrapolaÃ§Ã£o de quadros.
	 *
	 * `format` precisa ser o mesmo do swapchain de destino: a extensÃ£o exige
	 * que as trÃªs texturas tenham dimensÃ£o e formato iguais, e um formato
	 * diferente faria a chamada falhar em silÃªncio em vez de dar erro.
	 *
	 * Devolve false quando o aparelho nÃ£o tem `GL_QCOM_frame_extrapolation`, e
	 * aÃ­ a extrapolaÃ§Ã£o simplesmente nÃ£o existe -- o caminho normal continua.
	 */
	bool EnableExtrapolation(GLenum format);

	bool ExtrapolationReady() const { return extrapolate_fn_ != nullptr; }

	void Destroy();

	/** Nits em que o branco de referÃªncia do SDR Ã© mapeado. */
	void SetTargetNits(float nits) { target_nits_ = nits; }

	// AI depth source control (called from inference/UI thread).
	void SetAiDepthMode(bool on, int32_t mapsPerSecond);
	void SetAiCaptureRate(int32_t mapsPerSecond);
	int32_t AiFrameWidth() const;
	int32_t AiFrameHeight() const;
	bool AiDepthEnabled() const { return ai_mode_.load(); }

	/**
	 * Moonlight-style stage thread: give the depth resample + texture upload
	 * its own EGL context sharing objects with the main renderer. The shared
	 * context must already exist; this method only starts the thread. If the
	 * runtime refuses to bind it, StageEnabled() stays false and Render()
	 * keeps doing the work on the frame loop exactly as before.
	 */
	bool InitStageEgl(EGLDisplay display, EGLSurface surface, EGLContext context);
	void StopStage();
	bool StageEnabled() const { return stage_enabled_.load(); }

	// Debug view, toggled from the VR settings panel (any thread).
	void SetShowDepth(bool on) { show_depth_.store(on); }
	bool ShowDepth() const { return show_depth_.load(); }

private:
	bool CompileProgram();

	// ---- Shared-EGL AI stage thread -------------------------------------
	void StageThread();
	void StageWorkOnce();
	/** Frame loop: adopt a finished stage upload, or keep the old map. */
	void StageAdopt(int32_t width, int32_t height);
	bool EnsureHistory(int32_t width, int32_t height, int slot);
	bool AttachAndValidateTarget(GLuint target, bool write_history, GLuint history_tex);
	void UploadCommonUniforms(int32_t eyeWidth, int32_t eyeHeight, bool pq, float sharpen,
			bool encode, const float *matrix);
	bool DrawEye(GLuint target, int32_t width, int32_t height, bool pq, float sharpen,
			bool encode, bool extrapolate, bool fresh, const float *matrix,
			int eye, int slot);
	bool DrawWideStereo(GLuint target, int32_t eyeWidth, int32_t eyeHeight, bool pq,
			float sharpen, bool encode, const float *matrix);
	bool EnsureWindowTarget(int32_t width, int32_t height);
	GLuint EnsureDummyTexture();
	bool IsTargetValidated(GLuint tex) const;
	void MarkTargetValidated(GLuint tex);
	static constexpr int kMaxValidatedTargets = 16;
	struct ValidatedTarget { GLuint tex = 0; bool layered = false; };
	ValidatedTarget validated_targets_[kMaxValidatedTargets] = {};
	int validated_count_ = 0;
	bool depth_for_ai_ = false;

	ASurfaceTexture *surface_texture_ = nullptr;
	bool attached_ = false;
	bool has_frame_ = false;
	// Intro curtain: starts on the first real video frame.
	bool curtain_started_ = false;
	std::chrono::steady_clock::time_point curtain_start_;
	// Erro dentro do loop de frame fala uma vez: repetido, so afoga o log.
	bool logged_fbo_failure_ = false;
	bool logged_layered_ = false;

	GLuint program_ = 0;
	GLuint external_tex_ = 0;
	GLuint fbo_ = 0;
	/**
	 * Textura em que a janela e desenhada antes de ir para a tela.
	 *
	 * O modo janela nao desenha mais direto no framebuffer padrao. Ver o
	 * comentario em Render(): o shader tem duas saidas e o driver Adreno recusa
	 * isso quando o alvo e a tela.
	 */
	GLuint window_tex_ = 0;
	/**
	 * Uma textura 2D de um pixel, so para o sampler da profundidade ter onde
	 * apontar quando o 3D esta desligado. Ver Render().
	 */
	GLuint dummy_tex_ = 0;
	int32_t window_w_ = 0;
	int32_t window_h_ = 0;
	GLuint vbo_ = 0;
	GLuint vao_ = 0;

	GLint loc_tex_matrix_ = -1;
	GLint loc_sampler_ = -1;
	GLint loc_pq_ = -1;
	GLint loc_target_nits_ = -1;
	GLint loc_sharpen_ = -1;
	GLint loc_encode_ = -1;
	GLint loc_texel_step_ = -1;
	GLint loc_stereo_ = -1;
	GLint loc_eye_ = -1;
	GLint loc_depth_tex_ = -1;

	// -- Passada da profundidade -------------------------------------------
	//
	// Alvo proprio, a um quarto da resolucao em cada eixo. Duas texturas em
	// rodizio porque a passada le a anterior -- para suavizar no tempo e para
	// saber o que ficou parado -- e escreve na outra.
	GLuint depth_program_ = 0;
	GLuint depth_fbo_ = 0;
	GLuint depth_tex_[2] = { 0, 0 };
	int depth_newest_ = 0;
	bool depth_has_prev_ = false;
	int32_t depth_width_ = 0;
	int32_t depth_height_ = 0;
	bool logged_depth_ = false;
	bool logged_depth_failure_ = false;

	GLint dloc_tex_matrix_ = -1;
	GLint dloc_texel_step_ = -1;
	GLint dloc_sampler_ = -1;
	GLint dloc_prev_ = -1;
	GLint dloc_has_prev_ = -1;
	GLint dloc_ground_sign_ = -1;
	GLint dloc_convergence_ = -1;

	void DepthTargetSize(int32_t width, int32_t height, int32_t *w, int32_t *h) const;
	bool EnsureDepthTargets(int32_t width, int32_t height);
	void RenderDepth(const float *matrix, int32_t width, int32_t height);

	// -- Optional neural depth source --------------------------------------
	// The model runs off the frame loop using zero-copy registered direct
	// ByteBuffers shared with C++. CollectAiRgbPbo() fills the input slot
	// directly in C++; UploadAiDepthSlot() normalizes, smooths, and packs
	// GL_RGBA8 [R, G, B, D] on the shared-EGL stage thread.
	std::mutex ai_collect_mutex_;
	std::atomic<bool> ai_mode_{false};
	std::atomic<bool> show_depth_{false};
	std::atomic<bool> ai_uploaded_{false};
	int64_t ai_last_upload_ns_ = 0;
	std::atomic<int64_t> ai_capture_period_ns_{33333333};
	std::atomic<int32_t> ai_maps_per_sec_{30};
	int64_t ai_last_capture_ns_ = 0;
	int64_t ai_capture_accum_ns_ = 0;
	int64_t ai_last_tick_ns_ = 0;
	GLuint ai_program_ = 0;
	GLuint ai_fbo_ = 0;
	GLuint ai_tex_ = 0;
	static constexpr int kAiPboCount = 2;
	GLuint ai_pbo_[kAiPboCount] = {0, 0};
	std::atomic<bool> ai_pbo_pending_[kAiPboCount]{};
	std::atomic<GLsync> ai_pbo_fence_[kAiPboCount]{nullptr, nullptr};
	int ai_pbo_capture_slot_ = 0;
	std::atomic<int64_t> ai_pbo_seq_[kAiPboCount]{};
	std::atomic<int64_t> ai_pbo_seq_next_{0};
	GLint ai_loc_tex_matrix_ = -1;
	GLint ai_loc_sampler_ = -1;
	GLenum ai_depth_format_ = 0;
	bool logged_ai_failure_ = false;

	// Shared EGL context owned by XrVideoSession; the stage thread binds it
	// and never touches the main context (FBOs and VAOs are not shareable).
	EGLDisplay stage_display_ = EGL_NO_DISPLAY;
	EGLSurface stage_surface_ = EGL_NO_SURFACE;
	EGLContext stage_context_ = EGL_NO_CONTEXT;
	std::thread stage_thread_;
	std::atomic<bool> stage_running_{false};
	std::atomic<bool> stage_enabled_{false};
	std::mutex stage_mutex_;
	std::condition_variable stage_cv_;
	bool stage_wake_ = false;

	// One upload in flight: the stage thread writes the BACK depth texture and
	// publishes a GL fence; the frame loop adopts it only once the fence is
	// signalled, so the warp shader never samples a half-written texture.
	std::atomic<int> stage_write_dest_{1};
	std::atomic<int> stage_published_dest_{-1};
	std::atomic<GLsync> stage_fence_{nullptr};
	std::atomic<GLsync> stage_retired_fence_{nullptr};
	std::atomic<int64_t> stage_published_ns_{0};

	struct StageTargets
	{
		GLuint tex[2] = {0, 0};
		int32_t w = 0;
		int32_t h = 0;
		GLenum format = 0;
	};
	std::mutex stage_targets_mutex_;
	StageTargets stage_targets_;
	GLuint stage_neutral_tex_ = 0;

	// Stage-thread-only scratch buffer for GL_RGBA8 [R,G,B,D] packing.
	std::vector<uint8_t> stage_rgba8_;

	bool EnsureAiCaptureTargets();
	bool CaptureAiRgb(const float *matrix);
	void CollectAiRgbPbo();
	void AiCaptureTick(const float *matrix);
	bool DrainAiDepth(int32_t width, int32_t height);
	void AiFrameStep(const float *matrix, int32_t width, int32_t height);
	void DestroyAiTargets();
public:
	void WakeStage();
	void SetCornerParams(float radius, float aspect)
	{
		corner_radius_.store(radius, std::memory_order_relaxed);
		corner_aspect_.store(aspect, std::memory_order_relaxed);
	}
private:
	GLint loc_corner_radius_ = -1;
	GLint loc_corner_aspect_ = -1;
	std::atomic<float> corner_radius_{0.032f};
	std::atomic<float> corner_aspect_{1.7778f};
	GLint loc_stereo_strength_ = -1;
	GLint loc_convergence_ = -1;
	GLint loc_ai_depth_ = -1;
	GLint loc_offset_tex_ = -1;
	GLint loc_lowres_width_ = -1;
	GLint loc_frame_width_ = -1;
	GLint loc_disp_texels_ = -1;
	GLint loc_show_depth_ = -1;
	GLint loc_curtain_ = -1;
	GLint loc_depth_in_alpha_ = -1;
	GLint loc_depth_cubic_ = -1;
	GLint loc_depth_dims_ = -1;

	// -- GL_EXT_disjoint_timer_query GPU frame timing ----------------------
	typedef void (GL_APIENTRY *GenQueriesExtFn)(GLsizei n, GLuint *ids);
	typedef void (GL_APIENTRY *DeleteQueriesExtFn)(GLsizei n, const GLuint *ids);
	typedef void (GL_APIENTRY *BeginQueryExtFn)(GLenum target, GLuint id);
	typedef void (GL_APIENTRY *EndQueryExtFn)(GLenum target);
	typedef void (GL_APIENTRY *GetQueryObjectuivExtFn)(GLuint id, GLenum pname, GLuint *params);
	typedef void (GL_APIENTRY *GetQueryObjectui64vExtFn)(GLuint id, GLenum pname, GLuint64 *params);
	static constexpr int kGpuTimerCount = 4;
	GenQueriesExtFn fn_gen_queries_ = nullptr;
	DeleteQueriesExtFn fn_delete_queries_ = nullptr;
	BeginQueryExtFn fn_begin_query_ = nullptr;
	EndQueryExtFn fn_end_query_ = nullptr;
	GetQueryObjectuivExtFn fn_get_query_uiv_ = nullptr;
	GetQueryObjectui64vExtFn fn_get_query_ui64v_ = nullptr;
	GLuint gpu_timer_queries_[kGpuTimerCount] = {};
	bool gpu_timer_pending_[kGpuTimerCount] = {};
	int gpu_timer_wait_[kGpuTimerCount] = {};
	int gpu_timer_write_ = 0;
	bool gpu_timer_supported_ = false;
	bool gpu_timer_active_ = false;
	bool logged_timer_stuck_ = false;
	int64_t gpu_timer_window_ns_ = 0;
	int gpu_timer_window_samples_ = 0;
	uint32_t gpu_timer_sample_seq_ = 0;
	std::chrono::steady_clock::time_point gpu_timer_window_start_{};
	void InitGpuTimers();
	void DestroyGpuTimers();
	void HarvestGpuTimers();
	bool BeginGpuTimer();
	void EndGpuTimer();
public:
	void EndGpuTimerFromGuard() { EndGpuTimer(); }
private:

	// -- Joint bilateral upsample pass (Moonlight XR) -----------------------
	// Upsamples GL_RGBA8 [R,G,B,D] depth to quarter eye resolution (clamped to
	// [512, 768]px long edge) aligned with video RGB edges before offset search.
	GLuint bilateral_program_ = 0;
	GLuint bilateral_fbo_ = 0;
	GLuint bilateral_tex_ = 0;
	int32_t bilateral_w_ = 0;
	int32_t bilateral_h_ = 0;
	GLint bloc_tex_matrix_ = -1;
	GLint bloc_sampler_ = -1;
	GLint bloc_depth_tex_ = -1;
	GLint bloc_depth_size_ = -1;
	GLint bloc_sigma_r_ = -1;
	GLint bloc_sharp_ = -1;
	bool bilateral_ready_ = false;
	int64_t bilateral_built_upload_ns_ = -1;
	float bilateral_built_sharp_ = -1.0f;

	void BilateralTargetSize(int32_t eyeWidth, int32_t eyeHeight,
			int32_t *outW, int32_t *outH) const;
	bool CompileBilateralProgram();
	bool EnsureBilateralTargets(int32_t eyeWidth, int32_t eyeHeight);
	bool RenderBilateralMap(const float *matrix, int32_t eyeWidth, int32_t eyeHeight);
	void DestroyBilateralTargets();

	// -- Offset search pass (occlusion-aware inverse warp) ------------------
	// Runs once per frame at depth-map resolution, before the two eye draws.
	GLuint offset_program_ = 0;
	GLuint offset_fbo_ = 0;
	GLuint offset_tex_ = 0;
	int32_t offset_w_ = 0;
	int32_t offset_h_ = 0;
	GLint oloc_depth_ = -1;
	GLint oloc_disp_ = -1;
	GLint oloc_conv_ = -1;
	// Gates the gather warp. If the offset pass ever fails to compile or to
	// render, the stock warp formula is used instead of reading a stale table.
	bool offset_ready_ = false;
	// The offset shader only samples uDepthTex plus two scalar uniforms, so
	// its result cannot change unless the depth map, disparity or convergence
	// changed. The depth map arrives ~15 times a second while the frame loop
	// runs at 72 Hz; rebuilding the table on every frame was ~5x duplicate
	// GPU work and one of the reasons the runtime kept dropping 90 -> 72 Hz.
	bool offset_cached_ = false;
	int64_t offset_built_at_upload_ns_ = -1;
	float offset_built_disp_ = -1.0f;
	float offset_built_conv_ = -1.0f;

	bool CompileOffsetProgram();
	bool EnsureOffsetTargets(int32_t width, int32_t height);
	bool RenderOffsetMap(float dispTexels);
	void DestroyOffsetTargets();

	// Estado do modo 3D, posto pelo SetStereo e lido pelo Render (0=off, 1=AI, 2=SBS, 3=OU).
	int stereo_mode_ = 0;
	float stereo_strength_ = 0.0f;
	float convergence_ = 0.35f;
	float ground_sign_ = 1.0f;

	// -- ExtrapolaÃ§Ã£o de quadros (GL_QCOM_frame_extrapolation) --------------
	//
	// A fonte entrega 60 quadros por segundo e o painel mostra 120: metade das
	// passadas do frame loop nÃ£o tem imagem nova, e hoje redesenham a anterior.
	// A extensÃ£o da Qualcomm produz, dessas duas Ãºltimas, um quadro *previsto*
	// -- extrapolaÃ§Ã£o, nÃ£o interpolaÃ§Ã£o, entÃ£o nada Ã© esperado e nenhuma
	// latÃªncia Ã© adicionada.
	//
	// O histÃ³rico Ã© nosso e nÃ£o do swapchain: as imagens do swapchain giram e
	// pertencem ao compositor, e ler uma que ele jÃ¡ estÃ¡ compondo nÃ£o Ã©
	// contrato que exista.
	typedef void (*ExtrapolateFn)(GLuint src1, GLuint src2, GLuint output, GLfloat scale);
	ExtrapolateFn extrapolate_fn_ = nullptr;
	GLenum history_format_ = 0;
	// Um par por olho: [olho][duas mais recentes]. No mono so o par 0 nasce.
	GLuint history_[2][2] = {{0, 0}, {0, 0}};
	int32_t history_width_[2] = {0, 0};
	int32_t history_height_[2] = {0, 0};
	// Qual das duas guarda o quadro mais recente, e quantas jÃ¡ foram escritas:
	// com uma sÃ³ nÃ£o hÃ¡ de onde extrapolar, e prever a partir de uma textura
	// ainda em branco desenharia lixo no primeiro quadro da sessÃ£o.
	int history_newest_[2] = {-1, -1};
	int history_count_[2] = {0, 0};
	bool logged_extrapolation_ = false;
	// Timestamp do ultimo buffer prendido. E o que separa quadro novo de quadro
	// repetido: o updateTexImage devolve sucesso nos dois casos.
	int64_t last_timestamp_ = 0;
	bool logged_cadence_ = false;
	bool logged_window_ = false;
	int frames_seen_ = 0;
	int frames_fresh_ = 0;

	// Prepared frame cache: skip redundant swapchain acquire & heavy shader passes
	// on repeated video frames (~44% of XR passes) when depth & uniforms match.
	float prepared_matrix_[16] = {};
	bool prepared_fresh_ = false;
	bool last_rendered_valid_ = false;
	bool last_rendered_curtain_ = false;
	bool last_rendered_pq_ = false;
	bool last_rendered_ai_ = false;
	bool last_rendered_show_depth_ = false;
	int last_rendered_stereo_ = -1;
	float last_rendered_strength_ = -1.0f;
	float last_rendered_conv_ = -1.0f;
	float last_rendered_sharpen_ = -1.0f;
	float last_rendered_corner_r_ = -1.0f;
	float last_rendered_corner_a_ = -1.0f;
	float last_rendered_nits_ = -1.0f;
	int64_t last_rendered_upload_ns_ = -1;
	int32_t last_rendered_w_ = 0;
	int32_t last_rendered_h_ = 0;

	std::atomic<bool> ai_manipulating_{false};
	std::chrono::steady_clock::time_point manip_pause_start_{};
	std::chrono::steady_clock::time_point manip_last_capture_{};
	bool manip_paused_ = false;

	// 200 nits: o branco de referÃªncia do SDR fica em 100, e o conteÃºdo de jogo
	// costuma ser gravado com destaques bem acima disso. Mapear o pico do PQ
	// direto em 100 apagaria o brilho todo; 200 mantÃ©m alguma sensaÃ§Ã£o de
	// destaque sem estourar.
	float target_nits_ = 200.0f;
};

} // namespace p5m
