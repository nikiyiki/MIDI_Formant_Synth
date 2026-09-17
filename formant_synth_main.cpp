// formant_synth.cpp: formant_synth 的命令行入口
//                  （图形界面在 Synth_Gui.cpp）
//
// 用法：formant_synth.exe <输入.mid> -o <输出.mid> [选项]
//
// 定位（协议 1.2）：把输入 SMF 经共振峰合成「挤」成 17 轨输出。
// 与 smf_detect 的分工：smf_detect 只看不写；formant_synth 只写不看。
//
// 参数（协议 9.2 + 设计决定，详见 docs\OUTPUT-PARAMS.md）：
//     <输入>                  必填
//     -o, --output <路径>     必填
//     --format gs|xg|gm      默认 gs（设计决定）
//     --channel N            输入通道，默认 1
//     --voice male|female|child  音色，默认 male（PB52 男女童均值缩放共振峰）
//     --cc11-full-scale N    CC11 满响度基准，默认 127
//     --velocity-scale F     velocity 全局缩放，默认 1.0
//     --cc7-scale F          CC7 全局缩放，默认 1.0
//     --bpm F                覆盖为恒定 BPM
//     --key SF MI            调号覆盖
//     -h, --help             显示帮助

#include "Formant_Synth/Synth.h"
#include "PPQN_Reader/File_Open.h"   // FS::Utf8ToWide
#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

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

	/// grep 风格的取值：找到 key 就返回它后面那个参数，否则返回空。
	/// 刻意做得简单 —— 本轮不引第三方参数解析库。
	bool NextArg(int i, int argc, char** argv, std::string& out)
	{
		if (i + 1 >= argc)
			return false;
		out = argv[i + 1];
		return true;
	}

	void PrintUsage()
	{
		std::cout
			<< "用法：formant_synth.exe <输入.mid> -o <输出.mid> [选项]\n"
			<< "\n"
			<< "  把输入 SMF 经共振峰合成挤成 17 轨输出（协议第 7 章）。\n"
			<< "\n"
			<< "选项：\n"
			<< "  -o, --output <路径>     输出 MIDI（必填）\n"
			<< "  --format gs|xg|gm       输出格式，默认 gs\n"
			<< "  --channel N             输入通道 1-16，默认 1\n"
			<< "  --voice male|female|child  音色，默认 male\n"
			<< "  --cc11-full-scale N     CC11 满响度基准，默认 127\n"
			<< "  --velocity-scale F      velocity 全局缩放，默认 1.0\n"
			<< "  --cc7-scale F           CC7 全局缩放，默认 1.0\n"
			<< "  --bpm F                 覆盖为恒定 BPM\n"
			<< "  --key SF MI             调号覆盖（sf 和 mi）\n"
			<< "  -h, --help              显示本帮助\n";
	}
}

int main(int argc, char** argv)
{
	// 控制台按 UTF-8 解释我们输出的字节
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
	SetConsoleTitleW(L"formant_synth（共振峰合成）");

	SYNTH::Options opt;

	// 位置参数：第 1 个非选项参数当作输入，由 main 的 argv 不安全 →
	// 先按需从 FirstCommandLineArg 取，再让选项覆盖。
	const std::string firstArg = FirstCommandLineArg();
	if (firstArg.empty() || firstArg == "-h" || firstArg == "--help") {
		PrintUsage();
		return firstArg.empty() ? 1 : 0;
	}
	opt.input = firstArg;

	// 解析选项（从 argv[2] 起 —— argv[1] 已被当作输入路径）
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		std::string v;

		if (a == "-o" || a == "--output") {
			if (!NextArg(i, argc, argv, v)) { std::cout << "错误：-o 后面缺路径。\n"; return 1; }
			opt.output = v; ++i;
		} else if (a == "--format") {
			if (!NextArg(i, argc, argv, v)) { std::cout << "错误：--format 后面缺值。\n"; return 1; }
			opt.format = SYNTH::ParseFormat(v); ++i;
		} else if (a == "--channel") {
			if (!NextArg(i, argc, argv, v)) return 1;
			opt.channel = std::atoi(v.c_str()); ++i;
		} else if (a == "--voice") {
			if (!NextArg(i, argc, argv, v)) { std::cout << "错误：--voice 后面缺值。\n"; return 1; }
			opt.voice = PHON::ParseVoice(v); ++i;
		} else if (a == "--cc11-full-scale") {
			if (!NextArg(i, argc, argv, v)) return 1;
			opt.cc11FullScale = std::atoi(v.c_str()); ++i;
		} else if (a == "--velocity-scale") {
			if (!NextArg(i, argc, argv, v)) return 1;
			opt.velocityScale = std::atof(v.c_str()); ++i;
		} else if (a == "--cc7-scale") {
			if (!NextArg(i, argc, argv, v)) return 1;
			opt.cc7Scale = std::atof(v.c_str()); ++i;
		} else if (a == "--noise-pc") {
			if (!NextArg(i, argc, argv, v)) { std::cout << "错误：--noise-pc 后面缺值。\n"; return 1; }
			opt.noisePc = std::atoi(v.c_str()); ++i;
		} else if (a == "--bpm") {
			if (!NextArg(i, argc, argv, v)) return 1;
			opt.bpm = std::atof(v.c_str()); opt.bpmGiven = true; ++i;
		} else if (a == "--key") {
			std::string v2;
			if (!NextArg(i, argc, argv, v) || !NextArg(i + 1, argc, argv, v2)) {
				std::cout << "错误：--key 需要两个值（sf 和 mi）。\n"; return 1;
			}
			opt.keySf = std::atoi(v.c_str());
			opt.keyMi = std::atoi(v2.c_str());
			opt.keyGiven = true; i += 2;
		} else if (a == "-h" || a == "--help") {
			PrintUsage();
			return 0;
		} else {
			std::cout << "警告：忽略无法识别的参数 " << a << "\n";
		}
	}

	if (opt.output.empty()) {
		std::cout << "错误：必须用 -o 指定输出路径。\n\n";
		PrintUsage();
		return 1;
	}

	// 前置检查：输入打不开就别白跑一趟（SYNTH::Convert 里还会再查一次）
	{
		std::ifstream probe = FS::OpenFile(opt.input);
		if (!probe) {
			std::cout << "打不开输入捏，文件可能被占用或已被删除。\n";
			return 1;
		}
	}

	std::cout << "formant_synth v" << SYNTH::kVersion << " —— 共振峰合成（-o *.mid = 17 轨 SMF / -o *.wav = 直接音频）\n";
	std::cout << "输入：" << opt.input << "\n";
	std::cout << "输出：" << opt.output << "\n";
	std::cout << "格式：" << SYNTH::FormatName(opt.format)
	          << "    输入通道：" << opt.channel
	          << "    音色：" << PHON::VoiceName(opt.voice)
	          << "    CC11 基准：" << opt.cc11FullScale << "\n";
	std::cout << "----------------------------------------\n";

	// ★ 按输出扩展名分流：.wav → 直接合成音频（段表 → 样点），其余 → 17 轨 MIDI
	std::string ext;
	{
		const std::string& o = opt.output;
		const std::size_t dot = o.find_last_of('.');
		if (dot != std::string::npos) ext = o.substr(dot);
		for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	const SYNTH::Result r = (ext == ".wav") ? SYNTH::ConvertToWav(opt) : SYNTH::Convert(opt);

	std::cout << r.message << "\n";
	if (!r.ok) {
		std::cout << "转换失败。\n";
		return 1;
	}

	std::cout << "已生成：" << opt.output << "\n";
	if (ext == ".wav") {
		std::cout << "  直接音频（不走 MIDI）"
		          << "    音符 " << r.notes
		          << "    警告 " << r.warnings << "\n";
	} else {
		std::cout << "  轨数 " << r.tracks << "（应为 17）"
		          << "    音符 " << r.notes
		          << "    警告 " << r.warnings << "\n";
	}
	return 0;
}
