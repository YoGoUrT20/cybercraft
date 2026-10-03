package dev.cybercraft.client;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.buffers.GpuBuffer;
import com.mojang.renderpearl.api.buffers.GpuBufferSlice;
import com.mojang.renderpearl.api.textures.GpuTexture;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import java.lang.foreign.MemorySegment;
import net.minecraft.client.Minecraft;

/**
 * Copies Minecraft's main render target (HUD and screens, and the hand unless the plugin draws it,
 * on a transparent background) back from the GPU and publishes it to the plugin through the overlay
 * triple buffer.
 *
 * The copy is asynchronous: a frame is captured into one of a few staging buffers and shipped
 * once the GPU says the copy finished, typically a few milliseconds later (shipFinished).
 */
public final class FrameExporter {
	private static final int STAGING = 3;
	private static final int FREE = 0;
	private static final int PENDING = 1;
	private static final int READY = 2;

	private static final Staging[] staging = new Staging[STAGING];
	private static long nextFrameId = 1;
	private static boolean loggedFormat;

	private static final class Staging {
		GpuBuffer buffer;
		int width;
		int height;
		volatile int state = FREE;
		long frameId;
		long capturedNanos;
	}

	private FrameExporter() {
	}

	public static void capture(Minecraft minecraft) {
		shipReadyFrames();

		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		GpuTexture color = target.getColorTexture();
		if (color == null) {
			return;
		}
		int width = target.width;
		int height = target.height;
		if (width > Proto.MAX_OVERLAY_W || height > Proto.MAX_OVERLAY_H) {
			return;
		}
		if (!loggedFormat) {
			loggedFormat = true;
			CyberCraft.LOG.info("CyberCraft: overlay capture {}x{} format {}", width, height, color.getFormat());
		}

		Staging slot = null;
		for (Staging s : staging) {
			if (s != null && s.state == FREE) {
				slot = s;
				break;
			}
		}
		if (slot == null) {
			for (int i = 0; i < STAGING; i++) {
				if (staging[i] == null) {
					staging[i] = slot = new Staging();
					break;
				}
			}
		}
		if (slot == null) {
			FrameStats.dropped();
			return; // all staging buffers still in flight; skip this frame
		}

		long bytes = (long) width * height * 4L;
		if (slot.buffer == null || slot.width != width || slot.height != height) {
			if (slot.buffer != null) {
				slot.buffer.close();
			}
			slot.buffer = RenderSystem.getDevice().createBuffer(() -> "CyberCraft overlay readback", 9, bytes);
			slot.width = width;
			slot.height = height;
		}
		final Staging captured = slot;
		captured.state = PENDING;
		captured.frameId = nextFrameId++;
		captured.capturedNanos = System.nanoTime();
		RenderSystem.getDevice().createCommandEncoder().copyTextureToBuffer(color, captured.buffer, 0L, () -> captured.state = READY, 0);
	}

	/**
	 * Ships a capture as soon as its copy is done, rather than at the next capture, a whole frame
	 * later: Minecraft only checks its fences right before rendering. Called while
	 * CyberClient.paceFrame() waits for Cyberpunk's next frame.
	 */
	public static void shipFinished() {
		boolean inFlight = false;
		for (Staging s : staging) {
			inFlight |= s != null && s.state != FREE;
		}
		if (!inFlight) {
			return;
		}
		RenderSystem.executePendingTasks(); // runs the callbacks of finished copies: PENDING -> READY
		shipReadyFrames();
	}

	/** Maps the newest finished readback and copies it into shared memory. */
	private static void shipReadyFrames() {
		Staging newest = null;
		for (Staging s : staging) {
			if (s != null && s.state == READY && (newest == null || s.frameId > newest.frameId)) {
				newest = s;
			}
		}
		if (newest == null) {
			return;
		}
		MemorySegment shm = CyberLink.segment();
		if (shm != null) {
			long bytes = (long) newest.width * newest.height * 4L;
			try (GpuBufferSlice.MappedView view = newest.buffer.map(true, false)) {
				MemorySegment src = MemorySegment.ofBuffer(view.data());
				MemorySegment.copy(src, 0, shm, CyberLink.overlayBackSlotOffset(), Math.min(bytes, src.byteSize()));
			}
			CyberLink.publishOverlay(newest.width, newest.height, true, newest.frameId);
			FrameStats.shipped(System.nanoTime() - newest.capturedNanos);
		}
		// Anything older than what we just shipped is useless now.
		for (Staging s : staging) {
			if (s != null && s.state == READY && s.frameId <= newest.frameId) {
				s.state = FREE;
			}
		}
	}
}
