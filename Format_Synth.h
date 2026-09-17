#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────
//  Format_Synth — SMF 检测器
//  两个可执行目标共用本头文件：
//      smf_detect.exe      图形界面（GUI.cpp）
//      smf_detect_cli.exe  命令行（Format_Synth.cpp）
//  四项探测分别由 PPQN_Reader / Time_Signature_Reader /
//  BPM_Reader / System_Reader 提供。
// ─────────────────────────────────────────────────────────────

namespace FS {

	/// 本工具版本号。
	constexpr const char* kVersion = "0.1";

}
