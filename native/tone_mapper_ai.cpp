// SPDX-License-Identifier: AGPL-3.0-only
//
// Zero-copy AI depth source for ZipDepth synthetic 3D (MoonlightXR-aligned).
//
// Pipeline:
//   Frame loop thread : CaptureAiRgb() draws OES -> 512x288 PBO + fence, wakes stage
//   Stage thread (EGL): CollectAiRgbPbo() flips Y & writes FP32 straight into
//                       DirectByteBuffer input[slot] + saves GL-order RGB snapshot
//                       -> wakes inference worker via condvar (no 8 ms polling)
//   ZipDepth thread   : AcquireAiInputSlot() -> interpreter.run(in[slot], out[slot])
//                       -> SubmitAiOutputSlot(slot) wakes StageThread immediately
//   Stage thread (EGL): UploadLatestAiDepthSlot() reads out[slot] directly in C++,
//                       runs 2%-98% percentile norm + 16x16 cut detection + EMA,
//                       packs GL_RGBA8 [R, G, B, D] (no CPU bilinear resize, 1/4 BW)
//                       -> glTexSubImage2D + fence; StageAdopt() publishes to Warp.

#include "tone_mapper.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace p5m {

// Forwarded from tone_mapper.cpp's helper? Defined here cleanly and exposed via internal helper.
namespace {

constexpr int32_t kAiFrameW = 512;
constexpr int32_t kAiFrameH = 288;
constexpr int64_t kNsPerSecond = 1000000000LL;
constexpr int32_t kDefaultMapsPerSecond = 30;

constexpr int kDtypeF32 = 0;
constexpr int kDtypeU8 = 1;
constexpr int kDtypeI8 = 2;

constexpr float kRawEmaTauSec = 0.045f;
constexpr float kBoundsTauSec = 0.18f;
constexpr float kSmoothTauSec = 0.13f;
constexpr float kFastEmaTauSec = 0.065f;
constexpr int64_t kSceneCutCooldownNs = 350000000LL;
constexpr int kThumbGrid = 16;
constexpr int kThumbCells = kThumbGrid * kThumbGrid;
constexpr int kHistBins = 16;

struct SceneThumb
{
	std::array<float, kThumbCells * 3> rgb{};
	std::array<float, kThumbCells> luma{};
	std::array<float, kHistBins> hist{};
};

void BuildSceneThumb(const uint8_t *rgba, int32_t w, int32_t h, SceneThumb &out)
{
	const int32_t stride = w * 4;
	out.hist.fill(0.0f);
	int totalSamples = 0;

	for(int by = 0; by < kThumbGrid; by++)
	{
		const int32_t y0 = by * h / kThumbGrid;
		const int32_t y1 = (by + 1) * h / kThumbGrid;
		for(int bx = 0; bx < kThumbGrid; bx++)
		{
			const int32_t x0 = bx * w / kThumbGrid;
			const int32_t x1 = (bx + 1) * w / kThumbGrid;
			float sr = 0.0f, sg = 0.0f, sb = 0.0f, sl = 0.0f;
			int count = 0;
			for(int32_t y = y0; y < y1; y += 2)
			{
				const uint8_t *row = rgba + (size_t)y * stride;
				for(int32_t x = x0; x < x1; x += 2)
				{
					const uint8_t *p = row + x * 4;
					const float r = (float)p[0] * (1.0f / 255.0f);
					const float g = (float)p[1] * (1.0f / 255.0f);
					const float b = (float)p[2] * (1.0f / 255.0f);
					const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
					sr += r;
					sg += g;
					sb += b;
					sl += lum;
					int bin = (int)(lum * (float)kHistBins);
					if(bin < 0) bin = 0;
					else if(bin >= kHistBins) bin = kHistBins - 1;
					out.hist[(size_t)bin] += 1.0f;
					count++;
					totalSamples++;
				}
			}
			const size_t cell = (size_t)by * kThumbGrid + (size_t)bx;
			if(count > 0)
			{
				const float inv = 1.0f / (float)count;
				out.rgb[cell * 3 + 0] = sr * inv;
				out.rgb[cell * 3 + 1] = sg * inv;
				out.rgb[cell * 3 + 2] = sb * inv;
				out.luma[cell] = sl * inv;
			}
			else
			{
				out.rgb[cell * 3 + 0] = 0.0f;
				out.rgb[cell * 3 + 1] = 0.0f;
				out.rgb[cell * 3 + 2] = 0.0f;
				out.luma[cell] = 0.0f;
			}
		}
	}

	if(totalSamples > 0)
	{
		const float invTot = 1.0f / (float)totalSamples;
		for(int b = 0; b < kHistBins; b++)
			out.hist[(size_t)b] *= invTot;
	}
}

void CompareSceneThumbs(const SceneThumb &curr, const SceneThumb &prev,
		float *outRgbDiff, float *outLumaDiff, float *outHistDiff, float *outCorr)
{
	float rgbSum = 0.0f;
	for(size_t i = 0; i < (size_t)kThumbCells * 3; i++)
		rgbSum += std::fabs(curr.rgb[i] - prev.rgb[i]);
	*outRgbDiff = rgbSum / (float)(kThumbCells * 3);

	float lumaSum = 0.0f;
	float meanA = 0.0f;
	float meanB = 0.0f;
	for(int i = 0; i < kThumbCells; i++)
	{
		lumaSum += std::fabs(curr.luma[(size_t)i] - prev.luma[(size_t)i]);
		meanA += curr.luma[(size_t)i];
		meanB += prev.luma[(size_t)i];
	}
	*outLumaDiff = lumaSum / (float)kThumbCells;
	meanA /= (float)kThumbCells;
	meanB /= (float)kThumbCells;

	float histL1 = 0.0f;
	for(int b = 0; b < kHistBins; b++)
		histL1 += std::fabs(curr.hist[(size_t)b] - prev.hist[(size_t)b]);
	*outHistDiff = 0.5f * histL1;

	float cov = 0.0f, varA = 0.0f, varB = 0.0f;
	for(int i = 0; i < kThumbCells; i++)
	{
		const float da = curr.luma[(size_t)i] - meanA;
		const float db = prev.luma[(size_t)i] - meanB;
		cov += da * db;
		varA += da * da;
		varB += db * db;
	}
	// Flat solid-color screens carry near-zero spatial variance (~std < 0.05).
	constexpr float kMinVarSum = 0.00256f;
	*outCorr = (varA >= kMinVarSum && varB >= kMinVarSum)
			? (cov / std::sqrt(varA * varB)) : 0.0f;
}

const std::array<float, 256> &ByteToFloatLut()
{
	static const std::array<float, 256> kLut = []() {
		std::array<float, 256> t{};
		for(int i = 0; i < 256; i++)
			t[i] = (float)i / 255.0f;
		return t;
	}();
	return kLut;
}

int64_t NowNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline float Clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// ---------------------------------------------------------------------------
// Active stage notifier so Java SubmitOutputSlot wakes StageThread immediately
// without risking touching a destroyed ToneMapper.
// ---------------------------------------------------------------------------
std::mutex g_active_stage_mtx;
ToneMapper *g_active_stage_tm = nullptr;

void SetActiveStageToneMapper(ToneMapper *tm)
{
	std::lock_guard<std::mutex> lock(g_active_stage_mtx);
	g_active_stage_tm = tm;
}

void NotifyActiveStage()
{
	std::lock_guard<std::mutex> lock(g_active_stage_mtx);
	if(g_active_stage_tm != nullptr)
		g_active_stage_tm->WakeStage();
}

// ---------------------------------------------------------------------------
// Double-buffered zero-copy slot mailbox backed by Java DirectByteBuffers.
// ---------------------------------------------------------------------------
enum class SlotState
{
	FREE = 0,
	FILLING = 1,
	READY_IN = 2,
	INFERRING = 3,
	READY_OUT = 4,
	UPLOADING = 5,
};

struct AiSlot
{
	void *in_ptr = nullptr;
	int64_t in_bytes = 0;
	void *out_ptr = nullptr;
	int64_t out_bytes = 0;
	std::vector<uint8_t> rgb_u8;
	int64_t seq = 0;
	SlotState state = SlotState::FREE;
};

struct AiSlotMailbox
{
	std::mutex mtx;
	std::condition_variable infer_cv;
	std::condition_variable idle_cv;

	AiSlot slots[2];
	bool registered = false;
	bool unregistering = false;
	std::atomic<bool> active{false};
	std::atomic<bool> fast_ema{true};

	int32_t in_w = 0;
	int32_t in_h = 0;
	int32_t in_type = kDtypeF32;
	std::atomic<int32_t> out_w{0};
	std::atomic<int32_t> out_h{0};
	int32_t out_type = kDtypeF32;
	int64_t seq_counter = 0;

	// Post-processing temporal state (protected by post_mtx).
	std::mutex post_mtx;
	float norm_lo = 0.0f;
	float norm_hi = 1.0f;
	bool norm_valid = false;
	int64_t last_map_ns = 0;
	int64_t last_cut_ns = 0;
	SceneThumb prev_thumb{};
	bool has_prev_thumb = false;
	SceneThumb anchor_thumb{};
	bool anchor_valid = false;
	float anchor_luma_diff = 0.0f;
	float anchor_hist_diff = 0.0f;
	std::vector<float> raw_averaged;
	std::vector<float> averaged;
	std::vector<float> curr_buf;
};

AiSlotMailbox g_mailbox;

void ResetMailboxSessionLocked()
{
	for(int i = 0; i < 2; i++)
	{
		if(g_mailbox.slots[i].state == SlotState::READY_IN
				|| g_mailbox.slots[i].state == SlotState::READY_OUT)
			g_mailbox.slots[i].state = SlotState::FREE;
		g_mailbox.slots[i].seq = 0;
	}
	g_mailbox.seq_counter = 0;
	g_mailbox.infer_cv.notify_all();
}

void ResetPostTemporalState()
{
	std::lock_guard<std::mutex> lock(g_mailbox.post_mtx);
	g_mailbox.norm_lo = 0.0f;
	g_mailbox.norm_hi = 1.0f;
	g_mailbox.norm_valid = false;
	g_mailbox.last_map_ns = 0;
	g_mailbox.last_cut_ns = 0;
	g_mailbox.has_prev_thumb = false;
	g_mailbox.anchor_valid = false;
	g_mailbox.anchor_luma_diff = 0.0f;
	g_mailbox.anchor_hist_diff = 0.0f;
	g_mailbox.raw_averaged.clear();
	g_mailbox.averaged.clear();
}

bool FillInputSlotFromPbo(const uint8_t *mappedRgba)
{
	if(mappedRgba == nullptr)
		return false;

	int slotIdx = -1;
	void *inPtr = nullptr;
	int32_t inW = 0, inH = 0, inType = 0;
	int32_t outW = 0, outH = 0;
	uint8_t *rgbDst = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		if(!g_mailbox.registered || g_mailbox.unregistering
				|| !g_mailbox.active.load(std::memory_order_acquire))
			return false;

		for(int i = 0; i < 2; i++)
		{
			if(g_mailbox.slots[i].state == SlotState::FREE)
			{
				slotIdx = i;
				break;
			}
		}
		if(slotIdx < 0)
		{
			// Drop the older READY_IN slot if inference is busy rather than queuing stale frames.
			int64_t bestSeq = INT64_MAX;
			for(int i = 0; i < 2; i++)
			{
				if(g_mailbox.slots[i].state == SlotState::READY_IN
						&& g_mailbox.slots[i].seq < bestSeq)
				{
					bestSeq = g_mailbox.slots[i].seq;
					slotIdx = i;
				}
			}
		}
		if(slotIdx < 0)
			return false;

		AiSlot &s = g_mailbox.slots[slotIdx];
		inW = g_mailbox.in_w;
		inH = g_mailbox.in_h;
		inType = g_mailbox.in_type;
		outW = g_mailbox.out_w.load(std::memory_order_relaxed);
		outH = g_mailbox.out_h.load(std::memory_order_relaxed);
		s.rgb_u8.resize((size_t)outW * (size_t)outH * 4);
		inPtr = s.in_ptr;
		rgbDst = s.rgb_u8.data();
		s.state = SlotState::FILLING;
	}

	const auto &lut = ByteToFloatLut();
	const int srcStride = kAiFrameW * 4;

	// 1. Convert GL bottom-up RGBA8 -> model top-down RGB input directly into DirectByteBuffer.
	if(inType == kDtypeF32)
	{
		float *dst = reinterpret_cast<float *>(inPtr);
		if(inW == kAiFrameW && inH == kAiFrameH)
		{
			for(int32_t my = 0; my < inH; my++)
			{
				const uint8_t *srcRow = mappedRgba + (size_t)(kAiFrameH - 1 - my) * srcStride;
				float *dstRow = dst + (size_t)my * inW * 3;
				for(int32_t x = 0; x < inW; x++)
				{
					const uint8_t *p = srcRow + x * 4;
					dstRow[x * 3 + 0] = lut[p[0]];
					dstRow[x * 3 + 1] = lut[p[1]];
					dstRow[x * 3 + 2] = lut[p[2]];
				}
			}
		}
		else
		{
			for(int32_t my = 0; my < inH; my++)
			{
				const int32_t sy = kAiFrameH - 1 - (int32_t)((int64_t)my * kAiFrameH / inH);
				const uint8_t *srcRow = mappedRgba + (size_t)sy * srcStride;
				float *dstRow = dst + (size_t)my * inW * 3;
				for(int32_t x = 0; x < inW; x++)
				{
					const int32_t sx = (int32_t)((int64_t)x * kAiFrameW / inW);
					const uint8_t *p = srcRow + sx * 4;
					dstRow[x * 3 + 0] = lut[p[0]];
					dstRow[x * 3 + 1] = lut[p[1]];
					dstRow[x * 3 + 2] = lut[p[2]];
				}
			}
		}
	}
	else
	{
		uint8_t *dst = reinterpret_cast<uint8_t *>(inPtr);
		const int bias = (inType == kDtypeI8) ? -128 : 0;
		for(int32_t my = 0; my < inH; my++)
		{
			const int32_t sy = kAiFrameH - 1 - (int32_t)((int64_t)my * kAiFrameH / inH);
			const uint8_t *srcRow = mappedRgba + (size_t)sy * srcStride;
			uint8_t *dstRow = dst + (size_t)my * inW * 3;
			for(int32_t x = 0; x < inW; x++)
			{
				const int32_t sx = (int32_t)((int64_t)x * kAiFrameW / inW);
				const uint8_t *p = srcRow + sx * 4;
				dstRow[x * 3 + 0] = (uint8_t)((int)p[0] + bias);
				dstRow[x * 3 + 1] = (uint8_t)((int)p[1] + bias);
				dstRow[x * 3 + 2] = (uint8_t)((int)p[2] + bias);
			}
		}
	}

	// 2. Save matching GL bottom-up RGBA8 snapshot for [R, G, B, D] packing & 16x16 cut detection.
	if(outW == kAiFrameW && outH == kAiFrameH)
	{
		std::memcpy(rgbDst, mappedRgba, (size_t)kAiFrameW * kAiFrameH * 4);
	}
	else
	{
		for(int32_t gy = 0; gy < outH; gy++)
		{
			const int32_t sy = (int32_t)((int64_t)gy * kAiFrameH / outH);
			const uint8_t *srcRow = mappedRgba + (size_t)sy * srcStride;
			uint8_t *dstRow = rgbDst + (size_t)gy * outW * 4;
			for(int32_t x = 0; x < outW; x++)
			{
				const int32_t sx = (int32_t)((int64_t)x * kAiFrameW / outW);
				std::memcpy(dstRow + x * 4, srcRow + sx * 4, 4);
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		AiSlot &s = g_mailbox.slots[slotIdx];
		if(g_mailbox.registered && !g_mailbox.unregistering
				&& g_mailbox.active.load(std::memory_order_acquire)
				&& s.state == SlotState::FILLING)
		{
			s.seq = ++g_mailbox.seq_counter;
			s.state = SlotState::READY_IN;
			g_mailbox.infer_cv.notify_one();
			return true;
		}
		if(s.state == SlotState::FILLING)
			s.state = SlotState::FREE;
		g_mailbox.idle_cv.notify_all();
		return false;
	}
}

bool HasReadyOutputSlot()
{
	std::lock_guard<std::mutex> lock(g_mailbox.mtx);
	if(!g_mailbox.registered || g_mailbox.unregistering)
		return false;
	for(int i = 0; i < 2; i++)
	{
		if(g_mailbox.slots[i].state == SlotState::READY_OUT)
			return true;
	}
	return false;
}

bool UploadLatestAiDepthSlot(GLuint targetTex, int32_t targetW, int32_t targetH,
		int32_t effectiveMapsPerSec, std::vector<uint8_t> &rgba8Out)
{
	int slotIdx = -1;
	const void *outPtr = nullptr;
	int32_t outW = 0, outH = 0, outType = 0;
	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		if(!g_mailbox.registered || g_mailbox.unregistering)
			return false;
		int64_t bestSeq = -1;
		for(int i = 0; i < 2; i++)
		{
			if(g_mailbox.slots[i].state == SlotState::READY_OUT
					&& g_mailbox.slots[i].seq > bestSeq)
			{
				bestSeq = g_mailbox.slots[i].seq;
				slotIdx = i;
			}
		}
		if(slotIdx < 0)
			return false;
		outW = g_mailbox.out_w.load(std::memory_order_relaxed);
		outH = g_mailbox.out_h.load(std::memory_order_relaxed);
		outType = g_mailbox.out_type;
		outPtr = g_mailbox.slots[slotIdx].out_ptr;
		if(outPtr == nullptr || outW != targetW || outH != targetH)
		{
			g_mailbox.slots[slotIdx].state = SlotState::FREE;
			g_mailbox.idle_cv.notify_all();
			return false;
		}
		g_mailbox.slots[slotIdx].state = SlotState::UPLOADING;
	}

	bool ok = false;
	{
		std::lock_guard<std::mutex> postLock(g_mailbox.post_mtx);
		const size_t pixels = (size_t)outW * (size_t)outH;
		g_mailbox.curr_buf.resize(pixels);
		float *curr = g_mailbox.curr_buf.data();

		// 1. Flip raw model output vertically into GL bottom-up order.
		if(outType == kDtypeF32)
		{
			const float *raw = reinterpret_cast<const float *>(outPtr);
			for(int32_t gy = 0; gy < outH; gy++)
			{
				const float *srcRow = raw + (size_t)(outH - 1 - gy) * outW;
				float *dstRow = curr + (size_t)gy * outW;
				for(int32_t x = 0; x < outW; x++)
				{
					const float v = srcRow[x];
					dstRow[x] = std::isfinite(v) ? v : NAN;
				}
			}
		}
		else
		{
			const uint8_t *raw = reinterpret_cast<const uint8_t *>(outPtr);
			const float bias = (outType == kDtypeI8) ? -128.0f : 0.0f;
			for(int32_t gy = 0; gy < outH; gy++)
			{
				const uint8_t *srcRow = raw + (size_t)(outH - 1 - gy) * outW;
				float *dstRow = curr + (size_t)gy * outW;
				for(int32_t x = 0; x < outW; x++)
				{
					dstRow[x] = (float)srcRow[x] + bias;
				}
			}
		}

		// 2. Compute timing + 16x16 scene-cut detector first so cuts reset ALL temporal state together.
		const uint8_t *rgb = g_mailbox.slots[slotIdx].rgb_u8.data();
		const int64_t now = NowNs();
		float measuredDt = (g_mailbox.last_map_ns == 0) ? 1.0f
				: (float)(now - g_mailbox.last_map_ns) / 1e9f;
		measuredDt = std::clamp(measuredDt, 0.001f, 0.5f);
		g_mailbox.last_map_ns = now;
		const float nominal = 1.0f / (float)std::max(1, effectiveMapsPerSec);
		const float dt = std::clamp(measuredDt, nominal * 0.8f, nominal * 1.25f);
		const bool fast = g_mailbox.fast_ema.load(std::memory_order_relaxed);
		const float baseAlpha = 1.0f - std::exp(-dt / (fast ? kFastEmaTauSec : kSmoothTauSec));

		SceneThumb thumb{};
		BuildSceneThumb(rgb, outW, outH, thumb);

		float rgbDiff = 0.0f;
		float lumaDiff = 0.0f;
		float histDiff = 0.0f;
		float corr = 1.0f;
		bool cut = false;
		if(g_mailbox.has_prev_thumb)
		{
			CompareSceneThumbs(thumb, g_mailbox.prev_thumb,
					&rgbDiff, &lumaDiff, &histDiff, &corr);
			const bool stepCut = (rgbDiff > 0.22f)
					|| (histDiff > 0.48f)
					|| (lumaDiff > 0.14f && histDiff > 0.32f)
					|| (lumaDiff > 0.08f && histDiff > 0.22f && corr < 0.35f);
			bool fadeCut = false;
			bool flashRecoverCut = false;
			if(g_mailbox.anchor_valid)
			{
				float aRgb = 0.0f, aLum = 0.0f, aHist = 0.0f, aCorr = 1.0f;
				CompareSceneThumbs(thumb, g_mailbox.anchor_thumb,
						&aRgb, &aLum, &aHist, &aCorr);
				fadeCut = !stepCut && aCorr < 0.60f && aLum > 0.08f
						&& (aLum > 2.2f * std::max(0.02f, g_mailbox.anchor_luma_diff)
								|| aHist > 0.24f);
				flashRecoverCut = !stepCut && g_mailbox.anchor_luma_diff > 0.14f
						&& aLum < 0.06f && aHist < 0.20f;
				g_mailbox.anchor_valid = false;
			}
			else if(stepCut)
			{
				g_mailbox.anchor_thumb = g_mailbox.prev_thumb;
				g_mailbox.anchor_valid = true;
				g_mailbox.anchor_luma_diff = lumaDiff;
				g_mailbox.anchor_hist_diff = histDiff;
			}

			const bool cooled = (now - g_mailbox.last_cut_ns) > kSceneCutCooldownNs;
			cut = ((stepCut || fadeCut) && cooled) || flashRecoverCut;
			if(cut)
			{
				g_mailbox.last_cut_ns = now;
				g_mailbox.norm_valid = false;
				LOGI("AI scene cut: rgb=%.3f luma=%.3f hist=%.3f corr=%.2f fade=%d flash=%d",
						rgbDiff, lumaDiff, histDiff, corr, (int)fadeCut, (int)flashRecoverCut);
			}
		}
		g_mailbox.prev_thumb = thumb;
		g_mailbox.has_prev_thumb = true;

		// 3. Temporal EMA on raw model logits BEFORE percentile normalization (prevents
		//    flat-region 2%-98% scale amplification of raw FP16 jitter).
		float lo = INFINITY;
		float hi = -INFINITY;
		if(g_mailbox.raw_averaged.size() != pixels || cut)
		{
			g_mailbox.raw_averaged.assign(curr, curr + pixels);
			for(size_t i = 0; i < pixels; i++)
			{
				const float v = g_mailbox.raw_averaged[i];
				if(std::isfinite(v))
				{
					if(v < lo) lo = v;
					if(v > hi) hi = v;
				}
			}
		}
		else
		{
			const float rawAlpha = std::min(1.0f, std::max(1.0f - std::exp(-dt / kRawEmaTauSec),
					std::max(rgbDiff * 6.0f, histDiff * 2.0f)));
			float *rawAvg = g_mailbox.raw_averaged.data();
			for(size_t i = 0; i < pixels; i++)
			{
				const float v = curr[i];
				if(!std::isfinite(v))
				{
					rawAvg[i] = NAN;
					continue;
				}
				const float prev = rawAvg[i];
				const float next = std::isfinite(prev)
						? (prev + rawAlpha * (v - prev))
						: v;
				rawAvg[i] = next;
				curr[i] = next;
				if(next < lo) lo = next;
				if(next > hi) hi = next;
			}
		}

		// 4. 512-bin 2%-98% percentile stretch with temporally smoothed lo & hi bounds
		//    so flat surfaces do not pump depth up/down on single-bin percentile shifts.
		if(!std::isfinite(lo) || hi <= lo)
		{
			std::fill(curr, curr + pixels, 0.5f);
		}
		else
		{
			constexpr int kBins = 512;
			int hist[kBins] = {};
			int finiteCount = 0;
			const float scale = (float)kBins / (hi - lo);
			for(size_t i = 0; i < pixels; i++)
			{
				const float v = curr[i];
				if(!std::isfinite(v))
					continue;
				int b = (int)((v - lo) * scale);
				if(b < 0) b = 0;
				if(b >= kBins) b = kBins - 1;
				hist[b]++;
				finiteCount++;
			}
			if(finiteCount == 0)
			{
				std::fill(curr, curr + pixels, 0.5f);
			}
			else
			{
				const int loTarget = (int)(finiteCount * 0.02f);
				const int hiTarget = (int)(finiteCount * 0.98f);
				int acc = 0, loBin = 0, hiBin = kBins - 1;
				for(int b = 0; b < kBins; b++)
				{
					acc += hist[b];
					if(acc >= loTarget) { loBin = b; break; }
				}
				acc = 0;
				for(int b = 0; b < kBins; b++)
				{
					acc += hist[b];
					if(acc >= hiTarget) { hiBin = b; break; }
				}
				const float low = lo + (float)loBin / scale;
				const float high = lo + (float)(hiBin + 1) / scale;
				if(!g_mailbox.norm_valid)
				{
					g_mailbox.norm_lo = low;
					g_mailbox.norm_hi = high;
					g_mailbox.norm_valid = true;
				}
				else
				{
					const float prevSpan = std::max(1e-5f, g_mailbox.norm_hi - g_mailbox.norm_lo);
					const float newSpan = std::max(1e-5f, high - low);
					const float jump = std::fabs(newSpan - prevSpan) / prevSpan;
					const float baseBoundsAlpha = 1.0f - std::exp(-dt / kBoundsTauSec);
					const float k = (jump > 0.8f) ? std::max(baseBoundsAlpha, 0.5f) : baseBoundsAlpha;
					g_mailbox.norm_lo += k * (low - g_mailbox.norm_lo);
					g_mailbox.norm_hi += k * (high - g_mailbox.norm_hi);
				}
				const float floorSpan = std::max(std::fabs(g_mailbox.norm_lo) * 1e-4f, 1e-4f);
				const float smoothSpan = std::max(floorSpan, g_mailbox.norm_hi - g_mailbox.norm_lo);
				const float invSpan = 1.0f / smoothSpan;
				const float smoothLo = g_mailbox.norm_lo;
				for(size_t i = 0; i < pixels; i++)
				{
					const float v = curr[i];
					curr[i] = std::isfinite(v) ? Clamp01((v - smoothLo) * invSpan) : 0.5f;
				}
			}
		}

		// 5. Normalized output EMA with small-noise deadzone so flat surfaces lock steady
		//    while fast foreground subjects still track without ghosting.
		float depthMotion = 0.0f;
		if(g_mailbox.averaged.size() == pixels)
		{
			int samples = 0;
			const float *avg = g_mailbox.averaged.data();
			for(size_t i = 0; i < pixels; i += 16)
			{
				depthMotion += std::fabs(curr[i] - avg[i]);
				samples++;
			}
			if(samples > 0)
				depthMotion /= (float)samples;
		}
		const float decorrBoost = (lumaDiff > 0.05f && corr < 0.55f)
				? (0.55f - std::max(corr, -1.0f)) * 0.45f : 0.0f;
		const float motionFloor = std::max(rgbDiff * (fast ? 6.0f : 4.0f),
				std::max(histDiff * (fast ? 2.0f : 1.5f),
						std::max(decorrBoost, depthMotion * 4.0f)));
		const float floorAlpha = std::min(1.0f, std::max(baseAlpha, motionFloor));

		if(g_mailbox.averaged.size() != pixels || cut)
		{
			g_mailbox.averaged.assign(curr, curr + pixels);
		}
		else
		{
			constexpr float kNoiseDeadzone = 0.03f;
			constexpr float kMotionGain = 10.0f;
			float *avg = g_mailbox.averaged.data();
			if(fast)
			{
				for(size_t i = 0; i < pixels; i++)
				{
					const float delta = curr[i] - avg[i];
					const float absD = std::fabs(delta);
					const float excess = absD > kNoiseDeadzone ? (absD - kNoiseDeadzone) : 0.0f;
					const float local = excess * kMotionGain;
					float a = local > floorAlpha ? local : floorAlpha;
					if(a > 1.0f) a = 1.0f;
					avg[i] += a * delta;
				}
			}
			else
			{
				for(size_t i = 0; i < pixels; i++)
					avg[i] += floorAlpha * (curr[i] - avg[i]);
			}
		}

		// 4. Pack [R_u8, G_u8, B_u8, D_u8] into GL_RGBA8 and upload directly (1/4 bandwidth).
		rgba8Out.resize(pixels * 4);
		uint8_t *dst = rgba8Out.data();
		const float *avg = g_mailbox.averaged.data();
		for(size_t i = 0; i < pixels; i++)
		{
			dst[i * 4 + 0] = rgb[i * 4 + 0];
			dst[i * 4 + 1] = rgb[i * 4 + 1];
			dst[i * 4 + 2] = rgb[i * 4 + 2];
			int q = (int)(avg[i] * 255.0f + 0.5f);
			dst[i * 4 + 3] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
		}

		glBindTexture(GL_TEXTURE_2D, targetTex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, outW, outH,
				GL_RGBA, GL_UNSIGNED_BYTE, dst);
		glBindTexture(GL_TEXTURE_2D, 0);
		ok = true;
	}

	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		if(g_mailbox.slots[slotIdx].state == SlotState::UPLOADING)
			g_mailbox.slots[slotIdx].state = SlotState::FREE;
		g_mailbox.idle_cv.notify_all();
	}
	return ok;
}

const char *kAiVertexShader = R"(#version 300 es
in vec2 aPos;
uniform mat4 uTexMatrix;
out vec2 vUv;
void main()
{
	vec2 uv = aPos * 0.5 + 0.5;
	vUv = (uTexMatrix * vec4(uv, 0.0, 1.0)).xy;
	gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

GLuint CompileShaderLocal(GLenum type, const char *source)
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
		LOGE("AI capture shader compile failed: %s", info.c_str());
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

const char *kAiCaptureFragment = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES uTexture;
uniform mat4 uTexMatrix;
in vec2 vUv;
out vec4 fragColor;
void main()
{
	fragColor = texture(uTexture, vUv);
}
)";

struct AiStageCost
{
	std::atomic<int> count{0};
	std::atomic<int64_t> totalNs{0};
	std::atomic<int64_t> maxNs{0};
	void note(int64_t ns)
	{
		count.fetch_add(1);
		totalNs.fetch_add(ns);
		int64_t prev = maxNs.load();
		while(ns > prev && !maxNs.compare_exchange_weak(prev, ns)) {}
	}
	void reset() { count.store(0); totalNs.store(0); maxNs.store(0); }
};

AiStageCost g_cost_collect;
AiStageCost g_cost_capture;
AiStageCost g_cost_drain;
std::atomic<int64_t> g_cost_window_ns{0};
std::atomic<int32_t> g_cost_target_w{0};
std::atomic<int32_t> g_cost_target_h{0};
std::atomic<int> g_stage_count{0};
std::atomic<int64_t> g_stage_total_ns{0};
std::atomic<int64_t> g_stage_max_ns{0};
std::atomic<bool> g_stage_enabled{false};
std::atomic<int> g_stage_rgb_count{0};

struct ScopedAiCost
{
	AiStageCost &stage;
	int64_t startNs;
	explicit ScopedAiCost(AiStageCost &s) : stage(s), startNs(NowNs()) {}
	~ScopedAiCost() { stage.note(NowNs() - startNs); }
};

double AvgMs(const AiStageCost &c)
{
	const int n = c.count.load();
	return n > 0 ? (double)c.totalNs.load() / (double)n / 1000000.0 : 0.0;
}

void AiCostTick(int64_t nowNs)
{
	int64_t expected = 0;
	if(g_cost_window_ns.compare_exchange_strong(expected, nowNs))
		return;
	const int64_t winStart = g_cost_window_ns.load();
	const int64_t elapsed = nowNs - winStart;
	if(elapsed < 3000000000LL) return;
	int64_t prevWin = winStart;
	if(!g_cost_window_ns.compare_exchange_strong(prevWin, nowNs))
		return;
	const bool stageOn = g_stage_enabled.load();
	const int64_t total = g_cost_capture.totalNs.load()
			+ g_cost_drain.totalNs.load()
			+ (stageOn ? 0 : g_cost_collect.totalNs.load());
	const int sCount = g_stage_count.exchange(0);
	const int64_t sTotal = g_stage_total_ns.exchange(0);
	const int64_t sMax = g_stage_max_ns.exchange(0);
	const int rgbCount = g_stage_rgb_count.exchange(0);
	LOGI("AI cost / %.1fs: frame(drain=%d avg=%.2fms max=%.2fms | "
			"capture=%d avg=%.2fms | collect=%d avg=%.2fms) | "
			"stage=%d avg=%.2fms max=%.2fms rgb=%d | target=%dx%d | "
			"frame-budget share=%.1f%% | stage_on=%d",
			(double)elapsed / 1000000000.0,
			g_cost_drain.count.load(), AvgMs(g_cost_drain),
			(double)g_cost_drain.maxNs.load() / 1000000.0,
			g_cost_capture.count.load(), AvgMs(g_cost_capture),
			g_cost_collect.count.load(), AvgMs(g_cost_collect),
			sCount,
			sCount > 0 ? (double)sTotal / (double)sCount / 1000000.0 : 0.0,
			(double)sMax / 1000000.0, rgbCount,
			g_cost_target_w.load(), g_cost_target_h.load(),
			100.0 * (double)total / (double)elapsed,
			(int)stageOn);
	g_cost_collect.reset();
	g_cost_capture.reset();
	g_cost_drain.reset();
}

} // namespace

// Internal helper used by tone_mapper.cpp::DepthTargetSize
void GetAiOutputTargetDimensions(int32_t *outW, int32_t *outH)
{
	const int32_t w = g_mailbox.out_w.load(std::memory_order_acquire);
	const int32_t h = g_mailbox.out_h.load(std::memory_order_acquire);
	*outW = (w >= 8 && h >= 8) ? w : kAiFrameW;
	*outH = (w >= 8 && h >= 8) ? h : kAiFrameH;
}

bool RegisterAiSlotBuffers(void *in0, int64_t inBytes0, void *out0, int64_t outBytes0,
		void *in1, int64_t inBytes1, void *out1, int64_t outBytes1,
		int32_t inW, int32_t inH, int32_t inType,
		int32_t outW, int32_t outH, int32_t outType)
{
	if(!in0 || !out0 || !in1 || !out1 || inW < 8 || inH < 8 || outW < 8 || outH < 8)
		return false;
	if(inType < kDtypeF32 || inType > kDtypeI8 || outType < kDtypeF32 || outType > kDtypeI8)
		return false;
	const int64_t needIn = (int64_t)inW * inH * 3 * ((inType == kDtypeF32) ? 4 : 1);
	const int64_t needOut = (int64_t)outW * outH * ((outType == kDtypeF32) ? 4 : 1);
	if(inBytes0 < needIn || inBytes1 < needIn || outBytes0 < needOut || outBytes1 < needOut)
		return false;

	std::unique_lock<std::mutex> lock(g_mailbox.mtx);
	g_mailbox.unregistering = true;
	g_mailbox.idle_cv.wait(lock, []() {
		for(int i = 0; i < 2; i++)
		{
			if(g_mailbox.slots[i].state == SlotState::FILLING
					|| g_mailbox.slots[i].state == SlotState::UPLOADING)
				return false;
		}
		return true;
	});
	g_mailbox.unregistering = false;
	g_mailbox.slots[0].in_ptr = in0;
	g_mailbox.slots[0].in_bytes = inBytes0;
	g_mailbox.slots[0].out_ptr = out0;
	g_mailbox.slots[0].out_bytes = outBytes0;
	g_mailbox.slots[0].state = SlotState::FREE;
	g_mailbox.slots[0].seq = 0;

	g_mailbox.slots[1].in_ptr = in1;
	g_mailbox.slots[1].in_bytes = inBytes1;
	g_mailbox.slots[1].out_ptr = out1;
	g_mailbox.slots[1].out_bytes = outBytes1;
	g_mailbox.slots[1].state = SlotState::FREE;
	g_mailbox.slots[1].seq = 0;

	g_mailbox.in_w = inW;
	g_mailbox.in_h = inH;
	g_mailbox.in_type = inType;
	g_mailbox.out_w.store(outW, std::memory_order_release);
	g_mailbox.out_h.store(outH, std::memory_order_release);
	g_mailbox.out_type = outType;
	g_mailbox.seq_counter = 0;
	g_mailbox.registered = true;
	g_mailbox.infer_cv.notify_all();
	lock.unlock();

	ResetPostTemporalState();
	return true;
}

void UnregisterAiSlotBuffers()
{
	std::unique_lock<std::mutex> lock(g_mailbox.mtx);
	g_mailbox.unregistering = true;
	g_mailbox.infer_cv.notify_all();
	g_mailbox.idle_cv.wait(lock, []() {
		for(int i = 0; i < 2; i++)
		{
			if(g_mailbox.slots[i].state == SlotState::FILLING
					|| g_mailbox.slots[i].state == SlotState::UPLOADING)
				return false;
		}
		return true;
	});
	for(int i = 0; i < 2; i++)
	{
		g_mailbox.slots[i].in_ptr = nullptr;
		g_mailbox.slots[i].out_ptr = nullptr;
		g_mailbox.slots[i].in_bytes = 0;
		g_mailbox.slots[i].out_bytes = 0;
		g_mailbox.slots[i].state = SlotState::FREE;
		g_mailbox.slots[i].seq = 0;
		g_mailbox.slots[i].rgb_u8.clear();
	}
	g_mailbox.registered = false;
	g_mailbox.unregistering = false;
	g_mailbox.infer_cv.notify_all();
	g_mailbox.idle_cv.notify_all();
}

void SetAiFastEma(bool on)
{
	g_mailbox.fast_ema.store(on, std::memory_order_release);
}

int32_t AcquireAiInputSlot(int32_t timeoutMs)
{
	std::unique_lock<std::mutex> lock(g_mailbox.mtx);
	auto pickNewestInput = []() -> int32_t {
		int32_t best = -1;
		int64_t bestSeq = -1;
		for(int i = 0; i < 2; i++)
		{
			if(g_mailbox.slots[i].state == SlotState::READY_IN
					&& g_mailbox.slots[i].seq > bestSeq)
			{
				bestSeq = g_mailbox.slots[i].seq;
				best = i;
			}
		}
		return best;
	};
	const int32_t clampedMs = std::clamp(timeoutMs, 1, 250);
	g_mailbox.infer_cv.wait_for(lock, std::chrono::milliseconds(clampedMs), [&]() {
		return !g_mailbox.registered || g_mailbox.unregistering
				|| !g_mailbox.active.load(std::memory_order_acquire)
				|| pickNewestInput() >= 0;
	});
	if(!g_mailbox.registered || g_mailbox.unregistering
			|| !g_mailbox.active.load(std::memory_order_acquire))
		return -1;
	const int32_t slot = pickNewestInput();
	if(slot < 0)
		return -1;
	g_mailbox.slots[slot].state = SlotState::INFERRING;
	return slot;
}

void ReleaseAiInputSlot(int32_t slot)
{
	if(slot < 0 || slot > 1)
		return;
	std::lock_guard<std::mutex> lock(g_mailbox.mtx);
	if(g_mailbox.slots[slot].state == SlotState::INFERRING)
	{
		g_mailbox.slots[slot].state = SlotState::FREE;
		g_mailbox.idle_cv.notify_all();
	}
}

void SubmitAiOutputSlot(int32_t slot)
{
	if(slot < 0 || slot > 1)
		return;
	bool shouldWake = false;
	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		if(g_mailbox.slots[slot].state == SlotState::INFERRING)
		{
			if(g_mailbox.registered && !g_mailbox.unregistering
					&& g_mailbox.active.load(std::memory_order_acquire))
			{
				g_mailbox.slots[slot].seq = ++g_mailbox.seq_counter;
				g_mailbox.slots[slot].state = SlotState::READY_OUT;
				shouldWake = true;
			}
			else
			{
				g_mailbox.slots[slot].state = SlotState::FREE;
				g_mailbox.idle_cv.notify_all();
			}
		}
	}
	if(shouldWake)
		NotifyActiveStage();
}

void ToneMapper::WakeStage()
{
	{
		std::lock_guard<std::mutex> lock(stage_mutex_);
		stage_wake_ = true;
	}
	stage_cv_.notify_all();
}

void ToneMapper::SetAiDepthMode(bool on, int32_t mapsPerSecond)
{
	const int32_t maps = mapsPerSecond > 0 ? mapsPerSecond : kDefaultMapsPerSecond;
	ai_maps_per_sec_.store(std::min(maps, 60), std::memory_order_release);
	ai_capture_period_ns_.store(kNsPerSecond / std::min(maps, 60));
	ai_mode_.store(on);
	ai_uploaded_.store(false);
	g_mailbox.active.store(on, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		ResetMailboxSessionLocked();
	}
	if(!on)
	{
		ResetPostTemporalState();
	}
	WakeStage();
	LOGI("AI depth mode: %s (%d maps/s)", on ? "on" : "off", maps);
}

void ToneMapper::SetAiCaptureRate(int32_t mapsPerSecond)
{
	const int32_t maps = std::clamp(mapsPerSecond, 4, 60);
	ai_maps_per_sec_.store(maps, std::memory_order_release);
	ai_capture_period_ns_.store(kNsPerSecond / maps, std::memory_order_release);
}

int32_t ToneMapper::AiFrameWidth() const
{
	return kAiFrameW;
}

int32_t ToneMapper::AiFrameHeight() const
{
	return kAiFrameH;
}

bool ToneMapper::EnsureAiCaptureTargets()
{
	if(ai_tex_ != 0 && ai_fbo_ != 0 && ai_program_ != 0)
		return true;

	if(ai_program_ == 0)
	{
		GLuint vs = CompileShaderLocal(GL_VERTEX_SHADER, kAiVertexShader);
		GLuint fs = vs ? CompileShaderLocal(GL_FRAGMENT_SHADER, kAiCaptureFragment) : 0;
		if(vs && fs)
		{
			GLuint p = glCreateProgram();
			glAttachShader(p, vs);
			glAttachShader(p, fs);
			glBindAttribLocation(p, 0, "aPos");
			glLinkProgram(p);
			GLint ok = GL_FALSE;
			glGetProgramiv(p, GL_LINK_STATUS, &ok);
			if(ok == GL_TRUE)
			{
				ai_program_ = p;
				ai_loc_tex_matrix_ = glGetUniformLocation(p, "uTexMatrix");
				ai_loc_sampler_ = glGetUniformLocation(p, "uTexture");
			}
			else
			{
				LOGE("Failed to link the AI capture program");
				glDeleteProgram(p);
			}
		}
		if(vs) glDeleteShader(vs);
		if(fs) glDeleteShader(fs);
		if(ai_program_ == 0)
			return false;
	}

	if(ai_tex_ == 0)
	{
		glGenTextures(1, &ai_tex_);
		glBindTexture(GL_TEXTURE_2D, ai_tex_);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kAiFrameW, kAiFrameH);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		glGenFramebuffers(1, &ai_fbo_);
		glBindFramebuffer(GL_FRAMEBUFFER, ai_fbo_);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, ai_tex_, 0);
		const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if(!ok)
		{
			LOGE("AI capture framebuffer incomplete");
			glDeleteTextures(1, &ai_tex_);
			glDeleteFramebuffers(1, &ai_fbo_);
			ai_tex_ = ai_fbo_ = 0;
			return false;
		}
	}

	for(int i = 0; i < kAiPboCount; i++)
	{
		if(ai_pbo_[i] == 0)
		{
			glGenBuffers(1, &ai_pbo_[i]);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, ai_pbo_[i]);
			glBufferData(GL_PIXEL_PACK_BUFFER,
					(GLsizeiptr)kAiFrameW * kAiFrameH * 4, nullptr, GL_STREAM_READ);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
	}
	return true;
}

void ToneMapper::CollectAiRgbPbo()
{
	std::lock_guard<std::mutex> collectLock(ai_collect_mutex_);
	const int64_t tickNs = NowNs();
	AiCostTick(tickNs);
	ScopedAiCost cost(g_cost_collect);

	// Process older/newer PBOs; if both signaled in same tick, harvest newest last.
	int order[2] = {0, 1};
	if(ai_pbo_seq_[0].load() > ai_pbo_seq_[1].load())
	{
		order[0] = 1;
		order[1] = 0;
	}
	const size_t bytes = (size_t)kAiFrameW * kAiFrameH * 4;
	for(int k = 0; k < kAiPboCount; k++)
	{
		const int i = order[k];
		if(!ai_pbo_pending_[i].load())
			continue;
		GLsync fence = ai_pbo_fence_[i].load();
		if(fence != nullptr)
		{
			GLint status = GL_UNSIGNALED;
			GLsizei written = 0;
			glGetSynciv(fence, GL_SYNC_STATUS, 1, &written, &status);
			if(status == GL_UNSIGNALED)
				continue;
			GLsync expected = fence;
			if(ai_pbo_fence_[i].compare_exchange_strong(expected, nullptr))
				glDeleteSync(fence);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, ai_pbo_[i]);
		const uint8_t *mapped = reinterpret_cast<const uint8_t *>(
				glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT));
		if(mapped != nullptr)
		{
			if(FillInputSlotFromPbo(mapped))
				g_stage_rgb_count.fetch_add(1);
			glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		ai_pbo_pending_[i].store(false);
	}
}

bool ToneMapper::CaptureAiRgb(const float *matrix)
{
	ScopedAiCost cost(g_cost_capture);
	if(!EnsureAiCaptureTargets())
		return false;

	int slot = -1;
	for(int k = 0; k < kAiPboCount; k++)
	{
		const int s = (ai_pbo_capture_slot_ + k) % kAiPboCount;
		if(!ai_pbo_pending_[s].load())
		{
			slot = s;
			break;
		}
	}
	if(slot < 0)
		return false;
	ai_pbo_capture_slot_ = (slot + 1) % kAiPboCount;

	GLint previous_fbo = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ai_fbo_);
	glViewport(0, 0, kAiFrameW, kAiFrameH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glUseProgram(ai_program_);
	glUniformMatrix4fv(ai_loc_tex_matrix_, 1, GL_FALSE, matrix);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, external_tex_);
	glUniform1i(ai_loc_sampler_, 0);
	glBindVertexArray(vao_);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	glBindBuffer(GL_PIXEL_PACK_BUFFER, ai_pbo_[slot]);
	glReadPixels(0, 0, kAiFrameW, kAiFrameH,
			GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

	GLsync previous = ai_pbo_fence_[slot].exchange(nullptr);
	if(previous != nullptr) glDeleteSync(previous);
	GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	ai_pbo_fence_[slot].store(fence);
	glFlush();

	const GLenum err = glGetError();
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);

	if(err != GL_NO_ERROR || fence == nullptr)
	{
		if(!logged_ai_failure_)
		{
			logged_ai_failure_ = true;
			LOGE("AI capture setup failed (GL 0x%x, fence=%p)", err, (void *)fence);
		}
		GLsync failed = ai_pbo_fence_[slot].exchange(nullptr);
		if(failed != nullptr) glDeleteSync(failed);
		ai_pbo_pending_[slot].store(false);
		return false;
	}
	ai_pbo_seq_[slot].store(ai_pbo_seq_next_.fetch_add(1) + 1);
	ai_pbo_pending_[slot].store(true);
	WakeStage();
	return true;
}

void ToneMapper::AiCaptureTick(const float *matrix)
{
	if(!ai_mode_.load() || stereo_mode_ != 1)
	{
		ai_last_tick_ns_ = 0;
		ai_capture_accum_ns_ = 0;
		return;
	}
	const int64_t now = NowNs();
	int64_t dt = (ai_last_tick_ns_ == 0) ? 0 : (now - ai_last_tick_ns_);
	ai_last_tick_ns_ = now;
	if(dt < 0) dt = 0;
	if(dt > 100000000LL) dt = 100000000LL;
	const int64_t period = ai_capture_period_ns_.load();
	ai_capture_accum_ns_ += dt;
	if(ai_capture_accum_ns_ < period)
		return;
	if(CaptureAiRgb(matrix))
	{
		ai_capture_accum_ns_ -= period;
		if(ai_capture_accum_ns_ > period)
			ai_capture_accum_ns_ = 0;
		ai_last_capture_ns_ = now;
	}
}

bool ToneMapper::DrainAiDepth(int32_t width, int32_t height)
{
	ScopedAiCost cost(g_cost_drain);
	if(!HasReadyOutputSlot())
		return false;
	if(!EnsureDepthTargets(width, height))
		return false;
	const int32_t dw = depth_width_;
	const int32_t dh = depth_height_;
	if(dw < 8 || dh < 8)
		return false;
	const int32_t destino = 1 - depth_newest_;
	static std::vector<uint8_t> fallbackRgba8;
	if(!UploadLatestAiDepthSlot(depth_tex_[destino], dw, dh,
			ai_maps_per_sec_.load(std::memory_order_acquire), fallbackRgba8))
		return false;
	depth_newest_ = destino;
	depth_has_prev_ = true;
	ai_uploaded_ = true;
	g_cost_target_w.store(dw);
	g_cost_target_h.store(dh);
	ai_last_upload_ns_ = NowNs();
	return true;
}

bool ToneMapper::InitStageEgl(EGLDisplay display, EGLSurface surface, EGLContext context)
{
	if(display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE
			|| context == EGL_NO_CONTEXT || stage_thread_.joinable())
		return false;
	stage_display_ = display;
	stage_surface_ = surface;
	stage_context_ = context;
	stage_running_.store(true);
	SetActiveStageToneMapper(this);
	try
	{
		stage_thread_ = std::thread(&ToneMapper::StageThread, this);
	}
	catch(const std::exception &e)
	{
		LOGE("AI stage thread start failed: %s", e.what());
		SetActiveStageToneMapper(nullptr);
		stage_running_.store(false);
		return false;
	}
	LOGI("AI stage thread launched");
	return true;
}

void ToneMapper::StopStage()
{
	SetActiveStageToneMapper(nullptr);
	if(!stage_thread_.joinable())
		return;
	stage_running_.store(false);
	WakeStage();
	stage_thread_.join();
	stage_enabled_.store(false);
	g_stage_enabled.store(false);
	stage_display_ = EGL_NO_DISPLAY;
	stage_surface_ = EGL_NO_SURFACE;
	stage_context_ = EGL_NO_CONTEXT;
	LOGI("AI stage thread stopped");
}

void ToneMapper::StageThread()
{
	if(!eglMakeCurrent(stage_display_, stage_surface_, stage_surface_, stage_context_))
	{
		LOGE("AI stage eglMakeCurrent failed: 0x%x; depth work stays on the frame loop",
				eglGetError());
		stage_enabled_.store(false);
		g_stage_enabled.store(false);
		stage_running_.store(false);
		return;
	}
	LOGI("AI stage shared EGL context bound");
	setpriority(PRIO_PROCESS, 0, -4);
	stage_enabled_.store(true);
	g_stage_enabled.store(true);

	while(stage_running_.load())
	{
		const bool anyPbo = ai_pbo_pending_[0].load() || ai_pbo_pending_[1].load();
		const auto waitDur = anyPbo ? std::chrono::milliseconds(2) : std::chrono::milliseconds(20);
		{
			std::unique_lock<std::mutex> lock(stage_mutex_);
			stage_cv_.wait_for(lock, waitDur, [this] {
				return stage_wake_ || !stage_running_.load();
			});
			stage_wake_ = false;
		}
		if(!stage_running_.load())
			break;
		StageWorkOnce();
	}

	GLsync stale = stage_fence_.exchange(nullptr);
	if(stale != nullptr) glDeleteSync(stale);
	stale = stage_retired_fence_.exchange(nullptr);
	if(stale != nullptr) glDeleteSync(stale);
	eglMakeCurrent(stage_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	stage_enabled_.store(false);
	g_stage_enabled.store(false);
	LOGI("AI stage thread exited");
}

void ToneMapper::StageWorkOnce()
{
	GLsync retired = stage_retired_fence_.exchange(nullptr);
	if(retired != nullptr) glDeleteSync(retired);

	CollectAiRgbPbo();
	if(!ai_mode_.load() || stereo_mode_ != 1)
		return;
	if(stage_published_dest_.load() >= 0)
		return;
	if(!HasReadyOutputSlot())
		return;

	std::lock_guard<std::mutex> targetLock(stage_targets_mutex_);
	StageTargets t = stage_targets_;
	if(t.tex[0] == 0 || t.tex[1] == 0 || t.w < 8 || t.h < 8 || t.format != GL_RGBA8)
		return;

	const int64_t startNs = NowNs();
	const int dest = std::clamp(stage_write_dest_.load(), 0, 1);
	if(!UploadLatestAiDepthSlot(t.tex[dest], t.w, t.h,
			ai_maps_per_sec_.load(std::memory_order_acquire), stage_rgba8_))
		return;

	GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
	if(fence == nullptr)
	{
		LOGW("AI stage fence failed; falling back to the frame-loop drain");
		stage_enabled_.store(false);
		g_stage_enabled.store(false);
		stage_running_.store(false);
		return;
	}
	stage_fence_.store(fence);
	stage_published_dest_.store(dest);
	stage_published_ns_.store(NowNs());

	const int64_t ns = NowNs() - startNs;
	g_stage_count.fetch_add(1);
	g_stage_total_ns.fetch_add(ns);
	int64_t prevMax = g_stage_max_ns.load();
	while(ns > prevMax && !g_stage_max_ns.compare_exchange_weak(prevMax, ns)) {}
	g_cost_target_w.store(t.w);
	g_cost_target_h.store(t.h);
}

void ToneMapper::StageAdopt(int32_t width, int32_t height)
{
	if(!ai_mode_.load() || stereo_mode_ != 1)
	{
		std::lock_guard<std::mutex> targetLock(stage_targets_mutex_);
		stage_targets_ = StageTargets{};
		stage_neutral_tex_ = 0;
		GLsync old = stage_fence_.exchange(nullptr);
		stage_published_dest_.store(-1);
		stage_published_ns_.store(0);
		if(old != nullptr)
		{
			GLsync retired = stage_retired_fence_.exchange(old);
			if(retired != nullptr) glDeleteSync(retired);
		}
		return;
	}
	if(!EnsureDepthTargets(width, height))
		return;
	if(stage_neutral_tex_ != depth_tex_[0])
	{
		std::lock_guard<std::mutex> targetLock(stage_targets_mutex_);
		GLsync stale = stage_fence_.exchange(nullptr);
		stage_published_dest_.store(-1);
		stage_published_ns_.store(0);
		if(stale != nullptr)
		{
			GLsync retired = stage_retired_fence_.exchange(stale);
			if(retired != nullptr) glDeleteSync(retired);
		}
		const size_t pixels = (size_t)depth_width_ * depth_height_;
		if(ai_depth_format_ == GL_RGBA16F)
		{
			std::vector<float> neutral(pixels * 4);
			for(size_t i = 0; i < pixels; i++)
			{
				neutral[i * 4 + 0] = 0.0f;
				neutral[i * 4 + 1] = 0.0f;
				neutral[i * 4 + 2] = 0.0f;
				neutral[i * 4 + 3] = 0.5f;
			}
			for(int i = 0; i < 2; i++)
			{
				glBindTexture(GL_TEXTURE_2D, depth_tex_[i]);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
						depth_width_, depth_height_, GL_RGBA, GL_FLOAT, neutral.data());
			}
		}
		else
		{
			// [R, G, B, D] neutral seed: RGB=0, D=128 (~0.5)
			std::vector<uint8_t> neutral(pixels * 4);
			for(size_t i = 0; i < pixels; i++)
			{
				neutral[i * 4 + 0] = 0;
				neutral[i * 4 + 1] = 0;
				neutral[i * 4 + 2] = 0;
				neutral[i * 4 + 3] = 128;
			}
			for(int i = 0; i < 2; i++)
			{
				glBindTexture(GL_TEXTURE_2D, depth_tex_[i]);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
						depth_width_, depth_height_, GL_RGBA,
						GL_UNSIGNED_BYTE, neutral.data());
			}
		}
		glBindTexture(GL_TEXTURE_2D, 0);
		stage_neutral_tex_ = depth_tex_[0];
		stage_targets_.tex[0] = depth_tex_[0];
		stage_targets_.tex[1] = depth_tex_[1];
		stage_targets_.w = depth_width_;
		stage_targets_.h = depth_height_;
		stage_targets_.format = ai_depth_format_;
		stage_write_dest_.store(1 - depth_newest_);
	}

	const int dest = stage_published_dest_.load();
	if(dest < 0 || dest > 1)
		return;
	GLsync fence = stage_fence_.load();
	if(fence == nullptr)
	{
		stage_published_dest_.store(-1);
		return;
	}
	GLint status = GL_UNSIGNALED;
	GLsizei written = 0;
	glGetSynciv(fence, GL_SYNC_STATUS, 1, &written, &status);
	if(status == GL_UNSIGNALED)
	{
		if(NowNs() - stage_published_ns_.load() > 2000000000LL)
		{
			stage_published_dest_.store(-1);
			stage_fence_.store(nullptr);
			GLsync oldRetired = stage_retired_fence_.exchange(fence);
			if(oldRetired != nullptr) glDeleteSync(oldRetired);
		}
		return;
	}
	stage_published_dest_.store(-1);
	stage_fence_.store(nullptr);
	GLsync oldRetired = stage_retired_fence_.exchange(fence);
	if(oldRetired != nullptr) glDeleteSync(oldRetired);
	if(status == GL_SIGNALED)
	{
		depth_newest_ = dest;
		depth_has_prev_ = true;
		ai_uploaded_ = true;
		ai_last_upload_ns_ = NowNs();
		stage_write_dest_.store(1 - dest);
	}
	WakeStage();
}

void ToneMapper::AiFrameStep(const float *matrix, int32_t width, int32_t height)
{
	const int64_t now = NowNs();
	const bool stale = ai_last_upload_ns_ != 0
			&& now - ai_last_upload_ns_ > 2000000000LL;
	if((!ai_uploaded_ || stale) && !stage_enabled_.load())
		RenderDepth(matrix, width, height);
}

void ToneMapper::DestroyAiTargets()
{
	if(ai_program_ != 0)
		glDeleteProgram(ai_program_);
	if(ai_tex_ != 0)
		glDeleteTextures(1, &ai_tex_);
	if(ai_fbo_ != 0)
		glDeleteFramebuffers(1, &ai_fbo_);
	for(int i = 0; i < kAiPboCount; i++)
	{
		if(ai_pbo_[i] != 0)
			glDeleteBuffers(1, &ai_pbo_[i]);
		ai_pbo_[i] = 0;
		ai_pbo_pending_[i].store(false);
		GLsync fence = ai_pbo_fence_[i].exchange(nullptr);
		if(fence != nullptr) glDeleteSync(fence);
	}
	ai_program_ = ai_tex_ = ai_fbo_ = 0;
	ai_depth_format_ = 0;
	ai_mode_ = false;
	g_mailbox.active.store(false, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(g_mailbox.mtx);
		ResetMailboxSessionLocked();
	}
}

} // namespace p5m
