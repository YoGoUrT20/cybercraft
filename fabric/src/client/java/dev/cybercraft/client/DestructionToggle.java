package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.world.CyberCollision;
import dev.cybercraft.world.CyberDig;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import net.fabricmc.fabric.api.client.screen.v1.ScreenEvents;
import net.fabricmc.fabric.api.client.screen.v1.Screens;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.network.chat.Component;

/**
 * The pause menu's "City destruction" button (its top left corner, clear of the menu at any GUI
 * scale) and its setting, {@code destruction=} in config/cybercraft.properties (see CyberDig.destruction).
 * On until it's turned off. Only shown in the city (once there's collision from the plugin).
 */
public final class DestructionToggle {
	private static final String KEY = "destruction";

	private DestructionToggle() {
	}

	private static Path file() {
		return FabricLoader.getInstance().getConfigDir().resolve("cybercraft.properties");
	}

	public static void register() {
		load();
		ScreenEvents.AFTER_INIT.register((minecraft, screen, width, height) -> {
			if (screen instanceof PauseScreen pause && pause.showsPauseMenu() && minecraft.player != null && CyberCollision.active()) {
				Screens.getWidgets(screen).add(button(minecraft));
			}
		});
	}

	private static Button button(Minecraft minecraft) {
		// The host's server does all the digging, so in a friend's world it's their setting.
		boolean host = minecraft.hasSingleplayerServer();
		Button button = Button.builder(label(), b -> {
			CyberDig.destruction = !CyberDig.destruction;
			b.setMessage(label());
			save();
			CyberCraft.LOG.info("CyberCraft: city destruction {}", CyberDig.destruction ? "on" : "off");
		}).bounds(4, 4, 150, 20).tooltip(Tooltip.create(Component.literal(host
			? "Mining the city's streets and walls digs into them. Holes already dug stay either way."
			: "In a friend's world, their setting decides."))).build();
		button.active = host;
		return button;
	}

	private static Component label() {
		return Component.literal("City destruction: " + (CyberDig.destruction ? "On" : "Off"));
	}

	private static void load() {
		Properties props = new Properties();
		try (var in = Files.newBufferedReader(file())) {
			props.load(in);
		} catch (NoSuchFileException e) {
			return;
		} catch (IOException e) {
			CyberCraft.LOG.warn("CyberCraft: couldn't read {}", file(), e);
			return;
		}
		CyberDig.destruction = !"false".equalsIgnoreCase(props.getProperty(KEY, "true").trim());
	}

	/** Rewrites only its own line, keeping the file's comments and other settings (join=). */
	private static void save() {
		Path file = file();
		String line = KEY + "=" + CyberDig.destruction;
		try {
			List<String> lines = Files.exists(file) ? new ArrayList<>(Files.readAllLines(file)) : new ArrayList<>(List.of("# CyberCraft"));
			boolean found = false;
			for (int i = 0; i < lines.size(); i++) {
				if (lines.get(i).trim().startsWith(KEY + "=")) {
					lines.set(i, line);
					found = true;
				}
			}
			if (!found) {
				lines.add("# Mining digs into the city (the pause menu's \"City destruction\" button).");
				lines.add(line);
			}
			Files.createDirectories(file.getParent());
			Files.write(file, lines);
		} catch (IOException e) {
			CyberCraft.LOG.warn("CyberCraft: couldn't save {}", file, e);
		}
	}
}
