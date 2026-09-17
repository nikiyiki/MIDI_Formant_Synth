# Formant_Synth 输出规格 · 参数与决策

> **本文件性质**：协议第 7 / 8 / 9 章的**实现规格与用户决策记录**。
> 协议是规格来源；本文件记录「实现时怎么落」以及「哪些地方按设计决定**覆盖了协议**」。
> 优先级：**smf_detect 现有架构 > 协议 > 本文件（用户决策可覆盖协议，但必须在此登记）> 外部文献**。
>
> 配套：`CONSONANT-DICT.md`（音素字典）、`FORMANT-SYNTH-00262.txt`（协议）。

---

## 0. 一句话

**一个音符 = 歌词 + 音高 + 响度 → 共振峰合成 → 挤进 16 轨。**

smf_detect 不是「检测器」，是**挤压层**：各 reader 把输入榨成参数化的寄存器音符（`BoundNote`），
输出侧只负责把这个容器展开成 17 轨 SMF。所以**输出模块谁都不重读输入文件**。

---

## 1. ★ 输出侧的三条总原则

| # | 原则 | 出处 |
|---|---|---|
| 1 | **输出是全新 SMF，不基于输入轨编辑** | 协议 1.2 |
| 2 | **默认输出 GS** —— 才能用 CH10 解锁 SysEx，让噪声通道（CH9）独占 MIDI 通道 10 | 设计决定 |
| 3 | **RPN 0 = 24 恒定** —— 保证 ±2400 音分（协议第 11 章 BEND_RANGE_MAX），音高/元音调制量程够 | 设计决定 |

### 1.1 覆盖协议的决策登记

| 项 | 协议原文 | 实际执行 | 原因 |
|---|---|---|---|
| 默认输出格式 | 8.3：缺省跟随输入 | **GS** | 要 CH10 解锁；GS 有真正弦波（L1/L2 说 XG/GM 音色不确定） |
| RPN 0 | 8.1：GS/XG=12、GM=2 | **恒 24** | 量程与分辨率的折中 |
| velocity 通道差异 | 7.5：16 通道用**同一** velocity_out | **乘性配比** `× CC7表值(ch)/100` | 设计决定 |
| **渐慢（7.8 整节 + RIT_STEPS）** | 有 | **整节舍弃** | 设计决定：tempo 以 `--bpm` 为准 |

> ⚠️ 渐慢被舍弃后，协议 7.4 第 6 条「渐慢曲线」与 7.8 全节**均不实现**，`RIT_STEPS` 常量作废。
> 以后若有人对着协议来找这段代码，看这一行。

---

## 2. 命令行参数表（最终）

语法：`smf_detect_cli ...` / GUI 等价。`--` 前缀为命令行选项。

### 2.1 已有参数（协议 9.2，保留）

| 参数 | 默认 | 说明 |
|---|---|---|
| `input` | 必填 | 输入 MIDI |
| `-o` / `--output` | 必填 | 输出 MIDI |
| `--lang` | **`en`** | 歌词语言 `zh / en / ja`。**默认从协议的 `zh` 改为 `en`**（先服务罗马音场景） |
| `--format` | **`gs`** | 输出格式 `gs / xg / gm`。**默认从「跟随输入」改为 `gs`** |
| `--channel` | `1` | 输入通道 1–16 |
| `--bpm` | 无 | 覆盖为恒定 BPM |
| `--encoding` | `auto` | 输入歌词编码。**可选择 + auto**（见 §5） |
| `--slide` | `0` | 滑音半音数（作用 F0/H2/H3/H4） |
| `--unknown` | `drop` | 未知音素策略 `keep / drop / nearest` |
| `--report` | 无 | 替换/作废日志 JSON |
| `--velocity-scale` | `1.0` | velocity 全局缩放 |
| `--cc7-scale` | `1.0` | CC7 全局缩放 |

### 2.2 已删除

| 参数 | 原因 |
|---|---|
| ~~`--rit-start`~~ | **渐慢整节舍弃**（设计决定，见 §1.1） |
| ~~`--rit-end`~~ | 同上 |

### 2.3 新增参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--cc11-full-scale` | `127` | **CC11 满响度基准**。响度公式 `CC11(t)/R` 的分母。协议没有这个概念 —— 作者按 CC11=127 为满就填 127，按 100 为满就填 100 |
| `--noise-cc11` | 无（用默认曲线） | **噪声通道（CH9）单独一条 CC11 曲线**，按噪声声母的强弱走。见 §4.3 |

### 2.4 明确**不做**的参数（锁死）

| 项 | 值 | 理由 |
|---|---|---|
| `RPN 0` | **24** | 恒定，不做参数 |
| `note_off velocity` | **64** | 协议常量 |
| velocity 配比方式 | **乘性** `× CC7表值/100` | 设计定 |
| CC11 广播范围 | **全部 16 轨**（噪声通道除外，见 §4.3） | 声学特征要求 |
| `--formant-scale` | **不做** | 设计决定（有 `--cc11-full-scale` 已够） |
| `--cc7-table` | **不做** | 用 `--cc7-scale` 即可 |
| 事件 priority | `note_off → RPN → ctrl → note_on` | 协议 7.5 写死 |
| SysEx 内容 | 由格式唯一决定 | 协议 8.2 |

---

## 3. Meta 继承（协议 4.3 / 7.4）

默认**全部继承**，不是可选：

```
set_tempo          全部保留，按 tick 排序去重，同 tick 保留最后一条
time_signature     全部保留，按 tick 排序去重，同 tick 保留最后一条
key_signature      全部保留（若存在）      ← 需补 FF 59 reader
smpte_offset       全部保留（若存在）      ← 需补 FF 54 reader
text/copyright     丢弃
marker/cue         丢弃
track_name         丢弃，输出重写
end_of_track       丢弃，输出重写
```

### 3.1 兜底（协议 10.2）

| 情况 | 兜底 |
|---|---|
| 无 tempo | `500000 µs` = 120 BPM @ tick 0 |
| 无拍号 | `4/4` @ tick 0 |

### 3.2 `--bpm` 的语义

`--bpm` **不是**「继承开关」，而是**抛弃继承、拉平为恒定 BPM**。
默认（不给 `--bpm`）时继承输入的整条 tempo 时间线。

> 渐慢已舍弃，所以 `--bpm` 是 tempo 的**唯一**覆盖手段，不再有互斥问题。

---

## 4. 事件与通道

### 4.1 Voice 轨 tick 0 初始化（顺序固定，且必须早于 tick 0 的 note_on）

```
track_name = 'CHn'
[按格式] 通道初始化（CC0 / CC32 / PC）
RPN 0 = 24           CC101=0, CC100=0, CC6=24, CC38=0
CC7 = 7.3 表值 × --cc7-scale
CC11 = 初始表情值
```

### 4.2 事件排序

```
按 (tick, priority)：0 note_off → 1 RPN → 2 ctrl / pitch_wheel / CC11 → 3 note_on
同一 tick：note_off → RPN → ctrl → note_on
段边界：  先 note_off，再 pitch_wheel，再 note_on
```

### 4.3 ★ 两个 CC 的分工（本轮定稿）

```
CC7   ←  每通道响度配比（混音）    ← 协议 7.3 表，每轨 tick 0 写一次，**不随后续变化**
CC11  ←  响度自动化（音符内部变化） ← BoundNote 的响度轨迹
```

**CC11 的广播范围**：

| 通道 | CC11 |
|---|---|
| CH0–CH8（F0–F8） | 同一份（来自 `BoundNote` 的响度轨迹） |
| **CH9（噪声）** | **单独一条曲线**，按噪声声母的强弱走（见 `CONSONANT-DICT.md` 的噪声幅度参数） |

**CC7 一律按 7.3 表**（噪声 = 45），**不因 CC11 的独立性而改变**。

### 4.4 CH9 噪声通道的特殊性

- GS 下用 CH10 解锁 SysEx，噪声独占 MIDI 通道 10
- 协议 7.2：`CH8 的 RPN 0 / CC7 / CC11 / bend 同样作用于噪声事件`
- 噪声脉冲**不单独发送 bend**，也不继承 F8 的 bend

### 4.5 velocity

```
velocity_out(ch) = clamp(round(源velocity × velocity_scale × CC7表值(ch) / 100), 1, 127)
```

- 同一音符的 16 个通道**各自按配比缩放**（与协议 7.5「相同 velocity_out」不同，见 §1.1）
- 段边界重触发时各段使用同一 velocity_out，不衰减
- 源 velocity = 0 已按 note_off 处理（不产生音符）
- 下限 1

---

## 5. 歌词编码（协议 4.5）

### 5.1 参数语义

`--encoding` **可选择**，另提供 `auto`：

| 值 | 行为 |
|---|---|
| `auto` | 自动判定（见 §5.2） |
| `utf-8` / `gb18030` / `shift_jis` / `latin-1` | **只用这一种** |

### 5.2 auto 的判定（★ 比协议 4.5 收紧）

协议 4.5 给的是「按语言依次尝试」，但**实现上直接照做不可靠**：

> `gb18030` 覆盖极广，任意字节序列几乎都能「成功」解码（可能解出乱码汉字），
> 所以非 UTF-8 的字节流会被误判成 gb18030，**永远轮不到 fallback**。

因此 auto 收紧为：

1. **BOM 识别**（UTF-8 / UTF-16）
2. **严格 UTF-8 校验**：整串能完整解码且**不含 U+FFFD**，则判 UTF-8
3. 否则 → **落 `latin-1`**（查表映射，恒成功，保证不丢字节）
4. `gb18030` / `shift_jis` **不参与 auto**，由使用者显式指定

> 明文指定 `--encoding` **永远覆盖 auto**。这是修乱码的可靠兜底。

### 5.3 解码结果的存放（对应 C3 决策）

```
Lyric_Mapper::Event
    text       std::string   原始字节（保留，绝不丢）
    textUtf8   std::string   ← 新增：按 §5.2 解码后的 UTF-8

BoundNote::lyric             ← 存**解码后的 UTF-8**
```

- 「note 存放器」`BoundNote` 里拿到的是**可读文本**
- 6.3 邻近匹配也用解码后的文本
- 转码失败时 `text` 原字节仍在，不会把数据弄丢
- GUI 显示解码后的 UTF-8

---

## 6. 实现层次（谁都不重读输入文件）

```
输入 SMF
   │
   ├─ PPQN / 拍号 / BPM / System ──────────────────┐
   ├─ Note_Mapper ──┐                              │
   ├─ Pitch_Mapper ─┼─▶ Note_Binder ─▶ BoundNote ─┐│
   ├─ Lyric_Mapper ─┘   （寄存器音符）            ││
   ├─ Expression_Mapper ─────────────────────────┐││
   └─ Volume_Mapper（不进输出，CC7 查表重写）    │││
                                                 ▼▼▼
                        Formant_Synth（音素字典 + 6.x 管线 + 7.6/7.7）
                                                 │
                                         逻辑事件（16 通道）
                                                 ▼
                        Track_Builder（17 轨 + tick0 初始化 + priority）
                                                 ▼
                        Smf_Writer（纯序列化 → 字节）
```

| 层 | 只干这件事 | 不认识 |
|---|---|---|
| Reader / Mapper | 从字节里榨数据 | 输出 |
| Binder | 合体 | 输出 |
| Formant_Synth | 寄存器音符 → 16 通道逻辑事件 | SMF 字节、轨道 |
| Track_Builder | 逻辑事件 → 17 条轨 | 音素、共振峰 |
| Smf_Writer | 轨 → 字节 | 以上全部 |

---

## 7. 未实现的依赖（开工前要补）

| # | 缺口 | 影响 |
|---|---|---|
| 1 | `key_signature`（`FF 59`）reader | 协议 7.4 第 7 条要求 Setup 轨写调号 |
| 2 | `smpte_offset`（`FF 54`）reader | 协议 4.3 要求继承 |
| 3 | `Text_Decoder`（`TEXT` 命名空间） | §5 的编码解码 |
| 4 | `BoundNote` 的**响度轨迹**字段 | §4.3 的 CC11 自动化来源 |
| 5 | 音素字典（元音 + 辅音） | 见 `CONSONANT-DICT.md` |

---

## 8. 参数总览（一屏速查）

```
必填   input, -o
语言   --lang en                  （默认 en，协议原为 zh）
格式   --format gs                （默认 gs，协议原为跟随输入）
输入   --channel 1
       --encoding auto            （可选具体编码，永远覆盖 auto）
       --bpm                      （覆盖为恒定；渐慢已删）
音素   --unknown drop
       --slide 0
输出   --cc11-full-scale 127      （新增）
       --noise-cc11               （新增：噪声通道单独 CC11）
       --velocity-scale 1.0
       --cc7-scale 1.0
其他   --report
锁死   RPN 0 = 24 / note_off vel = 64 / velocity 乘性配比
       CC11 广播 16 轨（噪声单独） / SysEx / 事件 priority
删除   --rit-start, --rit-end     （渐慢整节舍弃）
```
