#pragma once

#include "Link.h"

// The plugin's way into scripts/CyberCraft.reds (class CyberCraftHost), which spawns things into
// Night City through Codeware. Each call is found by its exact signature the first time; without
// Codeware (or with the script not loaded) none is found, Ready() is false and every call does
// nothing. Main thread only.
namespace cybercraft::Scripts
{
	// The script is loaded and Codeware's static entity system can spawn now.
	bool Ready();

	// Cyberpunk positions (metres). Returns the entity's id hash, 0 when nothing was spawned.
	std::uint64_t SpawnLight(const CpVec& a_position, float a_r, float a_g, float a_b, float a_radius, float a_intensity, float a_flicker,
		bool a_shadows);
	// The box is turned a_yaw degrees (counter-clockwise seen from above) about its centre.
	std::uint64_t SpawnCollider(const CpVec& a_centre, const CpVec& a_halfExtents, float a_yaw, bool a_obstacle);
	void          Despawn(std::uint64_t a_entity);
	void          DespawnAll();

	// V's body off (Minecraft's body stands in for it) or back on. Returns how many parts changed.
	int SetPlayerBodyVisible(bool a_visible);

	// How many spawned lights (a_lights) or colliders have been given their component so far: fewer
	// than were spawned means the entities never reached the script. Empty with an older script.
	std::optional<int> ComponentsMade(bool a_lights);

	// Cyberpunk shows something to do with F (an interaction, loot, dialogue choices). Vanilla
	// script, so it doesn't need Codeware; empty when the script isn't loaded.
	std::optional<bool> InteractionShown();

	// V's weapons holstered and locked (the game's no-combat restriction) or free again. Vanilla
	// script. Returns whether this plugin's restriction is on V now; empty when the script isn't loaded.
	std::optional<bool> SetWeaponsBlocked(bool a_blocked);

	// V's health Minecraft's (hits deal her nothing, Cyberpunk's health bar hidden) or Cyberpunk's
	// again. Vanilla script. Returns whether it was set; empty when the script isn't loaded.
	std::optional<bool> SetMcHealth(bool a_on);
	// The health percent hits took off V since the last call, while her health is Minecraft's.
	// Vanilla script; empty when the script isn't loaded.
	std::optional<float> TakeHurt();

	// V's own collision off (she passes through the city's colliders) or back on, from her next
	// movement update. Vanilla script. Returns whether it was set; empty when the script isn't loaded.
	std::optional<bool> SetCollisionOff(bool a_off);

	// Minecraft hurt an NPC (a gameObject): it cries out, and with a non-zero a_impulse (Cyberpunk
	// world) it is ragdolled and sent flying. Vanilla script. Returns whether it went flying; empty
	// when the script isn't loaded.
	std::optional<bool> NpcHit(const RED4ext::Handle<RED4ext::IScriptable>& a_npc, const CpVec& a_impulse);
}
