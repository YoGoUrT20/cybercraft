package dev.cybercraft.client.render;

import com.mojang.blaze3d.vertex.QuadInstance;
import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.client.CyberClient;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import dev.cybercraft.world.CyberClip;
import it.unimi.dsi.fastutil.ints.IntOpenHashSet;
import it.unimi.dsi.fastutil.longs.LongLinkedOpenHashSet;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.client.model.geom.builders.UVPair;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.block.FluidRenderer;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.util.ARGB;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.Arrow;
import net.minecraft.world.entity.projectile.arrow.SpectralArrow;
import net.minecraft.world.entity.projectile.arrow.ThrownTrident;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.ItemDisplayContext;
import net.minecraft.world.item.ItemStack;
import net.minecraft.server.level.BlockDestructionProgress;
import net.minecraft.world.level.CardinalLighting;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.jspecify.annotations.Nullable;

/**
 * Ships what Minecraft would draw in the world to the plugin, which draws it in Cyberpunk's frame
 * (so it is locked to the world and hidden behind the city): block and fluid meshes built by
 * Minecraft's own block renderer (models, tint, smooth lighting), the texture atlas, arrows,
 * dropped items and cracks (and the targeted-block outline, which it doesn't draw yet). With each
 * section's mesh go its light blocks and solid blocks, which the plugin turns into Cyberpunk lights
 * and colliders. Render thread only.
 */
public final class WorldExporter {
	private static final int SECTIONS_PER_FRAME = 12;
	private static final double ENTITY_RANGE = 96.0;

	private static final LongLinkedOpenHashSet DIRTY = new LongLinkedOpenHashSet();
	private static final LongOpenHashSet SENT = new LongOpenHashSet(); // sections the plugin holds a mesh for
	private static final LongOpenHashSet LIT = new LongOpenHashSet(); // sections the plugin holds lights for
	private static final LongOpenHashSet SOLID = new LongOpenHashSet(); // sections the plugin holds colliders for
	private static final long[] SOLID_BITS = new long[64]; // 4096 blocks: bit x + 16z + 256y
	private static final LongOpenHashSet DUG = new LongOpenHashSet(); // sections whose dug cells went to the plugin
	private static final ByteBuffer LIGHTS = ByteBuffer.allocate(16 * 16 * 16 * 8).order(ByteOrder.LITTLE_ENDIAN);
	private static int sentGeneration = Integer.MIN_VALUE;
	private static int meshesSent;
	private static ClientLevel sentLevel;
	private static CyberAtlas atlas;
	private static ModelBlockRenderer blockRenderer;
	private static FluidRenderer fluidRenderer;
	private static final MeshBuilder MESH = new MeshBuilder();
	private static final List<CyberLink.WorldEntity> ENTITIES = new ArrayList<>();
	private static final IntOpenHashSet LEFT_TO_SCENE = new IntOpenHashSet(); // dropped items drawn by Minecraft's renderer instead
	private static final ItemStackRenderState ITEM_STATE = new ItemStackRenderState();
	private static final java.util.Map<Item, float[]> ICONS = new java.util.HashMap<>();
	private static final java.util.Map<BlockState, float[]> CUBE_FACES = new java.util.HashMap<>();
	private static final RandomSource RANDOM = RandomSource.create();

	private WorldExporter() {
	}

	public static void markDirty(int sx, int sy, int sz) {
		synchronized (DIRTY) {
			DIRTY.add(SectionPos.asLong(sx, sy, sz));
		}
	}

	/** A block the player just placed or broke: re-mesh ahead of everything else. */
	public static void markDirtyNow(int sx, int sy, int sz) {
		synchronized (DIRTY) {
			DIRTY.addAndMoveToFirst(SectionPos.asLong(sx, sy, sz));
		}
	}

	public static void frame(Minecraft minecraft, float partialTick) {
		ClientLevel level = minecraft.level;
		if (level == null || minecraft.player == null || !CyberLink.active()) {
			return;
		}
		if (sentGeneration != CyberLink.generation() || sentLevel != level || atlas == null || atlas.stale(minecraft)) {
			resendEverything(minecraft, level);
		}
		meshDirtySections(level);
		// Animated textures (water, lava, fire, ...): the frame for this game tick.
		atlas.animate(level.getGameTime(), region -> {
			ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(region.x()).putInt(region.y()).putInt(region.w()).putInt(region.h()).flip();
			return CyberLink.tryWriteRender(Proto.REN_ATLAS_REGION, header, region.pixels());
		});
		exportEntities(minecraft, level, partialTick);
		AvatarExporter.frame(minecraft, atlas, partialTick);
	}

	/**
	 * Where Minecraft's first-person hands and held items go instead of its own renderer: to the
	 * plugin, which draws them lit by Cyberpunk's picture (CYBER_DRAWS_HAND). Null leaves them to
	 * Minecraft. Pair with {@link #handDone}. Render thread.
	 */
	public static @Nullable SubmitNodeCollector handCollector() {
		if (atlas == null || !CyberLink.active() || !CyberClient.linked() || !CyberClient.state().drawsHand()) {
			return null;
		}
		return AvatarExporter.beginHand(atlas);
	}

	/** The hands went to {@link #handCollector}'s collector (a_ok), drawn with a_fov (degrees, vertical). */
	public static void handDone(float fov, boolean ok) {
		AvatarExporter.endHand(fov, ok);
	}

	private static void resendEverything(Minecraft minecraft, ClientLevel level) {
		sentGeneration = CyberLink.generation();
		sentLevel = level;
		atlas = CyberAtlas.build(minecraft);
		CyberCraft.LOG.info("CyberCraft: {} animated textures (water, lava, fire, ...) will play in Cyberpunk", atlas.animatedSprites());
		AvatarExporter.reset();
		ICONS.clear();
		CUBE_FACES.clear();
		boolean ao = minecraft.options.ambientOcclusion().get();
		blockRenderer = new ModelBlockRenderer(ao, true, minecraft.getBlockColors());
		fluidRenderer = new FluidRenderer(minecraft.getModelManager().getFluidStateModelSet());
		CyberLink.writeRender(Proto.REN_CLEAR_ALL, ByteBuffer.allocate(0), null);
		ByteBuffer header = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(atlas.width).putInt(atlas.height).flip();
		boolean ok = CyberLink.writeRender(Proto.REN_ATLAS, header, atlas.pixels.duplicate().clear());
		CyberCraft.LOG.info("CyberCraft: sent {}x{} texture atlas to Cyberpunk ({})", atlas.width, atlas.height, ok ? "ok" : "FAILED");
		SENT.clear();
		LIT.clear();
		SOLID.clear();
		DUG.clear();
		dev.cybercraft.client.CyberDigClient.resendAll();
		// Everything already loaded needs meshing again; later chunk loads mark themselves dirty.
		int radius = minecraft.options.getEffectiveRenderDistance() + 1;
		int pcx = SectionPos.blockToSectionCoord(minecraft.player.getBlockX()), pcz = SectionPos.blockToSectionCoord(minecraft.player.getBlockZ());
		for (int cx = pcx - radius; cx <= pcx + radius; cx++) {
			for (int cz = pcz - radius; cz <= pcz + radius; cz++) {
				LevelChunk chunk = level.getChunkSource().getChunk(cx, cz, ChunkStatus.FULL, false);
				if (chunk == null) {
					continue;
				}
				LevelChunkSection[] sections = chunk.getSections();
				for (int i = 0; i < sections.length; i++) {
					if (!sections[i].hasOnlyAir()) {
						markDirty(cx, chunk.getSectionYFromSectionIndex(i), cz);
					}
				}
			}
		}
	}

	private static void meshDirtySections(ClientLevel level) {
		// Chunk loads and light updates dirty thousands of all-air sections; those cost a lookup.
		// Real meshing is limited per frame.
		long deadline = System.nanoTime() + 3_000_000L;
		int meshed = 0;
		while (meshed < SECTIONS_PER_FRAME && System.nanoTime() < deadline) {
			long key;
			synchronized (DIRTY) {
				if (DIRTY.isEmpty()) {
					return;
				}
				key = DIRTY.removeFirstLong();
			}
			if (meshSection(level, key)) {
				meshed++;
			}
		}
	}

	private static LevelChunkSection sectionAt(ClientLevel level, LevelChunk chunk, int sy) {
		int index = level.getSectionIndexFromSectionY(sy);
		return index >= 0 && index < chunk.getSections().length ? chunk.getSections()[index] : null;
	}

	/** Returns true if real meshing work was done. */
	private static boolean meshSection(ClientLevel level, long key) {
		int sx = SectionPos.x(key), sy = SectionPos.y(key), sz = SectionPos.z(key);
		LevelChunk chunk = level.getChunkSource().getChunk(sx, sz, ChunkStatus.FULL, false);
		if (chunk == null) {
			return false; // unloaded: the plugin keeps what it has
		}
		LevelChunkSection section = sectionAt(level, chunk, sy);
		boolean empty = section == null || section.hasOnlyAir();
		long[] dug = dev.cybercraft.client.CyberDigClient.dugBits(chunk, sy);
		int dugCount = 0;
		if (dug != null) {
			for (long word : dug) {
				dugCount += Long.bitCount(word);
			}
		}
		if (empty && !SENT.contains(key) && !LIT.contains(key) && !SOLID.contains(key) && dugCount == 0 && !DUG.contains(key)) {
			return false; // nothing there and nothing to remove
		}
		MESH.reset();
		LIGHTS.clear();
		int lightCount = 0;
		java.util.Arrays.fill(SOLID_BITS, 0L);
		int solidCount = 0;
		MESH.cardinal = level.cardinalLighting();
		if (!empty) {
			BlockPos origin = SectionPos.of(sx, sy, sz).origin();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			for (int y = 0; y < 16; y++) {
				for (int z = 0; z < 16; z++) {
					for (int x = 0; x < 16; x++) {
						pos.set(origin.getX() + x, origin.getY() + y, origin.getZ() + z);
						BlockState state = chunk.getBlockState(pos);
						if (state.isAir()) {
							continue;
						}
						// Blocks NPCs can't walk through (anything with a collision shape): the plugin
						// gives them colliders, so NPCs, cars and bullets meet them.
						if (!state.getCollisionShape(level, pos).isEmpty()) {
							int bit = x + 16 * z + 256 * y;
							SOLID_BITS[bit >> 6] |= 1L << (bit & 63);
							solidCount++;
						}
						// Light-emitting blocks (torches, lava, glowstone, ...) light Night City too: the
						// plugin puts a Cyberpunk light at the nearest ones.
						int emission = state.getLightEmission();
						if (emission > 0) {
							LIGHTS.put((byte) x).put((byte) y).put((byte) z).put((byte) emission).putInt(BlockLightColors.of(state));
							lightCount++;
						}
						MESH.glows = emission > 0;
						FluidState fluid = state.getFluidState();
						if (!fluid.isEmpty()) {
							// The city's ground in the cell: the fluid is drawn in the space above it.
							MESH.fluidGround = dev.cybercraft.world.CyberCollision.groundTop(pos);
							MESH.fluidBaseY = y;
							fluidRenderer.tesselate(level, pos, MESH, state, fluid);
							MESH.fluidGround = 0.0F;
						}
						if (state.getRenderShape() == RenderShape.MODEL) {
							var model = Minecraft.getInstance().getModelManager().getBlockStateModelSet().get(state);
							blockRenderer.tesselateBlock(MESH, x, y, z, level, pos.immutable(), state, model, state.getSeed(pos));
						}
					}
				}
			}
		}
		MESH.glows = false;
		// Holes dug into the ground (CyberDig): Minecraft walls where its surface still runs above
		// them. None in Night City, where digging is off and the plugin sends no triangles to dig.
		var digLookup = dev.cybercraft.world.CyberDig.clientDug;
		if (dugCount > 0 && digLookup != null) {
			Minecraft minecraft = Minecraft.getInstance();
			DigWalls.add(level, sx, sy, sz, dug, digLookup, st -> cubeFaces(minecraft, st), MESH::wall);
		}
		if (MESH.vertexCount() == 0 && !SENT.contains(key) && lightCount == 0 && !LIT.contains(key) && solidCount == 0 && !SOLID.contains(key) && dugCount == 0
			&& !DUG.contains(key)) {
			return true;
		}
		ByteBuffer header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(MESH.vertexCount()).flip();
		if (CyberLink.writeRender(Proto.REN_SECTION, header, MESH.bytes())) {
			if (MESH.vertexCount() > 0) {
				SENT.add(key);
			} else {
				SENT.remove(key);
			}
			if (lightCount > 0 || LIT.contains(key)) {
				ByteBuffer lightHeader = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(lightCount).flip();
				if (!CyberLink.writeRender(Proto.REN_LIGHTS, lightHeader, LIGHTS.flip())) {
					markDirty(sx, sy, sz); // ring full; send both again later
				} else if (lightCount > 0) {
					LIT.add(key);
				} else {
					LIT.remove(key);
				}
			}
			if (solidCount > 0 || SOLID.contains(key)) {
				ByteBuffer solidHeader = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(solidCount).flip();
				ByteBuffer bits = ByteBuffer.allocate(solidCount > 0 ? 512 : 0).order(ByteOrder.LITTLE_ENDIAN);
				if (solidCount > 0) {
					for (long word : SOLID_BITS) {
						bits.putLong(word);
					}
				}
				if (!CyberLink.writeRender(Proto.REN_SOLIDS, solidHeader, bits.flip())) {
					markDirty(sx, sy, sz);
				} else if (solidCount > 0) {
					SOLID.add(key);
				} else {
					SOLID.remove(key);
				}
			}
			// The section's dug cells. The plugin drains them without using them: Cyberpunk's
			// geometry can't be cut open on screen.
			if (dugCount > 0 || DUG.contains(key)) {
				ByteBuffer dugHeader = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(sx).putInt(sy).putInt(sz).putInt(dugCount)
					.putInt(dev.cybercraft.client.CyberDigClient.world()).putInt(0).flip();
				ByteBuffer bits = ByteBuffer.allocate(dugCount > 0 ? 512 : 0).order(ByteOrder.LITTLE_ENDIAN);
				if (dugCount > 0) {
					for (long word : dug) {
						bits.putLong(word);
					}
				}
				if (!CyberLink.writeRender(Proto.REN_DUG, dugHeader, bits.flip())) {
					markDirty(sx, sy, sz);
				} else if (dugCount > 0) {
					DUG.add(key);
				} else {
					DUG.remove(key);
				}
			}
			if (++meshesSent <= 10 || meshesSent % 200 == 0) {
				CyberCraft.LOG.info("CyberCraft: block mesh for section {} {} {}: {} vertices ({} sections in Cyberpunk)", sx, sy, sz, MESH.vertexCount(), SENT.size());
			}
		} else {
			markDirty(sx, sy, sz); // ring full; try again later
		}
		return true;
	}

	/** A dropped item this path can't show, which Minecraft's own renderer draws in the scene (AvatarExporter). */
	static boolean leftToScene(Entity e) {
		return LEFT_TO_SCENE.contains(e.getId());
	}

	private static void exportEntities(Minecraft minecraft, ClientLevel level, float partialTick) {
		ENTITIES.clear();
		LEFT_TO_SCENE.clear();
		Vec3 eye = minecraft.player.getEyePosition(partialTick);
		for (Entity e : level.entitiesForRendering()) {
			if (ENTITIES.size() >= Proto.MAX_WORLD_ENTITIES || e.distanceToSqr(eye) > ENTITY_RANGE * ENTITY_RANGE) {
				continue;
			}
			Vec3 p = e.getPosition(partialTick);
			if (e instanceof AbstractArrow arrow) {
				boolean trident = arrow instanceof ThrownTrident;
				int kind = trident ? Proto.WE_TRIDENT : Proto.WE_ARROW;
				float yaw = Mth.rotLerp(partialTick, arrow.yRotO, arrow.getYRot());
				float pitch = Mth.lerp(partialTick, arrow.xRotO, arrow.getXRot());
				// Arrows use Minecraft's arrow model and entity texture; tridents their item icon.
				float[] uv = trident ? ICONS.computeIfAbsent(Items.TRIDENT, i -> iconUv(minecraft, level, new ItemStack(i)))
					: atlas.arrowUv(arrow instanceof SpectralArrow ? 2 : arrow instanceof Arrow tippable && tippable.getColor() > 0 ? 1 : 0);
				if (uv != null) {
					ENTITIES.add(new CyberLink.WorldEntity(kind, e.getId(), (float) p.x, (float) p.y, (float) p.z, yaw, pitch, 1.0F, null, uv, 0));
				}
			} else if (e instanceof ItemEntity item) {
				float bob = Mth.sin((item.getAge() + partialTick) / 10.0F + item.bobOffs) * 0.1F + 0.1F;
				float spin = ItemEntity.getSpin(item.getAge() + partialTick, item.bobOffs) * Mth.RAD_TO_DEG;
				addItem(minecraft, level, e, item.getItem(), p.add(0, bob, 0), spin);
			} else if (e instanceof net.minecraft.world.entity.LivingEntity && !(e instanceof dev.cybercraft.combat.NpcEntity) && !e.isInvisible()
				&& (e != minecraft.player || minecraft.gameRenderer.mainCamera().isDetached())) {
				// Players and mobs: a soft shadow under their feet (the plugin doesn't draw it yet).
				ENTITIES.add(new CyberLink.WorldEntity(Proto.WE_SHADOW, e.getId(), (float) p.x, (float) p.y, (float) p.z, 0.0F, 0.0F, e.getBbWidth(), null, null, 0));
			}
		}
		addCracks(level);
		CyberLink.writeWorldEntities(ENTITIES, selection(minecraft, level));
	}

	/**
	 * A dropped item the way Minecraft shows it: blocks as small spinning cubes with their own face
	 * textures, everything else as its icon. Items one untinted sprite can't show (potions, dyed
	 * leather, chests, heads, ...) are left to Minecraft's renderer. {@code p} is the item's resting
	 * point (bottom).
	 */
	private static void addItem(Minecraft minecraft, ClientLevel level, Entity e, ItemStack stack, Vec3 p, float yaw) {
		if (stack.getItem() instanceof BlockItem blockItem) {
			BlockState state = blockItem.getBlock().defaultBlockState();
			if (state.getRenderShape() == RenderShape.MODEL
				&& Block.isShapeFullBlock(state.getShape(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO))) {
				float[] faces = cubeFaces(minecraft, state);
				if (faces != null) {
					float size = 0.25F;
					int tint = cubeTint(minecraft, state);
					ENTITIES.add(new CyberLink.WorldEntity(
						Proto.WE_BLOCK, e.getId(), (float) p.x, (float) (p.y + size * 0.5 + 0.02), (float) p.z, yaw, 0.0F, size, null, faces, tint
					));
					return;
				}
			}
		}
		minecraft.getItemModelResolver().updateForTopItem(ITEM_STATE, stack, ItemDisplayContext.GROUND, level, null, 0);
		if (!oneFlatSprite(ITEM_STATE)) {
			LEFT_TO_SCENE.add(e.getId());
			return;
		}
		float[] uv = particleUv();
		if (uv != null) {
			ENTITIES.add(new CyberLink.WorldEntity(Proto.WE_ITEM, e.getId(), (float) p.x, (float) p.y + 0.25F, (float) p.z, yaw, 0.0F, 0.5F, null, uv, 0));
		}
	}

	/**
	 * True if the item's sprite alone shows it. A potion's sprite is only its grey liquid, which
	 * Minecraft colours (a tint layer) and draws inside a bottle (a second texture layer).
	 */
	private static boolean oneFlatSprite(ItemStackRenderState state) {
		var accessor = (dev.cybercraft.client.mixin.ItemStackRenderStateAccessor) state;
		if (accessor.cybercraft$activeLayerCount() != 1) {
			return false;
		}
		var layer = accessor.cybercraft$layers()[0];
		return layer.tintLayers().isEmpty() && ((dev.cybercraft.client.mixin.ItemLayerRenderStateAccessor) layer).cybercraft$specialRenderer() == null;
	}

	/** Side, top and bottom atlas rects of a full-cube block's model, cached per block state. */
	private static float[] cubeFaces(Minecraft minecraft, BlockState state) {
		return CUBE_FACES.computeIfAbsent(state, s -> {
			var model = minecraft.getModelManager().getBlockStateModelSet().get(s);
			var parts = new ArrayList<net.minecraft.client.renderer.block.dispatch.BlockStateModelPart>();
			RANDOM.setSeed(42);
			model.collectParts(RANDOM, parts);
			float[] out = new float[12];
			Direction[] dirs = { Direction.NORTH, Direction.UP, Direction.DOWN };
			for (int f = 0; f < 3; f++) {
				TextureAtlasSprite sprite = null;
				for (var part : parts) {
					var quads = part.getQuads(dirs[f]);
					if (!quads.isEmpty()) {
						sprite = quads.getFirst().materialInfo().sprite();
						break;
					}
				}
				if (sprite == null) {
					return null;
				}
				System.arraycopy(atlas.rect(sprite), 0, out, f * 4, 4);
			}
			return out;
		});
	}

	/** Grass and leaves are grey in the atlas; Minecraft tints them. RGBA8, 0 for none. */
	private static int cubeTint(Minecraft minecraft, BlockState state) {
		var sources = minecraft.getBlockColors().getTintSources(state);
		if (sources.isEmpty() || sources.getFirst() == null) {
			return 0;
		}
		int argb = sources.getFirst().color(state);
		return 0xFF000000 | (argb & 0xFF) << 16 | (argb & 0xFF00) | (argb >> 16 & 0xFF);
	}

	/** Cracks over blocks being mined (ours and anyone else's). */
	private static void addCracks(ClientLevel level) {
		for (BlockDestructionProgress progress : ((dev.cybercraft.client.mixin.ClientLevelAccessor) level).cybercraft$destroyingBlocks().values()) {
			int stage = progress.getProgress();
			if (stage < 0 || stage > 9 || ENTITIES.size() >= Proto.MAX_WORLD_ENTITIES) {
				continue;
			}
			BlockPos pos = progress.getPos();
			VoxelShape shape = level.getBlockState(pos).getShape(level, pos);
			if (shape.isEmpty()) {
				continue;
			}
			AABB box = shape.bounds().move(pos).inflate(0.004);
			ENTITIES.add(new CyberLink.WorldEntity(
				Proto.WE_CRACK, pos.hashCode(), (float) box.minX, (float) box.minY, (float) box.minZ, 0.0F, 0.0F, 1.0F,
				new float[] { (float) box.getXsize(), (float) box.getYsize(), (float) box.getZsize() }, atlas.crackUv(stage), 0
			));
		}
	}

	/** The item's icon in the combined atlas {u0, v0, u1, v1}, or null. */
	private static float[] iconUv(Minecraft minecraft, ClientLevel level, ItemStack stack) {
		minecraft.getItemModelResolver().updateForTopItem(ITEM_STATE, stack, ItemDisplayContext.GROUND, level, null, 0);
		return particleUv();
	}

	/** The sprite of the item last resolved into ITEM_STATE, or null. */
	private static float[] particleUv() {
		RANDOM.setSeed(0);
		var material = ITEM_STATE.pickParticleMaterial(RANDOM);
		if (material == null) {
			return null;
		}
		return atlas.rect(material.sprite());
	}

	/**
	 * The block outline (the plugin doesn't draw it yet): the targeted block, or where a held block
	 * would go against the city.
	 */
	private static float[] selection(Minecraft minecraft, ClientLevel level) {
		HitResult hit = minecraft.hitResult;
		if (!(hit instanceof BlockHitResult blockHit) || hit.getType() != HitResult.Type.BLOCK || minecraft.gui.screen() != null) {
			return null;
		}
		BlockPos pos = blockHit.getBlockPos();
		if (blockHit instanceof CyberClip.CyberHitResult) {
			if (!(minecraft.player.getMainHandItem().getItem() instanceof BlockItem)) {
				return null;
			}
			return new float[] { pos.getX(), pos.getY(), pos.getZ(), pos.getX() + 1, pos.getY() + 1, pos.getZ() + 1 };
		}
		VoxelShape shape = level.getBlockState(pos).getShape(level, pos);
		if (shape.isEmpty()) {
			return null;
		}
		AABB box = shape.bounds().move(pos);
		return new float[] { (float) box.minX, (float) box.minY, (float) box.minZ, (float) box.maxX, (float) box.maxY, (float) box.maxZ };
	}

	/**
	 * Collects Minecraft's block quads (and fluid vertices) as triangles in the RenVertex layout:
	 * section-relative position, combined-atlas UV, RGBA colour (tint and shading), block/sky light.
	 */
	private static final class MeshBuilder implements net.minecraft.client.renderer.block.BlockQuadOutput, FluidRenderer.Output, VertexConsumer {
		private ByteBuffer buf = ByteBuffer.allocateDirect(1 << 20).order(ByteOrder.LITTLE_ENDIAN);
		private int vertices;
		// fluid quad assembly
		private final float[] fq = new float[4 * 8];
		private int fqCount;
		private boolean fluidTranslucent;

		void reset() {
			this.buf.clear();
			this.vertices = 0;
			this.fqCount = 0;
			this.glows = false;
		}

		int vertexCount() {
			return this.vertices;
		}

		ByteBuffer bytes() {
			return this.buf.duplicate().flip();
		}

		private void ensure(int bytes) {
			if (this.buf.remaining() < bytes) {
				ByteBuffer bigger = ByteBuffer.allocateDirect(Math.max(this.buf.capacity() * 2, this.buf.position() + bytes)).order(ByteOrder.LITTLE_ENDIAN);
				this.buf.flip();
				bigger.put(this.buf);
				this.buf = bigger;
			}
		}

		/** The level's fixed per-face brightness, taken back out of the colours: the plugin shades faces itself. */
		CardinalLighting cardinal = CardinalLighting.DEFAULT;

		private void vertex(float x, float y, float z, float u, float v, int argb, int light, int flags) {
			this.buf.putFloat(x).putFloat(y).putFloat(z).putFloat(u).putFloat(v);
			this.buf.put((byte) (argb >> 16)).put((byte) (argb >> 8)).put((byte) argb).put((byte) (argb >>> 24));
			int block = (light >> 4) & 0xF;
			int sky = (light >> 20) & 0xF;
			this.buf.putInt(block | (sky << 8));
			this.buf.putInt(flags);
			this.vertices++;
		}

		/** The block being meshed gives light (torches, glowstone, lava, ...): its faces glow. */
		boolean glows;

		/**
		 * Vertex flags: cutout or translucent, glowing (bit 2), plus the face normal (Direction
		 * ordinal + 1) that the plugin lights the face by. 0 leaves it without a normal (plants and
		 * other quads Minecraft doesn't shade by direction).
		 */
		private int flags(boolean translucent, Direction normal) {
			return (translucent ? 2 : 1) | (this.glows ? 4 : 0) | (normal == null ? 0 : (normal.ordinal() + 1) << 4);
		}

		/** Takes Minecraft's fixed face brightness back out of a colour, leaving tint and ambient occlusion. */
		private static int unshade(int argb, float shade) {
			if (shade >= 0.999F || shade <= 0.0F) {
				return argb;
			}
			int r = Math.min(255, Math.round(((argb >> 16) & 0xFF) / shade));
			int g = Math.min(255, Math.round(((argb >> 8) & 0xFF) / shade));
			int b = Math.min(255, Math.round((argb & 0xFF) / shade));
			return (argb & 0xFF000000) | (r << 16) | (g << 8) | b;
		}

		/** A dug hole's wall (DigWalls): an untinted opaque quad. */
		void wall(float[] xyz, float[] uv, int light, Direction normal) {
			this.ensure(6 * Proto.REN_VERTEX_BYTES);
			int flags = flags(false, normal);
			for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
				this.vertex(xyz[k * 3], xyz[k * 3 + 1], xyz[k * 3 + 2], uv[k * 2], uv[k * 2 + 1], 0xFFFFFFFF, light, flags);
			}
		}

		// ---- block quads (BlockQuadOutput) ----
		@Override
		public void put(float x, float y, float z, BakedQuad quad, QuadInstance instance) {
			this.ensure(6 * Proto.REN_VERTEX_BYTES);
			TextureAtlasSprite sprite = quad.materialInfo().sprite();
			boolean translucent = quad.materialInfo().layer().translucent();
			int emission = quad.materialInfo().lightEmission();
			// Plants and the like are shaded as if facing up whatever way they face: no normal for them.
			Direction override = quad.materialInfo().shadeDirectionOverride();
			Direction normal = override == Direction.UP && quad.direction() != Direction.UP ? null : override != null ? override : quad.direction();
			float shade = this.cardinal.byFace(override != null ? override : quad.direction());
			int flags = flags(translucent, normal) | (emission > 0 ? 4 : 0);
			for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
				var p = quad.position(k);
				long uv = quad.packedUV(k);
				float u = atlas.u(sprite, UVPair.unpackU(uv));
				float v = atlas.v(sprite, UVPair.unpackV(uv));
				this.vertex(p.x() + x, p.y() + y, p.z() + z, u, v, unshade(instance.getColor(k), shade), instance.getLightCoordsWithEmission(k, emission), flags);
			}
		}

		// ---- fluids (FluidRenderer.Output + VertexConsumer) ----
		// The city's ground height in the fluid's cell (0..1) and the cell's section-relative y: a
		// Minecraft fluid level counts from the cell's floor, so on ground partway up the cell the
		// fluid is squeezed into the space above it (thin edges stay visible on the ground).
		float fluidGround;
		int fluidBaseY;

		@Override
		public VertexConsumer getBuilder(ChunkSectionLayer layer) {
			this.fluidTranslucent = layer.translucent();
			return this;
		}

		@Override
		public void addVertex(float x, float y, float z, int color, float u, float v, int overlay, int light, float nx, float ny, float nz) {
			int o = this.fqCount * 8;
			if (this.fluidGround > 0.0F) {
				float t = Math.max(0.0F, Math.min(1.0F, y - this.fluidBaseY));
				y = this.fluidBaseY + this.fluidGround + t * (1.0F - this.fluidGround);
			}
			this.fq[o] = x;
			this.fq[o + 1] = y;
			this.fq[o + 2] = z;
			this.fq[o + 3] = atlas.blockU(u);
			this.fq[o + 4] = atlas.blockV(v);
			this.fq[o + 5] = Float.intBitsToFloat(color);
			this.fq[o + 6] = Float.intBitsToFloat(light);
			if (++this.fqCount == 4) {
				this.fqCount = 0;
				this.ensure(6 * Proto.REN_VERTEX_BYTES);
				// Fluid faces are shaded like block faces (up, down, or the side's brightness).
				Direction normal = nx == 0 && ny == 0 && nz == 0 ? null : Direction.getApproximateNearest(nx, ny, nz);
				float shade = normal == null ? 1.0F
					: normal.getAxis() == Direction.Axis.Y ? this.cardinal.byFace(normal) : this.cardinal.up() * this.cardinal.byFace(normal);
				int flags = flags(this.fluidTranslucent, normal);
				for (int k : new int[] { 0, 1, 2, 0, 2, 3 }) {
					int b = k * 8;
					this.vertex(this.fq[b], this.fq[b + 1], this.fq[b + 2], this.fq[b + 3], this.fq[b + 4],
						unshade(Float.floatToRawIntBits(this.fq[b + 5]), shade), Float.floatToRawIntBits(this.fq[b + 6]), flags);
				}
			}
		}

		@Override
		public VertexConsumer addVertex(float x, float y, float z) {
			throw new UnsupportedOperationException();
		}

		@Override
		public VertexConsumer setColor(int r, int g, int b, int a) {
			return this;
		}

		@Override
		public VertexConsumer setColor(int color) {
			return this;
		}

		@Override
		public VertexConsumer setUv(float u, float v) {
			return this;
		}

		@Override
		public VertexConsumer setUv1(int u, int v) {
			return this;
		}

		@Override
		public VertexConsumer setUv2(int u, int v) {
			return this;
		}

		@Override
		public VertexConsumer setUv3(float u, float v) {
			return this;
		}

		@Override
		public VertexConsumer setNormal(float x, float y, float z) {
			return this;
		}

		@Override
		public VertexConsumer setLineWidth(float width) {
			return this;
		}
	}
}
