// Synth_Gui.cpp: formant_synth 的图形界面
//
// 与 smf_detect 的分工：
//     smf_detect.exe  —— 只看不写：榨参数、画曲线
//     Synth_Gui.exe   —— 看 + 导出：预览参数并「挤」出 17 轨 SMF
//
// 本文件刻意**不复用** GUI.h 的 PitchCanvas / SmfDetectWindow —— 那两个类的
// WndProc 与 kClassName 是静态成员、跟 GUI.cpp 绑死，复用就得把 GUI.cpp 也链进来
// （重复定义 + 窗口类名撞车）。所以这里做一个**自包含的精简窗口**：
// 三条曲线（弯音变化 / 绝对音高 / 模拟响度）+ 导出控件。
//
// 用法：Synth_Gui.exe [file.mid]

#include "Formant_Synth/Synth.h"
#include "PPQN_Reader/PPQN_Reader.h"
#include "PPQN_Reader/File_Open.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "System_Reader/System_Reader.h"
#include "Note_Mapper/Note_Mapper.h"
#include "Pitch_Mapper/Pitch_Mapper.h"
#include "Expression_Mapper/Expression_Mapper.h"
#include "Volume_Mapper/Volume_Mapper.h"
#include "Lyric_Mapper/Lyric_Mapper.h"
#include "Note_Binder/Note_Binder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace {

	int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

	std::wstring ToWide(const std::string& utf8)
	{
		if (utf8.empty()) return std::wstring();
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                                    static_cast<int>(utf8.size()), nullptr, 0);
		if (n <= 0) return std::wstring();
		// 用 vector 而非 wstring::data()：后者传给 LPWSTR 形参会报 C2664
		std::vector<wchar_t> w(static_cast<std::size_t>(n) + 1, L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                      static_cast<int>(utf8.size()), w.data(), n);
		return std::wstring(w.data());
	}

	std::string ToUtf8(const std::wstring& w)
	{
		if (w.empty()) return std::string();
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
		                                    static_cast<int>(w.size()),
		                                    nullptr, 0, nullptr, nullptr);
		if (n <= 0) return std::string();
		std::string s(static_cast<std::size_t>(n), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
		                      static_cast<int>(w.size()),
		                      &s[0], n, nullptr, nullptr);
		return s;
	}

	/// 取命令行第 1 个参数（UTF-8）。argv 在 Windows 上是 ANSI，中文路径会乱码。
	std::string FirstCommandLineArg()
	{
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (!argv) return std::string();
		std::string r;
		if (argc > 1 && argv[1] && argv[1][0] != L'\0')
			r = ToUtf8(argv[1]);
		LocalFree(argv);
		return r;
	}

	// ── 控件 ID ────────────────────────────────────────────────
	enum {
		kIdOpen    = 2001,
		kIdFormat  = 2002,
		kIdCc11    = 2003,
		kIdBpm     = 2004,
		kIdConvert = 2005,
		kIdLog     = 2006,
		kIdCanvas  = 2007,
		kIdMode    = 2008,
		kIdVoice   = 2009,
	};

	const wchar_t kCanvasClass[] = L"FSynthCurveCanvas";
	const wchar_t kWindowClass[] = L"FSynthGuiWindow";
	// 标题在运行时拼（版本号来自 SYNTH::kVersion，narrow → wide 要过 ToWide）
	const wchar_t kWindowTitleBase[] = L" —— 共振峰合成";

	const double kPxPerQuarter = 25.0;   // 与 smf_detect 的 PitchCanvas 一致
	const double kPxPerCent    = 0.1;

	enum class ViewMode { Bend = 0, Absolute = 1, Loudness = 2 };

	struct Snapshot {
		PITCH::Curve  pitch;
		EXP::Curve    exp;
		VOL::Curve    vol;
		std::vector<BIND::BoundNote> notes;
		int cc11FullScale = 100;
	};

	Snapshot g_snap;
	ViewMode g_mode    = ViewMode::Bend;
	int      g_scrollX = 0;

	// ── 画布 ───────────────────────────────────────────────────
	LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch (msg) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			RECT rc; GetClientRect(hwnd, &rc);
			const int w = rc.right - rc.left;
			const int h = rc.bottom - rc.top;
			const int midY = h / 2;

			HBRUSH bg = CreateSolidBrush(RGB(252, 252, 250));
			FillRect(dc, &rc, bg);
			DeleteObject(bg);
			if (w <= 0 || h <= 0) { EndPaint(hwnd, &ps); return 0; }

			const double pxQ = kPxPerQuarter;
			const int    sx  = g_scrollX;

			// 横向网格：每四分音符淡线
			{
				const double firstQ = std::floor(static_cast<double>(sx) / pxQ);
				const double lastQ  = firstQ + static_cast<double>(w) / pxQ + 1.0;
				HPEN pen = CreatePen(PS_SOLID, 1, RGB(232, 232, 228));
				HGDIOBJ old = SelectObject(dc, pen);
				for (double q = firstQ; q <= lastQ; q += 1.0) {
					const int x = static_cast<int>(q * pxQ) - sx;
					if (x < 0 || x >= w) continue;
					MoveToEx(dc, x, 0, nullptr); LineTo(dc, x, h);
				}
				SelectObject(dc, old); DeleteObject(pen);

				// 小节线 + 小节号
				SetBkMode(dc, TRANSPARENT);
				HPEN bp = CreatePen(PS_SOLID, 1, RGB(170, 170, 165));
				old = SelectObject(dc, bp);
				for (const PITCH::BarLine& b : g_snap.pitch.bars) {
					if (b.quarters < firstQ - 1.0 || b.quarters > lastQ + 1.0) continue;
					const int x = static_cast<int>(b.quarters * pxQ) - sx;
					if (x < 0 || x >= w) continue;
					MoveToEx(dc, x, 0, nullptr); LineTo(dc, x, h);
					wchar_t num[16]; swprintf_s(num, L"%d", b.measure);
					SetTextColor(dc, RGB(120, 120, 120));
					TextOutW(dc, x + 3, 22, num, static_cast<int>(wcslen(num)));
				}
				SelectObject(dc, old); DeleteObject(bp);
			}

			// 纵向：八度线 + 中央 0 线
			{
				HPEN po = CreatePen(PS_SOLID, 1, RGB(210, 210, 205));
				HGDIOBJ old = SelectObject(dc, po);
				for (int oct = -2; oct <= 2; ++oct) {
					const int y = midY - static_cast<int>(oct * 1200 * kPxPerCent);
					if (y < 0 || y >= h) continue;
					MoveToEx(dc, 0, y, nullptr); LineTo(dc, w, y);
				}
				SelectObject(dc, old); DeleteObject(po);

				HPEN pz = CreatePen(PS_SOLID, 1, RGB(150, 150, 145));
				old = SelectObject(dc, pz);
				MoveToEx(dc, 0, midY, nullptr); LineTo(dc, w, midY);
				SelectObject(dc, old); DeleteObject(pz);
			}

			// ── 三条曲线 ──
			if (g_mode == ViewMode::Bend) {
				if (!g_snap.pitch.points.empty()) {
					HPEN pen = CreatePen(PS_SOLID, 2, RGB(20, 90, 200));
					HGDIOBJ old = SelectObject(dc, pen);
					int prevY = midY; bool first = true;
					for (const PITCH::Point& p : g_snap.pitch.points) {
						const int x = static_cast<int>(p.quarters * pxQ) - sx;
						const int y = midY - static_cast<int>(p.cents * kPxPerCent);
						if (first) { MoveToEx(dc, 0, midY, nullptr); LineTo(dc, x, midY); first = false; }
						else       { LineTo(dc, x, prevY); }
						LineTo(dc, x, y);
						prevY = y;
					}
					LineTo(dc, w, prevY);
					SelectObject(dc, old); DeleteObject(pen);
				}
			} else if (g_mode == ViewMode::Absolute) {
				SetBkMode(dc, TRANSPARENT);
				for (const BIND::BoundNote& n : g_snap.notes) {
					if (n.pitch.empty()) continue;
					const int t  = ClampInt(n.velocity, 1, 127);
					const int r  = 90 + (255 - 90) * (t - 1) / 126;
					const int gg = 35 + (170 - 35) * (t - 1) / 126;
					const int bb = 15 + (80  - 15) * (t - 1) / 126;
					HPEN pen = CreatePen(PS_SOLID, 2, RGB(r, gg, bb));
					HGDIOBJ old = SelectObject(dc, pen);

					int y = midY - static_cast<int>(n.pitch.front().cents * kPxPerCent);
					const int x0 = static_cast<int>(n.quarters * pxQ) - sx;
					MoveToEx(dc, x0, y, nullptr);
					for (std::size_t i = 1; i < n.pitch.size(); ++i) {
						const int px = static_cast<int>((n.quarters + n.pitch[i].offset) * pxQ) - sx;
						LineTo(dc, px, y);
						y = midY - static_cast<int>(n.pitch[i].cents * kPxPerCent);
						LineTo(dc, px, y);
					}
					const int x1 = static_cast<int>((n.quarters + n.duration) * pxQ) - sx;
					LineTo(dc, x1, y);
					SelectObject(dc, old); DeleteObject(pen);
				}
			} else {
				// 模拟响度：一个音符一行，音符之间不连线
				//   响度 = velocity × CC7/100 × CC11/cc11FullScale
				SetBkMode(dc, TRANSPARENT);
				HPEN pen = CreatePen(PS_SOLID, 2, RGB(200, 60, 30));
				HGDIOBJ old = SelectObject(dc, pen);
				const int scale = g_snap.cc11FullScale > 0 ? g_snap.cc11FullScale : 100;
				for (const BIND::BoundNote& n : g_snap.notes) {
					const int velo = ClampInt(n.velocity, 1, 127);
					const int cc7  = VOL::ValueAt(g_snap.vol, n.quarters);
					const int cc11 = EXP::ValueAt(g_snap.exp, n.quarters);
					const double v = static_cast<double>(velo)
					               * cc7 / 100.0 * cc11 / static_cast<double>(scale);
					const int x0 = static_cast<int>(n.quarters * pxQ) - sx;
					const int x1 = static_cast<int>((n.quarters + n.duration) * pxQ) - sx;
					const int y  = midY - static_cast<int>(v);
					if (x1 < 0 || x0 > w) continue;
					MoveToEx(dc, x0, y, nullptr);
					LineTo(dc, x1, y);
				}
				SelectObject(dc, old); DeleteObject(pen);
			}

			// 左上角提示
			{
				SetBkMode(dc, TRANSPARENT);
				SetTextColor(dc, RGB(120, 120, 120));
				const wchar_t* m =
					(g_mode == ViewMode::Bend)     ? L"模式：弯音变化"
				  : (g_mode == ViewMode::Absolute) ? L"模式：绝对音高（0 音分 = A4）"
				  :                                  L"模式：模拟响度（velo × CC7/100 × CC11/基准）";
				wchar_t info[192];
				swprintf_s(info, L"%s    25 px/四分音符    滚动 %d px    [滚轮=横向滚动]", m, sx);
				TextOutW(dc, 8, 6, info, static_cast<int>(wcslen(info)));
			}

			EndPaint(hwnd, &ps);
			return 0;
		}
		case WM_MOUSEWHEEL: {
			const int delta = GET_WHEEL_DELTA_WPARAM(wp);
			g_scrollX += (delta > 0 ? -60 : 60);
			if (g_scrollX < 0) g_scrollX = 0;
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
		default: break;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// ── 主窗口 ─────────────────────────────────────────────────

	class SynthWindow {
	public:
		void SetInitialFile(const std::string& p) { input_ = p; }
		int  Run();

	private:
		HWND hwnd_  = nullptr;
		HFONT font_ = nullptr;
		std::string input_, output_;

		static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

		void OnCreate(HWND hwnd);
		void OnSize(int w, int h);
		void OpenInput();
		void OpenOutput();
		void LoadAndPreview();
		void DoConvert();
		void SetLog(const std::wstring& text);
		std::string ReadText(int id);
	};

	std::string SynthWindow::ReadText(int id)
	{
		const HWND h = GetDlgItem(hwnd_, id);
		if (!h) return std::string();
		const int n = GetWindowTextLengthW(h);
		if (n <= 0) return std::string();
		std::vector<wchar_t> buf(static_cast<std::size_t>(n) + 1, L'\0');
		GetWindowTextW(h, buf.data(), n + 1);
		return ToUtf8(std::wstring(buf.data(), static_cast<std::size_t>(n)));
	}

	void SynthWindow::SetLog(const std::wstring& text)
	{
		SetWindowTextW(GetDlgItem(hwnd_, kIdLog), text.c_str());
	}

	void SynthWindow::OnCreate(HWND hwnd)
	{
		hwnd_ = hwnd;

		font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
		                    L"Microsoft YaHei");

		auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
		              int id, DWORD exStyle = 0) -> HWND {
			HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
			                         0, 0, 10, 10, hwnd_,
			                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
			                         GetModuleHandleW(nullptr), nullptr);
			if (font_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
			return h;
		};

		mk(L"BUTTON", L"选择输入…",      BS_PUSHBUTTON, kIdOpen);
		mk(L"BUTTON", L"",               BS_GROUPBOX,   2110);   // 分组框：输入/输出参数
		mk(L"STATIC", L"输出格式：",     SS_LEFT,       2101);
		{
			HWND cb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kIdFormat);
			SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GS"));
			SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"XG"));
			SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GM"));
			SendMessageW(cb, CB_SETCURSEL, 0, 0);      // 默认 GS（设计决定）
		}
		mk(L"STATIC", L"CC11 基准：",    SS_LEFT,       2102);
		{
			HWND ed = mk(L"EDIT", L"127", ES_LEFT | ES_NUMBER | ES_AUTOHSCROLL,
			             kIdCc11, WS_EX_CLIENTEDGE);
			(void)ed;
		}
		mk(L"STATIC", L"音色：",         SS_LEFT,       2111);
		{
			// 音色：男（默认，基准）/ 女 / 童
			//   PB52 的男女童均值缩放共振峰；童声组只有 2 位发音人（设计定案强行用）
			HWND vb = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, kIdVoice);
			SendMessageW(vb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"男声"));
			SendMessageW(vb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"女声"));
			SendMessageW(vb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"童声"));
			SendMessageW(vb, CB_SETCURSEL, 0, 0);
		}
		mk(L"STATIC", L"BPM 覆盖：",     SS_LEFT,       2103);
		mk(L"EDIT", L"", ES_LEFT | ES_NUMBER | ES_AUTOHSCROLL, kIdBpm, WS_EX_CLIENTEDGE);
		// ⚠️ 不要再加「⬇」这类符号：U+2B07 在 Microsoft YaHei 里**不存在**
		//    （GetGlyphIndicesW 返回 0xFFFF）→ 按钮上会留一块空白，看着像坏了。
		//    本窗口全部 85 个非 ASCII 字符已逐个核对过，只有它是缺的，已去掉。
		mk(L"BUTTON", L"导出 17 轨 SMF", BS_PUSHBUTTON, kIdConvert);
		mk(L"BUTTON", L"弯音",           BS_PUSHBUTTON, 2104);
		mk(L"BUTTON", L"绝对音高",       BS_PUSHBUTTON, 2105);
		mk(L"BUTTON", L"模拟响度",       BS_PUSHBUTTON, 2106);
		mk(L"LISTBOX", L"", WS_VSCROLL | WS_BORDER | LBS_NOINTEGRALHEIGHT, kIdLog);

		// 画布（自注册窗口类）
		WNDCLASSEXW wc = {};
		wc.cbSize        = sizeof(wc);
		wc.lpfnWndProc   = &CanvasProc;
		wc.hInstance     = GetModuleHandleW(nullptr);
		wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		wc.lpszClassName = kCanvasClass;
		RegisterClassExW(&wc);
		CreateWindowExW(WS_EX_CLIENTEDGE, kCanvasClass, L"",
		                WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd_,
		                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCanvas)),
		                GetModuleHandleW(nullptr), nullptr);

		SetLog(L"选一个输入 .mid，然后点「导出 17 轨 SMF」。\r\n"
		       L"默认输出 GS（含 CH10 解锁，噪声轨才发得出声）。");

		if (!input_.empty())
			LoadAndPreview();
	}

	void SynthWindow::OnSize(int w, int h)
	{
		const int m = 10;
		const int rowH = 28;                 // 带边框的输入框要 26+
		const int gap = 8;
		const int boxH = 52;                 // 分组框高度

		// ── 第 1 行：分组框「输入」─────────────────────────────
		int x = m;
		HWND grpIn = GetDlgItem(hwnd_, 2110);
		if (grpIn) MoveWindow(grpIn, m, m, (w > 2 * m) ? w - 2 * m : 0, boxH, TRUE);

		x = m + 12;
		auto place = [&](int id, int width, int y, int hh) {
			HWND c = GetDlgItem(hwnd_, id);
			if (c) MoveWindow(c, x, y, width, hh, TRUE);
			x += width + gap;
		};
		const int ctlY = m + 20;

		// ⚠️ 下面这些宽度**不是随便填的**：本窗口统一用 Microsoft YaHei **16px**
		//    （见 OnCreate 的 CreateFontW(-16, …)，tmHeight=21），比系统默认字体大得多，
		//    所以按「默认字体」配的像素值会把文字截断（曾经：标签「输出格式：」=
		//    80 px 文字塞进 66 px 控件 → 显示成「输出格」；「⬇ 导出 17 轨 SMF」=
		//    129 px 文字塞进 150 px 按钮 → 左边的 ⬇ 被裁掉）。
		//
		//    当前值 = GDI `GetTextExtentPoint32W` 实测文本宽 + 内边距
		//    （按钮 ≈ +28，标签 ≈ +6，下拉框 ≈ +36 留箭头，编辑框 ≈ +18）。实测值：
		//      选择输入… 77 | 输出格式： 80 | CC11 基准： 93 | 音色： 48 | BPM 覆盖： 89
		//      导出 17 轨 SMF 115 | 弯音 32 | 绝对音高 64 | 模拟响度 64 | "127" 27
		//    ⚠️ **改字体或改文案时，这些宽度必须同步重算**（工具：`.dsh_tmp/meas/Measure_Text.cpp`）。
		place(kIdOpen,   112, ctlY,     26);   // 按钮「选择输入…」          (77+35)
		place(2101,       92, ctlY + 3, 20);   // 标签「输出格式：」          (80+12)
		place(kIdFormat,  78, ctlY,     200);  // 下拉框 GS/XG/GM（要高一点）
		place(2102,      102, ctlY + 3, 20);   // 标签「CC11 基准：」         (93+9)
		place(kIdCc11,    56, ctlY,     26);   // 编辑框（"127" 27 px）
		place(2111,       62, ctlY + 3, 20);   // 标签「音色：」              (48+14)
		place(kIdVoice,  100, ctlY,     200);  // 下拉框 男声/女声/童声
		place(2103,       98, ctlY + 3, 20);   // 标签「BPM 覆盖：」          (89+9)
		place(kIdBpm,     64, ctlY,     26);   // 编辑框
		place(kIdConvert,146, ctlY,     26);   // 按钮「导出 17 轨 SMF」      (115+31)

		// ── 第 2 行：曲线切换（独立一行，视觉上分开）──────────
		const int row2Y = m + boxH + gap;
		x = m;
		auto place2 = [&](int id, int width) {
			HWND c = GetDlgItem(hwnd_, id);
			if (c) MoveWindow(c, x, row2Y, width, rowH, TRUE);
			x += width + gap;
		};
		place2(2104,  70);                     // 弯音        (32+38)
		place2(2105,  94);                     // 绝对音高    (64+30)
		place2(2106,  94);                     // 模拟响度    (64+30)

		// ── 日志（带边框）────────────────────────────────────
		const int logY = row2Y + rowH + gap;
		const int logH = 72;
		if (HWND log = GetDlgItem(hwnd_, kIdLog))
			MoveWindow(log, m, logY, (w > 2 * m) ? w - 2 * m : 0, logH, TRUE);

		// ── 画布：占满剩余空间 ───────────────────────────────
		const int cvY = logY + logH + gap;
		const int cvH = h - cvY - m;
		if (HWND cv = GetDlgItem(hwnd_, kIdCanvas))
			MoveWindow(cv, m, cvY, (w > 2 * m) ? w - 2 * m : 0,
			           (cvH > 0) ? cvH : 0, TRUE);
	}

	void SynthWindow::OpenInput()
	{
		wchar_t buf[MAX_PATH] = L"";
		OPENFILENAMEW ofn = {};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner   = hwnd_;
		ofn.lpstrFilter = L"MIDI 文件\0*.mid;*.midi\0所有文件\0*.*\0";
		ofn.lpstrFile   = buf;
		ofn.nMaxFile    = MAX_PATH;
		ofn.lpstrTitle  = L"选择输入的 SMF";
		ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
		if (!GetOpenFileNameW(&ofn)) return;
		input_ = ToUtf8(buf);
		LoadAndPreview();
	}

	void SynthWindow::OpenOutput()
	{
		// 本轮：输出路径沿用输入旁边，导出时若为空自动生成
	}

	/// 读一遍输入并刷新预览（各 reader 互不相识，这里只负责汇合）
	void SynthWindow::LoadAndPreview()
	{
		if (input_.empty()) return;

		const TSR::Result  tsr = TSR::TSR(input_);
		const SR::Result   sr  = SR::SR(input_);
		const NOTE::Result nm  = NOTE::NM(input_);
		const PITCH::Result pm = PITCH::PM(input_, sr.system);
		const EXP::Result  ex  = EXP::EM(input_);
		const VOL::Result  vo  = VOL::VM(input_);
		const LYRIC::Result lm = LYRIC::LM(input_);

		g_snap.pitch = pm.curve;
		g_snap.exp   = ex.curve;
		g_snap.vol   = vo.curve;
		g_snap.notes = BIND::NB(nm.notes, pm.curve, lm.timeline);

		const int fs = _wtoi(ToWide(ReadText(kIdCc11)).c_str());
		g_snap.cc11FullScale = (fs >= 1 && fs <= 127) ? fs : 127;

		std::wstring s;
		s += L"输入：" + ToWide(input_) + L"\r\n";
		s += L"系统：" + ToWide(sr.system) + L"    PPQN：" + std::to_wstring(tsr.ppqn) + L"\r\n";
		s += ToWide(nm.message) + L"\r\n";
		s += ToWide(pm.message) + L"\r\n";
		s += ToWide(ex.message) + L"\r\n";
		s += L"音符 " + std::to_wstring(g_snap.notes.size())
		   + L" 个，已可导出。";
		SetLog(s);

		if (HWND cv = GetDlgItem(hwnd_, kIdCanvas))
			InvalidateRect(cv, nullptr, FALSE);
	}

	void SynthWindow::DoConvert()
	{
		if (input_.empty()) { SetLog(L"还没选输入文件捏。"); return; }

		// 输出路径：输入同目录、同名 + _fs.mid
		if (output_.empty()) {
			std::string o = input_;
			const std::size_t dot = o.find_last_of('.');
			const std::size_t sep = o.find_last_of("\\/");
			if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
				o = o.substr(0, dot);
			output_ = o + "_fs.mid";
		}

		SYNTH::Options opt;
		opt.input  = input_;
		opt.output = output_;

		const int sel = static_cast<int>(SendDlgItemMessageW(hwnd_, kIdFormat, CB_GETCURSEL, 0, 0));
		opt.format = (sel == 1) ? SYNTH::Format::XG
		           : (sel == 2) ? SYNTH::Format::GM
		           :              SYNTH::Format::GS;

		const int vsel = static_cast<int>(SendDlgItemMessageW(hwnd_, kIdVoice, CB_GETCURSEL, 0, 0));
		opt.voice = (vsel == 1) ? PHON::Voice::Female
		          : (vsel == 2) ? PHON::Voice::Child
		          :               PHON::Voice::Male;

		const std::string cc11 = ReadText(kIdCc11);
		if (!cc11.empty()) opt.cc11FullScale = ClampInt(std::atoi(cc11.c_str()), 1, 127);

		const std::string bpm = ReadText(kIdBpm);
		if (!bpm.empty()) {
			const double b = std::atof(bpm.c_str());
			if (b > 0.0) { opt.bpm = b; opt.bpmGiven = true; }
		}

		const SYNTH::Result r = SYNTH::Convert(opt);

		std::wstring s = ToWide(r.message) + L"\r\n";
		if (r.ok) {
			s += L"已生成：" + ToWide(output_) + L"\r\n";
			s += L"轨数 " + std::to_wstring(r.tracks)
			   + L"    音符 " + std::to_wstring(r.notes)
			   + L"    警告 " + std::to_wstring(r.warnings);
		} else {
			s += L"导出失败。";
		}
		SetLog(s);
	}

	LRESULT CALLBACK SynthWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		SynthWindow* self =
			reinterpret_cast<SynthWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

		switch (msg) {
		case WM_CREATE: {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = static_cast<SynthWindow*>(cs->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			self->OnCreate(hwnd);
			return 0;
		}
		case WM_SIZE: {
			if (self) self->OnSize(LOWORD(lp), HIWORD(lp));
			return 0;
		}
		// ★ 最小窗口尺寸：第 1 行控件总占宽是 **写死的像素和**
		//   （112+92+78+102+56+62+100+98+64+146 = 1000，加 9 个 gap×8 = 72，
		//    加 2×12 边距 = 1096）。窗口再窄就会把「导出 17 轨 SMF」切掉 ——
		//   把最小客户区宽夹到 1110，用户就不可能拖出一个缺控件的窗口。
		//   ⚠️ 改了上面任何控件宽度，这个数也要跟着改（或干脆改大一点留余量）。
		case WM_GETMINMAXINFO: {
			auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
			const int kMinClientW = 1110;   // 需 ≥ 1096
			const int kMinClientH = 470;    // 控件行 + 日志 + 画布不至于压扁
			RECT r = { 0, 0, kMinClientW, kMinClientH };
			const DWORD st = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
			const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
			AdjustWindowRectEx(&r, st, FALSE, ex);
			mmi->ptMinTrackSize.x = r.right - r.left;
			mmi->ptMinTrackSize.y = r.bottom - r.top;
			return 0;
		}
		case WM_COMMAND: {
			if (!self) return 0;
			if (HIWORD(wp) == BN_CLICKED) {
				switch (LOWORD(wp)) {
				case kIdOpen:    self->OpenInput();  return 0;
				case kIdConvert: self->DoConvert();  return 0;
				case 2104: g_mode = ViewMode::Bend;     break;
				case 2105: g_mode = ViewMode::Absolute; break;
				case 2106: g_mode = ViewMode::Loudness; break;
				default: break;
				}
				if (HWND cv = GetDlgItem(hwnd, kIdCanvas))
					InvalidateRect(cv, nullptr, FALSE);
				return 0;
			}
			// CC11 基准改了 → 重画响度曲线
			if (LOWORD(wp) == kIdCc11 && HIWORD(wp) == EN_CHANGE) {
				const int fs = _wtoi(ToWide(self->ReadText(kIdCc11)).c_str());
				g_snap.cc11FullScale = (fs >= 1 && fs <= 127) ? fs : 127;
				if (HWND cv = GetDlgItem(hwnd, kIdCanvas))
					InvalidateRect(cv, nullptr, FALSE);
				return 0;
			}
			return 0;
		}
		case WM_DESTROY:
			if (self && self->font_) { DeleteObject(self->font_); self->font_ = nullptr; }
			PostQuitMessage(0);
			return 0;
		default: break;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	int SynthWindow::Run()
	{
		HINSTANCE inst = GetModuleHandleW(nullptr);

		WNDCLASSEXW wc = {};
		wc.cbSize        = sizeof(wc);
		wc.lpfnWndProc   = &SynthWindow::WndProc;
		wc.hInstance     = inst;
		wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		wc.lpszClassName = kWindowClass;
		RegisterClassExW(&wc);

		HWND hwnd = CreateWindowExW(0, kWindowClass,
		                            (L"formant_synth v" + ToWide(SYNTH::kVersion)
		                             + kWindowTitleBase).c_str(),
		                            WS_OVERLAPPEDWINDOW,
		                            CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720,
		                            nullptr, nullptr, inst, this);
		if (!hwnd) return 1;

		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);

		MSG msg = {};
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return static_cast<int>(msg.wParam);
	}
}

/// 图形界面入口（WIN32_EXECUTABLE → 子系统 WINDOWS → 入口是 WinMain）。
/// 用法：Synth_Gui.exe [file.mid]
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	SynthWindow win;
	win.SetInitialFile(FirstCommandLineArg());
	return win.Run();
}
