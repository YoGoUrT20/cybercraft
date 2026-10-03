package dev.cybercraft.client;

import com.mojang.blaze3d.platform.InputConstants;
import dev.cybercraft.combat.CyberCombat;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import it.unimi.dsi.fastutil.ints.IntArrayList;
import java.util.Arrays;
import net.fabricmc.fabric.api.client.keymapping.v1.KeyMappingHelper;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Options;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.options.OptionsScreen;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonInfo;
import org.lwjgl.sdl.SDLKeyboard;

/**
 * Replays input captured in Cyberpunk into Minecraft's own input handlers, as if the (hidden) MC
 * window had focus. Keeps a virtual keyboard so InputConstants.isKeyDown() still works.
 */
public final class InputBridge {
	private static final boolean[] KEYS = new boolean[512];
	private static final boolean[] BUTTONS = new boolean[8];
	private static double cursorX, cursorY;
	private static int modifiers;
	private static int clickLogs;

	private InputBridge() {
	}

	public static boolean isKeyDown(int scancode) {
		return scancode >= 0 && scancode < KEYS.length && KEYS[scancode];
	}

	public static void drain(Minecraft minecraft) {
		CyberLink.drainInput((type, code, a, b, c) -> dispatch(minecraft, type, code, a, b, c));
	}

	private static void dispatch(Minecraft minecraft, int type, int code, int a, int b, int c) {
		long handle = minecraft.getWindow().handle();
		switch (type) {
			case Proto.IN_KEY -> key(minecraft, handle, code, a != 0);
			case Proto.IN_MOUSE_BUTTON -> {
				if (code > 0 && code < BUTTONS.length) {
					BUTTONS[code] = a != 0;
				}
				if (a != 0 && clickLogs++ < 20) {
					var hit = minecraft.hitResult;
					dev.cybercraft.CyberCraft.LOG.info("CyberCraft: click {} -> {} {} (grabbed {}, screen {})", code, hit == null ? "null" : hit.getType(),
						hit instanceof net.minecraft.world.phys.EntityHitResult eh ? eh.getEntity().getName().getString() : hit == null ? "" : hit.getLocation(),
						minecraft.mouseHandler.isMouseGrabbed(), minecraft.gui.screen());
				}
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(code, modifiers), a != 0 ? 1 : 0);
			}
			case Proto.IN_SCROLL -> minecraft.mouseHandler.onScroll(handle, 0.0, a / 120.0);
			case Proto.IN_CURSOR -> {
				double dx = a - cursorX;
				double dy = b - cursorY;
				cursorX = a;
				cursorY = b;
				minecraft.mouseHandler.onMove(handle, a, b, dx, dy);
			}
			case Proto.IN_TEXT -> {
				if (minecraft.gui.screen() != null) {
					minecraft.keyboardHandler.textInput(handle, new String(Character.toChars(a)));
				}
			}
			case Proto.IN_RELEASE_ALL -> releaseAll();
			case Proto.IN_HURT -> hurt(minecraft, code, a / 100.0F, b, c);
			case Proto.IN_OPEN_MENU -> {
				if (code == Proto.MENU_OPTIONS) {
					// Cyberpunk's Mod Settings: Done goes back to whatever was open, usually nothing.
					if (minecraft.player != null && !(minecraft.gui.screen() instanceof OptionsScreen)) {
						releaseAll();
						minecraft.gui.setScreen(new OptionsScreen(minecraft.gui.screen(), minecraft.options));
					}
				} else if (minecraft.gui.screen() == null && minecraft.player != null) {
					releaseAll();
					minecraft.gui.setScreen(new PauseScreen(true));
				}
			}
			default -> {
			}
		}
	}

	/** V got hurt: apply it as Minecraft damage on the integrated server (or the host's). */
	private static void hurt(Minecraft minecraft, int kind, float damage, int attacker, int flags) {
		var server = minecraft.getSingleplayerServer();
		if (minecraft.player == null) {
			return;
		}
		if (server == null) {
			// A guest in a friend's world: the host's server applies it.
			if (net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.canSend(dev.cybercraft.net.CyberNet.Hurt.TYPE)) {
				net.fabricmc.fabric.api.client.networking.v1.ClientPlayNetworking.send(new dev.cybercraft.net.CyberNet.Hurt(kind, damage, attacker, flags));
			}
			return;
		}
		var uuid = minecraft.player.getUUID();
		server.execute(() -> {
			ServerPlayer player = server.getPlayerList().getPlayer(uuid);
			if (player != null) {
				CyberCombat.hurtPlayer(player, kind, damage, attacker, flags);
			}
		});
	}

	private static void key(Minecraft minecraft, long handle, int scancode, boolean down) {
		if (scancode <= 0 || scancode >= KEYS.length) {
			return;
		}
		boolean wasDown = KEYS[scancode];
		KEYS[scancode] = down;
		updateModifiers();
		int action = down ? (wasDown ? -1 : 1) : 0; // -1 = repeat
		int keycode = SDLKeyboard.SDL_GetKeyFromScancode(scancode, (short) modifiers, true);
		minecraft.keyboardHandler.keyPress(handle, action, new KeyEvent(scancode, keycode, modifiers));
	}

	private static void updateModifiers() {
		int m = 0;
		if (KEYS[225]) m |= 0x0001; // SDL_KMOD_LSHIFT
		if (KEYS[229]) m |= 0x0002; // SDL_KMOD_RSHIFT
		if (KEYS[224]) m |= 0x0040; // SDL_KMOD_LCTRL
		if (KEYS[228]) m |= 0x0080; // SDL_KMOD_RCTRL
		if (KEYS[226]) m |= 0x0100; // SDL_KMOD_LALT
		if (KEYS[230]) m |= 0x0200; // SDL_KMOD_RALT
		modifiers = m;
	}

	// The keys Minecraft has a control on, so the host leaves those to Minecraft and gives
	// Cyberpunk the rest (V calls the car, M opens the map, J the journal, ...). Worked out twice a
	// second, since rebinding is rare; the F3 combinations count only while F3 is held.
	private static final int[] BOUND = new int[8];
	private static final int[] BOUND_DEBUG = new int[8];
	private static final IntArrayList DEBUG_MODIFIER = new IntArrayList();
	private static long nextBoundNanos;

	/** Bit n of out[n / 32]: SDL scancode n has a Minecraft control right now. */
	public static void boundKeys(Minecraft minecraft, int[] out) {
		long now = System.nanoTime();
		if (now - nextBoundNanos >= 0) {
			nextBoundNanos = now + 500_000_000L;
			findBoundKeys(minecraft.options);
		}
		boolean debug = false;
		for (int i = 0; i < DEBUG_MODIFIER.size(); i++) {
			debug |= isKeyDown(DEBUG_MODIFIER.getInt(i));
		}
		for (int i = 0; i < out.length; i++) {
			out[i] = BOUND[i] | (debug ? BOUND_DEBUG[i] : 0);
		}
	}

	private static void findBoundKeys(Options options) {
		// A mapping's key is an SDL scancode, the code the host sends (KeyMapping matches KeyEvent.key()).
		Arrays.fill(BOUND, 0);
		Arrays.fill(BOUND_DEBUG, 0);
		DEBUG_MODIFIER.clear();
		for (KeyMapping mapping : options.keyMappings) {
			InputConstants.Key key = KeyMappingHelper.getBoundKeyOf(mapping);
			int sc = key.getValue();
			if (mapping.isUnbound() || key.getType() != InputConstants.Type.KEYBOARD || sc <= 0 || sc >= 256) {
				continue;
			}
			if (mapping == options.keyDebugModifier) {
				DEBUG_MODIFIER.add(sc);
			}
			boolean combination = mapping.getCategory() == KeyMapping.Category.DEBUG && mapping != options.keyDebugOverlay
				&& mapping != options.keyDebugModifier;
			(combination ? BOUND_DEBUG : BOUND)[sc >> 5] |= 1 << (sc & 31);
		}
	}

	/** Lift every key and button we think is held (focus moved to Cyberpunk, link dropped, ...). */
	public static void releaseAll() {
		Minecraft minecraft = Minecraft.getInstance();
		long handle = minecraft.getWindow().handle();
		for (int sc = 0; sc < KEYS.length; sc++) {
			if (KEYS[sc]) {
				KEYS[sc] = false;
				updateModifiers();
				minecraft.keyboardHandler.keyPress(handle, 0, new KeyEvent(sc, SDLKeyboard.SDL_GetKeyFromScancode(sc, (short) 0, true), modifiers));
			}
		}
		for (int button = 1; button < BUTTONS.length; button++) {
			if (BUTTONS[button]) {
				BUTTONS[button] = false;
				minecraft.mouseHandler.onButton(handle, new MouseButtonInfo(button, 0), 0);
			}
		}
	}
}
