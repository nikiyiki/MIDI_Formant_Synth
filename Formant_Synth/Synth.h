#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Phoneme_Dict/Phoneme_Dict.h"   // PHON::Voice（音色是字典的数据，不是合成层的）

// ─────────────────────────────────────────────────────────────
//  Synth — 把输入 SMF 挤进 17 轨输出（共振峰合成的「合成 + 组装 + 落盘」）
//
//  数据流（协议 1.2：输出是全新 SMF，不基于输入轨编辑）：
//
//      输入 SMF
//         │  各 reader（PPQN / 拍号 / BPM / System / Pitch / Note / Lyric）
//         ▼
//      提取出「寄存器音符」—— 歌词 + 音高轨迹 + 力度
//         │  本模块（音素字典 + 6.x 管线 + 7.6 反算）
//         ▼
//      逻辑事件（16 个通道）
//         │  本模块（17 轨 + tick0 初始化 + priority 排序）
//         ▼
//      Smf_Writer（纯序列化 → 字节）
//
//  本模块**不重读输入文件**的语义层内容，只调 reader 拿 Result。
//  写字节的部分全在 Smf_Writer，本模块不认识 SMF 的 chunk/VLQ。
// ─────────────────────────────────────────────────────────────

namespace SYNTH {

	/// 本工具版本号。
	/// · v1.0.1demo = 首个「能听」的 demo：字典 + 16 通道共振峰 + 音色（男/女/童）+ 全特性测试样本。
	/// · v1.0.2demo = **元音落在原音符位置上**：辅音（成阻/爆破/噪声/过渡）整体前移一个
	///   「辅音提前量」，让元音本体从 tOn 起声 —— 修掉"带声母的音节节奏跑偏"。
	///   音符总时长因此变长（长出来的正是辅音那段）；首音与纯元音音符行为不变。
	constexpr const char* kVersion = "1.0.2demo";

	// ── 事件 ────────────────────────────────────────────────────

	/// 同一 tick 上的排序优先级（协议 7.5）
	enum Priority {
		PRI_NOTE_OFF = 0,
		PRI_RPN      = 1,
		PRI_CTRL     = 2,
		PRI_NOTE_ON  = 3,
	};

	/// 一条要写进输出的事件（已换算成绝对 tick 与通道）
	struct Event {
		std::uint32_t tick = 0;
		int  priority = PRI_CTRL;
		int  channel  = 0;        ///< 0..15 ＝ 输出轨 1..16
		std::uint8_t status = 0;  ///< 0xB0 / 0x90 / 0x80 / 0xE0
		std::uint8_t data1  = 0;
		std::uint8_t data2  = 0;
	};

	/// 一条 meta 事件（只出现在 Setup 轨）。bytes 是完整的 `FF xx len payload`
	struct MetaEvent {
		std::uint32_t tick = 0;
		int kind = 0;             ///< 0 = 拍号/调号/SysEx，1 = tempo（协议 7.4 的合并排序键）
		std::vector<std::uint8_t> bytes;
	};

	// ── 常量表（协议 7.2 / 7.3 / 第 11 章）────────────────────────

	/// 16 个输出通道的角色（协议 7.2：轨 1..16 ＝ CH0..CH15）
	enum ChannelRole {
		CH_F0 = 0, CH_F1, CH_F2, CH_F3, CH_F4, CH_F5, CH_F6, CH_F7,
		CH_F8, CH_NOISE, CH_H2, CH_H3, CH_H4, CH_BREATH, CH_F1W, CH_F2W,
		CH_COUNT = 16,
	};

	extern const int kCc7[CH_COUNT];          ///< 每通道 CC7（协议 7.3）
	const char* ChannelName(int ch);          ///< track_name（协议 7.5）

	extern const int kDefaultTempoUs;         ///< 500000
	extern const int kNoteOffVel;             ///< 64
	extern const int kDefaultCc11;            ///< 127
	extern const int kBendCenter;             ///< 8192

	/// ★ 输出侧恒定弯音范围（设计决定，覆盖协议 8.1 的 GS=12/GM=2）
	///   24 半音 → 量程 ±2400 音分（协议 BEND_RANGE_MAX）
	extern const int kOutputRpn;

	// ── 格式 ────────────────────────────────────────────────────

	enum class Format { GS, XG, GM };
	Format ParseFormat(const std::string& s);
	const char* FormatName(Format f);

	// ── 参数 ────────────────────────────────────────────────────

	struct Options {
		std::string input;
		std::string output;
		Format format = Format::GS;      ///< 默认 GS（设计决定）
		int    channel = 1;              ///< 输入通道 1..16
		PHON::Voice voice = PHON::Voice::Male;  ///< 音色：男（默认）/女/童
		int    cc11FullScale = 127;      ///< --cc11-full-scale
		double velocityScale = 1.0;      ///< --velocity-scale
		double cc7Scale      = 1.0;      ///< --cc7-scale
		bool   bpmGiven = false;
		double bpm      = 0.0;           ///< --bpm（覆盖为恒定 tempo）
		bool   keyGiven = false;
		int    keySf = 0, keyMi = 0;     ///< --key（调号覆盖）

		/// 噪声通道（CH9）的 PC。
		///
		/// ⚠️ 这里与协议 §8.2 的「全通道 PC=80」**有意不同**（2026-09-18，A/B 对比驱动的修正）：
		///    协议写 PC=80 是给共振峰通道找"最接近正弦"的替代品（协议自己把
		///    「无标准正弦波 program」列为已知风险 L9）。而噪声要的是**噪声**：
		///    PC=80 上发噪声音符只会得到一个方波蜂鸣，谱上是**线谱**而不是噪声带 ——
		///    实测 B 组在 6000/7500 Hz 比 A 组低 50 dB，就是这么来的。
		///    GS 的 **PC 121(0-based) = Breath Noise** 才是对应物。
		///    要回到协议原样：`--noise-pc 80`。
		int noisePc = 121;               ///< --noise-pc（CH9 音色）
	};

	// ── 7.6 频率 → note + bend ───────────────────────────────────

	struct NoteBend {
		int  note = 60;
		int  bend = kBendCenter;
		bool clamped = false;            ///< bend 超界被 clamp（协议要求记 warning）
	};
	NoteBend FreqToNoteBend(double freqHz, int rSemitones);

	// ── 写文件（由 Smf_Writer.cpp 实现）─────────────────────────

	/// 写 17 轨 SMF。tracks[0] 由 setupMeta 组成，tracks[1..16] 从 voiceEvents 里挑。
	bool WriteSmf(const std::string& path,
	              unsigned ppqn,
	              const std::vector<MetaEvent>& setupMeta,
	              const std::vector<Event>& voiceEvents,
	              std::string& err);

	// ── 主入口 ──────────────────────────────────────────────────

	struct Result {
		bool ok = false;
		int  notes = 0;          ///< 处理的音符数
		int  tracks = 0;         ///< 写出的轨数（应为 17）
		int  warnings = 0;       ///< bend 超界等
		std::string message;     ///< 可直接显示的一句话
	};

	// ── 段表：midi2wav 与 midi2midi 共用的事实源 ─────────────────
	//
	//  为什么要它（2026-09-18 设计定案，方案 A）：
	//    读音决策（歌词→音节→音素、动程关键点、ms→tick、音色缩放、力度口径）
	//    只该发生**一次**。两个后端都吃同一份段表：
	//      · MIDI 后端（Smf_Writer）  = 段表 → 逻辑事件 → 17 轨 → 交给音源
	//      · WAV  后端（Wav_Render）  = 段表 → 本机合成 → 样点
	//    这样"段表对不对"和"承载方式对不对"就是两个可以分开验的问题。

	enum class SegKind {
		Closure,     ///< 成阻静音（塞音的"堵住"）——WAV 后端写真静音，MIDI 后端发 gate 音符
		Burst,       ///< 爆发脉冲（塞音）——WAV 后端先支持；MIDI 后端本轮还没接线
		Noise,       ///< 持续噪声（擦音 / 送气）——走 CH9
		Murmur,      ///< 鼻音浊音段（共振峰停在 locus）
		Transition,  ///< 辅音 locus → 元音目标的过渡
		Vowel,       ///< 元音本体（含复韵母动程）
	};

	struct Segment {
		std::uint32_t t0 = 0, t1 = 0;   ///< 输出 tick，左闭右开
		SegKind       kind = SegKind::Vowel;
		double        f0 = 220.0;       ///< 基频 Hz（段首采样，跟绝对音高曲线）
		int           f8a[8] = { 0,0,0,0,0,0,0,0 };  ///< 段首共振峰（MIDI 用这个，零阶保持）
		int           f8b[8] = { 0,0,0,0,0,0,0,0 };  ///< 段末共振峰（WAV 在段内线性插值）
		int           vel = 100;        ///< 力度（后端各自决定怎么用）
		double        noiseHz = 0.0;    ///< Noise / Burst 段的中心频率
		double        msPerTick = 500.0 / 480.0;     ///< 本段所在位置的 ms/tick（tempo 决定）
	};

	struct SegmentTable {
		std::vector<Segment> segs;      ///< 按 t0 升序
		int notes = 0;                  ///< 参与展开的音符数
	};

	/// 段表 → wav（本机合成，不走 MIDI）。由 `Formant_Synth/Wav_Render.cpp` 实现。
	/// 口径：44100 Hz / 16-bit / 单声道 / 峰值归一到 −3 dBFS。
	bool WriteWavFromSegments(const std::string& path, const SegmentTable& t, std::string& err);

	/// midi2wav 入口：读同一个输入 SMF，直接出 wav（与 Convert 共用段表）。
	Result ConvertToWav(const Options& opt);

	/// 把输入 SMF 合成成 17 轨输出并落盘。
	Result Convert(const Options& opt);

}
