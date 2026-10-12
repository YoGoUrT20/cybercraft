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
