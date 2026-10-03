#pragma once

#include "Link.h"

namespace cybercraft
{
	// Shared runtime state between the per-frame update, the window (input) hook and the renderer.
	struct Runtime
	{
		// Minecraft is connected, in its world, has acknowledged our last teleport, and Cyberpunk
		// isn't loading: MC's player position drives V.
		std::atomic<bool> puppeting{ false };
		// The last frame Minecraft drove V (main thread only).
		std::chrono::steady_clock::time_point lastPuppeted{};
		// Minecraft's blocks are drawn (and its render ring drained): while it drives V, and in a car,
		// where Cyberpunk drives and only the blocks show, from the car's camera, without Minecraft's
		// hand and HUD.
		std::atomic<bool> drawBlocks{ false };
		// A Minecraft GUI screen (inventory, chat, ...) is open: mouse moves MC's cursor.
		std::atomic<bool> mcScreenOpen{ false };
		// A Cyberpunk menu (pause, map, journal, loading, ...) owns input.
		std::atomic<bool> gameMenuOpen{ false };
		// MC reports its player is in a world (overlay should be drawn).
		std::atomic<bool> mcInWorld{ false };
		// "Enable CyberCraft" in Mod Settings, as the main thread last read it.
		std::atomic<bool> enabled{ true };
		// V's health is zero: Cyberpunk's death screen owns the screen and the mouse.
		std::atomic<bool> vDead{ false };
		// The keys Minecraft has a control on (McState::mcKeys: bit n = SDL scancode n). The input
		// hook gives Cyberpunk the rest; all zero until Minecraft reports them, and then every key
		// is Minecraft's.
		std::array<std::atomic<std::uint32_t>, 8> mcKeys{};
		// Cyberpunk offers an interaction, loot or dialogue, so F is Cyberpunk's (Minecraft's swap
		// hands otherwise). True when the script that reads it isn't loaded.
		std::atomic<bool> cpInteraction{ true };
		// How much sky light Night City has right now (0.15 at night .. 1 by day), from its clock.
		std::atomic<float> daylight{ 1.0f };

		// Look direction in MC degrees; integrated from raw mouse input (main thread).
		float yaw{ 0.0f };
		float pitch{ 0.0f };
		bool  lookInitialized{ false };

		// Virtual MC cursor (overlay pixels) while an MC screen is open.
		std::atomic<int> cursorX{ 0 };
		std::atomic<int> cursorY{ 0 };
		std::atomic<int> viewportW{ 1920 };
		std::atomic<int> viewportH{ 1080 };

		// Minecraft's crosshair is on screen (first person, no Minecraft screen open) at this GUI scale.
		std::atomic<bool> mcCrosshair{ false };
		std::atomic<int>  mcGuiScale{ 0 };
	};

	// Cyberpunk's first-person camera, in Minecraft space: blocks are drawn from it, so they stay put
	// in the picture Cyberpunk shows instead of following Minecraft's own camera (which runs ahead
	// of V's smoothed position and swam while walking).
	struct CameraView
	{
		bool   valid{ false };
		double eye[3]{};
		float  forward[3]{};
		float  up[3]{};
		float  fovYDeg{ 0.0f };  // vertical
		// Third person: Minecraft's body (kRenAvatar) is drawn standing at these feet: where V is in
		// the frame the camera was read from, so the two move together.
		bool   avatar{ false };
		double feet[3]{};
	};

	// Written by the game thread every frame, read by the renderer.
	void       SetCameraView(const CameraView& a_view);
	CameraView GetCameraView();

	Runtime& State();

	namespace Game
	{
		// RED4ext "Running" state update: once per game frame, main thread (also in the main menu).
		void Tick();
	}
}
