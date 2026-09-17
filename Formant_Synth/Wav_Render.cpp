// ─────────────────────────────────────────────────────────────
//  Wav_Render.cpp —— 段表 → wav（**本机合成，不走 MIDI**）
//
//  定位（2026-09-18，方案 A：段表共用）：
//    与 `Formant_Synth.cpp`（段表 → 17 轨事件）**并列的第二个后端**。
//    段表由 `Formant_Synth.cpp` 的 SegBuilder 生成，两个后端吃同一份 ——
//    所以「段表对不对」和「承载方式对不对」是两个可以分开验的问题。
//
//  ── 移植自哪里 ─────────────────────────────────────────────
//    `cache\audio\Formant_Audio.cpp`（模式 A：一个声门源 → 8 个并联谐振器）。
//    那套已经验过（G1 报告：`/i/` 8 个共振峰 ≤2.5%），这里只做两件事：
//      ① 由**段表**驱动（而不是手写的样本表），所以能渲整首歌
//      ② 段与段之间**连续**（不分段重新起振）—— 否则元音被 kSeg=4 切成 5 段会听出拼接
//
//  ── 刻意保持的不确定性（别当已定稿）─────────────────────────
//    · 共振峰**带宽**：字典里没有这个字段（已知缺口），
//      这里用 Klatt 一系的典型量级表 kBandwidth（**非字典值**）
//    · 声门源：**平谱脉冲串**（各谐波等幅）—— 这是测量配置，不是自然嗓音
//    · 段间增益变化用 ~10 ms 线性过渡（避免"咔"），只在力度变化处
//
//  编译：由 CMakeLists.txt 列进 formant_synth / Synth_Gui 两个目标。
// ─────────────────────────────────────────────────────────────

#include "Synth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace SYNTH {

	namespace {

		const double kPi = 3.14159265358979323846;
		const int    kFs = 44100;

		/// 共振峰带宽（Hz）—— ★ **不是字典值**（字典没有这个字段）。
		/// 与 `Formant_Audio.cpp` 的 kBandwidth 一致，改一边就要改另一边。
		const double kBandwidth[8] = { 60, 90, 120, 180, 240, 300, 360, 420 };

		/// F1..F8 的响度配比 —— 必须与 `Formant_Synth.cpp` 的 kCc7[1..8] 一致
		/// （协议 7.3 的表：100/90/80/70/60/55/50/45）。
		/// 2026-09-18 hotfix：MIDI 后端已按"每通道 velocity × CC7/100"落地，WAV 后端要跟上，
		/// 否则两个后端量出来的元音平衡会不一样。
		const double kFracGain[8] = { 1.00, 0.90, 0.80, 0.70, 0.60, 0.55, 0.50, 0.45 };

		/// 源 → 8 个并联谐振器，峰值增益归一。
		struct Resonator {
			double b = 0, c = 0, a = 1, y1 = 0, y2 = 0;
			void Set(double F, double BW)
			{
				if (F < 1.0)        F = 1.0;
				if (F > kFs * 0.45) F = kFs * 0.45;
				if (BW < 1.0)       BW = 1.0;
				const double r  = std::exp(-kPi * BW / kFs);
				const double th = 2.0 * kPi * F / kFs;
				b = 2.0 * r * std::cos(th);
				c = -r * r;
				a = 1.0 - b - c;                 // DC 增益 = 1
			}
			double operator()(double x)
			{
				const double y = a * x + b * y1 + c * y2;
				y2 = y1; y1 = y;
				return y;
			}
		};

		/// 确定性噪声（xorshift32）——★ 不能用 LCG 取低位：
		/// 第 k 位周期 2^k，取 bit8 起会在 172 Hz 造出一条假线谱（G1 报告踩过）。
		struct Noise {
			std::uint32_t s = 0x12345678u;
			double operator()()
			{
				s ^= s << 13; s ^= s >> 17; s ^= s << 5;
				return static_cast<double>(s >> 8) * (2.0 / 16777216.0) - 1.0;
			}
		};

		/// 单极点带通（噪声段用）：中心频率可随段变化。
		/// 用"两段一阶"的简化写法 —— 目标是**噪声带的位置**，不是高 Q 谐振。
		struct BandPass {
			double lp1 = 0, lp2 = 0, hp = 0, k = 0;
			void Reset() { lp1 = lp2 = hp = 0; }
			double operator()(double x, double fc)
			{
				if (fc < 50.0)     fc = 50.0;
				if (fc > kFs*0.45) fc = kFs * 0.45;
				const double w = std::exp(-2.0 * kPi * fc / kFs);
				lp1 = (1.0 - w) * x + w * lp1;
				lp2 = (1.0 - w) * lp1 + w * lp2;
				hp  = lp1 - lp2;                     // 粗带通：一阶低通之差
				return hp * 4.0;
			}
		};

		bool WriteWav16(const std::string& path, const std::vector<double>& xIn)
		{
			std::vector<double> x = xIn;
			double peak = 0.0;
			for (double v : x) peak = std::max(peak, std::fabs(v));
			const double target = 0.707945784;                 // −3 dBFS
			if (peak > 1e-12) {
				const double g = target / peak;
				for (double& v : x) v *= g;
			}
			std::ofstream f(path, std::ios::binary);
			if (!f) return false;
			const std::uint32_t n = static_cast<std::uint32_t>(x.size());
			const std::uint32_t dataBytes = n * 2;
			auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) f.put(static_cast<char>((v >> (8*i)) & 0xFF)); };
			auto u16 = [&](std::uint16_t v) { for (int i = 0; i < 2; ++i) f.put(static_cast<char>((v >> (8*i)) & 0xFF)); };
			f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
			f.write("fmt ", 4); u32(16); u16(1); u16(1);
			u32(kFs); u32(kFs * 2); u16(2); u16(16);
			f.write("data", 4); u32(dataBytes);
			for (double v : x) {
				double s = v * 32767.0;
				if (s >  32767.0) s =  32767.0;
				if (s < -32768.0) s = -32768.0;
				u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(s))));
			}
			return true;
		}

	} // namespace

	bool WriteWavFromSegments(const std::string& path, const SegmentTable& t, std::string& err)
	{
		if (t.segs.empty()) { err = "段表是空的"; return false; }

		// 总时长：最后一段的末尾 + 200 ms 尾巴
		const Segment& last = t.segs.back();
		const double msPerTickLast = (last.msPerTick > 0.0) ? last.msPerTick : (500.0 / 480.0);
		const double totalMs = static_cast<double>(last.t1) * msPerTickLast + 200.0;
		const std::size_t total = static_cast<std::size_t>(totalMs * kFs / 1000.0) + 1;
		std::vector<double> out(total, 0.0);

		Resonator rs[8];
		Noise     nz;
		BandPass  bp;
		double phase = 0.0;                       // 脉冲相位（连续，不逐段重置）
		double gain  = 0.0;                       // 当前增益（力度/127），段间线性过渡
		double gainTarget = 0.0;
		const std::size_t rampN = static_cast<std::size_t>(0.010 * kFs);   // 10 ms

		for (const Segment& s : t.segs) {
			const double mpt = (s.msPerTick > 0.0) ? s.msPerTick : (500.0 / 480.0);
			const std::size_t a = static_cast<std::size_t>(static_cast<double>(s.t0) * mpt * kFs / 1000.0);
			const std::size_t b = static_cast<std::size_t>(static_cast<double>(s.t1) * mpt * kFs / 1000.0);
			if (b <= a || a >= out.size()) continue;
			const std::size_t end = std::min(b, out.size());
			const std::size_t n = end - a;

			gainTarget = s.vel / 127.0;

			if (s.kind == SegKind::Closure) {
				// 成阻 = 真静音；同时把谐振器状态清掉（成阻后重新起振，才有"爆发"感）
				for (std::size_t i = a; i < end; ++i) out[i] = 0.0;
				for (int k = 0; k < 8; ++k) rs[k] = Resonator{};
				bp.Reset();
				gain = 0.0;
				continue;
			}

			const bool isNoise = (s.kind == SegKind::Noise || s.kind == SegKind::Burst);
			const double fc = (s.noiseHz > 0.0) ? s.noiseHz : 4000.0;

			for (std::size_t i = 0; i < n; ++i) {
				const double u = (n > 1) ? static_cast<double>(i) / static_cast<double>(n - 1) : 1.0;

				// 增益过渡（10 ms 线性）—— 只在力度变化处才拉
				if (gain != gainTarget) {
					const double step = 1.0 / static_cast<double>(std::max<std::size_t>(1, rampN));
					gain += std::max(-step, std::min(step, gainTarget - gain));
				}

				double src;
				if (isNoise) {
					src = bp(nz(), fc);
				} else {
					// 平谱脉冲串：每周期一个单位脉冲
					const double f0 = (s.f0 > 1.0) ? s.f0 : 1.0;
					const double inc = f0 / kFs;
					phase += inc;
					if (phase >= 1.0) phase -= 1.0;
					src = (phase < inc) ? 1.0 : 0.0;
				}

				// 谐振器中心频率：段内 f8a → f8b 线性插值，每 64 采样重设一次
				if ((i & 63u) == 0u) {
					for (int k = 0; k < 8; ++k) {
						const double f = s.f8a[k] + (s.f8b[k] - s.f8a[k]) * u;
						rs[k].Set(f, kBandwidth[k]);
					}
				}

				double acc = 0.0;
				for (int k = 0; k < 8; ++k) acc += rs[k](src) * kFracGain[k];
				out[a + i] += (acc / 8.0) * gain;
			}
		}

		// 首尾各 5 ms 淡入淡出（防咔哒）
		const std::size_t fade = static_cast<std::size_t>(0.005 * kFs);
		if (out.size() > 2 * fade) {
			for (std::size_t i = 0; i < fade; ++i) {
				const double w = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(i) / static_cast<double>(fade));
				out[i] *= w;
				out[out.size()-1-i] *= w;
			}
		}

		if (!WriteWav16(path, out)) { err = "写 wav 失败（路径/权限？）"; return false; }
		return true;
	}

} // namespace SYNTH
