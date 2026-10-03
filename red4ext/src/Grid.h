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
namespace cybercraft::Grid
{
	// Loads the scale and the saved ground into gMetresPerBlock and gGridOffset. Call once, at
	// plugin load.
	void Load();

	// Re-reads the scale (at most twice a second). True when it changed: everything in Minecraft
	// coordinates is then somewhere else, as after AlignTo. Main thread.
	bool UpdateScale();

	// Sets gGridOffset so that a_groundCpZ (Cyberpunk metres) lands on a block boundary, and saves
	// it. Returns how far the grid moved (metres).
	double AlignTo(float a_groundCpZ);
}
