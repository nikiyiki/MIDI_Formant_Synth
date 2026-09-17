// Make_Test_Smf.cpp —— 生成「全特性」测试样本 SMF（v1.0.1demo 的验收素材）
//
// 用法：make_test_smf.exe [输出路径]        默认 tests\test_v101_demo.mid
//
// ── 为什么用 C++ 手写一个生成器，而不是拿 PowerShell 拼字节 ──────────────
//   坑 #13 / #14：手写脚本抠字节出过的错，比被验证的代码还多。
//   而且本文件**不引用 Smf_Writer**（产品代码），所以
//   「生成器写得出、产品读得懂」这件事本身就是一次独立实现的交叉验证 ——
//   要是两边都错在同一个地方，那才叫巧。
//
// ── 样本覆盖（目前实现/提及的全部输入侧特性）─────────────────────────────
//   · 格式 **1**、2 轨（Conductor + Voice）—— 验跨轨扫描
//   · PPQN **960**（故意 ≠ 输出固定的 480）—— 验 scaleTick 的 PPQN 对齐
//   · 拍号 4/4 → 3/4 → 6/8；速度 120 → 140 → 90 BPM —— 验两条 meta 时间线
//   · GS Reset + GM System On + CH10 解锁式 SysEx —— 验系统判定优先级
//     （GS > GM2 > GM）与「头不改变就不算新复位」
//   · RPN 0（弯音范围）12 → 2 → 24；12→2 之后**故意一段不发弯音** ——
//     验硬件语义「R 变化本身不产生音高变化」
//   · 弯音：长音符颤音、滑入、hammer-on 式跳进
//   · CC11 表情曲线（含一条 CC11=0，验「0 整条丢弃」）
//   · CC7 通道音量曲线（含一条 CC7=0，同上）
//   · **100 个随机音符**（固定种子 → 可复现），每个配一条罗马音歌词
//   · 歌词分三类：字典查得到的韵母 / 带声母（触发小 gate + 噪声曲线）/
//     查不到的（走中性兜底 —— 就是上一轮那个悬垂指针崩掉的那条路径）
//   · 边界：重叠音符对、零长度音符、note_on velocity=0、轨末挂音
// ────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <algorithm>

namespace {

using Bytes = std::vector<std::uint8_t>;

const int          kPpqn = 960;              // 输入 PPQN（故意不是 480）
const unsigned     kSeed = 20260917u;        // 固定种子 → 每次生成的文件完全一致
const int          kMelodyNotes = 100;       // 用户指定：100 个随机音符

// ── 事件与编码 ──────────────────────────────────────────────

struct Ev {
	std::uint32_t tick = 0;
	Bytes         bytes;
};

void PushVlq(Bytes& b, std::uint32_t v)
{
	std::uint8_t tmp[8];
	int n = 0;
	tmp[n++] = static_cast<std::uint8_t>(v & 0x7F);
	v >>= 7;
	while (v > 0) {
		tmp[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
		v >>= 7;
	}
	for (int i = n - 1; i >= 0; --i)
		b.push_back(tmp[i]);
}

void Add(std::vector<Ev>& v, std::uint32_t tick, const Bytes& b)
{
	Ev e;
	e.tick = tick;
	e.bytes = b;
	v.push_back(e);
}

Bytes Meta(std::uint8_t type, const Bytes& payload)
{
	Bytes b;
	b.push_back(0xFF);
	b.push_back(type);
	PushVlq(b, static_cast<std::uint32_t>(payload.size()));
	b.insert(b.end(), payload.begin(), payload.end());
	return b;
}

Bytes Text(const std::string& s)
{
	return Meta(0x01, Bytes(s.begin(), s.end()));
}

Bytes TrackName(const std::string& s)
{
	return Meta(0x03, Bytes(s.begin(), s.end()));
}

/// SysEx：`F0 <VLQ len> <payload>`；payload 自带结尾 F7
Bytes SysEx(const Bytes& payload)
{
	Bytes b;
	b.push_back(0xF0);
	PushVlq(b, static_cast<std::uint32_t>(payload.size()));
	b.insert(b.end(), payload.begin(), payload.end());
	return b;
}

Bytes Ch(int status, int d1, int d2)
{
	Bytes b;
	b.push_back(static_cast<std::uint8_t>(status));
	b.push_back(static_cast<std::uint8_t>(d1));
	b.push_back(static_cast<std::uint8_t>(d2));
	return b;
}

Bytes NoteOn (int ch, int note, int vel) { return Ch(0x90 | ch, note, vel); }
Bytes NoteOff(int ch, int note, int vel) { return Ch(0x80 | ch, note, vel); }
Bytes Cc     (int ch, int cc, int val)   { return Ch(0xB0 | ch, cc, val); }

/// 弯音：14 位，LSB 在前
Bytes Bend(int ch, int value)
{
	if (value < 0)     value = 0;
	if (value > 16383) value = 16383;
	Bytes b;
	b.push_back(static_cast<std::uint8_t>(0xE0 | ch));
	b.push_back(static_cast<std::uint8_t>(value & 0x7F));
	b.push_back(static_cast<std::uint8_t>((value >> 7) & 0x7F));
	return b;
}

/// 音分偏移 → 弯音值（R = 当前弯音范围，半音）
int BendFromCents(double cents, int rSemis)
{
	const double b = 8192.0 + cents * 8192.0 / (rSemis * 100.0);
	long v = std::lround(b);
	if (v < 0)     v = 0;
	if (v > 16383) v = 16383;
	return static_cast<int>(v);
}

/// 把事件按 tick 稳定排序 → 带 delta → 追加 end_of_track
Bytes Track(std::vector<Ev>& evs)
{
	std::stable_sort(evs.begin(), evs.end(),
	                 [](const Ev& a, const Ev& b) { return a.tick < b.tick; });

	Bytes body;
	std::uint32_t last = 0;
	for (const Ev& e : evs) {
		PushVlq(body, e.tick - last);
		last = e.tick;
		body.insert(body.end(), e.bytes.begin(), e.bytes.end());
	}
	PushVlq(body, 0);
	body.push_back(0xFF);
	body.push_back(0x2F);
	body.push_back(0x00);

	Bytes trk;
	const char id[4] = { 'M', 'T', 'r', 'k' };
	trk.insert(trk.end(), id, id + 4);
	const std::uint32_t len = static_cast<std::uint32_t>(body.size());
	trk.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
	trk.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
	trk.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
	trk.push_back(static_cast<std::uint8_t>(len & 0xFF));
	trk.insert(trk.end(), body.begin(), body.end());
	return trk;
}

// ── 自检：VLQ 编码（生成器自己也要被验证）────────────────────
bool SelfTest()
{
	struct { std::uint32_t v; int len; } kCases[] = {
		{ 0x00, 1 }, { 0x7F, 1 }, { 0x80, 2 }, { 0x3FFF, 2 },
		{ 0x4000, 3 }, { 0x1FFFFF, 3 }, { 0x200000, 4 }, { 0x0FFFFFFF, 4 },
	};
	for (const auto& c : kCases) {
		Bytes b;
		PushVlq(b, c.v);
		if (static_cast<int>(b.size()) != c.len) {
			std::printf("自检失败：VLQ(%u) 长度 %d，应为 %d\n",
			            c.v, static_cast<int>(b.size()), c.len);
			return false;
		}
	}
	return true;
}

// ── 歌词池 ──────────────────────────────────────────────────
//
//  分三类，按 index % 4 轮换，保证 100 个音符把三条路径都走到：
//    0 → 带声母（塞音/擦音/塞擦音 → 小 gate、噪声曲线）
//    1 → 字典**查得到**的韵母（真去查共振峰表）
//    2 → 字典**查不到**的音节（走中性兜底 —— 上一轮崩掉的那条路）
//    3 → 随便挑（混合）

const char* kWithInitial[] = {
	"ka","ki","ku","ke","ko", "sa","shi","su","se","so",
	"ta","chi","tsu","te","to", "na","ni","nu","ne","no",
	"ha","hi","fu","he","ho", "ma","mi","mu","me","mo",
	"ya","yu","yo", "ra","ri","ru","re","ro", "wa","wo",
	"pa","pi","pu","pe","po", "za","ji","zu","ze","zo",
	"ba","bi","bu","be","bo", "da","de","do",
	"ga","gi","gu","ge","go",
};
const char* kFinals[] = {
	"a","i","u","e","o","er", "ai","ei","ao","ou",
	"an","en","in","un","ang","eng","ing","ong",
	"ia","ie","ua","uo","iu","ui","iao",
};
const char* kUnknown[] = {
	"kya","nyu","ryo","fa","vi","she","tsa","qyo","fyu","wye",
};

template <std::size_t N>
const char* Pick(const char* (&pool)[N], std::mt19937& rng)
{
	std::uniform_int_distribution<std::size_t> d(0, N - 1);
	return pool[d(rng)];
}

} // namespace

int main(int argc, char** argv)
{
	const std::string outPath = (argc > 1) ? argv[1] : "tests\\test_v101_demo.mid";

	std::printf("make_test_smf —— 生成全特性测试样本\n");
	if (!SelfTest()) {
		std::printf("生成器自检未通过，终止。\n");
		return 1;
	}
	std::printf("自检：VLQ 编码通过（8 个边界值）\n");

	const double q = kPpqn;                    // 1 个四分音符 = 960 tick
	auto T = [&](double quarters) -> std::uint32_t {
		return static_cast<std::uint32_t>(quarters * q + 0.5);
	};

	std::mt19937 rng(kSeed);

	// ── 关键 tick（后面生成音符时要用，先定下来）────────────
	const std::uint32_t kTickR2      = T(10.0);    // RPN 0：12 → 2
	const std::uint32_t kTickR2Quiet = T(12.0);    // 这段之后才允许再发弯音
	const std::uint32_t kTickR24     = T(28.0);    // RPN 0：2 → 24
	const std::uint32_t kTickTs1     = T(16.0);    // 拍号 → 3/4
	const std::uint32_t kTickTs2     = T(28.0);    // 拍号 → 6/8（同时刻换 R=24）

	std::vector<Ev> cond;   // 轨 0：Conductor
	std::vector<Ev> voice;  // 轨 1：歌声（通道 1）

	// ── 轨 0 ────────────────────────────────────────────────
	Add(cond, 0, TrackName("Conductor"));
	Add(cond, 0, Text("formant_synth v1.0.1demo test smf / seed 20260917 / ppqn 960"));

	{	// GS Reset（41 10 42 12 40 00 7F 00 41 F7 —— 校验和 0x41）
		const std::uint8_t p[] = { 0x41,0x10,0x42,0x12,0x40,0x00,0x7F,0x00,0x41,0xF7 };
		Add(cond, 0, SysEx(Bytes(p, p + sizeof(p))));
	}
	{	// GM System On（7E 7F 09 01 F7）—— 与 GS 同时存在，验优先级 GS > GM
		const std::uint8_t p[] = { 0x7E,0x7F,0x09,0x01,0xF7 };
		Add(cond, 0, SysEx(Bytes(p, p + sizeof(p))));
	}
	{	// GS CH10 解锁式（41 10 42 12 40 10 15 00 1B F7）
		// 同一个 Roland 头 → 按「头不改变就不算新复位」，不该记成第二条 GS Reset
		const std::uint8_t p[] = { 0x41,0x10,0x42,0x12,0x40,0x10,0x15,0x00,0x1B,0xF7 };
		Add(cond, T(4.0), SysEx(Bytes(p, p + sizeof(p))));
	}

	// 拍号：4/4 @0（PPQN 960 → 一小节 3840），3/4 @16qn，6/8 @28qn
	{ const std::uint8_t p[] = { 4, 2, 24, 8 }; Add(cond, 0,       Meta(0x58, Bytes(p, p + 4))); }
	{ const std::uint8_t p[] = { 3, 2, 24, 8 }; Add(cond, kTickTs1, Meta(0x58, Bytes(p, p + 4))); }
	{ const std::uint8_t p[] = { 6, 3, 24, 8 }; Add(cond, kTickTs2, Meta(0x58, Bytes(p, p + 4))); }

	// 速度：120 @0，140 @16qn，90 @28qn（us = 60000000 / bpm）
	auto Tempo = [&](std::uint32_t tick, double bpm) {
		const std::uint32_t us = static_cast<std::uint32_t>(60000000.0 / bpm + 0.5);
		const std::uint8_t p[3] = {
			static_cast<std::uint8_t>((us >> 16) & 0xFF),
			static_cast<std::uint8_t>((us >> 8) & 0xFF),
			static_cast<std::uint8_t>(us & 0xFF)
		};
		Add(cond, tick, Meta(0x51, Bytes(p, p + 3)));
	};
	Tempo(0, 120.0);
	Tempo(kTickTs1, 140.0);
	Tempo(kTickTs2, 90.0);

	// ── 轨 1：初始化 ─────────────────────────────────────────
	Add(voice, 0, TrackName("Voice"));

	// RPN 0 = 12（半音）：CC101=0 → CC100=0 → CC6=12 → CC38=0
	Add(voice, 0, Cc(0, 101, 0));
	Add(voice, 0, Cc(0, 100, 0));
	Add(voice, 0, Cc(0, 6, 12));
	Add(voice, 0, Cc(0, 38, 0));

	Add(voice, 0, Cc(0, 7, 100));      // CC7：满
	Add(voice, 0, Cc(0, 11, 127));     // CC11：满

	// ── 100 个随机音符（每个配一条罗马音）────────────────────
	const int kScale[] = { 0, 2, 4, 5, 7, 9, 11 };          // C 大调
	const double kDurs[] = { 0.25, 0.375, 0.5, 0.75, 1.0 };

	std::uniform_int_distribution<int> dDeg(0, 6), dOct(0, 2), dDur(0, 4);
	std::uniform_int_distribution<int> dVel(24, 120), dRest(0, 3);

	int nNotes = 0, nLyrics = 0, nBends = 0, nCc11 = 0, nCc7 = 0;
	int cntInitial = 0, cntFinal = 0, cntUnknown = 0;
	int curR = 12;                      // 当前弯音范围（跟着 RPN 时间线走）

	std::uint32_t tick = 0;
	for (int i = 0; i < kMelodyNotes; ++i) {
		// 换 R 的 tick 到了就先写 RPN（写在音符之前，符合实际用法）
		if (tick > kTickR2 && curR == 12) {
			Add(voice, tick, Cc(0, 101, 0));
			Add(voice, tick, Cc(0, 100, 0));
			Add(voice, tick, Cc(0, 6, 2));
			Add(voice, tick, Cc(0, 38, 0));
			curR = 2;
		}
		if (tick > kTickR24 && curR == 2) {
			Add(voice, tick, Cc(0, 101, 0));
			Add(voice, tick, Cc(0, 100, 0));
			Add(voice, tick, Cc(0, 6, 24));
			Add(voice, tick, Cc(0, 38, 0));
			curR = 24;
		}

		if (dRest(rng) == 0)                        // 偶尔留个气口
			tick += T(0.125);

		const int    note = 60 + kScale[dDeg(rng)] + 12 * dOct(rng);
		const double dur  = kDurs[dDur(rng)];
		const int    vel  = dVel(rng);

		// 歌词（每个音符一条，放在音符同一个 tick → 就近匹配距离 0）
		const char* syl = nullptr;
		switch (i % 4) {
		case 0: syl = Pick(kWithInitial, rng); ++cntInitial; break;
		case 1: syl = Pick(kFinals,      rng); ++cntFinal;   break;
		case 2: syl = Pick(kUnknown,     rng); ++cntUnknown; break;
		default: {
			const int pick = static_cast<int>(rng() % 3);
			if (pick == 0)      { syl = Pick(kWithInitial, rng); ++cntInitial; }
			else if (pick == 1) { syl = Pick(kFinals,      rng); ++cntFinal;   }
			else                { syl = Pick(kUnknown,     rng); ++cntUnknown; }
			break;
		}
		}
		const std::string ly(syl);
		Add(voice, tick, Meta(0x05, Bytes(ly.begin(), ly.end())));
		++nLyrics;

		// 音符本体
		Add(voice, tick, NoteOn(0, note, vel));
		const std::uint32_t tOn  = tick;
		const std::uint32_t tOff = T(tick / q + dur);
		Add(voice, tOff, NoteOff(0, note, 64));
		++nNotes;

		// ★ R=2 生效后、kTickR2Quiet 之前：**故意一个弯音都不发**
		//   —— 验「R 变化本身不产生音高变化」
		const bool quiet = (tOn >= kTickR2 && tOn < kTickR2Quiet);

		if (!quiet) {
			// 滑入（每 10 个音符一次）：-120 音分 → 0，占音符前 1/4
			if (i % 10 == 3) {
				const double slideQ = (dur < 0.25) ? dur * 0.5 : 0.25;
				for (int k = 0; k <= 4; ++k) {
					const double frac = k / 4.0;
					Add(voice, T(tOn / q + slideQ * frac),
					    Bend(0, BendFromCents(-120.0 * (1.0 - frac), curR)));
					++nBends;
				}
			}
			// hammer-on 式跳进（每 17 个一次）：起手 -200 音分，1/32 后回中
			if (i % 17 == 5) {
				Add(voice, tOn, Bend(0, BendFromCents(-200.0, curR)));
				Add(voice, tOn + T(0.0625), Bend(0, BendFromCents(0.0, curR)));
				nBends += 2;
			}
			// 颤音：时长 ≥ 0.5 拍的音符，后半段起振，幅度渐入
			if (dur >= 0.5) {
				const double start = dur * 0.35;
				const double step  = 0.0625;                 // 十六分之一的十六分之一
				int k = 0;
				for (double t = start; t < dur; t += step, ++k) {
					const double amp = 55.0 * std::min(1.0, (t - start) / (dur * 0.4));
					const double cents = ((k % 2) ? -amp : amp);
					Add(voice, T(tOn / q + t), Bend(0, BendFromCents(cents, curR)));
					++nBends;
				}
			}
		}

		tick = tOff;
	}

	const std::uint32_t kMelodyEnd = tick;

	// ── CC11 / CC7 曲线（整曲范围）──────────────────────────
	{
		const double endQ = kMelodyEnd / q;
		for (double t = 0.0; t <= endQ; t += 2.0) {
			const int v = static_cast<int>(std::lround(
				107.0 + 20.0 * std::sin(2.0 * 3.14159265 * t / std::max(1.0, endQ))));
			Add(voice, T(t), Cc(0, 11, (v < 1) ? 1 : (v > 127 ? 127 : v)));
			++nCc11;
		}
		// 一条 CC11 = 0（设计决定：整条丢弃，不进时间线、也不画图）
		const std::uint32_t z = T(endQ * 0.5);
		Add(voice, z,     Cc(0, 11, 0));
		Add(voice, z + 1, Cc(0, 11, 110));

		Add(voice, T(endQ / 3.0), Cc(0, 7, 88));      // 通道音量：混音层
		Add(voice, T(endQ * 2.0 / 3.0), Cc(0, 7, 70));
		Add(voice, z,     Cc(0, 7, 0));               // 一条 CC7 = 0（同样整条丢弃）
		Add(voice, z + 1, Cc(0, 7, 70));
		nCc7 = 4;
	}

	// ── 边界用例（放在旋律之后，各自也配歌词）────────────────
	std::uint32_t edge = kMelodyEnd + T(1.0);

	// 1) 重叠音符对：后到的把前一个 gate 收缩（协议 6.1）
	Add(voice, edge, Meta(0x05, Bytes{'o','v','a'}));
	Add(voice, edge, NoteOn(0, 72, 100));
	Add(voice, edge + T(1.0), NoteOff(0, 72, 64));
	Add(voice, edge + T(0.5), NoteOn(0, 76, 96));           // ← 与上一个重叠
	Add(voice, edge + T(1.0), NoteOff(0, 76, 64));

	// 2) 零长度音符（note_on / note_off 同 tick → 应被丢弃）
	edge += T(2.0);
	Add(voice, edge, Meta(0x05, Bytes{'z','e','r'}));
	Add(voice, edge, NoteOn(0, 79, 100));
	Add(voice, edge, NoteOff(0, 79, 64));

	// 3) note_on velocity = 0 当作 note_off（协议 4.2）
	edge += T(1.0);
	Add(voice, edge, Meta(0x05, Bytes{'v','e','l'}));
	Add(voice, edge, NoteOn(0, 67, 100));
	Add(voice, edge + T(0.5), NoteOn(0, 67, 0));            // ← 收尾靠 vel=0

	// 4) 轨末挂音：有 on 无 off → 应在 end_of_track 处强制闭合
	edge += T(1.0);
	Add(voice, edge, Meta(0x05, Bytes{'h','a','n'}));
	Add(voice, edge, NoteOn(0, 69, 90));
	// （故意不写 note_off）
	//
	// ⚠️ 挂音后面必须**留一点事件**，否则轨末 tick 就等于它的起点，
	//    「强制闭合」会闭出一个零长度音符，再被零长度规则丢掉 ——
	//    这个用例就白造了（第一版就是这样，读取器报「丢弃 2 个零长度」才对上账）。
	//    真实文件里后面总有东西（CC、meta、至少 end_of_track 在更后面），照做。
	Add(voice, edge + T(1.0), Text("voice end"));

	// ── 组装 ────────────────────────────────────────────────
	Bytes out;
	{
		const char id[4] = { 'M', 'T', 'h', 'd' };
		out.insert(out.end(), id, id + 4);
		out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(6);
		out.push_back(0); out.push_back(1);                  // format 1
		out.push_back(0); out.push_back(2);                  // 2 轨
		out.push_back(static_cast<std::uint8_t>((kPpqn >> 8) & 0xFF));
		out.push_back(static_cast<std::uint8_t>(kPpqn & 0xFF));
	}
	const Bytes t0 = Track(cond);
	const Bytes t1 = Track(voice);
	out.insert(out.end(), t0.begin(), t0.end());
	out.insert(out.end(), t1.begin(), t1.end());

	// 用 ofstream 而不是 fopen：MSVC 会把 fopen 判为弃用（C4996），
	// 本工程的规矩是「编译无警告」。
	{
		std::ofstream f(outPath, std::ios::binary);
		if (!f) {
			std::printf("写不出文件：%s\n", outPath.c_str());
			return 1;
		}
		f.write(reinterpret_cast<const char*>(out.data()),
		        static_cast<std::streamsize>(out.size()));
		if (!f) {
			std::printf("写文件出错：%s\n", outPath.c_str());
			return 1;
		}
	}

	// ── 汇总（拿这些数字跟转换器的输出对账）──────────────────
	const double endQ = edge / q;
	std::printf("已生成：%s\n", outPath.c_str());
	std::printf("  格式 1 / 2 轨 / PPQN %d / 共 %u 字节\n",
	            kPpqn, static_cast<unsigned>(out.size()));
	std::printf("  Conductor：%u 字节（拍号 3 条、速度 3 条、SysEx 3 条）\n",
	            static_cast<unsigned>(t0.size()));
	std::printf("  Voice：%u 字节\n", static_cast<unsigned>(t1.size()));
	std::printf("  旋律音符 %d 个（每个 1 条歌词，共 %d 条）\n", nNotes, nLyrics);
	std::printf("    歌词三类：带声母 %d / 字典韵母 %d / 查不到 %d\n",
	            cntInitial, cntFinal, cntUnknown);
	std::printf("    弯音事件 %d 条，CC11 %d 条，CC7 %d 条\n", nBends, nCc11, nCc7);
	std::printf("  边界：重叠 1 对 / 零长度 1 个 / vel=0 收尾 1 个 / 轨末挂音 1 个\n");
	std::printf("  旋律结束 tick=%u（%.2f 个四分音符），文件末尾 tick=%u（%.2f）\n",
	            kMelodyEnd, kMelodyEnd / q, edge, endQ);
	std::printf("  RPN 0：12 @0 → 2 @%u → 24 @%u；R=2 生效后有一整段不发弯音\n",
	            kTickR2, kTickR24);
	std::printf("  随机种子 %u（固定 → 生成结果可复现）\n", kSeed);
	return 0;
}
