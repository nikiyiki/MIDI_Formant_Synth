#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  BPM_Reader — BPM（tempo）探测
//  职责：读取 SMF 的 set_tempo 时间线，并给出每条的 (M, T) 坐标。
//  依赖：坐标换算需要拍号，故内部使用 Time_Signature_Reader。
// ─────────────────────────────────────────────────────────────

namespace BR {

	/// 一个 tempo 事件。
	struct Event {
		int    track = 0;     ///< 所在轨号（从 0 开始）
		int    tick = 0;      ///< 绝对 tick
		double bpm = 0.0;     ///< 每分钟四分音符数 = 60000000 / tempo
		int    measure = 1;   ///< 坐标 M（从 1 开始）
		int    pos = 0;       ///< 坐标 T
	};

	/// BPM 探测结果。
	struct Result {
		bool              ok = false; ///< 是否成功打开并解析
		std::vector<Event> timeline;  ///< 按 tick 升序；文件无 tempo 时含兜底 120 BPM
		std::string       message;    ///< 可直接显示给用户的多行文本
	};

	/// 探测一个 SMF 的 BPM 时间线。
	Result BR(const std::string& path);

}
