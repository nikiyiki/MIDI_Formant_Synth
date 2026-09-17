#include "Time_Signature_Reader.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile：按 UTF-8 路径打开文件
#include <fstream>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace {
	// PPQN 常量：单位拍（四分音符）的 tick 数
	const int kPpqnBase = 4;

	/// 读满 14 字节的 MThd 头。成功返回 true。
	bool loadHead(const std::string& path, std::uint8_t head[14])
	{
		std::ifstream f = FS::OpenFile(path);
		if (!f)
			return false;
		f.read(reinterpret_cast<char*>(head), 14);
		return f.gcount() == 14;
	}

	/// ticksPerBeat = PPQN × 4 / 分母（分母 = 2^denomPow）。
	int ticksPerBeat(int ppqn, int denomPow)
	{
		int t = ppqn * kPpqnBase;
		if (denomPow > 0)
			t >>= denomPow;
		else if (denomPow < 0)
			t <<= (-denomPow);
		return t < 1 ? 1 : t;
	}

	/// 判断某 tick 是否正好落在"前一个拍号的小节线"上。
	bool onOldMeasureLine(int tick, int prevTick, int prevNum, int prevPow, int ppqn)
	{
		const int len = ticksPerBeat(ppqn, prevPow) * prevNum;
		return len > 0 && (tick - prevTick) % len == 0;
	}
}

namespace TSR {

	Result TSR(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		std::uint8_t head[14];
		if (!loadHead(path, head)) {
			r.message = "没有读取到拍号捏。";
			return r;
		}
		const int division =
			(static_cast<int>(head[12]) << 8) | static_cast<int>(head[13]);
		if (division & 0x8000) {
			r.message = "没有读取到拍号捏（SMPTE 计时不含 PPQN）。";
			return r;
		}
		r.ppqn = division & 0x7FFF;

		// 逐轨扫事件，收集拍号（FF 58）
		std::vector<Event> raw;
		std::ifstream f = FS::OpenFile(path);
		f.seekg(14);
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
				if (pos >= track.size())
					break;

				// 状态字节，或沿用 running status
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

				// meta 事件
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
					if (type == 0x58 && l >= 2 && pos + 2 <= track.size()) {
						Event e;
						e.tick = absTick;
						e.numerator = track[pos];
						e.denomPow = track[pos + 1];
						raw.push_back(e);
					}
					pos += l;
					if (type == 0x2F)                     // end_of_track
						break;
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

				// 通道消息：按状态高 4 位决定数据字节数
				switch (status & 0xF0) {
				case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
					pos += 2; break;
				case 0xC0: case 0xD0:
					pos += 1; break;
				default:
					pos = track.size(); break;
				}
			}
		}

		// 按 tick 排序；同一 tick 保留最后一条
		std::sort(raw.begin(), raw.end(),
			[](const Event& a, const Event& b) { return a.tick < b.tick; });
		for (std::size_t i = 0; i + 1 < raw.size();) {
			if (raw[i].tick == raw[i + 1].tick)
				raw.erase(raw.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}

		// 兜底：文件没有拍号 → 4/4 @ tick 0
		if (raw.empty()) {
			Event e;
			e.tick = 0;
			e.numerator = 4;
			e.denomPow = 2;
			raw.push_back(e);
		}

		// 计算每条拍号自身的 (M, T) 坐标
		// 规则：拍号变化在小节线起点或小节中间，都立即开始新的一小节
		//       （与 MIDI 实际行为一致；若变化点不在小节线上，说明这一小节有偏移）
		int m = 1;
		int segStart = raw[0].tick;
		int prevNum = raw[0].numerator;
		int prevPow = raw[0].denomPow;
		int prevTick = raw[0].tick;

		for (std::size_t i = 0; i < raw.size(); ++i) {
			if (i > 0) {
				const int tb = ticksPerBeat(r.ppqn, prevPow);
				const int d = raw[i].tick - prevTick;
				if (onOldMeasureLine(raw[i].tick, segStart, prevNum, prevPow, r.ppqn))
					m += d / (tb * prevNum);
				else
					m += d / (tb * prevNum) + 1;
				segStart = raw[i].tick;
			}
			raw[i].measure = m;
			raw[i].pos = 0;
			prevNum = raw[i].numerator;
			prevPow = raw[i].denomPow;
			prevTick = raw[i].tick;
		}

		r.ok = true;
		r.timeline = raw;

		r.message = "Time Signature：\n";
		for (const Event& e : r.timeline) {
			r.message += "    (" + std::to_string(e.measure) + ", "
			           + std::to_string(e.pos) + ")    "
			           + std::to_string(e.numerator) + "/"
			           + std::to_string(1 << e.denomPow) + "\n";
		}

		return r;
	}

}
