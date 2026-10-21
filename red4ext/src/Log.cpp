#include "Log.h"

#include <Windows.h>

#include <cstdio>
#include <algorithm>
#include <mutex>
#include <vector>

namespace cybercraft::log
{
	namespace
	{
		std::mutex lock;
		std::FILE* file = nullptr;
	}

	// CyberCraft-<date>-<time>.log in a_dir, like RED4ext's own and Codeware's: a new file each start
	// (the log of a start that went wrong isn't overwritten by the next), the newest few kept.
	void Init(const std::filesystem::path& a_dir)
	{
		std::lock_guard guard(lock);
		if (file) {
			return;
		}
		std::error_code ec;
		std::filesystem::create_directories(a_dir, ec);
		std::vector<std::filesystem::path> old;
		for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
			const auto name = entry.path().filename().wstring();
			if (name.starts_with(L"CyberCraft-") && entry.path().extension() == L".log") {
				old.push_back(entry.path());
			}
		}
		std::ranges::sort(old);
		for (std::size_t i = 0; i + 4 < old.size(); ++i) {
			std::filesystem::remove(old[i], ec);
		}
		std::filesystem::remove(a_dir / L"CyberCraft.log", ec);  // releases before 0.2.8 wrote this one
		SYSTEMTIME t;
		::GetLocalTime(&t);
		const auto name = std::format(L"CyberCraft-{:04}-{:02}-{:02}-{:02}-{:02}-{:02}.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		file = ::_wfsopen((a_dir / name).c_str(), L"w", _SH_DENYWR);
	}

	void Write(std::string_view a_level, std::string_view a_message)
	{
		SYSTEMTIME t;
		::GetLocalTime(&t);
		const auto line = std::format("[{:02}:{:02}:{:02}.{:03}] [{}] {}\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, a_level, a_message);
		std::lock_guard guard(lock);
		if (file) {
			std::fwrite(line.data(), 1, line.size(), file);
			std::fflush(file);
		}
	}
}
