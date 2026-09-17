#include "Pitch_Mapper.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile：按 UTF-8 路径打开文件
#include <fstream>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace {
	// 弯音轮的取值范围与中心（协议第 11 章：BEND_CENTER = 8192）
	const int kCenter = 8192;

	// 只处理 MIDI 通道 1 → 状态字节的低 4 位为 0
	const std::uint8_t kChannelIndex = 0;

	// MThd 不可用时给一个保守的 PPQN，只影响内部 tick → 四分音符的除法
	const int kFallbackPpqn = 480;

	// 小节线的数量上限：防止异常文件（拍号或长度离谱）把内存吃光
	const int kMaxBars = 200000;

	/// 按 MIDI 系统取「文件无 RPN 时的默认弯音轮范围」。
	/// GS / XG = 12 半音；GM / GM2 / 未知 = 2 半音（协议 4.6.1 / 8.1）。
	int defaultRange(const std::string& system)
	{
		if (system == "GS" || system == "XG")
			return 12;
		return 2;                      // GM / GM2 / 未知
	}

	/// 音分 = R × (弯音值 − 8192) / 8192 × 100，四舍五入到整数音分。
	int CentsOf(int R, int bendValue)
	{
		if (R < 1)
			R = 1;                     // 避免除零；协议 4.6 也把范围 clamp 在 1..24
		const double c =
			static_cast<double>(R) * (bendValue - kCenter) / kCenter * 100.0;
		return static_cast<int>(c >= 0.0 ? c + 0.5 : c - 0.5);
	}

	/// 从 MThd 取 PPQN（公共实现见 FS::ReadPpqn）。
	int readPpqn(std::ifstream& f)
	{
		return FS::ReadPpqn(f, kFallbackPpqn);
	}

	// ── 解析期的原始数据：用 tick 记录，最后统一换算成四分音符 ──

	struct RawRange {
		int tick = 0;
		int semitones = 12;
	};

	struct RawBend {
		int tick = 0;
		int value = kCenter;           ///< 原始弯音值（0..16383），不是音分
	};

	struct RawData {
		int ppqn = kFallbackPpqn;
		int tickEnd = 0;               ///< 见到的 end_of_track 里的最大 tick
		int maxTick = 0;               ///< 见到过的最大 tick（比 tickEnd 更可靠的长度参考）
		std::vector<RawRange> ranges;  ///< 已按 tick 升序、已去重、已补默认值
		std::vector<RawBend>  bends;   ///< 已按 tick 升序、已去重
	};

	/// 扫描全部轨，只收集通道 1 的 RPN(0,0) 与弯音轮消息。
	RawData scan(const std::string& path, int fallbackR)
	{
		RawData raw;

		std::ifstream f = FS::OpenFile(path);
		if (!f)
			return raw;

		raw.ppqn = readPpqn(f);
		if (raw.ppqn <= 0)
			return raw;

		std::uint8_t ch[8];
		while (f.read(reinterpret_cast<char*>(ch), 8) && f.gcount() == 8) {
			if (!(ch[0] == 'M' && ch[1] == 'T' && ch[2] == 'r' && ch[3] == 'k'))
				break;

			const std::uint32_t len =
				(static_cast<std::uint32_t>(ch[4]) << 24) |
				(static_cast<std::uint32_t>(ch[5]) << 16) |
				(static_cast<std::uint32_t>(ch[6]) << 8) |
				 static_cast<std::uint32_t>(ch[7]);

			std::vector<std::uint8_t> track(len);
			f.read(reinterpret_cast<char*>(track.data()), static_cast<std::streamsize>(len));
			if (f.gcount() != static_cast<std::streamsize>(len))
				break;

			std::size_t pos = 0;
			int absTick = 0;
			std::uint8_t running = 0;
			bool hasRunning = false;

			// RPN(0,0) 状态机（协议 4.6：严格 CC101 → CC100 → CC6）
			int rpnMsb = -1;
			int rpnLsb = -1;
			bool rpnSelected = false;

			while (pos < track.size()) {
				// delta time（可变长量）
				std::uint32_t delta = 0;
				while (pos < track.size()) {
					const std::uint8_t b = track[pos];
					++pos;
					delta = (delta << 7) | (b & 0x7F);
					if ((b & 0x80) == 0)
						break;
				}
				absTick += static_cast<int>(delta);
				if (absTick > raw.maxTick)
					raw.maxTick = absTick;
				if (pos >= track.size())
					break;

				std::uint8_t status;
				if ((track[pos] & 0x80) != 0) {
					status = track[pos];
					++pos;
				} else {
					if (!hasRunning)
						break;
					status = running;
				}

				if (status != 0xF0 && status != 0xF7 && status != 0xFF) {
					running = status;
					hasRunning = true;
				}

				// meta 事件：只用来确定覆盖范围的终点
				if (status == 0xFF) {
					if (pos >= track.size())
						break;
					const std::uint8_t type = track[pos];
					++pos;
					std::uint32_t l = 0;
					while (pos < track.size()) {
						const std::uint8_t b = track[pos];
						++pos;
						l = (l << 7) | (b & 0x7F);
						if ((b & 0x80) == 0)
							break;
					}
					pos += l;
					if (type == 0x2F) {                 // end_of_track
						if (absTick > raw.tickEnd)
							raw.tickEnd = absTick;
						break;
					}
					continue;
				}

				// sysex：跳过
				if (status == 0xF0 || status == 0xF7) {
					std::uint32_t l = 0;
					while (pos < track.size()) {
						const std::uint8_t b = track[pos];
						++pos;
						l = (l << 7) | (b & 0x7F);
						if ((b & 0x80) == 0)
							break;
					}
					pos += l;
					continue;
				}

				const std::uint8_t kind = status & 0xF0;
				const std::uint8_t channel = status & 0x0F;

				std::size_t dataBytes = 2;
				switch (kind) {
				case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
					dataBytes = 2; break;
				case 0xC0: case 0xD0:
					dataBytes = 1; break;
				default:
					pos = track.size(); continue;      // 认不出来 → 放弃这一轨
				}

				if (channel != kChannelIndex) {        // 只关心通道 1
					pos += dataBytes;
					continue;
				}

				// 控制变更：RPN(0,0) 的严格状态机
				if (kind == 0xB0 && pos + 1 < track.size()) {
					const int cc = track[pos];
					const int value = track[pos + 1];

					if (cc == 101) {                   // RPN MSB
						rpnMsb = value;
						rpnSelected = (rpnMsb == 0 && rpnLsb == 0);
					} else if (cc == 100) {            // RPN LSB
						rpnLsb = value;
						rpnSelected = (rpnMsb == 0 && rpnLsb == 0);
					} else if (cc == 6 && rpnSelected) {   // Data Entry MSB
						// 协议 4.6：半音数范围 1..24，超界 clamp
						int r = value;
						if (r < 1)  r = 1;
						if (r > 24) r = 24;

						RawRange e;
						e.tick = absTick;
						e.semitones = r;
						raw.ranges.push_back(e);
					}
				}

				// 弯音轮：14 位，低 7 位在前
				if (kind == 0xE0 && pos + 1 < track.size()) {
					RawBend e;
					e.tick = absTick;
					e.value = track[pos] | (track[pos + 1] << 7);
					raw.bends.push_back(e);
				}

				pos += dataBytes;
			}
		}

		// 排序 + 同一 tick 保留最后一条
		std::sort(raw.ranges.begin(), raw.ranges.end(),
			[](const RawRange& a, const RawRange& b) { return a.tick < b.tick; });
		for (std::size_t i = 0; i + 1 < raw.ranges.size();) {
			if (raw.ranges[i].tick == raw.ranges[i + 1].tick)
				raw.ranges.erase(raw.ranges.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
		std::sort(raw.bends.begin(), raw.bends.end(),
			[](const RawBend& a, const RawBend& b) { return a.tick < b.tick; });
		for (std::size_t i = 0; i + 1 < raw.bends.size();) {
			if (raw.bends[i].tick == raw.bends[i + 1].tick)
				raw.bends.erase(raw.bends.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}

		// 范围时间线补全（协议 4.6.1）：
		//   时间线为空 → 写一条 tick 0 的默认值
		//   首条事件 tick > 0 → 在 tick 0 补写默认值
		if (raw.ranges.empty()) {
			RawRange e;
			e.tick = 0;
			e.semitones = fallbackR;
			raw.ranges.push_back(e);
		} else if (raw.ranges.front().tick > 0) {
			RawRange e;
			e.tick = 0;
			e.semitones = fallbackR;
			raw.ranges.insert(raw.ranges.begin(), e);
		}

		return raw;
	}
}

namespace PITCH {

	Result PM(const std::string& path, const std::string& system)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		const int fallbackR = defaultRange(system);
		const RawData raw = scan(path, fallbackR);

		// 小节线需要拍号时间线（每段的小节长度不同）
		const TSR::Result tsr = TSR::TSR(path);

		const double ppqn = static_cast<double>(raw.ppqn);   // tick → 四分音符 的换算因子

		r.curve.channel = 1;
		// 长度参考：优先用"见过的最大 tick"，它比 end_of_track 更可靠
		const int lengthTick = (raw.maxTick > raw.tickEnd) ? raw.maxTick : raw.tickEnd;
		r.curve.quartersEnd = static_cast<double>(lengthTick) / ppqn;
		if (r.curve.quartersEnd < 1.0)
			r.curve.quartersEnd = 1.0;        // 至少一个四分音符，避免空图

		// R 时间线：tick → 四分音符
		for (const RawRange& rr : raw.ranges) {
			RangePoint p;
			p.quarters  = static_cast<double>(rr.tick) / ppqn;
			p.semitones = rr.semitones;
			r.curve.ranges.push_back(p);
		}

		// 小节线：由拍号时间线推出（每段的小节长度不同，因为分母会变）
		//   ticksPerMeasure = PPQN × 4 / 分母 × 分子
		if (tsr.ok && !tsr.timeline.empty()) {
			const double endQ = r.curve.quartersEnd;
			for (std::size_t i = 0; i < tsr.timeline.size(); ++i) {
				const TSR::Event& seg = tsr.timeline[i];
				const double segEndTick =
					(i + 1 < tsr.timeline.size())
						? static_cast<double>(tsr.timeline[i + 1].tick)
						: endQ * ppqn;

				// 一个四分音符 = PPQN 个 tick；一小节 = PPQN × 4/分母 × 分子 个 tick
				const double beatTicks  = static_cast<double>(raw.ppqn) * 4.0
				                          / static_cast<double>(1 << seg.denomPow);
				const double measureTicks = beatTicks * static_cast<double>(seg.numerator);
				if (measureTicks <= 0.0)
					continue;

				int count = 0;
				for (double t = seg.tick; t < segEndTick - 1e-9 && count < kMaxBars;
				     t += measureTicks, ++count) {
					BarLine b;
					b.quarters = (t / ppqn);
					b.measure  = seg.measure + count;
					r.curve.bars.push_back(b);
				}
			}

			// 去重（拍号变化点恰好落在小节线上时会出现重复）
			std::sort(r.curve.bars.begin(), r.curve.bars.end(),
				[](const BarLine& a, const BarLine& b) { return a.quarters < b.quarters; });
			for (std::size_t i = 0; i + 1 < r.curve.bars.size();) {
				if (r.curve.bars[i].quarters == r.curve.bars[i + 1].quarters)
					r.curve.bars.erase(r.curve.bars.begin() + static_cast<std::ptrdiff_t>(i));
				else
					++i;
			}
		}

		// ── 曲线：只在「弯音事件」处取点 ──
		// ⚠️ 硬件语义：音源只在收到弯音消息时才重新计算音高。
		//    所以新写入的 R（RPN 0）要等到下一个弯音事件才生效 ——
		//    **R 变化本身不产生音高变化**，不能为 R 变化单独取点。
		//    （真文件里 RPN 通常也在弯音之前写好，所以这条几乎不影响实际结果。）
		int curR = fallbackR;                               // 累计到当前 tick 的 R
		std::size_t ri = 0;                                 // R 时间线游标
		for (const RawBend& rb : raw.bends) {
			// 把该弯音事件之前（含同一 tick）的 R 变化都应用掉
			while (ri < raw.ranges.size() && raw.ranges[ri].tick <= rb.tick) {
				curR = raw.ranges[ri].semitones;
				++ri;
			}

			Point p;
			p.quarters = static_cast<double>(rb.tick) / ppqn;
			p.cents = CentsOf(curR, rb.value);
			r.curve.points.push_back(p);
		}

		r.ok = true;
		r.message = "Pitch：通道 " + std::to_string(r.curve.channel)
		          + "，" + std::to_string(r.curve.points.size()) + " 个弯音事件"
		          + "，默认 R = " + std::to_string(fallbackR) + " 半音"
		          + "，长度 " + std::to_string(static_cast<long long>(r.curve.quartersEnd))
		          + " 个四分音符";
		return r;
	}

	int CentsAt(const Curve& curve, double quarters)
	{
		// points 按 quarters 升序 → 找最后一个 quarters <= 查询位置的采样点
		const auto it = std::upper_bound(
			curve.points.begin(), curve.points.end(), quarters,
			[](double q, const Point& p) { return q < p.quarters; });
		if (it == curve.points.begin())
			return 0;                      // 第一个事件之前：弯音轮停在中心
		return (it - 1)->cents;
	}

}
