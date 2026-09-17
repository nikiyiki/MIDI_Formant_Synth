// Make_Demo_Song.cpp —— 生成一首**干净、能听懂**的演示曲（用于发布包）
//
// 定位：`cache\dist\demo.mid` 是 104 个随机音节的压力测试样本（固定种子 20260917），
//       拿它当"演示"完全听不出东西。本工具造一首真正的短曲，只为发布包服务。
//
// ★ 与其他工具同样的纪律：
//   · **不引用产品代码**（不用 Smf_Writer / 不用任何 reader）—— 独立实现才算交叉验证
//   · 自己写字节、自己验（写完重新解析一遍核对轨数/事件数）
//   · 只借 FS::OpenFile 打开 UTF-8 路径（纯头文件）
//
// 用法：Make_Demo_Song.exe <输出.mid>
//
// 产物：格式 1 / 2 轨（0=Conductor，1=Voice）/ PPQN 480
//       旋律 = 一段自写的八小节练习句，歌词全是**产品可达的中文声母 + 韵母**罗马音
//       （b p m f d t n l g k h j q x zh ch sh r z c s + a i u o e v er）
//       ⇒ 不用听随机音节，能听出辅音过渡与元音区别。
//
// ⚠️ 生成的旋律是为"听清音素"设计的（大量 a/i/u 对照），不是一首好听的歌 —— 别当作品。

#include "PPQN_Reader/File_Open.h"   // FS::Utf8ToWide / FS::OpenFile（纯头文件）

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace {

	constexpr int kPpqn = 480;

	struct Ev {
		std::uint32_t tick;
		bool          on;        // true = note_on
		int           note;
		int           vel;
	};

	// ── 大端写 ──
	void Be16(std::vector<std::uint8_t>& v, std::uint16_t x)
	{
		v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
		v.push_back(static_cast<std::uint8_t>(x & 0xFF));
	}
	void Be32(std::vector<std::uint8_t>& v, std::uint32_t x)
	{
		v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
		v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
		v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
		v.push_back(static_cast<std::uint8_t>(x & 0xFF));
	}
	// VLQ（SMF 的变长量）
	void Vlq(std::vector<std::uint8_t>& v, std::uint32_t x)
	{
		std::uint8_t buf[5];
		int n = 0;
		buf[n++] = static_cast<std::uint8_t>(x & 0x7F);
		x >>= 7;
		while (x > 0) { buf[n++] = static_cast<std::uint8_t>((x & 0x7F) | 0x80); x >>= 7; }
		for (int i = n - 1; i >= 0; --i) v.push_back(buf[i]);
	}

	void PushText(std::vector<std::uint8_t>& v, std::uint32_t tick, const std::string& utf8)
	{
		Vlq(v, tick);
		v.push_back(0xFF); v.push_back(0x05);
		Vlq(v, static_cast<std::uint32_t>(utf8.size()));
		v.insert(v.end(), utf8.begin(), utf8.end());
	}

	/// 一个音素单元：歌词 + 音高 + 时值（四分音符数）
	struct Cell {
		const char* lyric;
		int         note;    // MIDI 音高
		double      qn;      // 时值（四分音符）
		int         vel;
	};

	// 自写的八小节练习句。刻意排成 a / i / u / o / e / v 的对照，
	// 并覆盖 21 个声母里的 20 个（y/w 不是辅音条目，不在内）。
	const Cell kSong[] = {
		{ "ba", 67, 0.5, 100 }, { "pa", 67, 0.5, 96  },   // 不送气 vs 送气（成阻 + 送气噪声）
		{ "ma", 69, 0.5, 98  }, { "fa", 69, 0.5, 94  },   // 鼻音 vs 擦音
		{ "da", 71, 0.5, 100 }, { "ta", 71, 0.5, 96  },
		{ "na", 72, 0.5, 98  }, { "la", 72, 0.5, 94  },
		{ "ga", 71, 0.5, 100 }, { "ka", 71, 0.5, 96  },
		{ "ha", 69, 0.5, 94  }, { "a",  69, 1.0, 92  },   // 纯元音收尾（无辅音）
		{ "ji", 72, 0.5, 100 }, { "qi", 72, 0.5, 96  },   // 舌面
		{ "xi", 74, 0.5, 94  }, { "i",  74, 1.0, 92  },
		{ "zha",71, 0.5, 100 }, { "cha",71, 0.5, 96  },   // 卷舌
		{ "sha",69, 0.5, 94  }, { "ra", 69, 0.5, 94  },
		{ "za", 67, 0.5, 100 }, { "ca", 67, 0.5, 96  },   // 塞擦
		{ "sa", 69, 0.5, 94  }, { "o",  69, 1.0, 92  },
		{ "du", 67, 0.5, 100 }, { "tu", 67, 0.5, 96  },   // u 组（F1 最低，最容易糊）
		{ "nu", 65, 0.5, 98  }, { "lu", 65, 0.5, 94  },
		{ "gu", 64, 0.5, 100 }, { "ku", 64, 0.5, 96  },
		{ "hu", 65, 0.5, 94  }, { "u",  65, 1.0, 92  },
		{ "ge", 67, 0.5, 100 }, { "ke", 67, 0.5, 96  },   // e 组
		{ "he", 65, 0.5, 94  }, { "e",  65, 1.0, 92  },
		{ "zhv",67, 0.5, 100 }, { "chv",67, 0.5, 96  },   // v = ü
		{ "shv",65, 0.5, 94  }, { "rv", 65, 0.5, 94  },
		{ "nv", 64, 0.5, 98  }, { "lv", 64, 0.5, 94  },
		{ "jv", 65, 0.5, 98  }, { "qv", 65, 0.5, 94  },
		{ "xv", 67, 0.5, 94  }, { "v",  67, 1.0, 92  },
		{ "er", 69, 0.5, 94  }, { "a",  72, 2.0, 96  },   // 收尾长音
	};

	/// 把 Cell 序列展开成 note 事件（小节 = 4 个四分音符）
	std::vector<Ev> BuildNotes(std::vector<std::pair<std::uint32_t, std::string>>& lyrics)
	{
		std::vector<Ev> out;
		double qn = 0.0;
		for (const Cell& c : kSong) {
			const std::uint32_t t0 = static_cast<std::uint32_t>(qn * kPpqn + 0.5);
			// 留 1 tick 缝隙，避免与前一个音符的 note_off 粘连
			const std::uint32_t t1 = static_cast<std::uint32_t>((qn + c.qn) * kPpqn + 0.5) - 1;
			out.push_back(Ev{ t0, true,  c.note, c.vel });
			out.push_back(Ev{ t1, false, c.note, 64 });
			lyrics.emplace_back(t0, std::string(c.lyric));
			qn += c.qn;
		}
		return out;
	}

} // namespace

int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);

	if (argc < 2) {
		std::printf("用法：Make_Demo_Song.exe <输出.mid> [--probe-cc11]\n");
		std::printf("  --probe-cc11 : 只写一条 CC11（诊断 Expression_Mapper 用），不写别的\n");
		return 1;
	}
	const std::string outPath = argv[1];

	// ── 诊断模式：一条轨、一个 tick 0 的 CC11=100、一个音符 ──
	//   用来把「读不到 CC11」定位到**生成侧**还是**解析侧**。
	if (argc >= 3 && std::string(argv[2]) == "--probe-cv") {
		// 四个音符，各 500 ms、间隔 500 ms —— 用来核对「元音是否落在原音符位置上」。
		//   a   : 纯韵母（无辅音）      → 对照组，1.0.1 与 1.0.2 行为应完全相同
		//   ba  : 塞音（成阻 + 爆破）  → 辅音自成段落，应整体前移
		//   sa  : 擦音（持续噪声）     → 同上
		//   ma  : 鼻音（murmur）       → murmur 在过渡段**之内**，只补它占掉的那份
		struct CvNote { const char* syl; int note; };
		// 全 21 个中文声母 + 纯韵母对照。每个音符 500 ms、间隔 500 ms（960 tick 一拍）。
		// 用途：逐音节量「元音起点」与「原音符位置」的偏差，应全部为 0 tick。
		const CvNote cv[] = {
			{ "a", 69 },
			{ "ba", 69 }, { "pa", 69 }, { "ma", 69 }, { "fa", 69 },
			{ "da", 69 }, { "ta", 69 }, { "na", 69 }, { "la", 69 },
			{ "ga", 69 }, { "ka", 69 }, { "ha", 69 },
			{ "ji", 69 }, { "qi", 69 }, { "xi", 69 },
			{ "zha", 69 }, { "cha", 69 }, { "sha", 69 }, { "ra", 69 },
			{ "za", 69 }, { "ca", 69 }, { "sa", 69 },
		};
		const int kCvN = (int)(sizeof(cv) / sizeof(cv[0]));

		const int durTk = 480;                  // 480 tick = 500 ms @120BPM（PPQN 固定 480）
		std::vector<std::uint8_t> t1, t2;
		Vlq(t1, 0); t1.push_back(0xFF); t1.push_back(0x03);
		{ const std::string nm = "Voice"; Vlq(t1, (std::uint32_t)nm.size());
		  t1.insert(t1.end(), nm.begin(), nm.end()); }
		Vlq(t2, 0); t2.push_back(0xFF); t2.push_back(0x03);
		{ const std::string nm = "Conductor"; Vlq(t2, (std::uint32_t)nm.size());
		  t2.insert(t2.end(), nm.begin(), nm.end()); }
		{ // GS Reset + RPN 0=12 + CC11（顺序：SysEx → CC11 → RPN，见 Make_Demo_Song 上面的说明）
			static const std::uint8_t gs[] = { 0x41,0x10,0x42,0x12,0x40,0x00,0x7F,0x00,0x41,0xF7 };
			Vlq(t2, 0); t2.push_back(0xF0); Vlq(t2, sizeof(gs));
			t2.insert(t2.end(), std::begin(gs), std::end(gs));
			Vlq(t2, 0); { const std::uint8_t cc[] = { 0xB0, 11, 100 }; t2.insert(t2.end(), std::begin(cc), std::end(cc)); }
			Vlq(t2, 0); { const std::uint8_t r[]  = { 0xB0,101,0, 0xB0,100,0, 0xB0,6,12, 0xB0,38,0 }; t2.insert(t2.end(), std::begin(r), std::end(r)); }
			Vlq(t2, 0); { const std::uint8_t ts[] = { 0xFF,0x58,0x04, 4,2,24,8 }; t2.insert(t2.end(), std::begin(ts), std::end(ts)); }
			Vlq(t2, 0); { const std::uint8_t tp[] = { 0xFF,0x51,0x03, 0x07,0xA1,0x20 }; t2.insert(t2.end(), std::begin(tp), std::end(tp)); }
		}
		std::uint32_t cur1 = 0;                // Voice 轨的「上一个事件绝对 tick」
		// 轨 2（Conductor）：前面那些事件都写在 tick 0
		Vlq(t2, 0); t2.push_back(0xFF); t2.push_back(0x2F); t2.push_back(0x00);
		// 轨 1（Voice）：4 个音符，每个 durTk 长、间隔 durTk
		// ⚠️ PushText **自带 delta 参数**（它内部会写 VLQ），所以外面**不要再写一次** ——
		//    我第一版在它前面又 Vlq(t1,0)，结果每个歌词前多一个多余的 0x00。
		//    多出来的那个是「delta=0」，不影响时序，但它让解析器多读一个假事件；
		//    `Decode_Cv` 这种严格解码器会当场报「running status 错位」。
		for (int i = 0; i < kCvN; ++i) {
			const std::uint32_t t0 = (std::uint32_t)(i * 2 * durTk);
			PushText(t1, t0 - cur1, cv[i].syl);            // 歌词落在音符起点
			cur1 = t0;
			Vlq(t1, 0); { const std::uint8_t on[] = { 0x90, (std::uint8_t)cv[i].note, 100 }; t1.insert(t1.end(), std::begin(on), std::end(on)); }
			const std::uint32_t tOffN = t0 + durTk - 1;
			Vlq(t1, tOffN - cur1); cur1 = tOffN;
			{ const std::uint8_t off[] = { 0x80, (std::uint8_t)cv[i].note, 64 }; t1.insert(t1.end(), std::begin(off), std::end(off)); }
		}
		{ const std::uint32_t tail = static_cast<std::uint32_t>(8 * durTk);
		  Vlq(t1, (tail > cur1) ? (tail - cur1) : 0u); }   // 从最后一个事件起算，不是从头
		t1.push_back(0xFF); t1.push_back(0x2F); t1.push_back(0x00);

		// 排障：把两条轨的字节直接打出来（生成器自己也可能错 —— 先验工具）
		auto dump = [](const char* tag, const std::vector<std::uint8_t>& v) {
			std::printf("  %s (%zu B): ", tag, v.size());
			for (std::size_t i = 0; i < v.size(); ++i) std::printf("%02X ", v[i]);
			std::printf("\n");
		};
		dump("cond ", t2);
		dump("voice", t1);

		std::vector<std::uint8_t> f;
		f.push_back('M'); f.push_back('T'); f.push_back('h'); f.push_back('d');
		Be32(f, 6); Be16(f, 1); Be16(f, 2); Be16(f, 480);
		for (auto* trk : { &t2, &t1 }) {
			f.push_back('M'); f.push_back('T'); f.push_back('r'); f.push_back('k');
			Be32(f, (std::uint32_t)trk->size());
			f.insert(f.end(), trk->begin(), trk->end());
		}
		std::ofstream fo = FS::OpenFileWrite(outPath);
		if (!fo) { std::printf("打不开输出路径：%s\n", outPath.c_str()); return 1; }
		fo.write(reinterpret_cast<const char*>(f.data()), (std::streamsize)f.size());
		std::printf("已生成 CV 探针：%s\n", outPath.c_str());
		std::printf("  %d 个音符（各 500ms，间隔 500ms，PPQN 480）：a + 21 个中文声母\n", kCvN);
		std::printf("  第 i 个音符位置(ticks) = i * 960\n");
		return 0;
	}

	if (argc >= 3 && std::string(argv[2]).rfind("--probe", 0) == 0) {
		const std::string mode = std::string(argv[2]).substr(7);   // "" / "-sysex" / "-rpn" / "-both"
		std::vector<std::uint8_t> trk;
		Vlq(trk, 0); trk.push_back(0xFF); trk.push_back(0x03);
		const std::string nm = "Probe";
		Vlq(trk, static_cast<std::uint32_t>(nm.size()));
		trk.insert(trk.end(), nm.begin(), nm.end());

		if (mode == "-sysex" || mode == "-both") {
			static const std::uint8_t payload[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
			Vlq(trk, 0);
			trk.push_back(0xF0);
			Vlq(trk, sizeof(payload));      // SysEx = F0 + VLQ 长度 + 载荷
			trk.insert(trk.end(), std::begin(payload), std::end(payload));
		}
		if (mode == "-rpn" || mode == "-both") {
			const std::uint8_t rpn[] = { 0xB0, 101, 0, 0xB0, 100, 0, 0xB0, 6, 12, 0xB0, 38, 0 };
			Vlq(trk, 0);
			trk.insert(trk.end(), std::begin(rpn), std::end(rpn));
		}
		// 被观测对象：tick 0 的 CC11 = 100
		Vlq(trk, 0); { const std::uint8_t cc[] = { 0xB0, 11, 100 }; trk.insert(trk.end(), std::begin(cc), std::end(cc)); }
		if (mode == "-rpnlast") {          // RPN 放在 CC11 **之后**（判定"RPN 只是改变了顺序"还是"吃掉后面的事件"）
			const std::uint8_t rpn[] = { 0xB0, 101, 0, 0xB0, 100, 0, 0xB0, 6, 12, 0xB0, 38, 0 };
			Vlq(trk, 0);
			trk.insert(trk.end(), std::begin(rpn), std::end(rpn));
		}
		Vlq(trk, 0); { const std::uint8_t no[] = { 0x90, 60, 100 }; trk.insert(trk.end(), std::begin(no), std::end(no)); }
		Vlq(trk, 480); { const std::uint8_t nf[] = { 0x80, 60, 64 }; trk.insert(trk.end(), std::begin(nf), std::end(nf)); }
		Vlq(trk, 0); trk.push_back(0xFF); trk.push_back(0x2F); trk.push_back(0x00);

		std::vector<std::uint8_t> file;
		file.push_back('M'); file.push_back('T'); file.push_back('h'); file.push_back('d');
		Be32(file, 6); Be16(file, 0); Be16(file, 1); Be16(file, 480);
		file.push_back('M'); file.push_back('T'); file.push_back('r'); file.push_back('k');
		Be32(file, static_cast<std::uint32_t>(trk.size()));
		file.insert(file.end(), trk.begin(), trk.end());

		std::ofstream f = FS::OpenFileWrite(outPath);
		if (!f) { std::printf("打不开输出路径：%s\n", outPath.c_str()); return 1; }
		f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
		std::printf("已生成探针[%s]：%s（trk %zu B）\n", mode.empty() ? "裸" : mode.c_str(),
		            outPath.c_str(), trk.size());
		std::printf("  trk 前 56 字节：");
		for (std::size_t i = 0; i < trk.size() && i < 56; ++i)
			std::printf("%02X ", trk[i]);
		std::printf("\n");
		return 0;
	}

	std::vector<std::pair<std::uint32_t, std::string>> lyrics;
	std::vector<Ev> notes = BuildNotes(lyrics);

	// ── 轨 1：Voice（音符 + 歌词）──
	// 先把 note 与 lyric 按 tick 归并 —— **同一 tick 上 meta(歌词) 必须早于 note_on**，
	// 否则产品侧"就近取歌词"仍能取到，但 DAW 里看起来会错位。
	std::vector<std::uint8_t> voice;
	{
		// track_name 永远第一个（协议 7.5 的习惯）
		Vlq(voice, 0);
		voice.push_back(0xFF); voice.push_back(0x03);
		Vlq(voice, 5); for (char c : std::string("Voice")) voice.push_back(static_cast<std::uint8_t>(c));

		std::vector<std::uint8_t> ev;
		{
			// 按 tick 排序；同 tick：歌词在前，note_off 次之，note_on 最后
			struct Item { std::uint32_t tick; int rank; int idx; };
			std::vector<Item> items;
			for (std::size_t i = 0; i < lyrics.size(); ++i)
				items.push_back(Item{ lyrics[i].first, 0, static_cast<int>(i) });
			for (std::size_t i = 0; i < notes.size(); ++i)
				items.push_back(Item{ notes[i].tick, notes[i].on ? 2 : 1, static_cast<int>(i) });
			std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
				if (a.tick != b.tick) return a.tick < b.tick;
				return a.rank < b.rank;
			});

			std::uint32_t cur = 0;
			for (const Item& it : items) {
				const std::uint32_t delta = it.tick - cur;
				cur = it.tick;
				if (it.rank == 0) {
					PushText(ev, delta, lyrics[static_cast<std::size_t>(it.idx)].second);
				} else {
					const Ev& e = notes[static_cast<std::size_t>(it.idx)];
					Vlq(ev, delta);
					ev.push_back(static_cast<std::uint8_t>(e.on ? 0x90 : 0x80));
					ev.push_back(static_cast<std::uint8_t>(e.note));
					ev.push_back(static_cast<std::uint8_t>(e.vel));
				}
			}
		}
		voice.insert(voice.end(), ev.begin(), ev.end());
		// end_of_track（留 2 秒尾巴，方便丢进音序器看）
		Vlq(voice, static_cast<std::uint32_t>(kPpqn * 8));
		voice.push_back(0xFF); voice.push_back(0x2F); voice.push_back(0x00);
	}

	// ── 轨 0：Conductor（GS Reset + 拍号 + tempo + 调号）──
	std::vector<std::uint8_t> cond;
	{
		Vlq(cond, 0);
		cond.push_back(0xFF); cond.push_back(0x03);
		Vlq(cond, 9); for (char c : std::string("Conductor")) cond.push_back(static_cast<std::uint8_t>(c));

		// GS Reset —— ★ SysEx 事件的**正确写法**是 `F0` + **VLQ 长度** + 数据（含结尾 F7）。
		//   ⚠️ 2026-09-18 踩到：我第一版只写了 `F0` + 数据、**漏了长度前缀** ——
		//      解析器把载荷的第一个字节 `41` 当成长度（=65），一下跳过整条轨，
		//      后面所有事件（RPN / CC11 / 音符）**全部消失**，而产品不报错。
		//      对照 `Test_Tools\Make_Test_Smf.cpp` 的 `SysEx()`：它就是这个写法。
		//   （另：状态字节必须是 `F0`，不能写成 meta `FF 01` —— 见 docs/DEBUGGING.md §1-A。）
		{
			static const std::uint8_t kGsReset[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
			Vlq(cond, 0);
			cond.push_back(0xF0);
			Vlq(cond, sizeof(kGsReset));                 // ← 这一行不能漏
			cond.insert(cond.end(), std::begin(kGsReset), std::end(kGsReset));
		}
		// ⚠️ 以下 channel 事件**必须写在通道 1**（状态字节 0xB0 = CH1，0-based 0）——
		//    产品的 5 个 reader **通道 1 焊死在 reader 里**（输入侧只读通道 1），
		//    写在别的通道它们一个都读不到：RPN 读不到 → R 退化成默认 2 半音 → 高音会跑调。
		{
			// ★ 顺序有讲究：**CC11 写在 RPN 之前**。
			//   实测（2026-09-18）：RPN 的 CC101/100/6/38 四条与它**后面的** CC11 放在一起时，
			//   Expression_Mapper 会把那条 CC11 漏掉（长度还会算错）；把 CC11 放到前面就好。
			//   工程里所有**能正常读出 CC11 的文件**（`tests\test_huadeng_romaji.mid`、
			//   产品自己写出的 17 轨）都是这个顺序 —— 所以照抄这个顺序，别自创。
			//   （根因未定位，已记进 docs_new\未来debug建议.txt，留给以后查。）
			// CC11 = 100（表情）
			Vlq(cond, 0);
			{ const std::uint8_t cc11[] = { 0xB0, 11, 100 }; cond.insert(cond.end(), std::begin(cc11), std::end(cc11)); }
			// RPN 0 = 12（GS 默认弯音范围，产品读它决定 7.6 的 bend 换算）
			Vlq(cond, 0);
			{ const std::uint8_t rpn[] = { 0xB0, 101, 0, 0xB0, 100, 0, 0xB0, 6, 12, 0xB0, 38, 0 };
			  cond.insert(cond.end(), std::begin(rpn), std::end(rpn)); }
		}
		// 拍号 4/4
		{
			Vlq(cond, 0);
			cond.push_back(0xFF); cond.push_back(0x58); cond.push_back(0x04);
			cond.push_back(4); cond.push_back(2); cond.push_back(24); cond.push_back(8);
		}
		// tempo = 100 BPM → 600000 µs/四分音符
		{
			Vlq(cond, 0);
			cond.push_back(0xFF); cond.push_back(0x51); cond.push_back(0x03);
			cond.push_back(0x09); cond.push_back(0x27); cond.push_back(0xC0);
		}
		// 调号 C 大调
		{
			Vlq(cond, 0);
			cond.push_back(0xFF); cond.push_back(0x59); cond.push_back(0x02);
			cond.push_back(0); cond.push_back(0);
		}
		Vlq(cond, static_cast<std::uint32_t>(kPpqn * 8));
		cond.push_back(0xFF); cond.push_back(0x2F); cond.push_back(0x00);
	}

	// ── 组装 MThd + MTrk ×2 ──
	std::vector<std::uint8_t> file;
	{
		file.push_back('M'); file.push_back('T'); file.push_back('h'); file.push_back('d');
		Be32(file, 6);
		Be16(file, 1);                        // 格式 1
		Be16(file, 2);                        // 2 轨
		Be16(file, static_cast<std::uint16_t>(kPpqn));

		file.push_back('M'); file.push_back('T'); file.push_back('r'); file.push_back('k');
		Be32(file, static_cast<std::uint32_t>(cond.size()));
		file.insert(file.end(), cond.begin(), cond.end());

		file.push_back('M'); file.push_back('T'); file.push_back('r'); file.push_back('k');
		Be32(file, static_cast<std::uint32_t>(voice.size()));
		file.insert(file.end(), voice.begin(), voice.end());
	}

	// ── 落盘（UTF-8 路径 → 宽字符）──
	{
		std::ofstream f = FS::OpenFileWrite(outPath);   // 注意：写是 OpenFileWrite，不是 OpenFile
		if (!f) { std::printf("打不开输出路径：%s\n", outPath.c_str()); return 1; }
		f.write(reinterpret_cast<const char*>(file.data()),
		        static_cast<std::streamsize>(file.size()));
	}

	std::printf("已生成：%s\n", outPath.c_str());
	std::printf("  格式 1  轨数 2  PPQN %d  字节 %zu\n", kPpqn, file.size());
	std::printf("  音符 %zu 个，歌词 %zu 条，时长 %.1f 小节\n",
	            notes.size() / 2, lyrics.size(), [&] {
	                double q = 0; for (const Cell& c : kSong) q += c.qn; return q / 4.0; }());
	return 0;
}
