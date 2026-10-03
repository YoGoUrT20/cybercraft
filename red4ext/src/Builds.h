#pragma once

#include "Link.h"

// The player's builds made real in Night City, beyond being drawn: a Cyberpunk light for each of
// Minecraft's light-emitting blocks (torches, lanterns, glowstone, lava), and a box collider for each
// run of solid blocks, so NPCs, cars and bullets meet them. Minecraft says which blocks those are per
// section (kRenLights, kRenSolids); the objects are spawned through scripts/CyberCraft.reds, nearest
// to the player first and within the limits below. Without Codeware nothing is spawned.
//
// [Builds] in CyberCraft.ini:
//   bLights              on/off                                              (1)
//   iMaxLights           lights at once, nearest first                       (48)
//   fLightRadius         only lights this close to the player, blocks        (64)
//   fLightIntensity      a full-strength (level 15) light's intensity        (40)
//   fLightRangePerLevel  light radius per Minecraft light level, blocks      (0.8)
//   fFlicker             flame flicker strength (torches, fire)              (0.3)
//   bLightShadows        lights cast shadows (costly)                        (0)
//   bColliders           on/off                                              (1)
//   iMaxColliders        boxes at once                                       (1024)
//   fColliderRadius      only sections this close to the player, blocks      (48)
//   bNavObstacle         boxes are navigation obstacles NPCs walk around     (1)
//   iSpawnsPerFrame      spawns per frame                                    (16)
namespace cybercraft::Builds
{
	// Render thread (World::Consume): what Minecraft says about one section. Each replaces what
	// the section had (Minecraft sends both again whenever they change, an empty one included).
	void OnLights(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const proto::RenLight* a_lights, std::uint32_t a_count);
	void OnSolids(std::int32_t a_sx, std::int32_t a_sy, std::int32_t a_sz, const std::uint8_t* a_bits);  // 512 bytes, or null: none
	void OnClearAll();

	// Main thread, every frame while a save is loaded, with the player in Minecraft coordinates.
	void Update(const McVec& a_playerMc);

	// Everything spawned goes, and is spawned again where it belongs (the grid moved, Minecraft
	// reconnected). Main thread.
	void Respawn();

	// A save is loading: Cyberpunk takes its entities with the old session. Forget them without
	// despawning, and spawn again once the new session runs. Main thread.
	void Forget();
}
