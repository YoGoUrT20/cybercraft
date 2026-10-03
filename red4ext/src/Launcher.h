#pragma once

namespace cybercraft::Launcher
{
	// At plugin load: starts Minecraft (per CyberCraft.ini) unless it's already running.
	void StartMinecraft();

	enum class Status
	{
		kOff,         // not started by CyberCraft (bStartWithCyberpunk = 0)
		kRunning,     // was already running
		kStarting,    // being started
		kSignIn,      // first start of the bundled Minecraft: Prism asks for a Microsoft account
		kNoLauncher,  // nothing to start it with
		kFailed,      // starting it failed
	};
	Status GetStatus();

	// A Minecraft with the mod is running (it holds CyberCraft's mutex from early on).
	bool MinecraftRunning();
	// Prism Launcher is running (downloading, waiting on a sign-in or showing an error).
	bool PrismRunning();
}
