#include "System_Reader.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile：按 UTF-8 路径打开文件
#include <fstream>
#include <cstdint>
#include <vector>

namespace {
	/// SysEx 头的种类（只列举本工具关心的）。
	enum class HeaderType {
		None,        ///< 非本工具关心的头
		Roland,      ///< 41 10 42 12  → GS
		Yamaha,      ///< 43 10 4C     → XG
		UniversalGM, ///< 7E 7F 09 01  → GM
		UniversalGM2 ///< 7E 7F 09 03  → GM2
	};

	/// 判断载荷从 off 起的若干字节是否等于给定序列。
	bool bytesAt(const std::vector<std::uint8_t>& p, std::size_t off,
	             std::initializer_list<std::uint8_t> sig)
	{
		if (off + sig.size() > p.size())
			return false;
		std::size_t i = off;
		for (const std::uint8_t b : sig) {
			if (p[i] != b)
				return false;
			++i;
		}
		return true;
	}

	/// 在载荷里找一条 sysex 头的种类。
	HeaderType classify(const std::vector<std::uint8_t>& p)
	{
		for (std::size_t i = 0; i < p.size(); ++i) {
			if (bytesAt(p, i, { 0x41, 0x10, 0x42, 0x12 })) return HeaderType::Roland;
			if (bytesAt(p, i, { 0x43, 0x10, 0x4C }))       return HeaderType::Yamaha;
			if (bytesAt(p, i, { 0x7E, 0x7F, 0x09, 0x01 })) return HeaderType::UniversalGM;
			if (bytesAt(p, i, { 0x7E, 0x7F, 0x09, 0x03 })) return HeaderType::UniversalGM2;
		}
		return HeaderType::None;
	}

	/// 判断载荷是否"完全匹配"某条定长复位消息。
	/// ⚠️ 载荷里含长度字节（如 F0 05 7E 7F 09 01 F7 的 05），
	///    所以不能从第 0 字节比对，要从长度字节之后开始找签名。
	/// "完全匹配" = 签名之后**紧跟 F7**（消息到此为止，没有多余参数）。
	bool matchFull(const std::vector<std::uint8_t>& p,
	               std::initializer_list<std::uint8_t> sig)
	{
		if (p.empty() || p.back() != 0xF7)     // sysex 必须以 F7 结束
			return false;
		for (std::size_t i = 1; i + sig.size() < p.size(); ++i) {
			if (bytesAt(p, i, sig))
				return p[i + sig.size()] == 0xF7;   // 签名后必须紧跟 F7
		}
		return false;
	}
}

namespace SR {

	Result SR(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		// 拍号几何：用于把 tick 换算成 (M, T)
		const TSR::Result tsr = TSR::TSR(path);
		const int ppqn = tsr.ppqn;

		bool seenGS = false, seenXG = false, seenGM2 = false, seenGM = false;

		// 上一条"被判定为系统"的头。GS / XG 只有在它的头与这个值不同时才算新复位，
		// 所以同为 Roland 头的一串消息（通道重设等）不会重复记成 GS。
		HeaderType lastSystem = HeaderType::None;

		std::ifstream f = FS::OpenFile(path);
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

				// meta 事件：跳过
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
					if (type == 0x2F)
						break;
					continue;
				}

				// sysex：这才是本模块关心的
				if (status == 0xF0 || status == 0xF7) {
					std::vector<std::uint8_t> payload;
					std::uint32_t l = 0;
					while (pos < track.size()) {
						const std::uint8_t b = track[pos];
						++pos;
						l = (l << 7) | (b & 0x7F);
						payload.push_back(b);          // 长度字节本身也计入载荷
						if ((b & 0x80) == 0)
							break;
					}
					const std::size_t end = pos + l;
					while (pos < track.size() && pos < end) {
						payload.push_back(track[pos]);
						++pos;
					}

					if (status == 0xF0) {
						const HeaderType cur = classify(payload);

						const bool fullGM  = matchFull(payload, { 0x7E, 0x7F, 0x09, 0x01 });
						const bool fullGM2 = matchFull(payload, { 0x7E, 0x7F, 0x09, 0x03 });

						// GM / GM2：完全匹配即算，与头变化无关
						if (fullGM && !seenGM) {
							seenGM = true;
							Reset e; e.track = trackIndex; e.tick = absTick;
							e.system = "GM"; e.label = "GM System On";
							r.timeline.push_back(e);
						}
						if (fullGM2 && !seenGM2) {
							seenGM2 = true;
							Reset e; e.track = trackIndex; e.tick = absTick;
							e.system = "GM2"; e.label = "GM2 System On";
							r.timeline.push_back(e);
						}

						// GS：只看头（41 10 42 12），且该头要"发生改变"才算新复位
						if (cur == HeaderType::Roland && cur != lastSystem) {
							seenGS = true;
							Reset e; e.track = trackIndex; e.tick = absTick;
							e.system = "GS"; e.label = "GS Reset";
							r.timeline.push_back(e);
							lastSystem = cur;
						}
						// XG：只看头（43 10 4C），同样要求头发生改变
						else if (cur == HeaderType::Yamaha && cur != lastSystem) {
							seenXG = true;
							Reset e; e.track = trackIndex; e.tick = absTick;
							e.system = "XG"; e.label = "XG System On";
							r.timeline.push_back(e);
							lastSystem = cur;
						}
					}
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

		// 把 tick 换算成 (M, T)
		if (tsr.ok && !tsr.timeline.empty()) {
			for (Reset& e : r.timeline) {
				const TSR::Event* cur = &tsr.timeline[0];
				for (const TSR::Event& s : tsr.timeline) {
					if (s.tick <= e.tick)
						cur = &s;
					else
						break;
				}
				const int ticksPerBeat = (ppqn * 4) >> cur->denomPow;
				const int len = ticksPerBeat * cur->numerator;
				if (len <= 0)
					continue;
				const int d = e.tick - cur->tick;
				// 落在小节线上时 d % len == 0，pos 自然是 0；
				// 不需要为"正好在小节线"写特判（曾经多加了一个 -1，导致小节号偏小）。
				e.measure = cur->measure + d / len;
				e.pos = d % len;
			}
		}

		// 最终判定：GS > XG > GM2 > GM（GM2 与 GM 同前缀，必须先判 GM2）
		if (seenGS)       r.system = "GS";
		else if (seenXG)  r.system = "XG";
		else if (seenGM2) r.system = "GM2";
		else if (seenGM)  r.system = "GM";
		else              r.system = "未知";

		r.ok = true;
		r.message = "System：\n";
		if (r.timeline.empty()) {
			r.message += "    没有检测到系统复位捏。\n";
		} else {
			for (const Reset& e : r.timeline) {
				r.message += "    (" + std::to_string(e.measure) + ", "
				           + std::to_string(e.pos) + ")    "
				           + e.label + "\n";
			}
		}
		r.message += "    → 判定：" + r.system + "\n";

		return r;
	}

}
