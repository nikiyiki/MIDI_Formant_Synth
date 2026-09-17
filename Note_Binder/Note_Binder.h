#pragma once

#include <vector>

#include "Note_Mapper/Note_Mapper.h"
#include "Pitch_Mapper/Pitch_Mapper.h"
#include "Lyric_Mapper/Lyric_Mapper.h"

// ─────────────────────────────────────────────────────────────
//  Note_Binder — 绑定：把音符与弯音合起来
//  职责：给每个音符绑上「它自己的绝对音高轨迹」。
//
//  依赖方向：
//      Note_Mapper  ─┐
//                    ├─▶ Note_Binder        （前两者互不相识）
//      Pitch_Mapper ─┘
//  "音符"和"弯音"是两种独立信息，"把它们绑起来"是第三件事 —— 所以单开一个模块，
//  这样 Note_Mapper / Pitch_Mapper 保持纯净，转换层也能直接用绑定结果，不必经过 GUI。
//
//  绝对音高 = (音高编号 − 69) × 100 + 弯音音分      零点 = A4（音高编号 69）
//  一个音符内部弯音可能变多次，所以绑的是「轨迹」而不是单个值。
// ─────────────────────────────────────────────────────────────

namespace BIND {

	/// 轨迹上的一个点。
	struct PitchPoint {
		double offset = 0.0;   ///< 相对音符起点的偏移（四分音符）
		int    cents  = 0;     ///< 该处的绝对音高（音分），0 = A4
	};

	/// 绑定后的音符：音符本身 + 它自己的绝对音高轨迹 + 绑定的歌词。
	/// （也就是"寄存器音符"：一个把音符各项属性 + 音高 + 歌词存在一起的容器。）
	struct BoundNote {
		double quarters  = 0.0;   ///< 起点（四分音符）
		double duration  = 0.0;   ///< 时长（四分音符）
		int    number    = 60;    ///< 音高编号（0..127）
		int    velocity  = 100;   ///< 源力度（1..127）
		int    baseCents = 0;     ///< (音高编号 − 69) × 100，音符本身的音分

		/// 绑定的歌词（就近匹配；空串表示没绑到）
		std::string lyric;

		/// 绝对音高轨迹，按 offset 升序；首点 offset 恒为 0。
		/// 轨迹长度为 1 表示这个音符内部弯音没变过。
		std::vector<PitchPoint> pitch;
	};

	/// 把音符、弯音曲线、歌词绑在一起。
	/// 歌词匹配：就近（距离 = |音符起点 tick − 歌词 tick|）；
	///           等距时取**后面那个**（用严格 < 更新，等距自然被后者覆盖）；
	///           不做消耗、不推进、不做音节摊平。
	std::vector<BoundNote> NB(const std::vector<NOTE::Note>& notes,
	                          const PITCH::Curve& curve,
	                          const std::vector<LYRIC::Event>& lyrics);

}
