#include "Grid.h"

#include "Config.h"
#include "Link.h"
#include "ModSettings.h"

namespace cybercraft::Grid
{
	namespace
	{
		// The ground the grid was aligned to (Cyberpunk metres): a height on a block boundary at
		// any scale. Older saves kept only the offset, which was such a height too.
		double ground = 0.0;

		std::filesystem::path StatePath()
		{
			wchar_t     local[MAX_PATH]{};
			const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
			if (n == 0 || n >= MAX_PATH) {
				return {};
			}
			return std::filesystem::path(local) / L"CyberCraft" / L"cybercraft-world.ini";
		}

		// Mod Settings' slider once it has saved one, CyberCraft.ini otherwise.
		double ReadScale()
		{
			const float value = ModSettings::MetresPerBlock().value_or(
				Config::GetFloat(L"World", L"fMetresPerBlock", static_cast<float>(kDefaultMetresPerBlock)));
			return std::isfinite(value) ? std::clamp(static_cast<double>(value), 0.25, 2.0) : kDefaultMetresPerBlock;
		}

		// gGridOffset for the saved ground at the current scale: ground - offset is a whole block.
		void ApplyOffset()
		{
			const double size = MetresPerBlock();
			gGridOffset = ground - size * std::floor(ground / size);
		}
	}

	void Load()
	{
		gMetresPerBlock = ReadScale();
		const auto path = StatePath();
		if (!path.empty()) {
			wchar_t buf[64]{};
			::GetPrivateProfileStringW(L"World", L"fGridGround", L"", buf, static_cast<DWORD>(std::size(buf)), path.c_str());
			if (buf[0] == L'\0') {
				::GetPrivateProfileStringW(L"World", L"fGridOffset", L"0", buf, static_cast<DWORD>(std::size(buf)), path.c_str());
			}
			const double read = std::wcstod(buf, nullptr);
			ground = std::isfinite(read) ? read : 0.0;
		}
		ApplyOffset();
		logger::info("grid: a Minecraft block is {:.2f} m; the grid sits {:.3f} m up against Cyberpunk's heights", MetresPerBlock(), gGridOffset.load());
	}

	bool UpdateScale()
	{
		static auto next = std::chrono::steady_clock::time_point{};
		const auto  now = std::chrono::steady_clock::now();
		if (now < next) {
			return false;
		}
		next = now + 500ms;
		const double scale = ReadScale();
		const double was = MetresPerBlock();
		if (std::abs(scale - was) < 1e-4) {
			return false;
		}
		gMetresPerBlock = scale;
		ApplyOffset();
		logger::info("grid: a Minecraft block is now {:.2f} m (was {:.2f}); Minecraft's world is resent around V, placed blocks move with the scale",
			scale, was);
		return true;
	}

	double AlignTo(float a_groundCpZ)
	{
		const double before = gGridOffset.load();
		ground = a_groundCpZ;
		ApplyOffset();
		const double offset = gGridOffset.load();
		if (const auto path = StatePath(); !path.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			::WritePrivateProfileStringW(L"World", L"fGridGround", std::format(L"{:.4f}", ground).c_str(), path.c_str());
			::WritePrivateProfileStringW(L"World", L"fGridOffset", std::format(L"{:.4f}", offset).c_str(), path.c_str());
		}
		logger::info("grid: aligned to the ground at {:.3f} m: the grid now sits {:.3f} m up (moved {:+.3f} m)", ground, offset, offset - before);
		return offset - before;
	}
}
