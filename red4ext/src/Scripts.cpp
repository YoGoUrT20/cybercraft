#include "Scripts.h"

#include "Rtti.h"

#include <RED4ext/Scripting/Natives/Generated/ent/EntityID.hpp>

namespace cybercraft::Scripts
{
	namespace
	{
		constexpr const char* kClass = "CyberCraftHost";

		struct Functions
		{
			RED4ext::CBaseFunction* isReady{ nullptr };
			RED4ext::CBaseFunction* spawnLight{ nullptr };
			RED4ext::CBaseFunction* spawnCollider{ nullptr };
			RED4ext::CBaseFunction* despawn{ nullptr };
			RED4ext::CBaseFunction* despawnAll{ nullptr };
			RED4ext::CBaseFunction* bodyVisible{ nullptr };
			bool                    complete{ false };
		};

		// Looked up once, the first time anything here is used (Game::Tick, long after scripts load).
		// CyberCraftHost's static functions aren't in its RTTI class, so CyberCraft.reds wraps them
		// in global functions, which are.
		const Functions& Find()
		{
			static const Functions functions = [] {
				Functions f{};
				f.isReady = rtti::FindGlobalFunction("CyberCraftIsReady", {}, "Bool");
				f.spawnLight = rtti::FindGlobalFunction("CyberCraftSpawnLight", { "Vector4", "Vector4", "Float", "Float", "Float", "Bool" }, "entEntityID");
				f.spawnCollider = rtti::FindGlobalFunction("CyberCraftSpawnCollider", { "Vector4", "Vector4", "Float", "Bool" }, "entEntityID");
				f.despawn = rtti::FindGlobalFunction("CyberCraftDespawn", { "entEntityID" }, "Bool");
				f.despawnAll = rtti::FindGlobalFunction("CyberCraftDespawnAll", {});
				f.bodyVisible = rtti::FindGlobalFunction("CyberCraftSetPlayerBodyVisible", { "Bool" }, "Int32");
				f.complete = f.isReady && f.spawnLight && f.spawnCollider && f.despawn && f.despawnAll && f.bodyVisible;
				if (f.complete) {
					logger::info("scripts: CyberCraft.reds is loaded (lights, colliders, V's body in third person)");
				} else if (!RED4ext::CRTTISystem::Get()->GetClass(kClass)) {
					logger::warn("scripts: CyberCraft.reds isn't loaded (it needs Codeware); no lights or colliders for blocks, and V stays visible in third person");
				} else {
					logger::warn("scripts: CyberCraft.reds is loaded but doesn't match this plugin (IsReady {}, SpawnLight {}, SpawnCollider {}, Despawn {}, DespawnAll {}, SetPlayerBodyVisible {})",
						f.isReady != nullptr, f.spawnLight != nullptr, f.spawnCollider != nullptr, f.despawn != nullptr, f.despawnAll != nullptr, f.bodyVisible != nullptr);
					rtti::DumpFunctionsMatching(nullptr, { "CyberCraft" });
				}
				return f;
			}();
			return functions;
		}

		// A static script function: no instance, arguments by address.
		template <class... Args>
		bool CallStatic(RED4ext::CBaseFunction* a_fn, void* a_out, Args&... a_args)
		{
			RED4ext::StackArgs_t args;
			((args.emplace_back(nullptr, &a_args)), ...);
			return RED4ext::ExecuteFunction(static_cast<void*>(nullptr), a_fn, a_out, args);
		}

		RED4ext::Vector4 Point(const CpVec& a_v, float a_w)
		{
			return RED4ext::Vector4{ a_v.x, a_v.y, a_v.z, a_w };
		}
	}

	bool Ready()
	{
		const auto& f = Find();
		bool        ready = false;
		return f.complete && CallStatic(f.isReady, &ready) && ready;
	}

	std::uint64_t SpawnLight(const CpVec& a_position, float a_r, float a_g, float a_b, float a_radius, float a_intensity, float a_flicker,
		bool a_shadows)
	{
		const auto& f = Find();
		if (!f.complete) {
			return 0;
		}
		auto                 position = Point(a_position, 1.0f);
		RED4ext::Vector4     color{ a_r, a_g, a_b, 1.0f };
		RED4ext::ent::EntityID id{};
		CallStatic(f.spawnLight, &id, position, color, a_radius, a_intensity, a_flicker, a_shadows);
		return id.hash;
	}

	std::uint64_t SpawnCollider(const CpVec& a_centre, const CpVec& a_halfExtents, float a_yaw, bool a_obstacle)
	{
		const auto& f = Find();
		if (!f.complete) {
			return 0;
		}
		auto                   centre = Point(a_centre, 1.0f);
		auto                   half = Point(a_halfExtents, 0.0f);
		RED4ext::ent::EntityID id{};
		CallStatic(f.spawnCollider, &id, centre, half, a_yaw, a_obstacle);
		return id.hash;
	}

	void Despawn(std::uint64_t a_entity)
	{
		const auto& f = Find();
		if (!f.complete || a_entity == 0) {
			return;
		}
		RED4ext::ent::EntityID id{};
		id.hash = a_entity;
		bool done = false;
		CallStatic(f.despawn, &done, id);
	}

	void DespawnAll()
	{
		const auto& f = Find();
		if (f.complete) {
			CallStatic(f.despawnAll, nullptr);
		}
	}

	int SetPlayerBodyVisible(bool a_visible)
	{
		const auto& f = Find();
		if (!f.complete) {
			return 0;
		}
		bool         visible = a_visible;
		std::int32_t changed = 0;
		CallStatic(f.bodyVisible, &changed, visible);
		return changed;
	}

	std::optional<int> ComponentsMade(bool a_lights)
	{
		static RED4ext::CBaseFunction* const fn = rtti::FindGlobalFunction("CyberCraftComponentsMade", { "Bool" }, "Int32");
		bool         lights = a_lights;
		std::int32_t made = 0;
		if (!fn || !Find().complete || !CallStatic(fn, &made, lights)) {
			return std::nullopt;
		}
		return made;
	}

	std::optional<bool> InteractionShown()
	{
		static RED4ext::CBaseFunction* const fn = [] {
			auto* found = rtti::FindGlobalFunction("CyberCraftInteractionShown", {}, "Bool");
			if (found) {
				logger::info("input: F is Cyberpunk's while it offers an interaction, loot or dialogue, Minecraft's (swap hands) otherwise");
			} else {
				logger::warn("input: CyberCraftInteractionShown isn't in the scripts; F stays Cyberpunk's");
			}
			return found;
		}();
		bool shown = false;
		if (!fn || !CallStatic(fn, &shown)) {
			return std::nullopt;
		}
		return shown;
	}

	std::optional<bool> SetWeaponsBlocked(bool a_blocked)
	{
		static RED4ext::CBaseFunction* const fn = [] {
			auto* found = rtti::FindGlobalFunction("CyberCraftSetWeaponsBlocked", { "Bool" }, "Bool");
			if (!found) {
				logger::warn("weapons: CyberCraftSetWeaponsBlocked isn't in the scripts; V can still draw a weapon while Minecraft runs");
			}
			return found;
		}();
		bool blocked = a_blocked;
		bool on = false;
		if (!fn || !CallStatic(fn, &on, blocked)) {
			return std::nullopt;
		}
		return on;
	}
}
