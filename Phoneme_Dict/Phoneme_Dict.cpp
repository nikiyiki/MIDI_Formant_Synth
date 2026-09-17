#include "Phoneme_Dict.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ─────────────────────────────────────────────────────────────
//  Phoneme_Dict 数据表
//
//  ⚠️ 数据来源与可信度（详见 docs/CONSONANT-DICT.md §0）：
//    · 元音：**逐条照抄协议** 5.1 / 5.2 / 5.3（可信度 P）
//    · 辅音：照 docs/CONSONANT-DICT.md §3/§4/§5 的起点值（多为 T = 待真机实测）
//            —— 时长为典型值，需按曲速换算；听感校准后再定稿。
//
//  ⚠️ 时长一律 ms。**绝不硬编码 tick** —— 那要按当前 tempo / PPQN 换算。
//
//  ⚠️ 音节切分（协议 5.1.8 / 5.1.9 / 5.2 / 5.3）在本文件末尾。
//     在此之前 `FindVowel` 是整串精确匹配，"ka" / "shi" / "tsu" 这类真实音节
//     永远查不到字典（实测 63 个带声母罗马音命中 0 个，全部落中性兜底）。
// ─────────────────────────────────────────────────────────────

namespace PHON {

	Lang ParseLang(const std::string& s)
	{
		if (s == "zh" || s == "ZH" || s == "cn") return Lang::Zh;
		if (s == "ja" || s == "JA" || s == "jp") return Lang::Ja;
		if (s == "en" || s == "EN")              return Lang::En;
		return Lang::Unknown;
	}

	const char* LangName(Lang l)
	{
		switch (l) {
		case Lang::Zh: return "zh";
		case Lang::Ja: return "ja";
		case Lang::En: return "en";
		default:       return "?";
		}
	}

	const char* MannerName(Manner m)
	{
		switch (m) {
		case Manner::Stop:        return "塞音";
		case Manner::Fricative:   return "擦音";
		case Manner::Affricate:   return "塞擦音";
		case Manner::Nasal:       return "鼻音";
		case Manner::Approximant: return "近音";
		default:                  return "?";
		}
	}

	// ── 音色：PB52 的 type 分组均值 → 共振峰缩放 ────────────────
	//
	//  数据源：docs/sources/pb52_means_by_type.txt
	//          （由 docs/sources/pb52_arrays.csv 的 1520 条 = 76 人 × 10 元音 × 2 重复算出）
	//
	//  分组：男 33 人 / 女 41 人 / 童 2 人。
	//  ⚠️ 童声只有 2 位发音人（PB52 里儿童实际有 15 人，但女声与童声的 f0 重叠
	//     ——女 187~290 Hz、童 275~316 Hz——按 f0 自动分群只切得出 2 人）。
	//     设计定案：不折腾了，就用这 2 人的均值。
	//
	//  取 8 个**互不重复**的元音求 F1/F2/F3 均值
	//  （数据里 i≡I、U≡u 完全同值，是同一批评测，只算一次）：
	//
	//             F1       F2       F3
	//    男      521.0   1396.8   2343.1
	//    女      637.6   1734.9   2891.6     ÷男 = 1.224 / 1.242 / 1.234
	//    童      789.0   1860.5   3339.1     ÷男 = 1.514 / 1.332 / 1.425
	//
	//  男声取 1.0 是有道理的，不是偷懒：协议 5.3 的英文元音值本来就等于 PB52
	//  的**男声均值**（i 270/2290、E 530/1840、ae 660/1720… 逐条对得上），
	//  所以「字典原样不动」= 男声。
	//
	//  ⚠️ 可信度 T：这是**整体缩放**（声道长度比），不是逐元音实测。
	//     中文 38 韵母 / 日文 6 元音来自协议的**另一批数据**，也照这个比例外推
	//     —— 这一步没有任何测量支撑，真机听感校准后再定稿。
	//     F4~F8 是 F3 的外推结果，按 F3 的比例缩放（保持高阶共振峰的间距比）。

	Voice ParseVoice(const std::string& s)
	{
		if (s == "female" || s == "f" || s == "woman" || s == "女") return Voice::Female;
		if (s == "child"  || s == "c" || s == "kid"   || s == "童") return Voice::Child;
		return Voice::Male;
	}

	const char* VoiceName(Voice v)
	{
		switch (v) {
		case Voice::Female: return "female";
		case Voice::Child:  return "child";
		default:            return "male";
		}
	}

	VoiceScale ScaleOf(Voice v)
	{
		VoiceScale s;
		switch (v) {
		case Voice::Female: s.f1 = 1.224; s.f2 = 1.242; s.f3 = 1.234; break;
		case Voice::Child:  s.f1 = 1.514; s.f2 = 1.332; s.f3 = 1.425; break;
		default:            break;                       // 男声 = 基准
		}
		return s;
	}

	void ApplyVoiceTo8(int f8[8], Voice v)
	{
		const VoiceScale s = ScaleOf(v);
		if (s.f1 == 1.0 && s.f2 == 1.0 && s.f3 == 1.0)
			return;                                      // 男声：不动
		f8[0] = static_cast<int>(std::lround(f8[0] * s.f1));
		f8[1] = static_cast<int>(std::lround(f8[1] * s.f2));
		for (int k = 2; k < 8; ++k)
			f8[k] = static_cast<int>(std::lround(f8[k] * s.f3));
	}

	// ── 辅音表 ──────────────────────────────────────────────────
	//
	//  部位 → locus（F1/F2/F3），来自 CONSONANT-DICT.md §2.2：
	//    双唇   300 /  800 / 2400
	//    唇齿   400 / 1200 / 2400
	//    齿龈   300 / 1800 / 2600
	//    卷舌   300 / 1400 / 1800   ← F3 显著低，是卷舌的招牌线索
	//    舌面   300 / 2200 / 2900
	//    舌根   300 / 1200 / 2400

	namespace {

		std::vector<Consonant> BuildConsonants()
		{
			std::vector<Consonant> v;
			// ★ 先 reserve 到最终规模：add() 返回 v.back() 的引用，
			//   只要中途扩容，先前返回的引用就会失效（悬垂引用 —— 与
			//   Formant_Synth.cpp 里那个 `Vowel tmp` 的崩法同一类）。
			//   预留够空间后 push_back 永不重分配，引用全程有效。
			v.reserve(64);

			auto add = [&](const char* k, Manner m, int f1, int f2, int f3) -> Consonant& {
				Consonant c;
				c.key = k; c.manner = m; c.f1 = f1; c.f2 = f2; c.f3 = f3;
				v.push_back(c);
				return v.back();
			};

			// ── 中文塞音（6）────────────────────────────────────
			// 成阻 30ms；不送气 VOT 8~10ms，送气 70~80ms（CONSONANT-DICT §3.1）
			{
				Consonant& c = add("b", Manner::Stop, 300, 800, 2400);
				c.closureMs = 30; c.votMs = 8;  c.transitionMs = 25; c.noiseOnset = false;
			}
			{
				Consonant& c = add("p", Manner::Stop, 300, 800, 2400);
				c.closureMs = 30; c.votMs = 70; c.transitionMs = 25;
				c.aspirated = true; c.noiseOnset = true;
				c.noiseMs = 70; c.noisePeak = 800; c.noiseAmp = 1.0;
			}
			{
				Consonant& c = add("d", Manner::Stop, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 8;  c.transitionMs = 25; c.noiseOnset = false;
			}
			{
				Consonant& c = add("t", Manner::Stop, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 70; c.transitionMs = 25;
				c.aspirated = true; c.noiseOnset = true;
				c.noiseMs = 70; c.noisePeak = 1800; c.noiseAmp = 1.0;
			}
			{
				Consonant& c = add("g", Manner::Stop, 300, 1200, 2400);
				c.closureMs = 30; c.votMs = 10; c.transitionMs = 30; c.noiseOnset = false;
			}
			{
				Consonant& c = add("k", Manner::Stop, 300, 1200, 2400);
				c.closureMs = 30; c.votMs = 80; c.transitionMs = 30;
				c.aspirated = true; c.noiseOnset = true;
				c.noiseMs = 80; c.noisePeak = 1200; c.noiseAmp = 1.0;
			}

			// ── 中文擦音（5）────────────────────────────────────
			// CONSONANT-DICT §3.2：s 最尖最强，f/h 弱而低
			{
				Consonant& c = add("f", Manner::Fricative, 400, 1200, 2400);
				c.noiseMs = 90;  c.noisePeak = 3000; c.noiseAmp = 0.5;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("s", Manner::Fricative, 300, 1800, 2600);
				c.noiseMs = 100; c.noisePeak = 7000; c.noiseAmp = 1.0;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("sh", Manner::Fricative, 300, 1400, 1800);
				c.noiseMs = 100; c.noisePeak = 4000; c.noiseAmp = 0.9;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("x", Manner::Fricative, 300, 2200, 2900);
				c.noiseMs = 95;  c.noisePeak = 4500; c.noiseAmp = 0.7;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("h", Manner::Fricative, 300, 1200, 2400);
				c.noiseMs = 80;  c.noisePeak = 1500; c.noiseAmp = 0.4;
				c.transitionMs = 15; c.noiseOnset = true;
			}

			// ── 中文塞擦音（6）──────────────────────────────────
			// 结构 = 塞音的静音+burst → 紧跟摩擦段（CONSONANT-DICT §3.3）
			{
				Consonant& c = add("z", Manner::Affricate, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 60;
				c.noisePeak = 7000; c.noiseAmp = 1.0;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("c", Manner::Affricate, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 120; c.aspirated = true;
				c.noisePeak = 7000; c.noiseAmp = 1.0;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("zh", Manner::Affricate, 300, 1400, 1800);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 65;
				c.noisePeak = 4000; c.noiseAmp = 0.9;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("ch", Manner::Affricate, 300, 1400, 1800);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 125; c.aspirated = true;
				c.noisePeak = 4000; c.noiseAmp = 0.9;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("j", Manner::Affricate, 300, 2200, 2900);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 65;
				c.noisePeak = 4500; c.noiseAmp = 0.8;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("q", Manner::Affricate, 300, 2200, 2900);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 125; c.aspirated = true;
				c.noisePeak = 4500; c.noiseAmp = 0.8;
				c.transitionMs = 20; c.noiseOnset = true;
			}

			// ── 中文鼻音（2）────────────────────────────────────
			// murmur：F1 低（≈250）、带宽加大、幅度中等（CONSONANT-DICT §3.4）
			{
				Consonant& c = add("m", Manner::Nasal, 250, 800, 2400);
				c.murmurMs = 60; c.murmurAmp = 0.6; c.transitionMs = 25;
			}
			{
				Consonant& c = add("n", Manner::Nasal, 250, 1700, 2600);
				c.murmurMs = 60; c.murmurAmp = 0.6; c.transitionMs = 25;
			}

			// ── 中文近音（2）────────────────────────────────────
			{
				Consonant& c = add("l", Manner::Approximant, 350, 1200, 2600);
				c.transitionMs = 35; c.murmurAmp = 0.9;
			}
			{
				Consonant& c = add("r", Manner::Approximant, 350, 1300, 1600);
				c.transitionMs = 40; c.murmurAmp = 0.8;   // 卷舌 r：F3 低
			}

			// ── 英文塞音（清浊先只用 VOT 区分，见 CONSONANT-DICT §5）──
			{
				Consonant& c = add("en_b", Manner::Stop, 300, 800, 2400);
				c.closureMs = 30; c.votMs = 5; c.transitionMs = 25;
			}
			{
				Consonant& c = add("en_d", Manner::Stop, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 5; c.transitionMs = 25;
			}
			{
				Consonant& c = add("en_g", Manner::Stop, 300, 1200, 2400);
				c.closureMs = 30; c.votMs = 10; c.transitionMs = 30;
			}
			{
				Consonant& c = add("en_t", Manner::Stop, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 60; c.aspirated = true;
				c.noiseMs = 60; c.noisePeak = 1800; c.noiseAmp = 1.0;
				c.transitionMs = 25; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_k", Manner::Stop, 300, 1200, 2400);
				c.closureMs = 30; c.votMs = 70; c.aspirated = true;
				c.noiseMs = 70; c.noisePeak = 1200; c.noiseAmp = 1.0;
				c.transitionMs = 30; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_p", Manner::Stop, 300, 800, 2400);
				c.closureMs = 30; c.votMs = 60; c.aspirated = true;
				c.noiseMs = 60; c.noisePeak = 800; c.noiseAmp = 1.0;
				c.transitionMs = 25; c.noiseOnset = true;
			}

			// ── 英文擦音（θ/ð 带低、弱）─────────────────────────
			{
				Consonant& c = add("en_s", Manner::Fricative, 300, 1800, 2600);
				c.noiseMs = 100; c.noisePeak = 7000; c.noiseAmp = 1.0;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_z", Manner::Fricative, 300, 1800, 2600);
				c.noiseMs = 80; c.noisePeak = 7000; c.noiseAmp = 0.8;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_sh", Manner::Fricative, 300, 1400, 1800);
				c.noiseMs = 100; c.noisePeak = 4000; c.noiseAmp = 0.9;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_f", Manner::Fricative, 400, 1200, 2400);
				c.noiseMs = 90; c.noisePeak = 3000; c.noiseAmp = 0.5;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_th", Manner::Fricative, 400, 1300, 2400);
				c.noiseMs = 90; c.noisePeak = 4000; c.noiseAmp = 0.4;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("en_h", Manner::Fricative, 300, 1200, 2400);
				c.noiseMs = 70; c.noisePeak = 1500; c.noiseAmp = 0.4;
				c.transitionMs = 15; c.noiseOnset = true;
			}

			// ── ★ 英文补齐（CONSONANT-DICT §5 清单里缺的 7 条）──────
			//  §5 列的是：p b t d k g / f v θ ð s z ʃ ʒ h / tʃ dʒ / m n ŋ / l r w j
			//  原先只有 17 条，缺 v ð ʒ tʃ dʒ w j。
			{
				// v：唇齿浊擦音。与 f 同部位，但浊音摩擦弱、带更低
				Consonant& c = add("en_v", Manner::Fricative, 400, 1200, 2400);
				c.noiseMs = 70; c.noisePeak = 6000; c.noiseAmp = 0.6;   // T
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				// ð（th 的浊音，如 this）：比 θ 更弱更低
				Consonant& c = add("en_dh", Manner::Fricative, 400, 1300, 2400);
				c.noiseMs = 70; c.noisePeak = 3500; c.noiseAmp = 0.35;  // T
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				// ʒ（如 vision）：与 ʃ 同部位（卷舌/龈后），F3 低
				Consonant& c = add("en_zh", Manner::Fricative, 300, 1400, 1800);
				c.noiseMs = 90; c.noisePeak = 3500; c.noiseAmp = 0.7;   // T
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				// tʃ（ch，如 church）：结构 = 塞 + 擦；英文 ch 是送气的
				Consonant& c = add("en_ch", Manner::Affricate, 300, 1400, 1800);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 110;         // T
				c.noisePeak = 4000; c.noiseAmp = 0.9;
				c.aspirated = true; c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				// dʒ（j，如 judge）：与 tʃ 同部位，浊 → 摩擦段更短更弱
				Consonant& c = add("en_j", Manner::Affricate, 300, 1400, 1800);
				c.closureMs = 30; c.votMs = 5; c.noiseMs = 70;          // T
				c.noisePeak = 4000; c.noiseAmp = 0.7;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				// w：唇软腭近音（半元音）。F2 很低（双唇）+ F3 略降
				Consonant& c = add("en_w", Manner::Approximant, 300, 600, 2200);
				c.transitionMs = 40; c.murmurAmp = 0.8;                 // T
			}
			{
				// j（yes）：硬腭近音，与元音 /i/ 的起点同族
				Consonant& c = add("en_y", Manner::Approximant, 300, 2200, 2900);
				c.transitionMs = 35; c.murmurAmp = 0.8;                 // T
			}

			// ── 英文鼻音 / 近音 ─────────────────────────────────
			{
				Consonant& c = add("en_m", Manner::Nasal, 250, 800, 2400);
				c.murmurMs = 60; c.murmurAmp = 0.6; c.transitionMs = 25;
			}
			{
				Consonant& c = add("en_n", Manner::Nasal, 250, 1700, 2600);
				c.murmurMs = 60; c.murmurAmp = 0.6; c.transitionMs = 25;
			}
			{
				Consonant& c = add("en_ng", Manner::Nasal, 250, 1200, 2400);
				c.murmurMs = 70; c.murmurAmp = 0.6; c.transitionMs = 30;
			}
			{
				Consonant& c = add("en_l", Manner::Approximant, 350, 1200, 2600);
				c.transitionMs = 35; c.murmurAmp = 0.9;
			}
			{
				Consonant& c = add("en_r", Manner::Approximant, 350, 1300, 1600);
				c.transitionMs = 40; c.murmurAmp = 0.8;
			}

			// ── 日文辅音（协议 5.2 只有元音级，这里追加常用行）────
			//  CONSONANT-DICT §4：か行 k/g、さ行 s/z、た行 t/d、な行 n、
			//  は行 h/b/p、ま行 m、ら行 r；や行/わ行归元音，不建条目。
			{
				Consonant& c = add("ja_k", Manner::Stop, 300, 1200, 2400);
				c.closureMs = 30; c.votMs = 25; c.transitionMs = 30; c.noiseOnset = true;
				c.noiseMs = 25; c.noisePeak = 1200; c.noiseAmp = 0.6;
			}
			{
				Consonant& c = add("ja_t", Manner::Stop, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 25; c.transitionMs = 25; c.noiseOnset = true;
				c.noiseMs = 25; c.noisePeak = 1800; c.noiseAmp = 0.6;
			}
			{
				Consonant& c = add("ja_s", Manner::Fricative, 300, 1800, 2600);
				c.noiseMs = 90; c.noisePeak = 7000; c.noiseAmp = 0.9;
				c.transitionMs = 20; c.noiseOnset = true;
			}
			{
				Consonant& c = add("ja_h", Manner::Fricative, 300, 1200, 2400);
				c.noiseMs = 70; c.noisePeak = 1500; c.noiseAmp = 0.4;
				c.transitionMs = 15; c.noiseOnset = true;
			}
			{
				Consonant& c = add("ja_m", Manner::Nasal, 250, 800, 2400);
				c.murmurMs = 60; c.murmurAmp = 0.6; c.transitionMs = 25;
			}
			{
				Consonant& c = add("ja_n", Manner::Nasal, 250, 1700, 2600);
				c.murmurMs = 60; c.murmurAmp = 0.6; c.transitionMs = 25;
			}
			{
				Consonant& c = add("ja_r", Manner::Approximant, 350, 1700, 2600);
				c.transitionMs = 25; c.murmurAmp = 0.8;   // 日文 r 是闪音，近音近似
			}

			// ── ★ 日文补齐（CONSONANT-DICT §4 各行里缺的 5 条）──────
			//  日文清浊对立同样**先用 VOT 区分**（浊音 VOT 短），与英文同款处理。
			{
				Consonant& c = add("ja_g", Manner::Stop, 300, 1200, 2400);
				c.closureMs = 30; c.votMs = 15; c.transitionMs = 30; c.noiseOnset = true;
				c.noiseMs = 20; c.noisePeak = 1200; c.noiseAmp = 0.5;    // T
			}
			{
				Consonant& c = add("ja_d", Manner::Stop, 300, 1800, 2600);
				c.closureMs = 30; c.votMs = 15; c.transitionMs = 25; c.noiseOnset = true;
				c.noiseMs = 20; c.noisePeak = 1800; c.noiseAmp = 0.5;    // T
			}
			{
				Consonant& c = add("ja_b", Manner::Stop, 300, 800, 2400);
				c.closureMs = 30; c.votMs = 15; c.transitionMs = 25; c.noiseOnset = true;
				c.noiseMs = 20; c.noisePeak = 800; c.noiseAmp = 0.5;     // T
			}
			{
				Consonant& c = add("ja_p", Manner::Stop, 300, 800, 2400);
				c.closureMs = 30; c.votMs = 25; c.transitionMs = 25; c.noiseOnset = true;
				c.noiseMs = 25; c.noisePeak = 800; c.noiseAmp = 0.6;     // T
			}
			{
				// z（ざ行浊擦音）：与 s 同部位，浊 → 更短更弱
				Consonant& c = add("ja_z", Manner::Fricative, 300, 1800, 2600);
				c.noiseMs = 70; c.noisePeak = 6000; c.noiseAmp = 0.6;    // T
				c.transitionMs = 20; c.noiseOnset = true;
			}

			// ── burst 的**统一定义**（2026-09-18 新增）──────────────
			//
			//  规则：**只要有成阻，就有爆破** —— 与 `Formant_Audio.cpp` 的
			//  `RenderCVA()` 第 2 步完全一致（那里是 5 ms 带通噪声 × 0.8）。
			//  依据：CONSONANT-DICT §3.6 —— b/d/g 不在协议的"噪声声母"表里，
			//  但**仍要有 burst**，否则塞音听不出"堵住再爆开"。
			//  ⚠️ 改这里必须同时改 A 组渲染器，否则 A/B 对比失去意义。
			{
				const int    kBurstMs  = 5;
				const double kBurstAmp = 0.8;
				for (Consonant& cc : v)
					if (cc.closureMs > 0) { cc.burstMs = kBurstMs; cc.burstAmp = kBurstAmp; }
			}
			return v;
		}

		// ── 元音表：逐条照抄协议 5.1 / 5.2 / 5.3 ──────────────

		std::vector<Vowel> BuildVowels()
		{
			std::vector<Vowel> v;
			auto add = [&](const char* k, Lang lg, std::vector<VowelPoint> pts) {
				Vowel vw; vw.key = k; vw.lang = lg; vw.points = std::move(pts);
				v.push_back(std::move(vw));
			};
			auto P = [](int a, int b, int c) { VowelPoint p; p.f1 = a; p.f2 = b; p.f3 = c; return p; };

			// 5.1.1 单韵母（7）
			add("a",  Lang::Zh, { P(700, 1200, 2600) });
			add("o",  Lang::Zh, { P(500,  900, 2400) });
			add("e",  Lang::Zh, { P(500, 1400, 2500) });
			add("i",  Lang::Zh, { P(300, 2300, 3000) });
			add("u",  Lang::Zh, { P(350,  800, 2400) });
			add("v",  Lang::Zh, { P(300, 1800, 2600) });
			add("er", Lang::Zh, { P(500, 1400, 1800) });

			// 5.1.2 前响复韵母（4）
			add("ai", Lang::Zh, { P(650, 1500, 2400), P(400, 2100, 2600) });
			add("ei", Lang::Zh, { P(500, 1400, 2500), P(350, 2000, 2800) });
			add("ao", Lang::Zh, { P(700, 1200, 2400), P(450,  850, 2400) });
			add("ou", Lang::Zh, { P(500,  900, 2400), P(350,  800, 2400) });

			// 5.1.3 后响复韵母（5）
			add("ia", Lang::Zh, { P(300, 2300, 3000), P(700, 1200, 2600) });
			add("ie", Lang::Zh, { P(300, 2300, 3000), P(500, 1400, 2500) });
			add("ua", Lang::Zh, { P(350,  800, 2400), P(700, 1200, 2600) });
			add("uo", Lang::Zh, { P(350,  800, 2400), P(500,  900, 2400) });
			add("ve", Lang::Zh, { P(300, 1800, 2600), P(500, 1400, 2500) });

			// 5.1.4 中响复韵母（4）
			add("iao", Lang::Zh, { P(300, 2300, 3000), P(700, 1200, 2600), P(450, 850, 2400) });
			add("iu",  Lang::Zh, { P(300, 2300, 3000), P(500,  900, 2400), P(350, 800, 2400) });
			add("uai", Lang::Zh, { P(350,  800, 2400), P(650, 1500, 2400), P(400, 2100, 2600) });
			add("ui",  Lang::Zh, { P(350,  800, 2400), P(500, 1400, 2500), P(350, 2000, 2800) });

			// 5.1.5 前鼻韵母（8）
			add("an",  Lang::Zh, { P(650, 1400, 2400), P(500, 1200, 2400) });
			add("en",  Lang::Zh, { P(500, 1400, 2500), P(500, 1200, 2400) });
			add("in",  Lang::Zh, { P(300, 2300, 3000), P(500, 1500, 2500) });
			add("un",  Lang::Zh, { P(350,  800, 2400), P(500, 1200, 2400) });
			add("vn",  Lang::Zh, { P(300, 1800, 2600), P(500, 1200, 2400) });
			add("ian", Lang::Zh, { P(300, 2300, 3000), P(700, 1200, 2600), P(500, 1200, 2400) });
			add("uan", Lang::Zh, { P(350,  800, 2400), P(700, 1200, 2600), P(500, 1200, 2400) });
			add("van", Lang::Zh, { P(300, 1800, 2600), P(700, 1200, 2600), P(500, 1200, 2400) });

			// 5.1.6 后鼻韵母（8）
			add("ang",  Lang::Zh, { P(700, 1200, 2600), P(600, 1000, 2400) });
			add("eng",  Lang::Zh, { P(500, 1400, 2500), P(500, 1200, 2400) });
			add("ing",  Lang::Zh, { P(300, 2300, 3000), P(500, 1500, 2500) });
			add("ong",  Lang::Zh, { P(500,  900, 2400), P(600, 1000, 2400) });
			add("iang", Lang::Zh, { P(300, 2300, 3000), P(700, 1200, 2600), P(600, 1000, 2400) });
			add("uang", Lang::Zh, { P(350,  800, 2400), P(700, 1200, 2600), P(600, 1000, 2400) });
			add("ueng", Lang::Zh, { P(350,  800, 2400), P(500, 1400, 2500), P(500, 1200, 2400) });
			add("iong", Lang::Zh, { P(300, 2300, 3000), P(500,  900, 2400), P(600, 1000, 2400) });

			// 5.1.7 舌尖元音（2）
			add("i_apical_back",  Lang::Zh, { P(350, 1300, 2200) });   // zhi chi shi ri
			add("i_apical_front", Lang::Zh, { P(350, 1800, 2600) });   // zi ci si

			// 5.2 日文（6）
			add("ja_a", Lang::Ja, { P(700, 1200, 2600) });
			add("ja_i", Lang::Ja, { P(300, 2300, 3000) });
			add("ja_u", Lang::Ja, { P(350,  800, 2400) });
			add("ja_e", Lang::Ja, { P(500, 1800, 2500) });
			add("ja_o", Lang::Ja, { P(500,  900, 2400) });
			add("ja_n", Lang::Ja, { P(300, 1200, 2400) });

			// 5.3 英文（12）
			add("en_a",  Lang::En, { P(730, 1090, 2440) });
			add("en_e",  Lang::En, { P(530, 1840, 2480) });
			add("en_i",  Lang::En, { P(270, 2290, 3010) });
			add("en_o",  Lang::En, { P(570,  840, 2410) });
			add("en_u",  Lang::En, { P(300,  870, 2240) });
			add("en_ee", Lang::En, { P(270, 2290, 3010) });
			add("en_oo", Lang::En, { P(300,  870, 2240) });
			add("en_ai", Lang::En, { P(730, 1090, 2440), P(350, 2000, 2600) });
			add("en_ei", Lang::En, { P(530, 1840, 2480), P(350, 2200, 2800) });
			add("en_ou", Lang::En, { P(570,  840, 2410), P(350,  800, 2400) });
			add("en_au", Lang::En, { P(730, 1090, 2440), P(350,  800, 2400) });
			add("en_oi", Lang::En, { P(570,  840, 2410), P(350, 2200, 2800) });

			return v;
		}
	}

	const std::vector<Consonant>& AllConsonants()
	{
		static const std::vector<Consonant> kTable = BuildConsonants();
		return kTable;
	}

	const std::vector<Vowel>& AllVowels()
	{
		static const std::vector<Vowel> kTable = BuildVowels();
		return kTable;
	}

	const Consonant* FindConsonant(const std::string& key)
	{
		for (const Consonant& c : AllConsonants())
			if (c.key == key)
				return &c;
		return nullptr;
	}

	namespace {

		/// 某语言的元音 key 前缀（中文是裸 key，日/英带前缀）。
		std::string LangPrefix(Lang lang)
		{
			switch (lang) {
			case Lang::En: return "en_";
			case Lang::Ja: return "ja_";
			default:       return "";
			}
		}

		/// **只认这一种语言**的元音查询：① 精确 → ② 加本语言前缀。不做跨语言回退。
		const Vowel* FindVowelStrict(const std::string& key, Lang lang)
		{
			for (const Vowel& v : AllVowels())
				if (v.key == key && v.lang == lang)
					return &v;
			const std::string pk = LangPrefix(lang) + key;
			if (pk != key) {
				for (const Vowel& v : AllVowels())
					if (v.key == pk && v.lang == lang)
						return &v;
			}
			return nullptr;
		}

	} // namespace

	const Vowel* FindVowel(const std::string& key, Lang lang)
	{
		// ① (key, lang) 精确命中
		// ② 该语言的带前缀写法
		//    ★ 这一步是修 bug 加的：英文表用 "en_a" 这样的 key，而调用方传裸 "a"，
		//      以前没有 ② 就直接掉进 ③ 的「不限语言」，命中的是**中文** a
		//      ——实测 FindVowel("e", En) 返回 e (zh)，F2 差 440 Hz。
		if (const Vowel* v = FindVowelStrict(key, lang))
			return v;

		// ③ 不限语言的第一条同名条目（历史行为：罗马音场景下中英日写法常混用）
		for (const Vowel& v : AllVowels())
			if (v.key == key)
				return &v;
		return nullptr;
	}

	VowelPoint Neutral()
	{
		VowelPoint p;
		p.f1 = 500; p.f2 = 1500; p.f3 = 2500;   // 协议 6.5 的兜底值
		return p;
	}

	void ExtendTo8(const VowelPoint& p, int out8[8])
	{
		// 协议 6.5 的 extend_formants：
		//   F4 = F3 + 400，F5 = F4 + 800，F6 = F5 + 500，F7 = F6 + 500，F8 = F7 + 500
		out8[0] = p.f1;
		out8[1] = p.f2;
		out8[2] = p.f3;
		out8[3] = p.f3 + 400;
		out8[4] = out8[3] + 800;
		out8[5] = out8[4] + 500;
		out8[6] = out8[5] + 500;
		out8[7] = out8[6] + 500;
	}

	// ═══════════════════════════════════════════════════════════
	//  ★ 音节切分（协议 5.1.8 / 5.1.9 / 5.2 / 5.3）
	// ═══════════════════════════════════════════════════════════

	VowelPoint Syllable::Point() const
	{
		if (vowel && !vowel->points.empty())
			return vowel->points.front();
		return Neutral();
	}

	namespace {

		/// 归一化：只留 a-z 小写。
		/// 标点 / 空格 / 数字 / 非 ASCII 字节（含中文 UTF-8）一律剔除
		/// —— 剔除后为空就落中性，这正是「中文韵母暂停」阶段的期望行为。
		std::string Normalize(const std::string& s)
		{
			std::string o;
			o.reserve(s.size());
			for (char ch : s) {
				const unsigned char u = static_cast<unsigned char>(ch);
				if (u >= 'A' && u <= 'Z')      o.push_back(static_cast<char>(u - 'A' + 'a'));
				else if (u >= 'a' && u <= 'z') o.push_back(static_cast<char>(u));
			}
			return o;
		}

		// ── 中文声母表（协议 5.1.8，长度降序）────────────────────
		//
		//  协议表原文：zh ch sh b p m f d t n l g k h j q x r z c s y w
		//  ⚠️ `y` / `w` 不在本表里 —— 它们是**零声母规范的记号**，不是辅音实体
		//     （CONSONANT-DICT.md §3.6；`wu→u`、`yi→i` …）。所以剥离交给第 2 步。
		const char* const kZhInitials[] = {
			"zh", "ch", "sh",
			"b", "p", "m", "f", "d", "t", "n", "l",
			"g", "k", "h", "j", "q", "x", "r", "z", "c", "s",
		};

		// ── 零声母规范（协议 5.1.8 原文 20 条）──────────────────
		struct Rewrite { const char* from; const char* to; };
		const Rewrite kZhZeroInitial[] = {
			{ "wu", "u" }, { "yi", "i" }, { "yu", "v" },
			{ "ya", "ia" }, { "ye", "ie" }, { "yao", "iao" }, { "you", "iu" },
			{ "yan", "ian" }, { "yin", "in" }, { "ying", "ing" }, { "yong", "iong" },
			{ "wa", "ua" }, { "wo", "uo" }, { "wai", "uai" }, { "wei", "ui" },
			{ "wan", "uan" }, { "wen", "un" }, { "wang", "uang" }, { "weng", "ueng" },
		};

		// ── 舌尖元音特判（协议 5.1.9 第 1 步）────────────────────
		struct Apical { const char* syl; const char* ini; const char* vow; };
		const Apical kZhApical[] = {
			{ "zhi", "zh", "i_apical_back"  },
			{ "chi", "ch", "i_apical_back"  },
			{ "shi", "sh", "i_apical_back"  },
			{ "ri",  "r",  "i_apical_back"  },
			{ "zi",  "z",  "i_apical_front" },
			{ "ci",  "c",  "i_apical_front" },
			{ "si",  "s",  "i_apical_front" },
		};

		/// 剥声母。命中返回辅音条目（可能是 nullptr = 没有声母），剩余串写进 rest。
		const Consonant* StripInitial(const std::string& s,
		                              const char* const* list, std::size_t n,
		                              Lang lang, std::string& rest)
		{
			for (std::size_t i = 0; i < n; ++i) {
				const std::size_t k = std::strlen(list[i]);
				if (s.size() <= k)                 continue;   // 剥完没韵母了，不算
				if (s.compare(0, k, list[i]) != 0) continue;
				rest = s.substr(k);
				std::string full = list[i];
				if (lang == Lang::En)      full = "en_" + full;
				else if (lang == Lang::Ja) full = "ja_" + full;
				return FindConsonant(full);                    // 可能为 nullptr（如 w / y）
			}
			rest = s;
			return nullptr;
		}

		/// 后缀最长匹配（协议 5.1.9 第 6 步）。
		const Vowel* LongestSuffix(const std::string& s, Lang lang)
		{
			for (std::size_t len = s.size(); len >= 1; --len)
				if (const Vowel* v = FindVowelStrict(s.substr(s.size() - len), lang))
					return v;
			return nullptr;
		}

		/// 韵腹优先级回退（协议 5.1.9 第 7 步）：a > o > e > i > u > v。
		/// 注意是**按优先级**取，不是取最靠前的那个字符。
		const Vowel* NucleusFallback(const std::string& s, Lang lang)
		{
			for (char c : { 'a', 'o', 'e', 'i', 'u', 'v' }) {
				if (s.find(c) == std::string::npos) continue;
				std::string k(1, c);
				if (const Vowel* v = FindVowelStrict(k, lang))
					return v;
			}
			return nullptr;
		}

		/// ── 中文（协议 5.1.9 的 8 步识别顺序）─────────────────
		Syllable ResolveZh(const std::string& s)
		{
			Syllable r;
			r.matchedLang = Lang::Zh;

			// 1. 舌尖元音特判（不特判的话这 2 条永远查不到 —— key 里有下划线）
			for (const Apical& a : kZhApical) {
				if (s == a.syl) {
					r.consonant = FindConsonant(a.ini);
					r.vowel     = FindVowelStrict(a.vow, Lang::Zh);
					r.rest      = a.vow;
					return r;
				}
			}

			std::string w = s;

			// 2. 零声母规范（整音节改写）
			bool rewritten = false;
			for (const Rewrite& z : kZhZeroInitial) {
				if (w == z.from) { w = z.to; rewritten = true; break; }
			}
			if (!rewritten) {
				// 协议表没覆盖的 ü 系列（yue / yuan / yun）与少量 y/w 开头：
				// 用最保守的前缀改写补上。**这是扩展，不是协议原文** —— 标 T。
				if (w.size() >= 2 && w[0] == 'y' && w[1] == 'u')      w = "v" + w.substr(2);   // yue→ve, yuan→van, yun→vn
				else if (!w.empty() && w[0] == 'y')                   w = "i" + w.substr(1);
				else if (!w.empty() && w[0] == 'w')                   w = "u" + w.substr(1);
			}

			// 3. j/q/x + u → v（协议 5.1.8：ju→jv, jue→jve, juan→jvan, jun→jvn …）
			if (w.size() >= 2 && (w[0] == 'j' || w[0] == 'q' || w[0] == 'x') && w[1] == 'u')
				w[1] = 'v';

			// 4. 剥声母
			std::string rest;
			r.consonant = StripInitial(w, kZhInitials,
			                           sizeof(kZhInitials) / sizeof(kZhInitials[0]),
			                           Lang::Zh, rest);

			// 5. 精确查表
			if (const Vowel* v = FindVowelStrict(rest, Lang::Zh)) {
				r.vowel = v; r.rest = rest; return r;
			}
			// 6. 后缀最长匹配
			if (const Vowel* v = LongestSuffix(rest, Lang::Zh)) {
				r.vowel = v; r.rest = rest; return r;
			}
			// 7. 韵腹优先级回退
			if (const Vowel* v = NucleusFallback(rest, Lang::Zh)) {
				r.vowel = v; r.rest = rest; return r;
			}
			// 8. None
			r.rest = rest;
			r.neutral = true;
			return r;
		}

		// ── 日文（五十音罗马音）──────────────────────────────────
		//
		//  音变映射（CONSONANT-DICT.md §4）：
		//    sh → ja_s   （し [ɕ]，归さ行）
		//    ch → ja_t   （ち [tɕ]，归た行：「后接 i → [tʃ]」）
		//    ts → ja_t   （つ [ts]：「后接 u → [ts]」）
		//    f  → ja_h   （ふ [ɸ]，归は行）
		//    j  → ja_z   （じ [dʑ]，归ざ行）
		//  や行 / わ行 是半元音，归元音级 → **不建条目**，声母记 nullptr。
		struct JaOnset { const char* romaji; const char* entry; };
		const JaOnset kJaOnsets[] = {
			// 拗音（2 字母）优先，避免 "ky" 被 "k" 抢走
			{ "ky", "ja_k" }, { "gy", "ja_g" }, { "ny", "ja_n" }, { "hy", "ja_h" },
			{ "by", "ja_b" }, { "py", "ja_p" }, { "my", "ja_m" }, { "ry", "ja_r" },
			{ "sh", "ja_s" }, { "ch", "ja_t" }, { "ts", "ja_t" }, { "jy", "ja_z" },
			{ "k",  "ja_k" }, { "g",  "ja_g" }, { "s",  "ja_s" }, { "z",  "ja_z" },
			{ "t",  "ja_t" }, { "d",  "ja_d" }, { "n",  "ja_n" }, { "h",  "ja_h" },
			{ "b",  "ja_b" }, { "p",  "ja_p" }, { "m",  "ja_m" }, { "r",  "ja_r" },
			{ "f",  "ja_h" }, { "j",  "ja_z" },
			// 半元音：不建条目，剥掉但不产生辅音
			{ "y",  "" },     { "w",  "" },
		};

		Syllable ResolveJa(const std::string& s)
		{
			Syllable r;
			r.matchedLang = Lang::Ja;

			// ん / ン（拨音）：整音节就是 ja_n，没有声母
			if (s == "n" || s == "nn") {
				r.vowel = FindVowelStrict("ja_n", Lang::Ja);
				r.rest  = "n";
				if (!r.vowel) r.neutral = true;
				return r;
			}

			// 剥「行」
			std::string rest = s;
			for (const JaOnset& o : kJaOnsets) {
				const std::size_t k = std::strlen(o.romaji);
				if (s.size() <= k)                 continue;
				if (s.compare(0, k, o.romaji) != 0) continue;
				rest = s.substr(k);
				r.consonant = (o.entry[0] != '\0') ? FindConsonant(o.entry) : nullptr;
				break;
			}

			// 元音部分
			if (const Vowel* v = FindVowelStrict(rest, Lang::Ja)) {
				r.vowel = v; r.rest = rest; return r;
			}
			if (const Vowel* v = LongestSuffix(rest, Lang::Ja)) {
				r.vowel = v; r.rest = rest; return r;
			}
			if (const Vowel* v = NucleusFallback(rest, Lang::Ja)) {
				r.vowel = v; r.rest = rest; return r;
			}
			r.rest = rest;
			r.neutral = true;
			return r;
		}

		// ── 英文（协议 5.3）──────────────────────────────────────
		//
		//  协议 5.3 的「字母组合别名（最长匹配）」原文：
		//    eau ieu ai ay ea ee ei eu ey ie oa oe oi
		//    oo ou oy ui au aw ow aa ii uu
		//  协议只给了**别名串清单**，没给别名 → key 的映射；下表是补的映射，
		//  凡有歧义的（ow / ie / ui）都标 T，等真机听感定稿。
		struct EnAlias { const char* alias; const char* key; };
		const EnAlias kEnAliases[] = {
			// 3 字母先试
			{ "eau", "en_oo" }, { "ieu", "en_oo" },
			// 2 字母
			{ "ai", "en_ai" }, { "ay", "en_ai" },
			{ "ea", "en_ee" }, { "ee", "en_ee" },
			{ "ei", "en_ei" }, { "ey", "en_ei" },
			{ "eu", "en_oo" },                                  // T
			{ "ie", "en_ai" },                                  // T：pie / tie → /aɪ/
			{ "oa", "en_ou" }, { "oe", "en_ou" },               // T：boat → /oʊ/
			{ "oi", "en_oi" }, { "oy", "en_oi" },
			{ "oo", "en_oo" },
			{ "ou", "en_ou" },
			{ "ui", "en_oo" },                                  // T：fruit → /uː/
			{ "au", "en_au" }, { "aw", "en_au" },
			{ "ow", "en_ou" },                                  // T：slow /oʊ/ 与 cow /aʊ/ 歧义，取前者
			{ "aa", "en_a" },  { "ii", "en_i" }, { "uu", "en_u" },
			// 单元音
			{ "a", "en_a" }, { "e", "en_e" }, { "i", "en_i" },
			{ "o", "en_o" }, { "u", "en_u" },
		};

		const char* const kEnInitials[] = {
			"sh", "ch", "th", "dh", "zh", "ng",
			"p", "b", "t", "d", "k", "g", "f", "v", "s", "z", "h",
			"m", "n", "l", "r", "w", "y", "j",
		};

		Syllable ResolveEn(const std::string& s)
		{
			Syllable r;
			r.matchedLang = Lang::En;

			// 先整串精确（用户可能直接写 "en_a" 这种 key）
			if (const Vowel* v = FindVowelStrict(s, Lang::En)) {
				r.vowel = v; r.rest = s; return r;
			}

			// 剥首辅音
			std::string rest;
			r.consonant = StripInitial(s, kEnInitials,
			                           sizeof(kEnInitials) / sizeof(kEnInitials[0]),
			                           Lang::En, rest);

			// 别名最长匹配（表已按长度降序，首个「是 rest 后缀」的即最长）
			for (const EnAlias& a : kEnAliases) {
				const std::size_t k = std::strlen(a.alias);
				if (rest.size() < k) continue;
				if (rest.compare(rest.size() - k, k, a.alias) != 0) continue;
				if (const Vowel* v = FindVowelStrict(a.key, Lang::En)) {
					r.vowel = v; r.rest = rest; return r;
				}
			}
			if (const Vowel* v = NucleusFallback(rest, Lang::En)) {
				r.vowel = v; r.rest = rest; return r;
			}
			r.rest = rest;
			r.neutral = true;
			return r;
		}

	} // namespace

	Syllable ResolveIn(const std::string& syllable, Lang lang)
	{
		const std::string s = Normalize(syllable);
		if (s.empty()) {
			Syllable r;
			r.matchedLang = lang;
			r.neutral = true;
			return r;
		}
		switch (lang) {
		case Lang::Zh: return ResolveZh(s);
		case Lang::Ja: return ResolveJa(s);
		case Lang::En: return ResolveEn(s);
		default: {
			Syllable r;
			r.neutral = true;
			r.rest = s;
			return r;
		}
		}
	}

	Syllable Resolve(const std::string& syllable, Lang lang)
	{
		// 先用请求的语言
		if (lang != Lang::Unknown) {
			Syllable r = ResolveIn(syllable, lang);
			if (r.vowel)
				return r;
		}

		// 再依次试其它语言。为什么需要这一步：歌词池是**五十音罗马音**
		// （`ka` `shi` `tsu` …），而 `--lang` 默认 `en`（OUTPUT-PARAMS §2.1）。
		// 严格只认一种语言的话，"tsu" 在 en 表下必然失败。
		// 命中哪张表记录在 matchedLang / fellBack，调用方可自行裁决。
		static const Lang kOrder[] = { Lang::Zh, Lang::Ja, Lang::En };
		for (Lang l : kOrder) {
			if (l == lang) continue;
			Syllable r = ResolveIn(syllable, l);
			if (r.vowel) {
				r.fellBack = true;
				return r;
			}
		}

		Syllable r;
		r.neutral     = true;
		r.matchedLang = lang;
		r.rest        = Normalize(syllable);
		return r;
	}

	double Similarity(const std::string& a, const std::string& b)
	{
		// difflib 风格：ratio = 2·M / (len(a) + len(b))，M 取最长公共子序列长度。
		const std::size_t n = a.size(), m = b.size();
		if (n == 0 && m == 0) return 1.0;
		if (n == 0 || m == 0) return 0.0;
		std::vector<int> prev(m + 1, 0), cur(m + 1, 0);
		for (std::size_t i = 1; i <= n; ++i) {
			for (std::size_t j = 1; j <= m; ++j) {
				cur[j] = (a[i - 1] == b[j - 1])
				       ? prev[j - 1] + 1
				       : std::max(prev[j], cur[j - 1]);
			}
			std::swap(prev, cur);
			std::fill(cur.begin(), cur.end(), 0);
		}
		return 2.0 * prev[m] / static_cast<double>(n + m);
	}

	const Vowel* NearestVowel(const std::string& syllable, Lang lang)
	{
		const std::string s = Normalize(syllable);
		if (s.empty()) return nullptr;

		// 协议 5.4 第 1 步：`_vowel_hint` 从字符串抽取近似共振峰。
		// 本实现就用 5.1.9 的解析结果当 hint；拿不到就用中性点。
		const Syllable base = ResolveIn(s, lang);
		const VowelPoint hint = base.vowel ? base.vowel->points.front() : Neutral();

		double best = 1e18;
		const Vowel* bestV = nullptr;
		for (const Vowel& v : AllVowels()) {
			if (v.lang != lang || v.points.empty()) continue;
			// 协议 5.5：舌尖元音不参与最近邻候选
			if (v.key == "i_apical_back" || v.key == "i_apical_front") continue;

			const VowelPoint& p = v.points.front();
			const double d1 = p.f1 - hint.f1, d2 = p.f2 - hint.f2, d3 = p.f3 - hint.f3;
			const double fd = std::sqrt(d1 * d1 + d2 * d2 + d3 * d3) / 3000.0;
			const double sd = 1.0 - Similarity(s, v.key);
			const double score = 0.55 * fd + 0.45 * sd;
			if (score < best) { best = score; bestV = &v; }
		}
		// 协议 5.4 第 4 步：score > 0.85 → 放弃
		return (bestV && best <= 0.85) ? bestV : nullptr;
	}

	const std::vector<std::string>& Initials(Lang lang)
	{
		static const std::vector<std::string> kZh = [] {
			std::vector<std::string> o;
			for (const char* k : kZhInitials) o.push_back(k);
			return o;
		}();
		static const std::vector<std::string> kJa = [] {
			std::vector<std::string> o;
			for (const JaOnset& x : kJaOnsets)
				if (x.entry[0] != '\0') o.push_back(x.romaji);   // 半元音不算
			return o;
		}();
		static const std::vector<std::string> kEn = [] {
			std::vector<std::string> o;
			for (const char* k : kEnInitials) o.push_back(k);
			return o;
		}();

		switch (lang) {
		case Lang::Zh: return kZh;
		case Lang::Ja: return kJa;
		case Lang::En: return kEn;
		default:       return kZh;
		}
	}

	Stats GetStats()
	{
		Stats s;
		s.consonant = static_cast<int>(AllConsonants().size());
		for (const Vowel& v : AllVowels()) {
			switch (v.lang) {
			case Lang::Zh: ++s.zhVowel; break;
			case Lang::Ja: ++s.jaVowel; break;
			case Lang::En: ++s.enVowel; break;
			default: break;
			}
		}
		return s;
	}

}
