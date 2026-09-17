// Format_Synth.cpp: 命令行入口（图形界面入口在 GUI.cpp）。
//
// 用法：smf_detect_cli.exe <file.mid>

#include "Format_Synth.h"
#include "PPQN_Reader/PPQN_Reader.h"
#include "PPQN_Reader/File_Open.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "BPM_Reader/BPM_Reader.h"
#include "System_Reader/System_Reader.h"
#include "Pitch_Mapper/Pitch_Mapper.h"
#include "Expression_Mapper/Expression_Mapper.h"
#include "Volume_Mapper/Volume_Mapper.h"
#include "Note_Mapper/Note_Mapper.h"
#include "Note_Binder/Note_Binder.h"
#include <iostream>
#include <string>

namespace {
	/// 取命令行第 1 个参数（UTF-8）。
	/// 注意：不能用 main 的 argv —— Windows 上它按 ANSI 代码页编码，
	///       中文路径会变成乱码。必须从 GetCommandLineW 取宽字符后自己转 UTF-8。
	std::string FirstCommandLineArg()
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv)
			return std::string();

		std::string result;
		if (argc > 1 && argv[1] && argv[1][0] != L'\0') {
			const int n = ::WideCharToMultiByte(CP_UTF8, 0, argv[1], -1,
			                                    nullptr, 0, nullptr, nullptr);
			if (n > 0) {
				result.assign(static_cast<std::size_t>(n), '\0');
				::WideCharToMultiByte(CP_UTF8, 0, argv[1], -1,
				                      result.data(), n, nullptr, nullptr);
				if (!result.empty() && result.back() == '\0')
					result.pop_back();
			}
		}
		LocalFree(argv);
		return result;
	}
}

int main(int argc, char**)
{
	// 控制台按 UTF-8 解释我们输出的字节
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	SetConsoleTitleW(L"SMF 检测器（命令行）");

	const std::string path = FirstCommandLineArg();
	if (path.empty())
	{
		std::cout << "用法：smf_detect_cli.exe <file.mid>" << std::endl;
		return 1;
	}

	std::cout << "SMF 检测器 v" << FS::kVersion << std::endl;
	std::cout << "文件：" << path << std::endl;
	std::cout << "----------------------------------------" << std::endl;

	// 1. PPQN
	const PPQN::Result pr = PPQN::PR(path);
	std::cout << pr.message << std::endl << std::endl;

	// 2. Time Signature
	const TSR::Result tsr = TSR::TSR(path);
	std::cout << tsr.message << std::endl;

	// 3. BPM
	const BR::Result br = BR::BR(path);
	std::cout << br.message << std::endl;

	// 4. System
	const SR::Result sr = SR::SR(path);
	std::cout << sr.message << std::endl;

	// 5. Pitch（通道 1 的弯音曲线）
	const PITCH::Result pm = PITCH::PM(path, sr.system);
	std::cout << pm.message << std::endl;
	std::cout << "    R 时间线：";
	for (const PITCH::RangePoint& rp : pm.curve.ranges)
		std::cout << "(" << rp.quarters << "qn=" << rp.semitones << "st) ";
	std::cout << std::endl;
	for (std::size_t i = 0; i < pm.curve.points.size() && i < 12; ++i)
		std::cout << "    qn=" << pm.curve.points[i].quarters
		          << "  cents=" << pm.curve.points[i].cents << std::endl;
	if (pm.curve.points.size() > 12)
		std::cout << "    ...（共 " << pm.curve.points.size() << " 点）" << std::endl;

	// 5b. Expression（通道 1 的 CC11 表情时间线）
	// 5c. Volume（通道 1 的 CC7 通道音量时间线）
	//     两者都只读自己那一条曲线；把它们和 velocity 乘成"模拟响度"是下一步的事。
	const EXP::Result er = EXP::EM(path);
	std::cout << er.message << std::endl;
	std::cout << "    表情时间线：";
	for (const EXP::Point& p : er.curve.points)
		std::cout << "(" << p.quarters << "qn=" << p.value << ") ";
	std::cout << std::endl;

	const VOL::Result vr = VOL::VM(path);
	std::cout << vr.message << std::endl;
	std::cout << "    音量时间线：";
	for (const VOL::Point& p : vr.curve.points)
		std::cout << "(" << p.quarters << "qn=" << p.value << ") ";
	std::cout << std::endl;

	// 6. Note（通道 1 的音符时间线，已做重叠收缩）
	const NOTE::Result nm = NOTE::NM(path);
	std::cout << nm.message << std::endl;
	for (std::size_t i = 0; i < nm.notes.size() && i < 8; ++i)
		std::cout << "    qn=" << nm.notes[i].quarters
		          << "  时长=" << nm.notes[i].duration
		          << "  音高=" << nm.notes[i].number
		          << "  力度=" << nm.notes[i].velocity << std::endl;
	if (nm.notes.size() > 8)
		std::cout << "    ...（共 " << nm.notes.size() << " 个音符）" << std::endl;

	// 7. Bind（音符 + 绝对音高 + 力度 + 歌词，由 Note_Binder 合体）
	const LYRIC::Result lm = LYRIC::LM(path);
	std::cout << lm.message << std::endl;

	const std::vector<BIND::BoundNote> bound = BIND::NB(nm.notes, pm.curve, lm.timeline);
	int multi = 0;
	for (const BIND::BoundNote& b : bound)
		if (b.pitch.size() > 1)
			++multi;
	std::cout << "Bind：已给 " << bound.size() << " 个音符绑定绝对音高与歌词"
	          << "（其中 " << multi << " 个音符内部弯音变过）" << std::endl;
	for (std::size_t i = 0; i < bound.size() && i < 8; ++i) {
		std::cout << "    qn=" << bound[i].quarters
		          << "  音高=" << bound[i].number
		          << "  力度=" << bound[i].velocity
		          << "  歌词=[" << bound[i].lyric << "]"
		          << "  绝对音高轨迹:";
		for (const BIND::PitchPoint& p : bound[i].pitch)
			std::cout << " (" << p.offset << "qn, " << p.cents << "音分)";
		std::cout << std::endl;
	}

	return 0;
}
