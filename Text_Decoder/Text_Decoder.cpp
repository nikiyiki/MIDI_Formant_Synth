#include "Text_Decoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

namespace TEXT {

	Encoding ParseEncoding(const std::string& s)
	{
		if (s == "utf-8" || s == "utf8" || s == "UTF-8")            return Encoding::Utf8;
		if (s == "gb18030" || s == "gbk" || s == "gb2312")          return Encoding::Gb18030;
		if (s == "shift_jis" || s == "shift-jis" || s == "sjis")    return Encoding::ShiftJis;
		if (s == "latin-1" || s == "latin1" || s == "iso-8859-1")   return Encoding::Latin1;
		return Encoding::Auto;
	}

	const char* EncodingName(Encoding e)
	{
		switch (e) {
		case Encoding::Utf8:      return "utf-8";
		case Encoding::Gb18030:   return "gb18030";
		case Encoding::ShiftJis:  return "shift_jis";
		case Encoding::Latin1:    return "latin-1";
		default:                  return "auto";
		}
	}

	std::string FromCodePage(const std::string& bytes, unsigned cp)
	{
		if (bytes.empty())
			return std::string();

		// 字节 → 宽字符
		const int wn = ::MultiByteToWideChar(cp, 0, bytes.c_str(),
		                                     static_cast<int>(bytes.size()), nullptr, 0);
		if (wn <= 0)
			return std::string();
		std::vector<wchar_t> w(static_cast<std::size_t>(wn) + 1, L'\0');
		::MultiByteToWideChar(cp, 0, bytes.c_str(),
		                      static_cast<int>(bytes.size()), w.data(), wn);

		// 宽字符 → UTF-8
		const int un = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn,
		                                     nullptr, 0, nullptr, nullptr);
		if (un <= 0)
			return std::string();
		std::string out(static_cast<std::size_t>(un), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w.data(), wn,
		                      &out[0], un, nullptr, nullptr);
		return out;
	}

	bool IsStrictUtf8(const std::string& bytes)
	{
		if (bytes.empty())
			return false;

		// 用 MB_ERR_INVALID_CHARS：任何不合法序列直接失败。
		// 这样就不必自己去数 UTF-8 的续字节了 —— 系统 API 比手写可靠。
		const int wn = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		                                     bytes.c_str(),
		                                     static_cast<int>(bytes.size()), nullptr, 0);
		return wn > 0;
	}

	std::string Decode(const std::string& bytes, Encoding enc, const std::string& /*lang*/)
	{
		if (bytes.empty())
			return std::string();

		switch (enc) {
		case Encoding::Utf8:
			return FromCodePage(bytes, CP_UTF8);
		case Encoding::Gb18030:
			return FromCodePage(bytes, 936);
		case Encoding::ShiftJis:
			return FromCodePage(bytes, 932);
		case Encoding::Latin1:
			return FromCodePage(bytes, 28591);
		case Encoding::Auto:
		default:
			break;
		}

		// ── Auto ──
		// 1) BOM
		if (bytes.size() >= 3
		    && static_cast<unsigned char>(bytes[0]) == 0xEF
		    && static_cast<unsigned char>(bytes[1]) == 0xBB
		    && static_cast<unsigned char>(bytes[2]) == 0xBF)
			return FromCodePage(bytes.substr(3), CP_UTF8);

		// 2) 严格 UTF-8 校验
		if (IsStrictUtf8(bytes))
			return FromCodePage(bytes, CP_UTF8);

		// 3) 落 latin-1（查表映射，恒成功，保证不丢字节）
		//    ⚠️ gb18030 / shift_jis **不参加 auto**：
		//       它们覆盖面太广，会把任意字节流都「成功」解成乱码，
		//       于是永远轮不到这里的 fallback。要它们就显式指定。
		return FromCodePage(bytes, 28591);
	}

}
