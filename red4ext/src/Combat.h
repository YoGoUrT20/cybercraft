#pragma once

#include "Link.h"

// Fighting Cyberpunk's NPCs with Minecraft's weapons, and being fought back (DESIGN.md §8,
// Phase 3). Minecraft mirrors each nearby NPC as an invisible hittable stand-in, so Minecraft's
// own combat - reach, cooldown, crits, sweeping, shields - decides whether a swing lands, and
// only the result crosses the link.
//
// Scope note: mirroring every actor in a radius would mean decoding an array returned by the
// targeting system, whose element layout cannot be confirmed without running the game, and getting that
// wrong is a crash rather than a wrong number. So this mirrors the NPC under the crosshair -
// enough to trade blows with one enemy at a time - and keeps mirroring the last few for a while
// after ([Combat] fRememberSeconds, longer while they keep being hit), so potion effects on them
// and arrows still in flight carry on. The radius sweep waits for the in-game probe to pin the
// signature down.
namespace cybercraft::Combat
{
	// Once per frame while a save is loaded, with the player in Minecraft coordinates.
	void Update(const McVec& a_playerMc);

	// Minecraft reconnected: forget mirrored actors and the health baseline.
	void Reset();
}
