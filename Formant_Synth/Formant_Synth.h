#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Formant_Synth — 共振峰合成 → 16 轨 SMF 写出
//
//  定位（协议 1.2）：输出是**全新 SMF**，不基于输入轨编辑。
//  输入只是参数源与 meta 源。本模块**不重读**任何东西 ——
//  它只消费各 reader/mapper 已经榨好的 Result。
//
//  依赖方向（单向漏斗，谁都不认识输出）：
//      PPQN / 拍号 / BPM / System ────────────┐
//      Note / Pitch / Lyric / Expression ─────┼─▶ Formant_Synth ─▶ 字节
//      （Volume 不进输出：CC7 按协议 7.3 查表重写）
//
//  产出（协议 7.1）：Type-1，PPQN 与输入一致，17 轨
//      轨 0      Setup
//      轨 1–16   16 个通道轨 CH0–CH15
//
//  本轮 demo 边界（务必看清）：
//      ✅ 17 轨容器、PPQN、Setup 轨（GS SysEx + 拍号 + tempo）
//      ✅ 每轨 tick 0 初始化（track_name / CC0 / CC32 / PC / RPN 0=24 / CC7 / CC11）
//      ✅ 事件按 (tick, priority) 排序，priority = note_off 0 < RPN 1 < ctrl 2 < note_on 3
//      ✅ F0 通道（CH0）的音符 + 弯音事件
//      ⏳ 其余 15 个通道**结构性存在但暂无事件** —— 它们要等音素字典与
//         共振峰展开（协议 6.5）就位。所以本轮输出**只有基频那一层**，
//         听起来是单音，不是元音。
// ─────────────────────────────────────────────────────────────

namespace SYNTH {

	/// 逻辑事件（写出前的统一表示，与 SMF 字节无关）。
	struct Event {
		std::uint32_t tick = 0;    ///< 绝对 tick
		int channel = 0;           ///< MIDI 通道 0–15
		int priority = 2;          ///< 0 note_off / 1 RPN / 2 ctrl / 3 note_on
		int status = 0;            ///< 状态字节（含通道号）
		int data1 = 0;             ///< 第一数据字节
		int data2 = 0;             ///< 第二数据字节
		int len = 2;               ///< 数据字节数（1 或 2）
	};

	/// 一条轨（不含轨头）。
	struct Track {
		std::string                 name;      ///< 轨名（输出重写）
		std::vector<std::uint8_t>   sysex;     ///< 只用于 Setup 轨的 SysEx 前缀
		std::vector<Event>          events;    ///< 事件流
	};

	/// 转换结果（可直接显示的一句话）。
	struct Result {
		bool        ok = false;
		std::string message;
		int         tracks = 0;      ///< 实际写出的轨数
		int         events = 0;      ///< 事件总数
		int         notes = 0;       ///< 写出的音符数
	};

	/// 把输入 SMF 转换并写出到 outPath。
	/// 各项参数对应 OUTPUT-PARAMS.md 的决策：
	///   cc11FullScale  协议无此概念，是显示/合成模型的口径（默认 127）
	///   velocityScale  输出 velocity 全局缩放（默认 1.0）
	///   cc7Scale       输出 CC7 全局缩放（默认 1.0）
	Result Convert(const std::string& inPath,
	               const std::string& outPath,
	               int    cc11FullScale = 127,
	               double velocityScale = 1.0,
	               double cc7Scale      = 1.0);

}