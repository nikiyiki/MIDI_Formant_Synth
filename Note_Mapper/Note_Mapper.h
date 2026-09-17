#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Note_Mapper — 音符映射
//  职责：把通道 1 的 note_on / note_off 映射成音符时间线，
//        并在映射过程中解决「音符重叠」（协议 6.1 重叠收缩）。
//  只看音符消息，不看弯音轮 —— 与 Pitch_Mapper 互为对偶，两者互不相识。
//
//  坐标约定（本模块自己换算完，不把 PPQN 泄漏给调用者）：
//      横轴 = 四分音符数（从曲首起）
//
//  重叠收缩（协议 6.1，逐字实现）：
//      a_end = a.start + a.dur
//      if a_end > b.start:  a.dur = max(0, b.start - 1 - a.start)
//  也就是：前一个音符的 note_off 与下一个音符的 note_on 至少隔开 1 tick。
//  ⚠️ 该规则不看音高、只看时间先后 —— 所以同时发声的和弦会被削成最后一个音
//     （协议面向单声部歌声，这是预期行为）。
// ─────────────────────────────────────────────────────────────

namespace NOTE {

	/// 一个音符。
	struct Note {
		double quarters = 0.0;   ///< 起点（四分音符）
		double duration = 0.0;   ///< 时长（四分音符）
		int    number   = 60;    ///< 音高编号（0..127）
		int    velocity = 100;   ///< 源力度（1..127）。协议 4.2：保留每个音符的源 velocity
	};

	/// 音符探测结果。
	struct Result {
		bool              ok = false;   ///< 是否成功解析
		std::vector<Note> notes;        ///< 按起点升序；已收缩、已丢弃零长度
		std::string       message;      ///< 可直接显示的一句话
	};

	/// 解析通道 1 的音符，返回处理过重叠的音符时间线。
	Result NM(const std::string& path);

}
