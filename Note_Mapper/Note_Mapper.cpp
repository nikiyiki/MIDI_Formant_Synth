#include "Note_Mapper.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile / FS::ReadPpqn
#include <fstream>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace {
	// 只处理 MIDI 通道 1 → 状态字节的低 4 位为 0
	const std::uint8_t kChannelIndex = 0;

	// 一个音符的最大编号数（音高 0..127）
	const int kPitchCount = 128;

	// 解析期的音符：一律用 tick 记录。
	// 重叠收缩里的"隔开 1 tick"必须在 tick 域算，最后才换算成四分音符。
	struct RawNote {
		int startTick = 0;
		int endTick   = 0;      ///< 闭合位置
		int number    = 60;
		int velocity  = 100;    ///< 来自起始那个 note_on
	};

	/// 一个"挂着"的 note_on。
	struct Pending {
		int startTick = 0;
		int velocity  = 100;
	};

	/// 扫描全部轨，收集通道 1 的 note_on / note_off。
	/// note_on velocity = 0 视为 note_off（协议 4.2），不产生音符。
	void scanNotes(const std::string& path, std::vector<RawNote>& out,
	               int& ppqn, int& hangingCount)
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

			// 每个音高上"挂着"的 note_on。同一个音高再次 note_on 时排到后面，
			// 配对的 note_off 先闭合最早那个（FIFO）。
			std::vector<Pending> pending[kPitchCount];

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

				const std::uint8_t kind = status & 0xF0;
				const std::uint8_t channel = status & 0x0F;

				std::size_t dataBytes = 2;
				switch (kind) {
				case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
					dataBytes = 2; break;
				case 0xC0: case 0xD0:
					dataBytes = 1; break;
				default:
					pos = track.size(); continue;
				}

				if (channel == kChannelIndex && pos + 1 < track.size()) {
					// 闭合某个音高上最早挂着的那个 note_on（FIFO）
					auto closeNote = [&](int number) {
						if (number < 0 || number >= kPitchCount || pending[number].empty())
							return;
						RawNote rn;
						rn.startTick = pending[number].front().startTick;
						rn.velocity  = pending[number].front().velocity;
						rn.endTick   = absTick;
						rn.number    = number;
						pending[number].erase(pending[number].begin());
						out.push_back(rn);
					};

					if (kind == 0x90) {                     // note_on
						const int number = track[pos];
						const int velocity = track[pos + 1];
						if (velocity == 0) {
							// velocity = 0 视为 note_off（协议 4.2）
							closeNote(number);
						} else if (number < kPitchCount) {
							Pending p;
							p.startTick = absTick;
							p.velocity  = velocity;
							pending[number].push_back(p);
						}
					} else if (kind == 0x80) {              // note_off
						closeNote(track[pos]);
					}
				}

				pos += dataBytes;
			}

			// 轨末挂音：强制闭合到本轨末（协议 4.2）
			for (int n = 0; n < kPitchCount; ++n) {
				for (const Pending& p : pending[n]) {
					RawNote rn;
					rn.startTick = p.startTick;
					rn.velocity  = p.velocity;
					rn.endTick   = absTick;
					rn.number    = n;
					out.push_back(rn);
					++hangingCount;
				}
				pending[n].clear();
			}
		}
	}
}

namespace NOTE {

	Result NM(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		int ppqn = 480;
		int hanging = 0;
		std::vector<RawNote> raw;
		scanNotes(path, raw, ppqn, hanging);

		// 按起点排序；同一 tick 的按音高编号排（保证结果稳定）
		std::sort(raw.begin(), raw.end(), [](const RawNote& a, const RawNote& b) {
			if (a.startTick != b.startTick)
				return a.startTick < b.startTick;
			return a.number < b.number;
		});

		// ── 协议 6.1 重叠收缩 ──
		// 前一个音符的 note_off 若落在后一个音符的 note_on 之后，
		// 就把前一个的 gate 缩到「后一个起点 - 1」，两音之间至少隔 1 tick。
		int shrunk = 0;
		int dropped = 0;
		for (std::size_t i = 0; i + 1 < raw.size(); ++i) {
			RawNote& a = raw[i];
			const RawNote& b = raw[i + 1];
			if (a.endTick > b.startTick) {
				int end = b.startTick - 1;
				if (end < a.startTick)
					end = a.startTick;          // max(0, ...)：不允许负时长
				a.endTick = end;
				++shrunk;
			}
		}

		// 换算成四分音符；零长度音符直接丢弃
		const double ppqnF = static_cast<double>(ppqn);
		for (const RawNote& rn : raw) {
			if (rn.endTick <= rn.startTick) {
				++dropped;
				continue;
			}
			Note n;
			n.quarters = static_cast<double>(rn.startTick) / ppqnF;
			n.duration = static_cast<double>(rn.endTick - rn.startTick) / ppqnF;
			n.number = rn.number;
			n.velocity = rn.velocity;
			r.notes.push_back(n);
		}

		r.ok = true;
		r.message = "Note：通道 1，" + std::to_string(r.notes.size()) + " 个音符";
		if (shrunk > 0)
			r.message += "（收缩 " + std::to_string(shrunk) + " 处重叠）";
		if (dropped > 0)
			r.message += "（丢弃 " + std::to_string(dropped) + " 个零长度）";
		if (hanging > 0)
			r.message += "（闭合 " + std::to_string(hanging) + " 个挂音）";
		return r;
	}

}
