#pragma once

// Where things live, and CyberCraft.ini (next to the plugin DLL: red4ext/plugins/CyberCraft/).
namespace cybercraft::Config
{
	// Resolves the plugin and game folders. Call first, from the plugin's Load.
	void Init();

	const std::filesystem::path& PluginDir();  // <game>/red4ext/plugins/CyberCraft
	const std::filesystem::path& GameDir();    // <game>
	std::filesystem::path        IniPath();

	bool         GetBool(const wchar_t* a_section, const wchar_t* a_key, bool a_default);
	float        GetFloat(const wchar_t* a_section, const wchar_t* a_key, float a_default);
	std::wstring GetString(const wchar_t* a_section, const wchar_t* a_key, const wchar_t* a_default);

	// [Debug] bDiagnostics: detailed per-frame logs (several lines a second). Off by default.
	bool Diagnostics();
}
