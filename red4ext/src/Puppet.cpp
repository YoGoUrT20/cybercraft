#include "Puppet.h"

#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>

#include "Rtti.h"

namespace cybercraft::Puppet
{
	namespace
	{
		// gameTeleportationFacility::Teleport(entity, worldPosition, rotation). Unverified against
		// the live game: Method::Get logs a warning if the name is wrong, and Game's class dump
		// prints the real signature on the first run.
		rtti::Method teleport{ "gameTeleportationFacility", "Teleport" };

		CpVec lastPos{ 0.0f, 0.0f, 0.0f };
		float lastYaw = 0.0f;
		bool  had = false;
		bool  warned = false;
		// Minecraft's player standing still: V was seen at the pose with no teleport, and then off it
		// on her own (Cyberpunk doesn't hold her there).
		bool  seenHeld = false;
		bool  unheld = false;
		Stats stats;
	}

	Stats TakeStats()
	{
		return std::exchange(stats, Stats{});
	}

	void Apply(const McVec& a_feetMc, float a_mcYaw, const CpVec& a_vNow, float a_vYaw, bool a_onGround)
	{
		auto player = rtti::Player();
		if (!player) {
			return;
		}
		auto* facility = rtti::System("gameTeleportationFacility");
		if (!facility) {
			return;
		}

		const auto target = McToCp(a_feetMc.x, a_feetMc.y, a_feetMc.z);
		const auto yaw = McYawToCp(a_mcYaw);
		if (had) {
			const float drift = std::abs(a_vNow.z - lastPos.z);
			stats.maxSink = std::max(stats.maxSink, drift);
		}
		// V is left alone only while Minecraft's player stands still and Cyberpunk holds her there by
		// itself: placed every frame, standing still too, Night City spawned no cars around her once
		// Minecraft took over, and a called car never came. Anywhere else she is placed every frame.
		// In the air, and on what Cyberpunk can't feel (placed blocks without a collider, the voxel
		// ground's top), its gravity sinks her, at a high frame rate less than a millimetre in a
		// frame's first fall, so any per-frame slack let her sink and be snapped back over and over:
		// the bob, standing or flying still. So once she has moved on her own at a pose, she is placed
		// every frame until Minecraft's player moves again.
		const bool still = had && a_onGround && std::abs(target.x - lastPos.x) < 0.002f && std::abs(target.y - lastPos.y) < 0.002f &&
		                   std::abs(target.z - lastPos.z) < 0.0005f && std::abs(std::remainder(yaw - lastYaw, 360.0f)) < 0.05f;
		if (!still) {
			seenHeld = false;
			unheld = false;
		} else if (!unheld) {
			const bool at = std::abs(a_vNow.x - target.x) < 0.002f && std::abs(a_vNow.y - target.y) < 0.002f &&
			                std::abs(a_vNow.z - target.z) < 0.0005f && std::abs(std::remainder(a_vYaw - yaw, 360.0f)) < 0.05f;
			if (at) {
				seenHeld = true;
				return;
			}
			// Off the pose before she was ever seen on it: the last teleport hasn't landed yet.
			unheld = seenHeld;
		}
		++stats.teleports;

		RED4ext::Vector4 position{ target.x, target.y, target.z, 1.0f };
		// Only yaw: Cyberpunk's first-person camera owns pitch, and it reads the look input this
		// plugin swallows. Pitch reaches the view with the camera work in Phase 2.
		RED4ext::EulerAngles rotation{ 0.0f, 0.0f, yaw };
		if (!teleport.Call(facility, nullptr, player, position, rotation)) {
			if (!warned) {
				warned = true;
				logger::error("puppet: gameTeleportationFacility::Teleport failed; Minecraft cannot move V");
			}
			return;
		}
		lastPos = target;
		lastYaw = yaw;
		had = true;
	}

	void Release()
	{
		had = false;
	}
}
