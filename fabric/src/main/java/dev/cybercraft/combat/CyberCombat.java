package dev.cybercraft.combat;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageSources;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import org.jspecify.annotations.Nullable;

/**
 * Combat between the Minecraft player and Night City's NPCs, server side.
 *
 * <p>Every NPC the plugin mirrors (the one under V's crosshair and those fought lately) gets an
 * invisible {@link NpcEntity} at its exact position. Minecraft weapons hit those like any mob; the
 * resulting damage is sent to the plugin, which takes it off the real NPC's health. Whatever hurts V
 * comes back as Minecraft damage, so armor, shields, knockback, hurt sounds and death all work the
 * Minecraft way.
 */
public final class CyberCombat {
	public static final ResourceKey<EntityType<?>> NPC_KEY =
		ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "npc"));
	public static final EntityType<NpcEntity> NPC = Registry.register(
		BuiltInRegistries.ENTITY_TYPE,
		NPC_KEY,
		EntityType.Builder.<NpcEntity>of(NpcEntity::new, MobCategory.MISC)
			.sized(0.6F, 1.8F)
			.noSave()
			.noSummon()
			.noLootTable()
			.clientTrackingRange(10)
			.updateInterval(1)
			.build(NPC_KEY)
	);

	private static final Map<Integer, NpcEntity> PROXIES = new HashMap<>();
	private static final List<CyberLink.Actor> ACTORS = new ArrayList<>();

	private CyberCombat() {
	}

	public static void init() {
		FabricDefaultAttributeRegistry.register(NPC, LivingEntity.createLivingAttributes());
		ServerTickEvents.END_SERVER_TICK.register(CyberCombat::serverTick);
	}

	public static @Nullable NpcEntity proxy(int actorId) {
		return PROXIES.get(actorId);
	}

	private static void serverTick(MinecraftServer server) {
		List<ServerPlayer> players = server.getPlayerList().getPlayers();
		if (!CyberLink.active() || players.isEmpty()) {
			removeAll();
			return;
		}
		ServerLevel level = players.getFirst().level();
		for (ServerPlayer player : players) {
			pickUpNearby(player);
		}
		if (CyberLink.readActors(ACTORS)) {
			sync(level);
		}
		// Hits land during the tick (melee, sweeps, arrows, fire); send one combined hit per actor.
		for (NpcEntity proxy : PROXIES.values()) {
			float[] hit = proxy.takeHit();
			if (hit != null && (hit[0] > 0.0F || hit[3] > 0.0F)) {
				CyberLink.pushEvent(
					Proto.EV_HIT_ACTOR, proxy.actorId(), hit[0], hit[1], hit[2], hit[3], Float.floatToRawIntBits(hit[4]), Float.floatToRawIntBits(hit[5])
				);
				CyberCraft.LOG.info("CyberCraft: hit {} for {} (knockback {})", proxy.getName().getString(), hit[0], hit[3]);
			}
		}
	}

	private static void sync(ServerLevel level) {
		Map<Integer, CyberLink.Actor> live = new HashMap<>();
		for (CyberLink.Actor a : ACTORS) {
			if (!a.dead()) {
				live.put(a.actorId(), a);
			}
		}
		for (Iterator<Map.Entry<Integer, NpcEntity>> it = PROXIES.entrySet().iterator(); it.hasNext(); ) {
			Map.Entry<Integer, NpcEntity> e = it.next();
			NpcEntity proxy = e.getValue();
			if (!live.containsKey(e.getKey()) || proxy.isRemoved() || proxy.level() != level) {
				proxy.discard();
				it.remove();
			}
		}
		int before = PROXIES.size();
		for (CyberLink.Actor a : live.values()) {
			NpcEntity proxy = PROXIES.get(a.actorId());
			if (proxy == null) {
				proxy = new NpcEntity(NPC, level);
				proxy.setActorId(a.actorId());
				proxy.setSize(a.width(), a.height());
				proxy.snapTo(a.x(), a.y(), a.z(), a.yaw(), 0.0F);
				if (!a.name().isEmpty()) {
					proxy.setCustomName(Component.literal(a.name()));
				}
				if (!level.addFreshEntity(proxy)) {
					continue;
				}
				PROXIES.put(a.actorId(), proxy);
				continue;
			}
			proxy.setSize(a.width(), a.height());
			proxy.setPos(a.x(), a.y(), a.z());
			proxy.setYRot(a.yaw());
			proxy.setYHeadRot(a.yaw());
			stepOnTriggers(level, proxy);
		}
		if (PROXIES.size() != before && (PROXIES.size() % 5 == 0 || PROXIES.size() < 5)) {
			CyberCraft.LOG.info("CyberCraft: {} NPCs mirrored as hittable stand-ins", PROXIES.size());
		}
	}

	/**
	 * NPCs press pressure plates and trip tripwires. Their stand-ins are placed, not moved
	 * (no physics), so Minecraft never checks what they step into; do it for those blocks here.
	 */
	private static void stepOnTriggers(ServerLevel level, NpcEntity proxy) {
		var box = proxy.getBoundingBox().deflate(1.0E-5);
		var from = net.minecraft.core.BlockPos.containing(box.minX, box.minY, box.minZ);
		var to = net.minecraft.core.BlockPos.containing(box.maxX, box.maxY, box.maxZ);
		for (var pos : net.minecraft.core.BlockPos.betweenClosed(from, to)) {
			var state = level.getBlockState(pos);
			if (state.getBlock() instanceof net.minecraft.world.level.block.BasePressurePlateBlock
				|| state.getBlock() instanceof net.minecraft.world.level.block.TripWireBlock) {
				state.entityInside(level, pos, proxy, net.minecraft.world.entity.InsideBlockEffectApplier.NOOP, true);
			}
		}
	}

	/**
	 * Touch items and stuck arrows over a slightly bigger area than vanilla's, so walking past them
	 * on the city's uneven ground (stairs, curbs, slopes) picks them up.
	 * playerTouch applies all of Minecraft's own rules (pickup delay, owner, inventory space).
	 */
	private static void pickUpNearby(ServerPlayer player) {
		if (!player.isAlive() || player.isSpectator()) {
			return;
		}
		for (Entity entity : player.level().getEntities(player, player.getBoundingBox().inflate(1.25, 1.0, 1.25))) {
			if (!entity.isRemoved() && (entity instanceof net.minecraft.world.entity.item.ItemEntity
				|| entity instanceof net.minecraft.world.entity.projectile.arrow.AbstractArrow)) {
				entity.playerTouch(player);
			}
		}
	}

	private static void removeAll() {
		if (PROXIES.isEmpty()) {
			return;
		}
		PROXIES.values().forEach(Entity::discard);
		PROXIES.clear();
	}

	/**
	 * V got hurt. Runs on the server thread. {@code kind} is a Proto.HURT_* value and {@code damage}
	 * is already Minecraft damage: the plugin scales V's lost health by fDamageFromGame.
	 */
	public static void hurtPlayer(ServerPlayer player, int kind, float damage, int attackerId, int flags) {
		if (!player.isAlive() || damage <= 0.0F) {
			return;
		}
		ServerLevel level = player.level();
		NpcEntity attacker = PROXIES.get(attackerId);
		if (attacker != null && attacker.distanceToSqr(player) > 24.0 * 24.0) {
			attacker = null; // a guest's own NPC with the same actor id as one of the host's
		}
		DamageSources sources = level.damageSources();
		DamageSource source = switch (kind) {
			case Proto.HURT_MELEE -> attacker != null ? sources.mobAttack(attacker) : sources.generic();
			case Proto.HURT_PROJECTILE -> attacker != null ? sources.mobProjectile(attacker, attacker) : sources.generic();
			case Proto.HURT_MAGIC -> attacker != null ? sources.indirectMagic(attacker, attacker) : sources.magic();
			default -> sources.generic();
		};
		float healthBefore = player.getHealth();
		boolean hurt = player.hurtServer(level, source, damage);
		CyberCraft.LOG.info("CyberCraft: V got hurt for {} Minecraft damage: health {} -> {}{}", damage, healthBefore, player.getHealth(),
			hurt ? "" : " (blocked/immune)");
		if (hurt && attacker != null && (flags & Proto.HURT_POWER_ATTACK) != 0 && !player.isBlocking()) {
			// Power attacks shove harder, like a sprint hit does in Minecraft.
			player.knockback(0.5, attacker.getX() - player.getX(), attacker.getZ() - player.getZ(), source, damage);
		}
	}

	/** The actor id of the NPC behind a damage source, or 0. */
	public static int attackerId(DamageSource source) {
		return source.getEntity() instanceof NpcEntity proxy ? proxy.actorId() : 0;
	}
}
