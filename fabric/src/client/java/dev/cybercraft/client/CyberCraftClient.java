package dev.cybercraft.client;

import dev.cybercraft.combat.CyberCombat;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.client.rendering.v1.EntityRendererRegistry;
import net.minecraft.client.renderer.entity.NoopRenderer;

public final class CyberCraftClient implements ClientModInitializer {
	@Override
	public void onInitializeClient() {
		dev.cybercraft.link.CyberLink.announceRunning();
		DiscordPresence.start();
		DestructionToggle.register();
		// Multiplayer without editing files: the host's world opens to friends by itself (AutoHost)
		// and e4mc gives it a link; friends type /join <link> in chat (or press Join on Melty), and
		// /leave to come back.
		net.fabricmc.fabric.api.client.command.v2.ClientCommandRegistrationCallback.EVENT.register((dispatcher, context) -> {
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("join")
				.then(net.fabricmc.fabric.api.client.command.v2.ClientCommands.argument("link", com.mojang.brigadier.arguments.StringArgumentType.greedyString())
					.executes(c -> {
						String link = com.mojang.brigadier.arguments.StringArgumentType.getString(c, "link");
						c.getSource().sendFeedback(net.minecraft.network.chat.Component.literal("Joining " + link.trim() + "..."));
						// After the chat screen has closed: this leaves the current world.
						net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.joinFriend(net.minecraft.client.Minecraft.getInstance(), link));
						return 1;
					})));
			// Diagnostics: what Cyberpunk's physics has in the block looked at, and why Minecraft has it
			// solid or not, written to the plugin's log.
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("cityprobe").executes(c -> {
				net.minecraft.client.Minecraft minecraft = net.minecraft.client.Minecraft.getInstance();
				net.minecraft.core.BlockPos pos;
				if (minecraft.hitResult instanceof net.minecraft.world.phys.BlockHitResult block && block.getType() == net.minecraft.world.phys.HitResult.Type.BLOCK) {
					pos = block.getBlockPos();
				} else if (minecraft.player != null) {
					pos = net.minecraft.core.BlockPos.containing(minecraft.player.getEyePosition().add(minecraft.player.getLookAngle().scale(2.0)));
				} else {
					return 0;
				}
				dev.cybercraft.CyberCraft.LOG.info("CyberCraft: /cityprobe at {} {} {}: Minecraft has it {}% solid (city geometry {}, region known {})", pos.getX(), pos.getY(),
					pos.getZ(), Math.round(dev.cybercraft.world.CyberCollision.solidFraction(pos) * 100), dev.cybercraft.world.CyberCollision.hasGeometry(pos),
					dev.cybercraft.world.CyberCollision.isKnown(pos.getX(), pos.getY(), pos.getZ()));
				if (!dev.cybercraft.link.CyberLink.active()) {
					c.getSource().sendError(net.minecraft.network.chat.Component.literal("Cyberpunk isn't connected: nothing to ask."));
					return 0;
				}
				dev.cybercraft.link.CyberLink.pushEvent(dev.cybercraft.link.Proto.EV_CITY_PROBE, 0, pos.getX(), pos.getY(), pos.getZ(), 0, 0);
				c.getSource().sendFeedback(net.minecraft.network.chat.Component.literal(
					"Probed the city at " + pos.getX() + " " + pos.getY() + " " + pos.getZ() + ": what Cyberpunk has there is in CyberCraft.log."));
				return 1;
			}));
			dispatcher.register(net.fabricmc.fabric.api.client.command.v2.ClientCommands.literal("leave").executes(c -> {
				net.minecraft.client.Minecraft.getInstance().execute(() -> MirrorWorld.leaveFriend(net.minecraft.client.Minecraft.getInstance()));
				return 1;
			}));
		});
		ClientTickEvents.END_CLIENT_TICK.register(CyberClient::clientTick);
		// In a friend's world: Melty reads this line from the log to know the join worked. (Our own
		// world opens to friends by itself, AutoHost.)
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayConnectionEvents.JOIN.register((handler, sender, minecraft) -> {
			String friend = minecraft.getSingleplayerServer() == null ? MirrorWorld.friendAddress(minecraft) : null;
			if (friend != null) {
				dev.cybercraft.CyberCraft.LOG.info("CyberCraft: Melty joined: {}", friend);
			}
		});
		// A guest in a friend's world: dying there kills this player's own V.
		net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.registerGlobalReceiver(dev.cybercraft.net.CyberNet.Died.TYPE, (payload, context) -> {
			if (dev.cybercraft.link.CyberLink.active()) {
				dev.cybercraft.link.CyberLink.pushEvent(dev.cybercraft.link.Proto.EV_PLAYER_DIED, payload.attackerId(), 0, 0, 0, 0, 0);
			}
		});
		// Cyberpunk draws the real NPC; its Minecraft stand-in is only a hitbox.
		EntityRendererRegistry.register(CyberCombat.NPC, NoopRenderer::new);
		// Players (client-side movement AND the integrated server's re-check of it) use the smooth
		// triangle collider, never the voxels; otherwise the server sees the smooth position dip
		// into a voxel and teleports the player back every few ticks. Only once there are triangles
		// to collide with: the plugin sends voxels alone, and a player skipping those fell straight
		// through Night City.
		dev.cybercraft.world.CyberCollision.setSmoothCollider(
			e -> e instanceof net.minecraft.world.entity.player.Player && CyberClient.linked() && dev.cybercraft.world.CyberCollision.hasTriangles()
		);
	}
}
