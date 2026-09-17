#include "Lyric_Mapper.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile / FS::ReadPpqn
#include <fstream>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace {
	/// 歌词元事件的类型字节（协议 4.4：来源是 lyrics 元事件）
	const std::uint8_t kLyricMeta = 0x05;

	/// 最小清洗：控制字符 → 空格，去掉首尾空格。
	/// ⚠️ 只动 < 0x20 与 0x7F 的字节 —— ≥0x80 的字节（UTF-8 的一部分）原样保留，
	///    所以这个清洗不会把多字节编码打坏。
	/// 协议 4.4 那六步（LRC 时间戳 / LRC 元数据 / 残留方括号 / 行注释 / 多空格折叠）
	/// 等服务中文歌词时再补。
	void cleanLyric(std::string& s)
	{
		for (char& c : s) {
			const unsigned char u = static_cast<unsigned char>(c);
			if (u < 0x20 || u == 0x7F)
				c = ' ';
		}
		const std::size_t b = s.find_first_not_of(' ');
		if (b == std::string::npos) {
			s.clear();
			return;
		}
		const std::size_t e = s.find_last_not_of(' ');
		s = s.substr(b, e - b + 1);
	}

	/// 扫全部轨，收集歌词元事件（FF 05）。在 tick 域记录。
	void scanLyrics(const std::string& path, std::vector<LYRIC::Event>& out, int& ppqn)
	{
		std::ifstream f = FS::OpenFile(path);
		if (!f)
			return;

		ppqn = FS::ReadPpqn(f);
		if (ppqn <= 0)
			return;

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

					if (type == kLyricMeta && l > 0 && pos + l <= track.size()) {
						std::string text(reinterpret_cast<const char*>(&track[pos]), l);
						cleanLyric(text);
						if (!text.empty()) {
							LYRIC::Event e;
							e.tick = absTick;
							e.text = text;
							out.push_back(e);
						}
					}

					pos += l;
					if (type == 0x2F)                       // end_of_track
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
		}
	}
}

namespace LYRIC {

	Result LM(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		int ppqn = 480;
		scanLyrics(path, r.timeline, ppqn);

		// 按 tick 排序；同一 tick 保留最后一条（与其它时间线一致）
		std::sort(r.timeline.begin(), r.timeline.end(),
			[](const Event& a, const Event& b) { return a.tick < b.tick; });
		for (std::size_t i = 0; i + 1 < r.timeline.size();) {
			if (r.timeline[i].tick == r.timeline[i + 1].tick)
				r.timeline.erase(r.timeline.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}

		// tick → 四分音符
		const double ppqnF = static_cast<double>(ppqn);
		for (Event& e : r.timeline)
			e.quarters = static_cast<double>(e.tick) / ppqnF;

		r.ok = true;
		r.message = "Lyric：" + std::to_string(r.timeline.size()) + " 条歌词";
		if (!r.timeline.empty())
			r.message += "（首条 @ " + r.timeline.front().text + "）";
		return r;
	}

}
