#pragma once

#include <filesystem>
#include <format>
#include <string_view>

// A small thread-safe file log (red4ext/logs/CyberCraft.log), flushed on every line so it survives
// a crash. `logger::info(...)` takes std::format arguments.
namespace cybercraft::log
{
	void Init(const std::filesystem::path& a_file);
	void Write(std::string_view a_level, std::string_view a_message);

	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		Write("info", std::format(a_fmt, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		Write("warn", std::format(a_fmt, std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		Write("error", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
}

namespace logger = cybercraft::log;
