package dev.cybercraft.client;

import static dev.cybercraft.link.Proto.DIG_STONE;

import dev.cybercraft.client.render.WorldExporter;
import dev.cybercraft.net.CyberNet;
import dev.cybercraft.world.CyberClip;
import dev.cybercraft.world.CyberCollision;
import dev.cybercraft.world.CyberDig;
import dev.cybercraft.world.CyberRay;
import it.unimi.dsi.fastutil.longs.Long2IntLinkedOpenHashMap;
import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.util.ArrayList;
import java.util.List;
import net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking;
import net.minecraft.client.particle.TerrainParticle;
import net.minecraft.client.resources.sounds.SimpleSoundInstance;
import net.minecraft.client.resources.sounds.SoundInstance;
import net.minecraft.sounds.SoundSource;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import org.jspecify.annotations.Nullable;

/**
 * The digging player's side of {@link CyberDig}: hitting diggable geometry asks the server to open
 * that cell; a dug block breaking asks it to reveal what's around it (worked out from the collision
 * triangles, which only this client has); and the dug cells of loaded chunks go to the plugin
 * (WorldExporter).
 *
 * <p>Idle in Night City: only triangles can be diggable, and the plugin sends voxels alone (and
 * ignores dug cells). Cyberpunk's geometry can't be cut open on screen, so digging stays off.
 */
public final class CyberDigClient {
	private CyberDigClient() {
	}

	private static final int REVEAL_RANGE = 24;
	private static final int REVEAL_TRIES = 60; // ticks to wait for the collision around a cell

	// Mining a diggable surface: which cell, how far along (0..1), on which tick last.
	private static @Nullable BlockPos mining;
	private static float progress;
	private static int miningTick;
	private static int hits;
	private static int ticks;
	private static final Long2IntLinkedOpenHashMap REVEAL = new Long2IntLinkedOpenHashMap(); // cell -> tries left
	private static final Long2ObjectOpenHashMap<CyberDig.DugColumn> SEEN = new Long2ObjectOpenHashMap<>();
	private static final CyberDig.DugColumn NONE = new CyberDig.DugColumn(List.of());
	private static int seenWorld = Integer.MIN_VALUE;
	private static int pollTicks;

	/** The world dug cells are kept for (CyberState.worldId): Night City's constant "CP77". */
	public static int world() {
		return CyberClient.state().worldId;
	}

	/**
	 * The player attacked (clicked or held) diggable geometry under the crosshair: mining it, as long
	 * as mining the block it's made of would take. Returns true if it's being mined.
	 */
	public static boolean attack(Minecraft minecraft) {
		if (!(minecraft.hitResult instanceof CyberClip.CyberHitResult result) || minecraft.level == null || minecraft.player == null || minecraft.gameMode == null) {
			return false;
		}
		GameType mode = minecraft.gameMode.getPlayerMode();
		if (mode == GameType.ADVENTURE || mode == GameType.SPECTATOR || (!CyberDig.destruction && minecraft.hasSingleplayerServer())) {
			return false; // (a guest asks anyway: the host's setting decides)
		}
		CyberRay.Hit hit = result.hit;
		if (hit.tri() == null || !hit.tri().diggable) {
			return false; // something that stays: a building, or any voxel (all the plugin sends)
		}
		int[] cell = CyberRay.surfaceCell(hit);
		BlockPos pos = new BlockPos(cell[0], cell[1], cell[2]);
		BlockState here = minecraft.level.getBlockState(pos);
		if (!here.isAir() && !here.canBeReplaced()) {
			return false;
		}
		if (CyberDig.isDug(minecraft.level, world(), pos)) {
			return false;
		}
		// Once a tick (a click and a held button can both land in the same one).
		if (pos.equals(mining) && miningTick == ticks) {
			return true;
		}
		if (!pos.equals(mining) || ticks - miningTick > 2) {
			mining = pos;
			progress = 0.0F;
			hits = 0;
		}
		miningTick = ticks;
		int material = hit.tri().material == 0 ? DIG_STONE : hit.tri().material;
		BlockState state = CyberDig.materialState(material);
		progress += minecraft.player.getAbilities().instabuild ? 1.0F : state.getDestroyProgress(minecraft.player, minecraft.level, pos);
		if (hits++ % 4 == 0) {
			var sound = state.getSoundType();
			minecraft.getSoundManager().play(new SimpleSoundInstance(
				sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 8.0F, sound.getPitch() * 0.5F, SoundInstance.createUnseededRandom(), pos
			));
			minecraft.particleEngine.add(new TerrainParticle(minecraft.level, hit.x() + hit.nx() * 0.05, hit.y() + hit.ny() * 0.05, hit.z() + hit.nz() * 0.05, 0.0, 0.0, 0.0, state, pos)
				.setPower(0.2F).scale(0.6F));
		}
		if (progress >= 1.0F) {
			ClientPlayNetworking.send(new CyberNet.DigOpen(world(), pos, material));
			REVEAL.put(pos.asLong(), REVEAL_TRIES); // what's around it: blocks, or walls
			mining = null;
			progress = 0.0F;
		}
		return true;
	}

	/** Any block change on this client: a dug block that broke gets what's around it revealed. */
	public static void blockChanged(ClientLevel level, BlockPos pos, BlockState before, BlockState after) {
		if (before.isAir() || !(after.isAir() || after.canBeReplaced())) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		if (minecraft.player == null || minecraft.player.blockPosition().distSqr(pos) > REVEAL_RANGE * REVEAL_RANGE) {
			return;
		}
		if (CyberDig.isDug(level, world(), pos) && (CyberDig.destruction || !minecraft.hasSingleplayerServer())) {
			REVEAL.put(pos.asLong(), REVEAL_TRIES);
		}
	}

	public static void tick(Minecraft minecraft) {
		ticks++;
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null) {
			REVEAL.clear();
			SEEN.clear();
			CyberDig.clientDug = null;
			return;
		}
		int world = world();
		CyberDig.clientWorld = world;
		CyberDig.clientDug = (x, y, z) -> CyberDig.isDug(level, world, new BlockPos(x, y, z));
		reveal(level);
		if (++pollTicks % 5 == 0) {
			pollDug(minecraft, level);
		}
		// The collision around dug cells arrived or changed: their walls are drawn again.
		CyberCollision.takeChangedRegions(key -> {
			int rx = BlockPos.getX(key), ry = BlockPos.getY(key), rz = BlockPos.getZ(key);
			CyberDig.wallsChanged(rx, ry, rz, rx + 7, ry + 7, rz + 7);
			for (int sx = (rx - 1) >> 4; sx <= (rx + 8) >> 4; sx++) {
				for (int sz = (rz - 1) >> 4; sz <= (rz + 8) >> 4; sz++) {
					LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
					if (chunk == null) {
						continue;
					}
					for (int sy = (ry - 1) >> 4; sy <= (ry + 8) >> 4; sy++) {
						if (dugBits(chunk, sy) != null) {
							WorldExporter.markDirty(sx, sy, sz);
						}
					}
				}
			}
		});
	}

	private static void reveal(ClientLevel level) {
		if (REVEAL.isEmpty()) {
			return;
		}
		int world = world();
		List<BlockPos> cells = new ArrayList<>();
		List<Integer> materials = new ArrayList<>();
		LongOpenHashSet queued = new LongOpenHashSet();
		long[] keys = REVEAL.keySet().toLongArray();
		for (long key : keys) {
			BlockPos pos = BlockPos.of(key);
			boolean waiting = false;
			for (Direction d : Direction.values()) {
				BlockPos n = pos.relative(d);
				if (queued.contains(n.asLong()) || CyberDig.isDug(level, world, n)) {
					continue;
				}
				if (!CyberCollision.isKnown(n.getX(), n.getY(), n.getZ())) {
					waiting = true;
					continue;
				}
				int material = CyberDig.classify(n.getX(), n.getY(), n.getZ());
				if (material <= CyberDig.AIR) {
					continue;
				}
				BlockState state = level.getBlockState(n);
				if (!state.isAir() && !state.canBeReplaced()) {
					material = 0; // a Minecraft block is already there: just cut the geometry out
				}
				queued.add(n.asLong());
				cells.add(n);
				materials.add(material);
			}
			int tries = REVEAL.get(key) - 1;
			if (waiting && tries > 0) {
				REVEAL.put(key, tries); // the collision there hasn't arrived yet
			} else {
				REVEAL.remove(key);
			}
		}
		for (int i = 0; i < cells.size(); i += 64) {
			int end = Math.min(cells.size(), i + 64);
			ClientPlayNetworking.send(new CyberNet.DigReveal(world, List.copyOf(cells.subList(i, end)), List.copyOf(materials.subList(i, end))));
		}
	}

	/** Dug cells of this section in the current world, or null. */
	public static long @Nullable [] dugBits(LevelChunk chunk, int sectionY) {
		CyberDig.DugColumn column = chunk.getAttached(CyberDig.DUG);
		return column == null ? null : column.bits(world(), sectionY);
	}

	/** The plugin needs every dug cell again (WorldExporter cleared everything it holds). */
	public static void resendAll() {
		SEEN.clear();
	}

	// Chunks whose dug cells changed (or the world id did): their sections go to the plugin.
	private static void pollDug(Minecraft minecraft, ClientLevel level) {
		int world = world();
		boolean worldChanged = world != seenWorld;
		seenWorld = world;
		if (worldChanged) {
			CyberDig.wallsChanged();
		}
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = minecraft.player.getBlockX() >> 4, pcz = minecraft.player.getBlockZ() >> 4;
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				long key = ChunkPos.pack(cx, cz);
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					SEEN.remove(key);
					continue;
				}
				CyberDig.DugColumn column = chunk.getAttached(CyberDig.DUG);
				if (column == null) {
					column = NONE;
				}
				CyberDig.DugColumn previous = SEEN.get(key);
				if (previous == column && !worldChanged) {
					continue;
				}
				SEEN.put(key, column);
				for (CyberDig.DugColumn c : new CyberDig.DugColumn[] { column, previous == null ? NONE : previous }) {
					for (CyberDig.DugSection s : c.sections()) {
						if (worldChanged || s.world() == world) {
							// Its walls, and the walls next door (a dug cell can open one up).
							WorldExporter.markDirtyNow(cx, s.sectionY(), cz);
							WorldExporter.markDirty(cx - 1, s.sectionY(), cz);
							WorldExporter.markDirty(cx + 1, s.sectionY(), cz);
							WorldExporter.markDirty(cx, s.sectionY() - 1, cz);
							WorldExporter.markDirty(cx, s.sectionY() + 1, cz);
							WorldExporter.markDirty(cx, s.sectionY(), cz - 1);
							WorldExporter.markDirty(cx, s.sectionY(), cz + 1);
						}
					}
				}
			}
		}
		if (SEEN.size() > 8192) {
			SEEN.clear();
		}
	}
}
