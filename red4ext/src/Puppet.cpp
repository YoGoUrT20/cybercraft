#include "Puppet.h"

#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>

#include "Config.h"
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

		// Where V is expected to be now (Cyberpunk x, y): where she was placed, or last seen. A
		// teleport lands in the frame it is made, so anything else is Cyberpunk moving her itself.
		float expectedX = 0.0f, expectedY = 0.0f;
		// How far Cyberpunk has pushed V off Minecraft's player, kept: her body is wider than
		// Minecraft's player (0.3 blocks each way, 0.225 m at 0.75), so Minecraft walks her up to a
		// wall closer than Cyberpunk lets her stand. It pushed her out, the next teleport put her
		// back in, every frame she moved or turned: the shake near walls. Placed where Cyberpunk
		// pushed her instead, she stays put; the push wears off once nothing pushes any more.
		// Only while Minecraft's player moves or turns: standing still, Cyberpunk nudging her (a
		// slope, her feet a little in the ground) carried her off half a metre, the limit threw it
		// away and snapped her back, and over again, forever. Still, the offset stays as it is.
		float offsetX = 0.0f, offsetY = 0.0f;
		int   framesUnpushed = 0;
		// Minecraft's player last frame (Cyberpunk x, y; yaw): whether it moved or turned since.
		float lastMcX = 0.0f, lastMcY = 0.0f, lastMcYaw = 0.0f;

		// [Puppet] fMaxPushback: the most V is let stand off Minecraft's player (metres); 0 off.
		float MaxPushback()
		{
			static const float value = std::clamp(Config::GetFloat(L"Puppet", L"fMaxPushback", 0.25f), 0.0f, 2.0f);
			return value;
		}

		// Cyberpunk moved V by (a_x, a_y) since she was placed (or last seen). a_moving: Minecraft's
		// player moved or turned this frame.
		void GiveWay(float a_x, float a_y, bool a_moving)
		{
			constexpr float kNoise = 0.001f;
			// A little at a time while pushes keep coming (no visible creep back into a wall), faster
			// once they have stopped for half a second or so.
			constexpr float kWearNear = 0.002f;
			constexpr float kWearAway = 0.01f;
			constexpr int   kAwayFrames = 30;
			const float     limit = MaxPushback();
			const float     push = std::sqrt(a_x * a_x + a_y * a_y);
			if (limit <= 0.0f || !a_moving) {
				return;
			}
			if (push > limit) {
				// Not a body against a wall: something else moved her (the game placing her). Not
				// kept, and the offset as it was: dropping it snapped her back across it.
				return;
			}
			if (push > kNoise) {
				offsetX += a_x;
				offsetY += a_y;
				framesUnpushed = 0;
				++stats.pushes;
				stats.maxPush = std::max(stats.maxPush, push);
			} else {
				++framesUnpushed;
				const float length = std::sqrt(offsetX * offsetX + offsetY * offsetY);
				const float wear = framesUnpushed > kAwayFrames ? kWearAway : kWearNear;
				const float scale = length > wear ? (length - wear) / length : 0.0f;
				offsetX *= scale;
				offsetY *= scale;
			}
			const float length = std::sqrt(offsetX * offsetX + offsetY * offsetY);
			if (length > limit) {
				offsetX *= limit / length;
				offsetY *= limit / length;
			}
			stats.maxOffset = std::max(stats.maxOffset, std::min(length, limit));
		}
	}

	CpVec Offset()
	{
		return { offsetX, offsetY, 0.0f };
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

		if (had) {
			const float drift = std::abs(a_vNow.z - lastPos.z);
			stats.maxSink = std::max(stats.maxSink, drift);
		}
		auto       target = McToCp(a_feetMc.x, a_feetMc.y, a_feetMc.z);
		const auto yaw = McYawToCp(a_mcYaw);
		const bool moving = !had || std::abs(target.x - lastMcX) > 0.001f || std::abs(target.y - lastMcY) > 0.001f ||
		                    std::abs(std::remainder(yaw - lastMcYaw, 360.0f)) > 0.05f;
		lastMcX = target.x;
		lastMcY = target.y;
		lastMcYaw = yaw;
		if (had) {
			const float px = a_vNow.x - expectedX, py = a_vNow.y - expectedY;
			GiveWay(px, py, moving);
			// Each push (bDiagnostics), to set against where V got stuck, but no more than a few a
			// second: standing on a slope Cyberpunk can nudge her every frame.
			static auto nextLog = std::chrono::steady_clock::time_point{};
			if (Config::Diagnostics() && std::sqrt(px * px + py * py) > 0.01f && std::chrono::steady_clock::now() >= nextLog) {
				nextLog = std::chrono::steady_clock::now() + 250ms;
				logger::info("puppet: Cyberpunk moved V ({:+.3f}, {:+.3f}) m sideways and {:+.3f} m up{}; she now stands ({:+.3f}, {:+.3f}) m off "
							 "Minecraft's player (on ground {})",
					px, py, a_vNow.z - lastPos.z, moving ? "" : " (Minecraft's player still: not kept)", offsetX, offsetY, a_onGround);
			}
		}
		expectedX = a_vNow.x;
		expectedY = a_vNow.y;
		target.x += offsetX;
		target.y += offsetY;
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
		expectedX = target.x;
		expectedY = target.y;
		had = true;
	}

	void Release()
	{
		had = false;
		offsetX = offsetY = 0.0f;
		framesUnpushed = 0;
	}
}
