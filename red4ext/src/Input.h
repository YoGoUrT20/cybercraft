#pragma once

namespace cybercraft::Input
{
	// Subclasses the game window. Safe to call repeatedly: it only hooks once the window exists.
	void Install();
	void Uninstall();

	// Hooks GetRawInputData so that, while Minecraft drives V, Cyberpunk still reads vertical mouse
	// movement (it tilts its own camera) but never horizontal movement (turning is this plugin's)
	// or the buttons and wheel, which are Minecraft's. Off with bNativeLook = 0 in [Input]: then
	// this plugin owns the whole mouse, and the view only turns left and right. Call once, at load.
	void InstallRawInputHook(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk);
	void UninstallRawInputHook();

	// Cyberpunk owns looking up and down (the hook is in): read pitch back from its camera.
	bool NativeLook();

	// Mouse movement collected since the last call, in raw counts (main thread).
	void ConsumeLook(float& a_dx, float& a_dy);

	// The vertical movement Cyberpunk reads is scaled by this (1 leaves it as it is), so that it
	// tilts its camera as fast as this plugin turns V. 0 while Look.cpp holds the camera at
	// Minecraft's pitch itself.
	void SetPitchScale(float a_scale);
	// Vertical movement Cyberpunk has read since the last call, in counts after that scale (main thread).
	float ConsumeNativeDy();

	// Key and mouse-button presses Cyberpunk has received so far (input not routed to Minecraft).
	// After a load, the first one is "Press Space to continue" or the first step V takes.
	std::uint64_t GamePresses();

	// Insert was pressed while Minecraft drives V: align Minecraft's grid to the ground there, turned the way V faces.
	bool TakeAlignRequest();

	// Arrow presses (and their repeats) since the last call while Minecraft drives V: (forward,
	// left), back and right negative. Each nudges Minecraft's grid a little that way from V.
	std::pair<int, int> TakeNudge();

	// W/S and A/D held down for Minecraft now: (forward, left), each -1, 0 or 1. Diagnostics.
	std::pair<int, int> HeldMovement();

	// Tell Minecraft to let go of everything it thinks is held.
	void ReleaseAll();
}
