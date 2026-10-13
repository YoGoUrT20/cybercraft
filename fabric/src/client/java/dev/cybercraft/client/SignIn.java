package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import net.minecraft.client.Minecraft;
import net.minecraft.network.chat.Component;
import org.jspecify.annotations.Nullable;

/**
 * Whether Minecraft is signed in to a Microsoft account. Prism starts it offline (or as the demo) when
 * it has no account or couldn't refresh one, and then passes "0" as the access token: the game runs,
 * but a friend's world turns it away (every CyberCraft world checks sign-ins) and Realms answers 401
 * at the title screen. Players were never told; now they're told once, in chat over Cyberpunk.
 */
public final class SignIn {
	private static @Nullable Boolean offline;
	private static boolean told;

	private SignIn() {
	}

	/** A signed-in Microsoft session's access token is a JWT; Prism's offline and demo launches pass "0". */
	public static boolean isSession(@Nullable String token) {
		return token != null && token.chars().filter(c -> c == '.').count() == 2;
	}

	/** True without a real session. */
	public static synchronized boolean offline() {
		if (offline == null) {
			Minecraft minecraft = Minecraft.getInstance();
			offline = !isSession(minecraft.getUser().getAccessToken());
			if (offline) {
				CyberCraft.LOG.warn("CyberCraft: Minecraft isn't signed in to a Microsoft account (the launcher started it {}); joining friends' worlds won't work",
					minecraft.isDemo() ? "as the demo" : "offline");
			}
		}
		return offline;
	}

	/** Every client tick: the note, once the player is in a world Cyberpunk shows. */
	public static void tick(Minecraft minecraft) {
		if (told || minecraft.player == null || !CyberClient.linked() || !offline()) {
			return;
		}
		told = true;
		minecraft.gui.hud.getChat().addClientSystemMessage(Component.literal(
			"Minecraft isn't signed in to your Microsoft account (Prism started it " + (minecraft.isDemo() ? "as the demo" : "offline")
				+ "). Your own world works, but you can't join friends' worlds. To sign in again, quit and press Play in Melty while online."));
	}

	/** Why joining a friend's world failed, when it was this (and then the note above isn't needed). */
	public static String joinFailedNote(String address) {
		told = true;
		return "Couldn't join " + address + ": Minecraft isn't signed in to your Microsoft account, and friends' worlds only let signed-in players in."
			+ " You're in your own world. To sign in, quit and press Play in Melty again while online.";
	}
}
