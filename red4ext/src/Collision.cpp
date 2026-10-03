#include "Collision.h"

#include <limits>

#include <RED4ext/Scripting/Natives/physicsTraceResult.hpp>

#include "Config.h"
#include "Rtti.h"

namespace cybercraft
{
	namespace
	{
		constexpr double kEps = 1.0e-4;

		std::int32_t FloorDiv(std::int32_t a_value, std::int32_t a_divisor)
		{
			const auto q = a_value / a_divisor;
			return (a_value % a_divisor < 0) ? q - 1 : q;
		}

		std::int32_t FloorI(double a_value) { return static_cast<std::int32_t>(std::floor(a_value)); }

		std::uint64_t RegionKey(std::int32_t a_rx, std::int32_t a_ry, std::int32_t a_rz)
		{
			constexpr std::uint64_t mask = (1ull << 21) - 1;
			return ((static_cast<std::uint64_t>(a_rx) & mask) << 42) | ((static_cast<std::uint64_t>(a_ry) & mask) << 21) |
			       (static_cast<std::uint64_t>(a_rz) & mask);
		}

		// A RegionKey field (21 bits) back to its signed coordinate.
		std::int32_t Unpack21(std::uint64_t a_bits)
		{
			return static_cast<std::int32_t>(static_cast<std::int64_t>(a_bits << 43) >> 43);
		}

		std::uint64_t BlockKey(std::int32_t a_bx, std::int32_t a_by, std::int32_t a_bz)
		{
			return RegionKey(a_bx, a_by, a_bz);  // same packing, a block per unit
		}

		// Where a down ray that started inside a solid tries next, further each time it's still inside.
		constexpr std::array<double, 4> kInsideSteps{ 0.125, 0.25, 0.5, 1.0 };
		// How many of those a column may take on top of its surfaces (the whole 48-block window a block
		// at a time), and how deep one solid may go before the column gives up on it: one that thick is
		// the ground or a building's bulk, with nothing to stand on inside or under it in the window.
		constexpr int    kMaxInsideSteps = 48;
		constexpr double kMaxInsideDepth = 8.0;

		std::string Ascii(const std::wstring& a_wide)
		{
			std::string out;
			for (const auto wide : a_wide) {  // collision group names are ASCII
				out.push_back(wide < 128 ? static_cast<char>(wide) : '?');
			}
			return out;
		}

		// "Static,Terrain" -> the group names, trimmed.
		std::vector<RED4ext::CName> GroupList(std::string_view a_list)
		{
			std::vector<RED4ext::CName> names;
			while (!a_list.empty()) {
				const auto comma = a_list.find(',');
				auto       name = a_list.substr(0, comma);
				while (!name.empty() && name.front() == ' ') {
					name.remove_prefix(1);
				}
				while (!name.empty() && name.back() == ' ') {
					name.remove_suffix(1);
				}
				if (!name.empty()) {
					names.emplace_back(std::string(name).c_str());
				}
				a_list = comma == std::string_view::npos ? std::string_view{} : a_list.substr(comma + 1);
			}
			return names;
		}

		bool IsWater(RED4ext::CName a_material)
		{
			const auto* name = a_material.ToString();
			if (!name) {
				return false;
			}
			// Cyberpunk's surface material names are things like "water", "liquid_water", "water_deep".
			std::string_view view(name);
			return view.find("water") != std::string_view::npos || view.find("liquid") != std::string_view::npos;
		}
	}

	Collision& Collision::Get()
	{
		static Collision instance;
		return instance;
	}

	const Collision::Settings& Collision::Config()
	{
		static const Settings settings = [] {
			using namespace cybercraft::Config;
			Settings s{};
			s.enable = GetBool(L"Collision", L"bEnable", true);
			s.group = Ascii(GetString(L"Collision", L"sGroup", L"Static,Terrain"));
			s.radius = GetFloat(L"Collision", L"fRadius", 32.0f);
			s.up = GetFloat(L"Collision", L"fUp", 24.0f);
			s.down = GetFloat(L"Collision", L"fDown", 24.0f);
			s.samplesPerBlock = std::clamp(static_cast<int>(GetFloat(L"Collision", L"iSamplesPerBlock", 2.0f)), 1, 8);
			s.rayBudget = std::max(static_cast<int>(GetFloat(L"Collision", L"iRayBudget", 2000.0f)), 1);
			// 6 used to be the default: in a building, or under balconies, signs and overpasses, a
			// column ran out on what was above V and never reached the floor she stood on.
			s.maxHitsPerColumn = std::clamp(static_cast<int>(GetFloat(L"Collision", L"iMaxHitsPerColumn", 16.0f)), 1, 64);
			s.steepNormalY = GetFloat(L"Collision", L"fSteepNormalY", 0.64f);
			s.wallHeight = GetFloat(L"Collision", L"fWallHeight", 2.5f);
			s.rebuildDist = GetFloat(L"Collision", L"fRebuildDist", 4.0f);
			s.guard = GetBool(L"Collision", L"bGuard", true);
			s.guardDepth = std::clamp(GetFloat(L"Collision", L"fGuardDepth", 8.0f), 1.0f, 24.0f);
			s.vehicles = GetBool(L"Collision", L"bVehicles", true);
			s.vehicleGroup = Ascii(GetString(L"Collision", L"sVehicleGroup", L"Vehicle"));
			s.vehicleRadius = std::clamp(GetFloat(L"Collision", L"fVehicleRadius", 6.0f), 1.0f, 16.0f);
			s.vehicleFrames = std::max(static_cast<int>(GetFloat(L"Collision", L"iVehicleFrames", 2.0f)), 1);
			logger::info("collision: {} group \"{}\" radius {:.0f} window -{:.0f}/+{:.0f} samples {}/block budget {} rays/frame, {} surfaces per column",
				s.enable ? "on" : "off", s.group, s.radius, s.down, s.up, s.samplesPerBlock, s.rayBudget, s.maxHitsPerColumn);
			logger::info("collision: guard {} (the ground down to {:.0f} blocks under the player, every frame)", s.guard ? "on" : "off", s.guardDepth);
			logger::info("collision: cars {} (group \"{}\", {:.0f} blocks round the player, every {} frames)", s.vehicles ? "on" : "off",
				s.vehicleGroup, s.vehicleRadius, s.vehicleFrames);
			return s;
		}();
		return settings;
	}

	void Collision::Reset(std::uint32_t a_epoch)
	{
		epoch_ = a_epoch;
		cleared_ = false;
		sweeping_ = false;
		pending_.clear();
		regions_.clear();
		sentHash_.clear();
		sent_.clear();
		guard_.clear();
		hits_ = 0;
		// kColClear drops Minecraft's car layer with the rest.
		vehicleBlocks_.clear();
		vehicleHash_.reset();
	}

	int Collision::Samples() const
	{
		const auto edge = kRegionSize * Config().samplesPerBlock;
		return edge * edge;
	}

	void Collision::Update(const McVec& a_playerMc)
	{
		const auto& cfg = Config();
		if (!cfg.enable) {
			return;
		}
		auto& link = Link::Get();
		if (!link.Valid() || !link.McAlive()) {
			return;
		}
		if (!cleared_) {
			if (!link.WriteCollision(proto::kColClear, &epoch_, sizeof(epoch_))) {
				return;  // ring full: try again next frame, before any region goes out
			}
			cleared_ = true;
		}

		feetY_ = a_playerMc.y;
		Rebuild(a_playerMc);
		Guard(a_playerMc);

		PublishWater();

		const auto total = Samples();
		auto       budget = cfg.rayBudget;
		while (budget > 0 && !pending_.empty()) {
			auto& column = pending_.back();
			budget -= Sample(column, budget);
			if (column.next >= total) {
				Flush(column);
				pending_.pop_back();
			}
		}
	}

	void Collision::Rebuild(const McVec& a_playerMc)
	{
		const auto& cfg = Config();
		const auto  dx = a_playerMc.x - origin_.x;
		const auto  dy = a_playerMc.y - origin_.y;
		const auto  dz = a_playerMc.z - origin_.z;
		// Up and down the window has room to spare (fUp / fDown): a fall restarting the sweep every
		// 4 blocks left it no time to finish the pillar under the player.
		const auto  vertical = std::max(static_cast<double>(cfg.rebuildDist), std::min(cfg.up, cfg.down) / 3.0);
		const bool  moved = (dx * dx + dz * dz) > static_cast<double>(cfg.rebuildDist) * cfg.rebuildDist || std::abs(dy) > vertical;
		if (sweeping_ && !moved && !pending_.empty()) {
			return;
		}

		origin_ = a_playerMc;
		sweeping_ = true;
		if (sentHash_.size() > 20000) {
			sentHash_.clear();  // roamed far enough that remembering old regions costs more than resending
		}
		// What's kept per region for the guard only matters round the player.
		const auto farSq = (cfg.radius + 2.0 * kRegionSize) * (cfg.radius + 2.0 * kRegionSize);
		const auto distant = [&](const auto& a_entry) {
			const auto cx = Unpack21(a_entry.first >> 42) * kRegionSize + kRegionSize * 0.5 - a_playerMc.x;
			const auto cz = Unpack21(a_entry.first) * kRegionSize + kRegionSize * 0.5 - a_playerMc.z;
			return cx * cx + cz * cz > farSq;
		};
		std::erase_if(sent_, distant);
		std::erase_if(guard_, distant);
		regions_.clear();  // anything half-scanned is dropped rather than sent
		pending_.clear();
		water_.originX = FloorI(a_playerMc.x) - int(proto::kWaterGridSize) / 2;
		water_.originZ = FloorI(a_playerMc.z) - int(proto::kWaterGridSize) / 2;
		std::fill(std::begin(water_.surface), std::end(water_.surface), proto::kNoWater);
		waterDirty_ = true;
		yMin_ = FloorI(a_playerMc.y - cfg.down);
		yMax_ = FloorI(a_playerMc.y + cfg.up);

		const auto rxLo = FloorDiv(FloorI(a_playerMc.x - cfg.radius), kRegionSize);
		const auto rxHi = FloorDiv(FloorI(a_playerMc.x + cfg.radius), kRegionSize);
		const auto rzLo = FloorDiv(FloorI(a_playerMc.z - cfg.radius), kRegionSize);
		const auto rzHi = FloorDiv(FloorI(a_playerMc.z + cfg.radius), kRegionSize);
		for (auto rx = rxLo; rx <= rxHi; ++rx) {
			for (auto rz = rzLo; rz <= rzHi; ++rz) {
				pending_.push_back(Column{ rx, rz, 0 });
			}
		}
		// Nearest pillar last: the scan pops from the back, so the ground under the player
		// reaches Minecraft first.
		const auto distanceSq = [&](const Column& a_column) {
			const auto cx = (a_column.rx * kRegionSize) + kRegionSize * 0.5 - a_playerMc.x;
			const auto cz = (a_column.rz * kRegionSize) + kRegionSize * 0.5 - a_playerMc.z;
			return cx * cx + cz * cz;
		};
		std::sort(pending_.begin(), pending_.end(), [&](const Column& a_lhs, const Column& a_rhs) {
			return distanceSq(a_lhs) > distanceSq(a_rhs);
		});
		if (cybercraft::Config::Diagnostics()) {
			logger::info("collision: sweep from ({:.1f}, {:.1f}, {:.1f}), {} pillars, y {}..{} (since last: {} rays, {} hits, {} through CyberCraft's colliders, "
						 "{} started inside a solid, {} columns out of surfaces, {} regions the guard filled in under the player)",
				a_playerMc.x, a_playerMc.y, a_playerMc.z, pending_.size(), yMin_, yMax_, sweepRays_, sweepHits_, sweepOwn_, sweepInside_, sweepCapped_,
				guardFills_);
		}
		sweepRays_ = 0;
		sweepHits_ = 0;
		sweepOwn_ = 0;
		sweepInside_ = 0;
		sweepCapped_ = 0;
		guardFills_ = 0;
	}

	int Collision::Sample(Column& a_column, int a_budget)
	{
		const auto spb = Config().samplesPerBlock;
		const auto edge = kRegionSize * spb;
		const auto step = 1.0 / spb;
		const auto total = edge * edge;

		// A column once started is finished, past the budget if need be: the one the budget ran out
		// in used to be left with only its first few surfaces, often not down to the floor.
		int rays = 0;
		while (a_column.next < total && rays < a_budget) {
			const auto index = a_column.next++;
			rays += ScanColumn(a_column.rx * kRegionSize + (index % edge + 0.5) * step, a_column.rz * kRegionSize + (index / edge + 0.5) * step);
		}
		return rays;
	}

	int Collision::ScanColumn(double a_mcX, double a_mcZ)
	{
		const auto& cfg = Config();
		const auto  step = 1.0 / cfg.samplesPerBlock;
		const auto  top = yMax_ + 1.0;
		const auto  bottom = static_cast<double>(yMin_);
		const auto  x = a_mcX;
		const auto  z = a_mcZ;

		int              rays = 0;
		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};

		// Downward multi-hit: every surface in the column, so overhangs and the floors above
		// the player are solid too.
		double groundY = std::numeric_limits<double>::quiet_NaN();
		double from = top;
		int    surfaces = 0;
		int    insideSteps = 0;
		int    insideRun = 0;  // starts inside a solid in a row
		double insideDepth = 0.0;
		while (from > bottom && surfaces < cfg.maxHitsPerColumn && insideSteps < kMaxInsideSteps) {
			++rays;
			bool inside = false;
			if (!Raycast({ x, from, z }, { x, bottom, z }, hit, normal, material, &inside)) {
				break;
			}
			if (inside) {
				// The ray began inside a solid (a box, a convex hull: the next start under a hit is
				// inside whatever was hit, if it's thick). That says nothing about where the solid
				// ends, and taking it as a surface (or a miss) used to end the column there, never
				// reaching the floor under a roof or a balcony: step on down through it instead.
				++sweepInside_;
				++insideSteps;
				const auto down = kInsideSteps[std::min<std::size_t>(insideRun++, kInsideSteps.size() - 1)];
				from -= down;
				insideDepth += down;
				if (insideDepth > kMaxInsideDepth) {
					break;
				}
				continue;
			}
			insideRun = 0;
			insideDepth = 0.0;
			++surfaces;
			const auto surfaceY = static_cast<double>(hit.Y);
			if (IsWater(material)) {
				// Water is not collision: Minecraft wants its surface height so its own
				// swimming physics can take over.
				RecordWater(x, z, surfaceY);
				from = surfaceY - 0.05;
				continue;
			}
			MarkSurface(regions_, x, z, surfaceY, normal);
			if (std::abs(normal.Y) < cfg.steepNormalY) {
				// Too steep to walk: raise a wall column so Minecraft's step-up refuses it
				// (DESIGN.md §5.1). Down rays only graze such faces, so this is a backstop;
				// the horizontal probes below are what actually find walls.
				MarkSpan(regions_, x, z, surfaceY, surfaceY + cfg.wallHeight);
			}
			// The walls that matter are at the height the player is at now, not where the sweep
			// started.
			if (std::isnan(groundY) && surfaceY <= feetY_ + 2.0) {
				groundY = surfaceY;
			}
			from = surfaceY - 0.05;
		}
		if (surfaces >= cfg.maxHitsPerColumn) {
			++sweepCapped_;
		}
		if (std::isnan(groundY)) {
			return rays;  // nothing underfoot here: no height to hang the wall probes off
		}

		// Walls: short probes at knee, waist and head height. A hit marks a 3/4-block band, so
		// consecutive heights join into one continuous surface.
		for (const double height : { 0.25, 1.0, 1.75 }) {
			const auto y = groundY + height;
			if (y < bottom || y > top) {
				continue;
			}
			for (int axis = 0; axis < 2; ++axis) {
				++rays;
				const McVec to{ x + (axis == 0 ? step : 0.0), y, z + (axis == 1 ? step : 0.0) };
				if (Raycast({ x, y, z }, to, hit, normal, material)) {
					MarkWall(axis, axis == 0 ? z : x, hit, normal, y - 0.375, y + 0.375);
				}
			}
		}
		return rays;
	}

	void Collision::Guard(const McVec& a_playerMc)
	{
		const auto& cfg = Config();
		if (!cfg.guard) {
			return;
		}
		// Every sample cell under Minecraft's player (0.6 blocks wide) and half a block round it, for
		// where it will be by the next frame. The same cells the sweep samples, so a surface lands on
		// the same sub-voxels either way.
		constexpr double kReach = 0.3 + 0.5;
		const auto       spb = cfg.samplesPerBlock;
		const auto       step = 1.0 / spb;
		const auto       sx0 = FloorI((a_playerMc.x - kReach) * spb), sx1 = FloorI((a_playerMc.x + kReach) * spb);
		const auto       sz0 = FloorI((a_playerMc.z - kReach) * spb), sz1 = FloorI((a_playerMc.z + kReach) * spb);

		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};
		guardScratch_.clear();
		for (auto sx = sx0; sx <= sx1; ++sx) {
			for (auto sz = sz0; sz <= sz1; ++sz) {
				const auto x = (sx + 0.5) * step;
				const auto z = (sz + 0.5) * step;
				// From half a block up, inside the player's body where nothing solid is, down to the
				// first surface under the feet: what the player stands on, or lands on next.
				bool inside = false;
				if (!Raycast({ x, a_playerMc.y + 0.5, z }, { x, a_playerMc.y - cfg.guardDepth, z }, hit, normal, material, &inside) || inside ||
					IsWater(material) || std::abs(normal.Y) < 0.2f) {
					continue;  // a cell round the player can start in a wall; a grazed wall isn't ground
				}
				MarkSurface(guardScratch_, x, z, hit.Y, normal);
			}
		}

		for (const auto& [key, region] : guardScratch_) {
			auto&      kept = guard_[key];
			const auto swept = sent_.find(key);
			bool       fresh = false;  // ground Minecraft doesn't have yet
			for (const auto& [local, mask] : region.blocks) {
				auto&       into = kept.blocks[local];
				const Mask* sweep = nullptr;
				if (swept != sent_.end()) {
					const auto block = swept->second.blocks.find(local);
					sweep = block != swept->second.blocks.end() ? &block->second : nullptr;
				}
				for (std::size_t i = 0; i < mask.size(); ++i) {
					fresh |= (mask[i] & ~(into[i] | (sweep ? (*sweep)[i] : 0ull))) != 0;
					into[i] |= mask[i];
				}
			}
			if (!fresh) {
				continue;
			}
			if (swept == sent_.end() && sentHash_.contains(key)) {
				// Minecraft has this region from a sweep whose result isn't kept any more: sent on its
				// own, the guard's ground would replace the rest of it. It goes with the next flush.
				continue;
			}
			if (swept != sent_.end()) {
				++guardFills_;  // the sweep sent this region without the ground the player is on
			}
			SendRegion(key, Unpack21(key >> 42), Unpack21(key >> 21), Unpack21(key), swept != sent_.end() ? &swept->second : nullptr);
		}
	}

	void Collision::MarkSurface(Regions& a_into, double a_mcX, double a_mcZ, double a_surfaceY, const RED4ext::Vector4& a_normal)
	{
		// A sample stands for the whole cell around it (half a sample spacing each way), not just
		// the 1/8-block column it was cast down: one column per sample left a road 4 columns in 64
		// solid, so the player stood on it but arrows, snowballs and dropped items fell through.
		// The cell follows the surface's slope at 1/8 resolution, not a flat step per sample.
		const auto half = 0.5 / Config().samplesPerBlock;
		const auto ny = static_cast<double>(a_normal.Y);
		if (std::abs(ny) < 0.2) {
			// Grazing a near-vertical face: its slope says nothing about the cell. The horizontal
			// probes find these.
			MarkSpan(a_into, a_mcX, a_mcZ, a_surfaceY - 0.25, a_surfaceY - kEps);
			return;
		}
		const auto [gx0, gx1] = SubRange(a_mcX, half);
		const auto [gz0, gz1] = SubRange(a_mcZ, half);
		for (auto gx = gx0; gx <= gx1; ++gx) {
			for (auto gz = gz0; gz <= gz1; ++gz) {
				const auto dx = (gx + 0.5) / 8.0 - a_mcX;
				const auto dz = (gz + 0.5) / 8.0 - a_mcZ;
				const auto rise = std::clamp(-(a_normal.X * dx + a_normal.Z * dz) / ny, -2.0 * half, 2.0 * half);
				const auto top = a_surfaceY + rise;
				MarkSub(a_into, gx, gz, top - 0.25, top - kEps);
			}
		}
	}

	void Collision::MarkWall(int a_axis, double a_across, const RED4ext::Vector4& a_hit, const RED4ext::Vector4& a_normal, double a_y0, double a_y1)
	{
		// The hit sits on the face, which rounds to the air sub-voxel in front of it: step along
		// -normal so the solid side is what gets marked.
		constexpr double kIn = 0.0625;
		const double     along = a_axis == 0 ? a_normal.X : a_normal.Z;
		const double     across = a_axis == 0 ? a_normal.Z : a_normal.X;
		if (std::abs(along) < 0.5) {
			// Nearly parallel to the probe: the probes along the other axis see this face square on.
			MarkSpan(regions_, a_hit.X - a_normal.X * kIn, a_hit.Z - a_normal.Z * kIn, a_y0, a_y1);
			return;
		}
		// As with floors, the hit stands for the sample's cell across the probe, the face carried
		// on along its own plane.
		const auto half = 0.5 / Config().samplesPerBlock;
		const double hitAlong = a_axis == 0 ? a_hit.X : a_hit.Z;
		const auto [g0, g1] = SubRange(a_across, half);
		for (auto g = g0; g <= g1; ++g) {
			const auto pAcross = (g + 0.5) / 8.0;
			const auto pAlong = hitAlong + std::clamp(-across * (pAcross - a_across) / along, -2.0 * half, 2.0 * half);
			const auto solidAlong = pAlong - along * kIn;
			const auto solidAcross = pAcross - across * kIn;
			MarkSpan(regions_, a_axis == 0 ? solidAlong : solidAcross, a_axis == 0 ? solidAcross : solidAlong, a_y0, a_y1);
		}
	}

	std::pair<std::int32_t, std::int32_t> Collision::SubRange(double a_centre, double a_half)
	{
		// Sub-voxel g spans [g/8, (g+1)/8); it's in the cell if its centre is in [centre - half,
		// centre + half). Cells built from the sample grid tile without gaps or overlaps.
		const auto lo = static_cast<std::int32_t>(std::ceil((a_centre - a_half) * 8.0 - 0.5));
		const auto hi = static_cast<std::int32_t>(std::ceil((a_centre + a_half) * 8.0 - 0.5)) - 1;
		return { lo, std::max(lo, hi) };
	}

	void Collision::MarkSpan(Regions& a_into, double a_mcX, double a_mcZ, double a_y0, double a_y1)
	{
		MarkSub(a_into, FloorI(a_mcX * 8.0), FloorI(a_mcZ * 8.0), a_y0, a_y1);
	}

	void Collision::MarkSub(Regions& a_into, std::int32_t a_gx, std::int32_t a_gz, double a_y0, double a_y1)
	{
		if (a_y1 < a_y0) {
			return;
		}
		const auto bx = FloorDiv(a_gx, 8);
		const auto bz = FloorDiv(a_gz, 8);
		const auto bit = 1ull << ((a_gz - bz * 8) * 8 + (a_gx - bx * 8));
		const auto rx = FloorDiv(bx, kRegionSize);
		const auto rz = FloorDiv(bz, kRegionSize);

		const auto gyLo = FloorI(a_y0 * 8.0);
		const auto gyHi = FloorI(a_y1 * 8.0);
		for (auto gy = gyLo; gy <= gyHi; ++gy) {
			const auto by = FloorDiv(gy, 8);
			if (by < yMin_ || by > yMax_) {
				continue;  // outside the window this sweep promised Minecraft
			}
			const auto ry = FloorDiv(by, kRegionSize);
			auto&      region = a_into[RegionKey(rx, ry, rz)];
			const auto local = static_cast<std::uint32_t>((bx - rx * kRegionSize) + kRegionSize * ((by - ry * kRegionSize) + kRegionSize * (bz - rz * kRegionSize)));
			region.blocks[local][gy - by * 8] |= bit;
		}
	}

	void Collision::RecordWater(double a_mcX, double a_mcZ, double a_surfaceY)
	{
		const auto gx = FloorI(a_mcX) - water_.originX;
		const auto gz = FloorI(a_mcZ) - water_.originZ;
		if (gx < 0 || gz < 0 || gx >= int(proto::kWaterGridSize) || gz >= int(proto::kWaterGridSize)) {
			return;
		}
		auto& cell = water_.surface[gz * proto::kWaterGridSize + gx];
		if (cell == proto::kNoWater || float(a_surfaceY) > cell) {
			cell = float(a_surfaceY);  // the highest surface in the column is the one you swim in
		}
		waterDirty_ = true;
	}

	void Collision::PublishWater()
	{
		if (!waterDirty_) {
			return;
		}
		waterDirty_ = false;
		water_.worldId = 0x43503737;  // "CP77", as in Game.cpp
		Link::Get().WriteWaterGrid(water_);
	}

	void Collision::Flush(const Column& a_column)
	{
		const auto ryLo = FloorDiv(yMin_, kRegionSize);
		const auto ryHi = FloorDiv(yMax_, kRegionSize);
		for (auto ry = ryLo; ry <= ryHi; ++ry) {
			const auto key = RegionKey(a_column.rx, ry, a_column.rz);
			Region     sweep;
			if (const auto found = regions_.find(key); found != regions_.end()) {
				sweep = std::move(found->second);
				regions_.erase(found);
			}
			SendRegion(key, a_column.rx, ry, a_column.rz, &sweep);
			sent_[key] = std::move(sweep);
		}
	}

	void Collision::SendRegion(std::uint64_t a_key, std::int32_t a_rx, std::int32_t a_ry, std::int32_t a_rz, const Region* a_sweep)
	{
		merged_.clear();
		if (a_sweep) {
			merged_ = a_sweep->blocks;
		}
		if (const auto guard = guard_.find(a_key); guard != guard_.end()) {
			for (const auto& [local, mask] : guard->second.blocks) {
				auto& into = merged_[local];
				for (std::size_t i = 0; i < mask.size(); ++i) {
					into[i] |= mask[i];
				}
			}
		}

		// Skip only what Minecraft already has byte for byte. Empty regions still go out the first
		// time: CyberCollision.isKnown() means "sent, even if it was empty", and fluid flow and dig
		// blasts gate on it. Order-free, so the same blocks hash the same however the map iterates.
		scratch_.clear();
		std::size_t hash = merged_.size();
		for (const auto& [local, mask] : merged_) {
			proto::ColBlock block{};
			block.x = a_rx * kRegionSize + static_cast<std::int32_t>(local % kRegionSize);
			block.y = a_ry * kRegionSize + static_cast<std::int32_t>((local / kRegionSize) % kRegionSize);
			block.z = a_rz * kRegionSize + static_cast<std::int32_t>(local / (kRegionSize * kRegionSize));
			std::memcpy(block.bits, mask.data(), sizeof(block.bits));
			scratch_.push_back(block);
			std::uint64_t h = (local + 1ull) * 0x9E3779B97F4A7C15ull;
			for (const auto bits : mask) {
				h = (h ^ bits) * 0xBF58476D1CE4E5B9ull;
				h ^= h >> 31;
			}
			hash += h;
		}
		const auto sent = sentHash_.find(a_key);
		if (sent != sentHash_.end() && sent->second == hash) {
			return;
		}

		proto::ColRegion header{};
		header.minX = a_rx * kRegionSize;
		header.minY = a_ry * kRegionSize;
		header.minZ = a_rz * kRegionSize;
		header.maxX = header.minX + kRegionSize - 1;
		header.maxY = header.minY + kRegionSize - 1;
		header.maxZ = header.minZ + kRegionSize - 1;
		header.epoch = epoch_;
		header.count = static_cast<std::uint32_t>(scratch_.size());

		payload_.resize(sizeof(header) + scratch_.size() * sizeof(proto::ColBlock));
		std::memcpy(payload_.data(), &header, sizeof(header));
		if (!scratch_.empty()) {
			std::memcpy(payload_.data() + sizeof(header), scratch_.data(), scratch_.size() * sizeof(proto::ColBlock));
		}
		if (Link::Get().WriteCollision(proto::kColRegion, payload_.data(), static_cast<std::uint32_t>(payload_.size()))) {
			sentHash_[a_key] = hash;
		} else {
			// Ring full (Minecraft is behind). Forget it was ever sent so the next sweep retries.
			sentHash_.erase(a_key);
		}
	}

	void Collision::UpdateVehicles(const McVec& a_playerMc, bool a_active)
	{
		const auto& cfg = Config();
		if (!cfg.enable || !cfg.vehicles) {
			return;
		}
		auto& link = Link::Get();
		if (!cleared_ || !link.Valid() || !link.McAlive()) {
			return;  // not ahead of this epoch's kColClear, which would drop the layer again
		}
		if (!a_active) {
			// No cars in Minecraft's world while it isn't driving V: in a car, Minecraft's player is
			// brought along inside the one V sits in.
			vehicleBlocks_.clear();
			vehicleFrame_ = 0;
			const std::array<std::int32_t, 3> at{ FloorI(a_playerMc.x), FloorI(a_playerMc.y), FloorI(a_playerMc.z) };
			SendVehicles(at, at);
			return;
		}
		if (++vehicleFrame_ < cfg.vehicleFrames) {
			return;
		}
		vehicleFrame_ = 0;
		ScanVehicles(a_playerMc);
	}

	void Collision::ScanVehicles(const McVec& a_playerMc)
	{
		const auto&                              cfg = Config();
		static const std::vector<RED4ext::CName> groups = GroupList(cfg.vehicleGroup);
		const auto                               spb = cfg.samplesPerBlock;
		const auto                               step = 1.0 / spb;
		const auto                               radius = static_cast<double>(cfg.vehicleRadius);
		// From a little under the feet (the road below a kerb) to over the head (a van's roof).
		const auto top = a_playerMc.y + 5.0;
		const auto bottom = a_playerMc.y - 3.0;

		// One sub-voxel column of car, a_y0..a_y1.
		auto mark = [&](std::int32_t a_gx, std::int32_t a_gz, double a_y0, double a_y1) {
			const auto bx = FloorDiv(a_gx, 8);
			const auto bz = FloorDiv(a_gz, 8);
			const auto bit = 1ull << ((a_gz - bz * 8) * 8 + (a_gx - bx * 8));
			for (auto gy = FloorI(a_y0 * 8.0); gy <= FloorI(a_y1 * 8.0); ++gy) {
				const auto by = FloorDiv(gy, 8);
				auto&      block = vehicleBlocks_[BlockKey(bx, by, bz)];
				block.x = bx;
				block.y = by;
				block.z = bz;
				block.bits[gy - by * 8] |= bit;
			}
		};

		vehicleBlocks_.clear();
		++vehicleScans_;
		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};
		const auto       sx0 = FloorI((a_playerMc.x - radius) * spb), sx1 = FloorI((a_playerMc.x + radius) * spb);
		const auto       sz0 = FloorI((a_playerMc.z - radius) * spb), sz1 = FloorI((a_playerMc.z + radius) * spb);
		for (auto sx = sx0; sx <= sx1; ++sx) {
			for (auto sz = sz0; sz <= sz1; ++sz) {
				const auto x = (sx + 0.5) * step;
				const auto z = (sz + 0.5) * step;
				const auto dx = x - a_playerMc.x, dz = z - a_playerMc.z;
				if (dx * dx + dz * dz > radius * radius) {
					continue;
				}
				// Down onto the car's roof, bonnet or boot...
				++vehicleRays_;
				if (!RaycastGroups(groups, false, { x, top, z }, { x, bottom, z }, hit, normal, material)) {
					continue;
				}
				const double roof = hit.Y;
				// ...and back up from below to its underside: a car is solid between the two, an AV
				// passing overhead isn't solid down to the road. A miss leaves a slab under the roof.
				double floor = std::max(bottom, roof - 0.25);
				++vehicleRays_;
				if (roof - bottom > 0.05 && RaycastGroups(groups, false, { x, bottom, z }, { x, roof - 0.02, z }, hit, normal, material)) {
					floor = std::min(roof, static_cast<double>(hit.Y));
				}
				++vehicleCells_;
				// The sample stands for its whole cell, as the city's surfaces do (MarkSurface).
				const auto [gx0, gx1] = SubRange(x, 0.5 * step);
				const auto [gz0, gz1] = SubRange(z, 0.5 * step);
				for (auto gx = gx0; gx <= gx1; ++gx) {
					for (auto gz = gz0; gz <= gz1; ++gz) {
						mark(gx, gz, floor, roof - kEps);
					}
				}
			}
		}
		SendVehicles({ FloorI(a_playerMc.x - radius), FloorI(bottom), FloorI(a_playerMc.z - radius) },
			{ FloorI(a_playerMc.x + radius), FloorI(top), FloorI(a_playerMc.z + radius) });

		static auto nextLog = std::chrono::steady_clock::time_point{};
		const auto  now = std::chrono::steady_clock::now();
		if (cybercraft::Config::Diagnostics() && now >= nextLog) {
			nextLog = now + std::chrono::seconds(5);
			logger::info("collision: cars: {} scans, {} rays, {} columns of car in the last 5 s; {} blocks of car around the player now",
				vehicleScans_, vehicleRays_, vehicleCells_, vehicleBlocks_.size());
			vehicleScans_ = 0;
			vehicleRays_ = 0;
			vehicleCells_ = 0;
		}
	}

	void Collision::SendVehicles(const std::array<std::int32_t, 3>& a_min, const std::array<std::int32_t, 3>& a_max)
	{
		// Order-free, so the same cars in the same place hash the same however the map iterates.
		std::size_t hash = 0;
		for (const auto& [key, block] : vehicleBlocks_) {
			std::uint64_t h = key * 0x9E3779B97F4A7C15ull;
			for (const auto bits : block.bits) {
				h = (h ^ bits) * 0xBF58476D1CE4E5B9ull;
				h ^= h >> 31;
			}
			hash += h;
		}
		if (vehicleHash_ && *vehicleHash_ == hash) {
			return;  // parked cars, or none: Minecraft has this layer already
		}

		scratch_.clear();
		for (const auto& [key, block] : vehicleBlocks_) {
			scratch_.push_back(block);
		}
		proto::ColRegion header{};
		header.minX = a_min[0];
		header.minY = a_min[1];
		header.minZ = a_min[2];
		header.maxX = a_max[0];
		header.maxY = a_max[1];
		header.maxZ = a_max[2];
		header.epoch = epoch_;
		header.count = static_cast<std::uint32_t>(scratch_.size());
		payload_.resize(sizeof(header) + scratch_.size() * sizeof(proto::ColBlock));
		std::memcpy(payload_.data(), &header, sizeof(header));
		if (!scratch_.empty()) {
			std::memcpy(payload_.data() + sizeof(header), scratch_.data(), scratch_.size() * sizeof(proto::ColBlock));
		}
		if (Link::Get().WriteCollision(proto::kColVehicles, payload_.data(), static_cast<std::uint32_t>(payload_.size()))) {
			vehicleHash_ = hash;
		}
		// Ring full: the next scan tries again.
	}

	void Collision::ProbeRays(const RED4ext::Vector4& a_cpPos)
	{
		auto* system = rtti::System("gameSpatialQueriesSystem");
		if (!system) {
			logger::info("probe: no gameSpatialQueriesSystem");
			return;
		}
		// (start, end, collisionGroup, out result, staticOnly, dynamicOnly) -> Bool.
		static rtti::Method byGroup{ "gameSpatialQueriesSystem", "SyncRaycastByCollisionGroup" };

		RED4ext::Vector4 from{ a_cpPos.X, a_cpPos.Y, a_cpPos.Z + 3.0f, 1.0f };
		RED4ext::Vector4 to{ a_cpPos.X, a_cpPos.Y, a_cpPos.Z - 40.0f, 1.0f };
		logger::info("probe: rays from ({:.2f}, {:.2f}, {:.2f}) down to z {:.2f}", from.X, from.Y, from.Z, to.Z);

		auto report = [](std::string_view a_how, std::string_view a_what, bool a_static, bool a_dynamic, bool a_called, bool a_hit,
						  const RED4ext::physics::TraceResult& a_result) {
			const auto* material = a_result.material.ToString();
			logger::info("probe: {} \"{}\" static {} dynamic {} -> called {} hit {} at ({:.2f}, {:.2f}, {:.2f}) normal ({:.2f}, {:.2f}, {:.2f}) dist {:.2f} flags {:#x} material {}",
				a_how, a_what, a_static, a_dynamic, a_called, a_hit, a_result.position.X, a_result.position.Y, a_result.position.Z,
				a_result.normal.X, a_result.normal.Y, a_result.normal.Z, a_result.distance, a_result.flags, material ? material : "?");
		};
		auto byName = [&](rtti::Method& a_method, std::string_view a_how, std::initializer_list<const char*> a_names) {
			for (const auto* name : a_names) {
				for (const bool staticOnly : { true, false }) {
					RED4ext::CName                selector(name);
					RED4ext::physics::TraceResult result{};
					bool                          only = staticOnly;
					bool                          dynamicOnly = false;
					bool                          hit = false;
					const bool                    called = a_method.Call(system, &hit, from, to, selector, result, only, dynamicOnly);
					report(a_how, name, staticOnly, false, called, hit, result);
				}
			}
		};
		// Collision groups only: an unknown collision or query preset name is a null dereference
		// inside the game (it crashed on collision preset "Static"), and groups are what Raycast uses.
		byName(byGroup, "group", { "Static", "Terrain", "Dynamic", "Water", "PlayerBlocker", "VehicleBlocker", "Vehicle" });
		// From just under the floor V stands on: whether a ray that starts inside a solid hits it
		// where it starts (dist 0) or goes on through to what's below.
		from.Z = a_cpPos.Z - 0.1f;
		logger::info("probe: rays from just under V's feet ({:.2f}, {:.2f}, {:.2f}) down to z {:.2f}", from.X, from.Y, from.Z, to.Z);
		byName(byGroup, "inside", { "Static", "Terrain" });
	}

	std::optional<double> Collision::FirstHitY(const McVec& a_from, const McVec& a_to)
	{
		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};
		if (!Raycast(a_from, a_to, hit, normal, material)) {
			return std::nullopt;
		}
		return static_cast<double>(hit.Y);
	}

	std::optional<double> Collision::HitDistance(const McVec& a_from, const McVec& a_to)
	{
		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};
		if (!Raycast(a_from, a_to, hit, normal, material)) {
			return std::nullopt;
		}
		const double hx = hit.X - a_from.x, hy = hit.Y - a_from.y, hz = hit.Z - a_from.z;
		return std::sqrt(hx * hx + hy * hy + hz * hz);
	}

	bool Collision::Blocked(const McVec& a_from, const McVec& a_to, double a_margin)
	{
		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};
		if (!Raycast(a_from, a_to, hit, normal, material)) {
			return false;
		}
		const double tx = a_to.x - a_from.x, ty = a_to.y - a_from.y, tz = a_to.z - a_from.z;
		const double hx = hit.X - a_from.x, hy = hit.Y - a_from.y, hz = hit.Z - a_from.z;
		const double target = std::sqrt(tx * tx + ty * ty + tz * tz);
		return std::sqrt(hx * hx + hy * hy + hz * hz) < target - a_margin;
	}

	bool Collision::Raycast(const McVec& a_from, const McVec& a_to, RED4ext::Vector4& a_hit, RED4ext::Vector4& a_normal, RED4ext::CName& a_material,
		bool* a_inside)
	{
		// sGroup is a comma-separated list ("Static,Terrain"): the city and the ground under it are
		// separate groups, and the nearest hit of any of them wins.
		static const std::vector<RED4ext::CName> groups = GroupList(Config().group);
		++sweepRays_;
		if (!RaycastGroups(groups, true, a_from, a_to, a_hit, a_normal, a_material, a_inside)) {
			return false;
		}
		if (a_inside && *a_inside) {
			return true;  // not a surface
		}
		++hits_;
		++sweepHits_;
		return true;
	}

	bool Collision::RaycastGroups(const std::vector<RED4ext::CName>& a_groups, bool a_staticOnly, const McVec& a_from, const McVec& a_to,
		RED4ext::Vector4& a_hit, RED4ext::Vector4& a_normal, RED4ext::CName& a_material, bool* a_inside)
	{
		if (a_inside) {
			*a_inside = false;
		}
		// gameSpatialQueriesSystem::SyncRaycastByCollisionGroup(start, end, collisionGroup,
		// out result, staticOnly, dynamicOnly) -> Bool, as the live game's RTTI dump spells it.
		static rtti::Method sync{ "gameSpatialQueriesSystem", "SyncRaycastByCollisionGroup" };
		static bool         warned = false;

		auto* system = rtti::System("gameSpatialQueriesSystem");
		if (!system) {
			return false;
		}
		if (!sync.Get()) {
			if (!warned) {
				warned = true;
				logger::error("collision: no raycast function on gameSpatialQueriesSystem; no collision will be sent");
			}
			return false;
		}

		const auto from = McToCp(a_from.x, a_from.y, a_from.z);
		const auto to = McToCp(a_to.x, a_to.y, a_to.z);

		RED4ext::Vector4              fromV{ from.x, from.y, from.z, 1.0f };
		RED4ext::Vector4              toV{ to.x, to.y, to.z, 1.0f };
		RED4ext::physics::TraceResult best{};
		float                         bestSq = std::numeric_limits<float>::max();
		// The colliders Builds gives the player's own blocks are World Static too, and these rays
		// would find them: Minecraft would get its own blocks back as Night City's collision (still
		// there after the block is broken), and the city would hide the blocks' own faces. They're
		// the only thing made of kOwnMaterial (scripts/CyberCraft.reds), so the ray goes on through.
		static const RED4ext::CName kOwnMaterial("character_vr.physmat");
		const float                 lengthAll = std::sqrt((toV.X - fromV.X) * (toV.X - fromV.X) + (toV.Y - fromV.Y) * (toV.Y - fromV.Y) + (toV.Z - fromV.Z) * (toV.Z - fromV.Z));
		// A ray that starts inside a solid shape (a box, a convex hull) hits it right where it starts:
		// PhysX reports that overlap at distance 0, with its normal against the ray or none at all.
		constexpr float kInsideMetres = 1e-3f;
		bool            inside = false;
		for (auto group : a_groups) {
			RED4ext::physics::TraceResult result{};
			bool                          staticOnly = a_staticOnly;
			bool                          dynamicOnly = false;
			bool                          hit = false;
			RED4ext::Vector4              start = fromV;
			int                           pass = 0;
			for (; pass < 16; ++pass) {
				hit = false;
				if (!sync.Call(system, &hit, start, toV, group, result, staticOnly, dynamicOnly) || !hit || result.material != kOwnMaterial ||
					lengthAll < 1e-4f) {
					break;
				}
				++sweepOwn_;
				// Just past the hit, along the ray; a quarter metre on from inside one of the colliders
				// (boxes a block or more thick), where the hit is where the ray started.
				const float step = (result.distance < kInsideMetres ? 0.25f : 0.02f) / lengthAll;
				start = { result.position.X + (toV.X - fromV.X) * step, result.position.Y + (toV.Y - fromV.Y) * step,
					result.position.Z + (toV.Z - fromV.Z) * step, 1.0f };
				hit = false;
			}
			if (!hit) {
				continue;
			}
			if (a_inside && pass == 0 && result.distance < kInsideMetres) {
				inside = true;  // in any group's solid outweighs the others' hits further on
				continue;
			}
			// A hit with no normal is not a surface to stand on.
			const auto lengthSq = result.normal.X * result.normal.X + result.normal.Y * result.normal.Y + result.normal.Z * result.normal.Z;
			if (lengthSq < 0.01f) {
				continue;
			}
			const auto dx = result.position.X - fromV.X;
			const auto dy = result.position.Y - fromV.Y;
			const auto dz = result.position.Z - fromV.Z;
			const auto distSq = dx * dx + dy * dy + dz * dz;
			if (distSq < bestSq) {
				bestSq = distSq;
				best = result;
			}
		}
		if (inside) {
			*a_inside = true;
			a_hit = { static_cast<float>(a_from.x), static_cast<float>(a_from.y), static_cast<float>(a_from.z), 1.0f };
			a_normal = {};
			a_material = RED4ext::CName();
			return true;
		}
		if (bestSq == std::numeric_limits<float>::max()) {
			return false;
		}
		const auto& result = best;
		a_material = result.material;
		const auto hitMc = CpToMc(result.position.X, result.position.Y, result.position.Z);
		a_hit = { static_cast<float>(hitMc.x), static_cast<float>(hitMc.y), static_cast<float>(hitMc.z), 1.0f };
		// Normals swap axes the same way positions do, with no translation to drop.
		const auto normalMc = CpDirToMc(result.normal.X, result.normal.Y, result.normal.Z);
		a_normal = { static_cast<float>(normalMc.x), static_cast<float>(normalMc.y), static_cast<float>(normalMc.z), 0.0f };
		return true;
	}
}
