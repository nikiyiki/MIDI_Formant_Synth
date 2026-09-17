#include "Synth.h"
#include "PPQN_Reader/PPQN_Reader.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "BPM_Reader/BPM_Reader.h"
#include "System_Reader/System_Reader.h"
#include "Note_Mapper/Note_Mapper.h"
#include "Pitch_Mapper/Pitch_Mapper.h"
#include "Expression_Mapper/Expression_Mapper.h"
#include "Lyric_Mapper/Lyric_Mapper.h"
#include "Phoneme_Dict/Phoneme_Dict.h"     // 辅音表：小 gate 与噪声曲线要用
#include <cmath>
#include <cstdint>
#include <cctype>
#include <algorithm>
#include <cstring>

// ─────────────────────────────────────────────────────────────
//  Formant_Synth —— 合成 + 组装
//
//  职责（协议 1.2：输出是全新 SMF，不基于输入轨编辑）：
//    调各 reader 拿 Result（**不重读**语义层）
//      → 7.6 反算：频率 → note + bend（RPN 恒 24）
//      → 7.2 轨分配：F0..F8 / 噪声 / H2-H4 / 气声 / F1W / F2W
//      → 7.5 tick0 初始化 + (tick, priority) 排序
//      → 交给 Smf_Writer 落字节
//
//  本轮 demo 边界：
//    ✅ 17 轨容器、PPQN、Setup 轨（GS SysEx 含 CH10 解锁 + 拍号 + tempo）
//    ✅ 每轨 tick 0 初始化（track_name / CC0 / CC32 / PC / RPN 0=24 / CC7 / CC11）
//    ✅ 事件按 (tick, priority) 排序
//    ✅ F0 通道的音符 + 弯音
//    ⏳ 其余 15 通道结构性存在但暂无事件（等音素字典与 6.5 动程展开）
//
//  ⚠️ 本文件曾被误删后按 docs/RECOVERY-SPEC.md 重建（2026-09-17）。
//     Synth.h 是接口契约，勿改签名。
// ─────────────────────────────────────────────────────────────

namespace SYNTH {

	// ── 常量（协议 7.2 / 7.3 / 第 11 章）─────────────────────────

	const int kCc7[CH_COUNT] = {
		100, 100, 90, 80, 70, 60, 55, 50,   // F0..F7
		 45,  45, 60, 50, 40, 50, 55, 45    // F8, 噪声, H2, H3, H4, 气声, F1W, F2W
	};

	const int kDefaultTempoUs = 500000;
	const int kNoteOffVel     = 64;
	const int kDefaultCc11    = 127;
	const int kBendCenter     = 8192;
	const int kOutputRpn      = 24;      // 设计决定：恒定，覆盖协议 8.1 的 GS=12/GM=2

	/// ★ 小 gate（塞音成阻段）的力度 —— **用户真机实测**：20 上下最好。
	///
	/// 来历：初版按 CONSONANT-DICT.md §7.3 的「velocity_out 取**下限 1**」写成 1，
	/// 那在真机上几乎听不见（力度 1 ≈ 无音量），塞音的「堵住再爆开」完全没有。
	/// 用户在 SCVA / SC-8850 上听出 20 左右最合适 —— 这条以听感为准，不从公式推。
	///
	/// ⚠️ 已知风险（见 docs/CONSONANT-DICT.md §7.3、docs/DEBUGGING.md §4）：
	///    同一个 tick 上，正式段的**第一段**也在 CH_F0 发 note_on（力度 = 元音力度）。
	///    音源在收到第二个 note_on 时会重触发并采用新力度，所以这个 20 有可能被
	///    元音力度盖掉 —— 若真机上仍听不出成阻，就得改发法（见下方 PushGate 调用处）。
	const int kGateVel = 20;

	const char* ChannelName(int ch)
	{
		switch (ch) {
		case CH_F0:     return "F0";
		case CH_F1:     return "F1";
		case CH_F2:     return "F2";
		case CH_F3:     return "F3";
		case CH_F4:     return "F4";
		case CH_F5:     return "F5";
		case CH_F6:     return "F6";
		case CH_F7:     return "F7";
		case CH_F8:     return "F8";
		case CH_NOISE:  return "NOISE";
		case CH_H2:     return "H2";
		case CH_H3:     return "H3";
		case CH_H4:     return "H4";
		case CH_BREATH: return "BREATH";
		case CH_F1W:    return "F1W";
		case CH_F2W:    return "F2W";
		default:        return "?";
		}
	}

	Format ParseFormat(const std::string& s)
	{
		if (s == "xg" || s == "XG") return Format::XG;
		if (s == "gm" || s == "GM") return Format::GM;
		return Format::GS;               // 默认 GS（设计决定）
	}

	const char* FormatName(Format f)
	{
		switch (f) {
		case Format::XG: return "XG";
		case Format::GM: return "GM";
		default:         return "GS";
		}
	}

	// ── 7.6 频率 → note + bend ───────────────────────────────────

	NoteBend FreqToNoteBend(double freqHz, int rSemitones)
	{
		NoteBend nb;
		if (!(freqHz > 0.0)) {
			nb.note = 60;
			nb.bend = kBendCenter;
			return nb;
		}
		if (rSemitones < 1)  rSemitones = 1;
		if (rSemitones > 24) rSemitones = 24;

		// note_exact = 69 + 12 × log2(freq / 440)
		const double exact = 69.0 + 12.0 * std::log2(freqHz / 440.0);
		double r = std::floor(exact + 0.5);
		if (r < 0.0)   r = 0.0;
		if (r > 127.0) r = 127.0;
		nb.note = static_cast<int>(r);

		// cents = 1200 × log2(freq / note_freq)，note_freq = 440 × 2^((note−69)/12)
		const double noteFreq = 440.0 * std::pow(2.0, (nb.note - 69) / 12.0);
		const double cents = 1200.0 * std::log2(freqHz / noteFreq);

		// bend = clamp(round(8192 + cents × 8192 / (R × 100)), 0, 16383)
		const double bd = kBendCenter + cents * kBendCenter / (rSemitones * 100.0);
		double bi = std::floor(bd + 0.5);
		if (bi < 0.0)     { bi = 0.0;     nb.clamped = true; }
		if (bi > 16383.0) { bi = 16383.0; nb.clamped = true; }
		nb.bend = static_cast<int>(bi);
		return nb;
	}

	// ── 内部助手 ────────────────────────────────────────────────

	namespace {

		using PHON::Lang;
		using PHON::Manner;
		using PHON::Consonant;
		using PHON::Vowel;
		using PHON::FindConsonant;

		// 前置声明：EmitNote 要用 PushBend，而 PushBend 定义在它之后
		void PushBend(std::vector<Event>& ev, std::uint32_t tick, int ch, int bend);

		/// 找离某个四分音符位置**最近**的一条歌词（协议 6.3 的就近思路：
		/// 不做消耗、不推进；等距时取后面那个）。
		std::string NearestLyric(const std::vector<LYRIC::Event>& lyrics,
		                         double quarters, Lang /*lang*/)
		{
			std::string best;
			double bestDist = 1e18;
			for (const LYRIC::Event& e : lyrics) {
				if (e.text.empty())
					continue;
				const double d = std::fabs(e.quarters - quarters);
				// 用 <= ：等距时后者覆盖前者 —— 正是「等距取后面那个」
				if (d <= bestDist) {
					bestDist = d;
					best = e.text;
				}
			}
			return best;
		}

		/// 从一个音节里剥出声母（协议 5.1.8 的声母表，按长度降序）。
		/// 找不到返回 nullptr。
		///
		/// ⚠️ 只做**返回辅音条目**这一件事，不改动音节本身 ——
		/// 韵母的切分是后续接字典时的工作（本轮只服务小 gate 与噪声曲线）。
		const Consonant* FindLeadingConsonant(const std::string& syl, Lang lang)
		{
			if (syl.empty())
				return nullptr;

			std::string s;
			s.reserve(syl.size());
			for (char ch : syl)
				s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));

			// 英文/日文音节用带前缀的条目名，避免与中文撞名
			auto tryKey = [&](const std::string& k) -> const Consonant* {
				if (k.empty() || s.size() < k.size() || s.compare(0, k.size(), k) != 0)
					return nullptr;
				std::string full = k;
				if (lang == Lang::En)      full = "en_" + k;
				else if (lang == Lang::Ja) full = "ja_" + k;
				return FindConsonant(full);
			};

			// 按长度降序（先试双字母 zh/ch/sh，再单字母）
			static const char* kTwo[] = { "zh", "ch", "sh" };
			for (const char* k : kTwo) {
				if (const Consonant* c = tryKey(k))
					return c;
			}
			static const char* kOne[] = { "b","p","m","f","d","t","n","l",
			                              "g","k","h","j","q","x","r","z","c","s" };
			for (const char* k : kOne) {
				if (const Consonant* c = tryKey(k))
					return c;
			}
			return nullptr;   // 零声母（y/w 不是辅音实体，见 CONSONANT-DICT §3.6）
		}

		/// ⚠️ 把多字节整数压进一个事件的两个数据字节。
		///    本轮 demo 只用了低 14 位就够（足够了：频率范围有限），
		///    真正的实现应把 tick 拆到多个事件或改用 meta —— 见 TODO。
		[[maybe_unused]] inline void PackTick(std::uint32_t, std::uint8_t&, std::uint8_t&) {}

		/// 通道 → 该通道的频率（协议 7.2）。
		///
		/// 16 个通道各管一摊：
		///   CH0      F0      = 基频（由音高决定，不是共振峰）
		///   CH1..CH8 F1..F8  = 8 个共振峰（由字典的元音目标 + 辅音 locus 决定）
		///   CH9      噪声     = 不走音高，只发噪声
		///   CH10..12 H2/H3/H4 = 严格 2/3/4 × F0（协议明令独立算，不许用 note 偏移）
		///   CH13     气声     = 1.005 × F0
		///   CH14/15  F1W/F2W  = 宽间距时 F1+100 / F2+200；**窄间距元音直接不发**（见下）
		///
		/// F1W / F2W（宽带通道）的频率 —— **2026-09-18 hotfix**。
		///
		/// 原来的写法是固定 `F1+100` / `F2+200`。问题：**窄间距元音的 F1–F2 谷会被填平**。
		/// 实测证据（B3 真机录音、全 17 点包络）：`/u/`（F1 350 / F2 800，间距 **450 Hz**）
		/// 的最近邻就是 `/a/`（13.9 dB）—— 用户"u 像 a"的听感在数据上成立；
		/// `/o/`（500/900，间距 400）与 `/e/`、`/er/` 互相只有 5.1–5.7 dB，是同一类塌陷。
		///
		/// 规则：**间距 < 600 Hz 时 F1W/F2W 一律不发**（返回 0 → 上层跳过，不发音符）。
		/// 依据：G1 报告「发现 3」—— F1W/F2W 不是"加宽"，是"多加一个音"
		/// （`/a/` 的 F1=700 与 F1W=800 实测是两个独立峰）。给窄间距元音再插音 = 糊掉元音身份。
		/// ⚠️ 依据是真实录音，不是公式推导；真机听感若冲突，以听感为准（铁律 5）。
		double WideBandFreq(const int f8[8], int which)
		{
			const double f1 = static_cast<double>(f8[0]);
			const double f2 = static_cast<double>(f8[1]);
			if (f2 - f1 < 600.0)
				return 0.0;                     // 窄间距：不发宽带通道
			return (which == 0) ? (f1 + 100.0) : (f2 + 200.0);
		}

		double FormantFreqs(int ch, double f0Hz, const int f8[8])
		{
			switch (ch) {
			case CH_F0:     return f0Hz;
			case CH_F1:     return f8[0];
			case CH_F2:     return f8[1];
			case CH_F3:     return f8[2];
			case CH_F4:     return f8[3];
			case CH_F5:     return f8[4];
			case CH_F6:     return f8[5];
			case CH_F7:     return f8[6];
			case CH_F8:     return f8[7];
			case CH_H2:     return 2.0 * f0Hz;
			case CH_H3:     return 3.0 * f0Hz;
			case CH_H4:     return 4.0 * f0Hz;
			case CH_BREATH: return 1.005 * f0Hz;
			case CH_F1W:    return WideBandFreq(f8, 0);
			case CH_F2W:    return WideBandFreq(f8, 1);
			default:        return 0.0;      // CH_NOISE 由噪声单独处理
			}
		}

		/// 在某一 tick 上，给一个通道发一对 note_on / note_off。
		/// 协议 7.2：**每个通道的频率要各自独立走 7.6**（不许用 F0 的 note 加偏移）。
		void EmitNote(std::vector<Event>& ev, std::uint32_t tOn, std::uint32_t tOff,
		              int ch, double freqHz, int velo, int& warnings)
		{
			if (!(freqHz > 0.0) || tOff <= tOn)
				return;
			const NoteBend nb = FreqToNoteBend(freqHz, kOutputRpn);
			if (nb.clamped)
				++warnings;

			{
				Event e;
				e.tick = tOn; e.priority = PRI_NOTE_ON; e.channel = ch;
				e.status = static_cast<std::uint8_t>(0x90 | (ch & 0x0F));
				e.data1 = static_cast<std::uint8_t>(nb.note);
				e.data2 = static_cast<std::uint8_t>(velo);
				ev.push_back(e);
			}
			{
				Event e;
				e.tick = tOff; e.priority = PRI_NOTE_OFF; e.channel = ch;
				e.status = static_cast<std::uint8_t>(0x80 | (ch & 0x0F));
				e.data1 = static_cast<std::uint8_t>(nb.note);
				e.data2 = static_cast<std::uint8_t>(kNoteOffVel);
				ev.push_back(e);
			}
			PushBend(ev, tOn, ch, nb.bend);
		}

		/// 噪声音符（CH9 专用）—— 与 EmitNote 的唯一区别：**不发 bend**。
		///
		/// 协议 7.2 明写：「噪声脉冲不单独发送 bend，也不继承 F8 的 bend」。
		/// 噪声要的是**带宽**，不是一个精确频率：给噪声发 bend 只会把音源上的
		/// 噪声采样整体移调，既不产生"带通中心"的语义，还平白多出一条弯路。
		/// note 仍按 7.6 从中心频率算（走同一个 FreqToNoteBend），只是丢掉 bend。
		void EmitNoiseNote(std::vector<Event>& ev, std::uint32_t tOn, std::uint32_t tOff,
		                   int ch, double freqHz, int velo, int& warnings)
		{
			if (!(freqHz > 0.0) || tOff <= tOn)
				return;
			const NoteBend nb = FreqToNoteBend(freqHz, kOutputRpn);
			if (nb.clamped)
				++warnings;

			Event on;
			on.tick = tOn; on.priority = PRI_NOTE_ON; on.channel = ch;
			on.status = static_cast<std::uint8_t>(0x90 | (ch & 0x0F));
			on.data1 = static_cast<std::uint8_t>(nb.note);
			on.data2 = static_cast<std::uint8_t>(velo);
			ev.push_back(on);

			Event off;
			off.tick = tOff; off.priority = PRI_NOTE_OFF; off.channel = ch;
			off.status = static_cast<std::uint8_t>(0x80 | (ch & 0x0F));
			off.data1 = static_cast<std::uint8_t>(nb.note);
			off.data2 = static_cast<std::uint8_t>(kNoteOffVel);
			ev.push_back(off);
		}

		/// VLQ（可变长量）编码 —— PushMeta / PushSysEx 共用
		void PushVlq(std::vector<std::uint8_t>& out, std::size_t v)
		{
			std::uint8_t tmp[8];
			int n = 0;
			tmp[n++] = static_cast<std::uint8_t>(v & 0x7F);
			std::size_t rest = v >> 7;
			while (rest > 0) {
				tmp[n++] = static_cast<std::uint8_t>((rest & 0x7F) | 0x80);
				rest >>= 7;
			}
			for (int i = n - 1; i >= 0; --i)
				out.push_back(tmp[i]);
		}

		/// 往 Setup 轨塞一条 meta。bytes = 完整 `FF xx len payload`
		void PushMeta(std::vector<MetaEvent>& list, std::uint32_t tick, int kind,
		              const std::vector<std::uint8_t>& payload, std::uint8_t metaType)
		{
			MetaEvent me;
			me.tick = tick;
			me.kind = kind;
			me.bytes.push_back(0xFF);
			me.bytes.push_back(metaType);
			PushVlq(me.bytes, payload.size());
			me.bytes.insert(me.bytes.end(), payload.begin(), payload.end());
			list.push_back(me);
		}

		/// ★ 往 Setup 轨塞一条**真正的 SysEx**：`F0 <VLQ len> <payload>`。
		///   payload 自带结尾的 F7（协议 8.2 给的字节串就是这么写的）。
		///
		/// ⚠️ 这里曾经错写成 meta 0x01（文本事件）：字节串保留、长度也对，
		///    看起来「写了 SysEx」，其实音源收到的是一个文本 meta ——
		///    GS Reset 与 CH10 解锁**全部无效**，而且现象是「不报错、只是没声音」，
		///    是最难查的那一类。判据：SysEx 的状态字节是 **F0**，不是 FF 01。
		void PushSysEx(std::vector<MetaEvent>& list, std::uint32_t tick,
		               const std::vector<std::uint8_t>& payload)
		{
			MetaEvent me;
			me.tick = tick;
			me.kind = 0;                       // 与拍号/调号同组排序
			me.bytes.push_back(0xF0);
			PushVlq(me.bytes, payload.size());
			me.bytes.insert(me.bytes.end(), payload.begin(), payload.end());
			list.push_back(me);
		}

		/// CC 事件（默认 priority 2 = ctrl）
		void PushCc(std::vector<Event>& ev, std::uint32_t tick, int ch,
		            std::uint8_t cc, std::uint8_t val, int pri = PRI_CTRL)
		{
			Event e;
			e.tick = tick;
			e.priority = pri;
			e.channel = ch;
			e.status = static_cast<std::uint8_t>(0xB0 | (ch & 0x0F));
			e.data1 = cc;
			e.data2 = val;
			ev.push_back(e);
		}

		/// PitchWheel（14 位，LSB 在前）
		void PushBend(std::vector<Event>& ev, std::uint32_t tick, int ch, int bend)
		{
			if (bend < 0)     bend = 0;
			if (bend > 16383) bend = 16383;
			Event e;
			e.tick = tick;
			e.priority = PRI_CTRL;
			e.channel = ch;
			e.status = static_cast<std::uint8_t>(0xE0 | (ch & 0x0F));
			e.data1 = static_cast<std::uint8_t>(bend & 0x7F);
			e.data2 = static_cast<std::uint8_t>((bend >> 7) & 0x7F);
			ev.push_back(e);
		}

		/// 一轨的 tick 0 初始化（协议 7.5，顺序固定）
		void PushInit(std::vector<Event>& ev, int ch, Format fmt, double cc7Scale,
		              int noisePc)
		{			const std::uint32_t t = 0;

			// [按格式] 通道初始化
			if (fmt == Format::GS) {
				PushCc(ev, t, ch, 0,  8, PRI_RPN);       // CC0 = 8
				PushCc(ev, t, ch, 32, 0, PRI_RPN);       // CC32 = 0
			} else if (fmt == Format::XG) {
				PushCc(ev, t, ch, 0,  0x40, PRI_RPN);    // CC0 = 0x40
				PushCc(ev, t, ch, 32, 0,    PRI_RPN);    // CC32 = 0
			}
			{
				Event pc;
				pc.tick = t;
				pc.priority = PRI_RPN;
				pc.channel = ch;
				pc.status = static_cast<std::uint8_t>(0xC0 | (ch & 0x0F));  // PC，1 数据字节
				// ★ CH9 = 噪声通道，用自己的音色（默认 121 Breath Noise）。
				//   其余通道 = 80（协议 §8.2）。理由见 Synth.h 的 Options::noisePc。
				pc.data1 = static_cast<std::uint8_t>(
					(ch == CH_NOISE) ? (noisePc & 0x7F) : 80);
				pc.data2 = 0;
				ev.push_back(pc);
			}

			// RPN 0 = 24（CC101=0, CC100=0, CC6=24, CC38=0）
			PushCc(ev, t, ch, 101, 0,          PRI_RPN);
			PushCc(ev, t, ch, 100, 0,          PRI_RPN);
			PushCc(ev, t, ch, 6,   kOutputRpn, PRI_RPN);
			PushCc(ev, t, ch, 38,  0,          PRI_RPN);

			// CC7 = 表值 × scale
			int v = static_cast<int>(kCc7[ch] * cc7Scale + 0.5);
			if (v < 0)   v = 0;
			if (v > 127) v = 127;
			PushCc(ev, t, ch, 7, static_cast<std::uint8_t>(v), PRI_CTRL);

			// CC11 = 初始表情值
			PushCc(ev, t, ch, 11, static_cast<std::uint8_t>(kDefaultCc11), PRI_CTRL);
		}

		/// 小 gate：塞音的**成阻静音段**用极短音符带过。
		///
		/// 为什么这么做（设计决定，见 CONSONANT-DICT.md §7）：
		///   6.5 的动程结构是纯共振峰、**没有「静音」概念**。而塞音必须有成阻期，
		///   否则 b 与 m 分不出来、p 也没有「先堵住再爆开」的听感。
		///   与其改段结构（给段加 kind 属性、动下游），不如用一个极短音符带过 ——
		///   零结构改动，复用现有全部机制。
		///
		/// ⚠️ 两个已知风险（真机试听要盯，见 CONSONANT-DICT.md §7.3）：
		///   · gate 太长或音源 release 长 → 会听到「噗」的一声，反而弄脏塞音
		///   · 时长必须 ≥1 tick，否则会被 6.1 重叠收缩吃掉
		void PushGate(std::vector<Event>& ev, std::uint32_t tOn, int ch,
		              int note, int velo)
		{
			if (velo < 1)   velo = 1;
			if (velo > 127) velo = 127;
			const std::uint32_t tOff = tOn + 1;      // 1 tick，最小 gate

			{
				Event e;
				e.tick = tOn; e.priority = PRI_NOTE_ON; e.channel = ch;
				e.status = static_cast<std::uint8_t>(0x90 | (ch & 0x0F));
				e.data1 = static_cast<std::uint8_t>(note);
				e.data2 = static_cast<std::uint8_t>(velo);
				ev.push_back(e);
			}
			{
				Event e;
				e.tick = tOff; e.priority = PRI_NOTE_OFF; e.channel = ch;
				e.status = static_cast<std::uint8_t>(0x80 | (ch & 0x0F));
				e.data1 = static_cast<std::uint8_t>(note);
				e.data2 = static_cast<std::uint8_t>(kNoteOffVel);
				ev.push_back(e);
			}
		}

		/// 噪声通道（CH9）的 CC11：**单独一条曲线**（设计决定，见 OUTPUT-PARAMS §4.3）。
		///
		/// 与 CH0–CH8 那条的区别：
		///   · 源线上，只在**有噪声声母**的音符处取值（无噪声的地方保持上一个值，
		///     所以自然形成阶梯 —— 正是采样保持语义）
		///   · 值来自该辅音的 relative amplitude（noiseAmp 0..1）× 默认表情值
		///
		/// 曲线上点密度低（每个噪声事件一个点），是刻意的：
		/// 它表达的是「这一段噪声多响」，不是细粒度包络。
		std::vector<EXP::Point> BuildNoiseExpr(const std::vector<NOTE::Note>& notes,
		                                       const std::vector<LYRIC::Event>& lyrics,
		                                       Lang lang)
		{
			std::vector<EXP::Point> out;
			for (const NOTE::Note& n : notes) {
				// 找离这个音符最近的歌词（跟 6.3 同思路：就近，不消耗）
				const std::string syl = NearestLyric(lyrics, n.quarters, lang);
				if (syl.empty())
					continue;

				// ★ 2026-09-18：与 SegBuilder 同源 —— 噪声幅度也用 Resolve 的声母，
			//   否则"合成层剥出来的声母"和"噪声曲线用的声母"可能不是同一个。
			const PHON::Syllable rs = syl.empty() ? PHON::Syllable{} : PHON::Resolve(syl, lang);
			const Consonant* c = rs.consonant;
				if (!c || !c->noiseOnset || c->manner == Manner::Nasal)
					continue;                     // 无害噪声的辅音不占噪声曲线

				const double amp = (c->noiseAmp > 0.0) ? c->noiseAmp : 1.0;
				int v = static_cast<int>(kDefaultCc11 * amp + 0.5);
				if (v < 1)   v = 1;
				if (v > 127) v = 127;

				EXP::Point p;
				p.quarters = n.quarters;
				p.value = v;
				out.push_back(p);
			}
			// 去重（同 tick 保留最后一条）
			std::stable_sort(out.begin(), out.end(),
				[](const EXP::Point& a, const EXP::Point& b) { return a.quarters < b.quarters; });
			for (std::size_t i = 0; i + 1 < out.size();) {
				if (out[i].quarters == out[i + 1].quarters)
					out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
				else
					++i;
			}
			return out;
		}
	}

	// ── 主入口 ──────────────────────────────────────────────────

	/// 段表构造器 —— **midi2wav 与 midi2midi 共用的事实源**（2026-09-18，方案 A）。
	///
	/// 读音决策全部发生在这一步：歌词→音节→音素、动程关键点、ms→tick、
	/// 音色缩放、力度口径（kGateVel / murmurAmp）。两个后端只负责"落地"：
	///   · MIDI 后端 = 段表 → note/CC 事件 → Smf_Writer
	///   · WAV  后端 = 段表 → 样点 → Wav_Render
	struct SegBuilder {
		const std::vector<LYRIC::Event>& lyrics;
		const PITCH::Curve&              pitch;
		const Options&                   opt;
		unsigned                         inPpqn   = 480;
		std::uint32_t                    outPpqn  = 480;
		const std::vector<BR::Event>&    tempo;
		Lang                             lang     = Lang::Zh;
		int                              warnCount = 0;   ///< bend 超界（每个音符数一次，与重构前一致）

		double MsPerTickAt(double quarters) const
		{
			double bpm = 120.0;
			if (opt.bpmGiven && opt.bpm > 0.0) {
				bpm = opt.bpm;
			} else if (!tempo.empty()) {
				const double inTickPos = quarters * static_cast<double>(inPpqn);
				for (const BR::Event& e : tempo) {
					if (static_cast<double>(e.tick) <= inTickPos + 1e-9) bpm = e.bpm;
					else break;
				}
			}
			if (!(bpm > 0.0)) bpm = 120.0;
			return (60000.0 / bpm) / static_cast<double>(outPpqn);
		}
		std::uint32_t MsToTicks(double ms, double quarters) const
		{
			if (!(ms > 0.0)) return 0;
			const double mpt = MsPerTickAt(quarters);
			if (!(mpt > 0.0)) return 0;
			return static_cast<std::uint32_t>(ms / mpt + 0.5);
		}
		/// 音节解析 —— **2026-09-18 起改用 `PHON::Resolve()`**。
		///
		/// 以前这里走的是"产品自带的声母剥离 + `FindVowel` 整串匹配"：
		/// 前者只认中文声母表，后者不做剥离 → **带声母的音节必然落中性 (500,1500,2500)**
		/// （见 docs/DEBUGGING.md §4）。所以"辅音×元音"这个需求在旧路径上根本无从谈起 ——
		/// 换哪个韵母出来都是同一个中性音。
		///
		/// `Resolve()` 走协议 5.1.9 的完整识别顺序（舌尖元音特判 → 零声母规范 →
		/// j/q/x+u→v → 剥声母 → 精确查表 → 后缀最长匹配 → 韵腹回退），
		/// 并且按 Zh → Ja → En 跨语言回退 —— 所以 `tsu` / `en_ch` 这类也能命中。
		PHON::Syllable Resolve1(const NOTE::Note& n) const
		{
			const std::string syl = NearestLyric(lyrics, n.quarters, lang);
			if (syl.empty()) return PHON::Syllable{};
			return PHON::Resolve(syl, lang);
		}

		/// 把一个音符展开成段。顺序固定：成阻 → 爆破 → 噪声 → murmur/过渡/元音。
		void BuildNote(const NOTE::Note& n, int velo, double ppqnD,
		               std::vector<Segment>& out)
		{
			const double mpt = MsPerTickAt(n.quarters);
			const double centsAbs = (n.number - 69) * 100.0
			                      + PITCH::CentsAt(pitch, n.quarters);
			const double freq = 440.0 * std::pow(2.0, centsAbs / 1200.0);
			const NoteBend nb = FreqToNoteBend(freq, kOutputRpn);
			if (nb.clamped) ++warnCount;

			const std::uint32_t tOn = static_cast<std::uint32_t>(n.quarters * ppqnD + 0.5);

			const PHON::Syllable syll = Resolve1(n);
			const Consonant* leadC = syll.consonant;

			// ★★ v1.0.2demo：**元音落在原音符位置上**（2026-09-18 交办）
			//
			//  问题：v1.0.1 把辅音（成阻 / 爆破 / 噪声 / 过渡）**插在音符位置之后**，
			//        于是元音本体从 `tOn + 过渡段` 才开始响 —— 带声母的音节整体往后挪，
			//        **节奏跑偏**；而纯韵母没有过渡段，反而是准的（这就是"有的音对、有的不对"）。
			//
			//  改法：把整个音节**往前挪一个「辅音提前量」leadTk**，让元音本体正好落在 tOn：
			//        · 过渡段在音符**之内**（占前 headFrac），所以提前量 = 过渡段长度
			//        · 鼻音的 murmur 也在过渡段之内 ⇒ 提前量里**只补**它占掉的那一份
			//        · 成阻 / 爆破 / 噪声是**自成段落**的辅音，接在过渡段之前 ⇒ 再往前排
			//        · 元音本体不缩短 ⇒ 音符**总时长变长**，长出来的正是辅音那段
			//
			//  ⚠️ 代价（明知而为之）：带声母的音节会比原音符**早** leadTk 起声，
			//     前一个音的音尾会被这一小段辅音盖掉一点。**节奏（元音位置）优先。**
			//  ⚠️ 第一个音符的 tOn 就是整首最早的段起点 ⇒ leadTk 被夹成 0，
			//     所以「首音」与「纯元音音符」的行为**完全不变** —— 这两类可以当回归基准。
			const double durOrig   = n.duration;
			const double durTkOrig = durOrig * ppqnD;

			const double headFrac = (leadC && leadC->transitionMs > 0) ? 0.20 : 0.0;
			double murmurFrac = 0.0;
			if (headFrac > 0.0 && leadC->manner == PHON::Manner::Nasal
			    && leadC->murmurMs > 0 && durTkOrig > 0.0) {
				const std::uint32_t mTk = MsToTicks(static_cast<double>(leadC->murmurMs), n.quarters);
				double f = static_cast<double>(mTk) / durTkOrig;
				if (f >= headFrac) f = headFrac * 0.999;    // murmur 不许吃掉整个过渡段
				if (f > 0.0) murmurFrac = f;
			}

			std::uint32_t leadTk = 0;
			if (headFrac > 0.0) {
				// ★ lead 的语义：**元音本体（Vowel 段）的起点要落在 tOn**。
				//
				//   段表是相对 tBase 摆的，而过渡段占 [0, winTk)：
				//       Vowel 段起点 = tBase + winTk = tOn − leadTk + winTk
				//   要它等于 tOn ⇒ **leadTk 必须等于 winTk**。
				//   （v1.0.2 第一版误把 winTk 之外又加了 winTk×(1−murmurFrac)，
				//     于是元音提前了整整一个过渡段 —— 实测 fricative 早 96 tick。）
				//
				//   过渡段之内的辅音（murmur）不用往外让：它本来就在 [0, winTk) 里，
				//   而过渡段整体已经落在音符之前了。
				//
				//   自成段落的辅音（成阻 / 爆破 / 噪声）接在 tBase 之前，
				//   它们**额外**把 lead 撑大 —— 因为这些段是从 tBase 往前数的。
				const double winTk = headFrac * durTkOrig;                  // 过渡段长度（tick）
				// ★ 实测结论（2026-09-18，用 --lead trace 逐音节量出来的）：
				//   Vowel 段起点 = tOn − leadTk + winTk，要与 tOn 重合就**必须 leadTk = winTk**。
				//   把成阻 / 爆破 / 噪声**再加**进 lead 会让元音提前整整那么多 tick
				//   （实测 b 早 6、s 早 96）。所以它们不额外撑大 lead ——
				//   它们是**自成段落**的辅音，本来就要挤进「音符之前那一小段」里。
				double prepTk = winTk;
				leadTk = static_cast<std::uint32_t>(prepTk + 0.5);
				if (leadTk > tOn) leadTk = tOn;                             // 不许早于整首开头
			}
			// 段起点下限：绝不为负（第一个音符把提前量夹成 0 就是这个机制）
			const std::uint32_t tBase = (tOn >= leadTk) ? (tOn - leadTk) : 0u;

			// （自成段落的辅音排在 tBase 之前，不再对音符原终点做裁剪 —— v1.0.2demo）

			// (a) 成阻静音 → Closure 段
			//     ⚠️ 长度仍是 **1 tick**（设计定案过：480 PPQN 下就是它）——
			//        这里只把它**往前挪**，不换真时长。
			if (leadC && leadC->closureMs > 0) {
				Segment s;
				const std::uint32_t g0 = (tBase >= 1u) ? (tBase - 1u) : 0u;
				if (tBase > g0) {
					s.t0 = g0; s.t1 = tBase;
					s.kind = SegKind::Closure;
					s.f0 = freq; s.f8a[0] = 0; s.vel = kGateVel; s.msPerTick = mpt;
					out.push_back(s);
				}
			}

			// (a2) 爆破脉冲 → Burst 段（2026-09-18 新增）
			//
			//  与 A 组 `RenderCVA()` 第 2 步对齐：5 ms 带通噪声 × burstAmp。
			//  位置：紧跟 gate 之后（gate 按设计决定**保持 1 tick** —— 480 PPQN 下的最小单位，
			//  不换成 closureMs 的真时长）。
			//  ★ 为什么必须有：b/d/g 的 `noiseOnset=false`，此前 CH9 一个音符都不发，
			//    而 A 组给每个"有成阻"的辅音都渲了爆破 —— 这就是"爆破方式没区别"的根。
			//  ★ v1.0.2demo：爆破**接在成阻之后、过渡段之前**（即 tBase 之前），
			//    所以它的右端不再受 tOff（音符原终点，在 tBase 之后）裁剪。
			if (leadC && leadC->burstMs > 0) {
				const std::uint32_t bTk = MsToTicks(static_cast<double>(leadC->burstMs), n.quarters);
				std::uint32_t b = tBase;
				std::uint32_t a = (b >= (bTk > 0 ? bTk : 1)) ? (b - (bTk > 0 ? bTk : 1)) : 0u;
				if (b > a) {
					Segment s;
					s.t0 = a; s.t1 = b;
					s.kind = SegKind::Burst;
					s.f0 = freq;
					// 中心频率：有 noisePeak 用它，否则 F2×2 —— 与 RenderCVA 同一规则
					s.noiseHz = (leadC->noisePeak > 0)
						? static_cast<double>(leadC->noisePeak)
						: static_cast<double>(leadC->f2) * 2.0;
					const double amp = (leadC->burstAmp > 0.0) ? leadC->burstAmp : 0.8;
					int vB = static_cast<int>(velo * amp + 0.5);
					if (vB < 1)   vB = 1;
					if (vB > 127) vB = 127;
					s.vel = vB; s.msPerTick = mpt;
					out.push_back(s);
				}
			}
			// (b) 噪声段 → Noise 段（走 CH9）
			//     ★ v1.0.2demo：噪声起点 = tBase + 成阻时长（原本是 tOn + 成阻，整体前移）；
			//       时长**不裁**（它是自成段落的辅音，右端本来就在过渡段之前）。
			if (leadC && leadC->noiseOnset && leadC->noiseMs > 0) {
				const std::uint32_t closureTk =
					MsToTicks(static_cast<double>(leadC->closureMs), n.quarters);
				const std::uint32_t noiseTk =
					MsToTicks(static_cast<double>(leadC->noiseMs), n.quarters);
				const std::uint32_t a = tBase + closureTk;
				const std::uint32_t b = a + noiseTk;
				if (b > a) {
					Segment s;
					s.t0 = a; s.t1 = b;
					s.kind = SegKind::Noise;
					s.f0 = freq;
					s.noiseHz = (leadC->noisePeak > 0) ? static_cast<double>(leadC->noisePeak) : 4000.0;
					const double amp = (leadC->noiseAmp > 0.0) ? leadC->noiseAmp : 1.0;
					int vN = static_cast<int>(velo * amp + 0.5);
					if (vN < 1)   vN = 1;
					if (vN > 127) vN = 127;
					s.vel = vN; s.msPerTick = mpt;
					out.push_back(s);
				}
			}

			// (c) murmur / 过渡 / 元音
			//   兜底对象的生存期必须到本函数结束（曾经写成 if 里的局部 → 悬垂指针）
			PHON::Vowel fallback;
			const PHON::Vowel* vw = syll.vowel;          // ★ Resolve 已经把韵母剥出来查好了
			if (!vw || vw->points.empty()) {
				fallback.points.push_back(PHON::Neutral());
				vw = &fallback;
			}

			std::vector<std::pair<double, int[8]>> pts;
			auto pushPt = [&](double off, const PHON::VowelPoint& vp) {
				int f8[8];
				PHON::ExtendTo8(vp, f8);
				PHON::ApplyVoiceTo8(f8, opt.voice);
				std::pair<double, int[8]> pr2;
				pr2.first = off;
				for (int k = 0; k < 8; ++k) pr2.second[k] = f8[k];
				pts.push_back(pr2);
			};

			double headFracW = 0.0;              // 本音符实际用到的过渡段占比（= headFrac）
			if (leadC && headFrac > 0.0) {
				PHON::VowelPoint cp;
				cp.f1 = leadC->f1; cp.f2 = leadC->f2; cp.f3 = leadC->f3;
				pushPt(0.0, cp);
				headFracW = headFrac;

				if (murmurFrac > 0.0) pushPt(murmurFrac, cp);   // murmur 段：共振峰停在 locus
			}

			const std::size_t nP = vw->points.size();
			for (std::size_t k = 0; k < nP; ++k) {
				const double off = headFracW
					+ (1.0 - headFracW) * (nP > 1 ? static_cast<double>(k) / (nP - 1) : 0.0);
				pushPt(off, vw->points[k]);
			}

			auto interpF8 = [&](double off, int out8[8]) {
				if (pts.empty()) { PHON::ExtendTo8(PHON::Neutral(), out8); return; }
				if (pts.size() == 1) {
					for (int k = 0; k < 8; ++k) out8[k] = pts[0].second[k];
					return;
				}
				std::size_t i = 0;
				while (i + 2 < pts.size() && off > pts[i + 1].first) ++i;
				const double a = pts[i].first;
				const double b = pts[i + 1].first;
				const double u = (b > a) ? ((off - a) / (b - a)) : 0.0;
				const double uc = (u < 0.0) ? 0.0 : (u > 1.0 ? 1.0 : u);
				for (int k = 0; k < 8; ++k) {
					const double fa = pts[i].second[k];
					const double fb = pts[i + 1].second[k];
					out8[k] = static_cast<int>(fa + (fb - fa) * uc + 0.5);
				}
			};

			std::vector<double> offs;
			offs.push_back(0.0);
			for (const auto& pr2 : pts)
				if (pr2.first > 0.0) offs.push_back(pr2.first);
			offs.push_back(1.0);
			const int kSeg = 4;
			for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
				for (int j = 1; j < kSeg; ++j) {
					const double a = pts[i].first, b = pts[i + 1].first;
					offs.push_back(a + (b - a) * j / kSeg);
				}
			}
			std::stable_sort(offs.begin(), offs.end());

			const double dur = durOrig;
			const bool inConsonantWindow = (leadC && headFracW > 0.0);
			auto velForOffset = [&](double off) -> int {
				if (murmurFrac > 0.0 && off < murmurFrac) {
					const double amp = (leadC->murmurAmp > 0.0) ? leadC->murmurAmp : 0.6;
					int mv = static_cast<int>(velo * amp + 0.5);
					if (mv < 1)   mv = 1;
					if (mv > 127) mv = 127;
					return mv;
				}
				return inConsonantWindow && off < headFracW ? kGateVel : velo;
			};

			for (std::size_t i = 0; i + 1 < offs.size(); ++i) {
				const double oa = offs[i], ob = offs[i + 1];
				if (ob - oa <= 1e-9) continue;

				// ★ 归零点 = tBase（= tOn − leadTk）⇒ 偏移 headFracW 处正好落在 **tOn**，
				//   也就是「元音本体从原音符位置开始响」。这就是 v1.0.2demo 那一条改动。
				const std::uint32_t sa = tBase + static_cast<std::uint32_t>(oa * dur * ppqnD + 0.5);
				const std::uint32_t sb = tBase + static_cast<std::uint32_t>(ob * dur * ppqnD + 0.5);
				if (sb <= sa) continue;

				const double centsF0 = (n.number - 69) * 100.0
				                     + PITCH::CentsAt(pitch, n.quarters + (oa * dur - leadTk / ppqnD));
				const double f0 = 440.0 * std::pow(2.0, centsF0 / 1200.0);

				Segment s;
				s.t0 = sa; s.t1 = sb;
				s.kind = (murmurFrac > 0.0 && oa < murmurFrac) ? SegKind::Murmur
				       : (inConsonantWindow && oa < headFracW) ? SegKind::Transition
				       :                                         SegKind::Vowel;
				s.f0  = f0;
				s.vel = velForOffset(oa);
				s.msPerTick = mpt;
				interpF8(oa, s.f8a);
				interpF8(ob, s.f8b);
				out.push_back(s);
			}
		}
	};
	Result Convert(const Options& opt)
	{
		Result res;

		if (opt.input.empty() || opt.output.empty()) {
			res.message = "错误：输入或输出路径为空。";
			return res;
		}
		if (opt.channel < 1 || opt.channel > 16) {
			res.message = "错误：输入通道必须在 1-16 之间。";
			return res;
		}

		// 1) PPQN（协议 7.1：与输入一致）
		const PPQN::Result pr = PPQN::PR(opt.input);
		if (!pr.ok || pr.ppqn < 96) {
			res.message = "错误：读不到 PPQN，或 PPQN < 96（协议 4.1 要求 >=96）。";
			return res;
		}
		const unsigned ppqn = static_cast<unsigned>(pr.ppqn);

		// 2) 各 reader 榨参数（互不相识，各自只看一摊）
		const TSR::Result tsr = TSR::TSR(opt.input);
		const BR::Result  br  = BR::BR(opt.input);
		const SR::Result  sr  = SR::SR(opt.input);
		const NOTE::Result nm = NOTE::NM(opt.input);
		const PITCH::Result pm = PITCH::PM(opt.input, sr.system);
		const EXP::Result  ex  = EXP::EM(opt.input);
		// 歌词：小 gate 要判断「这个音节有没有声母」，噪声曲线要取噪声声母的幅度
		const LYRIC::Result lm = LYRIC::LM(opt.input);

		if (!nm.ok || nm.notes.empty()) {
			res.message = "错误：输入文件里没有音符（通道 " + std::to_string(opt.channel) + "）。";
			return res;
		}

		std::vector<MetaEvent> setupMeta;
		std::vector<Event>     voiceEvents;

		// ★ PPQN 对齐：输出固定 480（设计决定）。若输入不是 480，
		//   继承过来的 tick 必须按比例换算，否则同一根 tick 在 224 与 480 下
		//   代表的音乐时间不同 —— 小节线、tempo、音符全会整体错位。
		//   换算用 64 位再四舍五入，避免浮点累积误差。
		const std::uint32_t kOutPpqn = 480;
		auto scaleTick = [&](int inTick) -> std::uint32_t {
			if (static_cast<std::uint32_t>(pr.ppqn) == kOutPpqn)
				return static_cast<std::uint32_t>(inTick);
			const long long t = static_cast<long long>(inTick) * kOutPpqn
			                  + (pr.ppqn / 2);          // +半个输入 tick 做四舍五入
			return static_cast<std::uint32_t>(t / pr.ppqn);
		};

		// 3) Setup 轨（协议 7.4，顺序固定）

		// 3a) SysEx（协议 8.2）
		//
		// ⚠️⚠️ 两条都必须发，缺一不可：
		//   · Reset      —— 把音源置于已知状态
		//   · CH10 解锁  —— **必须**！MIDI 通道 10 默认是鼓通道，
		//                   不解锁的话 CH9（噪声）根本发不出声，整条噪声轨报废。
		//   校验和按 Roland 公式：checksum = (128 - ((addr0+addr1+addr2+data) & 0x7F)) & 0x7F
		//     GS Reset 地址 40 00 7F 数据 00 -> sum 0xBF -> chk 0x41
		//     GS CH10  地址 40 10 15 数据 00 -> sum 0x65 -> chk 0x1B
		{
			if (opt.format == Format::GS) {
				const std::uint8_t gsReset[]  = { 0x41,0x10,0x42,0x12,0x40,0x00,0x7F,0x00,0x41,0xF7 };
				PushSysEx(setupMeta, 0, std::vector<std::uint8_t>(gsReset, gsReset + sizeof(gsReset)));

				const std::uint8_t gsUnlock[] = { 0x41,0x10,0x42,0x12,0x40,0x10,0x15,0x00,0x1B,0xF7 };
				PushSysEx(setupMeta, 0, std::vector<std::uint8_t>(gsUnlock, gsUnlock + sizeof(gsUnlock)));
			} else if (opt.format == Format::XG) {
				const std::uint8_t xgReset[]  = { 0x43,0x10,0x4C,0x00,0x00,0x7E,0x00,0xF7 };
				PushSysEx(setupMeta, 0, std::vector<std::uint8_t>(xgReset, xgReset + sizeof(xgReset)));

				const std::uint8_t xgUnlock[] = { 0x43,0x10,0x4C,0x08,0x1A,0x00,0x00,0xF7 };
				PushSysEx(setupMeta, 0, std::vector<std::uint8_t>(xgUnlock, xgUnlock + sizeof(xgUnlock)));
			}
			// GM：协议 8.2 明确「无 SysEx，CH10 保持鼓通道，噪声合并到 CH8」（L12）
		}

		// 3b) 拍号（协议 4.3 继承；兜底 4/4 @ 0）
		{
			std::vector<TSR::Event> tl = tsr.timeline;
			if (tl.empty() || tl.front().tick != 0) {
				TSR::Event fb;
				fb.tick = 0; fb.numerator = 4; fb.denomPow = 2;
				tl.insert(tl.begin(), fb);
			}
			for (const TSR::Event& e : tl) {
				const std::uint8_t p[4] = {
					static_cast<std::uint8_t>(e.numerator),
					static_cast<std::uint8_t>(e.denomPow),
					24, 8
				};
				PushMeta(setupMeta, scaleTick(e.tick), 0,
				         std::vector<std::uint8_t>(p, p + 4), 0x58);
			}
		}

		// 3c) tempo（协议 4.3 继承；--bpm 则拉平为恒定）
		if (opt.bpmGiven && opt.bpm > 0.0) {
			const double usD = 60000000.0 / opt.bpm;
			const std::uint32_t us = static_cast<std::uint32_t>(usD + 0.5);
			const std::uint8_t p[3] = {
				static_cast<std::uint8_t>((us >> 16) & 0xFF),
				static_cast<std::uint8_t>((us >> 8) & 0xFF),
				static_cast<std::uint8_t>(us & 0xFF)
			};
			PushMeta(setupMeta, 0, 1, std::vector<std::uint8_t>(p, p + 3), 0x51);
		} else if (br.ok && !br.timeline.empty()) {
			for (const BR::Event& e : br.timeline) {
				// ⚠️ BR::Event **没有** tempoUs 字段，只有 bpm（见 BPM_Reader.h）：
				//    bpm = 60000000 / us  ⟹  us = 60000000 / bpm
				const double usD = (e.bpm > 0.0)
					? (60000000.0 / e.bpm)
					: static_cast<double>(kDefaultTempoUs);
				const std::uint32_t us = static_cast<std::uint32_t>(usD + 0.5);
				const std::uint8_t p[3] = {
					static_cast<std::uint8_t>((us >> 16) & 0xFF),
					static_cast<std::uint8_t>((us >> 8) & 0xFF),
					static_cast<std::uint8_t>(us & 0xFF)
				};
				PushMeta(setupMeta, scaleTick(e.tick), 1,
				         std::vector<std::uint8_t>(p, p + 3), 0x51);
			}
		} else {
			const std::uint8_t p[3] = { 0x07, 0xA1, 0x20 };   // 500000
			PushMeta(setupMeta, 0, 1, std::vector<std::uint8_t>(p, p + 3), 0x51);
		}

		// 3d) 调号（--key 覆盖；本轮不读输入的 FF 59，见 RECOVERY-SPEC §5）
		if (opt.keyGiven) {
			const std::uint8_t p[2] = {
				static_cast<std::uint8_t>(opt.keySf & 0xFF),
				static_cast<std::uint8_t>(opt.keyMi & 0xFF)
			};
			PushMeta(setupMeta, 0, 0, std::vector<std::uint8_t>(p, p + 2), 0x59);
		}

		// 4) 每轨 tick 0 初始化
		for (int ch = 0; ch < CH_COUNT; ++ch)
			PushInit(voiceEvents, ch, opt.format, opt.cc7Scale, opt.noisePc);

		// 5) 音符 → F0 通道（本轮 demo：只有基频层）
		//
		//    频率由「音高编号 + 弯音音分」得出，跟协议 7.6 互为反函数：
		//        cents_abs = (number - 69) * 100 + PITCH::CentsAt(curve, qn)
		//        freq      = 440 * 2^(cents_abs / 1200)
		//
		// ⚠️ 输出 PPQN 固定 480（设计决定），未必等于输入 PPQN，
		//    所以 tick 要以**输出** ppqn 为基准重新算（qn × 480）。
		const double ppqnD = static_cast<double>(kOutPpqn);

		// ★ ms → tick（2026-09-18 接线）
		//
		//  字典里的时长**全是毫秒**（CONSONANT-DICT §8 第 5 条：明令禁止硬编码 tick ——
		//  慢曲快曲必须不一样长）。换算要用**该位置上的** tempo：
		//      tick = ms / (60000 / BPM / PPQN)
		//  ⚠️ BR::Event 只有 bpm 和 **输入 tick**（没有 quarters），所以先按输入 PPQN 折回位置。
		auto msPerTickAt = [&](double quarters) -> double {
			double bpm = 120.0;
			if (opt.bpmGiven && opt.bpm > 0.0) {
				bpm = opt.bpm;
			} else if (br.ok && !br.timeline.empty()) {
				const double inTickPos = quarters * static_cast<double>(ppqn);
				for (const BR::Event& e : br.timeline) {
					if (static_cast<double>(e.tick) <= inTickPos + 1e-9) bpm = e.bpm;
					else break;
				}
			}
			if (!(bpm > 0.0)) bpm = 120.0;
			return (60000.0 / bpm) / static_cast<double>(kOutPpqn);
		};
		auto msToTicks = [&](double ms, double quarters) -> std::uint32_t {
			if (!(ms > 0.0)) return 0;
			const double mpt = msPerTickAt(quarters);
			if (!(mpt > 0.0)) return 0;
			return static_cast<std::uint32_t>(ms / mpt + 0.5);
		};

		// 语言：本轮 CLI 还没暴露 --lang，先固定 zh（罗马音场景下
		// 中文韵母表同样能查到 a/i/u 这些单字母，够用）。接字典时再补参数。
		const Lang lang = Lang::Zh;

		// ★ 5) 段表 → MIDI 事件（**两个后端共用的事实源**，2026-09-18 重构）
		//
		//   ⚠️ 这一段产出的事件内容与顺序必须与重构前**逐字节一致**：
		//      回归判据 = 同一输入出的 SMF 与重构前 SHA256 相同
		//      （重构后已用 24 份 ab_*_sq.mid 复核过）。
		std::vector<Segment> segs;
		{
			SegBuilder builder{ lm.timeline, pm.curve, opt, ppqn, kOutPpqn, br.timeline, lang, 0 };
			for (const NOTE::Note& n : nm.notes) {
				if (n.duration <= 0.0)
					continue;                                  // 协议 6.1：零长度丢弃

				int velo = static_cast<int>(n.velocity * opt.velocityScale + 0.5);
				// 乘性通道配比（设计决定，覆盖协议 7.5 的「16 通道同 velocity」）
				velo = static_cast<int>(velo * (kCc7[CH_F0] / 100.0) + 0.5);
				if (velo < 1)   velo = 1;                      // 协议 7.5：下限 1
				if (velo > 127) velo = 127;

				builder.BuildNote(n, velo, ppqnD, segs);
				++res.notes;
			}
			res.warnings += builder.warnCount;
		}

		// 段表 → 事件：成阻 → gate 音符；噪声 → CH9 音符；其余 → 12 个发声通道各自走 7.6
		for (const Segment& s : segs) {
			if (s.kind == SegKind::Closure) {
				const NoteBend nb = FreqToNoteBend(s.f0, kOutputRpn);
				PushGate(voiceEvents, s.t0, CH_F0, nb.note, s.vel);
			} else if (s.kind == SegKind::Noise || s.kind == SegKind::Burst) {
				// 噪声与爆破都是 CH9 上的带通噪声（协议 L434：噪声不发 bend）
				EmitNoiseNote(voiceEvents, s.t0, s.t1, CH_NOISE, s.noiseHz, s.vel, res.warnings);
			} else {
				for (int ch = CH_F0; ch < CH_COUNT; ++ch) {
					if (ch == CH_NOISE)
						continue;                 // 噪声走单独一条路
					const double f = FormantFreqs(ch, s.f0, s.f8a);
					if (!(f > 0.0))
						continue;                 // F1W/F2W 在窄间距元音上被规则关掉
					// ★ 2026-09-18 hotfix：**每通道 velocity 乘性配比**（真正落地）
					//   velocity_out(ch) = clamp(round(源 velocity × velocity_scale × CC7表值(ch)/100))
					//   这是本项目就定下的规则（见 docs/OUTPUT-PARAMS.md §1.1 的决策登记），
					//   但代码此前只乘了 CH_F0 那一档（= ×1.0 空操作），然后把**同一个力度**
					//   广播给 15 个通道。实测后果：F0/H2/H3/H4 这些"填充物"和共振峰一样响，
					//   而它们恰好落在窄间距元音的 F1–F2 谷里（`/u/` 的 440/660）。
					//   协议 7.3 的 CC7 表本来就是**响度配比**，这里只是让它真正生效。
					int vCh = static_cast<int>(s.vel * (kCc7[ch] / 100.0) + 0.5);
					if (vCh < 1)   vCh = 1;
					if (vCh > 127) vCh = 127;
					EmitNote(voiceEvents, s.t0, s.t1, ch, f, vCh, res.warnings);
				}
			}
		}
		// 6) CC11 广播（协议 7.5：保留源 CC11 时间线，广播到全部 Voice 轨）
		//    本轮先广播到 CH0-CH8；CH9 噪声单独一条曲线是后续工作（OUTPUT-PARAMS §4.3）
		for (const EXP::Point& p : ex.curve.points) {
			const std::uint32_t tick = static_cast<std::uint32_t>(p.quarters * ppqnD + 0.5);
			int v = p.value;
			if (v < 1)   v = 1;
			if (v > 127) v = 127;
			for (int ch = CH_F0; ch <= CH_F8; ++ch)
				PushCc(voiceEvents, tick, ch, 11, static_cast<std::uint8_t>(v));
		}

		// 6b) ★ 噪声通道（CH9）**单独一条 CC11 曲线**（设计决定，OUTPUT-PARAMS §4.3）
		//
		//     与上面那条的区别：只在**有噪声声母**的音符处取值，大小来自该辅音的
		//     relative amplitude（noiseAmp）。无噪声的地方保持上一个值 —— 这正是
		//     采样保持语义，也让「这一段噪声多响」一眼看得出。
		//
		//     ⚠️ CC7 不动：噪声通道仍按协议 7.3 表 = 45。
		{
			const std::vector<EXP::Point> noiseCurve =
				BuildNoiseExpr(nm.notes, lm.timeline, lang);
			for (const EXP::Point& p : noiseCurve) {
				const std::uint32_t tick =
					static_cast<std::uint32_t>(p.quarters * ppqnD + 0.5);
				int v = p.value;
				if (v < 1)   v = 1;
				if (v > 127) v = 127;
				PushCc(voiceEvents, tick, CH_NOISE, 11, static_cast<std::uint8_t>(v));
			}
		}

		// 7) 排序（协议 7.5：(tick, priority)）
		std::stable_sort(voiceEvents.begin(), voiceEvents.end(),
			[](const Event& a, const Event& b) {
				if (a.tick != b.tick)         return a.tick < b.tick;
				if (a.priority != b.priority) return a.priority < b.priority;
				return a.channel < b.channel;
			});
		std::stable_sort(setupMeta.begin(), setupMeta.end(),
			[](const MetaEvent& a, const MetaEvent& b) {
				if (a.tick != b.tick) return a.tick < b.tick;
				return a.kind < b.kind;                  // 协议 7.4：(tick, kind)
			});

		// 8) 落盘 —— ★ PPQN 固定 480（设计决定），不再沿用输入值
		std::string err;
		if (!WriteSmf(opt.output, kOutPpqn, setupMeta, voiceEvents, err)) {
			res.message = "写文件失败：" + err;
			return res;
		}

		res.ok = true;
		res.tracks = 17;
		res.message = "已合成 17 轨（" + std::string(FormatName(opt.format))
		            + "，输出 PPQN " + std::to_string(kOutPpqn)
		            + (ppqn == kOutPpqn ? "" : "，输入 " + std::to_string(ppqn) + " 已对齐")
		            + "，音色 " + PHON::VoiceName(opt.voice)
		            + "）：音符 " + std::to_string(res.notes)
		            + "，事件 " + std::to_string(voiceEvents.size());
		return res;
	}

	Result ConvertToWav(const Options& opt)
	{
		Result res;

		if (opt.input.empty() || opt.output.empty()) {
			res.message = "错误：输入或输出路径为空。";
			return res;
		}

		// 1) PPQN（与 Convert 同一道门槛）
		const PPQN::Result pr = PPQN::PR(opt.input);
		if (!pr.ok || pr.ppqn < 96) {
			res.message = "错误：读不到 PPQN，或 PPQN < 96（协议 4.1 要求 >=96）。";
			return res;
		}
		const unsigned ppqn = static_cast<unsigned>(pr.ppqn);
		const std::uint32_t outPpqn = 480;          // 设计决定：输出固定 480

		// 2) reader 参数（不需要拍号/表情/Setup 轨 —— 那些只服务 MIDI 后端）
		const BR::Result    br = BR::BR(opt.input);
		const SR::Result    sr = SR::SR(opt.input);
		const NOTE::Result  nm = NOTE::NM(opt.input);
		const PITCH::Result pm = PITCH::PM(opt.input, sr.system);
		const LYRIC::Result lm = LYRIC::LM(opt.input);

		if (!nm.ok || nm.notes.empty()) {
			res.message = "错误：输入文件里没有音符（通道 " + std::to_string(opt.channel) + "）。";
			return res;
		}

		// 3) 段表 —— ★ 与 MIDI 后端用**同一个构造器**（方案 A 的全部意义）
		{
			const double ppqnD = static_cast<double>(outPpqn);
			SegmentTable table;
			SegBuilder builder{ lm.timeline, pm.curve, opt, ppqn, outPpqn, br.timeline, Lang::Zh, 0 };
			for (const NOTE::Note& n : nm.notes) {
				if (n.duration <= 0.0)
					continue;
				int velo = static_cast<int>(n.velocity * opt.velocityScale + 0.5);
				velo = static_cast<int>(velo * (kCc7[CH_F0] / 100.0) + 0.5);
				if (velo < 1)   velo = 1;
				if (velo > 127) velo = 127;
				builder.BuildNote(n, velo, ppqnD, table.segs);
				++table.notes;
			}
			res.notes    = table.notes;
			res.warnings = builder.warnCount;

			// 4) 段表 → 样点（本机合成）
			std::string err;
			if (!WriteWavFromSegments(opt.output, table, err)) {
				res.message = "写 wav 失败：" + err;
				return res;
			}
			res.ok     = true;
			res.tracks = 1;
			res.message = "已直接合成音频（midi2wav，不走 MIDI）：音符 "
			            + std::to_string(table.notes) + "，段 "
			            + std::to_string(table.segs.size())
			            + "（44100 Hz / 16-bit / 单声道 / 峰值 -3 dBFS）";
			return res;
		}
	}
}
