// Smf_Note_Peek.cpp —— 单独解析一个 SMF，逐轨列出 note_on / note_off 的配对情况。
//
// 为什么用 C++ 而不是 PowerShell（坑 #13 / #14）：
//   本工程验证脚本自己出过的错比被验证的代码还多。而且这台机器上 PowerShell 的执行策略
//   是 Restricted，`.ps1` / `.psm1` 一律加载不了 —— 想绕就得改系统设置，不值得。
//
// 用途（当前主要在调字典）：
//   · 看某一轨的**力度分布** —— 例：小 gate 的力度到底写进去没有（期望 20）
//   · 看**时长分布** —— 例：1 tick 的小 gate 有没有被重叠收缩吃掉、闭成零长度
//   · 看每个音符的 tick 与力度，判断同 tick 上谁盖了谁
//
// 用法：
//   smf_note_peek.exe <file.mid>              列出全部轨的摘要
//   smf_note_peek.exe <file.mid> --track 1    只看第 1 轨（0-based，轨 1 = 输出 CH0/F0）
//   smf_note_peek.exe <file.mid> --vel 20     只列力度 = 20 的音符
//   smf_note_peek.exe <file.mid> --track 1 --vel 20
//
// 注意：本工具**不复用**任何产品代码（reader / Smf_Writer 都不用）——
//       「生成器/解析器互相独立」才算交叉验证（同款理由）。

#include "PPQN_Reader/File_Open.h"   // FS::OpenFile：UTF-8 路径打开

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

	struct Raw {
		std::vector<std::uint8_t> b;

		bool u8(std::size_t i, std::uint8_t& out) const {
			if (i >= b.size()) return false;
			out = b[i];
			return true;
		}
		std::uint32_t be32(std::size_t i) const {
			return (static_cast<std::uint32_t>(b[i]) << 24)
			     | (static_cast<std::uint32_t>(b[i + 1]) << 16)
			     | (static_cast<std::uint32_t>(b[i + 2]) << 8)
			     |  static_cast<std::uint32_t>(b[i + 3]);
		}
		std::uint16_t be16(std::size_t i) const {
			return static_cast<std::uint16_t>((b[i] << 8) | b[i + 1]);
		}
	};

	bool ReadVlq(const Raw& r, std::size_t& i, std::size_t end, std::uint32_t& out)
	{
		std::uint32_t v = 0;
		int n = 0;
		while (i < end && n < 4) {
			const std::uint8_t c = r.b[i++];
			v = (v << 7) | (c & 0x7Fu);
			if ((c & 0x80u) == 0) { out = v; return true; }
			++n;
		}
		return false;
	}

	struct Note {
		std::uint32_t onTick = 0;
		std::uint32_t offTick = 0;
		int note = 0, vel = 0;
	};

	struct Track {
		std::string name;
		std::vector<Note> notes;
		int stillOpen = 0;
		std::uint32_t maxTick = 0;
	};

	/// 解析一条轨。配对用 FIFO（与 Note_Mapper 的口径一致：先开先关）。
	Track ParseTrack(const Raw& r, std::size_t begin, std::size_t end)
	{
		Track t;
		std::size_t i = begin;
		std::uint32_t tick = 0;
		std::uint8_t stat = 0;

		// note -> 已开启但未关闭的 (tick, vel)，用 multimap 保留 FIFO 顺序
		std::multimap<int, std::pair<std::uint32_t, int>> open;

		while (i < end) {
			std::uint32_t dt = 0;
			if (!ReadVlq(r, i, end, dt)) break;
			tick += dt;
			if (i >= end) break;
			if (tick > t.maxTick) t.maxTick = tick;

			const std::uint8_t c = r.b[i];
			if (c == 0xFF) {                       // meta
				++i;
				if (i >= end) break;
				const std::uint8_t mt = r.b[i++];
				std::uint32_t ml = 0;
				if (!ReadVlq(r, i, end, ml)) break;
				if (mt == 0x03 && i + ml <= end)
					t.name.assign(reinterpret_cast<const char*>(&r.b[i]), ml);
				i += ml;
				continue;
			}
			if (c == 0xF0 || c == 0xF7) {          // SysEx
				++i;
				std::uint32_t sl = 0;
				if (!ReadVlq(r, i, end, sl)) break;
				i += sl;
				continue;
			}
			if (c >= 0x80) { stat = c; ++i; }      // running status 不重复出现状态字节
			const std::uint8_t hi = stat & 0xF0u;
			if (hi == 0xC0 || hi == 0xD0) { ++i; continue; }

			if (i + 1 >= end) break;
			const int d1 = r.b[i], d2 = r.b[i + 1];
			i += 2;

			if (hi == 0x90 && d2 > 0) {
				open.insert({ d1, { tick, d2 } });
			} else if (hi == 0x80 || (hi == 0x90 && d2 == 0)) {
				auto it = open.find(d1);           // FIFO：同音高先开先关
				if (it != open.end()) {
					Note n;
					n.onTick  = it->second.first;
					n.offTick = tick;
					n.note    = d1;
					n.vel     = it->second.second;
					t.notes.push_back(n);
					open.erase(it);
				}
			}
		}
		t.stillOpen = static_cast<int>(open.size());
		return t;
	}

	void PrintHistogram(const std::vector<Note>& notes, const char* title, bool byDuration)
	{
		std::map<int, int> hist;
		for (const Note& n : notes)
			++hist[byDuration ? static_cast<int>(n.offTick - n.onTick) : n.vel];
		if (hist.empty()) return;
		std::printf("  %s\n", title);
		for (const auto& kv : hist)
			std::printf("    %6d -> %5d \u4e2a\n", kv.first, kv.second);
	}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	const char* path = nullptr;
	int wantTrack = -1;
	int wantVel   = -1;
	int dumpN     = 0;

	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--track") == 0 && i + 1 < argc)      wantTrack = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--vel") == 0 && i + 1 < argc)   wantVel   = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)  dumpN     = std::atoi(argv[++i]);
		else if (!path)                                                path = argv[i];
	}

	if (!path) {
		std::printf("\u7528\u6cd5: smf_note_peek.exe <file.mid> [--track N] [--vel V]\n");
		return 1;
	}

	Raw r;
	{
		std::ifstream f = FS::OpenFile(path);
		if (!f) { std::printf("\u6253\u4e0d\u5f00\u6587\u4ef6\u3002\n"); return 1; }
		f.seekg(0, std::ios::end);
		const std::streamoff sz = f.tellg();
		f.seekg(0, std::ios::beg);
		if (sz <= 14) { std::printf("\u6587\u4ef6\u592a\u5c0f\u3002\n"); return 1; }
		r.b.resize(static_cast<std::size_t>(sz));
		f.read(reinterpret_cast<char*>(r.b.data()), sz);
	}

	if (r.b.size() < 14 || std::memcmp(&r.b[0], "MThd", 4) != 0) {
		std::printf("\u4e0d\u662f MThd \u5f00\u5934\uff0c\u4e0d\u662f\u5408\u6cd5 SMF\u3002\n");
		return 1;
	}
	const std::uint16_t ntrk = r.be16(10);
	const std::uint16_t ppqn = r.be16(12);
	std::printf("\u6587\u4ef6\uff1a%s\n", path);
	std::printf("\u683c\u5f0f %u  \u8f68\u6570 %u  PPQN %u  \u5171 %zu \u5b57\u8282\n\n",
	            static_cast<unsigned>(r.be16(8)), static_cast<unsigned>(ntrk),
	            static_cast<unsigned>(ppqn), r.b.size());

	std::size_t i = 14;
	int totalNotes = 0, totalVel20 = 0;

	for (std::uint16_t t = 0; t < ntrk; ++t) {
		if (i + 8 > r.b.size()) break;
		if (std::memcmp(&r.b[i], "MTrk", 4) != 0) {
			std::printf("\u8f68 %u \u4e0d\u662f MTrk\uff0c\u505c\u6b62\u3002\n", t);
			break;
		}
		const std::uint32_t len = r.be32(i + 4);
		const std::size_t begin = i + 8;
		const std::size_t end   = begin + len;
		if (end > r.b.size()) { std::printf("\u8f68 %u \u957f\u5ea6\u8d8a\u754c\u3002\n", t); break; }

		const Track tr = ParseTrack(r, begin, end);
		i = end;

		const std::vector<Note>* list = &tr.notes;
		std::vector<Note> filtered;
		if (wantVel >= 0) {
			for (const Note& n : tr.notes)
				if (n.vel == wantVel) filtered.push_back(n);
			list = &filtered;
		}
		totalNotes += static_cast<int>(tr.notes.size());
		for (const Note& n : tr.notes) if (n.vel == 20) ++totalVel20;

		if (wantTrack >= 0 && t != static_cast<std::uint16_t>(wantTrack)) continue;

		std::printf("\u8f68 %u\u300c%s\u300d: \u95ed\u5408\u97f3\u7b26 %zu \u4e2a\uff0c\u8f68\u672b\u4ecd\u5f00\u7740 %d \u4e2a\uff0c\u6700\u5927 tick %u\n",
		            t, tr.name.empty() ? "(no name)" : tr.name.c_str(),
		            list->size(), tr.stillOpen, tr.maxTick);

		PrintHistogram(*list, "\u529b\u5ea6\u5206\u5e03:", false);
		PrintHistogram(*list, "\u65f6\u957f(tick)\u5206\u5e03:", true);

		if (wantVel >= 0 && !list->empty()) {
			std::printf("  \u524d 20 \u6761\uff08tick, \u97f3\u9ad8, \u529b\u5ea6, \u65f6\u957f\uff09:\n");
			for (std::size_t k = 0; k < list->size() && k < 20; ++k) {
				const Note& n = (*list)[k];
				std::printf("    %8u  note %3d  vel %3d  \u65f6\u957f %u\n",
				            n.onTick, n.note, n.vel, n.offTick - n.onTick);
			}
		}

		// --dump N：按 tick 顺序原样列出前 N 个音符（调字典时看 gate 夹在哪、前后是什么）
		if (dumpN > 0) {
			std::vector<Note> sorted = tr.notes;
			std::sort(sorted.begin(), sorted.end(),
			          [](const Note& a, const Note& b) { return a.onTick < b.onTick; });
			std::printf("  \u524d %d \u4e2a\u97f3\u7b26\uff08\u6309 tick \u5347\u5e8f\uff09\uff1a\n", dumpN);
			std::printf("     %10s %6s %5s %8s %8s\n", "tick", "note", "vel", "dur", "offTick");
			for (std::size_t k = 0; k < sorted.size() && static_cast<int>(k) < dumpN; ++k) {
				const Note& n = sorted[k];
				std::printf("     %10u %6d %5d %8u %8u\n",
				            n.onTick, n.note, n.vel, n.offTick - n.onTick, n.offTick);
			}
		}
		std::printf("\n");
	}

	std::printf("\u5168\u90e8\u8f68\u5408\u8ba1\uff1a\u97f3\u7b26 %d \u4e2a\uff0c\u5176\u4e2d\u529b\u5ea6=20 \u7684 %d \u4e2a\n",
	            totalNotes, totalVel20);
	return 0;
}
