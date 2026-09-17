#include "Note_Binder.h"
#include <cstdlib>

namespace BIND {

	std::vector<BoundNote> NB(const std::vector<NOTE::Note>& notes,
	                          const PITCH::Curve& curve,
	                          const std::vector<LYRIC::Event>& lyrics)
	{
		std::vector<BoundNote> out;
		out.reserve(notes.size());

		for (const NOTE::Note& n : notes) {
			BoundNote b;
			b.quarters  = n.quarters;
			b.duration  = n.duration;
			b.number    = n.number;
			b.velocity  = n.velocity;
			b.baseCents = (n.number - 69) * 100;

			// ── 歌词：就近匹配 ──
			// 距离 = |音符起点 − 歌词位置|，取最小；等距时取**后面那个**。
			// 遍历按时间升序 → 用 <= 覆盖即可实现"等距绑后面"。
			// 加一个极小容差，抵消"tick 等距在四分音符域只差浮点尾数"的情况
			// （容差 1e-9 个四分音符 ≈ 0.0000005 tick，不影响任何真实判定）。
			if (!lyrics.empty()) {
				const double q = n.quarters;
				int bestIdx = -1;
				double bestDist = 0.0;
				for (std::size_t i = 0; i < lyrics.size(); ++i) {
					const double d = std::abs(lyrics[i].quarters - q);
					if (bestIdx < 0 || d <= bestDist + 1e-9) {
						bestDist = d;
						bestIdx = static_cast<int>(i);
					}
				}
				if (bestIdx >= 0)
					b.lyric = lyrics[static_cast<std::size_t>(bestIdx)].text;
			}

			// 首点：音符起点处的绝对音高
			PitchPoint head;
			head.offset = 0.0;
			head.cents  = b.baseCents + PITCH::CentsAt(curve, n.quarters);
			b.pitch.push_back(head);

			// 音符区间内的每一次弯音变化，各记一个点。
			// 弯音是采样保持的，所以"变化点"就是曲线上的采样点。
			const double endQ = n.quarters + n.duration;
			for (const PITCH::Point& p : curve.points) {
				if (p.quarters <= n.quarters)
					continue;              // 音符起点之前 / 起点的值已记在首点
				if (p.quarters >= endQ)
					break;                 // 音符结束之后
				PitchPoint pp;
				pp.offset = p.quarters - n.quarters;
				pp.cents  = b.baseCents + p.cents;
				b.pitch.push_back(pp);
			}

			out.push_back(b);
		}

		return out;
	}

}
