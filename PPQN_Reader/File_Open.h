#pragma once

// ─────────────────────────────────────────────────────────────
//  File_Open — 跨编码的文件打开助手（公共基础）
//
//  问题：std::ifstream 的构造函数只接受窄字符串（char*）。
//        Windows 上窄字符串会按「ANSI 代码页」解释，而本项目的路径
//        一律是 UTF-8（源码是 UTF-8，命令行/C 运行时给也是 UTF-8），
//        于是「G:\…\新建文件夹\曲子.mid」这类中文路径打不开。
//
//  办法：先把 UTF-8 路径转成宽字符（UTF-16），用 _wfopen 打开，
//        再把 FILE* 交给 ifstream 接管。不依赖清单文件，
//        也不依赖 Windows 的「活动代码页」设置。
//
//  用法：
//      std::ifstream f = FS::OpenFile(path);
//      if (!f) { ... 打不开 ... }
// ─────────────────────────────────────────────────────────────

#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <locale>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace FS {

	/// UTF-8 字符串 → 宽字符串（Win32 宽字符 API 用）。
	inline std::wstring Utf8ToWide(const std::string& utf8)
	{
		if (utf8.empty())
			return std::wstring();
#ifdef _WIN32
		// UNICODE 宏由 CMakeLists.txt 统一定义（add_compile_definitions(UNICODE _UNICODE)），
		// 所以这里的 MultiByteToWideChar 就是宽字符版。
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                                    static_cast<int>(utf8.size()), nullptr, 0);
		if (n <= 0)
			return std::wstring();
		std::vector<wchar_t> w(static_cast<std::size_t>(n) + 1, L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                      static_cast<int>(utf8.size()), w.data(), n);
		return std::wstring(w.data());
#else
		return std::wstring(utf8.begin(), utf8.end());
#endif
	}

	/// 按 UTF-8 路径打开一个二进制输入文件流。
	/// 打不开时返回一个**明确处于失败状态**的流，调用者用 if (!f) 就能检测到。
	/// ⚠️ 注意：默认构造的 std::ifstream 是"成功"状态（failbit 未置位），
	///    直接 return std::ifstream(); 会让 if (!f) 检测不到失败 —— 这是个坑。
	inline std::ifstream OpenFile(const std::string& pathUtf8)
	{
		std::ifstream f;

#ifdef _WIN32
		const std::wstring wide = Utf8ToWide(pathUtf8);
		FILE* fp = nullptr;
		if (!wide.empty() && _wfopen_s(&fp, wide.c_str(), L"rb") == 0 && fp != nullptr) {
			f = std::ifstream(fp);
			f.imbue(std::locale::classic());     // 数据是字节，不要做本地化转换
			return f;
		}
#else
		f.open(pathUtf8, std::ios::binary);
		if (f)
			return f;
#endif

		f.setstate(std::ios::failbit);           // 打不开：置失败位，让 !f 成立
		return f;
	}

	/// 按 UTF-8 路径打开一个二进制**输出**文件流。
	/// 跟 OpenFile 同一个理由：Windows 上窄字符串按 ANSI 解释，中文路径会失败。
	/// 写不出时返回一个明确处于失败状态的流。
	inline std::ofstream OpenFileWrite(const std::string& pathUtf8)
	{
		std::ofstream f;

#ifdef _WIN32
		const std::wstring wide = Utf8ToWide(pathUtf8);
		FILE* fp = nullptr;
		if (!wide.empty() && _wfopen_s(&fp, wide.c_str(), L"wb") == 0 && fp != nullptr) {
			f = std::ofstream(fp);
			f.imbue(std::locale::classic());
			return f;
		}
#else
		f.open(pathUtf8, std::ios::binary);
		if (f)
			return f;
#endif

		f.setstate(std::ios::failbit);
		return f;
	}

	/// 从已打开的文件流读 MThd 的 PPQN（division 在偏移 12、13，大端）。
	/// 头不足 14 字节、或 division 标明 SMPTE 计时时，返回 fallback。	/// 读完后流的位置停在 14（第一个块的开头）。
	inline int ReadPpqn(std::ifstream& f, int fallback = 480)
	{
		std::uint8_t head[14];
		f.read(reinterpret_cast<char*>(head), 14);
		if (f.gcount() != 14)
			return fallback;
		const int division =
			(static_cast<int>(head[12]) << 8) | static_cast<int>(head[13]);
		if (division & 0x8000)                   // SMPTE 计时，没有 PPQN
			return fallback;
		const int p = division & 0x7FFF;
		return p > 0 ? p : fallback;
	}

}
