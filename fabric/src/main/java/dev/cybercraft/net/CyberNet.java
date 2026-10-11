package dev.cybercraft.net;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.combat.CyberCombat;
import dev.cybercraft.world.CyberDig;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;

/**
 * Multiplayer: every player has their own Cyberpunk, talking to their own Minecraft client. The
 * host's Cyberpunk reaches the host's integrated server through shared memory; a guest's Cyberpunk
 * reaches the host's server through these packets instead.
 */
public final class CyberNet {
	private CyberNet() {
	}

	/** Guest -> server: the guest's V got hurt (as proto::InputEvent kInHurt, in Minecraft damage). */
	public record Hurt(int kind, float damage, int attackerId, int flags) implements CustomPacketPayload {
		public static final Type<Hurt> TYPE = new Type<>(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "hurt"));
		public static final StreamCodec<RegistryFriendlyByteBuf, Hurt> CODEC = StreamCodec.composite(
			ByteBufCodecs.VAR_INT, Hurt::kind,
			ByteBufCodecs.FLOAT, Hurt::damage,
			ByteBufCodecs.INT, Hurt::attackerId,
			ByteBufCodecs.VAR_INT, Hurt::flags,
			Hurt::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Server -> guest: the guest died in Minecraft, so their V dies too. */
	public record Died(int attackerId) implements CustomPacketPayload {
		public static final Type<Died> TYPE = new Type<>(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "died"));
		public static final StreamCodec<RegistryFriendlyByteBuf, Died> CODEC = StreamCodec.composite(ByteBufCodecs.INT, Died::attackerId, Died::new);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/**
	 * Client -> server: the player mined the host's geometry out of this block (CyberDig.open);
	 * {@code surface}: the height of the surface mined from above, NaN for a wall.
	 */
	public record DigOpen(int world, BlockPos pos, int material, float surface) implements CustomPacketPayload {
		public static final Type<DigOpen> TYPE = new Type<>(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "dig_open"));
		public static final StreamCodec<RegistryFriendlyByteBuf, DigOpen> CODEC = StreamCodec.composite(
			ByteBufCodecs.INT, DigOpen::world,
			BlockPos.STREAM_CODEC, DigOpen::pos,
			ByteBufCodecs.VAR_INT, DigOpen::material,
			ByteBufCodecs.FLOAT, DigOpen::surface,
			DigOpen::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: cells around a broken dug block that are inside the host's geometry (CyberDig.reveal). */
	public record DigReveal(int world, List<BlockPos> cells, List<Integer> materials) implements CustomPacketPayload {
		public static final Type<DigReveal> TYPE = new Type<>(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "dig_reveal"));
		public static final StreamCodec<RegistryFriendlyByteBuf, DigReveal> CODEC = StreamCodec.composite(
			ByteBufCodecs.INT, DigReveal::world,
			BlockPos.STREAM_CODEC.apply(ByteBufCodecs.list(64)), DigReveal::cells,
			ByteBufCodecs.VAR_INT.apply(ByteBufCodecs.list(64)), DigReveal::materials,
			DigReveal::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: the middle button on the city: pick the block of this Proto.DIG_* material. */
	public record PickCity(int material) implements CustomPacketPayload {
		public static final Type<PickCity> TYPE = new Type<>(Identifier.fromNamespaceAndPath(CyberCraft.MOD_ID, "pick_city"));
		public static final StreamCodec<RegistryFriendlyByteBuf, PickCity> CODEC = StreamCodec.composite(
			ByteBufCodecs.VAR_INT, PickCity::material,
			PickCity::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	public static void init() {
		PayloadTypeRegistry.serverboundPlay().register(Hurt.TYPE, Hurt.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(DigOpen.TYPE, DigOpen.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(DigReveal.TYPE, DigReveal.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(PickCity.TYPE, PickCity.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(PickCity.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			var stack = new net.minecraft.world.item.ItemStack(CyberDig.materialState(payload.material()).getBlock());
			// As vanilla picks a block: from the inventory, or given with infinite materials.
			context.server().execute(() -> ((dev.cybercraft.mixin.ServerGamePacketListenerAccessor) player.connection).cybercraft$tryPickItem(stack));
		});
		ServerPlayNetworking.registerGlobalReceiver(DigOpen.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> CyberDig.open(player, payload.world(), payload.pos(), payload.material(), payload.surface()));
		});
		ServerPlayNetworking.registerGlobalReceiver(DigReveal.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			int[] materials = payload.materials().stream().mapToInt(Integer::intValue).toArray();
			context.server().execute(() -> CyberDig.reveal(player, payload.world(), payload.cells(), materials));
		});
		PayloadTypeRegistry.clientboundPlay().register(Died.TYPE, Died.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(Hurt.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			// A hit's worth of damage, whatever the guest's client claims (friends only, but still).
			float damage = Math.max(0.0F, Math.min(payload.damage(), 10000.0F));
			context.server().execute(() -> CyberCombat.hurtPlayer(player, payload.kind(), damage, payload.attackerId(), payload.flags()));
		});
	}

	/** True if this player plays on this machine (their Cyberpunk is on the shared-memory link). */
	public static boolean isHost(ServerPlayer player) {
		var server = player.level().getServer();
		return server != null && server.isSingleplayerOwner(player.nameAndId());
	}
}
