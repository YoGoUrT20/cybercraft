#include "Config.h"
#include "Game.h"
#include "Grid.h"
#include "Input.h"
#include "Launcher.h"
#include "ModSettings.h"
#include "SceneDepth.h"
#include "Overlay.h"

namespace
{
	bool OnRunningUpdate(RED4ext::CGameApplication*)
	{
		cybercraft::Game::Tick();
		return false;  // keep updating (ignored for the Running state)
	}
}

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle a_handle, RED4ext::v1::EMainReason a_reason, const RED4ext::v1::Sdk* a_sdk)
{
	using namespace cybercraft;
	switch (a_reason) {
	case RED4ext::v1::EMainReason::Load:
		{
			Config::Init();
			log::Init(Config::GameDir() / L"red4ext" / L"logs" / L"CyberCraft.log");
			logger::info("CyberCraft {} loading (game {}.{}{})", CYBERCRAFT_VERSION, a_sdk->runtime->major, a_sdk->runtime->minor, a_sdk->runtime->patch);
			if (!Link::Get().Create()) {
				logger::error("CyberCraft disabled: could not create shared memory");
				return true;
			}
			static RED4ext::v1::GameState running{ nullptr, &OnRunningUpdate, nullptr };
			a_sdk->gameStates->Add(a_handle, RED4ext::EGameStateType::Running, &running);
			// scripts/CyberCraft.reds: lights and colliders for blocks, V hidden in third person (Codeware).
			const auto scripts = Config::PluginDir() / L"scripts";
			if (std::filesystem::exists(scripts)) {
				const bool added = a_sdk->scripts->Add(a_handle, scripts.c_str());
				logger::info("scripts: {} added to the redscript compilation: {}", scripts.string(), added);
			} else {
				logger::warn("scripts: {} is missing; no lights or colliders for blocks", scripts.string());
			}
			Grid::Load();
			Input::InstallRawInputHook(a_handle, a_sdk);
			SceneDepth::Install(a_handle, a_sdk);
			// As early as possible: Minecraft boots and opens its world (half a minute) while
			// Cyberpunk sits in its menu and loads the save. It touches nothing until V has loaded
			// in (Game::Tick's load gate); started only then, it kept players waiting ~30 s.
			// Turned off in Mod Settings: not until it's turned on (Game::Tick).
			if (ModSettings::Enabled()) {
				Launcher::StartMinecraft();
			} else {
				logger::info("CyberCraft is off in Mod Settings: Minecraft starts once it's turned on");
			}
			break;
		}
	case RED4ext::v1::EMainReason::Unload:
		logger::info("CyberCraft unloading");
		Input::UninstallRawInputHook();
		SceneDepth::Uninstall();
		Overlay::Shutdown();
		break;
	}
	return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* a_info)
{
	a_info->name = L"CyberCraft";
	a_info->author = L"CyberCraft";
	a_info->version = RED4EXT_V1_SEMVER(CYBERCRAFT_VERSION_MAJOR, CYBERCRAFT_VERSION_MINOR, CYBERCRAFT_VERSION_PATCH);
	a_info->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
	a_info->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT std::uint32_t RED4EXT_CALL Supports()
{
	return RED4EXT_API_VERSION_1;
}
