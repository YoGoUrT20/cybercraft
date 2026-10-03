#include "Builds.h"

#include "Config.h"
#include "Scripts.h"

namespace cybercraft::Builds
{
	namespace
	{
		struct Settings
		{
			bool  lights;
			int   maxLights;
			float lightRadius, lightIntensity, lightRange, flicker;
			bool  lightShadows;
			bool  colliders;
			int   maxColliders;
			float colliderRadius;
			bool  obstacles;
			int   spawnsPerFrame;
		};

		const Settings& Config()
		{
			static const Settings settings = [] {
				Settings s{};
				s.lights = Config::GetBool(L"Builds", L"bLights", true);
				s.maxLights = std::max(0, static_cast<int>(Config::GetFloat(L"Builds", L"iMaxLights", 48.0f)));
				s.lightRadius = Config::GetFloat(L"Builds", L"fLightRadius", 64.0f);
				s.lightIntensity = Config::GetFloat(L"Builds", L"fLightIntensity", 40.0f);
				s.lightRange = Config::GetFloat(L"Builds", L"fLightRangePerLevel", 0.8f);
				s.flicker = Config::GetFloat(L"Builds", L"fFlicker", 0.3f);
				s.lightShadows = Config::GetBool(L"Builds", L"bLightShadows", false);
				s.colliders = Config::GetBool(L"Builds", L"bColliders", true);
				s.maxColliders = std::max(0, static_cast<int>(Config::GetFloat(L"Builds", L"iMaxColliders", 1024.0f)));
				s.colliderRadius = Config::GetFloat(L"Builds", L"fColliderRadius", 48.0f);
				s.obstacles = Config::GetBool(L"Builds", L"bNavObstacle", true);
				s.spawnsPerFrame = std::max(1, static_cast<int>(Config::GetFloat(L"Builds", L"iSpawnsPerFrame", 16.0f)));
				return s;
			}();
			return settings;
		}

		std::uint64_t Key(std::int32_t a_x, std::int32_t a_y, std::int32_t a_z)
		{
			constexpr std::uint64_t kMask = (1ull << 21) - 1;
			return ((std::uint64_t(std::uint32_t(a_x)) & kMask) << 42) | ((std::uint64_t(std::uint32_t(a_y)) & kMask) << 21) |
			       (std::uint64_t(std::uint32_t(a_z)) & kMask);
		}

		using Bits = std::array<std::uint8_t, 512>;

		// Render thread -> main thread, in arrival order.
		struct Message
		{
			std::int32_t                  sx, sy, sz;
			bool                          isLights;
			std::vector<proto::RenLight>  lights;
			std::optional<Bits>           solids;
		};
		std::mutex           inboxMutex;
		std::vector<Message> inbox;
		bool                 inboxClear = false;

		struct Light
		{
			std::int32_t  x, y, z;  // Minecraft block
			std::uint32_t color;
			std::uint8_t  level;
		};

		struct Box
		{
			double cx, cy, cz;  // Minecraft blocks
			double hx, hy, hz;
		};

		struct Section
		{
			std::int32_t               sx, sy, sz;
			std::vector<Light>         lights;
			std::vector<Box>           boxes;
			std::vector<std::uint64_t> colliders;  // spawned, boxes[i] for colliders[i]
			bool                       stale{ false };  // colliders no longer match boxes
		};
		std::unordered_map<std::uint64_t, Section> sections;

		struct LiveLight
		{
			std::uint64_t entity;
			Light         light;
		};
		std::unordered_map<std::uint64_t, LiveLight> liveLights;  // by block
		std::size_t                                  liveColliders = 0;
		bool                                         warnedColliderLimit = false;
		// Every spawn this process, against what the script says it has given a component.
		int                                          spawnedLights = 0, spawnedColliders = 0;

		// Solid blocks of a section as few boxes: runs along x, grown along z, then along y.
		std::vector<Box> Merge(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const Bits& a_bits)
		{
			auto index = [](int x, int y, int z) { return x + 16 * z + 256 * y; };
			std::array<bool, 4096> used{};
			auto open = [&](int x, int y, int z) {
				const int i = index(x, y, z);
				return ((a_bits[i >> 3] >> (i & 7)) & 1) != 0 && !used[i];
			};
			std::vector<Box> boxes;
			for (int y = 0; y < 16; ++y) {
				for (int z = 0; z < 16; ++z) {
					for (int x = 0; x < 16; ++x) {
						if (!open(x, y, z)) {
							continue;
						}
						int x1 = x;
						while (x1 + 1 < 16 && open(x1 + 1, y, z)) {
							++x1;
						}
						auto rowOpen = [&](int yy, int zz) {
							for (int xx = x; xx <= x1; ++xx) {
								if (!open(xx, yy, zz)) {
									return false;
								}
							}
							return true;
						};
						int z1 = z;
						while (z1 + 1 < 16 && rowOpen(y, z1 + 1)) {
							++z1;
						}
						int y1 = y;
						while (y1 + 1 < 16) {
							bool layer = true;
							for (int zz = z; zz <= z1 && layer; ++zz) {
								layer = rowOpen(y1 + 1, zz);
							}
							if (!layer) {
								break;
							}
							++y1;
						}
						for (int yy = y; yy <= y1; ++yy) {
							for (int zz = z; zz <= z1; ++zz) {
								for (int xx = x; xx <= x1; ++xx) {
									used[index(xx, yy, zz)] = true;
								}
							}
						}
						boxes.push_back({ a_sx * 16.0 + (x + x1 + 1) * 0.5, a_sy * 16.0 + (y + y1 + 1) * 0.5, a_sz * 16.0 + (z + z1 + 1) * 0.5,
							(x1 - x + 1) * 0.5, (y1 - y + 1) * 0.5, (z1 - z + 1) * 0.5 });
					}
				}
			}
			return boxes;
		}

		void DespawnColliders(Section& a_section)
		{
			for (const auto entity : a_section.colliders) {
				Scripts::Despawn(entity);
			}
			liveColliders -= std::min(liveColliders, a_section.colliders.size());
			a_section.colliders.clear();
		}

		// What arrived from the render thread since the last frame.
		void Drain()
		{
			std::vector<Message> messages;
			bool                 clear = false;
			{
				std::lock_guard lock(inboxMutex);
				messages.swap(inbox);
				clear = std::exchange(inboxClear, false);
			}
			if (clear) {
				if (Scripts::Ready()) {
					Scripts::DespawnAll();
				}
				sections.clear();
				liveLights.clear();
				liveColliders = 0;
			}
			for (auto& message : messages) {
				auto& section = sections[Key(message.sx, message.sy, message.sz)];
				section.sx = message.sx;
				section.sy = message.sy;
				section.sz = message.sz;
				if (message.isLights) {
					section.lights.clear();
					for (const auto& light : message.lights) {
						section.lights.push_back({ message.sx * 16 + light.x, message.sy * 16 + light.y, message.sz * 16 + light.z, light.color, light.level });
					}
				} else {
					section.boxes = message.solids ? Merge(message.sx, message.sy, message.sz, *message.solids) : std::vector<Box>{};
					section.stale = true;
				}
			}
		}

		double DistanceSq(double a_x, double a_y, double a_z, const McVec& a_player)
		{
			const double dx = a_x - a_player.x, dy = a_y - a_player.y, dz = a_z - a_player.z;
			return dx * dx + dy * dy + dz * dz;
		}

		void UpdateColliders(const McVec& a_player, int& a_budget)
		{
			const auto& cfg = Config();
			const double nearSq = std::pow(cfg.colliderRadius + 14.0, 2.0);  // a section's centre to its corner is ~14
			const double farSq = std::pow(cfg.colliderRadius + 30.0, 2.0);
			std::vector<std::pair<double, Section*>> wanted;
			for (auto& [key, section] : sections) {
				const double distSq = DistanceSq(section.sx * 16 + 8.0, section.sy * 16 + 8.0, section.sz * 16 + 8.0, a_player);
				if (section.stale || distSq > farSq) {
					DespawnColliders(section);
					section.stale = false;
				}
				if (distSq <= nearSq && section.colliders.size() < section.boxes.size()) {
					wanted.emplace_back(distSq, &section);
				}
			}
			std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (auto& [distSq, section] : wanted) {
				while (a_budget > 0 && section->colliders.size() < section->boxes.size()) {
					if (liveColliders >= std::size_t(cfg.maxColliders)) {
						if (!warnedColliderLimit) {
							warnedColliderLimit = true;
							logger::warn("builds: {} colliders, the most iMaxColliders allows; farther blocks don't block NPCs", liveColliders);
						}
						return;
					}
					const auto& box = section->boxes[section->colliders.size()];
					const auto  centre = McToCp(box.cx, box.cy, box.cz);
					// Minecraft x, y, z extents are Cyberpunk x, z, y, in metres.
					const float size = float(MetresPerBlock());
					const auto  entity = Scripts::SpawnCollider(centre, CpVec{ float(box.hx) * size, float(box.hz) * size, float(box.hy) * size }, cfg.obstacles);
					--a_budget;
					if (entity == 0) {
						return;  // not spawning right now; try again next frame
					}
					section->colliders.push_back(entity);
					++liveColliders;
					++spawnedColliders;
				}
			}
		}

		void UpdateLights(const McVec& a_player, int& a_budget)
		{
			const auto& cfg = Config();
			struct Candidate
			{
				double        distSq;
				std::uint64_t key;
				const Light*  light;
			};
			std::vector<Candidate> candidates;
			const double           radiusSq = double(cfg.lightRadius) * cfg.lightRadius;
			for (const auto& [sectionKey, section] : sections) {
				for (const auto& light : section.lights) {
					const double distSq = DistanceSq(light.x + 0.5, light.y + 0.5, light.z + 0.5, a_player);
					if (distSq <= radiusSq) {
						candidates.push_back({ distSq, Key(light.x, light.y, light.z), &light });
					}
				}
			}
			std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.distSq < b.distSq; });
			if (candidates.size() > std::size_t(cfg.maxLights)) {
				candidates.resize(cfg.maxLights);
			}
			std::unordered_map<std::uint64_t, const Light*> wanted;
			for (const auto& candidate : candidates) {
				wanted.emplace(candidate.key, candidate.light);
			}
			for (auto it = liveLights.begin(); it != liveLights.end();) {
				const auto found = wanted.find(it->first);
				const bool same = found != wanted.end() && found->second->color == it->second.light.color && found->second->level == it->second.light.level;
				if (same) {
					++it;
					continue;
				}
				Scripts::Despawn(it->second.entity);
				it = liveLights.erase(it);
			}
			for (const auto& candidate : candidates) {
				if (a_budget <= 0) {
					return;
				}
				if (liveLights.contains(candidate.key)) {
					continue;
				}
				const auto&  light = *candidate.light;
				const float  r = float(light.color & 0xFF) / 255.0f;
				const float  g = float((light.color >> 8) & 0xFF) / 255.0f;
				const float  b = float((light.color >> 16) & 0xFF) / 255.0f;
				const auto   kind = static_cast<proto::LightKind>((light.color >> 24) & 0x0F);
				const float  level = float(light.level);
				// As far in blocks as the level says, however big a block is.
				const float  radius = std::max(1.0f, level * cfg.lightRange) * float(MetresPerBlock());
				const auto   entity = Scripts::SpawnLight(McToCp(light.x + 0.5, light.y + 0.5, light.z + 0.5), r, g, b, radius,
					  cfg.lightIntensity * level / 15.0f, kind == proto::kLightFlame ? cfg.flicker : 0.0f, cfg.lightShadows);
				--a_budget;
				if (entity == 0) {
					return;
				}
				liveLights.emplace(candidate.key, LiveLight{ entity, light });
				++spawnedLights;
			}
		}
	}

	void OnLights(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const proto::RenLight* a_lights, std::uint32_t a_count)
	{
		Message message{ a_sx, a_sy, a_sz, true, {}, std::nullopt };
		if (a_lights && a_count > 0) {
			message.lights.assign(a_lights, a_lights + a_count);
		}
		std::lock_guard lock(inboxMutex);
		inbox.push_back(std::move(message));
	}

	void OnSolids(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const std::uint8_t* a_bits)
	{
		Message message{ a_sx, a_sy, a_sz, false, {}, std::nullopt };
		if (a_bits) {
			Bits bits;
			std::memcpy(bits.data(), a_bits, bits.size());
			message.solids = bits;
		}
		std::lock_guard lock(inboxMutex);
		inbox.push_back(std::move(message));
	}

	void OnClearAll()
	{
		std::lock_guard lock(inboxMutex);
		inbox.clear();
		inboxClear = true;
	}

	void Update(const McVec& a_playerMc)
	{
		Drain();
		const auto& cfg = Config();
		if ((!cfg.lights && !cfg.colliders) || !Scripts::Ready()) {
			return;
		}
		int budget = cfg.spawnsPerFrame;
		if (cfg.colliders) {
			UpdateColliders(a_playerMc, budget);
		}
		// Lights are few and move slowly in and out of range: a few times a second is plenty.
		static int frame = 0;
		if (cfg.lights && ++frame % 6 == 0) {
			UpdateLights(a_playerMc, budget);
		}

		static auto nextLog = std::chrono::steady_clock::time_point{};
		const auto  now = std::chrono::steady_clock::now();
		if (Config::Diagnostics() && now >= nextLog) {
			nextLog = now + std::chrono::seconds(10);
			std::size_t lights = 0, boxes = 0;
			for (const auto& [key, section] : sections) {
				lights += section.lights.size();
				boxes += section.boxes.size();
			}
			logger::info("builds: {} light blocks ({} lit in Night City), {} boxes of solid blocks ({} colliders)", lights, liveLights.size(), boxes, liveColliders);
			// Spawned is only the entity; the light or collider is added when it reaches the script.
			const auto madeLights = Scripts::ComponentsMade(true);
			const auto madeColliders = Scripts::ComponentsMade(false);
			if (madeLights && madeColliders) {
				logger::info("builds: {} of {} lights and {} of {} colliders spawned so far got their component{}", *madeLights, spawnedLights,
					*madeColliders, spawnedColliders,
					*madeColliders < spawnedColliders / 2 ? "; the rest never reached CyberCraft.reds, so cars and NPCs go through them" : "");
			}
		}
	}

	void Respawn()
	{
		if (Scripts::Ready()) {
			Scripts::DespawnAll();
		}
		Forget();
	}

	void Forget()
	{
		liveLights.clear();
		liveColliders = 0;
		for (auto& [key, section] : sections) {
			section.colliders.clear();
			section.stale = false;
		}
	}
}
