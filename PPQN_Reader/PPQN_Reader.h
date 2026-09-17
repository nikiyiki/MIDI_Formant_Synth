#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────
//  PPQN_Reader — PPQN 探测
//  职责：读取 SMF 的 division 字段，取出 PPQN。
//  边界：只对 PPQN 负责；拍号 / BPM / 系统分别由 TSR / BR / SR 负责。
// ─────────────────────────────────────────────────────────────

namespace PPQN {

	/// PPQN 探测结果。
	struct Result {
		bool        ok = false;   ///< 是否读取到 PPQN
		int         ppqn = 0;     ///< PPQN 的值（1..32767），ok 为 true 时有效
		std::string message;      ///< 可直接显示给用户的一句话
	};

	/// 探测一个 SMF 文件的 PPQN。
	/// 只检查头字节数，不做严格格式检测（SMF 合法性由 SR 负责）。
	Result PR(const std::string& path);

}
