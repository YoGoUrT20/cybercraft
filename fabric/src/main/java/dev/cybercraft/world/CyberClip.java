package dev.cybercraft.world;

import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * Makes Minecraft ray casts (arrows and other projectiles, the crosshair pick) hit Night City's
 * collision: the plugin's voxels, or exact triangles where there are any (the plugin sends none).
 * Vanilla still clips against real Minecraft blocks; whichever is nearer wins.
 */
public final class CyberClip {
	private CyberClip() {
	}

	public enum Use {
		/** A projectile: the hit cell is the one the surface is in (it sticks there). */
		PROJECTILE,
		/** The player's crosshair: the hit cell is where a block placed against the surface goes. */
		PICK
	}

	private static final ThreadLocal<List<CyberTri>> SCRATCH = ThreadLocal.withInitial(ArrayList::new);

	public static BlockHitResult refine(Vec3 from, Vec3 to, BlockHitResult vanilla, Use use) {
		CyberRay.Hit hit = cast(from, to);
		if (use == Use.PICK) {
			// The walls of a dug hole (the ground the crosshair meets from inside the hole).
			double limit = hit != null ? hit.t() : 1.0;
			if (vanilla.getType() != HitResult.Type.MISS) {
				limit = Math.min(limit, Math.sqrt(from.distanceToSqr(vanilla.getLocation()) / Math.max(from.distanceToSqr(to), 1e-9)));
			}
			CyberRay.Hit wall = digWall(from, to, limit);
			if (wall != null) {
				hit = wall;
				vanilla = BlockHitResult.miss(to, Direction.UP, BlockPos.containing(to));
			}
		}
		if (hit == null) {
			return vanilla;
		}
		Vec3 location = new Vec3(hit.x(), hit.y(), hit.z());
		if (vanilla.getType() != HitResult.Type.MISS && from.distanceToSqr(vanilla.getLocation()) <= from.distanceToSqr(location)) {
			return vanilla;
		}
		Direction face = Direction.values()[CyberRay.dominantFace(hit.nx(), hit.ny(), hit.nz())];
		// On the plugin's voxels (Night City's flat streets and walls) the block goes to the grid line
		// nearest the surface: at most half a block sunk or proud, where the 0.4 out meant for sloped
		// triangles sank it up to 0.6 into a sidewalk or wall.
		int[] cell = use != Use.PICK ? CyberRay.surfaceCell(hit)
			: hit.tri() == VOXEL_SURFACE ? CyberRay.placementCell(hit, 0.5) : CyberRay.placementCell(hit);
		return new CyberHitResult(location, face, new BlockPos(cell[0], cell[1], cell[2]), hit);
	}

	private static final CyberTri STONE_WALL = new CyberTri(new float[9], 0, dev.cybercraft.link.Proto.TRI_DIGGABLE | (dev.cybercraft.link.Proto.DIG_STONE << dev.cybercraft.link.Proto.TRI_MATERIAL_SHIFT));

	/**
	 * Where the segment, having gone through dug cells, first enters an undug cell inside the
	 * geometry (the wall of a hole, drawn by the client's DigWalls), before segment parameter
	 * {@code limit}; or null. Never a hit with Cyberpunk: nothing is dug without triangles (CyberDig).
	 */
	static CyberRay.Hit digWall(Vec3 from, Vec3 to, double limit) {
		CyberDig.DugLookup dug = CyberDig.clientDug;
		if (dug == null) {
			return null;
		}
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		int x = (int) Math.floor(from.x), y = (int) Math.floor(from.y), z = (int) Math.floor(from.z);
		int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
		double tDeltaX = dx == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dx);
		double tDeltaY = dy == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dy);
		double tDeltaZ = dz == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dz);
		double tMaxX = dx == 0 ? Double.POSITIVE_INFINITY : ((dx > 0 ? x + 1 - from.x : from.x - x) * tDeltaX);
		double tMaxY = dy == 0 ? Double.POSITIVE_INFINITY : ((dy > 0 ? y + 1 - from.y : from.y - y) * tDeltaY);
		double tMaxZ = dz == 0 ? Double.POSITIVE_INFINITY : ((dz > 0 ? z + 1 - from.z : from.z - z) * tDeltaZ);
		boolean wasDug = dug.isDug(x, y, z);
		CyberDig.Probe probe = null;
		for (int i = 0; i < 64; i++) {
			double t;
			double nx = 0, ny = 0, nz = 0;
			if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
				t = tMaxX;
				tMaxX += tDeltaX;
				x += stepX;
				nx = -stepX;
			} else if (tMaxY <= tMaxZ) {
				t = tMaxY;
				tMaxY += tDeltaY;
				y += stepY;
				ny = -stepY;
			} else {
				t = tMaxZ;
				tMaxZ += tDeltaZ;
				z += stepZ;
				nz = -stepZ;
			}
			if (t > limit) {
				return null;
			}
			boolean isDug = dug.isDug(x, y, z);
			if (wasDug && !isDug) {
				double px = from.x + dx * t, py = from.y + dy * t, pz = from.z + dz * t;
				if (probe == null) {
					probe = new CyberDig.Probe().around(Math.min(from.x, to.x), Math.min(from.y, to.y), Math.min(from.z, to.z), Math.max(from.x, to.x), Math.max(from.y, to.y),
						Math.max(from.z, to.z));
				}
				if (probe.test(px - nx * 0.02, py - ny * 0.02, pz - nz * 0.02) > CyberDig.AIR) {
					return new CyberRay.Hit(t, px, py, pz, nx, ny, nz, probe.surface != null ? probe.surface : STONE_WALL);
				}
			}
			wasDug = isDug;
		}
		return null;
	}

	/** Nearest hit on the city's collision along the segment (triangles if any, else voxels), or null. */
	public static CyberRay.Hit cast(Vec3 from, Vec3 to) {
		if (!CyberCollision.hasTriangles()) {
			return castVoxels(from, to);
		}
		List<CyberTri> tris = SCRATCH.get();
		tris.clear();
		CyberCollision.trianglesNear(new AABB(from, to).inflate(0.01), tris);
		if (tris.isEmpty()) {
			return null;
		}
		CyberRay.Hit hit = CyberRay.cast(tris, from.x, from.y, from.z, to.x, to.y, to.z);
		tris.clear();
		return hit;
	}

	// What a voxel hit stands in for: a plain, undiggable surface (the plugin's voxels carry no
	// material Minecraft could dig into).
	private static final CyberTri VOXEL_SURFACE = new CyberTri(new float[9], 0, 0);

	/**
	 * The plugin sends the city as voxels only, never triangles: the nearest voxel the segment
	 * enters, walking the cells it crosses, or null.
	 */
	static CyberRay.Hit castVoxels(Vec3 from, Vec3 to) {
		if (!CyberCollision.active()) {
			return null;
		}
		double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
		double length = Math.sqrt(dx * dx + dy * dy + dz * dz);
		if (length < 1e-9) {
			return null;
		}
		int x = (int) Math.floor(from.x), y = (int) Math.floor(from.y), z = (int) Math.floor(from.z);
		int stepX = dx > 0 ? 1 : -1, stepY = dy > 0 ? 1 : -1, stepZ = dz > 0 ? 1 : -1;
		double tDeltaX = dx == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dx);
		double tDeltaY = dy == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dy);
		double tDeltaZ = dz == 0 ? Double.POSITIVE_INFINITY : Math.abs(1.0 / dz);
		double tMaxX = dx == 0 ? Double.POSITIVE_INFINITY : ((dx > 0 ? x + 1 - from.x : from.x - x) * tDeltaX);
		double tMaxY = dy == 0 ? Double.POSITIVE_INFINITY : ((dy > 0 ? y + 1 - from.y : from.y - y) * tDeltaY);
		double tMaxZ = dz == 0 ? Double.POSITIVE_INFINITY : ((dz > 0 ? z + 1 - from.z : from.z - z) * tDeltaZ);
		int cells = (int) Math.ceil(length) * 3 + 3;
		for (int i = 0; i < cells; i++) {
			BlockPos pos = new BlockPos(x, y, z);
			net.minecraft.world.phys.shapes.VoxelShape shape = CyberCollision.shapeAt(pos);
			if (shape != null) {
				BlockHitResult r = shape.clip(from, to, pos);
				if (r != null && r.getType() != HitResult.Type.MISS) {
					Vec3 at = r.getLocation();
					Direction face = r.getDirection();
					double t = Math.sqrt(from.distanceToSqr(at)) / length;
					return new CyberRay.Hit(t, at.x, at.y, at.z, face.getStepX(), face.getStepY(), face.getStepZ(), VOXEL_SURFACE);
				}
			}
			double t;
			if (tMaxX <= tMaxY && tMaxX <= tMaxZ) {
				t = tMaxX;
				tMaxX += tDeltaX;
				x += stepX;
			} else if (tMaxY <= tMaxZ) {
				t = tMaxY;
				tMaxY += tDeltaY;
				y += stepY;
			} else {
				t = tMaxZ;
				tMaxZ += tDeltaZ;
				z += stepZ;
			}
			if (t > 1.0) {
				return null;
			}
		}
		return null;
	}

	/** A hit on the city's collision (not a Minecraft block). Keeps the exact surface normal and triangle. */
	public static final class CyberHitResult extends BlockHitResult {
		public final double nx, ny, nz;
		public final CyberRay.Hit hit;

		public CyberHitResult(Vec3 location, Direction direction, BlockPos pos, CyberRay.Hit hit) {
			super(location, direction, pos, false);
			this.nx = hit.nx();
			this.ny = hit.ny();
			this.nz = hit.nz();
			this.hit = hit;
		}
	}
}
