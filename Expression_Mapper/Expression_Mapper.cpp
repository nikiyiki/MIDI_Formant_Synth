#include "Expression_Mapper.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFile / FS::ReadPpqn
#include <fstream>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace {
	// 只处理 MIDI 通道 1 → 状态字节的低 4 位为 0
	const std::uint8_t kChannelIndex = 0;

	// CC 号
	const int kCcExpression = 11;      // CC11 表情

	// MThd 不可用时给一个保守的 PPQN，只影响内部 tick → 四分音符的除法
	const int kFallbackPpqn = 480;

	/// 解析期的原始数据：用 tick 记录，最后统一换算成四分音符。
	struct RawEvent {
		int tick  = 0;
		int value = 127;
	};

	struct RawData {
		int ppqn = kFallbackPpqn;
		int tickEnd = 0;              ///< 见到的 end_of_track 里的最大 tick
		int maxTick = 0;              ///< 见到过的最大 tick（比 tickEnd 更可靠的长度参考）
		bool hasRealEvent = false;    ///< 文件里是否真有 CC11（补出来的默认值不算）
		int  realCount = 0;           ///< 真正读到的 CC11 条数（补全之前统计）
		std::vector<RawEvent> events; ///< 已按 tick 升序、已去重
	};

	/// 读一个 MIDI 可变长量（delta time / meta 长度）。
	std::uint32_t readVlq(const std::vector<std::uint8_t>& track, std::size_t& pos)
	{
		std::uint32_t v = 0;
		while (pos < track.size()) {
			const std::uint8_t b = track[pos];
			++pos;
			v = (v << 7) | (b & 0x7F);
			if ((b & 0x80) == 0)
				break;
		}
		return v;
	}

	/// 扫描全部轨，只收集通道 1 的 CC11。
	RawData scan(const std::string& path)
	{
		RawData raw;

		std::ifstream f = FS::OpenFile(path);
		if (!f)
			return raw;

		raw.ppqn = FS::ReadPpqn(f, kFallbackPpqn);
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

			while (pos < track.size()) {
				absTick += static_cast<int>(readVlq(track, pos));
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
					pos += readVlq(track, pos);
					if (type == 0x2F) {                 // end_of_track
						if (absTick > raw.tickEnd)
							raw.tickEnd = absTick;
						break;
					}
					continue;
				}

				// sysex：跳过
				if (status == 0xF0 || status == 0xF7) {
					pos += readVlq(track, pos);
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

				// 控制变更：只取 CC11
				if (kind == 0xB0 && pos + 1 < track.size()) {
					const int cc = track[pos];
					const int value = track[pos + 1];

					// ⚠️ CC11 = 0 不采纳：它会毁掉等比系数（zero gain），
					//    与 Note_Mapper 里 velocity = 0 的处理同款 ——
					//    事件被识别出来，然后整条丢掉（不进时间线、也不画进图）。
					if (cc == kCcExpression && value > 0) {
						RawEvent e;
						e.tick  = absTick;
						e.value = value;
						raw.events.push_back(e);
						raw.hasRealEvent = true;
					}
				}

				pos += dataBytes;
			}
		}

		// 排序 + 同一 tick 保留最后一条（后写覆盖前写，协议 4.7）
		std::sort(raw.events.begin(), raw.events.end(),
			[](const RawEvent& a, const RawEvent& b) { return a.tick < b.tick; });
		for (std::size_t i = 0; i + 1 < raw.events.size();) {
			if (raw.events[i].tick == raw.events[i + 1].tick)
				raw.events.erase(raw.events.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}

		// 补全之前先记下"真正读到几条"，供消息里报数用
		raw.realCount = static_cast<int>(raw.events.size());

		// 补全（协议 4.7）：
		//   时间线为空 → 写一条 tick 0 的默认值
		//   首条事件 tick > 0 → 在 tick 0 补写默认值
		if (raw.events.empty()) {
			RawEvent e;
			e.tick  = 0;
			e.value = EXP::kDefaultValue;
			raw.events.push_back(e);
		} else if (raw.events.front().tick > 0) {
			RawEvent e;
			e.tick  = 0;
			e.value = EXP::kDefaultValue;
			raw.events.insert(raw.events.begin(), e);
		}

		return raw;
	}
}

namespace EXP {

	Result EM(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		RawData raw = scan(path);

		const double ppqn = static_cast<double>(raw.ppqn);   // tick → 四分音符 的换算因子

		r.curve.channel = 1;
		// 长度参考：优先用"见过的最大 tick"，它比 end_of_track 更可靠
		const int lengthTick = (raw.maxTick > raw.tickEnd) ? raw.maxTick : raw.tickEnd;
		r.curve.quartersEnd = static_cast<double>(lengthTick) / ppqn;
		if (r.curve.quartersEnd < 1.0)
			r.curve.quartersEnd = 1.0;        // 至少一个四分音符，避免空图

		// 时间线：tick → 四分音符（原样保留每一条，含是否有 CC11 事件的信息）
		for (const RawEvent& re : raw.events) {
			Point p;
			p.quarters = static_cast<double>(re.tick) / ppqn;
			p.value    = re.value;
			r.curve.points.push_back(p);
		}

		// 消息里报"文件里真正有几条"：补出来的默认值不算事件
		const int realEvents = raw.hasRealEvent ? raw.realCount : 0;

		r.ok = true;
		r.message = "Expression：通道 " + std::to_string(r.curve.channel)
		          + "，" + std::to_string(realEvents) + " 个 CC11 事件"
		          + "，默认 " + std::to_string(kDefaultValue)
		          + "，长度 " + std::to_string(static_cast<long long>(r.curve.quartersEnd))
		          + " 个四分音符";
		return r;
	}

	int ValueAt(const Curve& curve, double quarters)
	{
		// points 按 quarters 升序 → 找最后一个 quarters <= 查询位置的采样点
		const auto it = std::upper_bound(
			curve.points.begin(), curve.points.end(), quarters,
			[](double q, const Point& p) { return q < p.quarters; });
		if (it == curve.points.begin())
			return kDefaultValue;          // 第一个事件之前：默认表情
		return (it - 1)->value;
	}

}
