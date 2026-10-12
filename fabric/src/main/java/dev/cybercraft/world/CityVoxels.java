package dev.cybercraft.world;

import static dev.cybercraft.link.Proto.*;

import java.util.concurrent.ConcurrentHashMap;
import java.util.function.LongPredicate;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;

/**
 * What the plugin's voxels (all Night City sends: its surfaces, from ray casts) say about the city
 * round a dug hole: what a cell next to one is ({@link #classify}), and whether what was in a dug cell
 * stood free ({@link #freeStanding}). Voxels only, no level (CyberDig keeps the dug cells): any thread.
 */
final class CityVoxels {
	private CityVoxels() {
	}

	/** How far up from a cell the city's surface over it is looked for: more than CITY_BEDROCK. */
	static final int COVER_REACH = 20;
	/** Blocks this far under the city's surface are bedrock: holes go no deeper (V stays in reach of the city). */
	private static final double CITY_BEDROCK = 16.0;
	/** Blocks less far than this under the city's surface are dirt, deeper ones stone. */
	private static final double CITY_DIRT = 3.5;
	/**
	 * A cell over a floor this close under it (blocks), with standing room over that floor (OPEN_ROOM
	 * cells clear, this one among them), is in the open: a terrace, a room, a street under an overpass.
	 * A player gets there, so it's never a block, whatever is over it (a ceiling, an awning, a sign, an
	 * atrium's roof 16 blocks up turned it into bedrock). A gap thinner than that (under a street's
	 * surface) is ground: blown or dug into, it doesn't open into nothing.
	 */
	private static final int OPEN_REACH = 6;
	private static final int OPEN_ROOM = 2;

	/** CyberDig.classifyVoxel. */
	static int classify(int x, int y, int z) {
		if (CyberCollision.rawGeometry(x, y, z)) {
			return CyberDig.PARTIAL;
		}
		for (int k = 1; k <= OPEN_REACH; k++) {
			if (CyberCollision.rawGeometry(x, y - k, z)) {
				if (CyberCollision.rawFloor(x, y - k, z)) {
					int room = k; // the clear cells over the floor up to this one, this one too
					for (int up = 1; room < OPEN_ROOM && up < OPEN_ROOM && !CyberCollision.rawGeometry(x, y + up, z); up++) {
						room++;
					}
					if (room >= OPEN_ROOM) {
						return CyberDig.AIR;
					}
				}
				break;
			}
		}
		for (int k = 1; k <= COVER_REACH; k++) {
			if (CyberCollision.rawGeometry(x, y + k, z)) {
				double depth = k - 0.5 + CyberCollision.rawTop(x, y + k, z);
				return depth >= CITY_BEDROCK ? DIG_BEDROCK : depth < CITY_DIRT ? DIG_DIRT : DIG_STONE;
			}
		}
		return CyberDig.AIR;
	}

	// ---- what stands free ------------------------------------------------------------------------

	private static final int FREE_THING = 1, FREE_OPEN = 1 << 1, FREE_GROUND = 1 << 5; // FREE_OPEN, FREE_GROUND: << side
	private static final Direction[] SIDES = { Direction.NORTH, Direction.EAST, Direction.SOUTH, Direction.WEST };
	// Per cell, by section, as worked out from the voxels alone: FREE_THING for a thing standing on a
	// floor, and for each side whether there's open air or ground beside it. 0: not one.
	private static final ConcurrentHashMap<Long, ConcurrentHashMap<Long, Integer>> FREE = new ConcurrentHashMap<>();

	/** The voxels changed everywhere (another world). */
	static void forget() {
		FREE.clear();
	}

	/** The voxels a cell in this section looks at changed. */
	static void forget(long section) {
		FREE.remove(section);
	}

	/**
	 * True if what was in this cell stands free in the open: a chair, a sofa's cushion, a bin, a low
	 * wall, a parapet's top, a window's frame, a post. It stands on a floor (straight down through the
	 * rest of it), has open air (or cells {@code dug} out) beside it on a side or more, and ground on
	 * none: there's no earth behind it, so nothing of stone comes with it. Ground beside it (a hillside,
	 * the street's rim) or nothing under it (the street's surface, an awning) and it's the city's ground.
	 */
	static boolean freeStanding(int x, int y, int z, LongPredicate dug) {
		if (FREE.size() > 2048) {
			FREE.clear();
		}
		var cells = FREE.computeIfAbsent(SectionPos.asLong(x >> 4, y >> 4, z >> 4), k -> new ConcurrentHashMap<>());
		long key = BlockPos.asLong(x, y, z);
		Integer known = cells.get(key);
		int sides = known != null ? known : sides(x, y, z);
		if (known == null) {
			cells.put(key, sides);
		}
		if (sides == 0) {
			return false;
		}
		int open = 0;
		for (int i = 0; i < 4; i++) {
			boolean gone = dug.test(BlockPos.asLong(x + SIDES[i].getStepX(), y, z + SIDES[i].getStepZ()));
			if ((sides & FREE_GROUND << i) != 0 && !gone) {
				return false; // the edge of solid ground (a terrace's corner), not a thing on it
			}
			if (gone || (sides & FREE_OPEN << i) != 0) {
				open++;
			}
		}
		return open >= 1;
	}

	private static int sides(int x, int y, int z) {
		if (!CyberCollision.rawGeometry(x, y, z)) {
			return 0; // ground under the surface, revealed
		}
		// Down to the floor it stands on, through the rest of it...
		int down = y - 1;
		while (!CyberCollision.rawFloor(x, down, z)) {
			if (!CyberCollision.rawGeometry(x, down, z) || y - down > COVER_REACH) {
				return 0; // over nothing the voxels show (the street's surface, an awning)
			}
			down--;
		}
		// ...and a bare thin floor right on a floor is the ground (a pavement on the road), unless it's
		// raised off it (a seat, a table), thicker (a cushion on a sofa) or has something on it (a chair's
		// legs in the same cell as the floor's top).
		if (down == y - 1 && CyberCollision.rawFloor(x, y, z) && CyberCollision.rawFloorTop(x, y, z) <= 0.5F
			&& CyberCollision.rawTop(x, y, z) <= CyberCollision.rawFloorTop(x, y, z) + 0.125F && !CyberCollision.rawGeometry(x, y + 1, z)
			&& CyberCollision.rawFloorBottom(x, y, z) + 1.0F - CyberCollision.rawFloorTop(x, down, z) < 0.1F) {
			return 0;
		}
		int sides = FREE_THING;
		for (int i = 0; i < 4; i++) {
			int nx = x + SIDES[i].getStepX(), nz = z + SIDES[i].getStepZ();
			if (CyberCollision.rawGeometry(nx, y, nz)) {
				// Only a floor at the very bottom of it (the one the thing stands on) leaves room over it.
				if (CyberCollision.rawFloor(nx, y, nz) && CyberCollision.rawFloorTop(nx, y, nz) <= 0.25F) {
					sides |= FREE_OPEN << i;
				}
				continue;
			}
			int kind = classify(nx, y, nz);
			sides |= kind == CyberDig.AIR ? FREE_OPEN << i : kind > CyberDig.AIR ? FREE_GROUND << i : 0;
		}
		return sides;
	}
}
