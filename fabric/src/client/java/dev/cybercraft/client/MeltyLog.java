package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import org.jspecify.annotations.Nullable;

/**
 * %LOCALAPPDATA%\CyberCraft\melty.log: where Melty reads how playing together is going, in its own
 * words. Hosting: "Melty join address: abc-def.e4mc.link" once the world is open to friends (Melty
 * hands that to joiners). Joining: "Melty joined: abc-def.e4mc.link" once in the host's world.
 *
 * <p>Minecraft's latest.log was where Melty looked before, and it often didn't find the line there:
 * the log is replaced at every start, and the mod's own logging pushes the line far from its end.
 * This file only ever holds these lines, and starts empty with each Minecraft, so an address from a
 * world that has since closed is never handed out.
 */
public final class MeltyLog {
	private static @Nullable Path file;
	private static @Nullable String lastHosted;
	private static @Nullable String lastJoined;

	private MeltyLog() {
	}

	private static @Nullable Path file() {
		if (file == null) {
			String local = System.getenv("LOCALAPPDATA");
			if (local == null || local.isBlank()) {
				return null;
			}
			file = Path.of(local, "CyberCraft", "melty.log");
		}
		return file;
	}

	/** At Minecraft's start: a fresh file, nothing hosted or joined yet. */
	public static void start() {
		Path path = file();
		if (path == null) {
			return;
		}
		try {
			Files.createDirectories(path.getParent());
			Files.writeString(path, "", StandardCharsets.UTF_8);
		} catch (IOException e) {
			CyberCraft.LOG.warn("CyberCraft: couldn't start {}", path, e);
		}
	}

	/** Our world is open to friends at this address (said again now and then while it stays open). */
	public static void hosting(String address) {
		lastJoined = null;
		lastHosted = address;
		write("Melty join address: " + address);
	}

	/** In a friend's world, joined at this address. */
	public static void joined(String address) {
		if (address.equals(lastJoined)) {
			return;
		}
		lastJoined = address;
		lastHosted = null;
		write("Melty joined: " + address);
	}

	/** Our world isn't open any more (closed to friends, or we left it). */
	public static void closed() {
		if (lastHosted != null) {
			lastHosted = null;
			start();  // emptied: Melty hands out no address while there's none to join
		}
	}

	private static void write(String line) {
		Path path = file();
		if (path == null) {
			return;
		}
		try {
			Files.writeString(path, line + System.lineSeparator(), StandardCharsets.UTF_8, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
		} catch (IOException e) {
			CyberCraft.LOG.warn("CyberCraft: couldn't write {}", path, e);
		}
	}
}
