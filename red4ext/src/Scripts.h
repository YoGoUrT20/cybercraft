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
	std::uint64_t SpawnCollider(const CpVec& a_centre, const CpVec& a_halfExtents, bool a_obstacle);
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
}
