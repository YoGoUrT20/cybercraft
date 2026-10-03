package dev.cybercraft.world;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.MinecraftServer;
import net.minecraft.world.clock.ServerClockManager;
import net.minecraft.world.clock.WorldClock;
import net.minecraft.world.clock.WorldClocks;
import net.minecraft.world.level.saveddata.WeatherData;

/**
 * Night City's clock and weather in Minecraft, and Minecraft's /time and /weather in Night City.
 *
 * <p>The plugin sends the hour and weather in CyberState, and the integrated server sets Minecraft's
 * overworld clock (the one the mirror dimension uses) and weather to match. Neither advances on its
 * own (CyberCraft.configureServer turns that off), so a change made by anything else came from /time,
 * /weather or another mod: it goes to Cyberpunk instead, and Cyberpunk's value isn't pushed back
 * until Cyberpunk has followed it (or a few seconds have passed and it hasn't).
 *
 * <p>In a shared world only the host's game drives this: guests' own games keep their own time.
 */
public final class CyberClock {
	private CyberClock() {
	}

	private static final CyberLink.CyberState STATE = new CyberLink.CyberState();
	// A day is 24000 ticks: 10 is 36 seconds of Night City's day, too little to be worth a time packet.
	private static final int MIN_STEP = 10;
	private static final long TIME_HOLD_MS = 5_000;
	// Weather blends in over seconds in Cyberpunk, so it gets longer to follow.
	private static final long WEATHER_HOLD_MS = 30_000;

	private static long appliedTicks = Long.MIN_VALUE;
	private static int appliedWeather = Proto.WEATHER_UNKNOWN;
	private static float requestedHour = Float.NaN;
	private static long timeHeldUntil;
	private static int requestedWeather = Proto.WEATHER_UNKNOWN;
	private static long weatherHeldUntil;

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(CyberClock::serverTick);
		ServerLifecycleEvents.SERVER_STOPPED.register(server -> reset());
	}

	private static void reset() {
		appliedTicks = Long.MIN_VALUE;
		appliedWeather = Proto.WEATHER_UNKNOWN;
		requestedHour = Float.NaN;
		requestedWeather = Proto.WEATHER_UNKNOWN;
	}

	private static void serverTick(MinecraftServer server) {
		if (!CyberLink.active()) {
			reset();
			return;
		}
		if (!CyberLink.readCyberState(STATE) || !STATE.inGame()) {
			return;
		}
		long now = System.currentTimeMillis();
		syncTime(server, now);
		syncWeather(server, now);
	}

	private static void syncTime(MinecraftServer server, long now) {
		Holder<WorldClock> clock = server.registryAccess().lookupOrThrow(Registries.WORLD_CLOCK).getOrThrow(WorldClocks.OVERWORLD);
		ServerClockManager clocks = server.clockManager();
		long ticks = clocks.getInstance(clock).totalTicks();
		if (appliedTicks != Long.MIN_VALUE && ticks != appliedTicks) {
			// /time set day, /time add 6000, ...: Night City's clock goes there too.
			appliedTicks = ticks;
			requestedHour = hourOf(ticks);
			timeHeldUntil = now + TIME_HOLD_MS;
			CyberLink.pushEvent(Proto.EV_SET_TIME, 0, requestedHour, 0.0F, 0.0F, 0.0F, 0);
			CyberCraft.LOG.info("CyberCraft: time changed in Minecraft; asking Cyberpunk for {}:{}", (int) requestedHour,
				String.format("%02d", (int) ((requestedHour % 1.0F) * 60.0F)));
			return;
		}
		if (!STATE.clock()) {
			appliedTicks = ticks;
			return;
		}
		if (!Float.isNaN(requestedHour)) {
			if (now < timeHeldUntil && hourDistance(STATE.gameHour, requestedHour) > 0.1F) {
				appliedTicks = ticks;
				return;
			}
			requestedHour = Float.NaN;
		}
		long delta = Math.floorMod(ticksOfDay(STATE.gameHour) - ticks, 24000L);
		if (delta > 12000L) {
			delta -= 24000L;
		}
		if (appliedTicks == Long.MIN_VALUE || Math.abs(delta) >= MIN_STEP) {
			long target = ticks + delta;
			if (target < 0) {
				target += 24000L;
			}
			clocks.setTotalTicks(clock, target);
			appliedTicks = target;
		} else {
			appliedTicks = ticks;
		}
	}

	private static void syncWeather(MinecraftServer server, long now) {
		WeatherData data = server.getWeatherData();
		int current = !data.isRaining() ? Proto.WEATHER_CLEAR : data.isThundering() ? Proto.WEATHER_THUNDER : Proto.WEATHER_RAIN;
		if (appliedWeather != Proto.WEATHER_UNKNOWN && current != appliedWeather) {
			// /weather rain, /weather clear, ...: Night City's weather changes to it.
			appliedWeather = current;
			requestedWeather = current;
			weatherHeldUntil = now + WEATHER_HOLD_MS;
			CyberLink.pushEvent(Proto.EV_SET_WEATHER, current, 0.0F, 0.0F, 0.0F, 0.0F, 0);
			CyberCraft.LOG.info("CyberCraft: weather changed in Minecraft; asking Cyberpunk for {}", name(current));
			return;
		}
		int city = STATE.weather;
		if (city < Proto.WEATHER_CLEAR || city > Proto.WEATHER_THUNDER) {
			appliedWeather = current;
			return;
		}
		if (requestedWeather != Proto.WEATHER_UNKNOWN) {
			if (now < weatherHeldUntil && city != requestedWeather) {
				appliedWeather = current;
				return;
			}
			requestedWeather = Proto.WEATHER_UNKNOWN;
		}
		if (city != current) {
			boolean clear = city == Proto.WEATHER_CLEAR;
			server.setWeatherParameters(clear ? 12000 : 0, clear ? 0 : 12000, !clear, city == Proto.WEATHER_THUNDER);
			CyberCraft.LOG.info("CyberCraft: Night City's weather is {} now", name(city));
		}
		appliedWeather = city;
	}

	/** Minecraft's day starts at 6:00: tick 0 is sunrise, 6000 noon, 18000 midnight. */
	private static long ticksOfDay(float hour) {
		return Math.floorMod(Math.round((hour - 6.0F) * 1000.0F), 24000L);
	}

	private static float hourOf(long ticks) {
		return (Math.floorMod(ticks, 24000L) / 1000.0F + 6.0F) % 24.0F;
	}

	private static float hourDistance(float a, float b) {
		float d = Math.abs(a - b) % 24.0F;
		return Math.min(d, 24.0F - d);
	}

	private static String name(int weather) {
		return switch (weather) {
			case Proto.WEATHER_CLEAR -> "clear";
			case Proto.WEATHER_RAIN -> "rain";
			case Proto.WEATHER_THUNDER -> "thunder";
			default -> "unknown";
		};
	}
}
