package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import net.minecraft.client.Minecraft;

/**
 * Where a linked frame's time goes, logged every 10 s: Minecraft's side of the plugin's
 * "overlay: N Minecraft frames/s against M Cyberpunk frames/s" line. Render thread only.
 */
public final class FrameStats {
	public static final int TICK = 0;    // runTick() up to renderFrame(): packets, client ticks, input
	public static final int RENDER = 1;  // renderFrame() up to the end of GameRenderer.render()
	public static final int EXPORT = 2;  // afterRender(): blocks, entities, avatar, overlay readback
	public static final int GPU = 3;     // CommandEncoder.submit(): waits for the previous frame's GPU work
	public static final int PRESENT = 4; // the hidden window's swap
	public static final int PACE = 5;    // paceFrame(): waiting for Cyberpunk's next frame
	private static final String[] NAMES = { "tick", "render", "export", "gpu wait", "present", "waiting for Cyberpunk" };
	private static final long PERIOD_NANOS = 10_000_000_000L;

	private static final long[] since = new long[NAMES.length];
	private static final long[] sum = new long[NAMES.length];
	private static final long[] max = new long[NAMES.length];
	private static long periodStart;
	private static long frameStart;
	private static long frameMax;
	private static int frames;
	private static int screenFrames;
	private static int shipped;
	private static int dropped;
	private static long latencySum;
	private static long latencyMax;

	private FrameStats() {
	}

	public static void begin(int phase) {
		since[phase] = System.nanoTime();
	}

	public static void end(int phase) {
		if (since[phase] == 0) {
			return;
		}
		long nanos = System.nanoTime() - since[phase];
		since[phase] = 0;
		sum[phase] += nanos;
		max[phase] = Math.max(max[phase], nanos);
	}

	/** Start of Minecraft.runTick(): one rendered frame. */
	public static void frame() {
		long now = System.nanoTime();
		if (frameStart != 0) {
			frameMax = Math.max(frameMax, now - frameStart);
		}
		frameStart = now;
		if (periodStart == 0) {
			periodStart = now;
		} else if (now - periodStart >= PERIOD_NANOS) {
			if (CyberClient.linked() && frames > 0) {
				log((now - periodStart) / 1e9);
			}
			periodStart = now;
			frames = screenFrames = shipped = dropped = 0;
			latencySum = latencyMax = frameMax = 0;
			java.util.Arrays.fill(sum, 0);
			java.util.Arrays.fill(max, 0);
		}
		frames++;
		if (Minecraft.getInstance().gui.screen() != null) {
			screenFrames++;
		}
		begin(TICK);
	}

	/** An overlay frame reached shared memory, this long after it was captured. */
	public static void shipped(long latencyNanos) {
		shipped++;
		latencySum += latencyNanos;
		latencyMax = Math.max(latencyMax, latencyNanos);
	}

	/** A frame wasn't captured: every readback buffer was still waiting for the GPU. */
	public static void dropped() {
		dropped++;
	}

	private static void log(double seconds) {
		StringBuilder phases = new StringBuilder();
		for (int i = 0; i < NAMES.length; i++) {
			phases.append(i == 0 ? "" : ", ").append(NAMES[i]).append(' ').append(ms(sum[i] / frames)).append('/').append(ms(max[i]));
		}
		CyberCraft.LOG.info(
			"CyberCraft: {} frames/s, {} shipped to Cyberpunk ({} dropped, readback {} ms avg, {} max); ms per frame (avg/max): {}; longest frame {}; screen open {}%",
			String.format("%.0f", frames / seconds), String.format("%.0f", shipped / seconds), dropped,
			shipped > 0 ? ms(latencySum / shipped) : "-", ms(latencyMax), phases, ms(frameMax), screenFrames * 100 / frames
		);
	}

	private static String ms(long nanos) {
		return String.format("%.1f", nanos / 1e6);
	}
}
