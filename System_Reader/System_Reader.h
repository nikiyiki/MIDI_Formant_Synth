#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  System_Reader — 系统复位探测
//  职责：扫描全部轨的 SysEx，找出系统复位消息，并给出每条的 (M, T) 坐标。
//  依赖：坐标换算需要拍号，故内部使用 Time_Signature_Reader。
//
//  判定规则（只看"系统复位"，通道重设之类不参与）：
//      GM  / GM2 : 完全匹配整条复位消息（7E 7F 09 01 / 7E 7F 09 03）
//      GS  / XG  : 只看头（41 10 42 12 / 43 10 4C），且头必须发生改变
//                  —— 头不改变时，没有新的系统复位
//
//  头的定义（取自 SysEx 解剖 IDH + DVH + MDH + CMH）：
//      GS 头 = 41 10 42 12        XG 头 = 43 10 4C（Yamaha 无命令 ID）
// ─────────────────────────────────────────────────────────────

namespace SR {

	/// 一条系统复位消息。
	struct Reset {
		int         track = 0;    ///< 所在轨号（从 0 开始）
		int         tick = 0;     ///< 绝对 tick
		int         measure = 1;  ///< 坐标 M（从 1 开始）
		int         pos = 0;      ///< 坐标 T
		std::string system;       ///< "GS" / "XG" / "GM" / "GM2"
		std::string label;        ///< 消息名称，如 "GS Reset"
	};

	/// 系统探测结果。
	struct Result {
		bool                ok = false; ///< 是否成功打开并解析
		std::vector<Reset>  timeline;   ///< 按出现顺序；被"头变化"过滤后的复位
		std::string         system;     ///< 最终判定："GS"/"XG"/"GM2"/"GM"/"未知"
		std::string         message;    ///< 可直接显示给用户的多行文本
	};

	/// 探测一个 SMF 的系统复位时间线并给出判定。
	Result SR(const std::string& path);

}
