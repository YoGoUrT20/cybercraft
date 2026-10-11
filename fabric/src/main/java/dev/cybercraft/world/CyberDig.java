package dev.cybercraft.world;

import static dev.cybercraft.link.Proto.*;

import com.mojang.serialization.Codec;
import com.mojang.serialization.codecs.RecordCodecBuilder;
import dev.cybercraft.CyberCraft;
import io.netty.buffer.ByteBuf;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.stream.LongStream;
import net.fabricmc.fabric.api.attachment.v1.AttachmentRegistry;
import net.fabricmc.fabric.api.attachment.v1.AttachmentSyncPredicate;
import net.fabricmc.fabric.api.attachment.v1.AttachmentType;
import net.minecraft.core.BlockPos;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.item.DyeColor;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.LevelEvent;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.core.Direction;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import dev.cybercraft.link.CyberLink;
import org.jspecify.annotations.Nullable;

/**
 * Digging into Night City. Its ground and walls are the plugin's voxels (or exact triangles, which
 * the plugin doesn't send), not blocks; this turns them into blocks one at a time, as they're dug:
 * <ul>
 * <li><b>open</b>: mining the city's surface (as long as mining stone takes) takes the cell the
 * surface is in out: its collision is gone for Minecraft (CyberCollision leaves dug cells out), and
 * the plugin cuts the city out of its picture there (World.cpp, from Cyberpunk's depth), with the
 * drops of that block. Nothing sticks up out of the ground: the cell is empty.</li>
 * <li><b>reveal</b>: the cells around a mined one that are under the city's surface become blocks
 * (dirt for the first few, then stone with ores, bedrock deep down). Cells the surface runs through
 * keep it, and the part under it shows as Minecraft walls (the client's DigWalls), so a hole is
 * always closed.</li>
 * </ul>
 * Which cells are dug is kept per chunk (saved with the world, synced to every player) and per
 * world id (CyberState.worldId), and sent over the link (Proto.REN_DUG). Opening and revealing are
 * decided by the digging player's client, which knows the city around them; the server just
 * applies it.
 *
 * <p>Holes belong to Night City, not to Minecraft's grid: when Insert, the arrows or the scale move
 * the grid, the plugin says how (Proto.COL_GRID_MOVED) and the holes, with the blocks in them, move
 * back to where they were dug ({@link #gridMoved}).
 */
public final class CyberDig {
	private CyberDig() {
	}

	/** One section's dug cells for one world id: bit x + 16z + 256y. */
	public record DugSection(int world, int sectionY, long[] bits) {
		static final Codec<DugSection> CODEC = RecordCodecBuilder.create(i -> i.group(
			Codec.INT.fieldOf("world").forGetter(DugSection::world),
			Codec.INT.fieldOf("y").forGetter(DugSection::sectionY),
			Codec.LONG_STREAM.xmap(LongStream::toArray, Arrays::stream).fieldOf("bits").forGetter(DugSection::bits)
		).apply(i, DugSection::new));
	}

	/** A chunk's dug cells. Immutable: changes make a new one (so the attachment syncs). */
	public record DugColumn(List<DugSection> sections) {
		public static final DugColumn EMPTY = new DugColumn(List.of());
		static final Codec<DugColumn> CODEC = DugSection.CODEC.listOf().xmap(DugColumn::new, DugColumn::sections);
		static final StreamCodec<ByteBuf, DugColumn> STREAM_CODEC = new StreamCodec<>() {
			@Override
			public DugColumn decode(ByteBuf buf) {
				int n = ByteBufCodecs.VAR_INT.decode(buf);
				List<DugSection> sections = new ArrayList<>(n);
				for (int i = 0; i < n; i++) {
					int world = buf.readInt();
					int y = ByteBufCodecs.VAR_INT.decode(buf);
					long[] bits = new long[64];
					for (int k = 0; k < 64; k++) {
						bits[k] = buf.readLong();
					}
					sections.add(new DugSection(world, y, bits));
				}
				return new DugColumn(List.copyOf(sections));
			}

			@Override
			public void encode(ByteBuf buf, DugColumn column) {
				ByteBufCodecs.VAR_INT.encode(buf, column.sections.size());
				for (DugSection s : column.sections) {
					buf.writeInt(s.world);
					ByteBufCodecs.VAR_INT.encode(buf, s.sectionY);
					for (int k = 0; k < 64; k++) {
						buf.writeLong(s.bits.length == 64 ? s.bits[k] : 0L);
					}
				}
			}
		};

		public long @Nullable [] bits(int world, int sectionY) {
			for (DugSection s : this.sections) {
				if (s.world == world && s.sectionY == sectionY && s.bits.length == 64) {
					return s.bits;
				}
			}
			return null;
		}

		public boolean isDug(int world, int x, int y, int z) {
			long[] bits = bits(world, y >> 4);
			int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);
			return bits != null && ((bits[bit >> 6] >>> (bit & 63)) & 1L) != 0;
		}

		DugColumn with(int world, int x, int y, int z) {
			int sy = y >> 4;
			int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);
			List<DugSection> out = new ArrayList<>(this.sections.size() + 1);
			boolean found = false;
			for (DugSection s : this.sections) {
				if (s.world == world && s.sectionY == sy && s.bits.length == 64) {
					long[] bits = s.bits.clone();
					bits[bit >> 6] |= 1L << (bit & 63);
					out.add(new DugSection(world, sy, bits));
					found = true;
				} else {
					out.add(s);
				}
			}
			if (!found) {
				long[] bits = new long[64];
				bits[bit >> 6] |= 1L << (bit & 63);
				out.add(new DugSection(world, sy, bits));
			}
			return new DugColumn(List.copyOf(out));
		}

		/** The same with this cell no longer dug (for that world id). */
		DugColumn minus(int world, int x, int y, int z) {
			int sy = y >> 4;
			int bit = (x & 15) + 16 * (z & 15) + 256 * (y & 15);
			List<DugSection> out = new ArrayList<>(this.sections.size());
			for (DugSection s : this.sections) {
				if (s.world == world && s.sectionY == sy && s.bits.length == 64) {
					long[] bits = s.bits.clone();
					bits[bit >> 6] &= ~(1L << (bit & 63));
					if (Arrays.stream(bits).anyMatch(w -> w != 0)) {
						out.add(new DugSection(world, sy, bits));
					}
				} else {
					out.add(s);
				}
			}
			return new DugColumn(List.copyOf(out));
		}

		/** The same without that world's dug cells. */
		DugColumn without(int world) {
			List<DugSection> out = new ArrayList<>(this.sections.size());
			for (DugSection s : this.sections) {
				if (s.world != world) {
					out.add(s);
				}
			}
			return out.size() == this.sections.size() ? this : new DugColumn(List.copyOf(out));
		}

		/** The same without that world's dug cells {@code which} picks (BlockPos longs; this is chunk cx, cz). */
		DugColumn without(int world, int cx, int cz, java.util.function.LongPredicate which) {
			List<DugSection> out = new ArrayList<>(this.sections.size());
			boolean changed = false;
			for (DugSection s : this.sections) {
				if (s.world != world || s.bits.length != 64) {
					out.add(s);
					continue;
				}
				long[] bits = s.bits.clone();
				boolean any = false;
				for (int word = 0; word < 64; word++) {
					long left = bits[word];
					while (left != 0) {
						int bit = word * 64 + Long.numberOfTrailingZeros(left);
						left &= left - 1;
						if (which.test(BlockPos.asLong(cx * 16 + (bit & 15), s.sectionY * 16 + (bit >> 8), cz * 16 + ((bit >> 4) & 15)))) {
							bits[word] &= ~(1L << (bit & 63));
							changed = true;
						}
					}
					any |= bits[word] != 0;
				}
				if (any) {
					out.add(new DugSection(world, s.sectionY, bits));
				}
			}
			return changed ? new DugColumn(List.copyOf(out)) : this;
		}

		/** Every dug cell of that world in this chunk (cx, cz), as BlockPos longs. */
		public void forEach(int world, int cx, int cz, java.util.function.LongConsumer out) {
			for (DugSection s : this.sections) {
				if (s.world != world || s.bits.length != 64) {
					continue;
				}
				for (int word = 0; word < 64; word++) {
					long bits = s.bits[word];
					while (bits != 0) {
						int bit = word * 64 + Long.numberOfTrailingZeros(bits);
						bits &= bits - 1;
						out.accept(BlockPos.asLong(cx * 16 + (bit & 15), s.sectionY * 16 + (bit >> 8), cz * 16 + ((bit >> 4) & 15)));
					}
				}
			}
		}
	}

	public static final AttachmentType<DugColumn> DUG = AttachmentRegistry.<DugColumn>builder()
		.persistent(DugColumn.CODEC)
		.syncWith(DugColumn.STREAM_CODEC, AttachmentSyncPredicate.all())
		.buildAndRegister(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "dug"));

	/**
	 * Where a hole was opened from above, and the height of the city's surface it went through
	 * (Minecraft y): what puts the hole back under that surface when the grid moves up or down.
	 */
	public record Opening(int world, long pos, float surface) {
		static final Codec<Opening> CODEC = RecordCodecBuilder.create(i -> i.group(
			Codec.INT.fieldOf("world").forGetter(Opening::world),
			Codec.LONG.fieldOf("pos").forGetter(Opening::pos),
			Codec.FLOAT.fieldOf("surface").forGetter(Opening::surface)
		).apply(i, Opening::new));
	}

	/** A chunk's openings (server only: nothing but the grid moving needs them). */
	public static final AttachmentType<List<Opening>> OPENINGS = AttachmentRegistry.<List<Opening>>builder()
		.persistent(Opening.CODEC.listOf())
		.buildAndRegister(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "openings"));

	/** The chunks with dug cells in them (on the level), so a grid move finds every hole, loaded or not. */
	public static final AttachmentType<List<Long>> HOLE_CHUNKS = AttachmentRegistry.<List<Long>>builder()
		.persistent(Codec.LONG.listOf())
		.buildAndRegister(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "hole_chunks"));

	private static volatile net.minecraft.server.@Nullable MinecraftServer server;

	/** Loads the class (registers the attachments) at mod start. */
	public static void init() {
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents.SERVER_STARTED.register(s -> server = s);
		net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents.SERVER_STOPPED.register(s -> {
			server = null;
			HERE.clear();
		});
		// /clearnear <radius>: every block within that many blocks goes, and every hole there is
		// filled with the city again (its picture, walls and collision), as if never dug.
		net.fabricmc.fabric.api.command.v2.CommandRegistrationCallback.EVENT.register((dispatcher, context, selection) -> dispatcher.register(
			net.minecraft.commands.Commands.literal("clearnear")
				.requires(net.minecraft.commands.Commands.hasPermission(net.minecraft.commands.Commands.LEVEL_GAMEMASTERS))
				.then(net.minecraft.commands.Commands.argument("radius", com.mojang.brigadier.arguments.IntegerArgumentType.integer(1, CLEAR_MAX))
					.executes(c -> {
						var source = c.getSource();
						int radius = com.mojang.brigadier.arguments.IntegerArgumentType.getInteger(c, "radius");
						Cleared cleared = clearNear(source.getLevel(), source.getPosition(), radius);
						source.sendSuccess(() -> net.minecraft.network.chat.Component.literal(String.format(
							"Cleared %d blocks and gave %d dug cells back to Night City within %d blocks", cleared.blocks(), cleared.cells(), radius)), true);
						return cleared.blocks() + cleared.cells();
					}))));
		CyberCraft.LOG.info("CyberCraft: dug cells attachment registered ({})", DUG.identifier());
	}

	// ---- this machine's dug cells, for queries with no level at hand ----------------------------

	// Night City's dug cells around this machine's player (the current world id), as BlockPos longs:
	// mirrored from the client's chunks (CyberDigClient.pollDug) and added to at once by this machine's
	// own server. The city's collision here is this machine's too (CyberCollision), so the two match.
	private static final java.util.Set<Long> HERE = java.util.concurrent.ConcurrentHashMap.newKeySet();

	/** True if this cell was dug out of the city. Any thread. */
	public static boolean dugHere(long pos) {
		return !HERE.isEmpty() && HERE.contains(pos);
	}

	public static boolean dugHere(int x, int y, int z) {
		return dugHere(BlockPos.asLong(x, y, z));
	}

	/** The client's copy of a chunk's dug cells changed (null: none, or unloaded). */
	public static void hereChanged(int world, int cx, int cz, @Nullable DugColumn before, @Nullable DugColumn after) {
		if (before != null) {
			before.forEach(world, cx, cz, HERE::remove);
		}
		if (after != null) {
			after.forEach(world, cx, cz, HERE::add);
		}
	}

	/** The client left the world, or its world id changed. */
	public static void hereCleared() {
		HERE.clear();
	}

	public static DugColumn column(LevelChunk chunk) {
		DugColumn column = chunk.getAttached(DUG);
		return column != null ? column : DugColumn.EMPTY;
	}

	public static boolean isDug(Level level, int world, BlockPos pos) {
		return column(level.getChunkAt(pos)).isDug(world, pos.getX(), pos.getY(), pos.getZ());
	}

	// ---- server --------------------------------------------------------------------------------

	private static final double REACH = 8.0;

	private static boolean inReach(ServerPlayer player, BlockPos pos, double reach) {
		return player.getEyePosition().distanceToSqr(pos.getX() + 0.5, pos.getY() + 0.5, pos.getZ() + 0.5) <= reach * reach;
	}

	/**
	 * A player mined the geometry out of this cell (the cell the surface is in): it's gone,
	 * with the drops and the sound of mining what it was made of. {@code surface}: the height of the
	 * surface mined from above (NaN for a wall), kept for when the grid moves.
	 */
	public static void open(ServerPlayer player, int world, BlockPos pos, int material, float surface) {
		ServerLevel level = player.level();
		if (!destruction || !inReach(player, pos, REACH) || !level.isLoaded(pos) || player.isSpectator()) {
			return;
		}
		if (!markDug(level, world, pos)) {
			return;
		}
		if (Float.isFinite(surface) && surface > pos.getY() - 0.5F && surface < pos.getY() + 1.5F) {
			LevelChunk chunk = level.getChunkAt(pos);
			List<Opening> openings = new ArrayList<>(openings(chunk));
			openings.add(new Opening(world, pos.asLong(), surface));
			chunk.setAttached(OPENINGS, List.copyOf(openings));
		}
		BlockState state = materialState(material);
		if (!player.isCreative()) {
			ItemStack tool = player.getMainHandItem();
			ItemStack used = tool.copy();
			boolean harvest = player.hasCorrectToolForDrops(state);
			tool.mineBlock(level, state, pos, player);
			if (harvest) {
				state.getBlock().playerDestroy(level, player, pos, state, null, used);
			}
		}
		level.levelEvent(LevelEvent.PARTICLES_AND_SOUND_DESTROY_BLOCK, pos, Block.getId(state));
	}

	/** Cells around a mined one that are wholly inside the geometry: blocks now. */
	public static void reveal(ServerPlayer player, int world, List<BlockPos> cells, int[] materials) {
		if (!destruction) {
			return;
		}
		ServerLevel level = player.level();
		for (int i = 0; i < cells.size() && i < materials.length; i++) {
			BlockPos pos = cells.get(i);
			if (inReach(player, pos, REACH + 4.0) && level.isLoaded(pos)) {
				digCell(level, world, pos, materials[i]);
			}
		}
	}

	/**
	 * Whether Minecraft digs into the geometry at all: the pause menu's "City destruction" button
	 * (DestructionToggle), saved in config/cybercraft.properties, on unless it says otherwise. Off,
	 * mining surfaces and explosions leave it alone (and breaking blocks in old holes digs no further);
	 * holes already dug stay. In a friend's world it's the host's setting that counts (their server
	 * does the digging).
	 */
	public static volatile boolean destruction = true;

	private static List<Opening> openings(LevelChunk chunk) {
		List<Opening> openings = chunk.getAttached(OPENINGS);
		return openings != null ? openings : List.of();
	}

	/** Marks a cell dug out of the geometry (for that world id). Returns false if it was already. */
	public static boolean markDug(ServerLevel level, int world, BlockPos pos) {
		LevelChunk chunk = level.getChunkAt(pos);
		DugColumn column = column(chunk);
		if (column.isDug(world, pos.getX(), pos.getY(), pos.getZ())) {
			return false;
		}
		if (column.sections().isEmpty()) {
			addHoleChunk(level, chunk.getPos().pack());
		}
		chunk.setAttached(DUG, column.with(world, pos.getX(), pos.getY(), pos.getZ()));
		HERE.add(pos.asLong());
		return true;
	}

	private static void addHoleChunk(ServerLevel level, long chunk) {
		List<Long> chunks = level.getAttached(HOLE_CHUNKS);
		if (chunks == null || !chunks.contains(chunk)) {
			List<Long> out = chunks == null ? new ArrayList<>() : new ArrayList<>(chunks);
			out.add(chunk);
			level.setAttached(HOLE_CHUNKS, List.copyOf(out));
		}
	}

	/**
	 * A block just placed. Stone or cobblestone (what the city's surface is mined into) put back in a
	 * cell dug out of the city's surface gives the city back there: the block goes, the cell isn't dug
	 * any more, and Cyberpunk's own surface shows and holds again. Ground under the surface (no
	 * surface in the cell) just gets the block, as anywhere.
	 */
	public static void placedBack(ServerLevel level, BlockPos pos) {
		BlockState state = level.getBlockState(pos);
		if (!state.is(Blocks.STONE) && !state.is(Blocks.COBBLESTONE)) {
			return;
		}
		int x = pos.getX(), y = pos.getY(), z = pos.getZ();
		if (!CyberCollision.rawGeometry(x, y, z)) {
			return;
		}
		int world = worldFor(level);
		LevelChunk chunk = level.getChunkAt(pos);
		DugColumn column = column(chunk);
		if (!column.isDug(world, x, y, z)) {
			return;
		}
		// Out of the dug cells first (this machine's at once), so the block going isn't taken as a dug
		// block broken, which reveals what's round it.
		DugColumn left = column.minus(world, x, y, z);
		chunk.setAttached(DUG, left.sections().isEmpty() ? null : left);
		HERE.remove(pos.asLong());
		List<Opening> openings = openings(chunk);
		if (openings.stream().anyMatch(o -> o.pos() == pos.asLong())) {
			List<Opening> keep = openings.stream().filter(o -> o.pos() != pos.asLong()).toList();
			chunk.setAttached(OPENINGS, keep.isEmpty() ? null : keep);
		}
		level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_ALL);
		CyberCraft.LOG.info("CyberCraft: {} put back at {}: the city is whole there again", state.getBlock().getName().getString(), pos.toShortString());
	}

	/** The biggest radius /clearnear takes. */
	private static final int CLEAR_MAX = 64;

	/** What /clearnear did: Minecraft blocks removed, dug cells given back to the city. */
	public record Cleared(int blocks, int cells) {
	}

	/**
	 * /clearnear: every Minecraft block within {@code radius} of {@code centre} goes (nothing drops or
	 * spills), and every cell dug there is given back to the city, as {@link #placedBack} does for one:
	 * Cyberpunk's own surface shows and holds again, and the Minecraft walls round it go. Anyone
	 * standing in a hole that's gone is put on the city's surface over it. Server thread.
	 */
	public static Cleared clearNear(ServerLevel level, net.minecraft.world.phys.Vec3 centre, int radius) {
		int world = worldFor(level);
		double r2 = (double) radius * radius;
		java.util.function.LongPredicate inside = p -> {
			double dx = BlockPos.getX(p) + 0.5 - centre.x, dy = BlockPos.getY(p) + 0.5 - centre.y, dz = BlockPos.getZ(p) + 0.5 - centre.z;
			return dx * dx + dy * dy + dz * dz <= r2;
		};
		int minX = net.minecraft.util.Mth.floor(centre.x) - radius, maxX = net.minecraft.util.Mth.floor(centre.x) + radius;
		int minZ = net.minecraft.util.Mth.floor(centre.z) - radius, maxZ = net.minecraft.util.Mth.floor(centre.z) + radius;
		int minY = Math.max(level.getMinY(), net.minecraft.util.Mth.floor(centre.y) - radius);
		int maxY = Math.min(level.getMaxY(), net.minecraft.util.Mth.floor(centre.y) + radius);

		// The holes first (this machine's at once), so the blocks going aren't taken as dug blocks
		// broken, which reveals what's round them.
		it.unimi.dsi.fastutil.longs.LongOpenHashSet given = new it.unimi.dsi.fastutil.longs.LongOpenHashSet();
		List<Long> emptied = new ArrayList<>();
		for (int cx = minX >> 4; cx <= maxX >> 4; cx++) {
			for (int cz = minZ >> 4; cz <= maxZ >> 4; cz++) {
				LevelChunk chunk = level.getChunkSource().getChunkNow(cx, cz);
				if (chunk == null) {
					continue;
				}
				DugColumn column = column(chunk);
				DugColumn left = column.without(world, cx, cz, p -> inside.test(p) && given.add(p));
				if (left == column) {
					continue;
				}
				chunk.setAttached(DUG, left.sections().isEmpty() ? null : left);
				if (left.sections().isEmpty()) {
					emptied.add(chunk.getPos().pack());
				}
				List<Opening> openings = openings(chunk);
				List<Opening> keep = openings.stream().filter(o -> o.world() != world || !inside.test(o.pos())).toList();
				if (keep.size() != openings.size()) {
					chunk.setAttached(OPENINGS, keep.isEmpty() ? null : keep);
				}
			}
		}
		given.forEach((java.util.function.LongConsumer) HERE::remove);
		List<Long> chunks = level.getAttached(HOLE_CHUNKS);
		if (chunks != null && !emptied.isEmpty()) {
			List<Long> registry = new ArrayList<>(chunks);
			registry.removeAll(emptied);
			level.setAttached(HOLE_CHUNKS, registry.isEmpty() ? null : List.copyOf(registry));
		}

		// Then the blocks, with nothing round them reacting until they're all gone.
		it.unimi.dsi.fastutil.longs.LongArrayList cleared = new it.unimi.dsi.fastutil.longs.LongArrayList();
		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		for (int x = minX; x <= maxX; x++) {
			for (int z = minZ; z <= maxZ; z++) {
				LevelChunk chunk = level.getChunkSource().getChunkNow(x >> 4, z >> 4);
				if (chunk == null) {
					continue;
				}
				for (int y = minY; y <= maxY; y++) {
					long p = BlockPos.asLong(x, y, z);
					if (!inside.test(p) || chunk.getBlockState(pos.set(x, y, z)).isAir()) {
						continue;
					}
					level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_SKIP_ALL_SIDEEFFECTS);
					cleared.add(p);
				}
			}
		}
		// What hung off them from outside (a torch on a wall left standing) goes too, without dropping.
		BlockState air = Blocks.AIR.defaultBlockState();
		for (int i = 0; i < cleared.size(); i++) {
			air.updateNeighbourShapes(level, pos.set(cleared.getLong(i)), Block.UPDATE_CLIENTS | Block.UPDATE_SUPPRESS_DROPS);
		}

		// Anyone in a hole that's gone stands on the surface over it again, not in the void under it.
		for (ServerPlayer player : level.players()) {
			BlockPos feet = player.blockPosition();
			if (!given.contains(feet.asLong()) && !given.contains(feet.above().asLong())) {
				continue;
			}
			int x = feet.getX(), z = feet.getZ();
			for (int y = feet.getY(); y <= feet.getY() + COVER_REACH; y++) {
				if (CyberCollision.rawGeometry(x, y, z)) {
					while (CyberCollision.rawGeometry(x, y + 1, z)) {
						y++; // up a wall's sheet to its top
					}
					player.teleportTo(player.getX(), y + partialTop(x, y, z), player.getZ());
					player.resetFallDistance();
					break;
				}
			}
		}
		CyberCraft.LOG.info("CyberCraft: /clearnear {} at {}: {} blocks cleared, {} dug cells given back to the city", radius,
			BlockPos.containing(centre).toShortString(), cleared.size(), given.size());
		return new Cleared(cleared.size(), given.size());
	}

	/** A cell wholly inside the geometry: dug out of it, and the block it's made of put there. */
	public static void digCell(ServerLevel level, int world, BlockPos pos, int material) {
		// Never round a player: a cell they stand in is open air, whatever the city's shape says.
		if (material > 0 && !level.getEntitiesOfClass(net.minecraft.world.entity.player.Player.class, new AABB(pos)).isEmpty()) {
			return;
		}
		if (!markDug(level, world, pos)) {
			return;
		}
		BlockState here = level.getBlockState(pos);
		// Keep whatever Minecraft block is already there (someone built into the ground).
		if (material > 0 && (here.isAir() || here.canBeReplaced())) {
			level.setBlock(pos, blockFor(level, pos, material), 3);
		}
	}

	/** The block a Proto.DIG_* material stands for (stone without ores). */
	public static BlockState materialState(int material) {
		return switch (material) {
			case DIG_GRASS -> Blocks.GRASS_BLOCK.defaultBlockState();
			case DIG_DIRT -> Blocks.DIRT.defaultBlockState();
			case DIG_COBBLE -> Blocks.COBBLESTONE.defaultBlockState();
			case DIG_SNOW -> Blocks.SNOW_BLOCK.defaultBlockState();
			case DIG_ICE -> Blocks.PACKED_ICE.defaultBlockState();
			case DIG_SAND -> Blocks.SAND.defaultBlockState();
			case DIG_GRAVEL -> Blocks.GRAVEL.defaultBlockState();
			case DIG_MUD -> Blocks.MUD.defaultBlockState();
			case DIG_OAK_LOG -> Blocks.OAK_LOG.defaultBlockState();
			case DIG_SPRUCE_LOG -> Blocks.SPRUCE_LOG.defaultBlockState();
			case DIG_BIRCH_LOG -> Blocks.BIRCH_LOG.defaultBlockState();
			case DIG_PLANKS -> Blocks.SPRUCE_PLANKS.defaultBlockState();
			case DIG_METAL -> Blocks.COPPER_BLOCK.waxed().unaffected().defaultBlockState();
			case DIG_GLASS -> Blocks.GLASS.defaultBlockState();
			case DIG_ORGANIC -> Blocks.MOSS_BLOCK.defaultBlockState();
			case DIG_CLOTH -> Blocks.WOOL.pick(DyeColor.BROWN).defaultBlockState();
			case DIG_BONE -> Blocks.BONE_BLOCK.defaultBlockState();
			case DIG_WEB -> Blocks.COBWEB.defaultBlockState();
			case DIG_ASH -> Blocks.CONCRETE_POWDER.pick(DyeColor.LIGHT_GRAY).defaultBlockState();
			case DIG_BEDROCK -> Blocks.BEDROCK.defaultBlockState();
			default -> Blocks.STONE.defaultBlockState();
		};
	}

	/** The block a Proto.DIG_* material becomes here. Stone has ores in it. */
	public static BlockState blockFor(ServerLevel level, BlockPos pos, int material) {
		return material == DIG_STONE || material == DIG_NONE ? stoneOrOre(level.getSeed(), pos) : materialState(material);
	}

	/** Ores come in little clusters (a 2x2x2 cell picks one ore, then each block in it maybe). */
	static BlockState stoneOrOre(long seed, BlockPos pos) {
		long cell = mix(seed ^ mix(((long) (pos.getX() >> 1) * 73856093L) ^ ((long) (pos.getY() >> 1) * 19349663L) ^ ((long) (pos.getZ() >> 1) * 83492791L)));
		long block = mix(cell ^ ((pos.getX() & 1) | (pos.getY() & 1) << 1 | (pos.getZ() & 1) << 2));
		if ((block & 0xFF) >= 150) { // about 60% of a vein cell's blocks
			return Blocks.STONE.defaultBlockState();
		}
		int roll = (int) ((cell >>> 16) % 10000L);
		// Chances per vein cell, in 1/10000.
		if ((roll -= 330) < 0) {
			return Blocks.COAL_ORE.defaultBlockState();
		}
		if ((roll -= 220) < 0) {
			return Blocks.IRON_ORE.defaultBlockState();
		}
		if ((roll -= 170) < 0) {
			return Blocks.COPPER_ORE.defaultBlockState();
		}
		if ((roll -= 80) < 0) {
			return Blocks.REDSTONE_ORE.defaultBlockState();
		}
		if ((roll -= 45) < 0) {
			return Blocks.GOLD_ORE.defaultBlockState();
		}
		if ((roll -= 35) < 0) {
			return Blocks.LAPIS_ORE.defaultBlockState();
		}
		if ((roll -= 14) < 0) {
			return Blocks.DIAMOND_ORE.defaultBlockState();
		}
		if ((roll -= 5) < 0) {
			return Blocks.EMERALD_ORE.defaultBlockState();
		}
		return Blocks.STONE.defaultBlockState();
	}

	private static long mix(long z) {
		z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
		z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
		return z ^ (z >>> 31);
	}

	// ---- client: what's inside the geometry ---------------------------------------------------

	/** Leave the cell alone: geometry that can't be dug (a building) is there. */
	public static final int KEEP = -1;
	/** Open air, or only partly inside the geometry (its surface stays; walls are drawn). */
	public static final int AIR = 0;
	/** Blocks this far under the land's surface are bedrock. */
	public static final double BEDROCK_DEPTH = 5.0;
	/** A cell becomes a block only when this many of its 27 sample points are inside. */
	private static final int WHOLE = 25;

	private static final double SEARCH = 2.5;
	private static final double LAND_REACH = 16.0; // how far above a point the land is looked for
	private static final double[] SAMPLE = { 1.0 / 6.0, 0.5, 5.0 / 6.0 };

	/** Is a point inside the geometry? Its nearest original surface (within SEARCH) decides. */
	public static final class Probe {
		/** The land (terrain) around: a height field, so "below it" is "inside". */
		public final List<CyberTri> land = new ArrayList<>();
		/** Everything else (rocks, cliffs, cave walls, trees, buildings). */
		public final List<CyberTri> tris = new ArrayList<>();
		private final List<CyberTri> gathered = new ArrayList<>();
		private final double[] q = new double[3];
		/** After {@link #test}: the surface deciding it (null: none near, deep inside). */
		public @Nullable CyberTri surface;
		/** After {@link #test}: how far behind that surface (infinite if none near). */
		public double depth;
		private double boxMinX, boxMinY, boxMinZ, boxMaxX, boxMaxY, boxMaxZ;
		// Far above and below the box (gathered only if a point is far from every surface), and
		// that by block column.
		private @Nullable List<CyberTri> column;
		private final it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<List<CyberTri>> columns = new it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<>();
		// Per block: the surfaces that could be within SEARCH of a point in it. A blast's probe holds
		// thousands, nearly all far from any one point; kept in this.tris's order (ties go the same way).
		private final it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<List<CyberTri>> nearCells = new it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<>();

		/** Gathers the surfaces around a box (MC coords) once, for many tests inside it. */
		public Probe around(double minX, double minY, double minZ, double maxX, double maxY, double maxZ) {
			this.gathered.clear();
			this.land.clear();
			this.tris.clear();
			this.column = null;
			this.columns.clear();
			this.nearCells.clear();
			this.boxMinX = minX;
			this.boxMinY = minY;
			this.boxMinZ = minZ;
			this.boxMaxX = maxX;
			this.boxMaxY = maxY;
			this.boxMaxZ = maxZ;
			// The land well above too: a point deep under it is still under it.
			CyberCollision.originalSurfacesNear(new AABB(minX - SEARCH, minY - SEARCH, minZ - SEARCH, maxX + SEARCH, maxY + SEARCH + LAND_REACH, maxZ + SEARCH), this.gathered);
			for (CyberTri t : this.gathered) {
				if (t.stairHelper) {
					continue;
				}
				if (t.terrain) {
					this.land.add(t);
				} else if (t.maxY >= minY - SEARCH && t.minY <= maxY + SEARCH) {
					this.tris.add(t);
				}
			}
			return this;
		}

		/** The land's height at (x, z), or NaN where there's none near. */
		public double landHeight(double x, double z) {
			double best = Double.NaN;
			for (CyberTri t : this.land) {
				double h = t.heightAt(x, z);
				if (!Double.isNaN(h) && (Double.isNaN(best) || h > best)) {
					best = h;
					this.surface = t;
				}
			}
			return best;
		}

		/** KEEP (inside something that stays), AIR (in front of a surface) or a material (inside). */
		public int test(double px, double py, double pz) {
			// Under the land: inside, whatever else is buried nearby (rocks, roots).
			double ground = landHeight(px, pz);
			CyberTri landSurface = this.surface;
			if (!Double.isNaN(ground) && py < ground) {
				this.surface = landSurface;
				this.depth = ground - py;
				return landSurface.material == DIG_NONE ? DIG_STONE : landSurface.material;
			}
			// Above the land (or where there's none, as in interiors): inside an object if behind its
			// nearest surface (surfaces face out of their solid side).
			CyberTri nearest = null;
			double best = SEARCH * SEARCH;
			double bx = 0, by = 0, bz = 0;
			for (CyberTri t : nearCell(px, py, pz)) {
				// Its box already farther than the best so far: it can't be nearer.
				double ex = Math.max(0.0, Math.max(t.minX - px, px - t.maxX));
				double ey = Math.max(0.0, Math.max(t.minY - py, py - t.maxY));
				double ez = Math.max(0.0, Math.max(t.minZ - pz, pz - t.maxZ));
				if (ex * ex + ey * ey + ez * ez >= best) {
					continue;
				}
				closestPoint(t, px, py, pz, this.q);
				double dx = px - this.q[0], dy = py - this.q[1], dz = pz - this.q[2];
				double d2 = dx * dx + dy * dy + dz * dz;
				if (d2 < best) {
					best = d2;
					nearest = t;
					bx = this.q[0];
					by = this.q[1];
					bz = this.q[2];
				}
			}
			this.surface = nearest;
			if (nearest == null) {
				this.depth = Double.POSITIVE_INFINITY;
				return farFromSurfaces(px, py, pz, ground);
			}
			if ((px - bx) * nearest.nx + (py - by) * nearest.ny + (pz - bz) * nearest.nz >= 0.0) {
				return AIR;
			}
			if (!nearest.diggable) {
				return KEEP;
			}
			this.depth = Math.sqrt(best);
			return nearest.material == DIG_NONE ? DIG_STONE : nearest.material;
		}

		/**
		 * A point with no surface near (open sky, the middle of a big room, deep in rock): straight
		 * up decides. Under the top of something (a surface facing up, away from it) it's inside
		 * that; under an underside (an overhang, a ceiling) or nothing at all it's in the open, as
		 * long as there's land under it. No land anywhere (interiors): deep in the rock.
		 */
		private int farFromSurfaces(double px, double py, double pz, double ground) {
			boolean landBelow = !Double.isNaN(ground);
			CyberTri above = null;
			double aboveY = Double.POSITIVE_INFINITY;
			for (CyberTri t : columnAt(px, pz)) {
				double h = t.heightAt(px, pz);
				if (Double.isNaN(h)) {
					continue;
				}
				if (h > py) {
					if (h < aboveY) {
						aboveY = h;
						above = t;
					}
				} else if (t.terrain) {
					landBelow = true;
				}
			}
			if (above != null && above.ny > 0.0) {
				this.surface = above;
				this.depth = aboveY - py;
				if (!above.diggable) {
					return KEEP;
				}
				return above.material == DIG_NONE ? DIG_STONE : above.material;
			}
			return above != null || landBelow ? AIR : DIG_STONE;
		}

		private List<CyberTri> nearCell(double px, double py, double pz) {
			int cx = (int) Math.floor(px), cy = (int) Math.floor(py), cz = (int) Math.floor(pz);
			long key = net.minecraft.core.BlockPos.asLong(cx, cy, cz);
			List<CyberTri> list = this.nearCells.get(key);
			if (list == null) {
				list = new ArrayList<>();
				for (CyberTri t : this.tris) {
					if (t.maxX >= cx - SEARCH && t.minX <= cx + 1 + SEARCH && t.maxY >= cy - SEARCH && t.minY <= cy + 1 + SEARCH && t.maxZ >= cz - SEARCH
						&& t.minZ <= cz + 1 + SEARCH) {
						list.add(t);
					}
				}
				this.nearCells.put(key, list);
			}
			return list;
		}

		/** The surfaces over or under the block column holding (x, z). */
		private List<CyberTri> columnAt(double x, double z) {
			if (this.column == null) {
				List<CyberTri> all = new ArrayList<>();
				CyberCollision.originalSurfacesNear(new AABB(Math.min(x, this.boxMinX), this.boxMinY - COLUMN_REACH, Math.min(z, this.boxMinZ),
					Math.max(x, this.boxMaxX), this.boxMaxY + COLUMN_REACH, Math.max(z, this.boxMaxZ)), all);
				this.column = new ArrayList<>();
				for (CyberTri t : all) {
					if (!t.stairHelper && Math.abs(t.ny) >= 0.05) {
						this.column.add(t);
					}
				}
			}
			int bx = (int) Math.floor(x), bz = (int) Math.floor(z);
			long key = ((long) bx << 32) | (bz & 0xFFFFFFFFL);
			List<CyberTri> list = this.columns.get(key);
			if (list == null) {
				list = new ArrayList<>();
				for (CyberTri t : this.column) {
					if (t.maxX >= bx && t.minX <= bx + 1 && t.maxZ >= bz && t.minZ <= bz + 1) {
						list.add(t);
					}
				}
				this.columns.put(key, list);
			}
			return list;
		}
	}

	private static final double COLUMN_REACH = 96.0; // how far up and down a lone point looks

	/**
	 * Is this cell wholly inside the geometry, and if so what block is it (a Proto.DIG_*
	 * material)? Cells only partly inside are left to the geometry (AIR): its surface stays, and the
	 * part under it shows as Minecraft walls wherever it's been dug next to (see the client's DigWalls).
	 * Only for cells next to a dug one: far from every surface means deep inside there.
	 */
	public static int classify(int x, int y, int z) {
		if (!CyberCollision.hasTriangles()) {
			return classifyVoxel(x, y, z);
		}
		Probe probe = new Probe().around(x, y, z, x + 1, y + 1, z + 1);
		int inside = 0;
		int centreMaterial = DIG_STONE;
		double centreDepth = Double.POSITIVE_INFINITY;
		for (double sy : SAMPLE) {
			for (double sz : SAMPLE) {
				for (double sx : SAMPLE) {
					int r = probe.test(x + sx, y + sy, z + sz);
					if (r == KEEP) {
						return KEEP;
					}
					if (r == AIR) {
						continue;
					}
					inside++;
					if (sx == 0.5 && sy == 0.5 && sz == 0.5) {
						centreMaterial = r;
						centreDepth = probe.depth;
					}
				}
			}
		}
		if (inside < WHOLE) {
			return AIR;
		}
		// Under the land: dirt, stone, then bedrock a few blocks down.
		CyberTri land = landAbove(x + 0.5, y + 0.5, z + 0.5);
		if (land != null) {
			double depth = land.heightAt(x + 0.5, z + 0.5) - (y + 0.5);
			if (depth >= BEDROCK_DEPTH) {
				return DIG_BEDROCK;
			}
			if (Double.isInfinite(centreDepth)) {
				centreMaterial = land.material == DIG_NONE ? DIG_STONE : land.material;
				centreDepth = depth;
			}
		}
		return underground(centreMaterial, centreDepth);
	}

	// ---- the same from the plugin's voxels (all Cyberpunk sends) ---------------------------------

	/** A cell the city's surface runs through: it stays the city's, and the part under it is solid. */
	public static final int PARTIAL = -2;
	/** How far up from a cell the city's surface over it is looked for: more than CITY_BEDROCK. */
	private static final int COVER_REACH = CityVoxels.COVER_REACH;

	/**
	 * What a cell next to a dug one is, from the plugin's voxels, which only show surfaces: PARTIAL
	 * where the city's surface runs through it; under a surface (within COVER_REACH) a Proto.DIG_*
	 * material by how deep (dirt, then stone, then bedrock); AIR where there's room to stand on a
	 * floor right under it, or nothing over it. Whatever is behind a surface is solid ground: Night
	 * City's rooms under its floors can't be told from earth under its streets.
	 */
	public static int classifyVoxel(int x, int y, int z) {
		return CityVoxels.classify(x, y, z);
	}

	/**
	 * True if nothing of the city lies within six blocks under this cell: ground under a street rather
	 * than a room over a floor, as far as the voxels can tell (explosions, CyberDigBlast).
	 */
	public static boolean openBelow(int x, int y, int z) {
		for (int k = 1; k <= 6; k++) {
			if (CyberCollision.rawGeometry(x, y - k, z)) {
				return false;
			}
		}
		return true;
	}

	/**
	 * How high (0..1) the solid part of a PARTIAL cell reaches: up to a floor's top (what's under a
	 * pavement is ground), all of it for a wall's sheet (what's behind it can't be told from voxels).
	 */
	public static float partialTop(int x, int y, int z) {
		return CyberCollision.rawFloor(x, y, z) ? CyberCollision.rawTop(x, y, z) : 1.0F;
	}

	/**
	 * True if what was in this cell stands free in the open: a chair, a bench, a bin, a low wall, a
	 * parapet's top (CityVoxels.freeStanding: on a floor, at most two cells tall, open air beside it on
	 * two sides or more, cells dug out counting as open). Dug out, it's just gone: there's no ground
	 * behind it, so no walls, floor or blocks come with it (DigWalls, wallShape, CyberDigClient.reveal),
	 * where the voxels (sheets all round a chair's sides) read as walls and lined it with stone hanging
	 * in the air. From the plugin's voxels; any thread.
	 */
	public static boolean freeStanding(int x, int y, int z) {
		return !CyberCollision.hasTriangles() && CityVoxels.freeStanding(x, y, z, CyberDig::dugHere);
	}

	/** The nearest stretch of land (terrain triangles) above a point, within 32 blocks, or null. */
	public static @Nullable CyberTri landAbove(double x, double y, double z) {
		List<CyberTri> column = new ArrayList<>();
		CyberCollision.originalSurfacesNear(new AABB(x - 0.01, y, z - 0.01, x + 0.01, y + 32, z + 0.01), column);
		CyberTri best = null;
		double bestHeight = Double.POSITIVE_INFINITY;
		for (CyberTri t : column) {
			if (!t.terrain) {
				continue;
			}
			double h = t.heightAt(x, z);
			if (!Double.isNaN(h) && h >= y && h < bestHeight) {
				bestHeight = h;
				best = t;
			}
		}
		return best;
	}

	/** What's this deep behind a surface of this material: grass has dirt under it, then stone. */
	public static int underground(int surface, double depth) {
		return switch (surface) {
			case DIG_GRASS, DIG_DIRT -> depth < 3.5 ? DIG_DIRT : DIG_STONE;
			case DIG_MUD -> depth < 3.5 ? DIG_MUD : DIG_STONE;
			case DIG_SNOW -> depth < 1.5 ? DIG_SNOW : depth < 3.5 ? DIG_DIRT : DIG_STONE;
			case DIG_SAND -> depth < 3.5 ? DIG_SAND : DIG_STONE;
			case DIG_GRAVEL -> depth < 2.5 ? DIG_GRAVEL : DIG_STONE;
			case DIG_ASH -> depth < 3.5 ? DIG_ASH : DIG_STONE;
			case DIG_ICE -> depth < 2.5 ? DIG_ICE : DIG_STONE;
			case DIG_COBBLE -> depth < 1.5 ? DIG_COBBLE : DIG_STONE;
			case DIG_OAK_LOG, DIG_SPRUCE_LOG, DIG_BIRCH_LOG -> surface;
			case DIG_PLANKS -> depth < 2.5 ? DIG_PLANKS : DIG_STONE;
			case DIG_METAL -> depth < 1.5 ? DIG_METAL : DIG_STONE;
			case DIG_GLASS, DIG_ORGANIC, DIG_CLOTH, DIG_BONE, DIG_WEB -> depth < 1.0 ? surface : DIG_STONE;
			default -> DIG_STONE;
		};
	}

	// ---- collision for the walls of dug holes -------------------------------------------------

	private static final CyberLink.CyberState SERVER_STATE = new CyberLink.CyberState();
	private static volatile int serverWorld;
	private static volatile long serverWorldAt;
	// Worked-out walls by section (they depend only on the original surfaces, not on what's dug), shared
	// by every thread. NO_WALL stands for "none" (a map can't hold null).
	private static final java.util.concurrent.ConcurrentHashMap<Long, java.util.concurrent.ConcurrentHashMap<Long, VoxelShape>> WALLS =
		new java.util.concurrent.ConcurrentHashMap<>();
	private static final VoxelShape NO_WALL = Shapes.empty();

	/** The world id changed: every wall is worked out again. */
	public static void wallsChanged() {
		WALLS.clear();
		CityVoxels.forget();
	}

	/** The triangles in this box changed: the walls that could see them are worked out again. */
	public static void wallsChanged(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
		int reach = (int) Math.ceil(SEARCH) + 1;
		int down = reach + Math.max((int) Math.ceil(LAND_REACH), COVER_REACH); // a cell looks this far up for the ground over it
		for (int sx = (minX - reach) >> 4; sx <= (maxX + reach) >> 4; sx++) {
			for (int sy = (minY - down) >> 4; sy <= (maxY + reach) >> 4; sy++) {
				for (int sz = (minZ - reach) >> 4; sz <= (maxZ + reach) >> 4; sz++) {
					WALLS.remove(net.minecraft.core.SectionPos.asLong(sx, sy, sz));
					CityVoxels.forget(net.minecraft.core.SectionPos.asLong(sx, sy, sz));
				}
			}
		}
	}

	private static int worldFor(Level level) {
		if (level.isClientSide()) {
			DugLookup lookup = clientDug;
			return lookup != null ? clientWorld : 0;
		}
		long now = System.currentTimeMillis();
		if (now - serverWorldAt > 250) {
			synchronized (SERVER_STATE) {
				if (CyberLink.readCyberState(SERVER_STATE)) {
					serverWorld = SERVER_STATE.worldId;
				}
			}
			serverWorldAt = now;
		}
		return serverWorld;
	}

	/** Set by the client with clientDug: the world id its dug cells are for. */
	public static volatile int clientWorld;

	private static @Nullable DugColumn columnFor(net.minecraft.world.level.CollisionGetter level, int x, int z) {
		var chunk = level.getChunkForCollisions(x >> 4, z >> 4);
		return chunk instanceof LevelChunk lc ? lc.getAttached(DUG) : null;
	}

	/**
	 * The solid part of an undug cell next to a dug one: under the land's surface (the wall of a
	 * hole, drawn by the client's DigWalls), which the geometry itself (just a surface) doesn't
	 * make solid. Null for none. Any thread.
	 */
	public static @Nullable VoxelShape wallShape(net.minecraft.world.level.CollisionGetter getter, BlockPos pos) {
		// This machine's dug cells (HERE), like the city's collision they're cut out of.
		if (HERE.isEmpty() || !(getter instanceof Level) || !CyberCollision.active()) {
			return null;
		}
		int x = pos.getX(), y = pos.getY(), z = pos.getZ();
		if (dugHere(x, y, z)) {
			return null;
		}
		boolean nextToDug = false;
		for (Direction d : Direction.values()) {
			int nx = x + d.getStepX(), ny = y + d.getStepY(), nz = z + d.getStepZ();
			// (A thing that stood free, dug out, leaves nothing solid round it.)
			if (dugHere(nx, ny, nz) && !freeStanding(nx, ny, nz)) {
				nextToDug = true;
				break;
			}
		}
		if (!nextToDug) {
			return null;
		}
		if (WALLS.size() > 2048) {
			WALLS.clear();
		}
		var shapes = WALLS.computeIfAbsent(net.minecraft.core.SectionPos.asLong(x >> 4, y >> 4, z >> 4), k -> new java.util.concurrent.ConcurrentHashMap<>());
		long key = pos.asLong();
		VoxelShape cached = shapes.get(key);
		if (cached != null) {
			return cached == NO_WALL ? null : cached;
		}
		VoxelShape shape = computeWall(x, y, z);
		shapes.put(key, shape == null ? NO_WALL : shape);
		return shape;
	}

	private static @Nullable VoxelShape computeWall(int x, int y, int z) {
		if (!CyberCollision.hasTriangles()) {
			// From voxels: under a surface, solid; where a surface runs through, solid up to it.
			int kind = classifyVoxel(x, y, z);
			if (kind == PARTIAL) {
				float top = partialTop(x, y, z);
				return top >= 0.999F ? Shapes.block() : Shapes.box(0, 0, 0, 1, top, 1);
			}
			return kind > AIR ? Shapes.block() : null;
		}
		Probe probe = new Probe().around(x, y, z, x + 1, y + 1, z + 1);
		if (!probe.land.isEmpty()) {
			// Solid up to the lowest the land gets over the cell (never above the ground).
			double lowest = Double.POSITIVE_INFINITY;
			for (double[] c : new double[][] { { 0.05, 0.05 }, { 0.95, 0.05 }, { 0.05, 0.95 }, { 0.95, 0.95 }, { 0.5, 0.5 } }) {
				double h = probe.landHeight(x + c[0], z + c[1]);
				if (!Double.isNaN(h)) {
					lowest = Math.min(lowest, h);
				}
			}
			if (Double.isFinite(lowest) && lowest - y > 0.05) {
				return lowest - y >= 0.999 ? Shapes.block() : Shapes.box(0, 0, 0, 1, lowest - y, 1);
			}
			if (probe.tris.isEmpty()) {
				return null;
			}
		}
		// Rocks, cave walls: solid if the middle is inside.
		return probe.test(x + 0.5, y + 0.5, z + 0.5) > AIR ? Shapes.block() : null;
	}

	// ---- the grid moving under Night City -------------------------------------------------------

	/**
	 * Collision thread: the plugin moved the grid. This machine's own server (singleplayer, or a world
	 * opened to friends) moves every hole back under the spot of Night City it was dug in; a guest's
	 * grid is its own business, and the holes are its host's.
	 */
	public static void gridMoved(HoleMove.GridMove move) {
		net.minecraft.server.MinecraftServer s = server;
		if (s == null) {
			return;
		}
		s.execute(() -> {
			try {
				remap(s.overworld(), move);
			} catch (RuntimeException e) {
				CyberCraft.LOG.error("CyberCraft: moving the holes with the grid failed", e);
			}
		});
	}

	private record OldCell(BlockState state, net.minecraft.nbt.@Nullable CompoundTag entity) {
	}

	/** Server thread: every hole of this world (and what's in it) moved by {@code move}. */
	static void remap(ServerLevel level, HoleMove.GridMove move) {
		int world = worldFor(level);
		List<Long> chunks = level.getAttached(HOLE_CHUNKS);
		if (chunks == null || chunks.isEmpty()) {
			return;
		}
		// Everything dug, what's in it, and where holes were opened from above.
		it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<OldCell> old = new it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap<>();
		List<Opening> openings = new ArrayList<>();
		for (long key : chunks) {
			int cx = net.minecraft.world.level.ChunkPos.getX(key), cz = net.minecraft.world.level.ChunkPos.getZ(key);
			LevelChunk chunk = level.getChunk(cx, cz);
			column(chunk).forEach(world, cx, cz, pos -> {
				BlockPos at = BlockPos.of(pos);
				BlockState state = level.getBlockState(at);
				var entity = level.getBlockEntity(at);
				old.put(pos, new OldCell(state, entity != null ? entity.saveWithFullMetadata(level.registryAccess()) : null));
			});
			for (Opening o : openings(chunk)) {
				if (o.world() == world) {
					openings.add(o);
				}
			}
		}
		if (old.isEmpty()) {
			return;
		}
		HoleMove.Plan plan = HoleMove.plan(old.keySet(), openings, move, pos -> !old.get(pos).state().isAir());

		// Out of the old cells (no drops, nothing reacting), then into the new ones.
		int flags = Block.UPDATE_CLIENTS | Block.UPDATE_SKIP_ALL_SIDEEFFECTS;
		for (var e : old.long2ObjectEntrySet()) {
			if (!e.getValue().state().isAir()) {
				level.setBlock(BlockPos.of(e.getLongKey()), Blocks.AIR.defaultBlockState(), flags);
			}
			HERE.remove(e.getLongKey());
		}
		List<Long> emptied = new ArrayList<>();
		for (long key : chunks) {
			LevelChunk chunk = level.getChunk(net.minecraft.world.level.ChunkPos.getX(key), net.minecraft.world.level.ChunkPos.getZ(key));
			DugColumn left = column(chunk).without(world);
			chunk.setAttached(DUG, left.sections().isEmpty() ? null : left);
			List<Opening> keep = new ArrayList<>();
			for (Opening o : openings(chunk)) {
				if (o.world() != world) {
					keep.add(o);
				}
			}
			chunk.setAttached(OPENINGS, keep.isEmpty() ? null : List.copyOf(keep));
			if (left.sections().isEmpty()) {
				emptied.add(key);
			}
		}
		List<Long> registry = new ArrayList<>(chunks);
		registry.removeAll(emptied);
		level.setAttached(HOLE_CHUNKS, registry.isEmpty() ? null : List.copyOf(registry));

		for (var e : plan.cells().long2LongEntrySet()) {
			BlockPos to = BlockPos.of(e.getLongKey());
			markDug(level, world, to);
			OldCell from = old.get(e.getLongValue());
			if (from == null || from.state().isAir()) {
				continue;
			}
			level.setBlock(to, from.state(), flags);
			if (from.entity() != null) {
				var moved = net.minecraft.world.level.block.entity.BlockEntity.loadStatic(to, from.state(), from.entity(), level.registryAccess());
				if (moved != null) {
					level.setBlockEntity(moved);
				}
			}
		}
		for (Opening o : plan.openings()) {
			LevelChunk chunk = level.getChunkAt(BlockPos.of(o.pos()));
			List<Opening> list = new ArrayList<>(openings(chunk));
			list.add(o);
			chunk.setAttached(OPENINGS, List.copyOf(list));
		}

		// What was lying or standing in the holes goes with them (players follow V, who stays put).
		int moved = 0;
		for (var e : old.long2ObjectEntrySet()) {
			BlockPos at = BlockPos.of(e.getLongKey());
			for (var entity : level.getEntitiesOfClass(net.minecraft.world.entity.Entity.class, new AABB(at),
				en -> !(en instanceof net.minecraft.world.entity.player.Player) && en.blockPosition().equals(at))) {
				long to = plan.moved(e.getLongKey());
				if (to == Long.MIN_VALUE) {
					continue;
				}
				BlockPos dest = BlockPos.of(to);
				entity.teleportTo(dest.getX() + (entity.getX() - at.getX()), dest.getY() + (entity.getY() - at.getY()), dest.getZ() + (entity.getZ() - at.getZ()));
				moved++;
			}
		}
		CyberCraft.LOG.info("CyberCraft: the grid moved; {} dug blocks ({} openings, {} things in them) moved {} with Night City", old.size(),
			openings.size(), moved, move.shift() ? String.format("by (%.0f, %.0f, %.0f)", move.tx(), move.ty(), move.tz()) : "and turned");
	}

	/**
	 * Looks up whether a cell is dug in the client's world (set by the client; null on a server).
	 * Used by the crosshair pick to hit the walls of dug holes.
	 */
	public interface DugLookup {
		boolean isDug(int x, int y, int z);
	}

	public static volatile @Nullable DugLookup clientDug;

	/** Closest point on the triangle to p (Ericson, Real-Time Collision Detection 5.1.5). */
	static void closestPoint(CyberTri t, double px, double py, double pz, double[] out) {
		double abx = t.bx - t.ax, aby = t.by - t.ay, abz = t.bz - t.az;
		double acx = t.cx - t.ax, acy = t.cy - t.ay, acz = t.cz - t.az;
		double apx = px - t.ax, apy = py - t.ay, apz = pz - t.az;
		double d1 = abx * apx + aby * apy + abz * apz, d2 = acx * apx + acy * apy + acz * apz;
		if (d1 <= 0 && d2 <= 0) {
			set(out, t.ax, t.ay, t.az);
			return;
		}
		double bpx = px - t.bx, bpy = py - t.by, bpz = pz - t.bz;
		double d3 = abx * bpx + aby * bpy + abz * bpz, d4 = acx * bpx + acy * bpy + acz * bpz;
		if (d3 >= 0 && d4 <= d3) {
			set(out, t.bx, t.by, t.bz);
			return;
		}
		double vc = d1 * d4 - d3 * d2;
		if (vc <= 0 && d1 >= 0 && d3 <= 0) {
			double v = d1 / (d1 - d3);
			set(out, t.ax + abx * v, t.ay + aby * v, t.az + abz * v);
			return;
		}
		double cpx = px - t.cx, cpy = py - t.cy, cpz = pz - t.cz;
		double d5 = abx * cpx + aby * cpy + abz * cpz, d6 = acx * cpx + acy * cpy + acz * cpz;
		if (d6 >= 0 && d5 <= d6) {
			set(out, t.cx, t.cy, t.cz);
			return;
		}
		double vb = d5 * d2 - d1 * d6;
		if (vb <= 0 && d2 >= 0 && d6 <= 0) {
			double w = d2 / (d2 - d6);
			set(out, t.ax + acx * w, t.ay + acy * w, t.az + acz * w);
			return;
		}
		double va = d3 * d6 - d5 * d4;
		if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
			double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
			set(out, t.bx + (t.cx - t.bx) * w, t.by + (t.cy - t.by) * w, t.bz + (t.cz - t.bz) * w);
			return;
		}
		double denom = 1.0 / (va + vb + vc);
		double v = vb * denom, w = vc * denom;
		set(out, t.ax + abx * v + acx * w, t.ay + aby * v + acy * w, t.az + abz * v + acz * w);
	}

	private static void set(double[] out, double x, double y, double z) {
		out[0] = x;
		out[1] = y;
		out[2] = z;
	}
}
