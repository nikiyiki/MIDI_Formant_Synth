#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Lyric_Mapper — 歌词映射
//  职责：读出 SMF 的歌词元事件（FF 05 Lyric），清洗成可用文本。
//
//  编码：暂不处理 —— 元事件字节原样取出，先服务罗马音/ASCII 场景。
//        （最小清洗只把控制字符换成空格，不碰 ≥0x80 的字节，
//          所以将来接 UTF-8 也不会把字节打坏。）
//
//  坐标：同时给出 tick 与四分音符。
//        「就近 / 等距」的判定必须在 tick 域做（等距才判得准），
//        对外显示用四分音符。
// ─────────────────────────────────────────────────────────────

namespace LYRIC {

	/// 一条歌词。
	struct Event {
		int         tick = 0;        ///< 绝对 tick
		double      quarters = 0.0;  ///< 同一位置，换算成四分音符
		std::string text;            ///< 歌词文本（原样，先不管编码）
	};

	/// 歌词探测结果。
	struct Result {
		bool               ok = false;   ///< 是否成功解析
		std::vector<Event> timeline;     ///< 按 tick 升序
		std::string        message;      ///< 可直接显示的一句话
	};

	/// 读出一个 SMF 的歌词时间线。
	Result LM(const std::string& path);

}
