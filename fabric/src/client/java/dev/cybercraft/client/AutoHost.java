package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import net.minecraft.client.Minecraft;
import net.minecraft.client.server.IntegratedServer;
import net.minecraft.network.chat.Component;
import net.minecraft.server.MinecraftServer;
import net.minecraft.util.HttpUtil;

/**
 * Multiplayer without menus: our own world opens to friends as soon as it's loaded, so e4mc gives it
 * a link (DiscordPresence logs it, and Melty hands it to anyone pressing Join), unless "Open to
 * friends" is off in Cyberpunk's Mod Settings. Switched off while open, the world closes again.
 * A world the player opened themselves (O, Open to LAN) is theirs to close.
 *
 * <p>Testing on one PC: CYBERCRAFT_LAN_PORT picks the port (and opens the world without waiting for
 * Cyberpunk), and CYBERCRAFT_LAN_OFFLINE lets offline (dev) clients join it (tools/run_guest.ps1).
 */
public final class AutoHost {
	private static final String TEST_PORT = System.getenv("CYBERCRAFT_LAN_PORT");
	private static final long RETRY_MS = 30_000L;
	private static boolean ours;      // the world is open because we opened it
	private static boolean told;      // the "open to friends" note has been shown this session
	private static long nextTry;

	private AutoHost() {
	}

	/** Every client tick. */
	public static void tick(Minecraft minecraft) {
		IntegratedServer server = minecraft.getSingleplayerServer();
		if (server == null || minecraft.player == null) {
			ours = false;
			return;
		}
		boolean testing = TEST_PORT != null && !TEST_PORT.isBlank();
		if (!testing && !CyberClient.linked()) {
			return; // what Mod Settings says isn't known yet
		}
		boolean open = testing || !CyberClient.state().privateWorld();
		if (open && !server.isPublished() && System.currentTimeMillis() >= nextTry) {
			publish(minecraft, server, testing);
		} else if (!open && server.isPublished() && ours) {
			ours = false;
			boolean closed = server.unpublishServer();
			CyberCraft.LOG.info("CyberCraft: \"Open to friends\" is off; world closed to friends ({})", closed ? "ok" : "FAILED");
			minecraft.gui.hud.getChat().addClientSystemMessage(Component.literal("Your world is private now (Open to friends is off in Mod Settings)."));
		}
	}

	private static void publish(Minecraft minecraft, IntegratedServer server, boolean testing) {
		int port = testing ? Integer.parseInt(TEST_PORT.trim()) : HttpUtil.getAvailablePort();
		boolean offline = System.getenv("CYBERCRAFT_LAN_OFFLINE") != null;
		if (offline) {
			server.setUsesAuthentication(false);
		}
		boolean ok = server.publishServer(MinecraftServer.MultiplayerScope.LAN, false, port);
		CyberCraft.LOG.info("CyberCraft: opening the world to friends on port {} ({}{})", port, ok ? "ok" : "FAILED", offline ? ", offline logins allowed" : "");
		if (!ok) {
			nextTry = System.currentTimeMillis() + RETRY_MS;
			return;
		}
		ours = true;
		if (!told) {
			told = true;
			minecraft.gui.hud.getChat().addClientSystemMessage(Component.literal(
				"Your world is open to friends: they can join from CyberCraft's Melty page. To keep it private, turn off Open to friends in Mod Settings (CyberCraft)."));
		}
	}
}
