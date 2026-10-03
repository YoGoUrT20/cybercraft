#pragma once

#include "Link.h"

// Night City's clock and weather, which Minecraft's time and weather follow, and Minecraft's /time
// and /weather carried back the other way (the Fabric mod's CyberClock is the other half).
//
// Weather is read as a state name (24h_weather_rain, ...) and how hard it rains, and sorted into
// what Minecraft has: clear, rain, thunder. A weather mod that adds states of its own is picked up
// as Night City switches to them; one that changes the game's own states needs nothing, since
// setting a state shows the mod's version of it. Archive-only mods (Enhanced Weather) have no API,
// so this is all there is to use.
//
// [Weather] in CyberCraft.ini:
//   bSync           on/off                                                     (1)
//   sClear          state for /weather clear                (24h_weather_sunny)
//   sRain           state for /weather rain                 (a mod's rain state seen, else 24h_weather_rain)
//   sThunder        state for /weather thunder              (a mod's storm state seen, else sRain's)
//   fBlendSeconds   how long Night City takes to change     (10)
//   iPriority       the weather request's priority          (5)
//   fRainThreshold  rain intensity that counts as rain      (0.1)
//   bKeepOnTimeChange  /time leaves the weather as it was   (1)
namespace cybercraft::Environment
{
	// Night City's clock as an hour of day (0-24); none outside a game session. Main thread.
	std::optional<float> Hour();

	// Night City's weather as Minecraft knows it. Main thread, every frame (read a few times a
	// second, and kept in between).
	proto::Weather Weather();

	// Minecraft's time was changed: Night City's clock moves forward to a_hour, the way waiting does,
	// and its weather is set again to what it was (the jump would roll it forward too).
	void SetHour(float a_hour);

	// Minecraft's weather was changed: Night City changes to the matching state.
	void SetWeather(proto::Weather a_weather);
}
