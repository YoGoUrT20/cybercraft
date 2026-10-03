#pragma once

#include "Rtti.h"

// Looking around exactly as Minecraft's mouse does (MouseHandler.turnPlayer and Entity.turn in
// Minecraft 26.3): its sensitivity curve, the cinematic camera, the spyglass's slower look, inverted
// axes, and pitch held to +-90. Minecraft reads raw mouse counts (SDL relative mode), and so does
// this plugin, so a count turns V as far as it would turn Minecraft's player.
//
// Turning goes out with V's teleport. Looking up and down is Cyberpunk's camera, and that is held at
// Minecraft's pitch through its pitch limits (pitchMin = pitchMax), so it is Minecraft's look too, not
// Cyberpunk's own sensitivity scaled to match. Whether the camera follows is checked as V looks
// around; if it doesn't, Cyberpunk tilts it from the mouse again, scaled to Minecraft's speed.
//
// [Input] in CyberCraft.ini:
//   bPinPitch     hold Cyberpunk's camera at Minecraft's pitch                            (1)
//   bMatchPitch   otherwise: Cyberpunk's tilt scaled to Minecraft's speed; 0 its own      (1)
namespace cybercraft::Look
{
	// Minecraft's mouse options (McState).
	struct Options
	{
		float sensitivity{ 0.5f };  // the slider, 0..1 (0.5 is "100%")
		bool  smooth{ false };      // cinematic camera
		bool  scoping{ false };     // looking through a spyglass in first person
		bool  invertX{ false };
		bool  invertY{ false };
	};

	// What this frame's mouse movement turns Minecraft's player by, in degrees (yaw right, pitch
	// down), and how far one count would tilt it without the cinematic camera.
	struct Turn
	{
		float yaw{ 0.0f };
		float pitch{ 0.0f };
		float pitchPerCount{ 0.0f };
	};

	// Main thread, every frame. a_dx/a_dy: raw counts since the last frame; a_seconds: the frame's
	// length. a_looking: the mouse turns the player (Minecraft drives V, no screen or menu open);
	// otherwise nothing turns, and the cinematic camera keeps its momentum for later as Minecraft's does.
	Turn Integrate(float a_dx, float a_dy, float a_seconds, bool a_looking, const Options& a_options);

	// Main thread, every frame, after Integrate. Applies the turn's pitch, brings Cyberpunk's camera
	// there, and returns the pitch Minecraft's player gets (Minecraft degrees, positive down).
	// a_driving: Minecraft drives V; a_cameraPitch: Cyberpunk's camera this frame, if it could be read;
	// a_pitch: the pitch Minecraft had.
	float Pitch(const rtti::Handle<rtti::IScriptable>& a_player, bool a_driving, bool a_looking, const Turn& a_turn,
		const std::optional<float>& a_cameraPitch, float a_pitch, float a_seconds);

	// Minecraft let go of V (a car, death, a load): the camera's own pitch limits go back, and the
	// cinematic camera's momentum is dropped. Pitch calls it itself when a_driving is false.
	void Release(const rtti::Handle<rtti::IScriptable>& a_player);

	// Where the pitch comes from, for the log.
	const char* PitchSource();
}
