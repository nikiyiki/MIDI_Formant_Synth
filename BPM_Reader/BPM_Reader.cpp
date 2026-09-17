#include "BPM_Reader.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile：按 UTF-8 路径打开文件
#include <fstream>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

namespace {
	// 文件没有 tempo 时的兜底值（协议 10.2：补 500000 = 120 BPM @ tick 0）
	const int kDefaultTempoUs = 500000;

	/// 扫出所有 set_tempo（FF 51 03）事件的 (轨号, 绝对 tick, 微秒每四分音符)。
	void collectTempos(const std::string& path, std::vector<BR::Event>& out)
	{
		std::ifstream f = FS::OpenFile(path);
		if (!f)
			return;
		f.seekg(14);

		std::uint8_t ch[8];
		int trackIndex = 0;

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
					if (type == 0x51 && l == 3 && pos + 3 <= track.size()) {
						const int us = (static_cast<int>(track[pos]) << 16) |
						               (static_cast<int>(track[pos + 1]) << 8) |
						                static_cast<int>(track[pos + 2]);
						if (us > 0) {
							BR::Event e;
							e.track = trackIndex;
							e.tick = absTick;
							e.bpm = 60000000.0 / us;
							out.push_back(e);
						}
					}
					pos += l;
					if (type == 0x2F)
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

				// 通道消息
				switch (status & 0xF0) {
				case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
					pos += 2; break;
				case 0xC0: case 0xD0:
					pos += 1; break;
				default:
					pos = track.size(); break;
				}
			}

			++trackIndex;
		}
	}
}

namespace BR {

	Result BR(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		collectTempos(path, r.timeline);

		// 按 (tick, 轨号) 排序；同一 tick 保留最后一条
		std::sort(r.timeline.begin(), r.timeline.end(),
			[](const Event& a, const Event& b) {
				if (a.tick != b.tick) return a.tick < b.tick;
				return a.track < b.track;
			});
		for (std::size_t i = 0; i + 1 < r.timeline.size();) {
			if (r.timeline[i].tick == r.timeline[i + 1].tick)
				r.timeline.erase(r.timeline.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}

		// 兜底：文件没有 tempo → 120 BPM @ tick 0
		if (r.timeline.empty()) {
			Event e;
			e.tick = 0;
			e.bpm = 60000000.0 / kDefaultTempoUs;
			r.timeline.push_back(e);
		}

		// 用拍号时间线把 tick 换算成 (M, T)
		const TSR::Result sr = TSR::TSR(path);
		if (sr.ok && !sr.timeline.empty()) {
			for (Event& e : r.timeline) {
				// 找 e.tick 所属的拍号段：tick ≤ e.tick 的最后一条
				const TSR::Event* cur = &sr.timeline[0];
				for (const TSR::Event& s : sr.timeline) {
					if (s.tick <= e.tick)
						cur = &s;
					else
						break;
				}

				const int ticksPerBeat = (sr.ppqn * 4) >> cur->denomPow;
				const int len = ticksPerBeat * cur->numerator;
				if (len <= 0)
					continue;

				const int d = e.tick - cur->tick;      // 距该段起点的 tick 数
				// 落在小节线上时 d % len == 0，pos 自然是 0；
				// 不需要为"正好在小节线"写特判（曾经多加了一个 -1，导致小节号偏小）。
				e.measure = cur->measure + d / len;
				e.pos = d % len;
			}
		}

		r.ok = true;
		r.message = "BPM：\n";
		for (const Event& e : r.timeline) {
			std::string bpmText = std::to_string(e.bpm);
			const std::size_t dot = bpmText.find('.');
			if (dot != std::string::npos && dot + 3 < bpmText.size())
				bpmText.erase(dot + 3);                // 保留两位小数，去掉尾随 0 由下面处理
			while (bpmText.size() > 1 && bpmText.back() == '0')
				bpmText.pop_back();
			if (!bpmText.empty() && bpmText.back() == '.')
				bpmText.pop_back();

			r.message += "    (" + std::to_string(e.measure) + ", "
			           + std::to_string(e.pos) + ")    "
			           + bpmText + " BPM\n";
		}

		return r;
	}

}
