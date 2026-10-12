#include "Collision.h"

#include <limits>

#include <RED4ext/Scripting/Natives/physicsTraceResult.hpp>

#include "Builds.h"
#include "Config.h"
#include "Rtti.h"

namespace cybercraft
{
	namespace
	{
		constexpr double kEps = 1.0e-4;
		constexpr double kCell = 1.0 / Collision::kCellsPerBlock;  // an octree cell's edge, blocks
		// Octree boxes are lifted this far (blocks) along every axis: a surface exactly on the line
		// between two cells is the lower one's. Floors are often exactly on a block boundary (the grid
		// is aligned to the ground under V), and a box of exactly a cell's size would put them in both
		// cells, or one inset a little in neither.
		constexpr double kPad = 0.002;
		// A ray whose hit is closer than this to where it started (blocks) started inside a solid.
		constexpr double kInsideBlocks = 0.002;

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

		std::uint64_t CellKey(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			return RegionKey(a_x, a_y, a_z);  // same packing, a cell per unit
		}

		double Axis(const McVec& a_v, int a_axis) { return a_axis == 0 ? a_v.x : a_axis == 1 ? a_v.y : a_v.z; }
		double Axis(const RED4ext::Vector4& a_v, int a_axis) { return a_axis == 0 ? a_v.X : a_axis == 1 ? a_v.Y : a_v.Z; }
		void   SetAxis(McVec& a_v, int a_axis, double a_value) { (a_axis == 0 ? a_v.x : a_axis == 1 ? a_v.y : a_v.z) = a_value; }

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

		const char* MaterialName(RED4ext::CName a_material)
		{
			const auto* name = a_material.ToString();
			return name ? name : "?";
		}

		// The colliders Builds gives the player's own blocks are World Static too, and every query would
		// find them: Minecraft would get its own blocks back as Night City's collision (still there after
		// the block is broken), and the city would hide the blocks' own faces. They're the only thing
		// made of this (scripts/CyberCraft.reds).
		const RED4ext::CName& OwnMaterial()
		{
			static const RED4ext::CName own("character_vr.physmat");
			return own;
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
			// Everything V couldn't walk through was in PlayerBlocker when probed (/cityprobe): static
			// walls and signs, railings and doors (moving bodies), road barriers (Destructible).
			s.solidGroups = Ascii(GetString(L"Collision", L"sSolidGroups", L"Static,Terrain,Destructible,PlayerBlocker"));
			s.group = Ascii(GetString(L"Collision", L"sGroup", L"Static,Terrain"));
			s.radius = GetFloat(L"Collision", L"fRadius", 32.0f);
			s.up = GetFloat(L"Collision", L"fUp", 24.0f);
			s.down = GetFloat(L"Collision", L"fDown", 24.0f);
			s.samplesPerBlock = std::clamp(static_cast<int>(GetFloat(L"Collision", L"iSamplesPerBlock", 2.0f)), 1, 8);
			s.queryBudget = std::max(static_cast<int>(GetFloat(L"Collision", L"iQueryBudget", 2000.0f)), 1);
			s.budgetMs = std::clamp(GetFloat(L"Collision", L"fBudgetMs", 3.0f), 0.25f, 50.0f);
			s.rebuildDist = GetFloat(L"Collision", L"fRebuildDist", 4.0f);
			s.guard = GetBool(L"Collision", L"bGuard", true);
			s.guardDepth = std::clamp(GetFloat(L"Collision", L"fGuardDepth", 8.0f), 1.0f, 24.0f);
			s.vehicles = GetBool(L"Collision", L"bVehicles", true);
			s.vehicleGroup = Ascii(GetString(L"Collision", L"sVehicleGroup", L"Vehicle"));
			s.vehicleRadius = std::clamp(GetFloat(L"Collision", L"fVehicleRadius", 6.0f), 1.0f, 16.0f);
			s.vehicleFrames = std::max(static_cast<int>(GetFloat(L"Collision", L"iVehicleFrames", 2.0f)), 1);
			logger::info("collision: {}, solid is \"{}\" (any body), radius {:.0f} window -{:.0f}/+{:.0f}, {} queries and {:.1f} ms a frame at most",
				s.enable ? "on" : "off", s.solidGroups, s.radius, s.down, s.up, s.queryBudget, s.budgetMs);
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
		phase_ = Phase::kStart;
		stack_.clear();
		cells_.clear();
		cellList_.clear();
		outside_.clear();
		movingCells_.clear();
		movingFound_.clear();
		moving_.clear();
		regions_.clear();
		sentHash_.clear();
		sent_.clear();
		guard_.clear();
		scanned_.clear();
		hits_ = 0;
		waterSampled_.fill(false);
		waterLevel_.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed);
		// kColClear drops Minecraft's car layer with the rest.
		vehicleBlocks_.clear();
		vehicleHash_.reset();
	}

	void Collision::SendGridMoved(const proto::ColGridMoved& a_move)
	{
		gridMoves_.push_back(a_move);
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
		// V read at the very origin (a frame right after Minecraft takes over): a sweep from there
		// threw away the one under her, and the city had no collision for seconds.
		if (a_playerMc.x == 0.0 && a_playerMc.y == 0.0 && a_playerMc.z == 0.0) {
			return;
		}
		const auto started = std::chrono::steady_clock::now();
		// Grid moves first: Minecraft moves the holes while it still has the city as it was.
		while (!gridMoves_.empty()) {
			if (!link.WriteCollision(proto::kColGridMoved, &gridMoves_.front(), sizeof(proto::ColGridMoved))) {
				return;  // ring full: next frame
			}
			gridMoves_.erase(gridMoves_.begin());
		}
		if (!cleared_) {
			if (!link.WriteCollision(proto::kColClear, &epoch_, sizeof(epoch_))) {
				return;  // ring full: try again next frame, before any region goes out
			}
			cleared_ = true;
		}

		spatial_ = rtti::System("gameSpatialQueriesSystem");
		if (!spatial_) {
			return;
		}
		// Where the player is heading, for what to scan first: smoothed over about a quarter second,
		// and nothing from a jump (a teleport, the grid moving).
		if (const double dt = std::chrono::duration<double>(started - movedAt_).count(); dt > 0.0 && dt < 0.5) {
			const McVec  step{ (a_playerMc.x - player_.x) / dt, (a_playerMc.y - player_.y) / dt, (a_playerMc.z - player_.z) / dt };
			const double k = std::min(1.0, dt / 0.25);
			if (step.x * step.x + step.y * step.y + step.z * step.z < 60.0 * 60.0) {
				velocity_ = { velocity_.x + (step.x - velocity_.x) * k, velocity_.y + (step.y - velocity_.y) * k, velocity_.z + (step.z - velocity_.z) * k };
			}
		} else {
			velocity_ = { 0.0, 0.0, 0.0 };
		}
		movedAt_ = started;
		player_ = a_playerMc;
		if (!Calibrate(a_playerMc)) {
			return;
		}
		Rebuild(a_playerMc);
		Guard(a_playerMc);

		PublishWater();
		waterLevel_.store(NearestWater(a_playerMc), std::memory_order_relaxed);

		// Regions scanned not long ago again (rescans: all that's left once the new ones are in) get
		// a third of the time: standing still, they're what the sweep does all day.
		auto       budget = cfg.queryBudget;
		const auto ms = [](double a_ms) { return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(a_ms)); };
		Watch(budget, started + ms(cfg.budgetMs));
		SampleWater(budget, started + ms(cfg.budgetMs));
		while (!pending_.empty()) {
			const auto deadline = started + ms(pending_.back().rescan ? cfg.budgetMs / 3.0 : cfg.budgetMs);
			if (std::chrono::steady_clock::now() >= deadline || !Advance(budget, deadline)) {
				break;
			}
			const auto& job = pending_.back();
			Flush(job);
			scanned_[RegionKey(job.rx, job.ry, job.rz)] = { std::chrono::steady_clock::now(), job.coarse };
			pending_.pop_back();
			++sweepRegions_;
		}
		updateMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		++updateFrames_;
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
		// A finished sweep from where the player still stands is redone every few seconds (the city
		// streams in, doors open), not straight away: back to back, it kept the whole budget busy
		// every frame for nothing.
		const auto now = std::chrono::steady_clock::now();
		if (sweeping_ && !moved && (!pending_.empty() || now - sweptAt_ < kResweepAfter)) {
			return;
		}
		sweptAt_ = now;

		ShiftWater(a_playerMc, !sweeping_);
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
		std::erase_if(moving_, distant);
		// The region in progress carries on if it's still wanted: started over every few blocks, none
		// round a moving player was ever finished.
		std::optional<Job> current;
		if (!pending_.empty() && phase_ != Phase::kStart) {
			current = pending_.back();
		}
		pending_.clear();
		yMin_ = FloorI(a_playerMc.y - cfg.down);
		yMax_ = FloorI(a_playerMc.y + cfg.up);

		const auto rxLo = FloorDiv(FloorI(a_playerMc.x - cfg.radius), kRegionSize);
		const auto rxHi = FloorDiv(FloorI(a_playerMc.x + cfg.radius), kRegionSize);
		const auto ryLo = FloorDiv(yMin_, kRegionSize);
		const auto ryHi = FloorDiv(yMax_, kRegionSize);
		const auto rzLo = FloorDiv(FloorI(a_playerMc.z - cfg.radius), kRegionSize);
		const auto rzHi = FloorDiv(FloorI(a_playerMc.z + cfg.radius), kRegionSize);
		// A region is scanned again after kResweepAfter round the player (a door opened, something
		// knocked over), kFreshFor further out; until then it's what the city still is.
		std::erase_if(scanned_, [&](const auto& a_entry) { return now - a_entry.second.at >= kFreshFor; });
		const auto distanceSq = [](const Job& a_job, const McVec& a_from) {
			const auto cx = a_job.rx * kRegionSize + kRegionSize * 0.5 - a_from.x;
			const auto cy = a_job.ry * kRegionSize + kRegionSize * 0.5 - a_from.y;
			const auto cz = a_job.rz * kRegionSize + kRegionSize * 0.5 - a_from.z;
			return cx * cx + cy * cy + cz * cz;
		};
		for (auto rx = rxLo; rx <= rxHi; ++rx) {
			for (auto ry = ryLo; ry <= ryHi; ++ry) {
				for (auto rz = rzLo; rz <= rzHi; ++rz) {
					Job        job{ rx, ry, rz, false, false };
					const auto here = distanceSq(job, a_playerMc);
					job.coarse = here > kFineBlocks * kFineBlocks;
					const auto last = scanned_.find(RegionKey(rx, ry, rz));
					if (last != scanned_.end()) {
						if (last->second.coarse && !job.coarse) {
							pending_.push_back(job);  // close now: its quarter-block cells, as a new one
							continue;
						}
						if (here > kNearBlocks * kNearBlocks || now - last->second.at < kResweepAfter) {
							continue;
						}
						job.rescan = true;
					}
					pending_.push_back(job);
				}
			}
		}
		// New regions before rescans, each nearest first to where the player will be in half a second;
		// the scan pops from the back.
		const McVec ahead{ a_playerMc.x + velocity_.x * 0.5, a_playerMc.y + velocity_.y * 0.5, a_playerMc.z + velocity_.z * 0.5 };
		std::sort(pending_.begin(), pending_.end(), [&](const Job& a_lhs, const Job& a_rhs) {
			if (a_lhs.rescan != a_rhs.rescan) {
				return a_lhs.rescan;
			}
			return distanceSq(a_lhs, ahead) > distanceSq(a_rhs, ahead);
		});
		std::size_t coarseJobs = 0;
		for (const auto& job : pending_) {
			coarseJobs += job.coarse ? 1 : 0;
		}
		auto kept = pending_.end();
		if (current) {
			kept = std::find_if(pending_.begin(), pending_.end(), [&](const Job& a_job) {
				return a_job.rx == current->rx && a_job.ry == current->ry && a_job.rz == current->rz && a_job.coarse == current->coarse;
			});
		}
		if (kept != pending_.end()) {
			const auto job = *kept;
			pending_.erase(kept);
			pending_.push_back(job);
		} else {
			phase_ = Phase::kStart;
			regions_.clear();  // anything half-scanned is dropped rather than sent
		}
		if (cybercraft::Config::Diagnostics()) {
			// The materials the solid cells were of, most first.
			std::vector<std::pair<std::uint32_t, std::uint64_t>> materials;
			for (const auto& [hash, count] : sweepMaterials_) {
				materials.emplace_back(count, hash);
			}
			std::sort(materials.begin(), materials.end(), std::greater<>());
			std::string kinds;
			for (std::size_t i = 0; i < materials.size() && i < 6; ++i) {
				kinds += std::format("{}{} {}", i ? ", " : "", MaterialName(RED4ext::CName(materials[i].second)), materials[i].first);
			}
			logger::info("collision: sweep from ({:.1f}, {:.1f}, {:.1f}) heading ({:.1f}, {:.1f}, {:.1f}) blocks/s, {} regions to scan ({} again, {} far ones coarsely), y {}..{} (since last: {} regions scanned, {} box queries, {} found something; {} solid cells{}; "
						 "{} rays, {} trimmed a cell; {} boxes deep inside a solid filled whole; {} cells of moving bodies, {} asked again, {} regions changed by them; {} cells of cars left to the car layer; {} guard rays; {} through CyberCraft's colliders, {} boxes of placed blocks skipped, "
						 "{} sub-voxels inside Minecraft's player left out; {} regions the guard filled in under the player, {} water surfaces)",
				a_playerMc.x, a_playerMc.y, a_playerMc.z, velocity_.x, velocity_.y, velocity_.z, pending_.size(),
				std::count_if(pending_.begin(), pending_.end(), [](const Job& a_job) { return a_job.rescan; }), coarseJobs, yMin_, yMax_, sweepRegions_, sweepBoxes_,
				sweepBoxHits_, sweepCells_,
				kinds.empty() ? std::string{} : " (" + kinds + ")", sweepRays_, sweepTrims_, sweepInterior_, sweepMoving_, watchQueries_, watchChanges_, sweepCars_, guardRays_, sweepOwn_, sweepBuilt_, sweepBody_, guardFills_,
				waterHits_);
		}
		sweepRegions_ = 0;
		sweepBoxes_ = 0;
		sweepBoxHits_ = 0;
		sweepCells_ = 0;
		sweepRays_ = 0;
		sweepTrims_ = 0;
		sweepInterior_ = 0;
		sweepMoving_ = 0;
		sweepCars_ = 0;
		watchQueries_ = 0;
		watchChanges_ = 0;
		sweepOwn_ = 0;
		sweepBuilt_ = 0;
		sweepBody_ = 0;
		guardRays_ = 0;
		guardFills_ = 0;
		waterHits_ = 0;
		sweepMaterials_.clear();
	}

	bool Collision::Advance(int& a_budget, std::chrono::steady_clock::time_point a_deadline)
	{
		const auto&    job = pending_.back();
		constexpr auto kEdge = kRegionSize * kCellsPerBlock;  // a region, in cells
		const auto     spent = [&] { return a_budget <= 0 || std::chrono::steady_clock::now() >= a_deadline; };
		if (phase_ == Phase::kStart) {
			stack_.clear();
			cells_.clear();
			cellList_.clear();
			outside_.clear();
			movingCells_.clear();
			movingFound_.clear();
			regions_.clear();
			nextCell_ = 0;
			known_ = { job.rx * kEdge, job.ry * kEdge, job.rz * kEdge, (job.rx + 1) * kEdge, (job.ry + 1) * kEdge, (job.rz + 1) * kEdge };
			Node root{ job.rx * kEdge, job.ry * kEdge, job.rz * kEdge, kEdge, false };
			root.car = CarIn(root);
			--a_budget;
			stack_.push_back(root);
			phase_ = Phase::kOctree;
		}
		if (phase_ == Phase::kOctree) {
			Descend(a_budget, a_deadline);
			if (!stack_.empty()) {
				return false;
			}
			cellList_.assign(cells_.begin(), cells_.end());
			phase_ = Phase::kCells;
		}
		if (phase_ == Phase::kCells) {
			while (nextCell_ < cellList_.size()) {
				if (spent()) {
					return false;
				}
				const auto key = cellList_[nextCell_++];
				const auto x = Unpack21(key >> 42), y = Unpack21(key >> 21), z = Unpack21(key);
				int        rays = 0;
				const auto bits = Carve(x, y, z, rays);
				sweepRays_ += rays;
				a_budget -= std::max(rays, 1);
				MarkCell(x, y, z, bits);
				if (movingCells_.contains(key)) {
					movingFound_.push_back({ key, bits, true });
				}
			}
		}
		phase_ = Phase::kStart;
		return true;
	}

	void Collision::Descend(int& a_budget, std::chrono::steady_clock::time_point a_deadline)
	{
		const auto yLo = known_[1], yHi = known_[4];  // the window, cells
		const bool coarse = !pending_.empty() && pending_.back().coarse;
		while (!stack_.empty()) {
			if (a_budget <= 0 || std::chrono::steady_clock::now() >= a_deadline) {
				return;
			}
			const auto node = stack_.back();
			stack_.pop_back();
			if (node.y >= yHi || node.y + node.size <= yLo) {
				continue;
			}
			// The player's own blocks: their colliders are Static, and Minecraft has the blocks anyway.
			if (Builds::Filled(FloorDiv(node.x, kCellsPerBlock), FloorDiv(node.y, kCellsPerBlock), FloorDiv(node.z, kCellsPerBlock),
					FloorDiv(node.x + node.size - 1, kCellsPerBlock) + 1, FloorDiv(node.y + node.size - 1, kCellsPerBlock) + 1,
					FloorDiv(node.z + node.size - 1, kCellsPerBlock) + 1)) {
				++sweepBuilt_;
				continue;
			}
			--a_budget;
			RED4ext::CName material{};
			bool           moving = false;
			if (!OverlapsNode(node, &material, &moving)) {
				continue;  // nothing anywhere in it: one query for the lot
			}
			// A car in the region: is it in this box? Cars are the car layer's, never the city's.
			bool car = false;
			if (node.car) {
				--a_budget;
				car = CarIn(node);
				if (car && (node.size == 1 || (coarse && node.size == 2))) {
					++sweepCars_;
					continue;
				}
			}
			if (coarse && node.size == 2) {
				// Far off: the half block whole, as long as it's the city's (not a car, the player's own
				// colliders, water).
				if (!car && material != OwnMaterial() && !IsWater(material)) {
					MarkSolid(node);
					++sweepCells_;
					++hits_;
					++sweepMaterials_[material.hash];
				}
				continue;
			}
			if (node.size > 1) {
				// Deep in a solid (the ground under the terrain's surface, a thick slab of street, a building's
				// convex bulk), every box is solid: the box grown by a cell all round has all its corners
				// inside something (a ray from each starts in it), and it's solid all through, without going
				// down to its cells. Grown, so that a box with the solid's surface on its side (which the
				// lift makes its) never is. Left out, the inside of a thick street read as a hollow under
				// it, and a crater blown in it opened into nothing.
				bool interior = true;
				for (int i = 0; i < 8 && interior; ++i) {
					const auto  corner1D = [&](std::int32_t a_lo, int a_far) { return (a_lo + a_far * node.size) * kCell + (a_far ? kCell : -kCell); };
					const McVec corner{ corner1D(node.x, i & 1), corner1D(node.y, (i >> 1) & 1), corner1D(node.z, (i >> 2) & 1) };
					RED4ext::Vector4 hit{};
					RED4ext::Vector4 normal{};
					RED4ext::CName   inMaterial{};
					float            distance = 1.0f;
					--a_budget;
					++sweepRays_;
					interior = RaySolid(corner, { corner.x, corner.y + 0.05, corner.z }, hit, normal, inMaterial, distance) && distance < kInsideBlocks;
				}
				if (interior) {
					++sweepInterior_;
					MarkSolid(node);
					continue;
				}
				const auto half = node.size / 2;
				for (int i = 0; i < 8; ++i) {
					stack_.push_back({ node.x + (i & 1) * half, node.y + ((i >> 1) & 1) * half, node.z + ((i >> 2) & 1) * half, half, car });
				}
				continue;
			}
			if (material == OwnMaterial()) {
				// One of the player's colliders: beside this cell and touching it by the lift, or one not
				// despawned yet. The cell again a hair smaller, clear of the one beside it.
				--a_budget;
				const McVec lo{ node.x * kCell + kPad, node.y * kCell + kPad, node.z * kCell + kPad };
				const McVec hi{ (node.x + 1) * kCell - kPad, (node.y + 1) * kCell - kPad, (node.z + 1) * kCell - kPad };
				if (!Overlaps(lo, hi, &material) || material == OwnMaterial()) {
					++sweepOwn_;
					continue;
				}
			}
			if (IsWater(material) || (bodyMaterial_ != RED4ext::CName() && material == bodyMaterial_)) {
				++sweepBody_;
				continue;  // water is swum in (the water grid); V's own body isn't the city
			}
			cells_.insert(CellKey(node.x, node.y, node.z));
			if (moving) {
				movingCells_.insert(CellKey(node.x, node.y, node.z));
				++sweepMoving_;
			}
			++sweepCells_;
			++hits_;
			++sweepMaterials_[material.hash];
		}
	}

	std::uint8_t Collision::Carve(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, int& a_rays)
	{
		// A solid cell is whole to start with: something is in it. A side with air beside it is looked
		// into from that air, along the cell's centre line: where the surface is, the near half of the
		// cell (a sub-voxel) is air too. A surface that leans (a ramp, a step's nosing) is followed by its
		// normal across the cell's four sub-voxel columns. A ray that finds nothing (the something isn't
		// on the centre line: a railing's bar, an edge) leaves the cell whole.
		const std::array<std::int32_t, 3> cell{ a_x, a_y, a_z };
		std::uint8_t                      bits = 0xFF;  // x + 2 * (z + 2 * y)
		for (int axis = 0; axis < 3; ++axis) {
			for (const int sign : { -1, 1 }) {
				auto beside = cell;
				beside[axis] += sign;
				const auto besideKey = CellKey(beside[0], beside[1], beside[2]);
				if (beside[axis] < known_[axis] || beside[axis] >= known_[axis + 3]) {
					// In the next region: asked about on its own (once).
					auto asked = outside_.find(besideKey);
					if (asked == outside_.end()) {
						RED4ext::CName material{};
						++a_rays;
						const bool solid = OverlapsNode({ beside[0], beside[1], beside[2], 1 }, &material) && material != OwnMaterial() && !IsWater(material) &&
						                   !(bodyMaterial_ != RED4ext::CName() && material == bodyMaterial_);
						asked = outside_.emplace(besideKey, solid).first;
					}
					if (asked->second) {
						continue;  // solid beside it: no surface on this side
					}
				} else if (cells_.contains(besideKey)) {
					continue;  // solid beside it: no surface on this side
				}
				McVec      from{ (a_x + 0.5) * kCell, (a_y + 0.5) * kCell, (a_z + 0.5) * kCell };
				McVec      to = from;
				const auto lo = cell[axis] * kCell;
				const auto face = sign > 0 ? lo + kCell : lo;
				SetAxis(from, axis, face + sign * 2.0 * kPad);
				SetAxis(to, axis, face - sign * (kCell + kPad));
				++a_rays;
				RED4ext::Vector4 hit{};
				RED4ext::Vector4 normal{};
				RED4ext::CName   material{};
				float            distance = 0.0f;
				const bool       found = RaySolid(from, to, hit, normal, material, distance);
				if (explain_) {
					logger::info("cityprobe:     cell ({}, {}, {}) from {}{}: {}", a_x, a_y, a_z, sign > 0 ? '+' : '-', "xyz"[axis],
						found ? std::format("surface {:.3f} blocks in, normal ({:.2f}, {:.2f}, {:.2f}), {}", distance, normal.X, normal.Y, normal.Z, MaterialName(material))
							  : std::string("nothing on its centre line"));
				}
				if (!found || distance < kInsideBlocks) {
					continue;
				}
				const int    b = (axis + 1) % 3, c = (axis + 2) % 3;
				const double na = Axis(normal, axis), nb = Axis(normal, b), nc = Axis(normal, c);
				const double mid = lo + 0.5 * kCell;
				bool         trimmed = false;
				for (int i = 0; i < 2; ++i) {
					for (int j = 0; j < 2; ++j) {
						// Where the surface crosses this sub-voxel column, a sixteenth of a block off the
						// centre line each way.
						double at = Axis(hit, axis);
						if (std::abs(na) > 0.3) {
							at -= (nb * (i ? 1.0 : -1.0) + nc * (j ? 1.0 : -1.0)) / 16.0 / na;
						}
						at = std::clamp(at, lo, lo + kCell);
						if (sign > 0 ? at >= mid : at <= mid) {
							continue;  // the surface is in the near sub-voxel: it stays
						}
						std::array<int, 3> sub{};
						sub[axis] = sign > 0 ? 1 : 0;
						sub[b] = i;
						sub[c] = j;
						bits &= static_cast<std::uint8_t>(~(1u << (sub[0] + 2 * (sub[2] + 2 * sub[1]))));
						trimmed = true;
					}
				}
				sweepTrims_ += trimmed ? 1 : 0;
			}
		}
		// Trimmed away to nothing by two unrelated surfaces: the query said something is in it.
		return bits ? bits : std::uint8_t(0xFF);
	}

	void Collision::MarkCell(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, std::uint8_t a_bits)
	{
		// Nothing found inside Minecraft's player (a hair smaller than it) goes to Minecraft, which would
		// only hold the player stuck in it: a door swung shut on it, or the game's own body.
		// From a little over the feet: the floor they stand on is rounded up to the eighth, and reaches
		// that far into a player standing on the real one.
		constexpr double kIn = 0.02;
		const double     px0 = player_.x - 0.3 + kIn, px1 = player_.x + 0.3 - kIn;
		const double     py0 = player_.y + 0.15, py1 = player_.y + 1.8 - kIn;
		const double     pz0 = player_.z - 0.3 + kIn, pz1 = player_.z + 0.3 - kIn;
		for (int i = 0; i < 8; ++i) {
			if (!(a_bits & (1u << i))) {
				continue;
			}
			const auto gx = a_x * 2 + (i & 1);
			const auto gz = a_z * 2 + ((i >> 1) & 1);
			const auto gy = a_y * 2 + ((i >> 2) & 1);
			if (gx / 8.0 < px1 && (gx + 1) / 8.0 > px0 && gy / 8.0 < py1 && (gy + 1) / 8.0 > py0 && gz / 8.0 < pz1 && (gz + 1) / 8.0 > pz0) {
				++sweepBody_;
				continue;
			}
			SetSub(regions_, gx, gy, gz);
		}
	}

	void Collision::MarkSolid(const Node& a_node)
	{
		// Whole blocks a mask at a time; a box smaller than a block (or off the block grid), sub-voxel by sub-voxel.
		if (a_node.size % kCellsPerBlock == 0 && a_node.x % kCellsPerBlock == 0 && a_node.y % kCellsPerBlock == 0 && a_node.z % kCellsPerBlock == 0) {
			Mask full;
			full.fill(~0ull);
			const auto blocks = a_node.size / kCellsPerBlock;
			for (std::int32_t by = 0; by < blocks; ++by) {
				for (std::int32_t bz = 0; bz < blocks; ++bz) {
					for (std::int32_t bx = 0; bx < blocks; ++bx) {
						const auto x = FloorDiv(a_node.x, kCellsPerBlock) + bx, y = FloorDiv(a_node.y, kCellsPerBlock) + by, z = FloorDiv(a_node.z, kCellsPerBlock) + bz;
						const auto rx = FloorDiv(x, kRegionSize), ry = FloorDiv(y, kRegionSize), rz = FloorDiv(z, kRegionSize);
						const auto local = static_cast<std::uint32_t>((x - rx * kRegionSize) + kRegionSize * ((y - ry * kRegionSize) + kRegionSize * (z - rz * kRegionSize)));
						regions_[RegionKey(rx, ry, rz)].blocks[local] = full;
					}
				}
			}
			return;
		}
		const auto per = 8 / kCellsPerBlock;  // sub-voxels per cell edge
		for (std::int32_t gy = a_node.y * per; gy < (a_node.y + a_node.size) * per; ++gy) {
			for (std::int32_t gz = a_node.z * per; gz < (a_node.z + a_node.size) * per; ++gz) {
				for (std::int32_t gx = a_node.x * per; gx < (a_node.x + a_node.size) * per; ++gx) {
					SetSub(regions_, gx, gy, gz);
				}
			}
		}
	}

	void Collision::SetSub(Regions& a_into, std::int32_t a_gx, std::int32_t a_gy, std::int32_t a_gz)
	{
		const auto bx = FloorDiv(a_gx, 8);
		const auto by = FloorDiv(a_gy, 8);
		const auto bz = FloorDiv(a_gz, 8);
		const auto rx = FloorDiv(bx, kRegionSize);
		const auto ry = FloorDiv(by, kRegionSize);
		const auto rz = FloorDiv(bz, kRegionSize);
		auto&      region = a_into[RegionKey(rx, ry, rz)];
		const auto local = static_cast<std::uint32_t>((bx - rx * kRegionSize) + kRegionSize * ((by - ry * kRegionSize) + kRegionSize * (bz - rz * kRegionSize)));
		region.blocks[local][a_gy - by * 8] |= 1ull << ((a_gz - bz * 8) * 8 + (a_gx - bx * 8));
	}

	bool Collision::SolidFilter(RED4ext::physics::QueryFilter& a_filter)
	{
		// Built by the game's own physicsQueryFilter.AddGroup, group by group, as scripts do.
		static RED4ext::physics::QueryFilter filter{};
		static bool                          built = false;
		static bool                          usable = false;
		if (!built) {
			built = true;
			static rtti::Method addGroup{ "physicsQueryFilter", "AddGroup" };
			for (auto group : GroupList(Config().solidGroups)) {
				if (!addGroup.Call(nullptr, nullptr, filter, group)) {
					logger::warn("collision: physicsQueryFilter.AddGroup({}) failed", group.ToString());
				}
			}
			usable = filter.mask1 != 0 || filter.mask2 != 0;
			logger::info("collision: the query filter of \"{}\" is mask1 {:#x} mask2 {:#x}", Config().solidGroups, filter.mask1, filter.mask2);
		}
		a_filter = filter;
		return usable;
	}

	bool Collision::OverlapRaw(const RED4ext::Vector4& a_dimension, const RED4ext::Vector4& a_centre, RED4ext::CName* a_material, bool* a_moving)
	{
		// OverlapByQueryFilter(primitiveDimension, position, rotation: Quaternion, queryFilter, out result)
		// and Overlap(primitiveDimension, position, rotation: EulerAngles, collisionGroup, out result),
		// as the live game's RTTI dump spells them.
		static rtti::Method                      byFilter{ "gameSpatialQueriesSystem", "OverlapByQueryFilter" };
		static rtti::Method                      byGroup{ "gameSpatialQueriesSystem", "Overlap" };
		static const std::vector<RED4ext::CName> groups = GroupList(Config().solidGroups);
		if (!spatial_) {
			return false;
		}
		++sweepBoxes_;
		// Turned with the grid, as Builds turns its colliders (about the vertical).
		const double                  yaw = gGridYaw.load(std::memory_order_relaxed);
		RED4ext::Vector4              dimension = a_dimension;
		RED4ext::Vector4              centre = a_centre;
		RED4ext::physics::TraceResult result{};
		bool                          hit = false;
		if (byFilter_) {
			RED4ext::physics::QueryFilter filter{};
			SolidFilter(filter);
			const double        half = yaw * 0.017453292519943295 * 0.5;
			RED4ext::Quaternion rotation{ 0.0f, 0.0f, static_cast<float>(std::sin(half)), static_cast<float>(std::cos(half)) };
			if (!byFilter.Call(spatial_, &hit, dimension, centre, rotation, filter, result)) {
				return false;
			}
		} else {
			RED4ext::EulerAngles rotation{ 0.0f, 0.0f, static_cast<float>(yaw) };
			for (auto group : groups) {
				result = {};
				hit = false;
				if (byGroup.Call(spatial_, &hit, dimension, centre, rotation, group, result) && hit) {
					break;
				}
			}
		}
		if (!hit) {
			return false;
		}
		++sweepBoxHits_;
		if (a_material) {
			*a_material = result.material;
		}
		if (a_moving) {
			*a_moving = result.simulationType == RED4ext::physics::SimulationType::Dynamic || result.simulationType == RED4ext::physics::SimulationType::Kinematic;
		}
		return true;
	}

	bool Collision::Overlaps(const McVec& a_min, const McVec& a_max, RED4ext::CName* a_material, bool* a_moving)
	{
		const auto scale = (dimension_ == Dimension::kFull ? 1.0 : 0.5) * MetresPerBlock();
		const auto centre = McToCp((a_min.x + a_max.x) * 0.5, (a_min.y + a_max.y) * 0.5, (a_min.z + a_max.z) * 0.5);
		// Minecraft x, y, z extents are Cyberpunk x, z, y, along the grid's turned axes.
		const RED4ext::Vector4 dimension{ static_cast<float>((a_max.x - a_min.x) * scale), static_cast<float>((a_max.z - a_min.z) * scale),
			static_cast<float>((a_max.y - a_min.y) * scale), 0.0f };
		return OverlapRaw(dimension, { centre.x, centre.y, centre.z, 1.0f }, a_material, a_moving);
	}

	bool Collision::OverlapsNode(const Node& a_node, RED4ext::CName* a_material, bool* a_moving)
	{
		const McVec  lo{ a_node.x * kCell + kPad, a_node.y * kCell + kPad, a_node.z * kCell + kPad };
		const double edge = a_node.size * kCell;
		return Overlaps(lo, { lo.x + edge, lo.y + edge, lo.z + edge }, a_material, a_moving);
	}

	bool Collision::CarIn(const Node& a_node)
	{
		static rtti::Method                      byGroup{ "gameSpatialQueriesSystem", "Overlap" };
		static const std::vector<RED4ext::CName> groups = GroupList(Config().vehicleGroup);
		if (!spatial_ || !Config().vehicles) {
			return false;
		}
		++sweepBoxes_;
		// Grown by a cell all round: a car's blocker reaches a little past its body (the Vehicle group's
		// hull), and those cells are the car's too.
		const McVec      lo{ (a_node.x - 1) * kCell, (a_node.y - 1) * kCell, (a_node.z - 1) * kCell };
		const double     edge = (a_node.size + 2) * kCell;
		const auto       scale = (dimension_ == Dimension::kFull ? 1.0 : 0.5) * MetresPerBlock();
		const auto       centre = McToCp(lo.x + edge * 0.5, lo.y + edge * 0.5, lo.z + edge * 0.5);
		RED4ext::Vector4 dimension{ static_cast<float>(edge * scale), static_cast<float>(edge * scale), static_cast<float>(edge * scale), 0.0f };
		RED4ext::Vector4 position{ centre.x, centre.y, centre.z, 1.0f };
		RED4ext::EulerAngles rotation{ 0.0f, 0.0f, static_cast<float>(gGridYaw.load(std::memory_order_relaxed)) };
		for (auto group : groups) {
			RED4ext::physics::TraceResult result{};
			bool                          hit = false;
			if (byGroup.Call(spatial_, &hit, dimension, position, rotation, group, result) && hit) {
				return true;
			}
		}
		return false;
	}

	void Collision::Watch(int& a_budget, std::chrono::steady_clock::time_point a_deadline)
	{
		// Four times a second, the moving bodies' cells in the regions round the player (the one it's
		// in and those next to it) asked about again, one query each.
		const auto now = std::chrono::steady_clock::now();
		if (moving_.empty() || now - watchedAt_ < std::chrono::milliseconds(250)) {
			return;
		}
		watchedAt_ = now;
		const auto prx = FloorDiv(FloorI(player_.x), kRegionSize), pry = FloorDiv(FloorI(player_.y), kRegionSize), prz = FloorDiv(FloorI(player_.z), kRegionSize);
		for (auto rx = prx - 1; rx <= prx + 1; ++rx) {
			for (auto ry = pry - 1; ry <= pry + 1; ++ry) {
				for (auto rz = prz - 1; rz <= prz + 1; ++rz) {
					const auto key = RegionKey(rx, ry, rz);
					const auto cells = moving_.find(key);
					const auto sent = sent_.find(key);
					if (cells == moving_.end() || sent == sent_.end()) {
						continue;
					}
					bool changed = false;
					for (auto& cell : cells->second) {
						if (a_budget <= 0 || std::chrono::steady_clock::now() >= a_deadline) {
							break;
						}
						--a_budget;
						++watchQueries_;
						const auto x = Unpack21(cell.cell >> 42), y = Unpack21(cell.cell >> 21), z = Unpack21(cell.cell);
						RED4ext::CName material{};
						const bool     there = OverlapsNode({ x, y, z, 1 }, &material) && material != OwnMaterial();
						if (there == cell.there) {
							continue;
						}
						cell.there = there;
						changed = true;
						// Its sub-voxels in what Minecraft has of the region: gone with the door, back when it shuts.
						for (int i = 0; i < 8; ++i) {
							if (!(cell.bits & (1u << i))) {
								continue;
							}
							const auto gx = x * 2 + (i & 1), gz = z * 2 + ((i >> 1) & 1), gy = y * 2 + ((i >> 2) & 1);
							const auto bx = FloorDiv(gx, 8), by = FloorDiv(gy, 8), bz = FloorDiv(gz, 8);
							const auto local = static_cast<std::uint32_t>((bx - rx * kRegionSize) + kRegionSize * ((by - ry * kRegionSize) + kRegionSize * (bz - rz * kRegionSize)));
							const auto bit = 1ull << ((gz - bz * 8) * 8 + (gx - bx * 8));
							auto&      mask = sent->second.blocks[local];
							mask[gy - by * 8] = there ? (mask[gy - by * 8] | bit) : (mask[gy - by * 8] & ~bit);
						}
					}
					if (changed) {
						++watchChanges_;
						SendRegion(key, rx, ry, rz, &sent->second);
					}
				}
			}
		}
	}

	bool Collision::RaySolid(const McVec& a_from, const McVec& a_to, RED4ext::Vector4& a_hit, RED4ext::Vector4& a_normal, RED4ext::CName& a_material,
		float& a_distance)
	{
		// SyncRaycastByQueryFilter(start, end, queryFilter, out result, staticOnly, dynamicOnly) and
		// SyncRaycastByCollisionGroup(start, end, collisionGroup, out result, staticOnly, dynamicOnly).
		static rtti::Method                      byFilter{ "gameSpatialQueriesSystem", "SyncRaycastByQueryFilter" };
		static rtti::Method                      byGroup{ "gameSpatialQueriesSystem", "SyncRaycastByCollisionGroup" };
		static const std::vector<RED4ext::CName> groups = GroupList(Config().solidGroups);
		if (!spatial_) {
			return false;
		}
		const auto       f = McToCp(a_from.x, a_from.y, a_from.z);
		const auto       t = McToCp(a_to.x, a_to.y, a_to.z);
		RED4ext::Vector4 from{ f.x, f.y, f.z, 1.0f };
		RED4ext::Vector4 to{ t.x, t.y, t.z, 1.0f };
		const float      dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
		const float      length = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (length < 1e-5f) {
			return false;
		}
		RED4ext::physics::TraceResult best{};
		float                         bestAlong = std::numeric_limits<float>::max();
		// One cast, on through the player's own colliders, water and V's body (none of them the city).
		const auto trace = [&](auto&& a_cast) {
			RED4ext::Vector4 start = from;
			for (int pass = 0; pass < 8; ++pass) {
				RED4ext::physics::TraceResult result{};
				if (!a_cast(start, result)) {
					return;
				}
				// From inside a solid the hit is where this cast started (its position isn't to be trusted).
				const auto  project = [&](float a_x, float a_y, float a_z) { return ((a_x - from.X) * dx + (a_y - from.Y) * dy + (a_z - from.Z) * dz) / length; };
				const float along = result.distance < 1e-3f ? project(start.X, start.Y, start.Z) : project(result.position.X, result.position.Y, result.position.Z);
				if (result.material == OwnMaterial() || IsWater(result.material) || (bodyMaterial_ != RED4ext::CName() && result.material == bodyMaterial_)) {
					sweepOwn_ += result.material == OwnMaterial() ? 1 : 0;
					// Just past it; a quarter metre on from inside one, where the hit is where it started.
					const float next = along + (result.distance < 1e-3f ? 0.25f : 0.02f);
					if (next >= length) {
						return;
					}
					start = { from.X + dx * next / length, from.Y + dy * next / length, from.Z + dz * next / length, 1.0f };
					continue;
				}
				if (along < bestAlong) {
					bestAlong = std::max(along, 0.0f);
					best = result;
				}
				return;
			}
		};
		bool staticOnly = false;
		bool dynamicOnly = false;
		if (rayByFilter_) {
			RED4ext::physics::QueryFilter filter{};
			SolidFilter(filter);
			trace([&](RED4ext::Vector4& a_start, RED4ext::physics::TraceResult& a_result) {
				bool hit = false;
				return byFilter.Call(spatial_, &hit, a_start, to, filter, a_result, staticOnly, dynamicOnly) && hit;
			});
		} else {
			for (auto group : groups) {
				trace([&](RED4ext::Vector4& a_start, RED4ext::physics::TraceResult& a_result) {
					bool hit = false;
					return byGroup.Call(spatial_, &hit, a_start, to, group, a_result, staticOnly, dynamicOnly) && hit;
				});
			}
		}
		if (bestAlong == std::numeric_limits<float>::max()) {
			return false;
		}
		a_material = best.material;
		const auto hitMc = CpToMc(best.position.X, best.position.Y, best.position.Z);
		a_hit = { static_cast<float>(hitMc.x), static_cast<float>(hitMc.y), static_cast<float>(hitMc.z), 1.0f };
		// Normals swap axes the same way positions do, with no translation to drop.
		const auto normalMc = CpDirToMc(best.normal.X, best.normal.Y, best.normal.Z);
		a_normal = { static_cast<float>(normalMc.x), static_cast<float>(normalMc.y), static_cast<float>(normalMc.z), 0.0f };
		a_distance = static_cast<float>(bestAlong / MetresPerBlock());
		return true;
	}

	bool Collision::Calibrate(const McVec& a_playerMc)
	{
		if (dimension_ != Dimension::kUnknown) {
			return true;
		}
		++calibrationTries_;
		// Whether the game takes a box's dimension as half extents or whole, and whether one query by
		// the filter finds what the groups one by one do: small boxes just over a floor a static ray
		// found. A box centred a quarter of its half size over the floor reaches it either way, at three
		// quarters only as half extents, at three halves not at all.
		RED4ext::physics::QueryFilter filter{};
		const bool                    haveFilter = SolidFilter(filter);
		constexpr double              kHalf = 0.1;  // blocks
		const auto                    size = static_cast<float>(kHalf * MetresPerBlock());
		const RED4ext::Vector4        dimension{ size, size, size, 0.0f };
		std::string                   tried;
		// A block from V: clear of her body, which the queries might see.
		constexpr std::array<std::pair<double, double>, 8> kAround{ { { 1.0, 0.0 }, { -1.0, 0.0 }, { 0.0, 1.0 }, { 0.0, -1.0 }, { 1.0, 1.0 },
			{ -1.0, -1.0 }, { 1.0, -1.0 }, { -1.0, 1.0 } } };
		for (const auto& [ox, oz] : kAround) {
			const McVec      top{ a_playerMc.x + ox, a_playerMc.y + 1.5, a_playerMc.z + oz };
			const McVec      bottom{ top.x, a_playerMc.y - 8.0, top.z };
			RED4ext::Vector4 hit{};
			RED4ext::Vector4 normal{};
			RED4ext::CName   material{};
			bool             inside = false;
			if (!Raycast(top, bottom, hit, normal, material, &inside) || inside || normal.Y < 0.9f) {
				continue;
			}
			const double floorY = hit.Y;
			const auto   at = [&](double a_above) {
                const auto c = McToCp(top.x, floorY + a_above, top.z);
                return RED4ext::Vector4{ c.x, c.y, c.z, 1.0f };
			};
			for (const bool filtered : { true, false }) {
				if (filtered && !haveFilter) {
					continue;
				}
				byFilter_ = filtered;
				const bool reach = OverlapRaw(dimension, at(0.25 * kHalf), nullptr);
				const bool half = OverlapRaw(dimension, at(0.75 * kHalf), nullptr);
				const bool clear = !OverlapRaw(dimension, at(3.0 * kHalf), nullptr);
				tried += std::format(" [{} over {} at y {:.2f}: {} {} {}]", filtered ? "filter" : "groups", MaterialName(material), floorY, reach, half, !clear);
				if (!reach || !clear) {
					continue;
				}
				dimension_ = half ? Dimension::kHalf : Dimension::kFull;
				// Rays: the filtered one must find the same floor.
				rayByFilter_ = haveFilter;
				if (haveFilter) {
					RED4ext::Vector4 rayHit{};
					RED4ext::Vector4 rayNormal{};
					RED4ext::CName   rayMaterial{};
					float            distance = 0.0f;
					if (!RaySolid(top, bottom, rayHit, rayNormal, rayMaterial, distance) || std::abs(rayHit.Y - floorY) > 0.02) {
						rayByFilter_ = false;
					}
				}
				// V's own body, which Minecraft's player stands in: if the queries see it, what it's made of
				// is left out of the city.
				const auto     chest = McToCp(a_playerMc.x, a_playerMc.y + 1.0, a_playerMc.z);
				RED4ext::CName body{};
				if (OverlapRaw(dimension, { chest.x, chest.y, chest.z, 1.0f }, &body) && body != material) {
					bodyMaterial_ = body;
				}
				logger::info("collision: box queries {}, rays {}; a box's dimension is its {} extents; V's body is {}{}",
					byFilter_ ? "by the one filter" : "group by group", rayByFilter_ ? "by the filter" : "group by group",
					dimension_ == Dimension::kHalf ? "half" : "whole", bodyMaterial_ != RED4ext::CName() ? "SEEN, left out: " : "not seen",
					bodyMaterial_ != RED4ext::CName() ? MaterialName(bodyMaterial_) : "");
				logger::info("collision: (calibrated on{})", tried);
				return true;
			}
		}
		if (calibrationTries_ == 1 || calibrationTries_ % 600 == 0) {
			logger::warn("collision: no sweep yet, the box queries aren't worked out (no flat floor a block from the player, or they find nothing):{}",
				tried.empty() ? std::string(" no floor") : tried);
		}
		return false;
	}

	void Collision::Guard(const McVec& a_playerMc)
	{
		const auto& cfg = Config();
		if (!cfg.guard) {
			return;
		}
		// Every sample cell under Minecraft's player (0.6 blocks wide) and half a block round it, for
		// where it will be by the next frame.
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
				++guardRays_;
				float      distance = 0.0f;
				const bool found = RaySolid({ x, a_playerMc.y + 0.5, z }, { x, a_playerMc.y - cfg.guardDepth, z }, hit, normal, material, distance);
				if (!found || distance < kInsideBlocks || std::abs(normal.Y) < 0.2f) {
					continue;  // a cell round the player can start in a wall; a grazed wall isn't ground
				}
				// The sub-voxels the surface is in, across the cell the octree finds there: never more than
				// the sweep has.
				const auto cx = FloorI(x * kCellsPerBlock);
				const auto cz = FloorI(z * kCellsPerBlock);
				const auto top = static_cast<std::int32_t>(std::ceil(hit.Y * 8.0)) - 1;  // one exactly on a boundary: the one under it
				for (auto gx = cx * 2; gx < cx * 2 + 2; ++gx) {
					for (auto gz = cz * 2; gz < cz * 2 + 2; ++gz) {
						SetSub(guardScratch_, gx, top, gz);
					}
				}
				++hits_;
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

	std::pair<std::int32_t, std::int32_t> Collision::SubRange(double a_centre, double a_half)
	{
		// Sub-voxel g spans [g/8, (g+1)/8); it's in the cell if its centre is in [centre - half,
		// centre + half). Cells built from the sample grid tile without gaps or overlaps.
		const auto lo = static_cast<std::int32_t>(std::ceil((a_centre - a_half) * 8.0 - 0.5));
		const auto hi = static_cast<std::int32_t>(std::ceil((a_centre + a_half) * 8.0 - 0.5)) - 1;
		return { lo, std::max(lo, hi) };
	}


	bool Collision::SentSolid(double a_x, double a_y, double a_z) const
	{
		const auto gx = FloorI(a_x * 8.0), gy = FloorI(a_y * 8.0), gz = FloorI(a_z * 8.0);
		const auto bx = FloorDiv(gx, 8), by = FloorDiv(gy, 8), bz = FloorDiv(gz, 8);
		const auto rx = FloorDiv(bx, kRegionSize), ry = FloorDiv(by, kRegionSize), rz = FloorDiv(bz, kRegionSize);
		const auto key = RegionKey(rx, ry, rz);
		const auto local = static_cast<std::uint32_t>((bx - rx * kRegionSize) + kRegionSize * ((by - ry * kRegionSize) + kRegionSize * (bz - rz * kRegionSize)));
		const auto bit = 1ull << ((gz - bz * 8) * 8 + (gx - bx * 8));
		for (const auto* regions : { &sent_, &guard_ }) {
			const auto region = regions->find(key);
			if (region == regions->end()) {
				continue;
			}
			const auto block = region->second.blocks.find(local);
			if (block != region->second.blocks.end() && (block->second[gy - by * 8] & bit)) {
				return true;
			}
		}
		return false;
	}

	std::string Collision::DescribeAhead(const McVec& a_feet, double a_dirX, double a_dirZ) const
	{
		const double length = std::sqrt(a_dirX * a_dirX + a_dirZ * a_dirZ);
		if (length < 1e-6) {
			return {};
		}
		const double fx = a_dirX / length, fz = a_dirZ / length;
		const double sx = -fz, sz = fx;  // across
		std::string  out;
		for (int step = 1; step <= 8; ++step) {
			const double d = step / 8.0;
			out += std::format("\n    {:.3f} ahead:", d);
			for (const double side : { -0.3, 0.0, 0.3 }) {
				out += ' ';
				for (int h = -2; h < 16; ++h) {
					const double y = a_feet.y + (h + 0.5) / 8.0;
					out += SentSolid(a_feet.x + fx * d + sx * side, y, a_feet.z + fz * d + sz * side) ? '#' : '.';
				}
			}
		}
		return out;
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

	void Collision::ShiftWater(const McVec& a_playerMc, bool a_clear)
	{
		constexpr auto size = int(proto::kWaterGridSize);
		const auto     originX = FloorI(a_playerMc.x) - size / 2;
		const auto     originZ = FloorI(a_playerMc.z) - size / 2;
		if (!a_clear && originX == water_.originX && originZ == water_.originZ) {
			return;
		}
		// The grid follows the player, the water stays put: what's been found keeps its place, and
		// only the columns new to the grid wait for the sweep. Cleared on every new sweep, Minecraft
		// lost the water under the player until the sweep came back to it, and dropped out of
		// swimming every few blocks.
		std::array<float, proto::kWaterGridSize * proto::kWaterGridSize> moved;
		std::array<bool, proto::kWaterGridSize * proto::kWaterGridSize>  sampled{};
		moved.fill(proto::kNoWater);
		if (!a_clear) {
			for (int gz = 0; gz < size; ++gz) {
				for (int gx = 0; gx < size; ++gx) {
					const auto ox = gx + originX - water_.originX;
					const auto oz = gz + originZ - water_.originZ;
					if (ox >= 0 && oz >= 0 && ox < size && oz < size) {
						moved[gz * size + gx] = water_.surface[oz * size + ox];
						sampled[gz * size + gx] = waterSampled_[oz * size + ox];
					}
				}
			}
		}
		std::copy(moved.begin(), moved.end(), std::begin(water_.surface));
		waterSampled_ = sampled;
		water_.originX = originX;
		water_.originZ = originZ;
		waterDirty_ = true;
	}

	float Collision::NearestWater(const McVec& a_playerMc) const
	{
		constexpr auto size = int(proto::kWaterGridSize);
		float          level = std::numeric_limits<float>::quiet_NaN();
		double         bestSq = std::numeric_limits<double>::max();
		for (int gz = 0; gz < size; ++gz) {
			for (int gx = 0; gx < size; ++gx) {
				const auto surface = water_.surface[gz * size + gx];
				if (surface == proto::kNoWater) {
					continue;
				}
				const auto dx = water_.originX + gx + 0.5 - a_playerMc.x;
				const auto dz = water_.originZ + gz + 0.5 - a_playerMc.z;
				if (dx * dx + dz * dz < bestSq) {
					bestSq = dx * dx + dz * dz;
					level = surface;
				}
			}
		}
		return level;
	}

	void Collision::SampleWater(int& a_budget, std::chrono::steady_clock::time_point a_deadline)
	{
		// The grid's columns by distance from its middle (where the player is), worked out once.
		constexpr int size = int(proto::kWaterGridSize);
		static const auto order = [] {
			std::array<int, size * size> cells{};
			for (int i = 0; i < size * size; ++i) {
				cells[i] = i;
			}
			std::sort(cells.begin(), cells.end(), [](int a_lhs, int a_rhs) {
				const auto d = [](int a_cell) {
					const double dx = a_cell % size + 0.5 - size / 2.0, dz = a_cell / size + 0.5 - size / 2.0;
					return dx * dx + dz * dz;
				};
				return d(a_lhs) < d(a_rhs);
			});
			return cells;
		}();
		for (const auto cell : order) {
			if (waterSampled_[cell]) {
				continue;
			}
			if (a_budget <= 0 || std::chrono::steady_clock::now() >= a_deadline) {
				return;
			}
			waterSampled_[cell] = true;
			const double x = water_.originX + cell % size + 0.5, z = water_.originZ + cell / size + 0.5;
			a_budget -= WaterColumn(x, z, yMax_ + 1.0, static_cast<double>(yMin_));
		}
	}

	int Collision::WaterColumn(double a_mcX, double a_mcZ, double a_top, double a_bottom)
	{
		int    rays = 1;
		double surfaceY = 0.0;
		if (WaterSurface({ a_mcX, a_top, a_mcZ }, { a_mcX, a_bottom, a_mcZ }, surfaceY)) {
			RecordWater(a_mcX, a_mcZ, surfaceY);
			return rays;
		}
		// The city's own water surfaces are static bodies of a water material (the box queries leave
		// them out): down through what's over it, a surface at a time.
		static const std::vector<RED4ext::CName> groups = GroupList(Config().group);
		RED4ext::Vector4                         hit{};
		RED4ext::Vector4                         normal{};
		RED4ext::CName                           material{};
		double                                   from = a_top;
		for (int pass = 0; pass < 12 && from > a_bottom; ++pass) {
			++rays;
			bool inside = false;
			if (!RaycastGroups(groups, true, { a_mcX, from, a_mcZ }, { a_mcX, a_bottom, a_mcZ }, hit, normal, material, &inside)) {
				break;
			}
			if (inside) {
				from -= 0.5;  // started inside a solid: on down through it
				continue;
			}
			if (IsWater(material)) {
				++waterHits_;
				RecordWater(a_mcX, a_mcZ, hit.Y);
				break;
			}
			from = hit.Y - 0.05;
		}
		return rays;
	}

	bool Collision::WaterSurface(const McVec& a_from, const McVec& a_to, double& a_surfaceY)
	{
		static rtti::Method        sync{ "gameSpatialQueriesSystem", "SyncRaycastByCollisionGroup" };
		static const RED4ext::CName kWater("Water");
		auto*                      system = rtti::System("gameSpatialQueriesSystem");
		if (!system || !sync.Get()) {
			return false;
		}
		const auto                    from = McToCp(a_from.x, a_from.y, a_from.z);
		const auto                    to = McToCp(a_to.x, a_to.y, a_to.z);
		RED4ext::Vector4              fromV{ from.x, from.y, from.z, 1.0f };
		RED4ext::Vector4              toV{ to.x, to.y, to.z, 1.0f };
		RED4ext::physics::TraceResult result{};
		bool                          hit = false;
		bool                          staticOnly = false;
		bool                          dynamicOnly = false;
		if (!sync.Call(system, &hit, fromV, toV, kWater, result, staticOnly, dynamicOnly) || !hit || !IsWater(result.material)) {
			return false;
		}
		++waterHits_;
		a_surfaceY = CpToMc(result.position.X, result.position.Y, result.position.Z).y;
		return true;
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

	void Collision::Flush(const Job& a_job)
	{
		const auto key = RegionKey(a_job.rx, a_job.ry, a_job.rz);
		Region     sweep;
		if (const auto found = regions_.find(key); found != regions_.end()) {
			sweep = std::move(found->second);
		}
		regions_.clear();
		SendRegion(key, a_job.rx, a_job.ry, a_job.rz, &sweep);
		sent_[key] = std::move(sweep);
		if (movingFound_.empty()) {
			moving_.erase(key);
		} else {
			moving_[key] = std::move(movingFound_);
		}
		movingFound_.clear();
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
		// With no car round the player at the last look, every third time as often: those rays found
		// nothing nearly all the time.
		if (++vehicleFrame_ < (vehicleBlocks_.empty() ? cfg.vehicleFrames * 3 : cfg.vehicleFrames)) {
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
				// The sample stands for its whole cell, so cars are continuous to arrows and items too.
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
			logger::info("collision: cars: {} scans, {} rays, {} columns of car in the last 5 s; {} blocks of car around the player now; the city's sweep {:.2f} ms a frame",
				vehicleScans_, vehicleRays_, vehicleCells_, vehicleBlocks_.size(), updateFrames_ ? updateMs_ / updateFrames_ : 0.0);
			vehicleScans_ = 0;
			vehicleRays_ = 0;
			vehicleCells_ = 0;
			updateMs_ = 0.0;
			updateFrames_ = 0;
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

	void Collision::ProbeCell(const McVec& a_block)
	{
		const auto bx = FloorI(a_block.x), by = FloorI(a_block.y), bz = FloorI(a_block.z);
		const auto mpb = MetresPerBlock();
		const auto centre = McToCp(bx + 0.5, by + 0.5, bz + 0.5);
		logger::info("cityprobe: block ({}, {}, {}), its centre at Cyberpunk ({:.2f}, {:.2f}, {:.2f}), {:.2f} m a block; the sweep's window y {}..{}", bx, by, bz,
			centre.x, centre.y, centre.z, mpb, yMin_, yMax_);

		// What Minecraft has there: the sweep's sub-voxels, and the guard's.
		const auto rx = FloorDiv(bx, kRegionSize), ry = FloorDiv(by, kRegionSize), rz = FloorDiv(bz, kRegionSize);
		const auto local = static_cast<std::uint32_t>((bx - rx * kRegionSize) + kRegionSize * ((by - ry * kRegionSize) + kRegionSize * (bz - rz * kRegionSize)));
		auto solid = [&](const Regions& a_regions) -> int {
			const auto region = a_regions.find(RegionKey(rx, ry, rz));
			if (region == a_regions.end()) {
				return -1;
			}
			const auto block = region->second.blocks.find(local);
			int        count = 0;
			if (block != region->second.blocks.end()) {
				for (const auto bits : block->second) {
					count += std::popcount(bits);
				}
			}
			return count;
		};
		logger::info("cityprobe:   Minecraft was sent {} of 512 sub-voxels solid by the sweep{}, {} by the guard", std::max(solid(sent_), 0),
			solid(sent_) < 0 ? " (its region wasn't sent)" : "", std::max(solid(guard_), 0));

		auto* system = rtti::System("gameSpatialQueriesSystem");
		if (!system) {
			logger::info("cityprobe:   no gameSpatialQueriesSystem (no save loaded)");
			return;
		}
		spatial_ = system;
		// Collision groups by name only (an unknown preset name crashed the game; groups don't).
		static constexpr std::array<const char*, 12> kGroups{ "Static", "Terrain", "Dynamic", "Destructible", "DestructibleCluster", "Collider", "Debris",
			"PlayerBlocker", "VehicleBlocker", "NPCBlocker", "Vehicle", "Water" };
		static rtti::Method overlap{ "gameSpatialQueriesSystem", "Overlap" };
		static rtti::Method byGroup{ "gameSpatialQueriesSystem", "SyncRaycastByCollisionGroup" };

		// Is anything of each group inside the block? A box a little smaller than it (0.45 of a block,
		// whether the game takes that as half or whole extents), unturned.
		const auto           size = static_cast<float>(0.45 * mpb);
		RED4ext::Vector4     dimension{ size, size, size, 0.0f };
		RED4ext::Vector4     position{ centre.x, centre.y, centre.z, 1.0f };
		RED4ext::EulerAngles rotation{};
		for (const auto* name : kGroups) {
			RED4ext::CName                group(name);
			RED4ext::physics::TraceResult result{};
			bool                          hit = false;
			const bool                    called = overlap.Call(system, &hit, dimension, position, rotation, group, result);
			if (called && !hit) {
				continue;  // only what's there
			}
			logger::info("cityprobe:   box {:.2f} m, {}: {}{}", size, name, !called ? "call failed" : "SOLID",
				hit ? std::format(" (material {})", MaterialName(result.material)) : std::string{});
		}

		// Rays through the block's centre, from a block before it to a block past it: what each group
		// has there, static bodies only and moving ones too.
		struct Line
		{
			const char*      what;
			RED4ext::Vector4 from, to;
		};
		const auto  reach = static_cast<float>(1.5 * mpb);
		const float cx = centre.x, cy = centre.y, cz = centre.z;
		const std::array<Line, 5> lines{ Line{ "down", { cx, cy, cz + reach, 1.0f }, { cx, cy, cz - reach, 1.0f } },
			Line{ "east", { cx - reach, cy, cz, 1.0f }, { cx + reach, cy, cz, 1.0f } }, Line{ "west", { cx + reach, cy, cz, 1.0f }, { cx - reach, cy, cz, 1.0f } },
			Line{ "north", { cx, cy - reach, cz, 1.0f }, { cx, cy + reach, cz, 1.0f } }, Line{ "south", { cx, cy + reach, cz, 1.0f }, { cx, cy - reach, cz, 1.0f } } };
		for (const auto& line : lines) {
			int hits = 0;
			for (const auto* name : kGroups) {
				for (const bool staticOnly : { true, false }) {
					RED4ext::CName                group(name);
					RED4ext::physics::TraceResult result{};
					bool                          hit = false;
					bool                          only = staticOnly;
					bool                          dynamicOnly = false;
					if (!byGroup.Call(system, &hit, line.from, line.to, group, result, only, dynamicOnly) || !hit) {
						continue;
					}
					++hits;
					const auto dx = result.position.X - line.from.X, dy = result.position.Y - line.from.Y, dz = result.position.Z - line.from.Z;
					const auto along = std::sqrt(dx * dx + dy * dy + dz * dz);
					// The block spans 1 to 2 blocks along the line.
					logger::info("cityprobe:   ray {}, {} ({}): hit {:.2f} blocks along ({}), normal ({:.2f}, {:.2f}, {:.2f}), material {}", line.what, name,
						staticOnly ? "static bodies" : "all bodies", along / mpb, along / mpb < 1.0 ? "before the block" : along / mpb <= 2.0 ? "IN the block" : "past it",
						result.normal.X, result.normal.Y, result.normal.Z, MaterialName(result.material));
				}
			}
			if (hits == 0) {
				logger::info("cityprobe:   ray {}: nothing in any group", line.what);
			}
		}

		// The sweep's own scan of the block, every query logged: its octree from the block down, then
		// each solid cell's faces.
		if (dimension_ == Dimension::kUnknown) {
			logger::info("cityprobe:   the sweep's box queries aren't worked out yet (see \"collision: no sweep yet\")");
			return;
		}
		auto       savedCells = std::move(cells_);
		const auto savedKnown = known_;
		cells_.clear();
		known_ = { bx * kCellsPerBlock, by * kCellsPerBlock, bz * kCellsPerBlock, (bx + 1) * kCellsPerBlock, (by + 1) * kCellsPerBlock,
			(bz + 1) * kCellsPerBlock };
		std::unordered_map<std::uint64_t, RED4ext::CName> found;
		std::vector<Node>                                 stack{ Node{ bx * kCellsPerBlock, by * kCellsPerBlock, bz * kCellsPerBlock, kCellsPerBlock } };
		if (Builds::Filled(bx, by, bz, bx + 1, by + 1, bz + 1)) {
			logger::info("cityprobe:   it's one of the player's placed blocks: the sweep leaves it out");
			stack.clear();
		}
		while (!stack.empty()) {
			const auto node = stack.back();
			stack.pop_back();
			RED4ext::CName material{};
			const bool     hit = OverlapsNode(node, &material);
			if (node.size > 1 || hit) {
				logger::info("cityprobe:     box of {} cells at ({}, {}, {}): {}", node.size, node.x, node.y, node.z,
					hit ? std::format("SOLID ({})", MaterialName(material)) : std::string("empty"));
			}
			if (!hit) {
				continue;
			}
			if (node.size > 1) {
				const auto half = node.size / 2;
				for (int i = 0; i < 8; ++i) {
					stack.push_back({ node.x + (i & 1) * half, node.y + ((i >> 1) & 1) * half, node.z + ((i >> 2) & 1) * half, half });
				}
				continue;
			}
			if (material == OwnMaterial()) {
				const McVec lo{ node.x * kCell + kPad, node.y * kCell + kPad, node.z * kCell + kPad };
				const McVec hi{ (node.x + 1) * kCell - kPad, (node.y + 1) * kCell - kPad, (node.z + 1) * kCell - kPad };
				const bool  again = Overlaps(lo, hi, &material);
				logger::info("cityprobe:       one of the player's colliders; a hair smaller: {}", again ? MaterialName(material) : "empty");
				if (!again || material == OwnMaterial()) {
					continue;
				}
			}
			if (IsWater(material) || (bodyMaterial_ != RED4ext::CName() && material == bodyMaterial_)) {
				continue;
			}
			cells_.insert(CellKey(node.x, node.y, node.z));
			found[CellKey(node.x, node.y, node.z)] = material;
		}
		explain_ = true;
		int subs = 0;
		for (const auto& [key, material] : found) {
			const auto x = Unpack21(key >> 42), y = Unpack21(key >> 21), z = Unpack21(key);
			int        rays = 0;
			const auto bits = Carve(x, y, z, rays);
			subs += std::popcount(bits);
			logger::info("cityprobe:   cell ({}, {}, {}) of {}: {} of its 8 sub-voxels solid ({:08b}, bit x + 2z + 4y)", x, y, z, MaterialName(material),
				std::popcount(bits), bits);
		}
		explain_ = false;
		cells_ = std::move(savedCells);
		known_ = savedKnown;
		logger::info("cityprobe:   the sweep makes {} of its 64 cells solid, {} of 512 sub-voxels (the faces trimmed with the block alone known)", found.size(), subs);
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

	std::optional<McVec> Collision::FirstHit(const McVec& a_from, const McVec& a_to)
	{
		RED4ext::Vector4 hit{};
		RED4ext::Vector4 normal{};
		RED4ext::CName   material{};
		if (!Raycast(a_from, a_to, hit, normal, material)) {
			return std::nullopt;
		}
		return McVec{ hit.X, hit.Y, hit.Z };
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
		if (!RaycastGroups(groups, true, a_from, a_to, a_hit, a_normal, a_material, a_inside)) {
			return false;
		}
		if (a_inside && *a_inside) {
			return true;  // not a surface
		}
		++hits_;
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
