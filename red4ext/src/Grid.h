#pragma once

// Minecraft's grid against Cyberpunk's world (gMetresPerBlock and gGridOffset, Link.h).
//
// Its size: how many metres a block is. Mod Settings' "Minecraft scale" slider when Mod Settings has
// saved it, [World] fMetresPerBlock in CyberCraft.ini otherwise (0.75). Changing it scales the whole
// of Minecraft's world about Night City's origin, so blocks already placed end up elsewhere in the
// city (their Minecraft coordinates stay). Everyone in a shared Minecraft world needs the same one.
//
// Its height: Minecraft blocks start at whole blocks while Night City's ground is anywhere in
// between, so a block on a pavement at 7.3 m sat 0.3 m sunk. Aligning shifts the whole grid so the
// ground under V is on a block boundary and blocks there sit flush. That ground belongs to the
// Minecraft world (its blocks are stored in grid coordinates), so it is saved next to it, in
// %LOCALAPPDATA%\CyberCraft, and the offset follows it to any scale.
//
// Its heading: Night City's streets run every which way, Minecraft's rows north-south and east-west,
// so blocks along a street went up as a staircase. Aligning also turns the grid to the way V faces
// (gGridYaw), about where she stands (gGridShiftX/Y), so blocks there stay where they were, turned.
// Saved with the ground.
namespace cybercraft::Grid
{
	// Loads the scale, the saved ground and heading into gMetresPerBlock, gGridOffset, gGridYaw and
	// gGridShiftX/Y. Call once, at plugin load.
	void Load();

	// Re-reads the scale (at most twice a second). True when it changed: everything in Minecraft
	// coordinates is then somewhere else, as after AlignTo. Main thread.
	bool UpdateScale();

	// Sets gGridOffset so that a_groundCpZ (Cyberpunk metres) lands on a block boundary, and saves
	// it. Returns how far the grid moved (metres).
	double AlignTo(float a_groundCpZ);

	// Turns the grid so its rows run along a_cpYaw (Cyberpunk degrees), whichever of the four ways
	// is nearest its heading now, about (a_cpX, a_cpY): that point keeps its Minecraft coordinates.
	// Saves it. Returns how far it turned (degrees).
	double TurnTo(float a_cpX, float a_cpY, float a_cpYaw);

	// Moves the grid by (a_cpDx, a_cpDy) Cyberpunk metres, and saves it: blocks move that way in
	// Night City.
	void Nudge(double a_cpDx, double a_cpDy);
}
