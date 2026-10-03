#pragma once

#include "Link.h"

// Minecraft is authoritative for the player (DESIGN.md §6): V becomes a puppet moved to wherever
// Minecraft's player is, so Cyberpunk's own systems - NPC awareness, trigger volumes, quest
// location checks - keep working around her.
//
// V's character controller isn't reachable through RED4ext, so this goes through
// gameTeleportationFacility, which is the only exposed way to place an entity.
namespace cybercraft::Puppet
{
	// Once per frame while Minecraft drives the player. a_feetMc is Minecraft's interpolated feet
	// position; a_mcYaw is its look yaw (Minecraft degrees). a_vNow and a_vYaw are where V is this
	// frame and her heading (Cyberpunk degrees; her own physics moves her between teleports).
	// a_onGround: whether Minecraft's player stands. She is only left untouched while it stands
	// still and Cyberpunk holds her at its pose by itself; otherwise she is teleported every frame.
	void Apply(const McVec& a_feetMc, float a_mcYaw, const CpVec& a_vNow, float a_vYaw, bool a_onGround);

	// Minecraft stopped driving (menu, death, disconnect): forget the last pose so the next
	// takeover teleports again rather than deciding nothing moved.
	void Release();

	// Diagnostics since the last call: teleports made, and how far V had moved vertically on her own
	// between two of them (Cyberpunk's gravity on ground it can't feel).
	struct Stats
	{
		std::uint32_t teleports{ 0 };
		float         maxSink{ 0.0f };
	};
	Stats TakeStats();
}
