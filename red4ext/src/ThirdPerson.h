#pragma once

#include "Link.h"
#include "Rtti.h"

// Minecraft's F5 views on Cyberpunk's camera. Cyberpunk has no third-person camera on foot, so its
// first-person camera is moved instead: back along the look (or out in front, turned around) by
// Minecraft's own camera distance, kept short of the city's walls. V's body is hidden
// (scripts/CyberCraft.reds) and Minecraft's body, skin and armour, is drawn where she stands
// (World.cpp, kRenAvatar). In first person too: looking down shows Minecraft's body without its head
// and arms (the hand is drawn on its own). In any view the camera also stands at Minecraft's eye:
// lower than V's at less than a metre a block (Link.h), and lower still sneaking, crawling, swimming.
//
// [ThirdPerson] in CyberCraft.ini:
//   bEnable         on/off                                                      (1)
//   bOrbitPitch     pitch the camera's offset as well: only if the camera       (0)
//                   stays level behind V while she looks up and down
// [Body]:
//   bHideV          V hidden in first person too (always in third person)       (1)
//   bMinecraftBody  then Minecraft's body in her place; 0: nothing              (1)
//   fBack           blocks it stands behind the eye in first person, so looking (0.2)
//                   down shows its chest and feet rather than its shoulders
namespace cybercraft::ThirdPerson
{
	// Main thread, every frame while Minecraft drives V. a_mode: McState::cameraMode (0 first person,
	// 1 behind, 2 in front); a_distance: McState::cameraDistance; a_mcYaw/a_mcPitch: the look in
	// Minecraft degrees; a_headMc: V's eyes in Minecraft coordinates (feet plus eye height);
	// a_eyeShift: metres Minecraft's eye sits above Cyberpunk's standing camera (negative: below it).
	void Update(const rtti::Handle<rtti::IScriptable>& a_player, std::uint32_t a_mode, float a_distance, float a_mcYaw, float a_mcPitch,
		const McVec& a_headMc, float a_eyeShift);

	// Back to first person at full height: Minecraft let go of V, a menu, a load.
	void Release(const rtti::Handle<rtti::IScriptable>& a_player);

	// Minecraft's body is shown in V's place (in first person, if Minecraft sends it).
	bool Active();

	// V is hidden in first person too and Minecraft should send its body there (kCyberBodyFirstPerson),
	// BodyBack() blocks behind its eye.
	bool  FirstPersonBody();
	float BodyBack();
}
