#pragma once

#define NOMINMAX          // 必须在 windows.h 之前：避免 min/max 宏污染 std::min / std::max
#include <windows.h>
#include <string>
#include <vector>

#include "Pitch_Mapper/Pitch_Mapper.h"
#include "Expression_Mapper/Expression_Mapper.h"
#include "Volume_Mapper/Volume_Mapper.h"
#include "Note_Mapper/Note_Mapper.h"
#include "Note_Binder/Note_Binder.h"

// ─────────────────────────────────────────────────────────────
//  GUI — Win32 窗口程序
//  职责：选一个 SMF 文件，显示四项探测结果，并绘制通道 1 的音高变化曲线。
//  边界：只负责显示；探测逻辑在四个 *_Reader 里，音高换算在 Pitch_Mapper 里。
// ─────────────────────────────────────────────────────────────

namespace GUI {

	/// 绘图区（音高曲线）：自己是一个子窗口，所以可以直接收键盘与滚轮。
	class PitchCanvas {
	public:
		static constexpr wchar_t kClassName[] = L"SmfPitchCanvas";

		/// 显示模式。
		enum class Mode {
			Bend,        ///< 弯音造成的音高变化（相对音符）
			Absolute,    ///< 绝对音高 = 音符音高 + 弯音变化
			Loudness,    ///< 模拟响度 = velo × CC7(t)/100 × CC11(t)/100
		};

		void Create(HWND parent, HINSTANCE inst, int id);
		HWND Handle() const { return hwnd_; }

		/// 换一份曲线数据并重画。
		void SetCurve(const PITCH::Curve& curve);

		/// 通道音量（CC7）时间线：模拟响度模式要用。
		void SetVolumeCurve(const VOL::Curve& curve);

		/// 表情（CC11）时间线：模拟响度模式要用。
		void SetExpressionCurve(const EXP::Curve& curve);

		/// 换一份"绑定后的音符"并重画（绝对音高 / 模拟响度模式要用）。
		void SetBoundNotes(const std::vector<BIND::BoundNote>& notes);

		/// 设置 CC11 的「满响度基准」（1..127）。越界会被夹到范围内。
		/// 响度公式 = velocity × CC7(t)/100 × CC11(t)/cc11FullScale —— 由用户给，
		/// 因为「这个文件把哪个 CC11 值当满响度」是文件的事，不是我们能假设的。
		/// ⚠️ 与 Pitch_Mapper 的「R」（RPN 弯音轮范围）无关，别混；详见下方成员注释。
		/// ⚠️ CC7 不受影响：它是 SMF 的固定参数（写出侧每通道一个定值）。
		void SetCc11FullScale(int v);
		int  Cc11FullScale() const { return cc11FullScale_; }

		/// 切换显示模式。
		void SetMode(Mode m);
		Mode GetMode() const { return mode_; }

		/// 横向 / 纵向缩放（factor > 1 放大）。ctrl 为真时只缩放纵向，否则只缩放横向。
		void Zoom(bool vertical, double factor);

		/// 横向滚动（单位：像素）。
		void ScrollBy(int dxPixels);

		/// 回到默认刻度（25 px/四分音符，10 音分/像素）与起点。
		void ResetView();

		/// 绘制（由 WM_PAINT 调用）。
		void Paint(HDC dc, const RECT& rc);

		static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

	private:
		HWND hwnd_ = nullptr;
		PITCH::Curve curve_;                  ///< 弯音曲线
		VOL::Curve   volCurve_;               ///< 通道音量（CC7）时间线
		EXP::Curve   expCurve_;               ///< 表情（CC11）时间线
		std::vector<BIND::BoundNote> notes_;  ///< 绑定后的音符（带绝对音高轨迹与力度）
		bool hasCurve_ = false;
		Mode mode_ = Mode::Bend;              ///< 当前显示模式

		/// 模拟响度曲线上的一个采样点。纵轴单位就是"响度"本身（0..127 量纲）。
		struct LoudPoint {
			double quarters = 0.0;   ///< 从曲首起的四分音符数
			double value    = 0.0;   ///< 该点起的模拟响度
		};

		/// 一个音符对应的一段模拟响度折线（正好是 [note_on, note_off] 区间）。
		/// 段内点按时间升序，首点 = note_on，末点 = note_off。
		struct LoudStroke {
			std::vector<LoudPoint> pts;
		};

		/// 预先算好的模拟响度曲线：**一个音符一段**。
		/// 用「每个音符一段」而不是一根扁平的点数组，是为了从结构上保证
		/// 音符与音符之间画不出线来 —— 不需要靠浮点数比较去猜音符边界
		/// （猜错的后果：要么把音符连起来，要么每个点各自成段、只画出一个点）。
		/// 它只依赖「音符 + CC7 + CC11」，跟缩放/滚动无关，所以在这里算一次，
		/// 绝不在 WM_PAINT 里现算。
		std::vector<LoudStroke> loud_;

		/// 模拟响度曲线每个音符内那段「轻微衰减」的参数。
		/// ⚠️ 这些全是**显示模型**的经验值，不是协议规格，随时可调。
		///
		/// 规则（用户定的）：
		///   · 衰减基准 = 本音符内**最后一次 CC11 事件**那个时刻已经算好的响度
		///     （音符内一次 CC11 都没有 → 基准就是 note_on 时刻的响度）
		///   · 只有当「最后一次 CC11 到 note_off」占音符 gate 的 **50% 及以上**才衰减
		///   · 衰减形状 = **先慢后快**（对数型，见 dcurve()）
		///   · 掉多少 = kDecayDropPctMax × 该占比（占比 1.0 时掉满 5%）
		///   · CC7(t) 只继承它前面最后一次的值，纯等比系数，不衰减
		static constexpr double kDecayMinGateRatio = 0.50;  ///< 衰减窗口至少要占 gate 这么多
		static constexpr double kDecayDropPctMax    = 0.05;  ///< 占比 1.0 时掉掉多少（5%）

		/// CC11 的「满响度基准」（1..127），用户在界面上填。
		///   响度(t) = velocity × CC7(t)/100 × CC11(t)/cc11FullScale
		///   取值 127 → 全满时正好 1.0（不越界）；取值 100 → 以 100 为满。
		///   注意 CC7 的分母恒为 100，**不受它影响**（CC7 是 SMF 固定参数）。
		///
		/// ⚠️⚠️ 千万别和 Pitch_Mapper 里的「R」搞混 —— 那是两个完全不同的东西：
		///
		///   · 这里 cc11FullScale_（用户口中的「R」）
		///       = 表情 CC11 的**满响度分母**，是**用户按文件口径手填**的一个数
		///         （1..127），全曲一个值，只影响「模拟响度曲线」怎么画。
		///       数学上它除的是**表情值**：
		///         CC11(t) / cc11FullScale
		///
		///   · Pitch_Mapper 的 R（注释里写作 R，代码里是 semitones / fallbackR）
		///       = RPN(0,0) 的**弯音轮范围**，单位是半音（1..24），
		///         来自**文件本身**：每个通道一条 R 时间线（GS/XG 默认 12、GM/GM2 默认 2）。
		///       数学上它除的是**弯音偏移量**：
		///         cents = R × (bend − 8192) / 8192 × 100
		///
		///   两者一个来自用户输入、一个来自文件；一个管响度、一个管音高；互不影响。
		static constexpr int kCc11FullScaleDefault = 100;   ///< 建议/默认值
		int cc11FullScale_ = kCc11FullScaleDefault;         ///< 当前 CC11 满响度基准

		// 力度 → 亮度：velocity 只有 128 个取值，预先造好 128 支画笔，画的时候直接取
		HPEN velPen_[128] = {};

		void MakePens();                      ///< 建 128 支按力度索引的画笔
		void FreePens();                      ///< 释放它们
		void ClampScroll();                   ///< 把滚动位置夹在 [0, 内容宽度 − 视口] 内
		void BuildLoudness();                 ///< 重算模拟响度曲线

		/// 「先慢后快」的衰减形状（B2）：x∈[0,1] → 累计掉的比例 ∈[0,1]。
		/// d(x) = [(1+x)·ln(1+x) − x] / (2ln2 − 1)   （分母 = ∫₀¹ln(1+u)du）
		/// d(0)=0、d(1)=1；x=0.25 掉 7.5%、x=0.5 掉 28.0%、x=0.75 掉 59.4% —— 先慢后快。
		static double dcurve(double x);

		// 视图参数
		double pxPerQuarter_ = 25.0;   ///< 横向：25 px = 1 个四分音符（即 100 px = 4 个四分音符）
		double pxPerCent_    = 0.1;    ///< 纵向：0.1 px = 1 音分（即 1 px = 10 音分）
		int    scrollX_      = 0;      ///< 横向滚动偏移（像素）
	};

	class SmfDetectWindow {
	public:
		SmfDetectWindow() = default;
		~SmfDetectWindow() = default;

		/// 指定启动时要打开的文件（命令行参数；留空则启动后手动选）。
		void SetInitialFile(const std::string& path) { initialFile_ = path; }

		/// 创建窗口并进入消息循环。返回进程退出码。
		int Run();

	private:
		static constexpr wchar_t kClassName[] = L"SmfDetectWindow";
		static constexpr wchar_t kTitle[] = L"SMF 检测器";

		static constexpr int kIdButtonOpen   = 1001;   ///< 「选择文件」
		static constexpr int kIdEditResult   = 1002;   ///< 结果文本框
		static constexpr int kIdButtonBend   = 1003;   ///< 「弯音变化」视图
		static constexpr int kIdCanvasPitch  = 1004;   ///< 绘图区
		static constexpr int kIdButtonReset  = 1005;   ///< 「重置视图」
		static constexpr int kIdButtonAbs    = 1006;   ///< 「绝对音高」视图
		static constexpr int kIdButtonLoud   = 1007;   ///< 「模拟响度曲线」视图
		static constexpr int kIdLabelExpR    = 1008;   ///< 「CC11 基准 R」标签
		static constexpr int kIdEditExpR     = 1009;   ///< R 的输入框（1..127）

		HWND hwnd_ = nullptr;
		HFONT font_ = nullptr;
		PitchCanvas canvas_;              ///< 绘图区
		std::string currentPath_;         ///< 当前打开的文件
		std::string initialFile_;         ///< 启动时自动打开的文件（可空）
		PITCH::Result pitch_;             ///< 当前弯音数据
		NOTE::Result  note_;              ///< 当前音符数据
		LYRIC::Result lyric_;             ///< 当前歌词数据
		EXP::Result   expr_;              ///< 当前表情（CC11）数据
		VOL::Result   vol_;               ///< 当前通道音量（CC7）数据
		std::vector<BIND::BoundNote> bound_;   ///< 绑定后的音符（音符 + 绝对音高 + 力度 + 歌词）

		/// R 输入框是否已经建好。建好之前不受理 EN_CHANGE —— 否则 CreateWindowExW
		/// 设置初值时触发的那次通知会把默认值 100 踩成 1。
		bool ratioUiReady_ = false;

		static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

		void OnCreate(HWND hwnd);
		void OpenFile();

		/// 读输入框里的 CC11 满响度基准，夹到 1..127，改了就重算响度曲线并重画。
		void ApplyCc11FullScaleFromEdit();

		/// 载入当前文件的弯音曲线与音符（两者互不相识，由这里汇合）。
		void UpdatePitch();

		/// 读取一个文件，返回各项探测结果拼成的可显示文本。
		static std::string Detect(const std::string& path);
	};

}
