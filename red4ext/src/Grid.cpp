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

		double ReadSaved(const std::filesystem::path& a_path, const wchar_t* a_key, const wchar_t* a_default)
		{
			wchar_t buf[64]{};
			::GetPrivateProfileStringW(L"World", a_key, a_default, buf, static_cast<DWORD>(std::size(buf)), a_path.c_str());
			const double read = std::wcstod(buf, nullptr);
			return std::isfinite(read) ? read : 0.0;
		}

		void Save(const wchar_t* a_key, double a_value)
		{
			if (const auto path = StatePath(); !path.empty()) {
				std::error_code ec;
				std::filesystem::create_directories(path.parent_path(), ec);
				::WritePrivateProfileStringW(L"World", a_key, std::format(L"{:.4f}", a_value).c_str(), path.c_str());
			}
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
			ground = buf[0] != L'\0' ? ReadSaved(path, L"fGridGround", L"0") : ReadSaved(path, L"fGridOffset", L"0");
			gGridYaw = ReadSaved(path, L"fGridYaw", L"0");
			gGridShiftX = ReadSaved(path, L"fGridShiftX", L"0");
			gGridShiftY = ReadSaved(path, L"fGridShiftY", L"0");
		}
		ApplyOffset();
		logger::info("grid: a Minecraft block is {:.2f} m; the grid sits {:.3f} m up against Cyberpunk's heights, turned {:.1f} degrees", MetresPerBlock(),
			gGridOffset.load(), gGridYaw.load());
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
		Save(L"fGridGround", ground);
		Save(L"fGridOffset", offset);
		logger::info("grid: aligned to the ground at {:.3f} m: the grid now sits {:.3f} m up (moved {:+.3f} m)", ground, offset, offset - before);
		return offset - before;
	}

	double TurnTo(float a_cpX, float a_cpY, float a_cpYaw)
	{
		// The grid looks the same turned a quarter turn, so of the four headings along a_cpYaw the
		// nearest one: the blocks round V turn as little as they can.
		const double before = gGridYaw.load();
		const double turn = std::remainder(double(a_cpYaw) - before, 90.0);
		// Where the point is on the grid now, before the turn; the shift puts it there again after.
		const auto [wasX, wasY] = CpToGrid(a_cpX, a_cpY);
		const double heldX = wasX - gGridShiftX.load(), heldY = wasY - gGridShiftY.load();
		gGridYaw = std::remainder(before + turn, 360.0);
		const auto [nowX, nowY] = CpToGrid(a_cpX, a_cpY);
		gGridShiftX = nowX - heldX;
		gGridShiftY = nowY - heldY;
		Save(L"fGridYaw", gGridYaw.load());
		Save(L"fGridShiftX", gGridShiftX.load());
		Save(L"fGridShiftY", gGridShiftY.load());
		logger::info("grid: turned {:+.1f} degrees about Cyberpunk ({:.2f}, {:.2f}): its rows now run at {:.1f} degrees", turn, a_cpX, a_cpY, gGridYaw.load());
		return turn;
	}

	void Nudge(double a_cpDx, double a_cpDy)
	{
		// A point at grid g is at Cyberpunk GridToCp(g + shift): the shift moves by the nudge turned
		// onto the grid's axes.
		const auto [dx, dy] = CpToGrid(a_cpDx, a_cpDy);
		gGridShiftX = gGridShiftX.load() + dx;
		gGridShiftY = gGridShiftY.load() + dy;
		Save(L"fGridShiftX", gGridShiftX.load());
		Save(L"fGridShiftY", gGridShiftY.load());
		if (Config::Diagnostics()) {
			logger::info("grid: nudged ({:+.3f}, {:+.3f}) m in Cyberpunk x, y", a_cpDx, a_cpDy);
		}
	}
}
