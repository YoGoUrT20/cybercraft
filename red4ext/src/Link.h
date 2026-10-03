#pragma once

#include "cybercraft_protocol.h"

namespace cybercraft
{
	// Owner of the shared-memory mapping (Cyberpunk creates it; Minecraft opens it).
	class Link
	{
	public:
		static Link& Get();

		bool Create();
		[[nodiscard]] bool Valid() const { return base_ != nullptr; }

		// True if Minecraft has touched its heartbeat recently.
		[[nodiscard]] bool McAlive() const;
		void               Heartbeat();
		// Process id Minecraft wrote when it opened the mapping (changes when Minecraft restarts).
		[[nodiscard]] std::uint32_t McPid() const;

		// Seqlock write of host -> MC state. Called once per frame (MC paces on seq).
		void WriteCyberState(const proto::CyberState& a_state);
		void WriteWaterGrid(const proto::WaterGrid& a_grid);
		// Seqlock read of MC -> host state. Returns false if no consistent snapshot was obtained.
		bool ReadMcState(proto::McState& a_out) const;

		// Input ring (producer side). Drops the event if MC has fallen a full ring behind.
		void PushInput(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);

		// Collision ring (producer side, one thread only). Returns false if the ring is full.
		bool WriteCollision(proto::ColType a_type, const void* a_payload, std::uint32_t a_bytes);

		// Actor table (producer, main thread): nearby NPCs Minecraft mirrors as hittable stand-ins.
		void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count);
		// Event ring (consumer, main thread). Returns false when empty.
		bool PopEvent(proto::McEvent& a_out);
		// World entities + block outline (seqlock read, render thread).
		bool ReadWorldEntities(proto::WorldEntities& a_out) const;
		// Render ring (consumer, render thread): calls a_fn(type, payload, bytes) for each pending
		// message, up to about a_maxBytes of payload. The payload points into shared memory.
		void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& a_fn, std::uint64_t a_maxBytes);

		// Overlay triple buffer (consumer side). If a newer frame is available, swaps it into the
		// front slot and returns true. FrontPixels/FrontHeader describe the current front slot.
		bool                                       AcquireOverlayFrame();
		// Minecraft (re)connected: its writer starts at slot 1, so restart the swap from scratch.
		void                                       ResetOverlay();
		[[nodiscard]] const std::uint8_t*          FrontPixels() const;
		[[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

	private:
		template <class T>
		T* At(std::uint64_t a_off) const { return reinterpret_cast<T*>(base_ + a_off); }

		HANDLE        mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::uint32_t overlayFront_{ 2 };
	};

	// ---- coordinate conversion (Cyberpunk metres <-> Minecraft blocks) -------------------------
	// Cyberpunk: X east, Y north, Z up, metres. Minecraft: X east, Y up, Z south, blocks.
	struct McVec
	{
		double x, y, z;
	};

	struct CpVec
	{
		float x, y, z;
	};

	// Cyberpunk metres per Minecraft block, the whole of Minecraft's world scaled by it: blocks, mobs,
	// the player and its physics. At 1 a block is a metre and Minecraft's player as tall as V, but
	// next to Night City's people and props its blocks and blocky bodies look oversized. Set by
	// Grid.cpp ([World] fMetresPerBlock, or Mod Settings' slider).
	inline constexpr double    kDefaultMetresPerBlock = 0.75;
	inline std::atomic<double> gMetresPerBlock{ kDefaultMetresPerBlock };

	// Minecraft's grid is shifted up by this many metres (0 up to a block) against Cyberpunk's
	// heights, so that the ground where the player aligned it ("Insert") is on a block boundary and
	// blocks sit flush on it instead of half sunk. Saved with the Minecraft world (Grid.cpp).
	inline std::atomic<double> gGridOffset{ 0.0 };

	inline double MetresPerBlock() { return gMetresPerBlock.load(std::memory_order_relaxed); }

	// Positions. Directions (normals, camera axes) take CpDirToMc / McDirToCp: no offset, and no
	// scale, the axes being the same. Lengths: multiply or divide by MetresPerBlock().
	inline McVec CpToMc(float a_x, float a_y, float a_z)
	{
		const double k = 1.0 / MetresPerBlock();
		return { a_x * k, (a_z - gGridOffset.load(std::memory_order_relaxed)) * k, -a_y * k };
	}

	inline CpVec McToCp(double a_x, double a_y, double a_z)
	{
		const double k = MetresPerBlock();
		return { float(a_x * k), float(-a_z * k), float(a_y * k + gGridOffset.load(std::memory_order_relaxed)) };
	}

	inline McVec CpDirToMc(float a_x, float a_y, float a_z)
	{
		return { a_x, a_z, -a_y };
	}

	// Cyberpunk yaw (degrees, 0 = north/+Y, counter-clockwise seen from above) <-> MC yaw
	// (degrees, 0 = south/+Z, 90 = west). Facing direction: CP (-sin y, cos y) in (x, y);
	// MC (-sin y, cos y) in (x, z) with z = -north, so mc = 180 - cp.
	inline float CpYawToMc(float a_yaw) { return 180.0f - a_yaw; }
	inline float McYawToCp(float a_yaw) { return 180.0f - a_yaw; }
	// Cyberpunk pitch is positive looking up; Minecraft's is positive looking down.
	inline float CpPitchToMc(float a_pitch) { return -a_pitch; }
	inline float McPitchToCp(float a_pitch) { return -a_pitch; }
}
