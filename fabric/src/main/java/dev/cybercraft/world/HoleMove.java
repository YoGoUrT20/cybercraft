package dev.cybercraft.world;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;

/**
 * Where the holes dug into Night City go when Minecraft's grid moves under it (CyberDig.remap):
 * pure, so it can be tested without a world.
 */
public final class HoleMove {
	private HoleMove() {
	}

	/**
	 * Where a point in Minecraft coordinates before a grid change is after it (Proto.COL_GRID_MOVED):
	 * x' = m00 x + m02 z + tx, y' = m11 y + ty, z' = m20 x + m22 z + tz. {@code shift}: by whole
	 * blocks (tx, ty, tz) and nothing else.
	 */
	public record GridMove(double m00, double m02, double m20, double m22, double m11, double tx, double ty, double tz, boolean shift) {
		double x(double x, double z) {
			return this.m00 * x + this.m02 * z + this.tx;
		}

		double z(double x, double z) {
			return this.m20 * x + this.m22 * z + this.tz;
		}

		double y(double y) {
			return this.m11 * y + this.ty;
		}

		/** Back from after to before, horizontally: {x, z}. */
		double[] inverse(double x, double z) {
			double det = this.m00 * this.m22 - this.m02 * this.m20;
			double dx = x - this.tx, dz = z - this.tz;
			return new double[] { (this.m22 * dx - this.m02 * dz) / det, (-this.m20 * dx + this.m00 * dz) / det };
		}

		/** Where a cell's lowest block-row y lands, and how many rows it covers after: {first, count}. */
		int[] rows(int y) {
			if (this.shift) {
				return new int[] { y + (int) Math.round(this.ty), 1 };
			}
			// The cells whose middles fall in the cell's span after.
			int first = (int) Math.ceil(y(y) - 0.5), end = (int) Math.ceil(y(y + 1) - 0.5);
			if (end <= first) {
				return new int[] { (int) Math.floor(y(y + 0.5)), 1 };
			}
			return new int[] { first, end - first };
		}
	}

	/**
	 * Where the holes go: new cell -> the old cell it takes its block from, and the openings there.
	 * {@link #moved} is where an old cell's contents went.
	 */
	record Plan(it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap cells, List<CyberDig.Opening> openings,
		it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap forward) {
		long moved(long from) {
			return this.forward.getOrDefault(from, Long.MIN_VALUE);
		}
	}

	/**
	 * Pure: the new dug cells for old ones under {@code move}. Each hole (cells joined face to face)
	 * also moves up or down by whole blocks so that where it was opened from above stays just under
	 * the surface it went through. Sideways the new cells are found from where their middles were
	 * (so a turned hole has no gaps); up and down each row of an old cell becomes the rows it covers.
	 * Where two old cells land in one new one, a {@code solid} one (a block in it) wins over air.
	 */
	static Plan plan(it.unimi.dsi.fastutil.longs.LongSet old, List<CyberDig.Opening> openings, GridMove move, java.util.function.LongPredicate solid) {
		it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap cells = new it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap();
		it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap forward = new it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap();
		List<CyberDig.Opening> moved = new ArrayList<>();
		if (move.shift()) {
			int dx = (int) Math.round(move.tx()), dy = (int) Math.round(move.ty()), dz = (int) Math.round(move.tz());
			for (long pos : old) {
				long to = BlockPos.offset(pos, dx, dy, dz);
				cells.put(to, pos);
				forward.put(pos, to);
			}
			for (CyberDig.Opening o : openings) {
				moved.add(new CyberDig.Opening(o.world(), BlockPos.offset(o.pos(), dx, dy, dz), o.surface() + dy));
			}
			return new Plan(cells, moved, forward);
		}

		// The holes, and for each how far up or down it goes on top of the plain move: its first
		// opening's cell lands on the cell its surface is in now.
		it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap hole = new it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap();
		it.unimi.dsi.fastutil.ints.IntArrayList lift = new it.unimi.dsi.fastutil.ints.IntArrayList();
		for (long start : old) {
			if (hole.containsKey(start)) {
				continue;
			}
			int id = lift.size();
			lift.add(0);
			it.unimi.dsi.fastutil.longs.LongArrayFIFOQueue queue = new it.unimi.dsi.fastutil.longs.LongArrayFIFOQueue();
			queue.enqueue(start);
			hole.put(start, id);
			while (!queue.isEmpty()) {
				long pos = queue.dequeueLong();
				for (Direction d : Direction.values()) {
					long n = BlockPos.offset(pos, d.getStepX(), d.getStepY(), d.getStepZ());
					if (old.contains(n) && !hole.containsKey(n)) {
						hole.put(n, id);
						queue.enqueue(n);
					}
				}
			}
		}
		boolean[] lifted = new boolean[lift.size()];
		for (CyberDig.Opening o : openings) {
			if (!hole.containsKey(o.pos())) {
				continue;
			}
			int id = hole.get(o.pos());
			int[] rows = move.rows(BlockPos.getY(o.pos()));
			int want = (int) Math.floor(move.y(o.surface()) - 0.01);
			if (!lifted[id]) {
				lifted[id] = true;
				lift.set(id, want - (rows[0] + rows[1] - 1));
			}
		}

		// Old cells by column, for finding them from the new columns.
		it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<it.unimi.dsi.fastutil.ints.IntArrayList> columns = new it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<>();
		for (long pos : old) {
			columns.computeIfAbsent(columnKey(BlockPos.getX(pos), BlockPos.getZ(pos)), k -> new it.unimi.dsi.fastutil.ints.IntArrayList()).add(BlockPos.getY(pos));
		}
		// Each new column takes the old column its middle was in (no gaps in a turned or grown hole),
		// and each old column goes where its own middle went (none lost in a shrunk one). The new
		// columns are looked for only over where each old one lands, not between holes far apart.
		for (var e : columns.long2ObjectEntrySet()) {
			int ox = (int) (e.getLongKey() >> 32), oz = (int) e.getLongKey();
			double minX = Double.POSITIVE_INFINITY, minZ = Double.POSITIVE_INFINITY, maxX = Double.NEGATIVE_INFINITY, maxZ = Double.NEGATIVE_INFINITY;
			for (int corner = 0; corner < 4; corner++) {
				double cx = ox + (corner & 1), cz = oz + (corner >> 1);
				minX = Math.min(minX, move.x(cx, cz));
				maxX = Math.max(maxX, move.x(cx, cz));
				minZ = Math.min(minZ, move.z(cx, cz));
				maxZ = Math.max(maxZ, move.z(cx, cz));
			}
			for (int nx = (int) Math.floor(minX); nx <= (int) Math.floor(maxX); nx++) {
				for (int nz = (int) Math.floor(minZ); nz <= (int) Math.floor(maxZ); nz++) {
					double[] back = move.inverse(nx + 0.5, nz + 0.5);
					if ((int) Math.floor(back[0]) == ox && (int) Math.floor(back[1]) == oz) {
						place(cells, forward, columns, hole, lift, move, solid, ox, oz, nx, nz);
					}
				}
			}
			int nx = (int) Math.floor(move.x(ox + 0.5, oz + 0.5)), nz = (int) Math.floor(move.z(ox + 0.5, oz + 0.5));
			place(cells, forward, columns, hole, lift, move, solid, ox, oz, nx, nz);
		}
		// Turned, a straight passage comes out as steps that only touch at their corners, which nobody
		// can walk through: open one of the two cells beside each such step.
		it.unimi.dsi.fastutil.longs.LongArrayList open = new it.unimi.dsi.fastutil.longs.LongArrayList();
		for (var e : cells.long2LongEntrySet()) {
			if (!solid.test(e.getLongValue())) {
				open.add(e.getLongKey());
			}
		}
		for (int i = 0; i < open.size(); i++) {
			long pos = open.getLong(i);
			long from = cells.get(pos);
			for (int dx = -1; dx <= 1; dx += 2) {
				for (int dz = -1; dz <= 1; dz += 2) {
					long corner = BlockPos.offset(pos, dx, 0, dz);
					if (!isOpen(cells, solid, corner)) {
						continue;
					}
					long a = BlockPos.offset(pos, dx, 0, 0), b = BlockPos.offset(pos, 0, 0, dz);
					if (!isOpen(cells, solid, a) && !isOpen(cells, solid, b)) {
						cells.put(a, from);
						open.add(a);
					}
				}
			}
		}
		for (CyberDig.Opening o : openings) {
			if (!hole.containsKey(o.pos())) {
				continue;
			}
			// Kept where its surface is now, over the middle of where its cell was.
			int ox = BlockPos.getX(o.pos()), oz = BlockPos.getZ(o.pos());
			double surface = move.y(o.surface());
			int x = (int) Math.floor(move.x(ox + 0.5, oz + 0.5)), z = (int) Math.floor(move.z(ox + 0.5, oz + 0.5));
			moved.add(new CyberDig.Opening(o.world(), BlockPos.asLong(x, (int) Math.floor(surface - 0.01), z), (float) surface));
		}
		return new Plan(cells, moved, forward);
	}

	/** Old column (ox, oz)'s cells into new column (nx, nz), each row of a cell to the rows it covers. */
	private static void place(it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap cells, it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap forward,
		it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<it.unimi.dsi.fastutil.ints.IntArrayList> columns, it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap hole,
		it.unimi.dsi.fastutil.ints.IntArrayList lift, GridMove move, java.util.function.LongPredicate solid, int ox, int oz, int nx, int nz) {
		var ys = columns.get(columnKey(ox, oz));
		if (ys == null) {
			return;
		}
		for (int i = 0; i < ys.size(); i++) {
			int oy = ys.getInt(i);
			long from = BlockPos.asLong(ox, oy, oz);
			int[] rows = move.rows(oy);
			int k = lift.getInt(hole.get(from));
			for (int r = 0; r < rows[1]; r++) {
				long to = BlockPos.asLong(nx, rows[0] + r + k, nz);
				long had = cells.getOrDefault(to, Long.MIN_VALUE);
				if (had == Long.MIN_VALUE || (!solid.test(had) && solid.test(from))) {
					cells.put(to, from);
				}
				forward.putIfAbsent(from, to);
			}
		}
	}

	private static boolean isOpen(it.unimi.dsi.fastutil.longs.Long2LongOpenHashMap cells, java.util.function.LongPredicate solid, long pos) {
		long from = cells.getOrDefault(pos, Long.MIN_VALUE);
		return from != Long.MIN_VALUE && !solid.test(from);
	}

	private static long columnKey(int x, int z) {
		return ((long) x << 32) | (z & 0xFFFFFFFFL);
	}

}
