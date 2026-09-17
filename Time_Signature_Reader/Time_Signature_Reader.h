#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Time_Signature_Reader — 拍号探测 + (M, T) 坐标换算
//  职责：读取 SMF 的拍号时间线，并把绝对 tick 换算成 (M, T) 坐标。
//  地基：小节长度由拍号决定，所以 BR / SR 想得到 (M, T) 都必须先用本模块。
//
//  换算依据（PPQN = 每四分音符 tick 数，全曲不变）：
//      ticksPerBeat    = PPQN × 4 / 分母      ← 变拍号时靠"分母 4/分母"变倍率
//      ticksPerMeasure = ticksPerBeat × 分子
//  小节从 1 开始计数（歌曲总是从第 1 小节开始）。
// ─────────────────────────────────────────────────────────────

namespace TSR {

	/// 一个拍号事件。
	struct Event {
		int tick = 0;        ///< 绝对 tick
		int numerator = 4;   ///< 分子（每小节几拍）
		int denomPow = 2;    ///< 分母的幂（2 → 4 分音符，3 → 8 分音符）
		int measure = 1;     ///< 所在小节的坐标 M（从 1 开始）
		int pos = 0;         ///< 小节内的坐标 T
	};

	/// 拍号探测结果。
	struct Result {
		bool               ok = false; ///< 是否可用（PPQN 读取成功）
		int                ppqn = 0;   ///< PPQN
		std::vector<Event> timeline;   ///< 按 tick 升序；文件无拍号时含一条兜底 4/4
		std::string        message;    ///< 可直接显示给用户的多行文本
	};

	/// 探测一个 SMF 的拍号时间线。
	Result TSR(const std::string& path);

}
