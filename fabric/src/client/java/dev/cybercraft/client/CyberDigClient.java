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
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import org.jspecify.annotations.Nullable;

/**
 * The digging player's side of {@link CyberDig}: hitting the city's surface asks the server to open
 * that cell; a dug block breaking asks it to reveal what's around it (worked out from the city's
 * collision, which only this client has); and the dug cells of loaded chunks go to the plugin
 * (WorldExporter), which cuts the city out of its picture there, and to CyberDig's copy of them that
 * the city's collision is cut by.
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
	/**
	 * The pause after a block breaks before the next one starts while the button is held (0.6 s):
	 * vanilla's destroyDelay for Minecraft's blocks (MultiPlayerGameModeMixin), and the same counter for
	 * the city's, so after a block the city behind it waits too, and the other way round. Without it the
	 * button held on dug on into the city behind a block the moment the block broke.
	 */
	public static final int PAUSE_TICKS = 12;
	private static int pausedOnTick = -1; // the tick the pause last counted down on (attack can come twice in one)
	private static final Long2IntLinkedOpenHashMap REVEAL = new Long2IntLinkedOpenHashMap(); // cell -> tries left
	// Cells opened from a surface -> the side it was mined from (Direction ordinal): open air, never revealed.
	private static final it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap OPENED_FROM = new it.unimi.dsi.fastutil.longs.Long2IntOpenHashMap();
	private static final Long2ObjectOpenHashMap<CyberDig.DugColumn> SEEN = new Long2ObjectOpenHashMap<>();
	private static final CyberDig.DugColumn NONE = new CyberDig.DugColumn(List.of());
	private static int seenWorld = Integer.MIN_VALUE;
	private static int pollTicks;

	/** The world dug cells are kept for (CyberState.worldId): Night City's constant "CP77". */
	public static int world() {
		return CyberClient.state().worldId;
	}

	/** The cell of the city mining would dig out now (under the crosshair), or null. */
	public static @Nullable BlockPos target(Minecraft minecraft) {
		if (!(minecraft.hitResult instanceof CyberClip.CyberHitResult result) || minecraft.level == null || minecraft.player == null || minecraft.gameMode == null) {
			return null;
		}
		GameType mode = minecraft.gameMode.getPlayerMode();
		if (mode == GameType.ADVENTURE || mode == GameType.SPECTATOR || (!CyberDig.destruction && minecraft.hasSingleplayerServer())) {
			return null; // (a guest asks anyway: the host's setting decides)
		}
		CyberRay.Hit hit = result.hit;
		if (hit.tri() == null || !hit.tri().diggable) {
			return null; // something that stays
		}
		int[] cell = CyberRay.surfaceCell(hit);
		BlockPos pos = new BlockPos(cell[0], cell[1], cell[2]);
		BlockState here = minecraft.level.getBlockState(pos);
		if (!here.isAir() && !here.canBeReplaced()) {
			return null;
		}
		if (CyberDig.isDug(minecraft.level, world(), pos) || CyberDig.dugHere(pos.asLong())) {
			return null;
		}
		return pos;
	}

	/** How much of a city cell is there to dig (and to outline): up to a floor's top, or all of it. */
	public static float height(BlockPos pos) {
		return CyberCollision.rawGeometry(pos.getX(), pos.getY(), pos.getZ()) ? CyberDig.partialTop(pos.getX(), pos.getY(), pos.getZ()) : 1.0F;
	}

	/** The city cell being mined and how far along (Minecraft's crack stage, 0..9), or null. */
	public static @Nullable BlockPos cracking() {
		return mining != null && ticks - miningTick <= 2 && progress > 0.0F ? mining : null;
	}

	public static int crackStage() {
		return Math.min(9, Math.max(0, (int) (progress * 10.0F)));
	}

	/**
	 * The player attacked (clicked or held) diggable geometry under the crosshair: mining it, as long
	 * as mining the block it's made of would take. Returns true if it's being mined.
	 */
	public static boolean attack(Minecraft minecraft, boolean click) {
		BlockPos pos = target(minecraft);
		if (pos == null) {
			return false;
		}
		CyberRay.Hit hit = ((CyberClip.CyberHitResult) minecraft.hitResult).hit;
		// The pause after the last block (a click goes straight in, as on Minecraft's blocks), counted
		// down here while the city is under the crosshair: vanilla counts it down only at its own blocks.
		var pause = (dev.cybercraft.client.mixin.MultiPlayerGameModeAccessor) minecraft.gameMode;
		if (click) {
			pause.cybercraft$setDestroyDelay(0);
		} else if (pause.cybercraft$destroyDelay() > 0) {
			if (pausedOnTick != ticks) {
				pausedOnTick = ticks;
				pause.cybercraft$setDestroyDelay(pause.cybercraft$destroyDelay() - 1);
			}
			return true; // swung at, not mined
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
		// The city's own surface (asphalt, concrete, its walls) takes as long as deepslate to mine, and
		// sounds like it; it still breaks into what stone does.
		BlockState hard = material == DIG_STONE ? Blocks.DEEPSLATE.defaultBlockState() : state;
		progress += minecraft.player.getAbilities().instabuild ? 1.0F : hard.getDestroyProgress(minecraft.player, minecraft.level, pos);
		if (hits++ % 4 == 0) {
			var sound = hard.getSoundType();
			minecraft.getSoundManager().play(new SimpleSoundInstance(
				sound.getHitSound(), SoundSource.BLOCKS, (sound.getVolume() + 1.0F) / 8.0F, sound.getPitch() * 0.5F, SoundInstance.createUnseededRandom(), pos
			));
			minecraft.particleEngine.add(new TerrainParticle(minecraft.level, hit.x() + hit.nx() * 0.05, hit.y() + hit.ny() * 0.05, hit.z() + hit.nz() * 0.05, 0.0, 0.0, 0.0, state, pos)
				.setPower(0.2F).scale(0.6F));
		}
		if (progress >= 1.0F) {
			// Mined from above: the surface's height, for putting the hole back under it when the grid moves.
			float surface = hit.ny() > 0.5 ? (float) hit.y() : Float.NaN;
			ClientPlayNetworking.send(new CyberNet.DigOpen(world(), pos, material, surface));
			REVEAL.put(pos.asLong(), REVEAL_TRIES); // what's around it: blocks, or walls
			OPENED_FROM.put(pos.asLong(), Direction.getApproximateNearest(hit.nx(), hit.ny(), hit.nz()).ordinal());
			mining = null;
			progress = 0.0F;
			pause.cybercraft$setDestroyDelay(PAUSE_TICKS);
		}
		return true;
	}

	/**
	 * The middle button on the city's surface or a hole's wall: the block it shows (stone for the
	 * street and a hole's paved rim, dirt or stone for ground not revealed yet), picked as vanilla
	 * picks a block (from the inventory, or given in creative). Returns true if it was the city.
	 */
	public static boolean pick(Minecraft minecraft) {
		if (!(minecraft.hitResult instanceof CyberClip.CyberHitResult result) || minecraft.player == null) {
			return false;
		}
		int[] cell = CyberRay.surfaceCell(result.hit);
		int kind = CyberDig.classifyVoxel(cell[0], cell[1], cell[2]);
		ClientPlayNetworking.send(new CyberNet.PickCity(kind > CyberDig.AIR ? kind : DIG_STONE));
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
		// (Still dug here too: stone put back where the city was gives it back, and goes, CyberDig.placedBack.)
		if (CyberDig.isDug(level, world(), pos) && CyberDig.dugHere(pos.asLong()) && (CyberDig.destruction || !minecraft.hasSingleplayerServer())) {
			REVEAL.put(pos.asLong(), REVEAL_TRIES);
		}
	}

	public static void tick(Minecraft minecraft) {
		ticks++;
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null) {
			REVEAL.clear();
			OPENED_FROM.clear();
			if (!SEEN.isEmpty()) {
				SEEN.clear();
				CyberDig.hereCleared();
			}
			CyberDig.clientDug = null;
			CyberCollision.takeChangedRegions(key -> {
			});
			return;
		}
		int world = world();
		CyberDig.clientWorld = world;
		CyberDig.clientDug = (x, y, z) -> CyberDig.isDug(level, world, new BlockPos(x, y, z));
		reveal(level);
		if (++pollTicks % 5 == 0) {
			pollDug(minecraft, level);
		}
		// The collision around dug cells arrived or changed: their walls are drawn again (a cell's
		// walls look up to 20 blocks up for the surface over them, CyberDig.classifyVoxel).
		CyberCollision.takeChangedRegions(key -> {
			int rx = BlockPos.getX(key), ry = BlockPos.getY(key), rz = BlockPos.getZ(key);
			CyberDig.wallsChanged(rx, ry, rz, rx + 7, ry + 7, rz + 7);
			for (int sx = (rx - 1) >> 4; sx <= (rx + 8) >> 4; sx++) {
				for (int sz = (rz - 1) >> 4; sz <= (rz + 8) >> 4; sz++) {
					LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
					if (chunk == null) {
						continue;
					}
					for (int sy = (ry - 21) >> 4; sy <= (ry + 8) >> 4; sy++) {
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
			if (CyberDig.freeStanding(pos.getX(), pos.getY(), pos.getZ()) && knownAround(pos)) {
				// A thing that stood free (a chair, a bin): just gone, with no ground behind it to reveal.
				REVEAL.remove(key);
				OPENED_FROM.remove(key);
				continue;
			}
			boolean waiting = false;
			int from = OPENED_FROM.getOrDefault(key, -1);
			for (Direction d : Direction.values()) {
				if (d.ordinal() == from) {
					continue; // where the player mined it from: the open side
				}
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
				OPENED_FROM.remove(key);
			}
		}
		for (int i = 0; i < cells.size(); i += 64) {
			int end = Math.min(cells.size(), i + 64);
			ClientPlayNetworking.send(new CyberNet.DigReveal(world, List.copyOf(cells.subList(i, end)), List.copyOf(materials.subList(i, end))));
		}
	}

	private static boolean knownAround(BlockPos pos) {
		for (Direction d : Direction.values()) {
			if (!CyberCollision.isKnown(pos.getX() + d.getStepX(), pos.getY() + d.getStepY(), pos.getZ() + d.getStepZ())) {
				return false;
			}
		}
		return true;
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
			CyberDig.hereCleared();
			SEEN.clear();
		}
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = minecraft.player.getBlockX() >> 4, pcz = minecraft.player.getBlockZ() >> 4;
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				long key = ChunkPos.pack(cx, cz);
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					CyberDig.DugColumn gone = SEEN.remove(key);
					if (gone != null) {
						CyberDig.hereChanged(world, cx, cz, gone, null);
					}
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
				CyberDig.hereChanged(world, cx, cz, previous, column);
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
		// Chunks left behind out of range: their dug cells are no business of the collision here.
		if (SEEN.size() > 4096) {
			var it = SEEN.long2ObjectEntrySet().iterator();
			while (it.hasNext()) {
				var e = it.next();
				int cx = ChunkPos.getX(e.getLongKey()), cz = ChunkPos.getZ(e.getLongKey());
				if (Math.abs(cx - pcx) > radius || Math.abs(cz - pcz) > radius) {
					CyberDig.hereChanged(world, cx, cz, e.getValue(), null);
					it.remove();
				}
			}
		}
	}
}
