#include "Log.h"

#include <Windows.h>

#include <cstdio>
#include <mutex>

namespace cybercraft::log
{
	namespace
	{
		std::mutex lock;
		std::FILE* file = nullptr;
	}

	void Init(const std::filesystem::path& a_file)
	{
		std::lock_guard guard(lock);
		if (file) {
			return;
		}
		std::error_code ec;
		std::filesystem::create_directories(a_file.parent_path(), ec);
		file = ::_wfsopen(a_file.c_str(), L"w", _SH_DENYWR);
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
