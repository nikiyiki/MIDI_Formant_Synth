#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────
//  Text_Decoder —— 字节 → Unicode 的转换助手（本地化）
//
//  职责：把歌词元事件里的**原始字节**按指定编码解成 UTF-8 文本。
//
//  为什么单独做一层（跟 File_Open 同级）：
//    · 它是纯函数、零依赖 —— 可以单独测
//    · 「读字节」和「解释字节」是两件事，前者在 Lyric_Mapper，后者在这
//
//  协议 4.5 的原始规定：
//    encoding='auto' 时按语言尝试：zh：utf-8 → gb18030 → shift_jis
//                                   ja：utf-8 → shift_jis → gb18030
//                                   en：utf-8 → gb18030 → shift_jis
//    全部失败落 latin-1；可显式指定 --encoding
//
//  ⚠️ 实现上比协议**收紧了一处**（见 docs/OUTPUT-PARAMS.md §5.2）：
//     gb18030 覆盖面极广，任意字节序列几乎都能「成功」解码（可能解出乱码汉字），
//     所以非 UTF-8 的字节流会被误判成 gb18030，**永远轮不到 fallback**。
//     因此 auto 只做「BOM 识别 + 严格 UTF-8 校验」，其余落 latin-1；
//     gb18030 / shift_jis 由用户显式指定（显式永远覆盖 auto）。
// ─────────────────────────────────────────────────────────────

namespace TEXT {

	enum class Encoding {
		Auto,        ///< BOM + 严格 UTF-8 校验，否则 latin-1
		Utf8,
		Gb18030,
		ShiftJis,
		Latin1,
	};

	/// "--encoding" 的值 → Encoding。认不出来落 Auto。
	Encoding ParseEncoding(const std::string& s);
	const char* EncodingName(Encoding e);

	/// 字节 → UTF-8。失败（或空输入）返回空串。
	///
	/// lang 只影响 **Auto** 的尝试顺序；显式指定时忽略。
	std::string Decode(const std::string& bytes, Encoding enc, const std::string& lang);

	/// 严格 UTF-8 校验：整串能完整解码、且不含 U+FFFD 才算通过。
	/// auto 判定靠它 —— 这就是为什么「不让 gb18030 参加 auto」。
	bool IsStrictUtf8(const std::string& bytes);

	/// Windows 代码页 → 字节转 UTF-8（内部用 MultiByteToWideChar/WideCharToMultiByte）。
	/// cp: 936=GBK/GB18030、932=Shift-JIS、65001=UTF-8、28591=Latin-1
	std::string FromCodePage(const std::string& bytes, unsigned cp);

}
