#pragma once

#include <RED4ext/Scripting/Natives/Generated/physics/QueryFilter.hpp>

#include "Link.h"

namespace cybercraft
{
	// Streams Cyberpunk's collision around the player to Minecraft as 1/8-block voxels
	// (DESIGN.md §5.1).
	//
	// RED4ext exposes no physics world to walk and voxelize, so this asks the game's physics instead
	// of guessing: gameSpatialQueriesSystem box overlaps, the way Cyberpunk itself asks "is anything
	// here". Nothing is solid to Minecraft unless a box query found something in it.
	//
	//   * an octree per region: an 8-block box that touches nothing costs one query; one that does is
	//     split in eight, down to quarter-block cells, each solid cell confirmed by its own query
	//   * then a ray into each face of a solid cell that has air beside it, from the air side: where
	//     the surface is inside the cell, to the 1/8 block (floors to stand on at their height)
	//
	// The queries take what blocks V: static and moving bodies (doors, gates, railings, kinematic
	// props) in the groups of sSolidGroups. Every probe of something V can't walk through found it in
	// PlayerBlocker; road barriers are Destructible, railings and doors aren't static bodies.
	//
	// A region only goes to Minecraft once all of it is scanned, so the ground right under the player
	// is also cast every frame (the guard) and sent at once if the sweep hasn't sent it: in a region
	// not scanned yet (a fall, a sprint).
	//
	// Cars move, so they aren't in that sweep. They get a layer of their own: every few frames, rays
	// in the vehicle group straight down and back up over a small circle around the player give each
	// car's top and underside, and the voxels between go to Minecraft as one message that replaces the
	// last (kColVehicles).
	//
	// Queries are game-object work, so everything here runs on the main thread, budgeted per frame
	// (iQueryBudget, fBudgetMs). Work goes region by region (8 blocks a side), nearest first to where
	// the player is heading; a region is only sent once all of it is in, so Minecraft never sees a
	// half-scanned one. One scanned a moment ago isn't scanned again for a while, and the one in
	// progress carries on when the sweep starts over from where the player has got to.
	//
	// [Collision] in CyberCraft.ini:
	//   bEnable            on/off                                     (1)
	//   sSolidGroups       what's solid: collision groups, any body   (Static,Terrain,Destructible,PlayerBlocker)
	//   sGroup             groups of the plugin's other rays (ground
	//                      checks, the third-person camera), static   (Static,Terrain)
	//   fRadius            horizontal scan radius, blocks             (32)
	//   fUp / fDown        scan window above/below the player, blocks (24 / 24)
	//   iQueryBudget       box queries and rays per frame             (2000)
	//   fBudgetMs          and at most this long per frame            (3)
	//   fRebuildDist       player movement that restarts the sweep    (4; up and down, a third of
	//                      the smaller of fUp / fDown if that's more)
	//   bGuard             cast the ground under the player per frame (1)
	//   fGuardDepth        how far under the feet it looks, blocks    (8)
	//   iSamplesPerBlock   guard and car rays per block edge          (2)
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
		// The grid moved under Night City (Grid::MovedSince): Minecraft moves the holes dug into the
		// city with it. Goes out ahead of the next kColClear. Main thread.
		void SendGridMoved(const proto::ColGridMoved& a_move);
		// Once per frame, with the player in Minecraft coordinates. Cheap when disabled.
		void Update(const McVec& a_playerMc);
		// Once per frame after Update: the cars around the player. a_active false (V in a car, or
		// Minecraft not driving her) takes the cars out of Minecraft's world until it's true again.
		void UpdateVehicles(const McVec& a_playerMc, bool a_active);

		// Surfaces the sweep and the guard have found since the last Reset. Zero means the queries
		// come back empty, and Minecraft must not drive V through a world it cannot see.
		[[nodiscard]] std::uint64_t Hits() const { return hits_; }

		// Night City's water surface nearest the player (Minecraft y), NaN with none in the water
		// grid. Any thread: the blocks' renderer fades what's under it.
		[[nodiscard]] float WaterLevel() const { return waterLevel_.load(std::memory_order_relaxed); }

		// Diagnostics: one straight-down ray at a Cyberpunk position through every raycast variant
		// the game offers, each result logged.
		static void ProbeRays(const RED4ext::Vector4& a_cpPos);

		// /cityprobe (kEvCityProbe): logs what Cyberpunk's physics has in the Minecraft block whose
		// lowest corner is a_block (box overlaps and rays, group by group), what Minecraft was sent
		// there, and the sweep's own queries for it again, cell by cell. Game thread.
		void ProbeCell(const McVec& a_block);

		// True if Cyberpunk's static world (sGroup) stands between a_from and a_to, more than
		// a_margin short of a_to. Minecraft coordinates, game thread.
		bool Blocked(const McVec& a_from, const McVec& a_to, double a_margin);

		// Height (Minecraft y) of the first static surface from a_from towards a_to, if any.
		std::optional<double> FirstHitY(const McVec& a_from, const McVec& a_to);

		// Where the first static surface from a_from towards a_to is, if any.
		std::optional<McVec> FirstHit(const McVec& a_from, const McVec& a_to);

		// How far from a_from the first static surface towards a_to is (blocks), if any.
		std::optional<double> HitDistance(const McVec& a_from, const McVec& a_to);

		// Diagnostics: what Minecraft was sent ahead of a_feet along (a_dirX, a_dirZ), one line per
		// eighth of a block out to a block: the sub-voxels from a quarter block under the feet to two
		// blocks up ('#' solid), at the player's left edge, middle and right edge.
		[[nodiscard]] std::string DescribeAhead(const McVec& a_feet, double a_dirX, double a_dirZ) const;

		static constexpr int kRegionSize = 8;  // blocks per region edge (must match the Java side)
		static constexpr int kCellsPerBlock = 4;  // the octree's smallest box: a quarter block, 2 sub-voxels

	private:
		struct Settings
		{
			bool        enable;
			std::string solidGroups;  // comma-separated collision group names: what the sweep finds solid
			std::string group;        // the other rays' groups (static bodies)
			float       radius, up, down;
			int         samplesPerBlock, queryBudget;
			float       budgetMs, rebuildDist;
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

		// One region: scanned as a unit, then sent.
		struct Job
		{
			std::int32_t rx, ry, rz;
			bool         rescan;  // scanned not long ago: what's left once the new ones are in
			bool         coarse;  // further than kFineBlocks: half-block cells, no faces trimmed
		};

		// An octree box: its lowest corner and edge in cells (kCellsPerBlock to a block), and whether a car
		// may be in it (its region had one).
		struct Node
		{
			std::int32_t x, y, z;
			std::int32_t size;
			bool         car{ false };
		};

		// How the game takes a box's primitiveDimension, and how to ask for several groups at once:
		// found out on the first floor under the player (Calibrate).
		enum class Dimension
		{
			kUnknown,
			kHalf,  // half extents, as PhysX boxes have them
			kFull,
		};

		static const Settings& Config();
		void Rebuild(const McVec& a_playerMc);
		// The region at the back of pending_, a_budget queries further on (its octree, then the faces
		// of its solid cells). True once it's all in.
		bool Advance(int& a_budget, std::chrono::steady_clock::time_point a_deadline);
		// The box queries, from the root regions down; a_budget, a_deadline as Advance.
		void Descend(int& a_budget, std::chrono::steady_clock::time_point a_deadline);
		// A solid cell's sub-voxels: those beyond the surface, seen from each side with air, stay.
		// Returns the 8 sub-voxels as bits (x + 2 * (z + 2 * y)), and the rays it cast.
		std::uint8_t Carve(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, int& a_rays);
		// The region's solid cells as sub-voxels into regions_.
		void MarkCell(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z, std::uint8_t a_bits);
		// A box deep inside a solid, all of it solid (whole blocks at a time where it covers them).
		void MarkSolid(const Node& a_node);
		void Flush(const Job& a_job);
		// The region as Minecraft should have it, what the sweep found (a_sweep, may be null) and what
		// the guard found, unless Minecraft has exactly that already.
		void SendRegion(std::uint64_t a_key, std::int32_t a_rx, std::int32_t a_ry, std::int32_t a_rz, const Region* a_sweep);
		// The ground under and just round the player, sent at once wherever Minecraft doesn't have it.
		void Guard(const McVec& a_playerMc);
		// One sub-voxel solid (by its index: a block is 8 to an edge).
		void SetSub(Regions& a_into, std::int32_t a_gx, std::int32_t a_gy, std::int32_t a_gz);
		// Sub-voxel indices whose centres fall in [a_centre - a_half, a_centre + a_half).
		static std::pair<std::int32_t, std::int32_t> SubRange(double a_centre, double a_half);

		// The query filter of sSolidGroups, built by the game's own physicsQueryFilter.AddGroup.
		bool SolidFilter(RED4ext::physics::QueryFilter& a_filter);
		// Is anything solid (sSolidGroups, any body) in the Minecraft box [a_min, a_max]? a_material: what.
		// Counts one query.
		bool Overlaps(const McVec& a_min, const McVec& a_max, RED4ext::CName* a_material, bool* a_moving = nullptr);
		// The same, a node: inset a hair across so a face exactly on its side isn't its, lifted a hair
		// so a floor exactly on its bottom is the node's below (the ground the grid was aligned to).
		bool OverlapsNode(const Node& a_node, RED4ext::CName* a_material, bool* a_moving = nullptr);
		// Is a car (sVehicleGroup, any body) in the node? Cars are the car layer's (kColVehicles: solid,
		// moving with them, never dug), so the city's sweep leaves them out. Counts one query.
		bool CarIn(const Node& a_node);
		// The cells of moving bodies (doors, gates) round the player asked about again, a few times a
		// second: a door opened (or shut again) reaches Minecraft without waiting for its region's rescan.
		void Watch(int& a_budget, std::chrono::steady_clock::time_point a_deadline);
		// The first solid surface (sSolidGroups, any body) on a_from..a_to, the player's own colliders
		// passed through. Counts one query a call to the game.
		bool RaySolid(const McVec& a_from, const McVec& a_to, RED4ext::Vector4& a_hit, RED4ext::Vector4& a_normal, RED4ext::CName& a_material,
			float& a_distance);
		// Box and query semantics, from a floor next to a_playerMc. True once known.
		bool Calibrate(const McVec& a_playerMc);
		// A box of half extents a_half (metres, Cyberpunk axes) about a_centre, as the game's queries
		// take it (byFilter_), dimension as given: what Overlaps and Calibrate call. a_moving: set if
		// what it found isn't a static body (a door, a gate).
		bool OverlapRaw(const RED4ext::Vector4& a_dimension, const RED4ext::Vector4& a_centre, RED4ext::CName* a_material, bool* a_moving = nullptr);

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
		[[nodiscard]] bool SentSolid(double a_x, double a_y, double a_z) const;
		// Water the down rays hit, as a height per block column around the player (DESIGN.md §5.2).
		// Minecraft's own swimming physics then applies unchanged.
		void PublishWater();
		// The first water surface (the "Water" collision group) on a_from..a_to, as a Minecraft y.
		bool WaterSurface(const McVec& a_from, const McVec& a_to, double& a_surfaceY);
		// The water grid's columns not looked down yet, nearest the player first, a_budget rays on:
		// water is apart from what's solid (its own group, or the city's static water surfaces, which
		// the box queries leave out), so it has a pass of its own.
		void SampleWater(int& a_budget, std::chrono::steady_clock::time_point a_deadline);
		// One block column's water surface, if any: the Water group's, else the first static surface of
		// water material down through the city (a bridge over it, a pier). Returns the rays used.
		int WaterColumn(double a_mcX, double a_mcZ, double a_top, double a_bottom);
		// Re-centres the water grid on the player, keeping the water already found (a_clear: none).
		void ShiftWater(const McVec& a_playerMc, bool a_clear);
		// The water surface in the grid closest to the player (Minecraft y), NaN if none.
		float NearestWater(const McVec& a_playerMc) const;
		void RecordWater(double a_mcX, double a_mcZ, double a_surfaceY);

		std::uint32_t                                  epoch_{ 0 };
		bool                                           cleared_{ false };
		std::vector<proto::ColGridMoved>               gridMoves_;  // waiting for room in the ring
		std::vector<Job>                               pending_;  // nearest region last (pop_back)
		Regions                                         regions_;  // the region being scanned
		std::unordered_map<std::uint64_t, std::size_t>  sentHash_;
		// Each region near the player as the sweep last found it, and what the guard has found there
		// since (sent with it every time, so a sweep that missed it doesn't take it away again).
		Regions                                         sent_;
		Regions                                         guard_;
		Regions                                         guardScratch_;
		std::unordered_map<std::uint32_t, Mask>         merged_;
		std::int32_t                                    yMin_{ 0 }, yMax_{ 0 };  // scan window, blocks
		McVec                                           origin_{ 0, 0, 0 };      // where the sweep started
		McVec                                           player_{ 0, 0, 0 };      // where the player is now
		McVec                                           velocity_{ 0, 0, 0 };    // blocks a second, smoothed
		std::chrono::steady_clock::time_point           movedAt_{};              // when player_ was read
		bool                                            sweeping_{ false };
		std::chrono::steady_clock::time_point           sweptAt_{};              // when the last sweep started
		static constexpr std::chrono::seconds           kResweepAfter{ 3 };     // standing still: sweep again after
		// When each region was last scanned, by RegionKey, and whether coarsely: not again within
		// kResweepAfter round the player (a door opened, something knocked over), kFreshFor further out.
		// Walking, a sweep every few blocks used to scan everything round the player again, the city
		// unchanged. One scanned coarsely is scanned again finely once the player is within kFineBlocks.
		struct Scanned
		{
			std::chrono::steady_clock::time_point at;
			bool                                  coarse;
		};
		std::unordered_map<std::uint64_t, Scanned>      scanned_;
		static constexpr std::chrono::seconds           kFreshFor{ 10 };
		static constexpr double                         kNearBlocks = 12.0;
		// Regions further than this (blocks, from their middle) are scanned coarsely: half-block cells,
		// solid whole, no faces trimmed. Five times cheaper, so the far ones are in before long (thrown
		// pearls and arrows, what the crosshair meets); quarter-block cells round the player.
		static constexpr double                         kFineBlocks = 16.0;

		// The region in progress (pending_.back()): octree boxes still to ask, the solid cells found
		// (CellKey), then the faces still to do; cells just outside it asked about for its faces
		// (CellKey -> solid).
		enum class Phase
		{
			kStart,
			kOctree,
			kCells,
		};
		Phase                                           phase_{ Phase::kStart };
		std::vector<Node>                               stack_;
		std::unordered_set<std::uint64_t>               cells_;
		std::vector<std::uint64_t>                      cellList_;
		std::unordered_map<std::uint64_t, bool>         outside_;
		// Solid cells of moving bodies: those of the region in progress (CellKey), and by region those
		// of the regions sent, each with the sub-voxels it was sent with and whether it's there now.
		struct Moving
		{
			std::uint64_t cell;
			std::uint8_t  bits;
			bool          there;
		};
		std::unordered_set<std::uint64_t>                          movingCells_;
		std::vector<Moving>                                        movingFound_;
		std::unordered_map<std::uint64_t, std::vector<Moving>>     moving_;
		std::chrono::steady_clock::time_point                      watchedAt_{};
		std::uint64_t                                               watchQueries_{ 0 }, watchChanges_{ 0 }, sweepMoving_{ 0 };
		std::size_t                                     nextCell_{ 0 };
		// The cells whose solidity cells_ has (x0, y0, z0, x1, y1, z1, ends exclusive): beside them, Carve
		// asks about the cell next door itself (outside_).
		std::array<std::int32_t, 6>                     known_{};
		RED4ext::IScriptable*                           spatial_{ nullptr };  // gameSpatialQueriesSystem, this frame

		Dimension                                       dimension_{ Dimension::kUnknown };
		bool                                            byFilter_{ true };     // boxes: one filtered query, else one per group
		bool                                            rayByFilter_{ true };  // rays: the same
		RED4ext::CName                                  bodyMaterial_{};       // V's own body, if the queries see it
		int                                             calibrationTries_{ 0 };
		// /cityprobe: log every query and ray of the sweep's scan of the block again.
		bool                                            explain_{ false };

		double                                          updateMs_{ 0.0 };  // time in Update since the last log
		std::uint32_t                                   updateFrames_{ 0 };
		proto::WaterGrid                                water_{};
		std::array<bool, proto::kWaterGridSize * proto::kWaterGridSize> waterSampled_{};  // as water_.surface
		bool                                            waterDirty_{ false };
		std::atomic<float>                              waterLevel_{ std::numeric_limits<float>::quiet_NaN() };
		std::vector<proto::ColBlock>                    scratch_;
		std::vector<std::uint8_t>                       payload_;
		std::uint64_t                                   hits_{ 0 };
		// Since the last sweep log: box queries (and those that found something), solid cells, face
		// rays (and those that trimmed a cell), rays through the player's own colliders, cells left out
		// for being the player's placed blocks or inside Minecraft's player, water surfaces, and the
		// materials of the solid cells.
		std::uint64_t                                   sweepBoxes_{ 0 }, sweepBoxHits_{ 0 }, sweepCells_{ 0 }, sweepRays_{ 0 }, sweepTrims_{ 0 }, sweepInterior_{ 0 };
		std::uint64_t                                   sweepOwn_{ 0 }, sweepBuilt_{ 0 }, sweepBody_{ 0 }, waterHits_{ 0 }, guardFills_{ 0 }, guardRays_{ 0 };
		std::uint64_t                                   sweepRegions_{ 0 }, sweepCars_{ 0 };
		std::unordered_map<std::uint64_t, std::uint32_t> sweepMaterials_;  // CName hash -> cells

		// The car layer: what the last scan found (block -> mask), and what Minecraft has.
		std::unordered_map<std::uint64_t, proto::ColBlock> vehicleBlocks_;
		std::optional<std::size_t>                      vehicleHash_;  // of the layer last sent this epoch
		int                                             vehicleFrame_{ 0 };
		std::uint64_t                                   vehicleRays_{ 0 }, vehicleScans_{ 0 }, vehicleCells_{ 0 };  // since the last log
	};
}
