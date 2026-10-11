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

	/** CyberDig.classifyVoxel. */
	static int classify(int x, int y, int z) {
		if (CyberCollision.rawGeometry(x, y, z)) {
			return CyberDig.PARTIAL;
		}
		if (CyberCollision.rawFloor(x, y - 1, z)) {
			return CyberDig.AIR;
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

	/** How many cells tall a thing standing on a floor can be and still stand free. */
	private static final int FREE_HEIGHT = 2;
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
	 * True if what was in this cell stands free in the open: a chair, a bench, a bin, a low wall, a
	 * parapet's top. It stands on a floor right under it, is no more than FREE_HEIGHT cells tall, and
	 * has open air (or cells {@code dug} out) beside it on two sides or more and ground on none.
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
		return open >= 2;
	}

	private static int sides(int x, int y, int z) {
		if (!CyberCollision.rawGeometry(x, y, z)) {
			return 0; // ground under the surface, revealed
		}
		// Down to the floor it stands on, through the rest of it...
		int height = 1;
		int down = y - 1;
		while (!CyberCollision.rawFloor(x, down, z)) {
			if (!CyberCollision.rawGeometry(x, down, z) || ++height > FREE_HEIGHT) {
				return 0; // over nothing the voxels show (the street's surface, an awning) or part of something tall
			}
			down--;
		}
		// ...up to where the city stops over it...
		for (int up = y + 1; CyberCollision.rawGeometry(x, up, z); up++) {
			if (++height > FREE_HEIGHT) {
				return 0; // a building's wall, a lamp post
			}
		}
		// ...and a floor on a floor is the ground (a pavement on the road), unless it's raised off it (a seat, a table).
		if (down == y - 1 && CyberCollision.rawFloor(x, y, z) && CyberCollision.rawBottom(x, y, z) + 1.0F - CyberCollision.rawTop(x, down, z) < 0.1F) {
			return 0;
		}
		int sides = FREE_THING;
		for (int i = 0; i < 4; i++) {
			int nx = x + SIDES[i].getStepX(), nz = z + SIDES[i].getStepZ();
			if (CyberCollision.rawGeometry(nx, y, nz)) {
				// Only a floor at the very bottom of it (the one the thing stands on) leaves room over it.
				if (CyberCollision.rawFloor(nx, y, nz) && CyberCollision.rawTop(nx, y, nz) <= 0.25F) {
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
