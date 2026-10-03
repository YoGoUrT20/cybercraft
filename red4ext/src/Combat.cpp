#include "Combat.h"

#include <RED4ext/Scripting/Natives/Generated/ent/EntityID.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/StatsObjectID.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/TargetSearchQuery.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/targeting/TargetPartInfo.hpp>
#include <RED4ext/Scripting/Natives/Generated/game/data/StatPoolType.hpp>

#include "Config.h"
#include "Environment.h"
#include "Game.h"
#include "Rtti.h"

namespace cybercraft::Combat
{
	namespace
	{
		// Health is the only stat pool this needs, and its value is reflection data from the game
		// rather than a guess: gamedataStatPoolType::Health == 17.
		constexpr auto kHealthPool = RED4ext::game::data::StatPoolType::Health;

		constexpr const char* kTargetingSystem = "gametargetingTargetingSystem";

		// Signatures checked against the game's RTTI dump.
		rtti::Method getStatPool{ "gameStatPoolsSystem", "GetStatPoolValue" };
		rtti::Method changeStatPool{ "gameStatPoolsSystem", "RequestChangingStatPoolValue" };
		rtti::Method getEntityId{ "entEntity", "GetEntityID" };
		rtti::Method getDisplayName{ "gameObject", "GetDisplayName" };
		rtti::Method getWorldPosition{ "entEntity", "GetWorldPosition" };
		rtti::Method getWorldYaw{ "entEntity", "GetWorldYaw" };

		struct Settings
		{
			float toGame;    // Minecraft damage -> Cyberpunk health percent
			float fromGame;  // Cyberpunk health percent lost -> Minecraft damage
			float reach;     // how far the crosshair looks for a target, metres
			float remember;  // how long an NPC stays mirrored after it was last targeted or hit, seconds
		};

		const Settings& Config()
		{
			static const Settings settings = [] {
				using namespace cybercraft::Config;
				Settings s{};
				// DESIGN.md §13: Minecraft hits land hard on the host, host hits are divided down.
				s.toGame = GetFloat(L"Combat", L"fDamageToGame", 5.0f);
				s.fromGame = GetFloat(L"Combat", L"fDamageFromGame", 0.2f);
				s.reach = GetFloat(L"Combat", L"fTargetReach", 30.0f);
				s.remember = std::max(0.0f, GetFloat(L"Combat", L"fRememberSeconds", 30.0f));
				logger::info("combat: MC damage x{:.2f} to Cyberpunk, Cyberpunk health percent x{:.2f} to MC, reach {:.0f} m, targets remembered {:.0f} s",
					s.toGame, s.fromGame, s.reach, s.remember);
				return s;
			}();
			return settings;
		}

		// The protocol carries a 32-bit actor id; Cyberpunk entity ids are 64-bit, so they get a small
		// handle here and are mapped back when Minecraft reports a hit.
		std::unordered_map<std::uint32_t, std::uint64_t> idToEntity;
		std::unordered_map<std::uint64_t, std::uint32_t> entityToId;
		std::uint32_t                                    nextId = 1;
		float                                            lastPlayerHealth = -1.0f;

		// NPCs mirrored lately. One stays mirrored for a while after it leaves the crosshair, so
		// what Minecraft put on its stand-in carries on instead of going with it: potion effects
		// (their swirls, poison's ticks), and arrows and thrown potions still on their way to it.
		struct Remembered
		{
			RED4ext::WeakHandle<rtti::IScriptable> object;
			std::uint64_t                          entity{ 0 };
			std::chrono::steady_clock::time_point  seen;  // last under the crosshair, or hit
			bool                                   fresh{ true };
		};
		std::vector<Remembered> remembered;
		constexpr std::size_t   kMaxRemembered = 8;
		constexpr double        kRememberRange = 64.0;  // metres from V; further away, an NPC is let go

		std::uint32_t HandleFor(std::uint64_t a_entity)
		{
			if (const auto found = entityToId.find(a_entity); found != entityToId.end()) {
				return found->second;
			}
			const auto id = nextId++;
			entityToId.emplace(a_entity, id);
			idToEntity.emplace(id, a_entity);
			return id;
		}

		RED4ext::ent::EntityID EntityIdOf(const rtti::Handle<rtti::IScriptable>& a_object)
		{
			RED4ext::ent::EntityID id{};
			if (a_object) {
				getEntityId.Call(a_object.GetPtr(), &id);
			}
			return id;
		}

		// The stat pool system keys objects by gameStatsObjectID (16 bytes), not the 8-byte entity id.
		RED4ext::game::StatsObjectID StatsIdOf(RED4ext::ent::EntityID a_id)
		{
			RED4ext::game::StatsObjectID object{};
			object.entityHash = a_id.hash;
			object.idType = RED4ext::game::StatIDType::EntityID;
			return object;
		}

		// Health as a fraction, or -1 when the stat pool could not be read.
		float HealthFraction(RED4ext::ent::EntityID a_id)
		{
			auto* pools = rtti::System("gameStatPoolsSystem");
			if (!pools || a_id.hash == 0) {
				return -1.0f;
			}
			// GetStatPoolValue(objID: gameStatsObjectID, statPoolType, perc) -> Float, per the RTTI dump.
			float percent = -1.0f;
			auto  object = StatsIdOf(a_id);
			auto  type = kHealthPool;
			bool  asPercentage = true;
			if (!getStatPool.Call(pools, &percent, object, type, asPercentage) || percent < 0.0f) {
				return -1.0f;
			}
			return std::clamp(percent / 100.0f, 0.0f, 1.0f);
		}

		void ChangeHealth(RED4ext::ent::EntityID a_id, float a_percentDelta, const rtti::Handle<rtti::IScriptable>& a_instigator)
		{
			auto* pools = rtti::System("gameStatPoolsSystem");
			if (!pools || a_id.hash == 0) {
				return;
			}
			// RequestChangingStatPoolValue(objID, statPoolType, diff, instigator: whandle,
			// forceChunkTransfering, perc, ignoreCustomLimit), per the RTTI dump.
			auto                                    object = StatsIdOf(a_id);
			auto                                    type = kHealthPool;
			float                                   value = a_percentDelta;
			RED4ext::WeakHandle<rtti::IScriptable>  instigator(a_instigator);
			bool                                    forceChunkTransfering = true;
			bool                                    percentage = true;
			bool                                    ignoreCustomLimit = false;
			changeStatPool.Call(pools, nullptr, object, type, value, instigator, forceChunkTransfering, percentage, ignoreCustomLimit);
		}

		// A Minecraft explosion (TNT, a creeper) hurts every NPC in its reach, not only the one under
		// the crosshair: the targeting system's GetTargetParts lists the NPCs near V (TSF_NPC), and
		// each takes Minecraft's own blast damage for its distance from the centre. All four game
		// calls are matched by their exact signatures from the RTTI dump.
		void Explode(const McVec& a_centre, float a_power, const rtti::Handle<rtti::IScriptable>& a_player)
		{
			static RED4ext::CBaseFunction* getParts = rtti::FindFunction(kTargetingSystem, "GetTargetParts",
				{ "whandle:gameObject", "gameTargetSearchQuery", "array:gametargetingTargetPartInfo" }, "Bool");
			static rtti::Global            npcFilter("TSF_NPC;");
			static RED4ext::CBaseFunction* partComponent = rtti::FindFunction("gametargetingTargetPartInfo", "GetComponent",
				{ "gametargetingTargetPartInfo" }, "whandle:gameTargetingComponent");
			static RED4ext::CBaseFunction* componentEntity = rtti::FindFunction("entIComponent", "GetEntity", {}, "whandle:entEntity");
			static bool                    warned = false;
			auto*                          targeting = rtti::System(kTargetingSystem);
			if (!getParts || !npcFilter.Get() || !partComponent || !componentEntity || !targeting || !a_player) {
				if (!warned) {
					warned = true;
					logger::warn("combat: blast sweep unavailable (GetTargetParts {}, TSF_NPC {}, GetComponent {}, GetEntity {}); explosions hurt only the crosshair target",
						getParts != nullptr, npcFilter.Get() != nullptr, partComponent != nullptr, componentEntity != nullptr);
				}
				return;
			}

			// Minecraft hurts entities out to twice the explosion's power.
			const double reach = 2.0 * a_power;
			RED4ext::Vector4 vPos{};
			getWorldPosition.Call(a_player.GetPtr(), &vPos);
			const auto vMc = CpToMc(vPos.X, vPos.Y, vPos.Z);
			const double toV = std::sqrt((vMc.x - a_centre.x) * (vMc.x - a_centre.x) + (vMc.y - a_centre.y) * (vMc.y - a_centre.y) +
										 (vMc.z - a_centre.z) * (vMc.z - a_centre.z));

			RED4ext::game::TargetSearchQuery query{};
			query.testedSet = RED4ext::game::TargetingSet::Complete;
			npcFilter.Call(&query.searchFilter);
			query.includeSecondaryTargets = false;
			query.ignoreInstigator = true;
			// Metres: the search is around V, the blast may not be.
			query.maxDistance = float(std::min((toV + reach) * MetresPerBlock(), 120.0));
			query.filterObjectByDistance = true;

			RED4ext::WeakHandle<rtti::IScriptable>                     self(a_player);
			RED4ext::DynArray<RED4ext::game::targeting::TargetPartInfo> parts;
			bool                                                       found = false;
			{
				RED4ext::StackArgs_t args;
				args.emplace_back(nullptr, &self);
				args.emplace_back(nullptr, &query);
				args.emplace_back(nullptr, &parts);
				if (!RED4ext::ExecuteFunction(targeting, getParts, &found, args) || !found) {
					return;
				}
			}

			std::unordered_set<std::uint64_t> done;  // an NPC has several targetable parts
			int                               hurt = 0;
			for (auto& part : parts) {
				RED4ext::WeakHandle<rtti::IScriptable> component;
				RED4ext::StackArgs_t                   partArgs;
				partArgs.emplace_back(nullptr, &part);
				if (!RED4ext::ExecuteFunction(static_cast<void*>(nullptr), partComponent, &component, partArgs)) {
					continue;
				}
				auto componentHandle = component.Lock();
				if (!componentHandle) {
					continue;
				}
				RED4ext::WeakHandle<rtti::IScriptable> entityWeak;
				RED4ext::StackArgs_t                   none;
				if (!RED4ext::ExecuteFunction(componentHandle.GetPtr(), componentEntity, &entityWeak, none)) {
					continue;
				}
				auto entity = entityWeak.Lock();
				if (!entity) {
					continue;
				}
				const auto id = EntityIdOf(entity);
				if (id.hash == 0 || !done.insert(id.hash).second) {
					continue;
				}
				RED4ext::Vector4 at{};
				getWorldPosition.Call(entity.GetPtr(), &at);
				const auto   mc = CpToMc(at.X, at.Y + 0.0f, at.Z + 0.9f);  // the body's middle, not the feet
				const double dist = std::sqrt((mc.x - a_centre.x) * (mc.x - a_centre.x) + (mc.y - a_centre.y) * (mc.y - a_centre.y) +
											  (mc.z - a_centre.z) * (mc.z - a_centre.z));
				if (dist >= reach) {
					continue;
				}
				// Minecraft's ServerExplosion: impact falls off linearly, damage grows with its square.
				const double impact = 1.0 - dist / reach;
				const double damage = (impact * impact + impact) / 2.0 * 7.0 * reach + 1.0;
				ChangeHealth(id, -float(damage) * Config().toGame, a_player);
				++hurt;
			}
			logger::info("combat: explosion at MC ({:.1f}, {:.1f}, {:.1f}) power {:.1f}: {} NPCs searched, {} hurt", a_centre.x, a_centre.y, a_centre.z,
				a_power, done.size(), hurt);
		}

		rtti::Handle<rtti::IScriptable> CrosshairTarget()
		{
			// gametargetingTargetingSystem::GetLookAtObject(instigator: wref<GameObject>, withLOS,
			// ignoreTranparent) -> GameObject, per the RTTI dump, and matched by those exact types so
			// a different overload is never called. (The class is not "gameTargetingSystem": that
			// name does not exist, and nothing was ever found under the crosshair.)
			static RED4ext::CBaseFunction* fn = [] {
				auto* found = rtti::FindFunction(kTargetingSystem, "GetLookAtObject", { "whandle:gameObject", "Bool", "Bool" }, "handle:gameObject");
				if (!found) {
					logger::warn("combat: {}::GetLookAtObject(wref<GameObject>, Bool, Bool) not found; Minecraft can't hit anyone", kTargetingSystem);
				}
				return found;
			}();
			auto* targeting = rtti::System(kTargetingSystem);
			auto  player = rtti::Player();
			if (!fn || !targeting || !player) {
				return {};
			}
			rtti::Handle<rtti::IScriptable>        target;
			RED4ext::WeakHandle<rtti::IScriptable> self(player);
			bool                                   withLineOfSight = true;
			bool                                   ignoreTransparent = false;
			RED4ext::StackArgs_t                   args;
			args.emplace_back(nullptr, &self);
			args.emplace_back(nullptr, &withLineOfSight);
			args.emplace_back(nullptr, &ignoreTransparent);
			if (!RED4ext::ExecuteFunction(targeting, fn, &target, args) || !target) {
				return {};
			}
			// Only within reach: the look-at target can be anyone down the street.
			RED4ext::Vector4 at{};
			RED4ext::Vector4 from{};
			getWorldPosition.Call(target.GetPtr(), &at);
			getWorldPosition.Call(player.GetPtr(), &from);
			const float dx = at.X - from.X, dy = at.Y - from.Y, dz = at.Z - from.Z;
			if (dx * dx + dy * dy + dz * dz > Config().reach * Config().reach) {
				return {};
			}
			return target;
		}

		void Remember(const rtti::Handle<rtti::IScriptable>& a_object, std::uint64_t a_entity, std::chrono::steady_clock::time_point a_now)
		{
			for (auto& npc : remembered) {
				if (npc.entity == a_entity) {
					npc.seen = a_now;
					return;
				}
			}
			if (remembered.size() >= kMaxRemembered) {
				// The one longest out of sight makes room.
				remembered.erase(std::ranges::min_element(remembered, {}, &Remembered::seen));
			}
			remembered.push_back({ a_object, a_entity, a_now, true });
		}

		// An NPC as Minecraft's stand-in for it: where it is, which way it faces, how hurt it is.
		proto::ActorRecord RecordFor(const rtti::Handle<rtti::IScriptable>& a_object, std::uint64_t a_entity)
		{
			RED4ext::Vector4 pos{};
			float            yaw = 0.0f;
			getWorldPosition.Call(a_object.GetPtr(), &pos);
			getWorldYaw.Call(a_object.GetPtr(), &yaw);
			const auto             here = CpToMc(pos.X, pos.Y, pos.Z);
			RED4ext::ent::EntityID entityId{};
			entityId.hash = a_entity;
			const auto health = HealthFraction(entityId);

			proto::ActorRecord record{};
			record.actorId = HandleFor(entityId.hash);
			record.flags = proto::kActorHostile | proto::kActorInCombat;
			if (health == 0.0f) {
				record.flags |= proto::kActorDead;
			}
			record.x = static_cast<float>(here.x);
			record.y = static_cast<float>(here.y);
			record.z = static_cast<float>(here.z);
			record.yaw = CpYawToMc(yaw);
			// Night City's people are human-sized (0.6 by 1.8 m), in blocks however big a block is;
			// Minecraft only needs a box to swing at.
			const float perMetre = float(1.0 / MetresPerBlock());
			record.width = 0.6f * perMetre;
			record.height = 1.8f * perMetre;
			record.healthFrac = health < 0.0f ? 1.0f : health;
			record.level = 1;

			RED4ext::CString name;
			if (getDisplayName.Call(a_object.GetPtr(), &name) && name.c_str()) {
				std::snprintf(record.name, sizeof(record.name), "%s", name.c_str());
			}
			return record;
		}

		// The NPC under the crosshair joins the remembered ones, and all of them go to Minecraft.
		void MirrorTarget(const McVec& a_playerMc)
		{
			const auto now = std::chrono::steady_clock::now();
			if (auto target = CrosshairTarget()) {
				const auto entityId = EntityIdOf(target);
				if (entityId.hash != 0 && HealthFraction(entityId) != 0.0f) {
					Remember(target, entityId.hash, now);
				}
			}

			const auto                                     forget = std::chrono::duration<float>(Config().remember);
			std::array<proto::ActorRecord, kMaxRemembered> records{};
			std::uint32_t                                  count = 0;
			for (auto it = remembered.begin(); it != remembered.end();) {
				const auto object = it->object.Lock();
				if (!object || now - it->seen > forget) {
					it = remembered.erase(it);
					continue;
				}
				const auto   record = RecordFor(object, it->entity);
				const double dx = record.x - a_playerMc.x, dy = record.y - a_playerMc.y, dz = record.z - a_playerMc.z;
				const double range = kRememberRange / MetresPerBlock();
				if (dx * dx + dy * dy + dz * dz > range * range) {
					it = remembered.erase(it);
					continue;
				}
				if (it->fresh) {
					it->fresh = false;
					logger::info("combat: mirroring {} (entity {:#x}) at MC ({:.1f}, {:.1f}, {:.1f}) health {:.0f}%", record.name, it->entity,
						record.x, record.y, record.z, record.healthFrac * 100.0f);
				}
				records[count++] = record;
				// Sent once as dead, which takes its stand-in away, then let go.
				it = (record.flags & proto::kActorDead) != 0 ? remembered.erase(it) : it + 1;
			}
			Link::Get().WriteActors(count ? records.data() : nullptr, count);
		}

		// Cyberpunk hurt V: Minecraft takes the same hit, scaled down (DESIGN.md §13).
		void MirrorPlayerHealth()
		{
			auto player = rtti::Player();
			if (!player) {
				lastPlayerHealth = -1.0f;
				State().vDead = false;
				return;
			}
			const auto health = HealthFraction(EntityIdOf(player));
			if (health < 0.0f) {
				return;
			}
			State().vDead = health == 0.0f;
			if (lastPlayerHealth >= 0.0f && health < lastPlayerHealth) {
				const float lostPercent = (lastPlayerHealth - health) * 100.0f;
				const float mcDamage = lostPercent * Config().fromGame;
				if (mcDamage > 0.01f) {
					Link::Get().PushInput(proto::kInHurt, proto::kHurtOther, static_cast<std::int32_t>(mcDamage * 100.0f), 0, 0);
				}
			}
			lastPlayerHealth = health;
		}

		void HandleEvent(const proto::McEvent& a_event)
		{
			auto player = rtti::Player();
			switch (a_event.type) {
			case proto::kEvHitActor:
				{
					const auto found = idToEntity.find(a_event.id);
					if (found == idToEntity.end()) {
						return;
					}
					RED4ext::ent::EntityID id{};
					id.hash = found->second;
					// Still being fought (or still poisoned): it stays mirrored.
					for (auto& npc : remembered) {
						if (npc.entity == id.hash) {
							npc.seen = std::chrono::steady_clock::now();
						}
					}
					// a is the damage Minecraft already resolved (reach, crits, cooldown, shields).
					const float percent = a_event.a * Config().toGame;
					ChangeHealth(id, -percent, player);
					logger::info("combat: Minecraft hit {:#x} for {:.1f} health percent", id.hash, percent);
					return;
				}
			case proto::kEvPlayerDied:
				// Only a death Minecraft's player died as V: one while Cyberpunk had the controls
				// (in a car, or before the ground was known) happened somewhere V never was. The death
				// itself ends the puppeting a frame or so before this event arrives.
				if (player && std::chrono::steady_clock::now() - State().lastPuppeted < std::chrono::seconds(2)) {
					logger::info("combat: the Minecraft player died; killing V");
					ChangeHealth(EntityIdOf(player), -100.0f, player);
				} else {
					logger::info("combat: the Minecraft player died while Cyberpunk had V; V lives");
				}
				return;
			case proto::kEvExplosion:
				// a/b/c: the centre (MC coords), d: the explosion's power (TNT 4).
				Explode(McVec{ a_event.a, a_event.b, a_event.c }, a_event.d, player);
				return;
			case proto::kEvSetTime:
				Environment::SetHour(a_event.a);
				return;
			case proto::kEvSetWeather:
				Environment::SetWeather(static_cast<proto::Weather>(a_event.id));
				return;
			default:
				return;
			}
		}
	}

	void Reset()
	{
		idToEntity.clear();
		entityToId.clear();
		nextId = 1;
		lastPlayerHealth = -1.0f;
		remembered.clear();
	}

	void Update(const McVec& a_playerMc)
	{
		auto& link = Link::Get();
		if (!link.Valid() || !link.McAlive()) {
			return;
		}

		// Actors are refreshed a few times a second: Minecraft interpolates its stand-ins, and the
		// crosshair target does not change faster than that.
		static int frame = 0;
		if (++frame % 6 == 0) {
			MirrorTarget(a_playerMc);
		}
		MirrorPlayerHealth();

		proto::McEvent event{};
		while (link.PopEvent(event)) {
			HandleEvent(event);
		}
	}
}
