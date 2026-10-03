#pragma once

// CyberCraft's entry in Cyberpunk's Mod Settings (scripts/CyberCraftModSettings.reds, jackhumbert's
// Mod Settings mod). "Enable CyberCraft" turns the whole mod off and on, "Minecraft scale" sizes
// Minecraft's world against Night City (Grid.cpp). Its "Open Minecraft settings"
// row leaves a request in the script, which this collects every frame in game and passes on to
// Minecraft as "open your options screen". The script has closed Cyberpunk's menu by then, so the
// screen shows over the game like any other.
namespace cybercraft::ModSettings
{
	// "Enable CyberCraft": true without Mod Settings, or until the row has been saved once. Read from
	// Mod Settings' user.ini, where it keeps what is accepted, so it is known at load (before
	// Minecraft would be started) and in the main menu; re-read at most twice a second. Main thread.
	bool Enabled();

	// "Minecraft scale", metres per block, as Mod Settings last saved it; nothing without Mod
	// Settings or until it has saved CyberCraft's rows once. Read from user.ini each call (Grid.cpp
	// asks at most twice a second).
	std::optional<float> MetresPerBlock();

	// Main thread, every frame a save is loaded. a_canShow: Minecraft drives V, so its screens show;
	// a request made while they can't (in a car, V dead) is dropped rather than kept for later.
	void Update(bool a_canShow);
}
