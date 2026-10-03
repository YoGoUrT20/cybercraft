#include "ModSettings.h"

#include "Config.h"
#include "Link.h"
#include "Rtti.h"

namespace cybercraft::ModSettings
{
	namespace
	{
		// Mod Settings' user.ini: a section per settings class, a key per variable, written when
		// its menu's changes are accepted. Missing (no Mod Settings, never saved, or caught while
		// being written): no answer.
		std::wstring ReadValue(const wchar_t* a_key)
		{
			static const std::wstring path = (Config::GameDir() / L"red4ext" / L"plugins" / L"mod_settings" / L"user.ini").wstring();
			wchar_t value[32]{};
			::GetPrivateProfileStringW(L"CyberCraftModSettings", a_key, L"", value, static_cast<DWORD>(std::size(value)), path.c_str());
			return value;
		}

		std::optional<bool> ReadEnabled()
		{
			const auto  text = ReadValue(L"cyberCraftEnabled");
			const auto* value = text.c_str();
			if (::_wcsicmp(value, L"false") == 0 || std::wcscmp(value, L"0") == 0) {
				return false;
			}
			if (::_wcsicmp(value, L"true") == 0 || std::wcscmp(value, L"1") == 0) {
				return true;
			}
			return std::nullopt;
		}
	}

	bool Enabled()
	{
		static bool enabled = ReadEnabled().value_or(true);
		static auto next = std::chrono::steady_clock::now() + 500ms;
		if (const auto now = std::chrono::steady_clock::now(); now >= next) {
			next = now + 500ms;
			if (const auto read = ReadEnabled()) {
				enabled = *read;
			}
		}
		return enabled;
	}

	std::optional<float> MetresPerBlock()
	{
		// Saved as "0.750000".
		const auto text = ReadValue(L"cyberCraftMetresPerBlock");
		wchar_t*   end = nullptr;
		const auto value = std::wcstof(text.c_str(), &end);
		if (text.empty() || end == text.c_str() || !std::isfinite(value) || value <= 0.0f) {
			return std::nullopt;
		}
		return value;
	}

	void Update(bool a_canShow)
	{
		// A global script function (the plugin registers no natives of its own), found the first
		// time. Without the script it's missing, and the Mod Settings row doesn't exist either.
		static RED4ext::CBaseFunction* const take = [] {
			auto* found = rtti::FindGlobalFunction("CyberCraftTakeMinecraftSettingsRequest", {}, "Bool");
			if (!found) {
				logger::warn("mod settings: CyberCraftTakeMinecraftSettingsRequest isn't in the scripts; no Minecraft settings from Mod Settings");
			}
			return found;
		}();
		if (!take) {
			return;
		}
		bool                 asked = false;
		RED4ext::StackArgs_t args;
		if (!RED4ext::ExecuteFunction(static_cast<void*>(nullptr), take, &asked, args) || !asked) {
			return;
		}
		if (a_canShow) {
			Link::Get().PushInput(proto::kInOpenMenu, proto::kMenuOptions);
			logger::info("mod settings: opening Minecraft's options");
		} else {
			logger::info("mod settings: Minecraft's options asked for while Minecraft isn't driving V; dropped");
		}
	}
}
