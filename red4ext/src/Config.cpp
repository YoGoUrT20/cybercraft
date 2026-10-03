#include "Config.h"

namespace cybercraft::Config
{
	namespace
	{
		std::filesystem::path pluginDir;
		std::filesystem::path gameDir;
	}

	void Init()
	{
		HMODULE self = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Init), &self);
		wchar_t path[MAX_PATH * 2]{};
		::GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
		pluginDir = std::filesystem::path(path).parent_path();
		// <game>/red4ext/plugins/CyberCraft -> <game>
		gameDir = pluginDir.parent_path().parent_path().parent_path();
	}

	const std::filesystem::path& PluginDir() { return pluginDir; }
	const std::filesystem::path& GameDir() { return gameDir; }
	std::filesystem::path        IniPath() { return pluginDir / L"CyberCraft.ini"; }

	std::wstring GetString(const wchar_t* a_section, const wchar_t* a_key, const wchar_t* a_default)
	{
		wchar_t    buf[1024]{};
		const auto ini = IniPath();
		::GetPrivateProfileStringW(a_section, a_key, a_default, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
		// Inline comments (";" after the value) aren't stripped by the Win32 reader.
		std::wstring value = buf;
		if (const auto semi = value.find(L';'); semi != std::wstring::npos) {
			value.resize(semi);
		}
		while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
			value.pop_back();
		}
		return value;
	}

	bool GetBool(const wchar_t* a_section, const wchar_t* a_key, bool a_default)
	{
		const auto v = GetString(a_section, a_key, a_default ? L"1" : L"0");
		return !(v == L"0" || _wcsicmp(v.c_str(), L"false") == 0 || v.empty());
	}

	float GetFloat(const wchar_t* a_section, const wchar_t* a_key, float a_default)
	{
		const auto v = GetString(a_section, a_key, L"");
		if (v.empty()) {
			return a_default;
		}
		try {
			return std::stof(v);
		} catch (...) {
			return a_default;
		}
	}

	bool Diagnostics()
	{
		static const bool on = GetBool(L"Debug", L"bDiagnostics", false);
		return on;
	}
}
