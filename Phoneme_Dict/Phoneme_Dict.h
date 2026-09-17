#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  Phoneme_Dict —— 音素字典（纯数据 + 查询 + 音节切分）
//
//  定位：**独立接口**。不依赖任何 reader、不读文件、不认识 SMF。
//        Formant_Synth 通过它把「歌词音节」翻成「共振峰目标 + 辅音参数」。
//
//  为什么单独做一层：
//    · 字典是**数据**，合成是**逻辑** —— 换数据不该动代码
//    · 可以单独测（打印某个音素的全部参数），不必跑整条合成链
//
//  协议依据：`FORMANT-SYNTH-00262.txt` 第 5 章（音素字典）：
//    · 5.1   中文 38 韵母 / 5.1.8 声母剥离规则 / 5.1.9 识别顺序
//    · 5.2   日文 6 元音
//    · 5.3   英文 12 元音 + 字母组合别名 + 噪声韵尾
//    · 5.4   最近音素替换（评分公式）
//    · 5.5   最近邻候选排除（舌尖元音不参与）
//  设计文档：`docs/CONSONANT-DICT.md`（辅音参数表 + 依据 + 可信度分级）。
//
//  可信度（见 CONSONANT-DICT.md §0）：
//    P = 协议原文      C = 经典声学语音学结论
//    M = 实测典型值    T = **待真机实测**（先用起点值，听感校准后定稿）
//  本表里辅音参数以 T 为主，元音表直接沿用协议 5.1/5.2/5.3（P）。
// ─────────────────────────────────────────────────────────────

namespace PHON {

	/// 语言。
	enum class Lang { Zh, Ja, En, Unknown };

	Lang ParseLang(const std::string& s);      // "zh"/"ja"/"en" → Lang
	const char* LangName(Lang l);

	// ── 音色（发声人类型）────────────────────────────────────────

	/// 音色。对应 PB52 数据集的 type 分组（原数据是 c / m / w 三个 factor 水平）。
	///
	/// ⚠️ 童声组在 PB52 里只切得出 **2 位发音人**（女声与童声的 f0 重叠，自动分群
	///    分不开）。样本严重不足，但**设计定案：就用这 2 人的均值强行当童声用**。
	///    所以童声那条的可信度是 T-（待真机实测），不是 P/C。
	enum class Voice { Male, Female, Child };

	Voice ParseVoice(const std::string& s);    // "male"/"female"/"child" → Voice
	const char* VoiceName(Voice v);

	/// 音色 → 共振峰缩放系数（相对男声 = 1.0）。推导见 .cpp。
	struct VoiceScale {
		double f1 = 1.0, f2 = 1.0, f3 = 1.0;
	};
	VoiceScale ScaleOf(Voice v);

	/// 把一个**已经外推到 8 个共振峰**的点按音色缩放（就地改 f8[8]）。
	/// 男声是基准 → 直接原样返回。
	void ApplyVoiceTo8(int f8[8], Voice v);

	// ── 发音方式（辅音）─────────────────────────────────────────

	enum class Manner {
		Stop,        ///< 塞音：成阻静音 → burst → 过渡
		Fricative,   ///< 擦音：持续噪声 → 过渡
		Affricate,   ///< 塞擦音：静音 → burst → 紧跟持续噪声
		Nasal,       ///< 鼻音：murmur → 过渡
		Approximant, ///< 近音：起点即目标 → 短过渡
	};

	const char* MannerName(Manner m);

	// ── 辅音条目 ────────────────────────────────────────────────

	/// 一个辅音的全部声学参数。
	/// 时长一律是**毫秒**，由合成层按当前 tempo / PPQN 换算成 tick —— 绝不硬编码 tick。
	struct Consonant {
		std::string key;        ///< "b" / "zh" / "sh" …
		Manner manner = Manner::Stop;

		/// 发音部位的共振峰「起点」（locus）—— 辅音后接元音时，共振峰从这里滑向元音目标
		int f1 = 300, f2 = 1200, f3 = 2500;

		// ── 塞音 / 塞擦音 ──
		int closureMs = 0;      ///< 成阻静音段（塞音必有；塞擦音很短）
		int votMs     = 0;      ///< burst 之后的送气/噪声段（送气音显著更长）

		// ── 擦音 / 塞擦音的摩擦段 ──
		int noiseMs   = 0;      ///< 持续噪声时长
		int noisePeak = 0;      ///< 噪声带中心频率 Hz（0 = 无噪声）
		double noiseAmp = 0.0;  ///< 相对幅度 0..1（决定送 CH9 的 CC11 强弱）

		// ── 鼻音 ──
		int murmurMs  = 0;      ///< 鼻音 murmur 时长
		double murmurAmp = 0.0; ///< murmur 相对幅度

		/// 过渡时长：locus → 韵母目标
		int transitionMs = 0;

		/// 是否送气（送气与否主要差在 VOT，不是频谱）
		bool aspirated = false;

		/// 是否触发 onset 噪声脉冲（协议 5.1.8 的「噪声声母」表）。
		/// ⚠️ b/d/g 不在该表里，但**仍要有 burst** —— 见 CONSONANT-DICT.md §3.6
		bool noiseOnset = false;

		// ── 爆破（burst）─────────────────────────────────────────
		//
		//  ★ 2026-09-18 新增（A/B 对比驱动）：此前 A 组渲染器（RenderCVA §2）
		//    给每个"有成阻"的辅音都渲了一个 5 ms 噪声脉冲，而**产品一个都不发** ——
		//    实测后果：b/d/g 的 CH9 是空的，听感上"爆破方式没有区别"。
		//    两个后端现在都读这两个字段，**A 组与产品必须吃同一套值**。
		int    burstMs  = 0;     ///< 爆破脉冲时长（ms）；0 = 无
		double burstAmp = 0.0;   ///< 爆破相对幅度 0..1
	};

	// ── 元音条目 ────────────────────────────────────────────────

	/// 一个元音/韵母。支持 1~3 个目标点（单韵母 1 个、复韵母 2 个、中响 3 个）。
	struct VowelPoint {
		int f1 = 500, f2 = 1500, f3 = 2500;
	};

	struct Vowel {
		std::string key;                  ///< "a" / "ai" / "iao" …
		std::vector<VowelPoint> points;   ///< 1~3 个；按时间顺序
		Lang lang = Lang::Zh;
	};

	// ── 查询 ────────────────────────────────────────────────────

	/// 查辅音。找不到返回 nullptr。
	const Consonant* FindConsonant(const std::string& key);

	/// 查元音/韵母。查找顺序：
	///   ① (key, lang) 精确命中
	///   ② 该语言的带前缀写法（En → "en_"+key，Ja → "ja_"+key）
	///   ③ 不限语言的第一条同名条目（罗马音场景下中英日写法常混用）
	///   ④ nullptr
	///
	/// ⚠️ 第 ③ 步是**历史行为**，曾经是唯一的回退路径，导致 `FindVowel("e", En)`
	///    命中的是**中文** e（F2 差 440 Hz）。加了第 ② 步之后，英文表才真正可达。
	///    需要「只认这一种语言」时用 ResolveIn()，它不做跨语言回退。
	const Vowel* FindVowel(const std::string& key, Lang lang);

	/// 全部条目（给「列出字典」用）。
	const std::vector<Consonant>& AllConsonants();
	const std::vector<Vowel>&     AllVowels();

	/// 中性元音（协议 6.5：识别失败时的兜底 (500, 1500, 2500)）。
	VowelPoint Neutral();

	/// 把 F1..F3 外推成 F4..F8（协议 6.5 的 extend_formants）。
	/// 填 8 个 int（F1..F8）。
	void ExtendTo8(const VowelPoint& p, int out8[8]);

	// ═══════════════════════════════════════════════════════════
	//  ★ 音节切分（协议 5.1.8 / 5.1.9 / 5.2 / 5.3）
	//
	//  背景：`FindVowel` 是**整串精确匹配**，而真实歌词是「声母 + 韵母」的音节
	//  （"ka" / "shi" / "tsu"）。不做剥离，这些音节永远查不到字典，全部落中性兜底
	//  —— 实测 63 个带声母罗马音命中 0 个。
	// ═══════════════════════════════════════════════════════════

	/// 一次音节解析的结果。
	struct Syllable {
		const Consonant* consonant = nullptr;  ///< 声母/首辅音；nullptr = 零声母或没有
		const Vowel*     vowel     = nullptr;  ///< 韵母/元音；nullptr = 完全查不到
		Lang  matchedLang = Lang::Unknown;     ///< 实际命中哪张表
		bool  fellBack    = false;             ///< 用了「非请求语言」的表
		bool  neutral     = false;             ///< vowel == nullptr，调用方该落中性兜底
		std::string rest;                      ///< 剥离后的韵母串（诊断/测试用）

		/// 拿到目标共振峰点；vowel 为空时给中性点。
		VowelPoint Point() const;
	};

	/// 单语言解析：只认 `lang` 这一张表，**不做跨语言回退**。
	/// 失败时返回 vowel == nullptr（neutral == true）。
	///
	/// 各语言的流程：
	///   · Zh —— 协议 5.1.9 的 8 步识别顺序（舌尖元音特判 → 零声母规范 →
	///           j/q/x+u→v → 剥声母 → 精确查表 → 后缀最长匹配 → 韵腹回退 → None）
	///   · Ja —— 五十音罗马音：先切「行」（含 sh/ch/ts/f/j 的音变），再取元音
	///   · En —— 剥首辅音 + 协议 5.3 的字母组合别名（最长匹配）
	Syllable ResolveIn(const std::string& syllable, Lang lang);

	/// 完整解析：先 ResolveIn(lang)，失败再依次试其它语言（Zh → Ja → En）。
	///
	/// 为什么要跨语言回退：歌词池是**五十音罗马音**（`ka` `shi` `tsu` …），
	/// 而 `--lang` 默认 `en`（OUTPUT-PARAMS.md §2.1）。若严格只认一种语言，
	/// "tsu" 在 en 表下必然失败。回退让「哪个语言」这个选择不再决定成败，
	/// 命中哪张表记录在 `matchedLang` / `fellBack` 里，调用方可自行裁决。
	Syllable Resolve(const std::string& syllable, Lang lang);

	/// 协议 5.4 最近音素替换：按评分找一个「最接近」的元音。
	///   fd = |ΔF1,ΔF2,ΔF3| / 3000 ; sd = 1 - ratio(s, key) ; score = 0.55·fd + 0.45·sd
	///   score > 0.85 → 放弃，返回 nullptr（调用方落中性）
	/// 协议 5.5：`i_apical_back` / `i_apical_front` **不参与**候选。
	const Vowel* NearestVowel(const std::string& syllable, Lang lang);

	/// 两个串的相似度（difflib 风格：2·LCS / 总长）。给 NearestVowel 打分，也给测试用。
	double Similarity(const std::string& a, const std::string& b);

	/// 认得的声母/首辅音表（协议 5.1.8，**长度降序**）。给诊断与测试用。
	/// 中文表 = `zh ch sh b p m f d t n l g k h j q x r z c s`
	/// （`y` / `w` 是零声母规范的记号，不是辅音实体 —— CONSONANT-DICT.md §3.6）。
	const std::vector<std::string>& Initials(Lang lang);

	/// 统计（打印字典规模用）。
	struct Stats {
		int zhVowel = 0, jaVowel = 0, enVowel = 0;
		int consonant = 0;
	};
	Stats GetStats();

}
