#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Expression_Mapper — 表情（CC11）映射
//  职责：把通道 1 的 CC11 消息读成"表情时间线"。
//  只看 CC11 —— 不看音符、不看弯音、不看 CC7，是纯粹的 时间 → 表情值 函数。
//
//  坐标约定（本模块自己换算完，不把 PPQN 泄漏给调用者）：
//      横轴 = 四分音符数（PPQN 只在本模块内部用一次）
//      纵轴 = CC11 值（int，0..127 的 MIDI 7 位量）
//
//  CC11 是"等比"系数：模拟响度 = velo × CC7(t)/100 × CC11(t)/100。
//  所以 CC11 = 0 会让整条曲线塌成零增益、也失去等比含义 ——
//  与 velocity = 0 一样，这类事件被识别出来后**不采纳**（整条丢掉）。
//
//  解析规则（协议 4.7）：
//      - 只读取所选输入通道的 CC11
//      - 同一 tick 多次生效：后写覆盖前写
//      - 若输入无 CC11，时间线仅含 tick 0 的默认值 127
//      - 首条事件 tick > 0 → 在 tick 0 补一条默认值 127
//      - 采样保持：没有新消息时值不变（画曲线要画阶梯，不能连直线）
// ─────────────────────────────────────────────────────────────

namespace EXP {

	/// 表情时间线上的一个采样点。
	struct Point {
		double quarters = 0.0;   ///< 从曲首起的四分音符数
		int    value    = 127;   ///< 该点起的 CC11 值（1..127）
	};

	/// 通道 1 的表情时间线。
	struct Curve {
		int    channel       = 1;     ///< MIDI 通道号（本工具只处理 1）
		double quartersBegin = 0.0;   ///< 覆盖范围起点（四分音符）
		double quartersEnd   = 0.0;   ///< 覆盖范围终点（四分音符）
		std::vector<Point> points;    ///< 采样点，按时间升序；首点恒在 tick 0
	};

	/// 探测结果。
	struct Result {
		bool        ok = false;   ///< 是否成功解析
		Curve       curve;        ///< 表情时间线
		std::string message;      ///< 可直接显示的一句话
	};

	/// 默认表情值（协议第 11 章 DEFAULT_CC11 = 127）。
	/// ⚠️ 与弯音不同：它跟 GS/GM 无关，所以本模块**不需要 system 参数**。
	const int kDefaultValue = 127;

	/// 解析通道 1 的 CC11，生成表情时间线。
	Result EM(const std::string& path);

	/// 查询某个四分音符位置上的 CC11 值（采样保持语义）。
	/// 第一个事件之前 → kDefaultValue。纯查询：无依赖、无副作用。
	int ValueAt(const Curve& curve, double quarters);

}
