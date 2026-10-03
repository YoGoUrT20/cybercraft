package dev.cybercraft.world;

import static dev.cybercraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import java.lang.foreign.MemorySegment;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Night City's collision as Minecraft sees it: an 8x8x8 sub-voxel collision shape per block
 * position, which the plugin builds from its ray casts and streams over. These are not blocks; they
 * are merged into block collision queries (see BlockCollisionsMixin) so vanilla movement code
 * collides with them.
 *
 * <p>The store also takes exact triangles (Proto.COL_TRIS), for the smooth player collider
 * (TriCollider), triangle ray casts and digging (CyberDig). The plugin never sends any, so with
 * Cyberpunk that side stays empty and everything keyed off {@link #hasTriangles} is idle.
 */
public final class CyberCollision {
	/** The plugin streams collision in cubes of this many blocks. Must match its Collision::kRegionSize. */
	public static final int REGION_SIZE = 8;

	private static final ConcurrentHashMap<Long, VoxelShape> SHAPES = new ConcurrentHashMap<>();
	// Per block: sub-voxel count (bits 0-9), any in the lower half (bit 10), any in the upper half (bit 11).
	private static final ConcurrentHashMap<Long, Integer> FILL = new ConcurrentHashMap<>();
	private static final int FILL_LOWER = 1 << 10;
	private static final int FILL_UPPER = 1 << 11;
	private static final int FILL_TOP_SHIFT = 12; // highest occupied of the 8 voxel layers (3 bits)
	private static final ConcurrentHashMap<Long, CyberTri[]> TRIS = new ConcurrentHashMap<>();
	// Diggable surfaces as they were before blocks were dug out of them (Proto.TRI_GHOST).
	private static final ConcurrentHashMap<Long, CyberTri[]> GHOSTS = new ConcurrentHashMap<>();
	// A hash of each region's triangles as last received, and the regions whose triangles changed
	// since the client last looked (the walls of dug holes are drawn from them).
	private static final ConcurrentHashMap<Long, Long> TRI_HASH = new ConcurrentHashMap<>();
	private static final java.util.concurrent.ConcurrentLinkedQueue<Long> CHANGED = new java.util.concurrent.ConcurrentLinkedQueue<>();

	/** Regions (min corner, as BlockPos longs) whose triangles changed since the last call. */
	public static void takeChangedRegions(java.util.function.LongConsumer out) {
		Long key;
		while ((key = CHANGED.poll()) != null) {
			out.accept(key);
		}
	}
	// The cars around the player (Proto.COL_VEHICLES): a layer of its own, replaced whole by every
	// message. Never written in place, so readers on any thread see one scan or the next.
	private static volatile java.util.Map<Long, VoxelShape> vehicles = java.util.Map.of();
	private static volatile java.util.function.Predicate<net.minecraft.world.entity.Entity> smoothCollider = e -> false;
	private static final Set<Long> KNOWN_REGIONS = ConcurrentHashMap.newKeySet();
	private static volatile int epoch = -1;
	private static Thread consumer;

	private CyberCollision() {
	}

	public static @Nullable VoxelShape shapeAt(BlockPos pos) {
		return SHAPES.isEmpty() ? null : SHAPES.get(pos.asLong());
	}

	/** The cars in this block, as the plugin last saw them. */
	public static @Nullable VoxelShape vehicleShapeAt(BlockPos pos) {
		java.util.Map<Long, VoxelShape> now = vehicles;
		return now.isEmpty() ? null : now.get(pos.asLong());
	}

	/** True if a car is inside {@code box} (by more than a hair). */
	public static boolean vehicleIn(net.minecraft.world.phys.AABB box) {
		java.util.Map<Long, VoxelShape> now = vehicles;
		if (now.isEmpty()) {
			return false;
		}
		net.minecraft.world.phys.AABB inner = box.deflate(1.0E-3);
		int x0 = (int) Math.floor(inner.minX), x1 = (int) Math.floor(inner.maxX);
		int y0 = (int) Math.floor(inner.minY), y1 = (int) Math.floor(inner.maxY);
		int z0 = (int) Math.floor(inner.minZ), z1 = (int) Math.floor(inner.maxZ);
		for (int x = x0; x <= x1; x++) {
			for (int y = y0; y <= y1; y++) {
				for (int z = z0; z <= z1; z++) {
					VoxelShape shape = now.get(BlockPos.asLong(x, y, z));
					if (shape == null) {
						continue;
					}
					for (net.minecraft.world.phys.AABB part : shape.toAabbs()) {
						if (part.move(x, y, z).intersects(inner)) {
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	public static int vehicleBlockCount() {
		return vehicles.size();
	}

	/** Entities (players, while there are triangles) that collide with the exact triangles instead of the voxels. */
	public static void setSmoothCollider(java.util.function.Predicate<net.minecraft.world.entity.Entity> predicate) {
		smoothCollider = predicate;
	}

	public static boolean usesSmoothCollider(net.minecraft.world.entity.@Nullable Entity entity) {
		return entity != null && smoothCollider.test(entity);
	}

	/** Adds every triangle whose bounds overlap {@code box}. */
	public static void trianglesNear(net.minecraft.world.phys.AABB box, java.util.List<CyberTri> out) {
		near(TRIS, box, out);
	}

	/**
	 * Every triangle whose bounds overlap {@code box} as it was before anything was dug out of it:
	 * what's behind these is inside the geometry (CyberDig).
	 */
	public static void originalSurfacesNear(net.minecraft.world.phys.AABB box, java.util.List<CyberTri> out) {
		near(TRIS, box, out);
		near(GHOSTS, box, out);
	}

	private static void near(ConcurrentHashMap<Long, CyberTri[]> store, net.minecraft.world.phys.AABB box, java.util.List<CyberTri> out) {
		if (store.isEmpty()) {
			return;
		}
		int rx0 = Math.floorDiv((int) Math.floor(box.minX), REGION_SIZE), rx1 = Math.floorDiv((int) Math.floor(box.maxX), REGION_SIZE);
		int ry0 = Math.floorDiv((int) Math.floor(box.minY), REGION_SIZE), ry1 = Math.floorDiv((int) Math.floor(box.maxY), REGION_SIZE);
		int rz0 = Math.floorDiv((int) Math.floor(box.minZ), REGION_SIZE), rz1 = Math.floorDiv((int) Math.floor(box.maxZ), REGION_SIZE);
		for (int rx = rx0; rx <= rx1; rx++) {
			for (int ry = ry0; ry <= ry1; ry++) {
				for (int rz = rz0; rz <= rz1; rz++) {
					CyberTri[] tris = store.get(regionKey(rx, ry, rz));
					if (tris == null) {
						continue;
					}
					for (CyberTri t : tris) {
						if (t.maxX >= box.minX && t.minX <= box.maxX && t.maxY >= box.minY && t.minY <= box.maxY && t.maxZ >= box.minZ && t.minZ <= box.maxZ) {
							out.add(t);
						}
					}
				}
			}
		}
	}

	/** The top of the highest part of the plugin's voxels inside {@code box}, or NaN if none is. */
	public static double voxelTopIn(net.minecraft.world.phys.AABB box) {
		double top = Double.NaN;
		if (SHAPES.isEmpty()) {
			return top;
		}
		int x0 = (int) Math.floor(box.minX), x1 = (int) Math.floor(box.maxX);
		int y0 = (int) Math.floor(box.minY), y1 = (int) Math.floor(box.maxY);
		int z0 = (int) Math.floor(box.minZ), z1 = (int) Math.floor(box.maxZ);
		for (int x = x0; x <= x1; x++) {
			for (int y = y0; y <= y1; y++) {
				for (int z = z0; z <= z1; z++) {
					VoxelShape shape = SHAPES.get(BlockPos.asLong(x, y, z));
					if (shape == null) {
						continue;
					}
					for (net.minecraft.world.phys.AABB part : shape.toAabbs()) {
						part = part.move(x, y, z);
						if (part.intersects(box) && !(part.maxY <= top)) {
							top = part.maxY;
						}
					}
				}
			}
		}
		return top;
	}

	/** True once the plugin has sent the region containing this block (even if it was empty). */
	public static boolean isKnown(int x, int y, int z) {
		return KNOWN_REGIONS.contains(regionKey(Math.floorDiv(x, REGION_SIZE), Math.floorDiv(y, REGION_SIZE), Math.floorDiv(z, REGION_SIZE)));
	}

	/** True if any of the city's collision is in the 3x3 column below (x, y, z), down to {@code depth} blocks. */
	public static boolean hasSolidBelow(int x, int y, int z, int depth) {
		for (int dy = 0; dy <= depth; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				for (int dz = -1; dz <= 1; dz++) {
					if (SHAPES.containsKey(BlockPos.asLong(x + dx, y - dy, z + dz))) {
						return true;
					}
				}
			}
		}
		return false;
	}

	/** Fraction (0..1) of this block's volume that is the city's collision. */
	public static float solidFraction(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		return fill == null ? 0.0F : (fill & 0x3FF) / 512.0F;
	}

	/** True if any of the city's collision is in this cell. */
	public static boolean hasGeometry(BlockPos pos) {
		return !FILL.isEmpty() && FILL.containsKey(pos.asLong());
	}

	/**
	 * How high (0..1) the city's collision reaches in this cell: the top of its highest part. Ground
	 * arrives as a thin slab (the rays only find its top), so what lies below counts as ground too.
	 */
	public static float groundTop(BlockPos pos) {
		Integer fill = FILL.isEmpty() ? null : FILL.get(pos.asLong());
		return fill == null ? 0.0F : (((fill >> FILL_TOP_SHIFT) & 7) + 1) / 8.0F;
	}

	/** True if the city's ground holds up whatever is in this cell (collision in its lower half or the upper half of the cell below). */
	public static boolean supportsFromBelow(BlockPos pos) {
		if (FILL.isEmpty()) {
			return false;
		}
		Integer here = FILL.get(pos.asLong());
		if (here != null && (here & FILL_LOWER) != 0) {
			return true;
		}
		Integer below = FILL.get(BlockPos.asLong(pos.getX(), pos.getY() - 1, pos.getZ()));
		return below != null && (below & FILL_UPPER) != 0;
	}

	public static int blockCount() {
		return SHAPES.size();
	}

	public static int regionCount() {
		return KNOWN_REGIONS.size();
	}

	/** The plugin is streaming the city around the player (false in a plain Minecraft world). */
	public static boolean active() {
		return !KNOWN_REGIONS.isEmpty();
	}

	private static long regionKey(int rx, int ry, int rz) {
		return BlockPos.asLong(rx, ry, rz);
	}

	public static synchronized void startConsumer() {
		if (consumer != null) {
			return;
		}
		consumer = new Thread(CyberCollision::consumeLoop, "CyberCraft collision");
		consumer.setDaemon(true);
		consumer.start();
	}

	private static void consumeLoop() {
		while (true) {
			try {
				if (!drainOnce()) {
					Thread.sleep(2);
				}
			} catch (InterruptedException e) {
				return;
			} catch (Throwable t) {
				CyberCraft.LOG.error("CyberCraft: collision consumer error", t);
				try {
					Thread.sleep(500);
				} catch (InterruptedException e) {
					return;
				}
			}
		}
	}

	/** Processes all pending collision messages. Returns true if anything was consumed. */
	private static boolean drainOnce() {
		MemorySegment s = CyberLink.segment();
		if (s == null) {
			return false;
		}
		long head = CyberLink.collisionHead();
		long tail = CyberLink.collisionTail();
		if (tail >= head) {
			return false;
		}
		long data = OFF_COLLISION_RING + CR_DATA;
		while (tail < head) {
			long pos = tail % CR_DATA_BYTES;
			int type = s.get(JAVA_INT, data + pos);
			int payloadBytes = s.get(JAVA_INT, data + pos + 4);
			if (type == COL_PAD) {
				tail += CR_DATA_BYTES - pos;
				continue;
			}
			long payload = data + pos + 8;
			switch (type) {
				case COL_CLEAR -> clear(s.get(JAVA_INT, payload));
				case COL_REGION -> readRegion(s, payload);
				case COL_TRIS -> readTris(s, payload);
				case COL_VEHICLES -> readVehicles(s, payload);
				default -> CyberCraft.LOG.warn("CyberCraft: unknown collision message {}", type);
			}
			tail += align8(8 + payloadBytes);
		}
		CyberLink.setCollisionTail(tail);
		return true;
	}

	private static long align8(long v) {
		return (v + 7) & ~7L;
	}

	/** A freshly started client joins whatever collision epoch the plugin is already on. */
	private static void adoptEpochIfFresh(int msgEpoch) {
		if (epoch == -1) {
			epoch = msgEpoch;
			CyberCraft.LOG.info("CyberCraft: joined collision epoch {} already in progress", msgEpoch);
		}
	}

	private static void clear(int newEpoch) {
		SHAPES.clear();
		FILL.clear();
		TRIS.clear();
		GHOSTS.clear();
		TRI_HASH.clear();
		KNOWN_REGIONS.clear();
		vehicles = java.util.Map.of();
		epoch = newEpoch;
		CyberCraft.LOG.info("CyberCraft: collision cleared (epoch {})", newEpoch);
	}

	private static void readRegion(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int maxX = s.get(JAVA_INT, p + 12);
		int maxY = s.get(JAVA_INT, p + 16);
		int maxZ = s.get(JAVA_INT, p + 20);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return; // stale region from before a world change
		}

		// Build the new shapes first so readers never see a half-empty region.
		java.util.HashMap<Long, VoxelShape> fresh = new java.util.HashMap<>(count * 2);
		java.util.HashMap<Long, Integer> freshFill = new java.util.HashMap<>(count * 2);
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
			int x = s.get(JAVA_INT, e);
			int y = s.get(JAVA_INT, e + 4);
			int z = s.get(JAVA_INT, e + 8);
			VoxelShape shape = buildShape(s, e + 16);
			if (shape != null) {
				long key = BlockPos.asLong(x, y, z);
				fresh.put(key, shape);
				freshFill.put(key, fillInfo(s, e + 16));
			}
		}

		for (int x = minX; x <= maxX; x++) {
			for (int y = minY; y <= maxY; y++) {
				for (int z = minZ; z <= maxZ; z++) {
					long key = BlockPos.asLong(x, y, z);
					VoxelShape shape = fresh.get(key);
					if (shape != null) {
						SHAPES.put(key, shape);
						FILL.put(key, freshFill.get(key));
					} else {
						SHAPES.remove(key);
						FILL.remove(key);
					}
				}
			}
		}

		for (int rx = Math.floorDiv(minX, REGION_SIZE); rx <= Math.floorDiv(maxX, REGION_SIZE); rx++) {
			for (int ry = Math.floorDiv(minY, REGION_SIZE); ry <= Math.floorDiv(maxY, REGION_SIZE); ry++) {
				for (int rz = Math.floorDiv(minZ, REGION_SIZE); rz <= Math.floorDiv(maxZ, REGION_SIZE); rz++) {
					KNOWN_REGIONS.add(regionKey(rx, ry, rz));
				}
			}
		}
	}

	/** The whole car layer, replacing the last one (header box: where the plugin looked). */
	private static void readVehicles(MemorySegment s, long p) {
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return;
		}
		java.util.HashMap<Long, VoxelShape> fresh = new java.util.HashMap<>(count * 2);
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_BLOCK_BYTES) {
			VoxelShape shape = buildShape(s, e + 16);
			if (shape != null) {
				fresh.put(BlockPos.asLong(s.get(JAVA_INT, e), s.get(JAVA_INT, e + 4), s.get(JAVA_INT, e + 8)), shape);
			}
		}
		vehicles = fresh.isEmpty() ? java.util.Map.of() : java.util.Collections.unmodifiableMap(fresh);
	}

	private static void readTris(MemorySegment s, long p) {
		int minX = s.get(JAVA_INT, p);
		int minY = s.get(JAVA_INT, p + 4);
		int minZ = s.get(JAVA_INT, p + 8);
		int msgEpoch = s.get(JAVA_INT, p + 24);
		int count = s.get(JAVA_INT, p + 28);
		adoptEpochIfFresh(msgEpoch);
		if (msgEpoch != epoch) {
			return;
		}
		CyberTri[] tris = new CyberTri[count];
		java.util.List<CyberTri> ghosts = new java.util.ArrayList<>();
		float[] v = new float[9];
		int kept = 0;
		long hash = count;
		long e = p + COL_REGION_HEADER_BYTES;
		for (int i = 0; i < count; i++, e += COL_TRI_BYTES) {
			for (int k = 0; k < 9; k++) {
				v[k] = s.get(JAVA_FLOAT, e + k * 4L);
				hash = hash * 31 + Float.floatToRawIntBits(v[k]);
			}
			int flags = s.get(JAVA_INT, e + 36);
			hash = hash * 31 + flags;
			CyberTri t = new CyberTri(v, 0, flags);
			if (t.degenerate()) {
				continue;
			}
			if ((flags & TRI_GHOST) != 0) {
				ghosts.add(t);
			} else {
				tris[kept++] = t;
			}
		}
		long region = regionKey(Math.floorDiv(minX, REGION_SIZE), Math.floorDiv(minY, REGION_SIZE), Math.floorDiv(minZ, REGION_SIZE));
		if (ghosts.isEmpty()) {
			GHOSTS.remove(region);
		} else {
			GHOSTS.put(region, ghosts.toArray(new CyberTri[0]));
		}
		TRIS.put(region, java.util.Arrays.copyOf(tris, kept));
		Long before = TRI_HASH.put(region, hash);
		if (before == null || before != hash) {
			CHANGED.add(BlockPos.asLong(minX, minY, minZ));
		}
	}

	/** Any exact triangles at all. Always false with Cyberpunk: the plugin sends voxels from ray casts only. */
	public static boolean hasTriangles() {
		return !TRIS.isEmpty();
	}

	public static int triangleCount() {
		int n = 0;
		for (CyberTri[] t : TRIS.values()) {
			n += t.length;
		}
		return n;
	}

	private static int fillInfo(MemorySegment s, long bitsOff) {
		int count = 0;
		int info = 0;
		int top = 0;
		for (int y = 0; y < 8; y++) {
			long layer = s.get(JAVA_LONG, bitsOff + y * 8L);
			count += Long.bitCount(layer);
			if (layer != 0) {
				info |= y < 4 ? FILL_LOWER : FILL_UPPER;
				top = y;
			}
		}
		return info | count | top << FILL_TOP_SHIFT;
	}

	private static @Nullable VoxelShape buildShape(MemorySegment s, long bitsOff) {
		boolean any = false;
		boolean full = true;
		long[] layers = new long[8];
		for (int y = 0; y < 8; y++) {
			layers[y] = s.get(JAVA_LONG, bitsOff + y * 8L);
			any |= layers[y] != 0;
			full &= layers[y] == -1L;
		}
		if (!any) {
			return null;
		}
		if (full) {
			return Shapes.block();
		}
		BitSetDiscreteVoxelShape discrete = new BitSetDiscreteVoxelShape(8, 8, 8);
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			while (layer != 0) {
				int bit = Long.numberOfTrailingZeros(layer);
				layer &= layer - 1;
				discrete.fill(bit & 7, y, bit >>> 3);
			}
		}
		return new CubeVoxelShape(discrete);
	}
}
