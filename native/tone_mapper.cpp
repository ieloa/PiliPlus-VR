// SPDX-License-Identifier: AGPL-3.0-only
#include "tone_mapper.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace p5m {

// Defined in tone_mapper_ai.cpp
void GetAiOutputTargetDimensions(int32_t *outW, int32_t *outH);

namespace {

// The opening curtain covers the whole startup: 400 ms head start +
// ~3.1 s to build the 12 MB ZipDepth model and its GPU delegate. Once
// the picture is fully revealed, playback resumes.
constexpr int32_t kCurtainDurationDefaultMs = 3600;
std::atomic<int32_t> g_curtain_duration_ms{kCurtainDurationDefaultMs};

// P2 visual enhancement & telemetry state shared cleanly across JNI / frame loop.
std::atomic<float> g_ai_edge_sharp{0.0f};
std::atomic<bool> g_ai_depth_cubic{true};
std::atomic<float> g_warp_gpu_ms{-1.0f};
std::atomic<float> g_display_refresh_hz{72.0f};
std::atomic<float> g_missed_refresh_pct{-1.0f};
std::mutex g_pacing_mtx;
int g_pacing_total = 0;
int g_pacing_missed = 0;
int64_t g_pacing_win_start_ns = 0;

// GL_EXT_disjoint_timer_query constants.
constexpr GLenum kGlTimeElapsedExt = 0x88BF;
constexpr GLenum kGlQueryResultExt = 0x8866;
constexpr GLenum kGlQueryResultAvailableExt = 0x8867;
constexpr GLenum kGlGpuDisjointExt = 0x8FBB;

const char *kVertexShader = R"(#version 300 es
in vec2 aPos;
uniform mat4 uTexMatrix;
uniform vec2 uTexelStep;
out vec2 vUv;
out vec2 vDx;
out vec2 vDy;
// Coordenada no ALVO, de 0 a 1. O vUv aponta para a fonte e ja passou pela
// matriz da SurfaceTexture; para saber em qual metade da tela dividida este
// pixel caiu, o que serve e a posicao no destino, antes de qualquer matriz.
out vec2 vScreen;
void main()
{
	vScreen = aPos * 0.5 + 0.5;
	// A matriz da SurfaceTexture carrega recorte e orientacao do buffer, entao
	// ela resolve tambem a inversao vertical -- aqui nao ha o que corrigir a
	// mao, ao contrario do caminho direto para o compositor.
	vec2 uv = aPos * 0.5 + 0.5;
	vUv = (uTexMatrix * vec4(uv, 0.0, 1.0)).xy;

	// O passo de um pixel tem de atravessar a mesma matriz, senao a cruz de
	// amostras da nitidez sai torta em qualquer buffer que venha recortado ou
	// girado. Calculado por vertice, custa quatro multiplicacoes de matriz na
	// sessao inteira em vez de duas por pixel.
	vDx = (uTexMatrix * vec4(uv + vec2(uTexelStep.x, 0.0), 0.0, 1.0)).xy - vUv;
	vDy = (uTexMatrix * vec4(uv + vec2(0.0, uTexelStep.y), 0.0, 1.0)).xy - vUv;

	gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Passada da profundidade: um quarto da resolucao em cada eixo, um dezesseis
// avos dos pixels.
//
// Antes isto vivia dentro da passada principal, e era caro pelo pior motivo: a
// passada principal tem o dobro dos pixels no modo 3D, entao cinco amostras de
// profundidade viravam cinco amostras em 3840x1080. Num alvo pequeno o mesmo
// trabalho custa um dezesseis avos, e a leitura de volta sai de graca com
// filtragem linear -- que ainda por cima borra o mapa, que e exatamente o que
// se quer.
//
// A separacao paga tres coisas de uma vez: o custo cai, sobra orcamento para
// pistas melhores, e a profundidade passa a ter um lugar proprio -- se um dia
// ela vier de uma rede neural, e este alvo que ela preenche, sem tocar em mais
// nada.
const char *kDepthShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

uniform samplerExternalOES uTexture;
// Profundidade e luminancia do quadro anterior: R e a profundidade ja
// suavizada, G a luminancia, que serve para saber o que ficou parado.
uniform sampler2D uPrev;
uniform int uHasPrev;
uniform float uGroundSign;
uniform float uConvergence;

in vec2 vUv;
in vec2 vDx;
in vec2 vDy;
in vec2 vScreen;
out vec4 fragColor;

void main()
{
	vec3 c = texture(uTexture, vUv).rgb;

	// Amostras largas: e nelas que moram as pistas de cena -- textura contra
	// neblina, cor viva contra cor lavada. Espalhadas de proposito, porque mapa
	// de profundidade com borda dura produz halo em volta de tudo.
	vec3 n = texture(uTexture, vUv - vDy * 8.0).rgb;
	vec3 s = texture(uTexture, vUv + vDy * 8.0).rgb;
	vec3 w = texture(uTexture, vUv - vDx * 8.0).rgb;
	vec3 e = texture(uTexture, vUv + vDx * 8.0).rgb;

	const vec3 media = vec3(0.3333);
	float detalhe = clamp((abs(dot(n - c, media)) + abs(dot(s - c, media))
			+ abs(dot(w - c, media)) + abs(dot(e - c, media))) * 2.0, 0.0, 1.0);

	float mx = max(max(c.r, c.g), c.b);
	float mn = min(min(c.r, c.g), c.b);
	float sat = (mx - mn) / max(mx, 1e-4);

	float chao = clamp(uGroundSign > 0.0 ? 1.0 - vScreen.y : vScreen.y, 0.0, 1.0);

	// A pista de detalhe pesa menos do que pesava.
	//
	// Ela empurra para perto o que tem alta frequencia, e texto e a coisa de
	// maior frequencia que existe num quadro -- entao legenda e HUD saltavam
	// para a frente, separados do fundo. Foi o defeito relatado no primeiro
	// teste, e "o texto fica estranho" e exatamente o que essa pista causa.
	//
	// A rampa do chao pesa conforme haja chao.
	//
	// Ela e a pista mais forte -- "o que esta embaixo da tela esta perto" --, e
	// numa cena de terceira pessoa com o piso enquadrado ela acerta quase
	// sozinha. Numa parede, num close ou num menu de tela cheia nao ha chao
	// nenhum, e ela continuava impondo a mesma inclinacao global: a imagem
	// inteira tombava para tras sem que nada na cena pedisse.
	//
	// O que distingue os dois casos e gradiente vertical. Um piso que se afasta
	// clareia ou escurece de baixo para cima; uma parede chapada, uma cara em
	// close e um fundo de menu nao. Onde nao ha essa variacao a rampa recua
	// para menos da metade da autoridade e as outras duas pistas ficam com o que
	// ela devolveu -- na mesma proporcao entre si, que ja foi ajustada e nao e o
	// que esta em teste aqui.
	float grad_v = abs(dot(s - n, media));
	float peso_chao = mix(0.25, 0.60, smoothstep(0.010, 0.075, grad_v));
	float resto = 1.0 - peso_chao;
	float d = clamp(peso_chao * chao + resto * (0.375 * detalhe + 0.625 * sat),
			0.0, 1.0);

	// Contraste em escala fina, de um texel: e o que separa texto de textura.
	// Parede de tijolo tem detalhe largo; letra tem borda dura de um pixel para
	// o outro. Esta e a unica medida que distingue os dois.
	vec3 fn = texture(uTexture, vUv - vDy).rgb;
	vec3 fs = texture(uTexture, vUv + vDy).rgb;
	vec3 fw = texture(uTexture, vUv - vDx).rgb;
	vec3 fe = texture(uTexture, vUv + vDx).rgb;
	float fino = (abs(dot(fn - c, media)) + abs(dot(fs - c, media))
			+ abs(dot(fw - c, media)) + abs(dot(fe - c, media))) * 0.25;

	float luma = dot(c, vec3(0.299, 0.587, 0.114));
	float parado = 1.0;
	float mudanca = 0.0;
	if(uHasPrev == 1)
	{
		vec4 ant = texture(uPrev, vScreen);
		mudanca = abs(luma - ant.g);
		// Parado e o que nao mudou nada de um quadro para o outro. Num jogo, o
		// mundo sempre se mexe um pouco -- camera, animacao, luz --, e o que
		// fica exatamente igual e interface desenhada por cima.
		parado = 1.0 - smoothstep(0.004, 0.03, mudanca);
	}

	// Interface: borda dura E imovel. Uma coisa so nao basta -- textura fina
	// tambem tem borda dura, e ceu tambem fica parado.
	float conv = fract(uConvergence);
	float subProt = (uConvergence >= 1.0) ? (floor(uConvergence) * 0.01) : 0.0;
	float subMask = (subProt > 0.005) ? smoothstep(subProt, subProt + 0.06, vScreen.y) : 1.0;
	float interface_ = smoothstep(0.05, 0.16, fino) * parado;
	d = mix(d, conv, interface_ * 0.9);
	d = mix(conv, d, subMask);

	if(uHasPrev == 1)
	{
		vec4 ant = texture(uPrev, vScreen);
		// Media exponencial com o quadro anterior. A estimativa treme de quadro
		// a quadro porque as pistas tremem, e tremor de profundidade vira
		// cintilacao -- o tipo de desconforto que faz tirar o headset.
		//
		// O passo e adaptativo: numa cena parada a suavizacao e forte e o mapa
		// fica estavel; num corte de cena, em que a imagem inteira muda, ela
		// afrouxa para o mapa nao arrastar o enquadramento antigo por meio
		// segundo.
		float alfa = mix(0.12, 0.85, smoothstep(0.02, 0.20, mudanca));
		d = mix(ant.r, d, alfa);
	}

	fragColor = vec4(clamp(d, 0.0, 1.0), luma, 0.0, 1.0);
}
)";

const char *kFragmentShader = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

uniform samplerExternalOES uTexture;
uniform float uTargetNits;
uniform int uPq;
uniform float uSharpen;
uniform int uEncode;

// A mesma matriz do vertice. Declarada aqui tambem porque no modo 3D o
// remapeamento acontece por pixel: a coordenada de origem depende de qual
// metade do alvo o pixel ocupa, e isso o vertice nao tem como resolver.
uniform mat4 uTexMatrix;

uniform int uStereo;
// Qual olho esta sendo desenhado: -1 esquerdo, +1 direito.
//
// Antes o lado saia da posicao do pixel dentro de um alvo do dobro da largura.
// Cada olho agora tem alvo proprio, do tamanho da fonte, e o lado deixa de ser
// deduzivel da coordenada -- vem de fora, uma vez por passada.
uniform float uEye;
// Disparidade maxima, em fracao da largura da fonte. O teto vem da distancia
// interpupilar lida do runtime: acima dela os olhos teriam de divergir, o que
// nao e desconforto, e impossivel.
uniform float uStereoStrength;
// Que profundidade fica exatamente no plano da tela. O que estiver a frente
// salta para fora, o que estiver atras afunda.
uniform float uConvergence;
// 1.0 only when ZipDepth is the depth source. Gates the occlusion-aware
// offset-search gather warp; the stock heuristic 3D keeps its original path.
uniform float uAiDepth;
// Lookup table produced once per frame at depth-map resolution by the offset
// search pass. R = left eye, G = right eye, encoded as off/(2*reach) + 0.5.
uniform sampler2D uOffsetTex;
uniform float uLowResWidth;
uniform float uFrameWidth;
uniform float uDispTexels;
// Debug view (Moonlight XR's "Show depth map"): near=red / far=blue instead of video.
uniform float uShowDepth;
uniform float uCurtain;      // 0 = closed, 1 = fully revealed
uniform float uDepthInAlpha; // 1.0 when reading raw GL_RGBA8 [R,G,B,D], 0.0 for bilateral/heuristic .r
uniform float uDepthCubic;   // 1.0 = 4-tap B-spline depth interpolation in AI warp
uniform vec4 uDepthDims;     // xy = active depth texture dimensions, zw = inverse dimensions
uniform float uCornerRadius; // 0.0 = square (360/180), >0 = Apple Vision Pro rounded corners
uniform float uCornerAspect; // cinema screen aspect ratio for circular corners
// Para que lado do quadro fica o chao: +1 quando a borda de baixo do alvo e a
// parte de baixo da cena, -1 quando o compositor inverte a imagem.
// O mapa de profundidade, pronto, vindo da passada pequena. Ler daqui custa
// uma amostra; calcular custava nove, e em dobro de pixels.
uniform sampler2D uDepthTex;

float ReadDepthTex(vec2 p)
{
	vec4 t = texture(uDepthTex, p);
	return (uDepthInAlpha > 0.5) ? t.a : t.r;
}

vec3 splineAxis(float t)
{
	float base = floor(t);
	float f = t - base;
	float f2 = f * f;
	float g0 = 5.0 + f * (-3.0 + f * (-3.0 + 2.0 * f));
	float w1 = 4.0 + f2 * (-6.0 + 3.0 * f);
	return vec3(base - 0.5 + w1 / g0, base + 1.5 + f2 * f / (6.0 - g0), g0 / 6.0);
}

float depthAt(float x, float y, vec2 depthSize, float depthTexelX, vec3 depthRow)
{
	if(uDepthCubic < 0.5)
		return texture(uDepthTex, vec2(x, y)).r;
	vec3 sx = splineAxis(x * depthSize.x - 0.5);
	vec2 hx = sx.xy * depthTexelX;
	float a00 = texture(uDepthTex, vec2(hx.x, depthRow.x)).r;
	float a10 = texture(uDepthTex, vec2(hx.y, depthRow.x)).r;
	float a01 = texture(uDepthTex, vec2(hx.x, depthRow.y)).r;
	float a11 = texture(uDepthTex, vec2(hx.y, depthRow.y)).r;
	return mix(mix(a11, a01, sx.z), mix(a10, a00, sx.z), depthRow.z);
}

in vec2 vUv;
in vec2 vDx;
in vec2 vDy;
in vec2 vScreen;
// Duas saidas: a do swapchain e a do historico da extrapolacao. Escrever nas
// duas na mesma passada custa uma gravacao a mais, e nada perto de uma copia
// de tela inteira depois. Quando so ha um anexo, o glDrawBuffers manda a
// posicao 1 para GL_NONE e a segunda gravacao e descartada.
layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 historyColor;

// Two dark panels part from the vertical centre line. Painting this in the
// video shader means the curtain follows the screen's curvature, FOV and
// aspect ratio exactly - no extra layer or JNI is needed.
vec4 ApplyCurtain(vec3 video)
{
	float open = clamp(uCurtain, 0.0, 1.0);
	vec4 res;
	if(open >= 0.999)
	{
		res = vec4(video, 1.0);
	}
	else
	{
		float d = abs(vScreen.x - 0.5);
		float halfOpen = 0.5 * open;

		// The panels are genuinely TRANSPARENT: alpha 0 lets the compositor show
		// the passthrough / void behind the video. Only the pink seam is opaque.
		float edgeDist = abs(d - halfOpen);
		float seam = 1.0 - smoothstep(0.0, 0.010, edgeDist);
		float glow = 1.0 - smoothstep(0.0, 0.075, edgeDist);
		vec3 pink = vec3(0.984, 0.447, 0.600);
		float line = clamp(seam * 0.95 + glow * 0.22, 0.0, 1.0);

		float reveal = (1.0 - smoothstep(halfOpen - 0.006, halfOpen + 0.006, d))
				* step(0.0005, open);
		res = vec4(mix(pink * (seam * 0.95 + glow * 0.22), video, reveal),
						mix(line, 1.0, reveal));
	}
	if(uCornerRadius > 0.0001)
	{
		float asp = max(uCornerAspect, 0.2);
		vec2 p = (vScreen - 0.5) * vec2(asp, 1.0);
		vec2 halfSize = vec2(0.5 * asp, 0.5) - vec2(uCornerRadius);
		float sdf = length(max(abs(p) - halfSize, 0.0)) - uCornerRadius;
		const float aa = 0.0028;
		float mask = 1.0 - smoothstep(-aa, aa, sdf);
		res.rgb *= mask;
		res.a *= mask;
	}
	return res;
}

// EOTF do SMPTE ST 2084: codigo PQ -> luminancia absoluta, em nits.
vec3 PqToNits(vec3 e)
{
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 p = pow(max(e, vec3(0.0)), vec3(1.0 / m2));
	vec3 num = max(p - c1, vec3(0.0));
	vec3 den = max(c2 - c3 * p, vec3(1e-6));
	return pow(num / den, vec3(1.0 / m1)) * 10000.0;
}

// BT.2020 -> BT.709, em luz linear. O gamut do HDR e mais amplo que o do
// painel; sem esta matriz as cores saem deslocadas, nao so mais claras.
vec3 Bt2020ToBt709(vec3 c)
{
	const mat3 m = mat3(
		 1.6605, -0.1246, -0.0182,
		-0.5876,  1.1329, -0.1006,
		-0.0728, -0.0083,  1.1187);
	return m * c;
}

// Curva do sRGB, para quando o alvo nao a aplica sozinho.
//
// Num swapchain sRGB o proprio GL codifica na escrita, e o shader entrega luz
// linear -- e o caso do compositor. Numa janela comum o alvo e RGBA8 cru:
// entregar luz linear ali deixa a imagem lavada, com as sombras levantadas.
vec3 EncodeSrgb(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
			step(vec3(0.0031308), c));
}

// Nitidez adaptativa ao contraste, no molde do CAS da AMD.
//
// Aplicada no valor codificado, antes de linearizar: a curva PQ (e a gama, no
// caso SDR) ja distribui os passos de forma aproximadamente perceptual, e um
// realce feito ai cresce junto com o que o olho ve. Em luz linear o mesmo
// realce arrancaria halos nas altas luzes e quase nada nas sombras.
//
// ## O que muda em relacao a mascara de desfoque que estava aqui
//
// A mascara fixa realcava tudo com a mesma forca: `c + (c - blur) * ganho`.
// Numa borda de alto contraste -- legenda branca sobre fundo escuro, mira sobre
// o ceu -- isso produz halo, a orla clara de um lado e escura do outro que
// denuncia o filtro. E numa area quase lisa ela realca o ruido do compressor
// junto com o detalhe, porque para ela os dois sao a mesma diferenca.
//
// O CAS resolve os dois com uma conta so: mede o contraste local e **reduz o
// realce onde ele ja e alto**. A borda forte quase nao e tocada, e o detalhe
// fino de baixo contraste -- textura, grama, tecido, que e onde a nitidez
// realmente aparece -- leva o realce inteiro.
//
// O custo e o mesmo. Sao os mesmos cinco toques de textura da mascara anterior:
// o centro e os quatro vizinhos da cruz. O que mudou foi o peso, nao o numero
// de amostras -- e num caminho que ja paga uma passada de GPU, trocar a conta
// sai de graca.
vec3 Sharpen(vec2 uv)
{
	vec3 c = texture(uTexture, uv).rgb;
	if(uSharpen <= 0.0)
		return c;

	vec3 n = texture(uTexture, uv - vDy).rgb;
	vec3 s = texture(uTexture, uv + vDy).rgb;
	vec3 w = texture(uTexture, uv - vDx).rgb;
	vec3 e = texture(uTexture, uv + vDx).rgb;

	vec3 mn = min(min(min(n, s), min(w, e)), c);
	vec3 mx = max(max(max(n, s), max(w, e)), c);

	// Quanta folga existe ate o preto e ate o branco locais.
	//
	// Numa vizinhanca lisa e clara (mn ~ mx ~ 1) isto vai a 1: realce cheio.
	// Numa borda dura (mn ~ 0, mx ~ 1) vai a 0: nao mexe. E a raiz suaviza a
	// transicao entre os dois, para o filtro nao ligar e desligar de um pixel
	// para o outro e criar uma borda que nao existia na imagem.
	vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-4), 0.0, 1.0));

	// O degrau escolhido vive entre os dois extremos do proprio CAS: -1/8 e o
	// realce contido, -1/5 o maximo que ele admite sem passar a doer.
	vec3 peso = vec3(min(min(amp.r, amp.g), amp.b) * -(1.0 / mix(10.0, 6.5, clamp(uSharpen, 0.0, 1.0))));
	vec3 soma = (n + s + w + e) * peso + c;
	return clamp(soma / max(1.0 + 4.0 * peso, vec3(1e-4)), 0.0, 1.0);
}

void main()
{
	// Intro curtain fast path. Covered pixels need no video at all: return
	// before any texture fetch, sharpening or disparity warp. This keeps the
	// reveal smooth while the AI depth delegate compiles in the background.
	float curtainOpen = clamp(uCurtain, 0.0, 1.0);
	if(curtainOpen < 0.999)
	{
		float dCur = abs(vScreen.x - 0.5);
		float halfOpenCur = 0.5 * curtainOpen;
		float revealCur = (1.0 - smoothstep(halfOpenCur - 0.006,
				halfOpenCur + 0.006, dCur)) * step(0.0005, curtainOpen);
		if(revealCur < 0.001)
		{
			float edgeCur = abs(dCur - halfOpenCur);
			float seamCur = 1.0 - smoothstep(0.0, 0.010, edgeCur);
			float glowCur = 1.0 - smoothstep(0.0, 0.075, edgeCur);
			vec3 lineCur = vec3(0.984, 0.447, 0.600)
					* (seamCur * 0.95 + glowCur * 0.22);
			float alphaCur = clamp(seamCur * 0.95 + glowCur * 0.22, 0.0, 1.0);
			fragColor = vec4(lineCur, alphaCur); // alpha 0 = see-through panel
			historyColor = fragColor;
			return;
		}
	}

	// Debug view: perceptual heat map, far = violet/blue -> cyan -> green ->
	// yellow -> orange -> near = deep red. The normalised depth in .r uses
	// 0 = far, 1 = near, the same convention as (d - conv) in the warp below.
	// A plain blue->red lerp only traverses dark purple, so small depth
	// differences are invisible. Turbo's multi-hue ramp plus local contrast
	// makes silhouettes and depth steps readable even though the depth texture
	// is only quarter resolution and bilinearly magnified.
	// Both outputs are written because the extrapolation history attachment is
	// bound on some frames; leaving it undefined would feed a stale colour to
	// the next one.
	if(uShowDepth > 0.5)
	{
		vec2 texel = 1.0 / max(vec2(textureSize(uDepthTex, 0)), vec2(1.0));
		vec2 t2 = texel * 2.0;
		float d  = ReadDepthTex(vScreen);
		float dL = ReadDepthTex(vScreen - vec2(t2.x, 0.0));
		float dR = ReadDepthTex(vScreen + vec2(t2.x, 0.0));
		float dB = ReadDepthTex(vScreen - vec2(0.0, t2.y));
		float dT = ReadDepthTex(vScreen + vec2(0.0, t2.y));
		float mean = (d + dL + dR + dB + dT) * 0.2;

		// Keep the global percentile stretch, add a little overall contrast and
		// amplify high-frequency structure so flat regions do not collapse.
		float base = clamp((d - 0.5) * 1.12 + 0.5, 0.0, 1.0);
		// Avoid the very dark/brown polynomial endpoints: 0.02 is violet-blue
		// (far sky), 0.98 is deep red (nearest surface).
		float vis = clamp(base + (d - mean) * 1.4, 0.02, 0.98);

		// Google Turbo polynomial fit (5th order, perceptual, no lookup table).
		vec4 v4 = vec4(1.0, vis, vis * vis, vis * vis * vis);
		vec2 v2 = v4.zw * v4.z;
		vec3 col = clamp(vec3(
			dot(v4, vec4(0.13572138, 4.61539260, -42.66032258, 132.13108234))
				+ dot(v2, vec2(-152.94239396, 59.28637943)),
			dot(v4, vec4(0.09140261, 2.19418839, 4.84296658, -14.18503333))
				+ dot(v2, vec2(4.27729857, 2.82956604)),
			dot(v4, vec4(0.10667330, 12.64194608, -60.58204836, 110.36276771))
				+ dot(v2, vec2(-89.90310912, 27.34824973))), 0.0, 1.0);

		// Thin dark outline at depth discontinuities: the model's silhouettes
		// are soft, so a gradient-based edge makes them read like the reference
		// depth visualisation instead of a blurry colour wash.
		float grad = max(abs(dR - dL), abs(dT - dB));
		col *= 1.0 - clamp(grad * 4.0, 0.0, 0.55);

		fragColor = ApplyCurtain(col);
		historyColor = vec4(col, 1.0);
		return;
	}
	vec2 uv = vUv;
	if(uStereo == 1)
	{
		// O alvo tem o tamanho da fonte e pertence a um olho so, entao a
		// coordenada da tela ja cobre a imagem inteira: nao ha metade a
		// desdobrar.
		vec2 meia = vScreen;

		// O deslocamento acontece no espaco normalizado, antes da matriz da
		// SurfaceTexture. E o mesmo espaco em que o mapa de profundidade foi
		// desenhado, entao os dois se enderecam pela mesma coordenada -- e a
		// matriz, que carrega recorte e orientacao do buffer, entra uma vez so
		// no fim, sobre o resultado.
		//
		// Meia disparidade para cada olho, e nao a inteira num deles: assim a
		// imagem nao anda para o lado quando o efeito liga, e o erro se reparte
		// entre os dois em vez de pesar todo num.
		float conv = fract(uConvergence);
		float subProt = (uConvergence >= 1.0) ? (floor(uConvergence) * 0.01) : 0.0;
		float subMask = (subProt > 0.005) ? smoothstep(subProt, subProt + 0.06, vScreen.y) : 1.0;

		if(uAiDepth > 0.5)
		{
			// Occlusion-aware gather warp driven by the RGB-guided bilateral
			// depth map (uDepthTex .r). Attenuate disparity & initial offset by
			// edgeW BEFORE Newton solving so border pixels stay monotonic and
			// converge inside [0, 1] without edge stretching.
			float edgeW = smoothstep(0.0, 0.012, min(vScreen.x, 1.0 - vScreen.x));
			float disp = -uEye * 0.5 * uStereoStrength * subMask * edgeW;
			vec2 enc = texture(uOffsetTex, meia).rg;
			float off = (uEye < 0.0 ? enc.r : enc.g) - 0.5;
			float reach = ceil(abs(uDispTexels) * max(conv, 1.0 - conv)) + 2.0;
			float tcx = meia.x + edgeW * off * 2.0 * reach / uLowResWidth;

			float h = 1.0 / uFrameWidth;
			vec2 dSize = uDepthDims.xy;
			float dTexelX = uDepthDims.z;
			vec3 dRow = vec3(meia.y, meia.y, 1.0);
			if(uDepthCubic > 0.5)
			{
				vec3 sy = splineAxis(meia.y * dSize.y - 0.5);
				dRow = vec3(sy.xy * uDepthDims.w, sy.z);
			}
			float stepLim = max(4.0 * h, 0.75 / max(uLowResWidth, 1.0));
			for(int i = 0; i < 2; i++)
			{
				// Iteration 0 locates the root cheaply with bilinear depth; only
				// iteration 1 evaluates the 4-tap B-spline when uDepthCubic is enabled.
				float d0 = (i == 1 && uDepthCubic > 0.5)
						? depthAt(tcx, meia.y, dSize, dTexelX, dRow)
						: texture(uDepthTex, vec2(tcx, meia.y)).r;
				float dm = texture(uDepthTex, vec2(tcx - h, meia.y)).r;
				float dp = texture(uDepthTex, vec2(tcx + h, meia.y)).r;
				float e = (tcx - meia.x) + disp * (d0 - conv);
				// Clamp negative occlusion wall slopes positively so Newton never
				// steps backwards across steep foreground silhouettes at high 3D.
				float slope = max(1.0 + disp * (dp - dm) / (2.0 * h), 0.25);
				tcx -= clamp(e / slope, -stepLim, stepLim);
			}

			tcx = clamp(tcx, 0.5 / uFrameWidth, 1.0 - 0.5 / uFrameWidth);
			meia.x = tcx;
		}
		else
		{
			// Stock heuristic or un-bilateralized fallback depth source.
			float d = ReadDepthTex(meia);
			vec2 depthTexel = uDepthDims.zw;
			float dL = ReadDepthTex(meia - vec2(depthTexel.x, 0.0));
			float dR = ReadDepthTex(meia + vec2(depthTexel.x, 0.0));
			float dB = ReadDepthTex(meia - vec2(0.0, depthTexel.y));
			float dT = ReadDepthTex(meia + vec2(0.0, depthTexel.y));
			float grad = max(abs(dR - dL), abs(dT - dB));
			float edgeFade = 1.0 - smoothstep(0.02, 0.12, grad);
			meia.x += uEye * 0.5 * uStereoStrength * subMask * (d - conv) * edgeFade;
		}
		uv = (uTexMatrix * vec4(meia, 0.0, 1.0)).xy;
	}
	else if(uStereo == 2)
	{
		// SBS: Left eye (uEye < 0.0) -> [0.0, 0.5], Right eye (uEye > 0.0) -> [0.5, 1.0]
		vec2 meia = vScreen;
		meia.x = (uEye < 0.0) ? (meia.x * 0.5) : (0.5 + meia.x * 0.5);
		uv = (uTexMatrix * vec4(meia, 0.0, 1.0)).xy;
	}
	else if(uStereo == 3)
	{
		// OU: Top half (uEye < 0.0) -> [0.0, 0.5], Bottom half (uEye > 0.0) -> [0.5, 1.0]
		vec2 meia = vScreen;
		meia.y = (uEye < 0.0) ? (meia.y * 0.5) : (0.5 + meia.y * 0.5);
		uv = (uTexMatrix * vec4(meia, 0.0, 1.0)).xy;
	}

	vec3 rgb = Sharpen(uv);

	if(uPq == 0)
	{
		// Fonte SDR num alvo que nao reencoda: linearizar aqui so para o alvo
		// reencodar em seguida e ida e volta pela mesma curva. Nesse caso o
		// valor sai como veio, ja com a nitidez aplicada.
		if(uEncode == 1)
		{
			vec3 shown = clamp(rgb, 0.0, 1.0);
			fragColor = ApplyCurtain(shown);
			historyColor = vec4(shown, 1.0);
			return;
		}
		// Fonte SDR: so linearizar a curva de 8 bits, sem mapear nada.
		vec3 linearRgb = pow(max(rgb, vec3(0.0)), vec3(2.2));
		fragColor = ApplyCurtain(linearRgb);
		historyColor = vec4(linearRgb, 1.0);
		return;
	}

	vec3 nits = PqToNits(rgb);

	// Reinhard estendido, normalizado pelo alvo. Comprime as altas luzes em vez
	// de cortar: cortar em 1.0 transformaria todo destaque num borrao branco
	// chapado, que e justamente o defeito que se quer eliminar.
	vec3 scaled = nits / uTargetNits;
	const float white = 4.0;
	vec3 mapped = scaled * (1.0 + scaled / (white * white)) / (1.0 + scaled);

	vec3 rec709 = clamp(Bt2020ToBt709(mapped), 0.0, 1.0);
	vec3 finalRgb = uEncode == 1 ? EncodeSrgb(rec709) : rec709;
	fragColor = ApplyCurtain(finalRgb);
	historyColor = vec4(finalRgb, 1.0);
}
)";

const char *kBilateralVertex = R"(#version 300 es
in vec2 aPos;
out vec2 vScreen;
void main()
{
	vScreen = aPos * 0.5 + 0.5;
	gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char *kBilateralFragment = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in highp vec2 vScreen;
uniform samplerExternalOES uTexture;
uniform sampler2D uDepthTex;
uniform highp mat4 uTexMatrix;
uniform highp vec2 uDepthSize;
uniform float uSigmaR;
uniform float uSharp;
out vec4 fragColor;
void main()
{
	// 4-tap rotated-grid supersampling filters high-res un-mipmapped OES video
	// down to bilateral resolution so diagonal character contours never alias.
	highp vec2 du = dFdx(vScreen);
	highp vec2 dv = dFdy(vScreen);
	vec3 hi = 0.25 * (
		texture(uTexture, (uTexMatrix * vec4(vScreen + du * 0.35 + dv * 0.15, 0.0, 1.0)).xy).rgb +
		texture(uTexture, (uTexMatrix * vec4(vScreen - du * 0.35 - dv * 0.15, 0.0, 1.0)).xy).rgb +
		texture(uTexture, (uTexMatrix * vec4(vScreen - du * 0.15 + dv * 0.35, 0.0, 1.0)).xy).rgb +
		texture(uTexture, (uTexMatrix * vec4(vScreen + du * 0.15 - dv * 0.35, 0.0, 1.0)).xy).rgb);
	highp vec2 lp = vScreen * uDepthSize - 0.5;
	ivec2 base = ivec2(floor(lp));
	ivec2 maxCoord = max(ivec2(uDepthSize) - 1, ivec2(0));
	const float SIGMA_S = 1.5;
	const float kLog2E = 1.44269504;
	const float invTwoSigmaS2Log2 = kLog2E / (2.0 * SIGMA_S * SIGMA_S);
	float invTwoSigmaR2Log2 = kLog2E / max(2.0 * uSigmaR * uSigmaR, 1e-6);
	float wSum = 0.0;
	float dSum = 0.0;
	float dLo = 1.0;
	float dHi = 0.0;
	for(int dy = -2; dy <= 2; dy++)
	{
		for(int dx = -2; dx <= 2; dx++)
		{
			if(abs(dx) + abs(dy) > 2)
				continue;
			ivec2 q = clamp(base + ivec2(dx, dy), ivec2(0), maxCoord);
			vec4 s = texelFetch(uDepthTex, q, 0);
			vec2 off = vec2(q) - lp;
			vec3 cd = hi - s.rgb;
			float w = exp2(-(dot(off, off) * invTwoSigmaS2Log2 + dot(cd, cd) * invTwoSigmaR2Log2));
			wSum += w;
			dSum += w * s.a;
			dLo = min(dLo, s.a);
			dHi = max(dHi, s.a);
		}
	}
	float d = (wSum > 1e-5)
			? (dSum / wSum)
			: texelFetch(uDepthTex, clamp(ivec2(round(lp)), ivec2(0), maxCoord), 0).a;
	float span = dHi - dLo;
	float edgeAmt = uSharp * smoothstep(0.08, 0.20, span);
	if(edgeAmt > 1e-4)
	{
		float u = clamp((d - dLo) / max(span, 1e-5), 0.0, 1.0);
		float snapped = dLo + span / (1.0 + exp2(-14.42695 * (u - 0.5)));
		d = mix(d, snapped, edgeAmt);
	}
	fragColor = vec4(d, d, d, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	if(shader == 0)
		return 0;
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(ok != GL_TRUE)
	{
		// O log do driver e a unica pista util quando um shader nao compila no
		// hardware, e ele nao aparece em lugar nenhum se nao for lido aqui.
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		std::string info((size_t)(len > 0 ? len : 1), '\0');
		glGetShaderInfoLog(shader, len, nullptr, &info[0]);
		LOGE("Failed to compile shader: %s", info.c_str());
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

} // namespace

void SetCurtainDurationMs(int32_t ms)
{
	g_curtain_duration_ms.store(std::clamp(ms, 200, 10000), std::memory_order_release);
}

int32_t CurtainDurationMs()
{
	return g_curtain_duration_ms.load(std::memory_order_acquire);
}

void SetAiEdgeEnhance(float sharp, bool cubic)
{
	g_ai_edge_sharp.store(std::clamp(sharp, 0.0f, 1.0f), std::memory_order_release);
	g_ai_depth_cubic.store(cubic, std::memory_order_release);
	LOGI("AI edge enhance: sharp=%.2f cubic=%d", g_ai_edge_sharp.load(), (int)cubic);
}

float AiEdgeSharp()
{
	return g_ai_edge_sharp.load(std::memory_order_acquire);
}

bool AiDepthCubic()
{
	return g_ai_depth_cubic.load(std::memory_order_acquire);
}

float GetLastWarpGpuMs()
{
	return g_warp_gpu_ms.load(std::memory_order_acquire);
}

void SetCurrentDisplayRefreshHz(float hz)
{
	if(hz >= 50.0f && hz <= 240.0f)
		g_display_refresh_hz.store(hz, std::memory_order_release);
}

float GetCurrentDisplayRefreshHz()
{
	return g_display_refresh_hz.load(std::memory_order_acquire);
}

void RecordDisplayPacingSample(bool missed)
{
	const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	std::lock_guard<std::mutex> lock(g_pacing_mtx);
	if(g_pacing_win_start_ns == 0)
		g_pacing_win_start_ns = nowNs;
	g_pacing_total++;
	if(missed)
		g_pacing_missed++;
	if(g_pacing_total >= 40 && (nowNs - g_pacing_win_start_ns) >= 1500000000LL)
	{
		const float pct = 100.0f * (float)g_pacing_missed / (float)g_pacing_total;
		g_missed_refresh_pct.store(pct, std::memory_order_release);
		g_pacing_total = 0;
		g_pacing_missed = 0;
		g_pacing_win_start_ns = nowNs;
	}
}

void ResetDisplayPacing()
{
	std::lock_guard<std::mutex> lock(g_pacing_mtx);
	g_pacing_total = 0;
	g_pacing_missed = 0;
	g_pacing_win_start_ns = 0;
	g_missed_refresh_pct.store(-1.0f, std::memory_order_release);
}

float GetMissedRefreshPct()
{
	return g_missed_refresh_pct.load(std::memory_order_acquire);
}

bool ToneMapper::CompileProgram()
{
	GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	if(vs == 0)
		return false;
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
	if(fs == 0)
	{
		glDeleteShader(vs);
		return false;
	}

	program_ = glCreateProgram();
	glAttachShader(program_, vs);
	glAttachShader(program_, fs);
	glBindAttribLocation(program_, 0, "aPos");
	glLinkProgram(program_);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = GL_FALSE;
	glGetProgramiv(program_, GL_LINK_STATUS, &ok);
	if(ok != GL_TRUE)
	{
		GLint len = 0;
		glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &len);
		std::string info((size_t)(len > 0 ? len : 1), '\0');
		glGetProgramInfoLog(program_, len, nullptr, &info[0]);
		LOGE("Failed to link program: %s", info.c_str());
		glDeleteProgram(program_);
		program_ = 0;
		return false;
	}

	loc_tex_matrix_ = glGetUniformLocation(program_, "uTexMatrix");
	loc_sampler_ = glGetUniformLocation(program_, "uTexture");
	loc_pq_ = glGetUniformLocation(program_, "uPq");
	loc_target_nits_ = glGetUniformLocation(program_, "uTargetNits");
	loc_sharpen_ = glGetUniformLocation(program_, "uSharpen");
	loc_texel_step_ = glGetUniformLocation(program_, "uTexelStep");
	loc_encode_ = glGetUniformLocation(program_, "uEncode");
	loc_stereo_ = glGetUniformLocation(program_, "uStereo");
	loc_eye_ = glGetUniformLocation(program_, "uEye");
	loc_stereo_strength_ = glGetUniformLocation(program_, "uStereoStrength");
	loc_convergence_ = glGetUniformLocation(program_, "uConvergence");
	loc_ai_depth_ = glGetUniformLocation(program_, "uAiDepth");
	loc_depth_tex_ = glGetUniformLocation(program_, "uDepthTex");
	loc_offset_tex_ = glGetUniformLocation(program_, "uOffsetTex");
	loc_lowres_width_ = glGetUniformLocation(program_, "uLowResWidth");
	loc_frame_width_ = glGetUniformLocation(program_, "uFrameWidth");
	loc_disp_texels_ = glGetUniformLocation(program_, "uDispTexels");
	loc_show_depth_ = glGetUniformLocation(program_, "uShowDepth");
	loc_curtain_ = glGetUniformLocation(program_, "uCurtain");
	loc_depth_in_alpha_ = glGetUniformLocation(program_, "uDepthInAlpha");
	loc_depth_cubic_ = glGetUniformLocation(program_, "uDepthCubic");
	loc_depth_dims_ = glGetUniformLocation(program_, "uDepthDims");
	loc_corner_radius_ = glGetUniformLocation(program_, "uCornerRadius");
	loc_corner_aspect_ = glGetUniformLocation(program_, "uCornerAspect");

	// O programa da profundidade: mesmo vertice, fragmento proprio.
	//
	// Falhar aqui nao derruba o resto -- o 3D fica sem mapa e o video continua
	// normal, que e muito melhor do que a sessao nao abrir por causa de um
	// recurso opcional.
	GLuint dvs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
	GLuint dfs = dvs ? CompileShader(GL_FRAGMENT_SHADER, kDepthShader) : 0;
	if(dvs && dfs)
	{
		depth_program_ = glCreateProgram();
		glAttachShader(depth_program_, dvs);
		glAttachShader(depth_program_, dfs);
		glBindAttribLocation(depth_program_, 0, "aPos");
		glLinkProgram(depth_program_);
		GLint dok = GL_FALSE;
		glGetProgramiv(depth_program_, GL_LINK_STATUS, &dok);
		if(dok != GL_TRUE)
		{
			GLint len = 0;
			glGetProgramiv(depth_program_, GL_INFO_LOG_LENGTH, &len);
			std::string info((size_t)(len > 0 ? len : 1), '\0');
			glGetProgramInfoLog(depth_program_, len, nullptr, &info[0]);
			LOGE("Failed to link the depth program: %s", info.c_str());
			glDeleteProgram(depth_program_);
			depth_program_ = 0;
		}
		else
		{
			dloc_tex_matrix_ = glGetUniformLocation(depth_program_, "uTexMatrix");
			dloc_texel_step_ = glGetUniformLocation(depth_program_, "uTexelStep");
			dloc_sampler_ = glGetUniformLocation(depth_program_, "uTexture");
			dloc_prev_ = glGetUniformLocation(depth_program_, "uPrev");
			dloc_has_prev_ = glGetUniformLocation(depth_program_, "uHasPrev");
			dloc_ground_sign_ = glGetUniformLocation(depth_program_, "uGroundSign");
			dloc_convergence_ = glGetUniformLocation(depth_program_, "uConvergence");
		}
	}
	if(dvs)
		glDeleteShader(dvs);
	if(dfs)
		glDeleteShader(dfs);
	return true;
}

bool ToneMapper::Init()
{
	if(!CompileProgram())
		return false;

	glGenTextures(1, &external_tex_);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_tex_);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

	glGenFramebuffers(1, &fbo_);

	// Dois triangulos cobrindo o clip inteiro. Sem indices e sem atributo de
	// UV: a coordenada sai da posicao no vertex shader.
	const float quad[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f,
	};
	glGenVertexArrays(1, &vao_);
	glBindVertexArray(vao_);
	glGenBuffers(1, &vbo_);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	glBindVertexArray(0);

	InitGpuTimers();
	LOGI("Tone mapper ready (external texture %u, gpuTimer=%d)",
			external_tex_, gpu_timer_supported_ ? 1 : 0);
	return true;
}

void ToneMapper::InitGpuTimers()
{
	DestroyGpuTimers();
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	bool present = false;
	for(GLint i = 0; i < count && !present; i++)
	{
		const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		present = name && strcmp(name, "GL_EXT_disjoint_timer_query") == 0;
	}
	if(!present)
	{
		LOGW("GL_EXT_disjoint_timer_query missing, GPU times unavailable");
		return;
	}
	fn_gen_queries_ = (GenQueriesExtFn)eglGetProcAddress("glGenQueriesEXT");
	fn_delete_queries_ = (DeleteQueriesExtFn)eglGetProcAddress("glDeleteQueriesEXT");
	fn_begin_query_ = (BeginQueryExtFn)eglGetProcAddress("glBeginQueryEXT");
	fn_end_query_ = (EndQueryExtFn)eglGetProcAddress("glEndQueryEXT");
	fn_get_query_uiv_ = (GetQueryObjectuivExtFn)eglGetProcAddress("glGetQueryObjectuivEXT");
	fn_get_query_ui64v_ = (GetQueryObjectui64vExtFn)eglGetProcAddress("glGetQueryObjectui64vEXT");
	if(!fn_gen_queries_ || !fn_delete_queries_ || !fn_begin_query_
			|| !fn_end_query_ || !fn_get_query_uiv_ || !fn_get_query_ui64v_)
	{
		LOGW("GL_EXT_disjoint_timer_query entry points incomplete, GPU times unavailable");
		DestroyGpuTimers();
		return;
	}
	while(glGetError() != GL_NO_ERROR) { }
	fn_gen_queries_(kGpuTimerCount, gpu_timer_queries_);
	const GLenum err = glGetError();
	bool ok = (err == GL_NO_ERROR);
	for(int i = 0; i < kGpuTimerCount; i++)
	{
		if(gpu_timer_queries_[i] == 0)
			ok = false;
		gpu_timer_pending_[i] = false;
		gpu_timer_wait_[i] = 0;
	}
	if(!ok)
	{
		LOGW("glGenQueriesEXT failed (err=0x%x), GPU times unavailable", (unsigned)err);
		DestroyGpuTimers();
		return;
	}
	gpu_timer_write_ = 0;
	gpu_timer_active_ = false;
	gpu_timer_supported_ = true;
	gpu_timer_window_ns_ = 0;
	gpu_timer_window_samples_ = 0;
	gpu_timer_sample_seq_ = 0;
	gpu_timer_window_start_ = std::chrono::steady_clock::now();
}

void ToneMapper::DestroyGpuTimers()
{
	if(gpu_timer_active_ && fn_end_query_)
	{
		fn_end_query_(kGlTimeElapsedExt);
	}
	gpu_timer_active_ = false;
	if(fn_delete_queries_)
	{
		bool hasAny = false;
		for(int i = 0; i < kGpuTimerCount; i++)
		{
			if(gpu_timer_queries_[i] != 0) hasAny = true;
		}
		if(hasAny)
			fn_delete_queries_(kGpuTimerCount, gpu_timer_queries_);
	}
	for(int i = 0; i < kGpuTimerCount; i++)
	{
		gpu_timer_queries_[i] = 0;
		gpu_timer_pending_[i] = false;
		gpu_timer_wait_[i] = 0;
	}
	gpu_timer_write_ = 0;
	gpu_timer_sample_seq_ = 0;
	gpu_timer_supported_ = false;
	fn_gen_queries_ = nullptr;
	fn_delete_queries_ = nullptr;
	fn_begin_query_ = nullptr;
	fn_end_query_ = nullptr;
	fn_get_query_uiv_ = nullptr;
	fn_get_query_ui64v_ = nullptr;
	g_warp_gpu_ms.store(-1.0f, std::memory_order_release);
}

void ToneMapper::HarvestGpuTimers()
{
	if(!gpu_timer_supported_)
		return;
	bool anyPending = false;
	for(int i = 0; i < kGpuTimerCount; i++)
	{
		if(gpu_timer_pending_[i])
		{
			anyPending = true;
			break;
		}
	}
	if(!anyPending)
		return;

	GLint disjoint = 0;
	glGetIntegerv(kGlGpuDisjointExt, &disjoint);
	if(disjoint != 0)
	{
		for(int i = 0; i < kGpuTimerCount; i++)
		{
			gpu_timer_pending_[i] = false;
			gpu_timer_wait_[i] = 0;
		}
		return;
	}

	for(int k = 0; k < kGpuTimerCount; k++)
	{
		const int idx = (gpu_timer_write_ + k) % kGpuTimerCount;
		if(!gpu_timer_pending_[idx])
			continue;
		GLuint avail = 0;
		fn_get_query_uiv_(gpu_timer_queries_[idx], kGlQueryResultAvailableExt, &avail);
		if(avail == 0)
		{
			gpu_timer_wait_[idx]++;
			if(gpu_timer_wait_[idx] > 90)
			{
				gpu_timer_pending_[idx] = false;
				gpu_timer_wait_[idx] = 0;
				if(!logged_timer_stuck_)
				{
					logged_timer_stuck_ = true;
					LOGW("GPU timer query stalled (>90 frames); resetting slot");
				}
			}
			continue;
		}
		GLuint64 elapsedNs = 0;
		fn_get_query_ui64v_(gpu_timer_queries_[idx], kGlQueryResultExt, &elapsedNs);
		gpu_timer_pending_[idx] = false;
		gpu_timer_wait_[idx] = 0;
		if(elapsedNs > 0 && elapsedNs <= 50000000ULL)
		{
			const float ms = (float)elapsedNs / 1000000.0f;
			const float prev = g_warp_gpu_ms.load(std::memory_order_relaxed);
			const float next = (prev < 0.0f) ? ms : (prev * 0.80f + ms * 0.20f);
			g_warp_gpu_ms.store(next, std::memory_order_release);
			gpu_timer_window_ns_ += (int64_t)elapsedNs;
			gpu_timer_window_samples_++;
		}
	}

	const auto now = std::chrono::steady_clock::now();
	const int64_t winMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - gpu_timer_window_start_).count();
	if(winMs >= 3000 && gpu_timer_window_samples_ > 0)
	{
		const double avgMs = (double)gpu_timer_window_ns_
				/ (double)gpu_timer_window_samples_ / 1000000.0;
		if(stereo_mode_ == 1)
		{
			LOGI("XR warp GPU: avg=%.2fms ema=%.2fms (%d samples) | hz=%.0f missed=%.1f%% cubic=%d sharp=%.2f",
					avgMs, (double)g_warp_gpu_ms.load(std::memory_order_relaxed),
					gpu_timer_window_samples_, (double)GetCurrentDisplayRefreshHz(),
					(double)GetMissedRefreshPct(),
					(int)AiDepthCubic(), (double)AiEdgeSharp());
		}
		gpu_timer_window_ns_ = 0;
		gpu_timer_window_samples_ = 0;
		gpu_timer_window_start_ = now;
	}
}

bool ToneMapper::BeginGpuTimer()
{
	if(!gpu_timer_supported_ || gpu_timer_active_)
		return false;
	if(gpu_timer_pending_[gpu_timer_write_])
		return false;
	fn_begin_query_(kGlTimeElapsedExt, gpu_timer_queries_[gpu_timer_write_]);
	gpu_timer_active_ = true;
	return true;
}

void ToneMapper::EndGpuTimer()
{
	if(!gpu_timer_active_)
		return;
	fn_end_query_(kGlTimeElapsedExt);
	gpu_timer_active_ = false;
	gpu_timer_pending_[gpu_timer_write_] = true;
	gpu_timer_wait_[gpu_timer_write_] = 0;
	gpu_timer_write_ = (gpu_timer_write_ + 1) % kGpuTimerCount;
}

bool ToneMapper::EnableExtrapolation(GLenum format)
{
	if(extrapolate_fn_)
		return true;

	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	bool present = false;
	for(GLint i = 0; i < count && !present; i++)
	{
		const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		present = name && strcmp(name, "GL_QCOM_frame_extrapolation") == 0;
	}
	if(!present)
	{
		LOGW("GL_QCOM_frame_extrapolation unavailable; no extrapolated frames");
		return false;
	}

	auto fn = (ExtrapolateFn)eglGetProcAddress("glExtrapolateTex2DQCOM");
	if(!fn)
	{
		// Extensao anunciada e ponteiro ausente acontece, e e pior que ausencia
		// declarada: a checagem passa e a chamada nao existe.
		LOGW("glExtrapolateTex2DQCOM did not resolve despite the extension being advertised");
		return false;
	}
	extrapolate_fn_ = fn;
	history_format_ = format;
	LOGI("Frame extrapolation available (history format 0x%x)",
			(unsigned)format);
	return true;
}

/**
 * O historico e por olho.
 *
 * A extrapolacao preve o proximo quadro a partir dos dois anteriores DAQUELA
 * imagem. No 3D as duas imagens sao diferentes -- e essa diferenca e o efeito
 * inteiro --, entao um historico compartilhado preveria o olho direito a partir
 * do esquerdo e o erro apareceria como profundidade tremendo, que e exatamente
 * o desconforto que se quer evitar.
 */
bool ToneMapper::EnsureHistory(int32_t width, int32_t height, int slot)
{
	GLuint *hist = history_[slot];
	if(hist[0] != 0 && history_width_[slot] == width && history_height_[slot] == height)
		return true;

	if(hist[0] != 0)
		glDeleteTextures(2, hist);
	hist[0] = hist[1] = 0;
	history_newest_[slot] = -1;
	history_count_[slot] = 0;

	while(glGetError() != GL_NO_ERROR) { }
	glGenTextures(2, hist);
	for(int i = 0; i < 2; i++)
	{
		glBindTexture(GL_TEXTURE_2D, hist[i]);
		// Armazenamento imutavel: o formato tem de bater exatamente com o do
		// swapchain, e glTexStorage2D e o que garante isso sem depender de o
		// driver escolher um formato interno equivalente por conta propria.
		glTexStorage2D(GL_TEXTURE_2D, 1, history_format_, width, height);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	history_width_[slot] = width;
	history_height_[slot] = height;
	// Mesmo cuidado: sem esvaziar a fila antes, um erro alheio seria cobrado
	// desta funcao.
	return glGetError() == GL_NO_ERROR;
}

bool ToneMapper::Attach(ASurfaceTexture *texture)
{
	surface_texture_ = texture;
	if(!surface_texture_ || external_tex_ == 0)
		return false;
	if(ASurfaceTexture_attachToGLContext(surface_texture_, external_tex_) != 0)
	{
		LOGE("ASurfaceTexture_attachToGLContext failed");
		return false;
	}
	attached_ = true;
	return true;
}

/**
 * Cria (ou redimensiona) o alvo pequeno da profundidade.
 *
 * Um quarto da resolucao em cada eixo. Nao e economia cega: o mapa que
 * queremos e borrado de proposito -- borda dura nele produz halo em volta de
 * cada objeto --, entao resolucao baixa e leitura com filtragem linear dao de
 * graca a suavizacao que de outro modo custaria amostras.
 *
 * Duas texturas, em rodizio: a passada le a anterior para suavizar no tempo e
 * para saber o que ficou parado, e escreve na outra. Ler e escrever a mesma
 * textura na mesma passada e comportamento indefinido em GL.
 */
/**
 * A textura em que a janela e desenhada antes de ir para a tela.
 *
 * Recriada quando a janela muda de tamanho, e so entao: alocar por quadro
 * seria pagar uma textura de tela inteira sessenta vezes por segundo.
 */
/** A textura de um pixel, criada uma vez e nunca mais. Ver Render(). */
GLuint ToneMapper::EnsureDummyTexture()
{
	if(dummy_tex_ != 0)
		return dummy_tex_;
	const uint8_t preto[4] = { 0, 0, 0, 255 };
	glGenTextures(1, &dummy_tex_);
	glBindTexture(GL_TEXTURE_2D, dummy_tex_);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, preto);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return dummy_tex_;
}

bool ToneMapper::EnsureWindowTarget(int32_t width, int32_t height)
{
	if(window_tex_ != 0 && window_w_ == width && window_h_ == height)
		return true;
	if(window_tex_ != 0)
	{
		glDeleteTextures(1, &window_tex_);
		window_tex_ = 0;
	}
	if(width <= 0 || height <= 0)
		return false;

	glGenTextures(1, &window_tex_);
	glBindTexture(GL_TEXTURE_2D, window_tex_);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	window_w_ = width;
	window_h_ = height;
	LOGI("Window target texture created: %dx%d", width, height);
	return true;
}

/**
 * O mapa de profundidade guarda meia precisao quando o aparelho deixa.
 *
 * Em RGBA8 a media exponencial do fim da passada trava. `mix(ant, d, alfa)` com
 * `alfa` em 0,12 -- que e o valor de cena parada, justamente onde se quer
 * estabilidade -- anda 0,12 da diferenca, e num alvo de 8 bits qualquer passo
 * menor que meio nivel arredonda de volta ao ponto de partida. Diferencas
 * abaixo de cerca de 4/255 nunca convergem: em vez de chegar devagar, o mapa
 * simplesmente para onde esta.
 *
 * A magnitude e pequena -- 1,6% da faixa, subpixel depois do teto de
 * disparidade --, entao isto e acabamento e nao conserto. Mas o alvo tem um
 * dezesseis avos dos pixels e a troca custa so o dobro por pixel dele.
 *
 * Duas condicoes, e nao uma: alem de o formato ser renderizavel, ele precisa
 * aceitar filtragem linear. Sem `OES_texture_half_float_linear` a textura fica
 * incompleta na leitura e devolve preto -- e preto aqui nao da erro, da um mapa
 * de profundidade zerado, que e um sintoma mudo. Por isso a extensao e conferida
 * antes, e nao depois.
 */
void ToneMapper::DepthTargetSize(int32_t width, int32_t height,
		int32_t *outW, int32_t *outH) const
{
	if(ai_mode_.load())
	{
		GetAiOutputTargetDimensions(outW, outH);
		return;
	}
	int32_t w = width > 4 ? width / 4 : 1;
	int32_t h = height > 4 ? height / 4 : 1;
	*outW = w;
	*outH = h;
}

bool ToneMapper::EnsureDepthTargets(int32_t width, int32_t height)
{
	const bool forAi = ai_mode_.load();
	int32_t w = 1, h = 1;
	DepthTargetSize(width, height, &w, &h);
	if(depth_tex_[0] != 0 && depth_width_ == w && depth_height_ == h
			&& depth_for_ai_ == forAi)
		return true;

	std::lock_guard<std::mutex> targetLock(stage_targets_mutex_);
	stage_targets_ = StageTargets{};
	stage_published_dest_.store(-1);
	stage_published_ns_.store(0);
	GLsync published = stage_fence_.exchange(nullptr);
	if(published != nullptr)
	{
		GLsync retired = stage_retired_fence_.exchange(published);
		if(retired != nullptr) glDeleteSync(retired);
	}
	if(depth_tex_[0] != 0)
		glDeleteTextures(2, depth_tex_);
	depth_tex_[0] = depth_tex_[1] = 0;
	depth_has_prev_ = false;
	stage_neutral_tex_ = 0;
	bilateral_built_upload_ns_ = -1;

	if(depth_fbo_ == 0)
		glGenFramebuffers(1, &depth_fbo_);

	bool meia_precisao = false;
	if(!forAi)
	{
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		bool renderizavel = false;
		bool filtravel = false;
		for(GLint i = 0; i < count; i++)
		{
			const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
			if(!name)
				continue;
			if(strcmp(name, "GL_EXT_color_buffer_half_float") == 0
					|| strcmp(name, "GL_EXT_color_buffer_float") == 0)
				renderizavel = true;
			else if(strcmp(name, "GL_OES_texture_half_float_linear") == 0)
				filtravel = true;
		}
		meia_precisao = renderizavel && filtravel;
	}

	const GLenum candidatos[2] = { GL_RGBA16F, GL_RGBA8 };
	GLenum escolhido = 0;
	const int startCandidate = (!forAi && meia_precisao) ? 0 : 1;
	for(int c = startCandidate; c < 2 && escolhido == 0; c++)
	{
		while(glGetError() != GL_NO_ERROR) { }
		glGenTextures(2, depth_tex_);
		for(int i = 0; i < 2; i++)
		{
			glBindTexture(GL_TEXTURE_2D, depth_tex_[i]);
			glTexStorage2D(GL_TEXTURE_2D, 1, candidatos[c], w, h);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		glBindTexture(GL_TEXTURE_2D, 0);

		GLint anterior = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &anterior);
		glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo_);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				depth_tex_[0], 0);
		const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
				&& glGetError() == GL_NO_ERROR;
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)anterior);

		if(ok)
			escolhido = candidatos[c];
		else
		{
			glDeleteTextures(2, depth_tex_);
			depth_tex_[0] = depth_tex_[1] = 0;
		}
	}

	if(escolhido == 0)
	{
		if(!logged_depth_failure_)
		{
			logged_depth_failure_ = true;
			LOGE("No renderable format for the depth target; 3D will have no depth");
		}
		return false;
	}

	ai_depth_format_ = escolhido;
	depth_width_ = w;
	depth_height_ = h;
	depth_for_ai_ = forAi;
	if(!logged_depth_ || forAi)
	{
		logged_depth_ = true;
		LOGI("Depth target (%s): %dx%d (video %dx%d), format %s",
				forAi ? "AI [R,G,B,D]" : "heuristic",
				w, h, width, height, escolhido == GL_RGBA16F ? "RGBA16F" : "RGBA8");
	}
	return true;
}

bool ToneMapper::CompileBilateralProgram()
{
	if(bilateral_program_ != 0)
		return true;
	GLuint vs = CompileShader(GL_VERTEX_SHADER, kBilateralVertex);
	GLuint fs = vs ? CompileShader(GL_FRAGMENT_SHADER, kBilateralFragment) : 0;
	if(vs == 0 || fs == 0)
	{
		if(vs) glDeleteShader(vs);
		if(fs) glDeleteShader(fs);
		return false;
	}
	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glBindAttribLocation(p, 0, "aPos");
	glLinkProgram(p);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = GL_FALSE;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if(ok != GL_TRUE)
	{
		GLint len = 0;
		glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
		std::string info((size_t)(len > 0 ? len : 1), '\0');
		glGetProgramInfoLog(p, len, nullptr, &info[0]);
		LOGE("Bilateral upsample link failed: %s", info.c_str());
		glDeleteProgram(p);
		return false;
	}
	bilateral_program_ = p;
	bloc_tex_matrix_ = glGetUniformLocation(p, "uTexMatrix");
	bloc_sampler_ = glGetUniformLocation(p, "uTexture");
	bloc_depth_tex_ = glGetUniformLocation(p, "uDepthTex");
	bloc_depth_size_ = glGetUniformLocation(p, "uDepthSize");
	bloc_sigma_r_ = glGetUniformLocation(p, "uSigmaR");
	bloc_sharp_ = glGetUniformLocation(p, "uSharp");
	LOGI("Joint bilateral upsample pass ready");
	return true;
}

void ToneMapper::BilateralTargetSize(int32_t eyeWidth, int32_t eyeHeight,
		int32_t *outW, int32_t *outH) const
{
	// Keep bilateral + offset long edge within [512, 768] so 1080p is never
	// shrunk below the 512x288 AI model output (avoiding jagged silhouettes at
	// high disparity) while 4K/8K caps cleanly at 768x432 instead of 1920x1080.
	constexpr int32_t kMinBilateralLongEdge = 512;
	constexpr int32_t kMaxBilateralLongEdge = 768;
	int32_t w = std::max(8, eyeWidth >> 2);
	int32_t h = std::max(8, eyeHeight >> 2);
	const int32_t longEdge = std::max(w, h);
	if(longEdge < kMinBilateralLongEdge)
	{
		const float scale = (float)kMinBilateralLongEdge / (float)std::max(8, longEdge);
		w = std::max(8, ((int32_t)std::lround((float)w * scale)) & ~1);
		h = std::max(8, ((int32_t)std::lround((float)h * scale)) & ~1);
	}
	else if(longEdge > kMaxBilateralLongEdge)
	{
		const float scale = (float)kMaxBilateralLongEdge / (float)longEdge;
		w = std::max(8, ((int32_t)std::lround((float)w * scale)) & ~1);
		h = std::max(8, ((int32_t)std::lround((float)h * scale)) & ~1);
	}
	*outW = w;
	*outH = h;
}

bool ToneMapper::EnsureBilateralTargets(int32_t eyeWidth, int32_t eyeHeight)
{
	int32_t w = 8, h = 8;
	BilateralTargetSize(eyeWidth, eyeHeight, &w, &h);
	if(bilateral_tex_ != 0 && bilateral_w_ == w && bilateral_h_ == h)
		return true;
	if(bilateral_tex_ != 0)
	{
		glDeleteTextures(1, &bilateral_tex_);
		bilateral_tex_ = 0;
	}
	if(bilateral_fbo_ == 0)
		glGenFramebuffers(1, &bilateral_fbo_);

	while(glGetError() != GL_NO_ERROR) { }
	glGenTextures(1, &bilateral_tex_);
	glBindTexture(GL_TEXTURE_2D, bilateral_tex_);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	GLint prev = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
	glBindFramebuffer(GL_FRAMEBUFFER, bilateral_fbo_);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			bilateral_tex_, 0);
	const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &drawBuf);
	const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
			&& glGetError() == GL_NO_ERROR;
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev);
	if(!ok)
	{
		LOGE("Bilateral upsample FBO incomplete (%dx%d)", w, h);
		glDeleteTextures(1, &bilateral_tex_);
		bilateral_tex_ = 0;
		return false;
	}
	bilateral_w_ = w;
	bilateral_h_ = h;
	LOGI("Joint bilateral upsample target: %dx%d (eye %dx%d)", w, h, eyeWidth, eyeHeight);
	return true;
}

bool ToneMapper::RenderBilateralMap(const float *matrix, int32_t eyeWidth, int32_t eyeHeight)
{
	if(depth_tex_[depth_newest_] == 0 || depth_width_ < 8 || depth_height_ < 8)
		return false;
	if(!CompileBilateralProgram() || !EnsureBilateralTargets(eyeWidth, eyeHeight))
		return false;

	GLint prevFbo = 0;
	GLint prevVp[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
	glGetIntegerv(GL_VIEWPORT, prevVp);

	glBindFramebuffer(GL_FRAMEBUFFER, bilateral_fbo_);
	const GLenum kBilatAtt = GL_COLOR_ATTACHMENT0;
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &kBilatAtt);
	glViewport(0, 0, bilateral_w_, bilateral_h_);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(bilateral_program_);
	glUniformMatrix4fv(bloc_tex_matrix_, 1, GL_FALSE, matrix);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_tex_);
	glUniform1i(bloc_sampler_, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth_tex_[depth_newest_]);
	glUniform1i(bloc_depth_tex_, 1);
	glUniform2f(bloc_depth_size_, (float)depth_width_, (float)depth_height_);
	glUniform1f(bloc_sigma_r_, 0.25f);
	const float sharpVal = AiEdgeSharp();
	glUniform1f(bloc_sharp_, sharpVal);

	glBindVertexArray(vao_);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
	glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
	bilateral_built_sharp_ = sharpVal;
	return true;
}

void ToneMapper::DestroyBilateralTargets()
{
	if(bilateral_program_ != 0) { glDeleteProgram(bilateral_program_); bilateral_program_ = 0; }
	if(bilateral_tex_ != 0) { glDeleteTextures(1, &bilateral_tex_); bilateral_tex_ = 0; }
	if(bilateral_fbo_ != 0) { glDeleteFramebuffers(1, &bilateral_fbo_); bilateral_fbo_ = 0; }
	bilateral_w_ = bilateral_h_ = 0;
	bilateral_ready_ = false;
	bilateral_built_upload_ns_ = -1;
	bilateral_built_sharp_ = -1.0f;
}

/** Desenha o mapa de profundidade do quadro, no alvo pequeno. */
void ToneMapper::RenderDepth(const float *matrix, int32_t width, int32_t height)
{
	if(depth_program_ == 0 || !EnsureDepthTargets(width, height))
		return;

	// The heuristic pass also replaces the depth texture, but it does not
	// touch ai_last_upload_ns_. Without this, the cached offset table could
	// be built from a previous AI map while the warp samples a heuristic one
	// (visible on the very first frames before the model's first result).
	offset_cached_ = false;

	const int destino = 1 - depth_newest_;
	glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo_);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			depth_tex_[destino], 0);
	if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
	{
		if(!logged_depth_failure_)
		{
			logged_depth_failure_ = true;
			LOGE("Incomplete framebuffer in the depth pass; 3D will have no depth");
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return;
	}

	glViewport(0, 0, depth_width_, depth_height_);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(depth_program_);
	glUniformMatrix4fv(dloc_tex_matrix_, 1, GL_FALSE, matrix);
	// O passo e o da FONTE, e nao o do alvo pequeno: a pista de escala fina
	// procura borda de um pixel do video, que e o que separa letra de textura.
	// Medida na grade pequena, ela mediria borda de quatro pixels e o texto
	// deixaria de se distinguir de uma parede.
	glUniform2f(dloc_texel_step_, width > 0 ? 1.0f / (float)width : 0.0f,
			height > 0 ? 1.0f / (float)height : 0.0f);
	glUniform1f(dloc_ground_sign_, ground_sign_);
	glUniform1f(dloc_convergence_, convergence_);
	glUniform1i(dloc_has_prev_, depth_has_prev_ ? 1 : 0);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_tex_);
	glUniform1i(dloc_sampler_, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth_tex_[depth_newest_]);
	glUniform1i(dloc_prev_, 1);

	glBindVertexArray(vao_);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	glActiveTexture(GL_TEXTURE0);
	depth_newest_ = destino;
	depth_has_prev_ = true;
	// Devolve o framebuffer. Quem chama pode sair antes de prender o seu -- e o
	// caso do quadro previsto --, e deixar o alvo pequeno preso faria a proxima
	// escrita cair nele.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool ToneMapper::CurtainActive() const
{
	if(!curtain_started_)
		return false;
	const auto now = std::chrono::steady_clock::now();
	float ms = std::chrono::duration<float, std::milli>(now - curtain_start_).count();
	return ms < (float)CurtainDurationMs();
}

ToneMapper::FrameAction ToneMapper::PrepareFrame(int32_t eyeWidth, int32_t eyeHeight,
		int stereoEffMode, float stereoStrength, float stereoConv, bool pq,
		float sharpen, bool swapchainReady)
{
	if(!attached_ || program_ == 0)
		return FrameAction::kNoFrame;

	const bool updated = ASurfaceTexture_updateTexImage(surface_texture_) == 0;
	const int64_t timestamp = updated
			? (int64_t)ASurfaceTexture_getTimestamp(surface_texture_)
			: last_timestamp_;
	const bool fresh = updated && timestamp != last_timestamp_;
	last_timestamp_ = timestamp;
	if(fresh)
	{
		has_frame_ = true;
		// The curtain and Java's first-frame freeze start on the SAME frame.
		if(!curtain_started_)
		{
			curtain_started_ = true;
			curtain_start_ = std::chrono::steady_clock::now();
		}
	}
	if(!has_frame_)
		return FrameAction::kNoFrame;

	if(!logged_cadence_)
	{
		frames_seen_++;
		if(fresh)
			frames_fresh_++;
		if(frames_seen_ >= 600)
		{
			logged_cadence_ = true;
			LOGI("Cadence: %d of %d passes had a fresh frame (%.0f%%)",
					frames_fresh_, frames_seen_,
					100.0 * frames_fresh_ / (double)frames_seen_);
		}
	}

	ASurfaceTexture_getTransformMatrix(surface_texture_, prepared_matrix_);
	prepared_fresh_ = fresh;

	if(!stage_enabled_.load())
		CollectAiRgbPbo();

	// During screen drag/resize gestures, yield Adreno OpenCL compute for active motion
	// so XR frame pacing stays locked, while allowing a slow keepalive refresh for long holds.
	const bool manipNow = ai_manipulating_.load(std::memory_order_acquire);
	const auto nowTp = std::chrono::steady_clock::now();
	bool allowAiCapture = true;
	if(manipNow)
	{
		if(!manip_paused_)
		{
			manip_paused_ = true;
			manip_pause_start_ = nowTp;
			manip_last_capture_ = nowTp;
			allowAiCapture = false;
		}
		else
		{
			const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					nowTp - manip_pause_start_).count();
			const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					nowTp - manip_last_capture_).count();
			if(heldMs >= 1500 && sinceMs >= 450)
			{
				manip_last_capture_ = nowTp;
				allowAiCapture = true;
			}
			else
			{
				allowAiCapture = false;
			}
		}
	}
	else if(manip_paused_)
	{
		manip_paused_ = false;
		// Reset capture interval on release so a fresh map runs immediately.
		ai_capture_accum_ns_ = ai_capture_period_ns_.load(std::memory_order_acquire);
	}

	if(allowAiCapture)
		AiCaptureTick(prepared_matrix_);

	if(stage_enabled_.load())
		StageAdopt(eyeWidth, eyeHeight);
	else if(ai_mode_.load() && stereoEffMode == 1)
		DrainAiDepth(eyeWidth, eyeHeight);

	if(stereoEffMode == 1 && ai_mode_.load())
		HarvestGpuTimers();

	if(!swapchainReady || !last_rendered_valid_ || fresh)
		return FrameAction::kNeedDraw;

	const bool aiActive = (stereoEffMode == 1 && ai_mode_.load());
	const bool curtainNow = CurtainActive();
	const bool depthUpdated = aiActive && (ai_last_upload_ns_ != last_rendered_upload_ns_);
	const bool showDepthNow = show_depth_.load();
	const float cr = corner_radius_.load(std::memory_order_relaxed);
	const float ca = corner_aspect_.load(std::memory_order_relaxed);

	if(curtainNow || last_rendered_curtain_
			|| depthUpdated
			|| last_rendered_w_ != eyeWidth
			|| last_rendered_h_ != eyeHeight
			|| last_rendered_stereo_ != stereoEffMode
			|| last_rendered_ai_ != aiActive
			|| last_rendered_pq_ != pq
			|| last_rendered_show_depth_ != showDepthNow
			|| std::fabs(last_rendered_strength_ - stereoStrength) > 1e-4f
			|| std::fabs(last_rendered_conv_ - stereoConv) > 1e-4f
			|| std::fabs(last_rendered_sharpen_ - sharpen) > 1e-4f
			|| std::fabs(last_rendered_corner_r_ - cr) > 1e-5f
			|| std::fabs(last_rendered_corner_a_ - ca) > 1e-4f
			|| std::fabs(last_rendered_nits_ - target_nits_) > 0.1f)
	{
		return FrameAction::kNeedDraw;
	}

	return FrameAction::kReuseLast;
}

bool ToneMapper::Render(GLuint target, int32_t width, int32_t height, bool pq,
		float sharpen, bool encode, bool extrapolate, GLuint target_right,
		bool wide_sbs)
{
	if(PrepareFrame(width, height, stereo_mode_, stereo_strength_, convergence_,
			pq, sharpen, true) == FrameAction::kNoFrame)
		return false;
	return RenderPrepared(target, width, height, pq, sharpen, encode,
			extrapolate, target_right, wide_sbs);
}

bool ToneMapper::RenderPrepared(GLuint target, int32_t width, int32_t height, bool pq,
		float sharpen, bool encode, bool extrapolate, GLuint target_right,
		bool wide_sbs)
{
	if(!attached_ || program_ == 0 || !has_frame_)
		return false;

	const bool fresh = prepared_fresh_;
	const float *matrix = prepared_matrix_;

	const bool wantGpuTimer = (stereo_mode_ == 1 && ai_mode_.load());
	const bool gpuTimerStarted = wantGpuTimer
			&& ((gpu_timer_sample_seq_++ & 3u) == 0u)
			&& BeginGpuTimer();
	struct ScopedGpuTimer
	{
		ToneMapper *tm;
		bool active;
		~ScopedGpuTimer() { if(active && tm) tm->EndGpuTimerFromGuard(); }
	} gpuTimerGuard{this, gpuTimerStarted};

	if(stereo_mode_ == 1 && fresh)
	{
		// AI depth replaces the heuristic pass only when a model result is
		// ready; otherwise AiFrameStep() runs the stock RenderDepth() itself.
		if(ai_mode_.load())
			AiFrameStep(matrix, width, height);
		else
			RenderDepth(matrix, width, height);
	}

	bilateral_ready_ = false;
	offset_ready_ = false;
	if(stereo_mode_ == 1 && ai_mode_.load() && depth_tex_[depth_newest_] != 0)
	{
		int32_t wantBilatW = 8, wantBilatH = 8;
		BilateralTargetSize(width, height, &wantBilatW, &wantBilatH);
		const bool depthUploaded = ai_last_upload_ns_ != bilateral_built_upload_ns_;
		const bool sharpChanged = std::fabs(AiEdgeSharp() - bilateral_built_sharp_) > 1e-3f;
		const bool bilatSizeMismatch = bilateral_tex_ == 0
				|| bilateral_w_ != wantBilatW || bilateral_h_ != wantBilatH;
		bool bilatUpdated = false;
		if(depthUploaded || sharpChanged || bilatSizeMismatch || bilateral_program_ == 0)
		{
			bilateral_ready_ = RenderBilateralMap(matrix, width, height);
			if(bilateral_ready_)
				bilateral_built_upload_ns_ = ai_last_upload_ns_;
			bilatUpdated = bilateral_ready_;
		}
		else
		{
			bilateral_ready_ = (bilateral_tex_ != 0 && bilateral_w_ == wantBilatW
					&& bilateral_h_ == wantBilatH);
		}

		if(bilateral_ready_)
		{
			const float dispTexels = 0.5f * stereo_strength_
					* (float)(bilateral_w_ > 0 ? bilateral_w_ : 1);
			const bool depthChanged = ai_last_upload_ns_ != offset_built_at_upload_ns_;
			const bool paramsChanged =
					std::fabs(dispTexels - offset_built_disp_) > 1e-3f
					|| std::fabs(convergence_ - offset_built_conv_) > 1e-3f;
			if(bilatUpdated || depthChanged || paramsChanged || !offset_cached_)
			{
				offset_ready_ = RenderOffsetMap(dispTexels);
				offset_cached_ = offset_ready_;
				offset_built_at_upload_ns_ = ai_last_upload_ns_;
				offset_built_disp_ = dispTexels;
				offset_built_conv_ = convergence_;
			}
			else
			{
				offset_ready_ = offset_cached_;
			}
		}
		else
		{
			offset_cached_ = false;
		}
	}
	else
	{
		offset_cached_ = false;
		bilateral_built_upload_ns_ = -1;
	}

	bool ok = false;
	if(wide_sbs && stereo_mode_ != 0 && target != 0)
	{
		ok = DrawWideStereo(target, width, height, pq, sharpen, encode, matrix);
	}
	else if(stereo_mode_ != 0 && target_right != 0)
	{
		const bool esq = DrawEye(target, width, height, pq, sharpen, encode,
				extrapolate, fresh, matrix, -1, 0);
		const bool dir = DrawEye(target_right, width, height, pq, sharpen, encode,
				extrapolate, fresh, matrix, +1, 1);
		ok = esq && dir;
	}
	else
	{
		ok = DrawEye(target, width, height, pq, sharpen, encode, extrapolate, fresh,
				matrix, 0, 0);
	}

	if(ok)
	{
		last_rendered_valid_ = true;
		last_rendered_curtain_ = CurtainActive();
		last_rendered_pq_ = pq;
		last_rendered_ai_ = (stereo_mode_ == 1 && ai_mode_.load());
		last_rendered_show_depth_ = show_depth_.load();
		last_rendered_stereo_ = stereo_mode_;
		last_rendered_strength_ = stereo_strength_;
		last_rendered_conv_ = convergence_;
		last_rendered_sharpen_ = sharpen;
		last_rendered_corner_r_ = corner_radius_.load(std::memory_order_relaxed);
		last_rendered_corner_a_ = corner_aspect_.load(std::memory_order_relaxed);
		last_rendered_nits_ = target_nits_;
		last_rendered_upload_ns_ = ai_last_upload_ns_;
		last_rendered_w_ = width;
		last_rendered_h_ = height;
	}
	return ok;
}

bool ToneMapper::IsTargetValidated(GLuint tex) const
{
	for(int i = 0; i < validated_count_; i++)
	{
		if(validated_targets_[i].tex == tex)
			return true;
	}
	return false;
}

void ToneMapper::MarkTargetValidated(GLuint tex)
{
	if(tex == 0 || IsTargetValidated(tex))
		return;
	if(validated_count_ < kMaxValidatedTargets)
	{
		validated_targets_[validated_count_].tex = tex;
		validated_targets_[validated_count_].layered = logged_layered_;
		validated_count_++;
	}
}

bool ToneMapper::AttachAndValidateTarget(GLuint target, bool write_history, GLuint history_tex)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
	if(write_history)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, history_tex, 0);
	else if(extrapolate_fn_ != nullptr)
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);

	if(!write_history && IsTargetValidated(target))
	{
		bool layered = false;
		for(int i = 0; i < validated_count_; i++)
		{
			if(validated_targets_[i].tex == target)
			{
				layered = validated_targets_[i].layered;
				break;
			}
		}
		if(layered)
			glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0, 0);
		else
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
		return true;
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if(status != GL_FRAMEBUFFER_COMPLETE && write_history)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
		GLenum without = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if(without == GL_FRAMEBUFFER_COMPLETE)
		{
			if(!logged_extrapolation_)
			{
				logged_extrapolation_ = true;
				LOGW("The extrapolation history does not match the swapchain "
						"(status 0x%x); carrying on without extrapolated frames", status);
			}
			extrapolate_fn_ = nullptr;
			status = without;
		}
	}
	if(status != GL_FRAMEBUFFER_COMPLETE)
	{
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0, 0);
		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if(status == GL_FRAMEBUFFER_COMPLETE && !logged_layered_)
		{
			logged_layered_ = true;
			LOGI("Swapchain handed over as a texture array; attached per layer");
		}
	}
	if(status != GL_FRAMEBUFFER_COMPLETE)
	{
		if(!logged_fbo_failure_)
		{
			logged_fbo_failure_ = true;
			LOGE("Incomplete framebuffer in the tone mapper: status 0x%x, texture %u",
					status, target);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return false;
	}
	if(!write_history)
		MarkTargetValidated(target);
	return true;
}

void ToneMapper::UploadCommonUniforms(int32_t eyeWidth, int32_t eyeHeight, bool pq,
		float sharpen, bool encode, const float *matrix)
{
	glUseProgram(program_);
	{
		float curtainOpen = 1.0f;
		if(curtain_started_)
		{
			const auto now = std::chrono::steady_clock::now();
			float ms = std::chrono::duration<float, std::milli>(now - curtain_start_).count();
			float t = std::clamp(ms / (float)CurtainDurationMs(), 0.0f, 1.0f);
			float e = t * t;
			float tail = std::clamp((t - 0.88f) / 0.12f, 0.0f, 1.0f);
			tail = tail * tail * (3.0f - 2.0f * tail);
			curtainOpen = e + (1.0f - e) * tail;
		}
		glUniform1f(loc_curtain_, curtainOpen);
	}
	if(loc_corner_radius_ >= 0)
		glUniform1f(loc_corner_radius_, corner_radius_.load(std::memory_order_relaxed));
	if(loc_corner_aspect_ >= 0)
		glUniform1f(loc_corner_aspect_, corner_aspect_.load(std::memory_order_relaxed));
	glUniformMatrix4fv(loc_tex_matrix_, 1, GL_FALSE, matrix);
	glUniform1i(loc_pq_, pq ? 1 : 0);
	glUniform1f(loc_target_nits_, target_nits_);
	glUniform1f(loc_sharpen_, sharpen);
	glUniform2f(loc_texel_step_, eyeWidth > 0 ? 1.0f / (float)eyeWidth : 0.0f,
			eyeHeight > 0 ? 1.0f / (float)eyeHeight : 0.0f);
	glUniform1i(loc_encode_, encode ? 1 : 0);
	glUniform1i(loc_stereo_, stereo_mode_);
	glUniform1f(loc_stereo_strength_, stereo_strength_);
	glUniform1f(loc_convergence_, convergence_);

	const bool useBilat = AiDepthEnabled() && bilateral_ready_ && bilateral_tex_ != 0;
	const bool depthInAlpha = AiDepthEnabled() && !useBilat;
	glUniform1f(loc_ai_depth_, (useBilat && offset_ready_) ? 1.0f : 0.0f);
	glUniform1f(loc_depth_in_alpha_, depthInAlpha ? 1.0f : 0.0f);
	glUniform1f(loc_depth_cubic_, (useBilat && offset_ready_ && AiDepthCubic()) ? 1.0f : 0.0f);
	const GLuint activeDepthTex = useBilat ? bilateral_tex_
			: ((stereo_mode_ == 1 && depth_tex_[depth_newest_] != 0)
					? depth_tex_[depth_newest_] : 0);
	glUniform1f(loc_show_depth_, (ShowDepth() && stereo_mode_ == 1 && activeDepthTex != 0) ? 1.0f : 0.0f);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, activeDepthTex != 0 ? activeDepthTex : EnsureDummyTexture());
	glUniform1i(loc_depth_tex_, 1);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_tex_);
	glUniform1i(loc_sampler_, 0);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, offset_tex_ != 0 ? offset_tex_ : EnsureDummyTexture());
	glUniform1i(loc_offset_tex_, 2);

	const float lowResW = (float)(useBilat
			? (bilateral_w_ > 0 ? bilateral_w_ : 1)
			: (depth_width_ > 0 ? depth_width_ : 1));
	const float lowResH = (float)(useBilat
			? (bilateral_h_ > 0 ? bilateral_h_ : 1)
			: (depth_height_ > 0 ? depth_height_ : 1));
	if(loc_depth_dims_ >= 0)
		glUniform4f(loc_depth_dims_, lowResW, lowResH, 1.0f / lowResW, 1.0f / lowResH);
	const float frameW = (float)(eyeWidth > 0 ? eyeWidth : 1);
	glUniform1f(loc_lowres_width_, lowResW);
	glUniform1f(loc_frame_width_, frameW);
	glUniform1f(loc_disp_texels_, 0.5f * stereo_strength_ * lowResW);
	glActiveTexture(GL_TEXTURE0);
}

bool ToneMapper::DrawWideStereo(GLuint target, int32_t eyeWidth, int32_t eyeHeight,
		bool pq, float sharpen, bool encode, const float *matrix)
{
	if(!AttachAndValidateTarget(target, false, 0))
		return false;

	const GLenum single = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &single);
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &single);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	UploadCommonUniforms(eyeWidth, eyeHeight, pq, sharpen, encode, matrix);
	glBindVertexArray(vao_);

	// Left eye -> [0, eyeWidth)
	glViewport(0, 0, eyeWidth, eyeHeight);
	glUniform1f(loc_eye_, -1.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	// Right eye -> [eyeWidth, 2 * eyeWidth)
	glViewport(eyeWidth, 0, eyeWidth, eyeHeight);
	glUniform1f(loc_eye_, +1.0f);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

/**
 * Desenha um alvo: a imagem inteira, para um olho.
 *
 * Saiu de dentro do Render porque no 3D ela acontece duas vezes por quadro, uma
 * por swapchain, enquanto tudo o que vem antes -- prender o buffer, decidir se
 * o quadro e novo, estimar a profundidade -- acontece uma vez so. Chamar Render
 * duas vezes consumiria dois quadros do decodificador para mostrar um.
 *
 * `eye` e -1, +1 ou 0 quando nao ha olho a distinguir. `slot` escolhe o par de
 * texturas de historico, que e por olho.
 */
bool ToneMapper::DrawEye(GLuint target, int32_t width, int32_t height, bool pq,
		float sharpen, bool encode, bool extrapolate, bool fresh, const float *matrix,
		int eye, int slot)
{
	// Quadro previsto, quando nao ha um novo para mostrar.
	//
	// A fonte entrega 60 por segundo e o painel mostra 120: metade das passadas
	// nao tem imagem nova. Em vez de repetir a anterior, a extensao da Qualcomm
	// produz das duas ultimas um quadro adiante no tempo. Meio passo, porque e
	// exatamente onde esta a passada intermediaria.
	//
	// E extrapolacao e nao interpolacao: nada e esperado, entao nao ha latencia
	// adicionada -- o preco e artefato no que foi previsto errado, nao atraso.
	const bool use_history = extrapolate && extrapolate_fn_ && target != 0
			&& EnsureHistory(width, height, slot);
	if(!fresh && use_history && history_count_[slot] >= 2)
	{
		const int newest = history_newest_[slot];
		const int older = 1 - newest;

		// A conferencia de erro so acontece ate a primeira vez que da certo.
		//
		// Duas razoes. glGetError devolve *um* erro por chamada e o remove da
		// fila: um erro deixado para tras por qualquer chamada anterior seria
		// lido aqui como falha da extrapolacao, e o recurso se desligaria
		// sozinho, errado, logo na primeira tentativa. Por isso a fila e
		// esvaziada antes. E porque glGetError forca sincronizacao com o driver
		// em varias implementacoes -- pagar isso sessenta vezes por segundo para
		// confirmar o que ja foi confirmado seria caro a troco de nada.
		const bool checking = !logged_extrapolation_;
		if(checking)
			while(glGetError() != GL_NO_ERROR) { }

		extrapolate_fn_(history_[slot][older], history_[slot][newest], target, 0.5f);

		if(!checking)
			return true;

		const GLenum err = glGetError();
		logged_extrapolation_ = true;
		if(err == GL_NO_ERROR)
		{
			LOGI("First extrapolated frame handed to the compositor");
			return true;
		}
		// Falhou: cai no redesenho de sempre. Uma vez que falha, falha sempre,
		// entao desliga em vez de tentar a cada quadro.
		LOGW("glExtrapolateTex2DQCOM returned 0x%x; falling back to a full redraw", err);
		extrapolate_fn_ = nullptr;
	}

	// Alvo zero e a janela: no modo janela quem recebe o desenho e o
	// framebuffer padrao da GLSurfaceView, e nao ha textura para anexar.
	// A janela nao e desenhada direto na tela.
	//
	// O fragment shader declara duas saidas: a imagem e o historico da
	// extrapolacao. O framebuffer padrao tem uma so. Pela especificacao a
	// segunda deveria ser descartada em silencio, e no compositor e o que
	// acontece -- o modo imersivo sempre funcionou. Neste Adreno, com a tela
	// como alvo, o desenho vira GL_INVALID_OPERATION e nao sai nada. O diario
	// mostrou exatamente isso: 58 passadas desenhadas, `gl 0x502`, tela preta
	// com som. Declarar o draw buffer como GL_BACK nao resolveu.
	//
	// Entao a janela passa a usar o mesmo caminho do modo imersivo, que e o
	// caminho que funciona: desenha numa textura nossa e copia para a tela. A
	// copia e um blit de framebuffer, feito pelo proprio GL, e a 1600x900 ela
	// nao aparece na conta.
	const bool to_window = target == 0;
	// Uma passada instrumentada, a primeira da janela. Saber que houve erro nao
	// diz qual chamada o produziu, e essa diferenca ja custou duas builds.
	const bool depurar = to_window && !logged_window_;
	if(depurar)
		while(glGetError() != GL_NO_ERROR) { }
	if(to_window)
	{
		if(!EnsureWindowTarget(width, height))
		{
			if(!logged_fbo_failure_)
			{
				logged_fbo_failure_ = true;
				LOGE("Could not create the window target texture (%dx%d)", width, height);
			}
			return false;
		}
		target = window_tex_;
	}
	bool write_history = fresh && use_history;
	int history_slot = 0;
	GLuint history_tex = 0;
	if(write_history)
	{
		history_slot = history_newest_[slot] < 0 ? 0 : 1 - history_newest_[slot];
		history_tex = history_[slot][history_slot];
	}
	if(!AttachAndValidateTarget(target, write_history, history_tex))
		return false;
	if(write_history && extrapolate_fn_ == nullptr)
		write_history = false;

	{
		const GLenum both[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		const GLenum single[] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers(write_history ? 2 : 1, write_history ? both : single);
		glInvalidateFramebuffer(GL_FRAMEBUFFER, write_history ? 2 : 1, write_history ? both : single);
	}

	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	UploadCommonUniforms(width, height, pq, sharpen, encode, matrix);
	glUniform1f(loc_eye_, (float)eye);

	const GLenum erro_antes = depurar ? glGetError() : GL_NO_ERROR;

	glBindVertexArray(vao_);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	const GLenum erro_desenho = depurar ? glGetError() : GL_NO_ERROR;

	if(write_history)
	{
		history_newest_[slot] = history_slot;
		if(history_count_[slot] < 2)
			history_count_[slot]++;
		// Solta o anexo: a proxima passada pode ser sem historico, e um anexo
		// esquecido apontando para textura que sera reescrita e a receita do
		// framebuffer incompleto que ja custou uma sessao aqui.
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
	}
	glBindVertexArray(0);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

	// A copia para a tela, no modo janela.
	GLenum erro_copia = GL_NO_ERROR;
	if(to_window)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if(depurar)
			erro_copia = glGetError();
		// GL_NEAREST porque origem e destino tem o mesmo tamanho: nao ha o que
		// filtrar, e pedir filtragem so custaria.
	}

	// Uma linha, na primeira passada para a janela.
	//
	// Ate aqui o modo janela ficava mudo depois de "Tone mapper ready": nao
	// havia como distinguir "desenhou preto" de "nao desenhou". O erro do GL
	// vai junto porque uma janela preta com som e, quase sempre, uma chamada
	// recusada que ninguem leu.
	if(to_window && !logged_window_)
	{
		logged_window_ = true;
		const GLenum err = glGetError();
		LOGI("First window frame drawn: %dx%d, sharpen %.2f, pq %d | "
				"gl before 0x%x, draw 0x%x, blit 0x%x, after 0x%x",
				width, height, sharpen, pq ? 1 : 0,
				erro_antes, erro_desenho, erro_copia, err);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

void ToneMapper::Destroy()
{
	// Stop the stage before deleting any shared GL object or releasing EGL.
	StopStage();
	if(attached_ && surface_texture_)
		ASurfaceTexture_detachFromGLContext(surface_texture_);
	attached_ = false;

	if(vbo_ != 0)
		glDeleteBuffers(1, &vbo_);
	if(vao_ != 0)
		glDeleteVertexArrays(1, &vao_);
	if(fbo_ != 0)
		glDeleteFramebuffers(1, &fbo_);
	for(int slot = 0; slot < 2; slot++)
	{
		if(history_[slot][0] != 0)
			glDeleteTextures(2, history_[slot]);
		history_[slot][0] = history_[slot][1] = 0;
		history_newest_[slot] = -1;
		history_count_[slot] = 0;
		history_width_[slot] = history_height_[slot] = 0;
	}
	extrapolate_fn_ = nullptr;

	if(window_tex_ != 0)
		glDeleteTextures(1, &window_tex_);
	window_tex_ = 0;
	window_w_ = window_h_ = 0;
	if(dummy_tex_ != 0)
		glDeleteTextures(1, &dummy_tex_);
	dummy_tex_ = 0;

	if(depth_fbo_ != 0)
		glDeleteFramebuffers(1, &depth_fbo_);
	if(depth_tex_[0] != 0)
		glDeleteTextures(2, depth_tex_);
	depth_tex_[0] = depth_tex_[1] = 0;
	depth_fbo_ = 0;
	depth_newest_ = 0;
	depth_has_prev_ = false;
	depth_width_ = depth_height_ = 0;
	if(depth_program_ != 0)
		glDeleteProgram(depth_program_);
	depth_program_ = 0;

	if(external_tex_ != 0)
		glDeleteTextures(1, &external_tex_);
	if(program_ != 0)
		glDeleteProgram(program_);
	vbo_ = vao_ = fbo_ = external_tex_ = program_ = 0;

	DestroyAiTargets();
	DestroyBilateralTargets();
	DestroyOffsetTargets();
	DestroyGpuTimers();
	ResetFboCache();
	depth_for_ai_ = false;

	if(surface_texture_)
		ASurfaceTexture_release(surface_texture_);
	surface_texture_ = nullptr;
	has_frame_ = false;
	curtain_started_ = false;
}

} // namespace p5m
