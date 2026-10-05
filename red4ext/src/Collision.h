#pragma once

#include "Link.h"

namespace cybercraft
{
	// Streams Cyberpunk's collision around the player to Minecraft as 1/8-block voxels
	// (DESIGN.md §5.1, stage A: ray casts, no shape decoding).
	//
	// RED4ext exposes no physics world to walk and voxelize, so this samples the world with
	// gameSpatialQueriesSystem rays:
	//
	//   * one downward multi-hit ray per sample column -> floors, roads, ledges, overhangs
	//   * short +X / +Z probes at knee/waist/head height -> walls, railings, crates
	//
	// A pillar only goes to Minecraft once all of it is scanned, so the ground right under the player
	// is also cast every frame (the guard) and sent at once if the sweep hasn't sent it: in a pillar
	// not scanned yet (a fall, a sprint), or anywhere the sweep missed it.
	//
	// Cars move, so they aren't in that sweep (its rays only see static geometry). They get a layer
	// of their own: every few frames, rays in the vehicle group straight down and back up over a
	// small circle around the player give each car's top and underside, and the voxels between go
	// to Minecraft as one message that replaces the last (kColVehicles).
	//
	// Rays are game-object work, so everything here runs on the main thread, budgeted per frame
	// (iRayBudget). Work is ordered by region column, nearest the player first; a column is only
	// sent once every one of its samples is in, so Minecraft never sees a half-scanned region.
	//
	// [Collision] in CyberCraft.ini:
	//   bEnable            on/off                                     (1)
	//   sGroup             collision groups for the rays              (Static,Terrain)
	//   fRadius            horizontal scan radius, blocks             (32)
	//   fUp / fDown        scan window above/below the player, blocks (24 / 24)
	//   iSamplesPerBlock   sample columns per block edge              (2)
	//   iRayBudget         rays per frame                             (2000)
	//   iMaxHitsPerColumn  surfaces found per downward ray            (16)
	//   fSteepNormalY      |normal.y| below this is a wall, not floor (0.64 ~ 50 degrees)
	//   fWallHeight        wall column raised above a steep hit       (2.5)
	//   fRebuildDist       player movement that restarts the sweep    (4; up and down, a third of
	//                      the smaller of fUp / fDown if that's more)
	//   bGuard             cast the ground under the player per frame (1)
	//   fGuardDepth        how far under the feet it looks, blocks    (8)
	//   bVehicles          cars collide with Minecraft's player       (1)
	//   sVehicleGroup      collision groups cars are in               (Vehicle)
	//   fVehicleRadius     how far round the player cars are, blocks  (6)
	//   iVehicleFrames     frames between car scans                   (2)
	class Collision
	{
	public:
		static Collision& Get();

		// Drops everything and tells Minecraft to clear its store. Call on world/link change.
		void Reset(std::uint32_t a_epoch);
		// Once per frame, with the player in Minecraft coordinates. Cheap when disabled.
		void Update(const McVec& a_playerMc);
		// Once per frame after Update: the cars around the player. a_active false (V in a car, or
		// Minecraft not driving her) takes the cars out of Minecraft's world until it's true again.
		void UpdateVehicles(const McVec& a_playerMc, bool a_active);

		// Surfaces the rays have found since the last Reset. Zero means the raycasts come back empty,
		// and Minecraft must not drive V through a world it cannot see.
		[[nodiscard]] std::uint64_t Hits() const { return hits_; }

		// Night City's water surface nearest the player (Minecraft y), NaN with none in the water
		// grid. Any thread: the blocks' renderer fades what's under it.
		[[nodiscard]] float WaterLevel() const { return waterLevel_.load(std::memory_order_relaxed); }

		// Diagnostics: one straight-down ray at a Cyberpunk position through every raycast variant
		// the game offers, each result logged.
		static void ProbeRays(const RED4ext::Vector4& a_cpPos);

		// True if Cyberpunk's static world (the same groups the collision rays use) stands between
		// a_from and a_to, more than a_margin short of a_to. Minecraft coordinates, game thread.
		bool Blocked(const McVec& a_from, const McVec& a_to, double a_margin);

		// Height (Minecraft y) of the first static surface from a_from towards a_to, if any.
		std::optional<double> FirstHitY(const McVec& a_from, const McVec& a_to);

		// How far from a_from the first static surface towards a_to is (blocks), if any.
		std::optional<double> HitDistance(const McVec& a_from, const McVec& a_to);

		// Diagnostics: what Minecraft was sent ahead of a_feet along (a_dirX, a_dirZ), one line per
		// eighth of a block out to a block: the sub-voxels from a quarter block under the feet to two
		// blocks up ('#' solid), at the player's left edge, middle and right edge.
		[[nodiscard]] std::string DescribeAhead(const McVec& a_feet, double a_dirX, double a_dirZ) const;

		static constexpr int kRegionSize = 8;  // blocks per region edge (must match the Java side)

	private:
		struct Settings
		{
			bool        enable;
			std::string group;  // collision group CName for the rays
			float        radius, up, down;
			int          samplesPerBlock, rayBudget, maxHitsPerColumn;
			float        steepNormalY, wallHeight, rebuildDist;
			bool        guard;
			float       guardDepth;
			bool        vehicles;
			std::string vehicleGroup;  // comma-separated, like group
			float       vehicleRadius;
			int         vehicleFrames;
		};

		using Mask = std::array<std::uint64_t, 8>;  // one block: bits[y] bit (z * 8 + x)

		struct Region
		{
			std::unordered_map<std::uint32_t, Mask> blocks;  // bx + 8 * (by + 8 * bz), region-local
		};
		using Regions = std::unordered_map<std::uint64_t, Region>;  // by RegionKey

		// One (rx, rz) pillar of regions: scanned as a unit, then flushed together.
		struct Column
		{
			std::int32_t rx, rz;
			int          next{ 0 };  // next sample index within the pillar
		};

		static const Settings& Config();
		void            Rebuild(const McVec& a_playerMc);
		// Spends about a_budget rays on the pillar, advancing a_column.next: a column once started is
		// finished, so it can go over. Returns the rays used; the pillar is done when next reaches
		// Samples().
		int  Sample(Column& a_column, int a_budget);
		// One sample column, all of it. Returns the rays used.
		int  ScanColumn(double a_mcX, double a_mcZ);
		[[nodiscard]] int Samples() const;
		void Flush(const Column& a_column);
		// The region as Minecraft should have it, what the sweep found (a_sweep, may be null) and what
		// the guard found, unless Minecraft has exactly that already.
		void SendRegion(std::uint64_t a_key, std::int32_t a_rx, std::int32_t a_ry, std::int32_t a_rz, const Region* a_sweep);
		// The ground under and just round the player, sent at once wherever Minecraft doesn't have it.
		void Guard(const McVec& a_playerMc);
		// A down ray's hit: the sample's whole cell, following the surface's slope.
		void MarkSurface(Regions& a_into, double a_mcX, double a_mcZ, double a_surfaceY, const RED4ext::Vector4& a_normal);
		// A horizontal probe's hit (a_axis 0 = +X, 1 = +Z): the sample's cell across the probe
		// (a_across is the sample's coordinate on that axis), one sub-voxel into the face.
		void MarkWall(int a_axis, double a_across, const RED4ext::Vector4& a_hit, const RED4ext::Vector4& a_normal, double a_y0, double a_y1);
		// One 1/8-block column at a point (MarkSpan) or by sub-voxel index (MarkSub), a_y0..a_y1.
		void MarkSpan(Regions& a_into, double a_mcX, double a_mcZ, double a_y0, double a_y1);
		void MarkSub(Regions& a_into, std::int32_t a_gx, std::int32_t a_gz, double a_y0, double a_y1);
		// Sub-voxel indices whose centres fall in [a_centre - a_half, a_centre + a_half).
		static std::pair<std::int32_t, std::int32_t> SubRange(double a_centre, double a_half);
		// a_inside (optional): set if a_from is inside a solid shape instead of reporting a hit there.
		bool Raycast(const McVec& a_from, const McVec& a_to, RED4ext::Vector4& a_hit, RED4ext::Vector4& a_normal, RED4ext::CName& a_material,
			bool* a_inside = nullptr);
		// The nearest hit of any of a_groups. a_staticOnly: static geometry only (the city), else
		// moving bodies too (cars).
		bool RaycastGroups(const std::vector<RED4ext::CName>& a_groups, bool a_staticOnly, const McVec& a_from, const McVec& a_to,
			RED4ext::Vector4& a_hit, RED4ext::Vector4& a_normal, RED4ext::CName& a_material, bool* a_inside = nullptr);
		// The cars around a_playerMc, voxelized into vehicleBlocks_ and sent as one kColVehicles layer.
		void ScanVehicles(const McVec& a_playerMc);
		// vehicleBlocks_ as the layer for the block box [a_min, a_max], unless Minecraft has it already.
		void SendVehicles(const std::array<std::int32_t, 3>& a_min, const std::array<std::int32_t, 3>& a_max);
		// Water the down rays hit, as a height per block column around the player (DESIGN.md §5.2).
		// Minecraft's own swimming physics then applies unchanged.
		[[nodiscard]] bool SentSolid(double a_x, double a_y, double a_z) const;
		void PublishWater();
		// The first water surface (the "Water" collision group) on a_from..a_to, as a Minecraft y.
		bool WaterSurface(const McVec& a_from, const McVec& a_to, double& a_surfaceY);
		// Re-centres the water grid on the player, keeping the water already found (a_clear: none).
		void ShiftWater(const McVec& a_playerMc, bool a_clear);
		// The water surface in the grid closest to the player (Minecraft y), NaN if none.
		float NearestWater(const McVec& a_playerMc) const;
		void RecordWater(double a_mcX, double a_mcZ, double a_surfaceY);

		std::uint32_t                                  epoch_{ 0 };
		bool                                           cleared_{ false };
		std::vector<Column>                            pending_;  // nearest pillar last (pop_back)
		Regions                                         regions_;  // pillars being scanned
		std::unordered_map<std::uint64_t, std::size_t>  sentHash_;
		// Each region near the player as the sweep last found it, and what the guard has found there
		// since (sent with it every time, so a sweep that missed it doesn't take it away again).
		Regions                                         sent_;
		Regions                                         guard_;
		Regions                                         guardScratch_;
		std::unordered_map<std::uint32_t, Mask>         merged_;
		std::int32_t                                    yMin_{ 0 }, yMax_{ 0 };  // scan window, blocks
		McVec                                           origin_{ 0, 0, 0 };      // where the sweep started
		double                                          feetY_{ 0.0 };           // where the player is now
		bool                                            sweeping_{ false };
		proto::WaterGrid                                water_{};
		bool                                            waterDirty_{ false };
		std::atomic<float>                              waterLevel_{ std::numeric_limits<float>::quiet_NaN() };
		std::vector<proto::ColBlock>                    scratch_;
		std::vector<std::uint8_t>                       payload_;
		std::uint64_t                                   hits_{ 0 };
		std::uint64_t                                   sweepRays_{ 0 }, sweepHits_{ 0 };  // since the last sweep log
		std::uint64_t                                   waterHits_{ 0 };                   // since the last sweep log
		// Rays that met one of Builds' colliders and went on through: none while colliders stand
		// around V means Cyberpunk's physics never got them, and nothing (cars included) meets them.
		std::uint64_t                                   sweepOwn_{ 0 };
		// Down rays that started inside a solid shape and stepped on through it, columns that ran out
		// of iMaxHitsPerColumn, and regions the guard had to fill in under the player after the sweep
		// had sent them without that ground.
		std::uint64_t                                   sweepInside_{ 0 }, sweepCapped_{ 0 }, guardFills_{ 0 };

		// The car layer: what the last scan found (block -> mask), and what Minecraft has.
		std::unordered_map<std::uint64_t, proto::ColBlock> vehicleBlocks_;
		std::optional<std::size_t>                      vehicleHash_;  // of the layer last sent this epoch
		int                                             vehicleFrame_{ 0 };
		std::uint64_t                                   vehicleRays_{ 0 }, vehicleScans_{ 0 }, vehicleCells_{ 0 };  // since the last log
	};
}
