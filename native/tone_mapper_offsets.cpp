// SPDX-License-Identifier: AGPL-3.0-only
//
// Occlusion-aware inverse warp for the ZipDepth synthetic-3D path.
//
// The stock warp samples the depth map AT the destination pixel and shifts the
// source coordinate by that depth. That is correct only where depth is flat.
// At a silhouette step it is wrong by most of the disparity range, so several
// destination pixels read the same source edge and a vertical pole is drawn
// two or three times, mirrored between the eyes. Fading or snapping the shift
// at those steps only changes how many copies appear.
//
// This pass instead SOLVES for the source position: for every destination
// texel and both eyes it searches t over the disparity span for a zero of
//     e(t) = t + disp * (d(x + t) - convergence)
// and keeps the crossing with the nearest (largest) depth, which is what
// occlusion means. The main warp pass then reads this offset table and refines
// it with two Newton steps against the full-resolution depth.
//
// Algorithm ported from Gilleece/moonlight-android-xr (xr_shaders.c,
// OFFSET_FRAGMENT_SRC, GPLv3). Reimplemented here against P5M's own depth
// texture, vertex strip and convergence/subtitle-protection encoding; no file
// was copied. Only the maths and the idea of the search are shared.

#include "tone_mapper.h"
#include "log.h"

#include <string>

namespace p5m {
namespace {

const char *kOffsetVertex = R"(#version 300 es
in vec2 aPos;
out vec2 vScreen;
void main()
{
	vScreen = aPos * 0.5 + 0.5;
	gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char *kOffsetFragment = R"(#version 300 es
precision mediump float;
in highp vec2 vScreen;
uniform sampler2D uDepthTex;
uniform float uDispTexels;
uniform float uConvergence;
out vec4 fragColor;
void main()
{
	ivec2 sz = textureSize(uDepthTex, 0);
	int x = int(gl_FragCoord.x);
	int y = int(gl_FragCoord.y);
	// uConvergence packs the bottom-of-frame subtitle protection the same way
	// the main shader does: integer part * 0.01 is the protected fraction.
	float conv = fract(uConvergence);
	float subProt = (uConvergence >= 1.0) ? (floor(uConvergence) * 0.01) : 0.0;
	float subMask = (subProt > 0.005)
			? smoothstep(subProt, subProt + 0.06, float(vScreen.y)) : 1.0;
	int reach = int(ceil(abs(uDispTexels) * max(conv, 1.0 - conv))) + 2;

	float dispL = uDispTexels * subMask;
	float dispR = -dispL;
	float here = texelFetch(uDepthTex, ivec2(x, y), 0).r;
	float bestDL = -1.0;
	float bestDR = -1.0;
	float bestOffL = -dispL * (here - conv);
	float bestOffR = -dispR * (here - conv);

	float pd = texelFetch(uDepthTex,
			ivec2(clamp(x - reach, 0, sz.x - 1), y), 0).r;
	float dShift = pd - conv;
	float peL = float(-reach) + dispL * dShift;
	float peR = float(-reach) + dispR * dShift;

	for(int t = -reach + 1; t <= reach; t++)
	{
		float cd = texelFetch(uDepthTex,
				ivec2(clamp(x + t, 0, sz.x - 1), y), 0).r;
		float dc = cd - conv;
		float ft = float(t);

		float ceL = ft + dispL * dc;
		float spanL = ceL - peL;
		if(peL * ceL <= 0.0 && abs(spanL) > 1e-5)
		{
			float f = clamp(-peL / spanL, 0.0, 1.0);
			float rd = pd + f * (cd - pd);
			if(rd > bestDL)
			{
				bestDL = rd;
				bestOffL = float(t - 1) + f;
			}
		}
		peL = ceL;

		float ceR = ft + dispR * dc;
		float spanR = ceR - peR;
		if(peR * ceR <= 0.0 && abs(spanR) > 1e-5)
		{
			float f = clamp(-peR / spanR, 0.0, 1.0);
			float rd = pd + f * (cd - pd);
			if(rd > bestDR)
			{
				bestDR = rd;
				bestOffR = float(t - 1) + f;
			}
		}
		peR = ceR;

		pd = cd;
	}
	vec2 result = vec2(bestOffL, bestOffR);
	fragColor = vec4(result / (2.0 * float(reach)) + 0.5, 0.0, 1.0);
}
)";

GLuint CompileShaderOffset(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if(ok != GL_TRUE)
	{
		GLint len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
		std::string info((size_t)(len > 0 ? len : 1), '\0');
		glGetShaderInfoLog(shader, len, nullptr, &info[0]);
		LOGE("Offset pass shader compile failed: %s", info.c_str());
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

} // namespace

bool ToneMapper::CompileOffsetProgram()
{
	if(offset_program_ != 0)
		return true;
	GLuint vs = CompileShaderOffset(GL_VERTEX_SHADER, kOffsetVertex);
	GLuint fs = vs ? CompileShaderOffset(GL_FRAGMENT_SHADER, kOffsetFragment) : 0;
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
		LOGE("Offset pass link failed: %s", info.c_str());
		glDeleteProgram(p);
		return false;
	}
	offset_program_ = p;
	oloc_depth_ = glGetUniformLocation(p, "uDepthTex");
	oloc_disp_ = glGetUniformLocation(p, "uDispTexels");
	oloc_conv_ = glGetUniformLocation(p, "uConvergence");
	LOGI("Offset search pass ready");
	return true;
}

bool ToneMapper::EnsureOffsetTargets(int32_t w, int32_t h)
{
	if(offset_tex_ != 0 && offset_w_ == w && offset_h_ == h)
		return true;
	if(offset_tex_ != 0)
	{
		glDeleteTextures(1, &offset_tex_);
		offset_tex_ = 0;
	}
	if(offset_fbo_ == 0)
		glGenFramebuffers(1, &offset_fbo_);
	glGenTextures(1, &offset_tex_);
	glBindTexture(GL_TEXTURE_2D, offset_tex_);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	GLint previous = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
	glBindFramebuffer(GL_FRAMEBUFFER, offset_fbo_);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			offset_tex_, 0);
	const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &drawBuf);
	const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
			&& glGetError() == GL_NO_ERROR;
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
	if(!ok)
	{
		LOGE("Offset target framebuffer incomplete; AI 3D will use the stock warp");
		glDeleteTextures(1, &offset_tex_);
		offset_tex_ = 0;
		return false;
	}
	offset_w_ = w;
	offset_h_ = h;
	return true;
}

bool ToneMapper::RenderOffsetMap(float dispTexels)
{
	const bool useBilat = bilateral_ready_ && bilateral_tex_ != 0
			&& bilateral_w_ > 0 && bilateral_h_ > 0;
	const GLuint srcTex = useBilat ? bilateral_tex_ : depth_tex_[depth_newest_];
	const int32_t targetW = useBilat ? bilateral_w_ : depth_width_;
	const int32_t targetH = useBilat ? bilateral_h_ : depth_height_;
	if(!useBilat || srcTex == 0 || targetW < 8 || targetH < 8)
		return false;
	if(!CompileOffsetProgram() || !EnsureOffsetTargets(targetW, targetH))
		return false;

	GLint previous_fbo = 0;
	GLint previous_vp[4] = { 0, 0, 0, 0 };
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
	glGetIntegerv(GL_VIEWPORT, previous_vp);

	glBindFramebuffer(GL_FRAMEBUFFER, offset_fbo_);
	const GLenum kOffsetAtt = GL_COLOR_ATTACHMENT0;
	glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &kOffsetAtt);
	glViewport(0, 0, offset_w_, offset_h_);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(offset_program_);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, srcTex);
	glUniform1i(oloc_depth_, 0);
	glUniform1f(oloc_disp_, dispTexels);
	glUniform1f(oloc_conv_, convergence_);
	glBindVertexArray(vao_);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);
	glViewport(previous_vp[0], previous_vp[1], previous_vp[2], previous_vp[3]);
	return true;
}

void ToneMapper::DestroyOffsetTargets()
{
	if(offset_program_ != 0) { glDeleteProgram(offset_program_); offset_program_ = 0; }
	if(offset_tex_ != 0) { glDeleteTextures(1, &offset_tex_); offset_tex_ = 0; }
	if(offset_fbo_ != 0) { glDeleteFramebuffers(1, &offset_fbo_); offset_fbo_ = 0; }
	offset_w_ = offset_h_ = 0;
}

} // namespace p5m
