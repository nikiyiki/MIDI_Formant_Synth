# formant_synth

**把单声部歌唱 SMF「挤压」成 17 轨 GS 输出的 MIDI → MIDI 转换器。**

读入一个带歌词的单声部歌唱 SMF（音符 + 歌词 + 弯音 + CC11），
经共振峰合成把每个音符展开成 **16 个发声通道 + 1 条 Setup 轨**，
输出一份**全新的** Type-1 SMF，交给 GS 音源（Roland SCVA / SC-8850 / SC-8820 等）播放。

```
输入 SMF
  │ 各 reader（PPQN / 拍号 / BPM / System / Pitch / Expression / Volume / Note / Lyric）
  ▼
「寄存器音符」BoundNote —— 歌词 + 音高轨迹 + 力度
  │ Formant_Synth（音素字典 + 动程管线 + 频率反算）
  ▼
段表 SegmentTable（SegKind / 频率轨迹 / 力度）   ← 一个事实源
  ├─ MIDI 后端 → 逻辑事件（16 通道）→ 17 轨 → Smf_Writer → 字节
  └─ WAV  后端 → 本机合成 → 样点
```

- `smf_detect` 系列 **只看不写** —— 提取层。
- `formant_synth` **只写不看** —— 挤压层。
- 输出**不基于输入轨编辑**（输出是全新文件）。

---

## ⚠️ 先读这一条，否则会误判这个项目

**这是「加法合成冒充共振峰合成」。**

GS 音源没有可控的共振峰滤波器。要让音源发出某个元音，唯一的办法是
**把 F1…F8 各自当成一个音弹出来**（8 个通道各发 note + 弯音），
再叠上基频、谐波（H2/H3/H4）、气声、噪声、宽带通道 —— 一共 17 轨。

所以：

| 你想的 | 实际的 |
|---|---|
| 一个声门源过 8 个并联谐振器（真·共振峰合成） | **8 个独立发声的音，频率 = F1…F8**（加法合成） |
| 那 17 轨是「合成规格」 | 那 17 轨是**给别人的合成器下的一份控制脚本** |
| 「宽带通道 F1W/F2W」是把共振峰加宽 | **是「多加一个音」** —— 实测 `/a/` 的 F1=700 与 F1W=800 是两个独立谱峰（698 / 798 Hz） |

实测证据：把 17 轨参数送进渲染器，频谱是**离散音的梳**（尖刺密布），
而不是连续共振峰包络；8 个共振峰只占谱峰的一小半。

**结论：它听起来不会像真人唱歌，像一台音源在按元音频率表发声。这是设计如此。**

---

## 设计原则

优先级（冲突时按此裁决）：

```
① 规则正确  >  ② 改起来便宜  >  ……  >  ⑨ 跑起来快
```

这是个小体积 midi2midi 转换器，**不是 MIDI 播放器**。推论：

- **不做缓存优化** —— 同一个文件被重复读 20 次也无所谓，不引入需要同步的状态。
- **分层，各自只干一件事**：

| 层 | 只干这件事 | **不认识** |
|---|---|---|
| Reader / Mapper | 从字节里榨数据 | 输出 |
| Note_Binder | 合体成寄存器音符 | 输出 |
| `Formant_Synth` | 寄存器音符 → 16 通道逻辑事件 | **SMF 的 chunk / VLQ** |
| `Smf_Writer` | 轨 → 字节（纯序列化） | 以上全部 |

- **一个事实源两个后端**：读音决策（歌词→音节→音素、动程、ms→tick、音色缩放）只发生**一次**，
  产出一份**段表**；MIDI 后端和 WAV 后端都吃它。于是「段表对不对」和「承载方式对不对」
  变成两个可以**独立验证**的问题。

---

## 构建

**主要支持平台：Windows + MSVC（已在 Windows 11 + VS 2022 + Ninja + C++20 上验证）。**

抽取层（各 reader / mapper / 字典）是纯标准 C++；
`formant_synth_main.cpp`、`Synth_Gui.cpp`、`GUI.cpp` 用了 Win32 API（UTF-8 路径、宽字符、窗口）。

```powershell
# 从已安装的 VS 开发者命令提示符里跑（或用 vcvars64.bat 配好环境）
cmake --preset x64-debug
cmake --build out\build\x64-debug
```

> ⚠️ 本工程的 `CMakePresets.json` **只声明了 `configurePresets`，没有 `buildPresets`**，
> 所以只能用 `cmake --build <目录>`，**不能用** `cmake --build --preset x64-debug`。

不想用 preset 就手写：

```powershell
cmake -S . -B out/build/x64-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/x64-debug
```

**构建三个坑**（都踩过，写在 `CMakeLists.txt` 注释里）：

1. **`/utf-8` 必须走 `set(CMAKE_CXX_FLAGS ...)`** —— 某些 CMake 版本上
   `add_compile_options("/utf-8")` 不生效（源码含中文字符串字面量）。
2. **`Formant_Synth.cpp` 与 `formant_synth.cpp` 只差大小写** —— Windows 上 CMake 按大小写不敏感去重，
   会把 CLI 的 `main` 当重复源文件**静默丢掉**，**只在链接期炸**。
   **规则：CLI main 永远叫 `formant_synth_main.cpp`，放根目录。**
3. **Windows 文件名不分大小写：绝不用只差大小写的路径做 move / delete。**

---

## 可执行目标

| 目标 | 说明 |
|---|---|
| `formant_synth` | **主程序（CLI）**：`-o *.mid` → 17 轨 SMF；`-o *.wav` → 直接合成音频 |
| `Synth_Gui` | 主程序的图形界面（自包含 Win32 窗口） |
| `smf_detect` | 输入侧检视 GUI（画弯音 / 绝对音高 / 模拟响度三条曲线） |
| `smf_detect_cli` | 输入侧检视 CLI（打印 PPQN / 拍号 / BPM / 系统 / 弯音 / 表情 / 音符 / 歌词 / 绑定） |
| `make_test_smf` | 生成主测试样本 `tests/test_v101_demo.mid`（**不引用产品代码**，独立实现） |
| `smf_note_peek` | 音符检视工具：逐轨列 note 配对 / 力度 / 时长分布（**同样不引用产品代码**） |
| `make_demo_song` | 生成演示曲（48 音节练习句，发布/演示用）；带 `--probe*` 排障模式 |

> 「不引用产品代码」是刻意的：**生成器/解析器互相独立，才算交叉验证。**

## 运行

```
formant_synth.exe <输入.mid> -o <输出.mid|输出.wav> [选项]

  -o, --output <路径>        输出（必填）；扩展名决定后端
  --format gs|xg|gm          输出格式，默认 gs
  --voice male|female|child  音色，默认 male（只改共振峰，不改旋律音高）
  --bpm F                    覆盖为恒定 BPM（抛弃输入的 tempo 时间线）
  --key SF MI                覆盖调号
  --noise-pc N               噪声通道（CH9）音色，默认 121（Breath Noise）
  --velocity-scale F         力度全局缩放，默认 1.0
  --cc7-scale F              16 通道混音配比全局缩放，默认 1.0
```

> ⚠️ `--channel N` 与 `--cc11-full-scale N` 目前**解析了但未接线**
> （通道 1 焊死在 reader 里；响度公式长在绘制层）。见 `docs/OUTPUT-PARAMS.md`。

---

## 输出规格

**17 轨** Type-1 SMF，PPQN 480：

| 轨 | 通道 | 角色 | CC7 |
|---|---|---|---|
| 0 | — | Setup（SysEx / tempo / 拍号 / 调号） | — |
| 1 | CH0 | 基频 F0 | 100 |
| 2–9 | CH1–CH8 | 共振峰 F1…F8 | 100/90/80/70/60/55/50/45 |
| 10 | CH9 | 噪声（爆破 / 摩擦 / 送气） | 45 |
| 11–13 | CH10–CH12 | 谐波 H2/H3/H4（严格 = 2/3/4 × F0） | 60/50/40 |
| 14 | CH13 | 气声（1.005 × F0） | 50 |
| 15–16 | CH14–CH15 | F1W / F2W 宽带 | 55/45 |

- **RPN 0 恒 24**（±2400 音分），覆盖协议里 GS=12 / GM=2 的默认值；
  频率→note+弯音的反算**也一律用 24**，不读输入文件的 RPN。
- 默认输出 **GS**（要 GS Reset + **CH10 解锁** SysEx —— 缺了它 CH9 噪声通道完全静音，
  而且**不报错、只是听不见**）。
- 同一 tick 事件排序：`note_off(0) → RPN(1) → ctrl/pitch_wheel/CC11(2) → note_on(3)`。
- `velocity_out(ch) = clamp(round(源力度 × velocity_scale × kCc7[ch]/100), 1, 127)`
  —— **每通道各自的配比**，让「填充物」（H2/H3/H4）自然给元音让路。

详细规格见 `docs/OUTPUT-PARAMS.md` 与 `docs/FORMANT-SYNTH-00262.txt` 第 7 章。

---

## 音素字典

`Phoneme_Dict/` 是本项目的知识库，条目数：中文 38 韵母 + 21 声母 / 日文 6 + 12 / 英文 12 + 24。

辅音条目 14 个字段（`f1/f2/f3` locus、`closureMs`、`votMs`、`noiseMs/noisePeak/noiseAmp`、
`murmurMs/murmurAmp`、`transitionMs`、`aspirated`、`noiseOnset`、`burstMs/burstAmp`），
其中 **`votMs` 与 `aspirated` 目前未接线**（送气时长实际由 `noiseMs` 承担）。

> ⚠️ 只看被接线的字段，57 条辅音的**有效可区分数**是：
> **字节层 36 种**（17 个塌陷组、涉及 38 条），**可听层只有 20 类**。
> 「字典里有 57 条」不等于「听得出 57 种」。

辅音怎么进 16 轨，见 `docs/CONSONANT-DICT.md`。

---

## 数据来源与引用

元音目标值基于 **Peterson & Barney (1952)** 的英语元音测量（业界称 PB52），
男女声按该数据集的均值缩放共振峰；童声因样本量不足（仅 2 位发音人）标注为低可信度。

辅音参数依据经典声学语音学的通行结论（locus 理论、VOT 与送气的关系等）与普通话实测的典型值，
`docs/CONSONANT-DICT.md` 用 **P / C / M / T** 四级标注每个数字的可信度
（协议原文 / 经典结论 / 实测典型值 / **待真机校准**）。

外部论文仅作为设计参考，**未收录进本仓库**。

---

## 验证方法

这个仓库里的每一处结论都尽量做到「可复现」，而不是「看起来对」：

| 手段 | 用途 |
|---|---|
| **两个后端互为对照** | 段表 → wav（本机可量）vs 段表 → 事件 → 音源（真实承载） |
| **独立实现的交叉验证** | `make_test_smf` / `smf_note_peek` **刻意不引用产品代码** —— 两套解析器对不上时，原始字节 dump 才是真相 |
| **逐字节回归** | 重构前后重出全部样本，比 **SHA256**；段表重构那次是 **25/25 逐字节相同** |
| **独立探针** | `smf_detect_cli` 报输入侧读数；`make_demo_song --probe*` 用来二分排障 |

已验收的数字（可复现）：

- 频率精度：`/i/` 八个共振峰全出，**最大偏差 2.5%**；`/a/`、`/u/` 除 F7（与 F6 并峰）外 **≤2.0%**
- 真机链（100 个随机音节）：**100/100 段切对**，元音类内散布 **0.4–2.7 dB**，
  类间 `u`vs`a` 16.7 / `u`vs`o` 13.8 / `o`vs`a` 15.1 dB ⇒ **分离度 8–9 倍**
- WAV 后端与独立渲染器对同一元音：峰位一致，最大偏差 **5.9%**

**验证纪律**（本项目最贵的三条）：

1. **先验工具，再验结论。** 工具自己也会错 —— 有一轮 7 个坑里 **6 个是工具自身的 bug**，
   全部由自检或逐字节核对抓出，**没有一个是被"看"出来的**。
2. **数字要跑出来，不要手数。** 参数表由探针实跑生成；回归用 SHA256 比。
3. **「实测过」三个字要能兑现。** 没验证就别写"实测过"。

排障手册与已知坑：`docs/DEBUGGING.md`。

---

## 目录

```
CMakeLists.txt / CMakePresets.json
formant_synth_main.cpp        CLI 入口（★ 必须在根目录，见构建坑 #2）
Synth_Gui.cpp                 主程序图形界面
GUI.cpp / GUI.h               输入侧检视界面
Format_Synth.cpp / .h         输入侧检视 CLI

PPQN_Reader/                  读 PPQN；含 File_Open.h（UTF-8 路径 → 宽字符）
Time_Signature_Reader/        拍号 + (小节, tick) 坐标换算
BPM_Reader/                   tempo 时间线
System_Reader/                GS / XG / GM Reset 与 SysEx
Pitch_Mapper/                 弯音轮（RPN 0 时间线 + 采样保持音分曲线）
Expression_Mapper/            CC11 表情时间线
Volume_Mapper/                CC7 通道音量时间线（不进输出）
Note_Mapper/                  音符 on/off 配对
Note_Binder/                  ★ 合体成「寄存器音符」BoundNote
Lyric_Mapper/                 歌词元事件（FF 05）
Phoneme_Dict/                 ★ 音素字典（元音 + 辅音）
Text_Decoder/                 字节 → UTF-8（已实现，**尚未接进 Lyric_Mapper**）

Formant_Synth/                ★ 挤压层
  Formant_Synth.cpp             主转换器（读音决策 → 段表）
  Synth.h                       对外契约
  Smf_Writer.cpp                纯序列化 → SMF 字节
  Wav_Render.cpp                段表 → 样点（midi2wav 后端）

Test_Tools/                   测试工具（不引用产品代码）
tests/                        测试输入 SMF
docs/                         协议 + 音素字典 + 输出规格 + 排障手册
```

---

## 已知限制

1. **噪声辅音不分类** —— PC 121（Breath Noise）是宽带噪声采样，靠音高移调只改亮度、不改音色，
   所以十几个带噪声的辅音目前是**同一种嘶声**。
2. **只读通道 1** —— reader 的签名不带通道参数。
3. **单声部** —— 和弦会被削成最后一个音（面向单声部歌声的设计）。
4. **歌词只认罗马音 / ASCII** —— `Text_Decoder` 写好了但没接进 `Lyric_Mapper`。
5. **成阻（塞音的"堵住"）恒为 1 tick** —— 不随曲速变化。
6. **GM 格式不完整** —— 协议规定「GM 下噪声与 F8 共用通道」，尚未实现；默认输出 GS 不受影响。
7. **弯音精度** —— 按段边界采样，音符内部很密的颤音会被阶梯化。
8. **共振峰带宽表不是字典值** —— 长在两个后端各自的常量里，改要同步。
9. **`BoundNote` 没有响度轨迹** —— 音符内部的衰减形状目前写不进输出。

---

## 文档

| 文件 | 内容 |
|---|---|
| `docs/FORMANT-SYNTH-00262.txt` | **技术协议原文**（FS-TP-2.6，13 章）—— 规格来源，逐条实现的依据 |
| `docs/CONSONANT-DICT.md` | **音素字典**：元音 + 辅音参数表与依据（P/C/M/T 可信度分级） |
| `docs/OUTPUT-PARAMS.md` | **输出规格与决策记录**（含覆盖协议的部分逐条登记） |
| `docs/DEBUGGING.md` | **排障手册**：验证纪律、静默失效清单、已定位的坑、工具速查 |

> 面向**使用者**的手册（怎么用、每个参数什么意思、出问题怎么办）
> 不在本仓库 —— 它以发布包里的 `OM.pdf` 形式分发，源码在其 HTML 源文件里维护。

---

## 许可

[MIT](LICENSE)。

---

## 版本与改动记录

| 版本 | 主要改动 |
|---|---|
| `1.0.2demo` | **元音落在原音符位置上**：辅音（成阻/爆破/噪声/过渡）整体前移一个「辅音提前量」，让元音本体从音符起点起声 —— 修掉带声母音节「节奏往后挪一个过渡段」的问题（实测原先是晚 96 tick = 100 ms @120BPM，纯韵母不受影响）。<br>并修掉界面三处：控件宽度按系统默认字体配的、而本窗口用 Microsoft YaHei 16px，导致文字被截断；导出按钮上的 `⬇`(U+2B07) 在该字体里**不存在**（空字形），换成纯文字；补 `WM_GETMINMAXINFO` 最小窗口宽度，避免拖窄后切掉控件。 |
| `1.0.1demo` | 首个「能听」的 demo：音素字典 + 16 通道共振峰 + 音色（男/女/童）+ 段表（一个事实源两个后端）+ midi2wav 后端。 |

**元音位置的验收判据**（可复现）：

```
make_demo_song cv.mid --probe-cv      # 22 个音节：纯韵母 + 全 21 个中文声母，各 500ms、间隔 500ms
formant_synth cv.mid -o cv_fs.mid     # 挤压
smf_note_peek cv_fs.mid --track 1     # 看 F0 轨：第 i 个音节的「元音本体（力度≥90）」应落在 i*960 tick
```

期望：**22 个音节偏差全部 0 tick**（容差 ±2）。