#include "PPQN_Reader.h"
#include "File_Open.h"          // FS::OpenFile：按 UTF-8 路径打开文件
#include <fstream>
#include <cstdint>

namespace {
	/// 读满 14 字节的 MThd 头。成功返回 true。
	bool loadHead(const std::string& path, std::uint8_t head[14])
	{
		std::ifstream f = FS::OpenFile(path);
		if (!f)
			return false;
		f.read(reinterpret_cast<char*>(head), 14);
		return f.gcount() == 14;
	}
}

namespace PPQN {

	Result PR(const std::string& path)
	{
		Result r;

		std::ifstream probe = FS::OpenFile(path);
		if (!probe) {
			r.message = "打不开捏，文件可能被占用或已被删除。";
			return r;
		}

		std::uint8_t head[14];
		if (!loadHead(path, head)) {
			r.message = "没有读取到 PPQN 捏。";
			return r;
		}

		// MThd 布局：偏移 12、13 = division（大端，高字节在前）
		const int division =
			(static_cast<int>(head[12]) << 8) | static_cast<int>(head[13]);

		// bit15 = 1 表示 SMPTE 计时，没有 PPQN 这个量
		if (division & 0x8000) {
			r.message = "没有读取到 PPQN 捏。";
			return r;
		}

		r.ok = true;
		r.ppqn = division & 0x7FFF;
		r.message = "PPQN : " + std::to_string(r.ppqn);
		if (r.ppqn < 96)
			r.message += "\n（PPQN 太小，请设置 PPQN≥96 捏。）";

		return r;
	}

}
