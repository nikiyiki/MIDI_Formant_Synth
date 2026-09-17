#include "GUI.h"
#include "PPQN_Reader/PPQN_Reader.h"
#include "Time_Signature_Reader/Time_Signature_Reader.h"
#include "BPM_Reader/BPM_Reader.h"
#include "System_Reader/System_Reader.h"
#include <string>
#include <algorithm>
#include <cmath>

namespace {

	/// UTF-8 → UTF-16（所有 Win32 宽字符 API 都要这一手）。
	std::wstring toWide(const std::string& utf8)
	{
		if (utf8.empty())
			return std::wstring();
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                                    static_cast<int>(utf8.size()), nullptr, 0);
		if (n <= 0)
			return std::wstring();
		std::wstring w(static_cast<std::size_t>(n), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
		                      static_cast<int>(utf8.size()), w.data(), n);
		return w;
	}

	/// 把文本写进某个控件。
	void SetControlText(HWND hwnd, int id, const std::string& utf8)
	{
		const std::wstring w = toWide(utf8);
		SetWindowTextW(GetDlgItem(hwnd, id), w.c_str());
	}

	/// 弹出「打开文件」对话框。用户取消时返回 false。
	bool PickMidiFile(std::string& outPath)
	{
		wchar_t buf[MAX_PATH] = L"";

		OPENFILENAMEW ofn = {};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		ofn.lpstrFilter = L"MIDI 文件\0*.mid;*.midi\0所有文件\0*.*\0";
		ofn.lpstrFile = buf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = L"选择一个 SMF 文件";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

		if (!GetOpenFileNameW(&ofn))
			return false;

		const int n = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
		if (n <= 0)
			return false;
		std::string utf8(static_cast<std::size_t>(n), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), n, nullptr, nullptr);
		if (!utf8.empty() && utf8.back() == '\0')
			utf8.pop_back();

		outPath = utf8;
		return true;
	}

	/// 取命令行第 1 个参数（作为启动时要打开的文件路径）。没有则返回空串。
	/// 用 CommandLineToArgvW 是 Windows 上解析命令行最可靠的方式。
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

namespace GUI {

	// ───────────────────────── PitchCanvas ─────────────────────────

	constexpr wchar_t PitchCanvas::kClassName[];

	void PitchCanvas::Create(HWND parent, HINSTANCE inst, int id)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &PitchCanvas::WndProc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
		wc.hbrBackground = nullptr;                                   // 自绘，不需要背景刷
		wc.lpszClassName = kClassName;
		RegisterClassExW(&wc);

		hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, L"",
		                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
		                        0, 0, 0, 0, parent,
		                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
		                        inst, this);

		MakePens();          // 128 支按力度索引的画笔
	}

	void PitchCanvas::SetCurve(const PITCH::Curve& curve)
	{
		curve_ = curve;
		hasCurve_ = true;
		scrollX_ = 0;
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::SetVolumeCurve(const VOL::Curve& curve)
	{
		volCurve_ = curve;
		BuildLoudness();
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::SetExpressionCurve(const EXP::Curve& curve)
	{
		expCurve_ = curve;
		BuildLoudness();
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::SetCc11FullScale(int v)
	{
		if (v < 1)   v = 1;
		if (v > 127) v = 127;
		if (v == cc11FullScale_)
			return;
		cc11FullScale_ = v;
		BuildLoudness();                    // 分母变了，整条响度曲线要重算
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::SetBoundNotes(const std::vector<BIND::BoundNote>& notes)
	{
		notes_ = notes;
		BuildLoudness();
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	// 「先慢后快」的衰减形状（B2）：x∈[0,1] → 累计掉的比例 ∈[0,1]。
	//   ∫₀ˣ ln(1+u) du = (1+x)·ln(1+x) − x      ← 别写成 x·ln(1+x)−x+0.5x²，那是错的
	//   除以 x=1 处的值（2ln2 − 1 ≈ 0.3862943611）归一化，于是 d(0)=0、d(1)=1。
	// 形状：x=0.25 掉 7.5%、x=0.5 掉 28.0%、x=0.75 掉 59.4% —— 先慢后快。
	double PitchCanvas::dcurve(double x)
	{
		if (x <= 0.0) return 0.0;
		if (x >= 1.0) return 1.0;
		const double kNorm = 2.0 * std::log(2.0) - 1.0;      // ≈ 0.3862943611
		return ((1.0 + x) * std::log(1.0 + x) - x) / kNorm;
	}

	// ── 模拟响度曲线 ──────────────────────────────────────────────
	//  响度(t) = velo × CC7(t)/100 × CC11(t)/100（衰减窗口内再乘一个衰减系数）
	//
	//  · velo        ：这个音符的源力度（音符身上的定值）
	//  · CC7(t)/100  ：通道音量的等比系数。**只继承它前面最后一次的值**，
	//                  纯采样保持 —— 不衰减、也不参与衰减起算。
	//  · CC11(t)/100 ：表情的等比系数。
	//
	//  两个 /100 都是**显示模型**的归一化口径（基准 = 100，即系数 1.0），
	//  ⚠️ 与协议 7.3 那里"输出 CC7 = 声部表值"完全无关。
	//
	//  衰减（规则由用户定，全部是本显示模型的经验值）：
	//    · 起算点 L = 本音符内**最后一次 CC11 事件**的时刻；
	//      音符内一次 CC11 都没有 → L = note_on
	//    · 只有「(note_off − L) / gate ≥ 50%」时才衰减，否则整段平推
	//    · 基准 = **L 时刻已经算好的响度**，到 note_off 掉
	//      kDecayDropPctMax × 占比（占比 1.0 时掉 5%）
	//    · 形状 = dcurve()，先慢后快
	//    · 每个音符各自重来（note_on 处不带上一音的衰减）
	//
	//  每个音符产出**一段**（LoudStroke），所以音符之间结构上就画不出线。
	void PitchCanvas::BuildLoudness()
	{
		loud_.clear();
		if (notes_.empty())
			return;

		loud_.reserve(notes_.size());

		for (const BIND::BoundNote& n : notes_) {
			int velo = n.velocity;
			if (velo < 1)   velo = 1;      // 等比系数不可为 0（velocity=0 本就不会成为音符）
			if (velo > 127) velo = 127;

			const double q0 = n.quarters;
			const double q1 = n.quarters + n.duration;
			const double gate = (q1 > q0) ? (q1 - q0) : 0.0;

			// 本音符内最后一次 CC11 的时刻（没有就是 note_on）
			double lQ = q0;
			for (const EXP::Point& p : expCurve_.points) {
				if (p.quarters > q0 && p.quarters < q1 && p.quarters > lQ)
					lQ = p.quarters;
			}

			// L 时刻的响度：这就是衰减的**基准**
			// 响度 = velocity × CC7(t)/100 × CC11(t)/R   （CC7 分母恒 100，不受 R 影响）
			const double base = static_cast<double>(velo)
			                  * static_cast<double>(VOL::ValueAt(volCurve_,  lQ)) / 100.0
			                  * static_cast<double>(EXP::ValueAt(expCurve_, lQ))
			                    / static_cast<double>(cc11FullScale_);

			// 衰减窗口占 gate 的比例；不到 50% 就完全不衰减
			const double ratio = (gate > 0.0) ? ((q1 - lQ) / gate) : 0.0;
			const bool   decay = (ratio >= kDecayMinGateRatio);

			// 本音符的事件时刻（相对 note_on 的偏移）：
			//   恒有 0（note_on）与 gate（note_off）
			//   落在音符内部的 CC7 / CC11 跳变点
			//   需要衰减时，再把 L 也放进去（它之前是平的，之后才是衰减段）
			std::vector<double> offs;
			offs.push_back(0.0);
			for (const VOL::Point& p : volCurve_.points)
				if (p.quarters > q0 && p.quarters < q1)
					offs.push_back(p.quarters - q0);
			for (const EXP::Point& p : expCurve_.points)
				if (p.quarters > q0 && p.quarters < q1)
					offs.push_back(p.quarters - q0);
			if (decay)
				offs.push_back(lQ - q0);
			if (gate > 0.0)
				offs.push_back(gate);

			std::sort(offs.begin(), offs.end());

			// ⚠️ 衰减窗口里**必须真的把曲线采样出来**，不能只放 L 和 note_off 两个端点
			//    然后用直线连起来 —— 那样 dcurve() 的「先慢后快」完全表现不出来，
			//    画出来就是个台阶。采样密度按衰减窗口的**像素宽度**定（约 2 px 一个点），
			//    并且强制包含 note_off，保证曲线末点正好落在音符右边界上。
			if (decay && gate > 0.0 && q1 > lQ) {
				const double winPx = (q1 - lQ) * pxPerQuarter_;
				int segs = 2;
				if (winPx > 2.0) {
					segs = static_cast<int>(winPx / 2.0);
					if (segs < 2)   segs = 2;
					if (segs > 256) segs = 256;    // 缩得很深时别把点爆掉
				}
				for (int j = 1; j < segs; ++j)
					offs.push_back((lQ - q0) + (q1 - lQ) * (static_cast<double>(j) / segs));
				std::sort(offs.begin(), offs.end());
			}

			LoudStroke stroke;
			stroke.pts.reserve(offs.size());

			for (std::size_t k = 0; k < offs.size(); ++k) {
				const double off = offs[k];
				if (k > 0 && off - offs[k - 1] <= 1e-12)
					continue;                       // 同一时刻重合的点只留一个

				const double qt = q0 + off;

				double v;
				if (decay && qt > lQ) {
					// 衰减窗口内：基准固定，乘 dcurve() 的形状
					const double x = (q1 > lQ) ? ((qt - lQ) / (q1 - lQ)) : 1.0;
					const double drop = kDecayDropPctMax * ratio * dcurve(x);
					v = base * (1.0 - drop);
				} else {
					// 衰减窗口之前（或本音符不衰减）：普通等比乘积
					v = static_cast<double>(velo)
					  * static_cast<double>(VOL::ValueAt(volCurve_,  qt)) / 100.0
					  * static_cast<double>(EXP::ValueAt(expCurve_, qt))
					    / static_cast<double>(cc11FullScale_);
				}

				LoudPoint lp;
				lp.quarters = qt;
				lp.value    = v;
				stroke.pts.push_back(lp);
			}

			if (!stroke.pts.empty())
				loud_.push_back(std::move(stroke));
		}
	}

	void PitchCanvas::MakePens()
	{
		// 力度 → 亮度：1 最暗（深褐）→ 127 最亮（亮橙），中间线性插值。
		// velocity 只有 128 个取值，所以一次造好，画的时候直接取，不必每个音符 CreatePen。
		for (int v = 0; v < 128; ++v) {
			const int t = (v < 1) ? 0 : (v > 126 ? 126 : v - 1);   // 0..126
			const int r = 90 + (255 - 90) * t / 126;
			const int g = 35 + (170 - 35) * t / 126;
			const int b = 15 + (80  - 15) * t / 126;
			velPen_[v] = CreatePen(PS_SOLID, 2, RGB(r, g, b));
		}
	}

	void PitchCanvas::FreePens()
	{
		for (int v = 0; v < 128; ++v) {
			if (velPen_[v]) {
				DeleteObject(velPen_[v]);
				velPen_[v] = nullptr;
			}
		}
	}

	void PitchCanvas::SetMode(Mode m)
	{
		mode_ = m;
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::ClampScroll()
	{
		if (scrollX_ < 0)
			scrollX_ = 0;

		// 不许滚进空白：内容宽度 = 曲长 × 每四分音符像素
		if (hwnd_) {
			RECT rc = {};
			if (GetClientRect(hwnd_, &rc)) {
				const int contentW =
					static_cast<int>(curve_.quartersEnd * pxPerQuarter_);
				int maxScroll = contentW - (rc.right - rc.left);
				if (maxScroll < 0)
					maxScroll = 0;
				if (scrollX_ > maxScroll)
					scrollX_ = maxScroll;
			}
		}
	}

	void PitchCanvas::Zoom(bool vertical, double factor)
	{
		if (vertical) {
			pxPerCent_ *= factor;
			pxPerCent_ = std::max(0.02, std::min(2.0, pxPerCent_));
		} else {
			pxPerQuarter_ *= factor;
			pxPerQuarter_ = std::max(2.0, std::min(400.0, pxPerQuarter_));
		}
		ClampScroll();                      // 缩放后滚动位置可能越界
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::ScrollBy(int dxPixels)
	{
		scrollX_ += dxPixels;
		ClampScroll();
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::ResetView()
	{
		pxPerQuarter_ = 25.0;   // 100 px = 4 个四分音符
		pxPerCent_    = 0.1;    // 1 px = 10 音分
		scrollX_      = 0;
		if (hwnd_)
			InvalidateRect(hwnd_, nullptr, FALSE);
	}

	void PitchCanvas::Paint(HDC dc, const RECT& rc)
	{
		const int w = rc.right - rc.left;
		const int h = rc.bottom - rc.top;
		const int midY = h / 2;                       // 0 音分所在的那条线

		// 背景
		HBRUSH bg = CreateSolidBrush(RGB(252, 252, 250));
		FillRect(dc, &rc, bg);
		DeleteObject(bg);

		if (!hasCurve_ || w <= 0 || h <= 0)
			return;

		// 横向坐标已经是"四分音符"，这里只需乘上像素比例
		const double pxPerQuarter = pxPerQuarter_;		const double pxPerCent    = pxPerCent_;
		const int    scrollX      = scrollX_;

		// 横向网格（丙）：
		//   淡线 = 每四分音符一条
		//   深线 = 每小节一条 + 小节号
		const double firstQ = std::floor(static_cast<double>(scrollX) / pxPerQuarter);
		const double lastQ  = firstQ + static_cast<double>(w) / pxPerQuarter + 1.0;

		{
			HPEN pen = CreatePen(PS_SOLID, 1, RGB(232, 232, 228));
			HGDIOBJ old = SelectObject(dc, pen);
			for (double q = firstQ; q <= lastQ; q += 1.0) {
				const int x = static_cast<int>(q * pxPerQuarter) - scrollX;
				if (x < 0 || x >= w)
					continue;
				MoveToEx(dc, x, 0, nullptr);
				LineTo(dc, x, h);
			}
			SelectObject(dc, old);
			DeleteObject(pen);
		}

		{
			SetBkMode(dc, TRANSPARENT);
			HPEN pen = CreatePen(PS_SOLID, 1, RGB(170, 170, 165));
			HGDIOBJ old = SelectObject(dc, pen);
			for (const PITCH::BarLine& b : curve_.bars) {
				if (b.quarters < firstQ - 1.0 || b.quarters > lastQ + 1.0)
					continue;
				const int x = static_cast<int>(b.quarters * pxPerQuarter) - scrollX;
				if (x < 0 || x >= w)
					continue;
				MoveToEx(dc, x, 0, nullptr);
				LineTo(dc, x, h);

				// 小节号（画在小节线右侧）
				wchar_t num[16];
				swprintf_s(num, L"%d", b.measure);
				SetTextColor(dc, RGB(120, 120, 120));
				TextOutW(dc, x + 3, 22, num, static_cast<int>(wcslen(num)));
			}
			SelectObject(dc, old);
			DeleteObject(pen);
		}

		// 纵向：每 1200 音分（八度）一条线 + 中央 0 线
		{
			HPEN penOct = CreatePen(PS_SOLID, 1, RGB(210, 210, 205));
			HGDIOBJ old = SelectObject(dc, penOct);
			for (int oct = -2; oct <= 2; ++oct) {
				const int y = midY - static_cast<int>(oct * 1200 * pxPerCent);
				if (y < 0 || y >= h)
					continue;
				MoveToEx(dc, 0, y, nullptr);
				LineTo(dc, w, y);
			}
			SelectObject(dc, old);
			DeleteObject(penOct);

			HPEN penZero = CreatePen(PS_SOLID, 1, RGB(150, 150, 145));
			old = SelectObject(dc, penZero);
			MoveToEx(dc, 0, midY, nullptr);
			LineTo(dc, w, midY);
			SelectObject(dc, old);
			DeleteObject(penZero);
		}

		// ── 曲线 ──────────────────────────────────────────────────
		// 两种模式都是「采样保持」语义：没有新消息时值不变，
		// 所以一律画成阶梯（水平保持 + 垂直跳变），不能把相邻两点连直线 ——
		// 否则颤音之间的长空白会被画成一条根本不存在的缓坡。
		if (mode_ == Mode::Bend) {
			// 弯音造成的音高变化（相对音符）
			if (!curve_.points.empty()) {
				HPEN pen = CreatePen(PS_SOLID, 2, RGB(20, 90, 200));
				HGDIOBJ old = SelectObject(dc, pen);

				int  prevY = midY;           // 第一个事件之前，弯音轮停在中心（0 音分）
				bool first = true;

				for (const PITCH::Point& p : curve_.points) {
					const int x = static_cast<int>(p.quarters * pxPerQuarter) - scrollX;
					const int y = midY - static_cast<int>(p.cents * pxPerCent);

					if (first) {
						// 从绘图区左边缘保持 0 音分，一直画到第一个事件处
						MoveToEx(dc, 0, midY, nullptr);
						LineTo(dc, x, midY);
						first = false;
					} else {
						// 保持上一段的值，延伸到本事件的位置
						LineTo(dc, x, prevY);
					}
					// 垂直跳到新值
					LineTo(dc, x, y);
					prevY = y;
				}
				// 最后一个值保持到右边缘
				LineTo(dc, w, prevY);

				SelectObject(dc, old);
				DeleteObject(pen);
			}
		} else if (mode_ == Mode::Absolute) {
			// 音符之间空开：只在 note_on 到 note_off 之间画（数据里真有的才画）。
			// 纵轴零点 = A4（音高编号 69）。
			if (!notes_.empty()) {
				SetBkMode(dc, TRANSPARENT);

				for (const BIND::BoundNote& n : notes_) {
					if (n.pitch.empty())
						continue;                       // 理论上不会（绑定必给首点）

					// 力度 → 画笔（越用力越亮）
					int v = n.velocity;
					if (v < 1)   v = 1;
					if (v > 127) v = 127;
					HPEN pen = velPen_[v] ? velPen_[v] : velPen_[100];
					HGDIOBJ old = pen ? SelectObject(dc, pen) : nullptr;

					const double q0 = n.quarters;
					const double q1 = n.quarters + n.duration;

					// 首点（绝对音高已绑在音符身上，不用再查弯音表）
					int y = midY - static_cast<int>(n.pitch.front().cents * pxPerCent);
					int minY = y;                       // 轨迹最高处，用来放歌词
					const int x0 = static_cast<int>(q0 * pxPerQuarter) - scrollX;
					MoveToEx(dc, x0, y, nullptr);

					// 轨迹其余各点：保持 → 跳变
					for (std::size_t i = 1; i < n.pitch.size(); ++i) {
						const int px = static_cast<int>(
							(q0 + n.pitch[i].offset) * pxPerQuarter) - scrollX;
						LineTo(dc, px, y);              // 保持在旧值
						y = midY - static_cast<int>(n.pitch[i].cents * pxPerCent);
						if (y < minY)
							minY = y;
						LineTo(dc, px, y);              // 跳到新值
					}
					// 保持到音符结束（音符之间空开：note_on 到 note_off 之间才画）
					const int x1 = static_cast<int>(q1 * pxPerQuarter) - scrollX;
					LineTo(dc, x1, y);

					if (old)
						SelectObject(dc, old);

					// 歌词：写在音符线段上方（线段太窄就不写，免得糊成一团）
					if (!n.lyric.empty() && (x1 - x0) > 16) {
						const std::wstring w = toWide(n.lyric);
						SetTextColor(dc, RGB(90, 90, 90));
						TextOutW(dc, x0 + 2, minY - 20, w.c_str(),
						         static_cast<int>(w.size()));
					}
				}
			}
		} else {
			// ── 模拟响度曲线 ──
			// 响度(t) = velo × CC7(t)/100 × CC11(t)/100（详见 BuildLoudness）。
			// loud_ 是「一个音符一段」（LoudStroke），所以音符之间**结构上就不可能
			// 被连起来**：每段各自 MoveToEx 起笔、画完就结束，不跨段 LineTo。
			// 横向按 quarters 严格定位：音符左边界对得上刻度，第一个音符之前留白。
			// 纵向仍以 midY 为 0 基准：1 个响度单位 = 1 像素，
			// 于是响度 127 落在 midY−127，跟 pxPerCent_ 无关、永远可见。
			if (!loud_.empty()) {
				SetBkMode(dc, TRANSPARENT);
				HPEN pen = CreatePen(PS_SOLID, 2, RGB(200, 60, 30));   // 砖红
				HGDIOBJ old = SelectObject(dc, pen);

				auto toY = [midY](double v) {
					return midY - static_cast<int>(v);                  // 1 单位 = 1 像素
				};
				auto toX = [&](double q) {
					return static_cast<int>(q * pxPerQuarter) - scrollX;
				};

				for (const LoudStroke& st : loud_) {
					if (st.pts.empty())
						continue;

					// 音符左边界：从 tick 0 开始的音符才允许伸到左边缘，
					// 否则严格落在它的 note_on 处（不硬拉一条线过来）。
					int xs = toX(st.pts.front().quarters);
					if (st.pts.front().quarters <= 0.0)
						xs = 0;
					if (xs < 0)
						xs = 0;
					if (xs >= w)
						continue;                       // 整段在视野右侧，跳过

					const int k = static_cast<int>(st.pts.size());
					MoveToEx(dc, xs, toY(st.pts[0].value), nullptr);

					for (int j = 1; j <= k; ++j) {
						const bool last = (j == k);
						int x = toX(st.pts[last ? k - 1 : j].quarters);
						if (x > w)
							x = w;                      // 右边界夹到画布内
						// 先保持上一段的值（带轻微衰减的斜线）延伸到本处，再垂直跳变
						LineTo(dc, x, toY(st.pts[j - 1].value));
						if (!last)
							LineTo(dc, x, toY(st.pts[j].value));
					}
				}

				SelectObject(dc, old);
				DeleteObject(pen);
			}
		}

		// 左上角提示当前模式与刻度
		{
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(120, 120, 120));
			wchar_t info[192];
			const wchar_t* modeName = (mode_ == Mode::Bend)
				? L"模式：弯音变化"
				: (mode_ == Mode::Absolute
					? L"模式：绝对音高（0 音分 = A4）"
					: L"模式：模拟响度曲线（velo × CC7/100 × CC11/R）");
			if (mode_ == Mode::Loudness) {
				swprintf_s(info,
				           L"%s    纵向 1 单位响度 = 1 像素    横向 %.0f px/四分音符    滚动 %d px"
				           L"    [滚轮=横向滚动  横滚轮/Shift+滚轮/+−=横向缩放  ←→ PgUp PgDn=滚动]",
				           modeName, pxPerQuarter, scrollX);
			} else {
				swprintf_s(info,
				           L"%s    %.0f px/四分音符    1 px = %.3g 音分    滚动 %d px"
				           L"    [滚轮=横向滚动  横滚轮/Shift+滚轮/+−=横向缩放  Ctrl+滚轮/↑↓=纵向缩放]",
				           modeName, pxPerQuarter, 1.0 / (pxPerCent > 0 ? pxPerCent : 1), scrollX);
			}
			TextOutW(dc, 8, 6, info, static_cast<int>(wcslen(info)));
		}
	}

	LRESULT CALLBACK PitchCanvas::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		PitchCanvas* self =
			reinterpret_cast<PitchCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

		switch (msg) {
		case WM_CREATE: {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = static_cast<PitchCanvas*>(cs->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			if (self)
				self->hwnd_ = hwnd;
			return 0;
		}

		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			if (self)
				self->Paint(dc, rc);
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_ERASEBKGND:
			return 1;                                  // 已在 Paint 里填了背景

		case WM_LBUTTONDOWN:
			SetFocus(hwnd);                            // 点一下就能用键盘
			return 0;

		case WM_KEYDOWN: {
			if (!self)
				return 0;
			const int step = 40;
			switch (wp) {
			case VK_LEFT:   self->ScrollBy(-step); return 0;
			case VK_RIGHT:  self->ScrollBy(step);  return 0;
			case VK_UP:     self->Zoom(true, 1.25);  return 0;   // 纵向放大
			case VK_DOWN:   self->Zoom(true, 0.8);   return 0;   // 纵向缩小
			case VK_PRIOR:  self->ScrollBy(-step * 8); return 0; // PageUp
			case VK_NEXT:   self->ScrollBy(step * 8);  return 0; // PageDown
			// 没有横滚轮的鼠标用这两个键代替（横向缩放）
			case VK_OEM_PLUS:   case VK_ADD:
				self->Zoom(false, 1.25); return 0;
			case VK_OEM_MINUS:  case VK_SUBTRACT:
				self->Zoom(false, 0.8);  return 0;
			default: break;
			}
			return 0;
		}

		case WM_MOUSEHWHEEL: {
			if (!self)
				return 0;
			// 横向滚轮（鼠标的倾斜滚轮）：直接调「像素/四分音符」= 横向缩放。
			// 往右推 → 放大（每个四分音符占更多像素），短音符的线段才放得下歌词。
			const int delta = GET_WHEEL_DELTA_WPARAM(wp);
			self->Zoom(false, delta > 0 ? 1.25 : 0.8);
			return 0;
		}

		case WM_MOUSEWHEEL: {
			if (!self)
				return 0;
			const int delta = GET_WHEEL_DELTA_WPARAM(wp);
			const bool ctrl  = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
			const bool shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
			const double factor = (delta > 0) ? 1.25 : 0.8;

			if (ctrl)
				self->Zoom(true, factor);              // Ctrl + 滚轮 → 纵向缩放
			else if (shift)
				self->Zoom(false, factor);             // Shift + 滚轮 → 横向缩放
			else
				self->ScrollBy(delta > 0 ? -60 : 60);  // 滚轮 → 横向滚动
			return 0;
		}

		case WM_NCDESTROY: {
			if (self)
				self->FreePens();
			return DefWindowProcW(hwnd, msg, wp, lp);
		}

		default:
			break;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	// ───────────────────────── SmfDetectWindow ─────────────────────────

	constexpr wchar_t SmfDetectWindow::kClassName[];
	constexpr wchar_t SmfDetectWindow::kTitle[];

	LRESULT CALLBACK SmfDetectWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		SmfDetectWindow* self =
			reinterpret_cast<SmfDetectWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

		switch (msg) {
		case WM_CREATE: {
			auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
			self = static_cast<SmfDetectWindow*>(cs->lpCreateParams);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			self->OnCreate(hwnd);
			return 0;
		}

		case WM_SIZE: {
			if (!self)
				return 0;
			const int w = LOWORD(lp);
			const int h = HIWORD(lp);
			const int margin = 12;
			const int btnH = 32;
			const int gap = 8;
			const int availH = h - btnH - margin * 3;

			// 按钮行（5 个）+ 右端 R 选择：选择文件 ｜ 弯音变化 ｜ 绝对音高 ｜ 模拟响度曲线 ｜ 重置视图 ｜ CC11 基准 R [___]
			// 窗口默认宽 1100 → 客户区约 1090，扣掉两侧边距 12 后正好放得下
			int bx = margin;
			const int btnWidths[5] = { 100, 90, 90, 116, 90 };
			const int btnIds[5] = { kIdButtonOpen, kIdButtonBend, kIdButtonAbs, kIdButtonLoud, kIdButtonReset };
			const int btnGap = 8;
			for (int i = 0; i < 5; ++i) {
				MoveWindow(GetDlgItem(hwnd, btnIds[i]), bx, margin, btnWidths[i], btnH, TRUE);
				bx += btnWidths[i] + btnGap;
			}

			// 右端：R 标签 + 输入框（1..127）
			{
				const int lblW = 112;
				const int edW  = 52;
				MoveWindow(GetDlgItem(hwnd, kIdLabelExpR), bx, margin + 7, lblW, 20, TRUE);
				MoveWindow(GetDlgItem(hwnd, kIdEditExpR), bx + lblW + 4, margin + 2, edW, 26, TRUE);
			}

			// 上半 = 文本结果，下半 = 绘图区
			const int textH = availH / 2;
			const int canvasY = margin + btnH + margin + textH + gap;
			const int canvasH = h - canvasY - margin;

			MoveWindow(GetDlgItem(hwnd, kIdEditResult),
			           margin, margin + btnH + margin,
			           w - margin * 2 > 0 ? w - margin * 2 : 0,
			           textH > 0 ? textH : 0, TRUE);

			if (self->canvas_.Handle())
				MoveWindow(self->canvas_.Handle(),
				           margin, canvasY,
				           w - margin * 2 > 0 ? w - margin * 2 : 0,
				           canvasH > 0 ? canvasH : 0, TRUE);
			return 0;
		}

		case WM_COMMAND: {
			if (!self)
				return 0;
			const int id = LOWORD(wp);
			if (HIWORD(wp) == BN_CLICKED) {
				if (id == kIdButtonOpen) {
					self->OpenFile();
					return 0;
				}
				if (id == kIdButtonBend) {
					self->canvas_.SetMode(PitchCanvas::Mode::Bend);
					return 0;
				}
				if (id == kIdButtonAbs) {
					self->canvas_.SetMode(PitchCanvas::Mode::Absolute);
					return 0;
				}
				if (id == kIdButtonLoud) {
					self->canvas_.SetMode(PitchCanvas::Mode::Loudness);
					return 0;
				}
				if (id == kIdButtonReset) {
					self->canvas_.ResetView();
					return 0;
				}
			}
			if (id == kIdEditResult && HIWORD(wp) == EN_UPDATE)
				return 0;

			// CC11 满响度基准输入框：边打边生效（EN_CHANGE）。空串/0/超界在
			// ApplyCc11FullScaleFromEdit 里夹到 1..127。
			// ⚠️ 必须在控件初始化完之后才受理：CreateWindowExW 里给 EDIT 设初值时
			//    也会发 EN_CHANGE，那一刻读输入框可能读到空串 → 被夹成 1，
			//    于是默认值 100 会被自己踩成 1。
			if (self->ratioUiReady_ && id == kIdEditExpR && HIWORD(wp) == EN_CHANGE) {
				self->ApplyCc11FullScaleFromEdit();
				return 0;
			}
			// 失焦：此时输入串若不合法（比如用户把框清空就走了），回写纠正过的值。
			// 合法的输入串在里面会提前 return，不会被打扰。
			if (id == kIdEditExpR && HIWORD(wp) == EN_KILLFOCUS) {
				self->ApplyCc11FullScaleFromEdit();
				return 0;
			}
			return 0;
		}

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;

		default:
			break;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	void SmfDetectWindow::OnCreate(HWND hwnd)
	{
		hwnd_ = hwnd;

		// 中文字体（微软雅黑）
		font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
		                    L"Microsoft YaHei");

		struct { const wchar_t* text; int id; int width; } buttons[] = {
			{ L"选择文件",     kIdButtonOpen,  118 },
			{ L"弯音变化",     kIdButtonBend,  104 },
			{ L"绝对音高",     kIdButtonAbs,   104 },
			{ L"模拟响度曲线", kIdButtonLoud,  132 },
			{ L"重置视图",     kIdButtonReset, 104 },
		};
		for (const auto& b : buttons) {
			HWND h = CreateWindowExW(0, L"BUTTON", b.text,
			                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
			                         0, 0, b.width, 32, hwnd,
			                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(b.id)),
			                         GetModuleHandleW(nullptr), nullptr);
			if (font_)
				SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
		}

		// CC11 的「满响度基准」R：标签 + 输入框（1..127，默认 100）
		{
			HWND lbl = CreateWindowExW(0, L"STATIC", L"CC11 基准 R：",
			                           WS_CHILD | WS_VISIBLE | SS_LEFT,
			                           0, 0, 112, 20, hwnd,
			                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLabelExpR)),
			                           GetModuleHandleW(nullptr), nullptr);
			if (font_)
				SendMessageW(lbl, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

			wchar_t buf[16];
			swprintf_s(buf, L"%d", canvas_.Cc11FullScale());   // 画布公开 getter，默认 100
			HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf,
			                          WS_CHILD | WS_VISIBLE | ES_LEFT | ES_NUMBER | ES_AUTOHSCROLL,
			                          0, 0, 52, 26, hwnd,
			                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditExpR)),
			                          GetModuleHandleW(nullptr), nullptr);
			if (font_)
				SendMessageW(ed, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
		}

		HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		                            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
		                            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
		                            0, 0, 0, 0, hwnd,
		                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditResult)),
		                            GetModuleHandleW(nullptr), nullptr);
		if (font_)
			SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
		if (edit) {
			SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
			SetWindowTextW(edit, L"点「选择文件」挑一个 .mid，捏。\r\n");
		}

		canvas_.Create(hwnd, GetModuleHandleW(nullptr), kIdCanvasPitch);

		// 控件都建好了，现在开始受理 R 输入框的通知；
		// 顺便把输入框里的初值正式应用一次（幂等）。
		ratioUiReady_ = true;
		ApplyCc11FullScaleFromEdit();

		// 让窗口一打开就有合理尺寸（避免首个 WM_SIZE 之前控件都在 0 尺寸）
		RECT rc;
		GetClientRect(hwnd, &rc);
		SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED,
		             MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));

		// 命令行带了文件 → 直接载入
		if (!initialFile_.empty()) {
			currentPath_ = initialFile_;
			SetControlText(hwnd_, kIdEditResult, Detect(initialFile_));
			UpdatePitch();
		}
	}

	// 读输入框里的 CC11 满响度基准 → 夹到 1..127 → 交给画布重算响度曲线。
	// 空串 / 0 / 超界都会落到 1 或 127，保证分母永远 ≥1（除零在这里挡掉）。
	// ⚠️ 只在输入串**本身合法**（1..127）时才回写输入框：
	//    否则用户想把 100 改成 50、先把内容清空的那一瞬间就会被夹成 "1"，
	//    接着打的 "5" 就变成 "15" —— 边打边改会打架。
	//    不合法的内容留到失焦（EN_KILLFOCUS）时再回写纠正。
	void SmfDetectWindow::ApplyCc11FullScaleFromEdit()
	{
		const HWND ed = GetDlgItem(hwnd_, kIdEditExpR);
		if (!ed)
			return;

		wchar_t buf[16] = L"";
		GetWindowTextW(ed, buf, 16);

		const int raw = _wtoi(buf);         // 非数字 / 空串 → 0
		int v = raw;
		if (v < 1)   v = 1;
		if (v > 127) v = 127;

		canvas_.SetCc11FullScale(v);        // 内部已做夹取，值相同则直接返回

		// 只有「输入串合法」才回写，避免打断正在打字的人
		if (buf[0] != L'\0' && raw >= 1 && raw <= 127)
			return;

		wchar_t show[16];
		swprintf_s(show, L"%d", v);
		if (wcscmp(show, buf) != 0)
			SetWindowTextW(ed, show);
	}

	void SmfDetectWindow::OpenFile()
	{
		std::string path;
		if (!PickMidiFile(path))
			return;

		currentPath_ = path;
		SetControlText(hwnd_, kIdEditResult, Detect(path));
		UpdatePitch();                       // 选完文件立刻刷新曲线
	}

	void SmfDetectWindow::UpdatePitch()
	{
		if (currentPath_.empty()) {
			MessageBoxW(hwnd_, L"还没有选文件捏。", L"提示", MB_OK | MB_ICONINFORMATION);
			return;
		}

		// 系统判定 → 决定文件无 RPN 时的默认 R
		const SR::Result sr = SR::SR(currentPath_);
		pitch_ = PITCH::PM(currentPath_, sr.system);

		// 音符 → 绑定：Note_Mapper / Pitch_Mapper / Lyric_Mapper 互不相识，由 Note_Binder 合体
		note_ = NOTE::NM(currentPath_);
		lyric_ = LYRIC::LM(currentPath_);
		bound_ = BIND::NB(note_.notes, pitch_.curve, lyric_.timeline);

		// 表情（CC11）与通道音量（CC7）：模拟响度曲线要用；
		// 两者各读各的曲线，乘法在画布里做（见 BuildLoudness）。
		expr_ = EXP::EM(currentPath_);
		vol_  = VOL::VM(currentPath_);

		canvas_.SetCurve(pitch_.curve);
		canvas_.SetExpressionCurve(expr_.curve);
		canvas_.SetVolumeCurve(vol_.curve);
		canvas_.SetBoundNotes(bound_);
	}

	std::string SmfDetectWindow::Detect(const std::string& path)
	{
		std::string out;

		out += "文件：" + path + "\r\n";
		out += "----------------------------------------\r\n";

		const PPQN::Result pr = PPQN::PR(path);
		out += pr.message + "\r\n\r\n";

		const TSR::Result tsr = TSR::TSR(path);
		out += tsr.message + "\r\n";

		const BR::Result br = BR::BR(path);
		out += br.message + "\r\n";

		const SR::Result sr = SR::SR(path);
		out += sr.message + "\r\n";

		const PITCH::Result pm = PITCH::PM(path, sr.system);
		out += pm.message + "\r\n";

		const EXP::Result er = EXP::EM(path);
		out += er.message + "\r\n";

		const VOL::Result vr = VOL::VM(path);
		out += vr.message + "\r\n";

		const NOTE::Result nm = NOTE::NM(path);
		out += nm.message + "\r\n";

		const LYRIC::Result lm = LYRIC::LM(path);
		out += lm.message + "\r\n";

		return out;
	}

	int SmfDetectWindow::Run()
	{
		HINSTANCE inst = GetModuleHandleW(nullptr);

		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &SmfDetectWindow::WndProc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		wc.lpszClassName = kClassName;
		RegisterClassExW(&wc);

		HWND hwnd = CreateWindowExW(0, kClassName, kTitle,
		                            WS_OVERLAPPEDWINDOW,
		                            CW_USEDEFAULT, CW_USEDEFAULT, 1100, 820,
		                            nullptr, nullptr, inst, this);
		if (!hwnd)
			return 1;

		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);

		MSG msg = {};
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		if (font_)
			DeleteObject(font_);

		return static_cast<int>(msg.wParam);
	}

}

/// 图形界面入口（WIN32_EXECUTABLE → 子系统 WINDOWS → 入口是 WinMain）。
/// 用法：smf_detect.exe [file.mid]
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	GUI::SmfDetectWindow win;
	win.SetInitialFile(FirstCommandLineArg());
	return win.Run();
}
