#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Pitch_Mapper — 音高（弯音轮）映射
//  职责：把通道 1 的弯音轮消息换算成"绝对变化"（音分）。
//  只看弯音轮消息，不看音符消息 —— 本模块是纯粹的 时间 → 音分 函数。
//
//  坐标约定（本模块自己换算完，不把 PPQN 泄漏给调用者）：
//      横轴 = 四分音符数（避开 tick / PPQN；PPQN 只在本模块内部用一次）
//      纵轴 = 音分（int）
//
//  音高的决定因素（各管一摊）：
//      MIDI 系统   → 决定文件无 RPN 时的「默认 R」（GS/XG=12，GM/GM2=2）
//      RPN (0,0)   → 决定「弯音轮范围 R」的时间线（有则覆盖默认值）
//      弯音轮值    → 决定「弯多少」
//
//  换算：
//      cents(t) = round( R(t) × (bend(t) − 8192) / 8192 × 100 )
//      中心 8192 → 0 音分；R = 12 时满量程 ±1200 音分（±1 个八度）
// ─────────────────────────────────────────────────────────────

namespace PITCH {

	/// 弯音轮范围时间线上的一个点。
	struct RangePoint {
		double quarters  = 0.0;   ///< 从曲首起的四分音符数
		int    semitones = 12;    ///< 该点起的弯音轮范围（整数半音，1..24）
	};

	/// 曲线上的一个采样点。
	struct Point {
		double quarters = 0.0;    ///< 从曲首起的四分音符数
		int    cents    = 0;      ///< 弯音造成的绝对变化（音分，int）
	};

	/// 一条小节线（横轴用四分音符表示）。
	struct BarLine {
		double quarters = 0.0;   ///< 从曲首起的四分音符数
		int    measure  = 1;     ///< 小节号（从 1 开始）
	};

	/// 通道 1 的音高变化曲线。
	struct Curve {
		int    channel       = 1;     ///< MIDI 通道号（本工具只处理 1）
		double quartersBegin = 0.0;   ///< 覆盖范围起点（四分音符）
		double quartersEnd   = 0.0;   ///< 覆盖范围终点（四分音符）
		std::vector<BarLine>    bars;     ///< 小节线（由拍号时间线推出）
		std::vector<RangePoint> ranges;   ///< R 时间线（调试/显示用）
		std::vector<Point>      points;   ///< 曲线数据，按时间升序
	};

	/// 探测结果。
	struct Result {
		bool        ok = false;   ///< 是否成功解析
		Curve       curve;        ///< 音高曲线
		std::string message;      ///< 可直接显示的一句话
	};

	/// 解析通道 1 的弯音轮，生成音高变化曲线。
	/// system 由 System_Reader 提供（"GS"/"XG"/"GM"/"GM2"/其他），用于取默认 R。
	Result PM(const std::string& path, const std::string& system);

	/// 查询某个四分音符位置上的弯音音分（采样保持语义）。
	/// 第一个事件之前 → 0（弯音轮停在中心）。
	/// 这是个纯查询：无依赖、无副作用，供上层把音符和弯音相加成绝对音高。
	int CentsAt(const Curve& curve, double quarters);

}
