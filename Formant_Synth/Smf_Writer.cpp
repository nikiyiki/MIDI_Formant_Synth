#include "Synth.h"
#include "PPQN_Reader/File_Open.h"   // FS::OpenFileWrite
#include <fstream>
#include <cstdint>
#include <algorithm>

// ─────────────────────────────────────────────────────────────
//  Smf_Writer —— 纯序列化器：事件列表 → SMF 字节
//  职责边界：只认「绝对 tick 的 (channel, status, d1, d2)」与 meta 字节，
//            **不认识**音素、共振峰、通道角色。
// ─────────────────────────────────────────────────────────────

namespace SYNTH {

	/// 一条已排好序的轨事件（SMF 内部用：带 delta）
	struct TrackItem {
		std::uint32_t delta = 0;
		std::vector<std::uint8_t> bytes;   ///< 完整事件字节（含 status）
	};

	/// 写一个 MIDI 可变长量（VLQ）
	static void PushVlq(std::vector<std::uint8_t>& out, std::uint32_t v)
	{
		std::uint8_t buf[5];
		int n = 0;
		buf[n++] = static_cast<std::uint8_t>(v & 0x7F);
		v >>= 7;
		while (v > 0) {
			buf[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
			v >>= 7;
		}
		for (int i = n - 1; i >= 0; --i)
			out.push_back(buf[i]);
	}

	/// meta 事件编码：FF type len payload
	static std::vector<std::uint8_t> MetaOf(std::uint8_t type,
	                                       const std::vector<std::uint8_t>& payload)
	{
		std::vector<std::uint8_t> b;
		b.push_back(0xFF);
		b.push_back(type);
		PushVlq(b, static_cast<std::uint32_t>(payload.size()));
		b.insert(b.end(), payload.begin(), payload.end());
		return b;
	}

	/// 组装一条 MTrk（自动补 end_of_track）
	static std::vector<std::uint8_t> AssembleTrack(std::vector<TrackItem> items)
	{
		std::vector<std::uint8_t> body;
		for (const TrackItem& it : items) {
			PushVlq(body, it.delta);
			body.insert(body.end(), it.bytes.begin(), it.bytes.end());
		}
		PushVlq(body, 0);
		body.push_back(0xFF);
		body.push_back(0x2F);
		body.push_back(0x00);

		std::vector<std::uint8_t> trk;
		const char id[4] = { 'M', 'T', 'r', 'k' };
		trk.insert(trk.end(), id, id + 4);
		const std::uint32_t len = static_cast<std::uint32_t>(body.size());
		trk.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
		trk.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
		trk.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
		trk.push_back(static_cast<std::uint8_t>(len & 0xFF));
		trk.insert(trk.end(), body.begin(), body.end());
		return trk;
	}

	/// 一条 Voice 轨：track_name 打头（协议 7.5），
	/// 其余按 (tick, priority) 排好 → 带 delta
	static std::vector<std::uint8_t> MakeVoiceTrack(const std::vector<Event>& ev,
	                                                const char* name)
	{
		std::vector<TrackItem> items;

		// 协议 7.5：tick 0 的第一条必须是 track_name（在 CC/PC/RPN/CC7/CC11 之前）
		if (name && name[0] != '\0') {
			TrackItem nm;
			nm.delta = 0;
			nm.bytes = MetaOf(0x03, std::vector<std::uint8_t>(name, name + std::strlen(name)));
			items.push_back(std::move(nm));
		}

		std::vector<const Event*> p;
		p.reserve(ev.size());
		for (const Event& e : ev)
			p.push_back(&e);

		std::stable_sort(p.begin(), p.end(), [](const Event* a, const Event* b) {
			if (a->tick != b->tick)
				return a->tick < b->tick;
			return a->priority < b->priority;   // 同 tick：note_off→RPN→ctrl→note_on
		});

		items.reserve(items.size() + p.size());
		std::uint32_t last = 0;
		for (const Event* e : p) {
			TrackItem it;
			it.delta = e->tick - last;
			last = e->tick;
			it.bytes.push_back(e->status);
			it.bytes.push_back(e->data1);
			const std::uint8_t kind = e->status & 0xF0;
			if (kind != 0xC0 && kind != 0xD0)
				it.bytes.push_back(e->data2);
			items.push_back(std::move(it));
		}
		return AssembleTrack(std::move(items));
	}

	bool WriteSmf(const std::string& path,
	              unsigned ppqn,
	              const std::vector<MetaEvent>& setupMeta,
	              const std::vector<Event>& voiceEvents,
	              std::string& err)
	{
		// ── Setup 轨：track_name → meta（按 (tick, kind) 排序）──
		std::vector<TrackItem> setup;
		{
			TrackItem it;
			it.delta = 0;
			it.bytes = MetaOf(0x03, std::vector<std::uint8_t>{ 'S', 'e', 't', 'u', 'p' });
			setup.push_back(std::move(it));
		}
		{
			std::vector<const MetaEvent*> ms;
			ms.reserve(setupMeta.size());
			for (const MetaEvent& m : setupMeta)
				ms.push_back(&m);
			std::stable_sort(ms.begin(), ms.end(), [](const MetaEvent* a, const MetaEvent* b) {
				if (a->tick != b->tick)
					return a->tick < b->tick;
				return a->kind < b->kind;        // 同 tick：(tick, kind)
			});
			std::uint32_t last = 0;
			for (const MetaEvent* m : ms) {
				TrackItem mi;
				mi.delta = m->tick - last;
				last = m->tick;
				mi.bytes = m->bytes;
				setup.push_back(std::move(mi));
			}
		}

		std::vector<std::vector<std::uint8_t>> tracks;
		tracks.push_back(AssembleTrack(std::move(setup)));

		for (int ch = 0; ch < CH_COUNT; ++ch) {
			std::vector<Event> ev;
			for (const Event& e : voiceEvents)
				if (e.channel == ch)
					ev.push_back(e);
			tracks.push_back(MakeVoiceTrack(ev, ChannelName(ch)));
		}

		// ── 文件头 + 落盘 ──
		std::vector<std::uint8_t> out;
		{
			const char id[4] = { 'M', 'T', 'h', 'd' };
			out.insert(out.end(), id, id + 4);
			out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(6);
			out.push_back(0); out.push_back(1);                 // format 1
			const std::uint16_t nt = static_cast<std::uint16_t>(tracks.size());
			out.push_back(static_cast<std::uint8_t>((nt >> 8) & 0xFF));
			out.push_back(static_cast<std::uint8_t>(nt & 0xFF));
			out.push_back(static_cast<std::uint8_t>((ppqn >> 8) & 0xFF));
			out.push_back(static_cast<std::uint8_t>(ppqn & 0xFF));
		}
		for (const std::vector<std::uint8_t>& t : tracks)
			out.insert(out.end(), t.begin(), t.end());

		std::ofstream f = FS::OpenFileWrite(path);
		if (!f) {
			err = "写不出文件捏，输出路径不可写。";
			return false;
		}
		f.write(reinterpret_cast<const char*>(out.data()),
		        static_cast<std::streamsize>(out.size()));
		if (!f) {
			err = "写文件时出错（磁盘满或被占用？）。";
			return false;
		}
		return true;
	}

}
