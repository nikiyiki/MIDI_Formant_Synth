# CMakeList.txt: Format_Synth（SMF 检测器）
#
cmake_minimum_required (VERSION 3.10)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("Format_Synth")

# 让 MSVC 按 UTF-8 读写源码（源码含中文字符串字面量）
# 注意：本机 CMake 4.3.1-msvc1 上 add_compile_options("/utf-8") 不生效
#       （选项不会出现在编译器命令行），故改用 CMAKE_CXX_FLAGS，已实测有效。
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

# 走宽字符版 Win32 API（标准做法）。
# ⚠️ 不定义 UNICODE 时 MultiByteToWideChar / MultiByteToWideChar 等名字会被
#    <windows.h> 映射到 ANSI 版的 _Xxx，参数类型是 char，跟 UTF-8 代码里传的
#    wchar_t* 不匹配，报 C2664「转换丢失限定符」，而且错误里会同时列出
#    _Elem=char 与 _Elem=wchar_t 两个重载 —— 极易误判成 STL 的 const 问题。
#    定义之后 TCHAR 系列全部走 wchar_t，与工程「路径一律 UTF-8 → 宽字符」的
#    约定一致（见 PPQN_Reader/File_Open.h）。
add_compile_definitions(UNICODE _UNICODE)

# ── 探测核心：四个 *_Reader 编译成静态库，供两个可执行目标共用 ──
add_library (smf_detect_core STATIC
    "Format_Synth.h"
    "PPQN_Reader/PPQN_Reader.cpp" "PPQN_Reader/PPQN_Reader.h"
    "Time_Signature_Reader/Time_Signature_Reader.cpp" "Time_Signature_Reader/Time_Signature_Reader.h"
    "BPM_Reader/BPM_Reader.cpp" "BPM_Reader/BPM_Reader.h"
    "System_Reader/System_Reader.cpp" "System_Reader/System_Reader.h"
    "Pitch_Mapper/Pitch_Mapper.cpp" "Pitch_Mapper/Pitch_Mapper.h"
    "Expression_Mapper/Expression_Mapper.cpp" "Expression_Mapper/Expression_Mapper.h"
    "Volume_Mapper/Volume_Mapper.cpp" "Volume_Mapper/Volume_Mapper.h"
    "Note_Mapper/Note_Mapper.cpp" "Note_Mapper/Note_Mapper.h"
    "Note_Binder/Note_Binder.cpp" "Note_Binder/Note_Binder.h"
    "Lyric_Mapper/Lyric_Mapper.cpp" "Lyric_Mapper/Lyric_Mapper.h"
    "Phoneme_Dict/Phoneme_Dict.cpp" "Phoneme_Dict/Phoneme_Dict.h"
    "Text_Decoder/Text_Decoder.cpp" "Text_Decoder/Text_Decoder.h"
)

# 让各模块之间能用「目录/头文件」的形式互相包含（如 "Time_Signature_Reader/…"）
target_include_directories (smf_detect_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# ── 图形界面（Win32）──
add_executable (smf_detect "GUI.cpp" "GUI.h")
target_link_libraries (smf_detect PRIVATE smf_detect_core)
# WIN32_EXECUTABLE：不弹控制台窗口；入口仍是 main（CMake 会自动配 mainCRTStartup）
set_target_properties (smf_detect PROPERTIES WIN32_EXECUTABLE TRUE)

# ── 命令行 ──
add_executable (smf_detect_cli "Format_Synth.cpp")
target_link_libraries (smf_detect_cli PRIVATE smf_detect_core)

# ── formant_synth：把输入 SMF 挤成 17 轨输出（协议第 7 章）──
#   注意：这三个 .cpp 没有进 smf_detect_core —— 它们自带 Synth.h，
#   各 reader/mapper 的实现由 smf_detect_core 提供，链接时合并即可。
#
#   ⚠️⚠️ CLI 的 main 叫 formant_synth_main.cpp，放在**根目录**，故意不叫
#        Formant_Synth/formant_synth.cpp —— 因为那样会跟 Formant_Synth/Formant_Synth.cpp
#        **只差大小写**，而 CMake 在 Windows 上按大小写不敏感去重，会把 main
#        当成重复源文件**静默丢掉**，表现为链接期报 3 个 SYNTH:: 符号未解析。
#        这个坑极难看出来（配置成功、编译不报错，只在链接时炸）。
add_executable (formant_synth
    "formant_synth_main.cpp"
    "Formant_Synth/Formant_Synth.cpp" "Formant_Synth/Formant_Synth.h"
    "Formant_Synth/Smf_Writer.cpp" "Formant_Synth/Wav_Render.cpp"
    "Formant_Synth/Synth.h"
)

# ── Synth_Gui：formant_synth 的图形界面 ──
#   刻意不复用 GUI.h 的 PitchCanvas / SmfDetectWindow —— 那两个类的 WndProc
#   与 kClassName 是静态成员、跟 GUI.cpp 绑死；复用就得把 GUI.cpp 也链进来，
#   会重复定义 SYNTH/FS 之类的符号并导致窗口类名撞车。
#   所以 Synth_Gui.cpp 是自包含的精简窗口（三条曲线 + 导出控件）。
#   需要 comdlg32（GetOpenFileNameW）。
add_executable (Synth_Gui WIN32
    "Synth_Gui.cpp"
    "Formant_Synth/Formant_Synth.cpp" "Formant_Synth/Formant_Synth.h"
    "Formant_Synth/Smf_Writer.cpp" "Formant_Synth/Wav_Render.cpp"    "Formant_Synth/Synth.h"
)
target_include_directories (Synth_Gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries (Synth_Gui PRIVATE smf_detect_core comdlg32)
target_include_directories (formant_synth PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries (formant_synth PRIVATE smf_detect_core)

# ── make_test_smf：测试样本生成器（不属于产品，纯造样本）──
#   为什么用 C++ 而不是 PowerShell：坑 #13 / #14 —— 手写脚本抠字节出过的错
#   比被验证的代码还多。生成器自己写字节，**独立性**正是它的价值：
#   它不引用 Smf_Writer，所以「生成器写出来的东西，产品读得懂」才算交叉验证。
add_executable (make_test_smf "Test_Tools/Make_Test_Smf.cpp")

# ── smf_note_peek：解析一个 SMF，逐轨列出 note_on/note_off 配对与力度、时长分布 ──
#   调字典时的眼睛：例「小 gate 的力度 20 到底写进去没有、有没有被闭成零长度」。
#   刻意**不复用**产品代码（reader / Smf_Writer 都不用）——
#   生成器/解析器互相独立才算交叉验证（同 make_test_smf 的理由）。
#   只借 FS::OpenFile 打开 UTF-8 路径（纯头文件，不引入 reader 逻辑）。
add_executable (smf_note_peek "Test_Tools/Smf_Note_Peek.cpp")
# 只为了 #include "PPQN_Reader/File_Open.h"（UTF-8 路径打开，纯头文件）——
# **故意不链 smf_detect_core**：这个工具要跟产品代码保持独立。
target_include_directories (smf_note_peek PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# ── make_demo_song：造演示曲 / 跑节奏对齐探针（发布包与验收都用它）──
#   两种用法：
#     make_demo_song <输出.mid>              造一首 48 音节的练习句（可听）
#     make_demo_song <输出.mid> --probe-cv   造 22 个音节（纯韵母 + 全 21 个中文声母），
#                                            用来量「元音起点 vs 原音符位置」是否对齐
#   同样**不引用产品代码**（自己写 SMF 字节），写完自己重新解析核对。
#   另有 --probe / --probe-sysex / --probe-rpn / --probe-rpnlast 四个排障探针，
#   见 docs/DEBUGGING.md「Expression_Mapper 漏读 CC11」。
add_executable (make_demo_song "Test_Tools/Make_Demo_Song.cpp")
target_include_directories (make_demo_song PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET smf_detect PROPERTY CXX_STANDARD 20)
  set_property(TARGET smf_detect_cli PROPERTY CXX_STANDARD 20)
  set_property(TARGET formant_synth PROPERTY CXX_STANDARD 20)
  set_property(TARGET smf_detect_core PROPERTY CXX_STANDARD 20)
  # ⚠️ Synth_Gui 曾经漏在这里 —— 同一份 Formant_Synth.cpp 在 formant_synth 里
  #    按 C++20 编、在 Synth_Gui 里按默认（C++14）编，报 C5051（[[maybe_unused]]
  #    被忽略）。两个目标必须同标准，否则「改了这边那边不报错」只是假象。
  set_property(TARGET Synth_Gui PROPERTY CXX_STANDARD 20)
  set_property(TARGET make_test_smf PROPERTY CXX_STANDARD 20)
  set_property(TARGET smf_note_peek PROPERTY CXX_STANDARD 20)
  set_property(TARGET make_demo_song PROPERTY CXX_STANDARD 20)
endif()
