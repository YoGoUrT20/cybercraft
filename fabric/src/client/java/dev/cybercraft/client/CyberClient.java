package dev.cybercraft.client;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.client.render.WorldExporter;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import dev.cybercraft.world.CyberCollision;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.phys.Vec3;
import org.lwjgl.sdl.SDLVideo;

/**
 * Per-frame glue between the Minecraft client and Cyberpunk. Everything here runs on the render
 * thread, called from MinecraftMixin.
 */
public final class CyberClient {
	private static final boolean SHOW_WINDOW = Boolean.getBoolean("cybercraft.showWindow");
	// Started by the plugin (CyberCraft's bundled instance passes -Dcybercraft.startHidden=true): no
	// window and no title-screen music from the first frame, even before the two have linked up.
	// Otherwise the window only goes once Cyberpunk is there.
	private static final boolean START_HIDDEN = Boolean.getBoolean("cybercraft.startHidden");
	private static boolean startedHidden;

	private static final CyberLink.CyberState state = new CyberLink.CyberState();
	private static final CyberLink.McState mc = new CyberLink.McState();
	private static volatile boolean linked;
	private static boolean tookOver;
	private static boolean windowHidden;
	private static int appliedViewportW, appliedViewportH;

	// Teleport / hold state: Cyberpunk decides where the player is after loads, car rides and respawns.
	private static int lastTeleportSeq = -1;
	private static int teleportAck;
	private static boolean teleportPending;
	private static LocalPlayer lastPlayer;
	private static Vec3 holdPos;
	private static Vec3 unlinkedHold;
	private static long holdSince;
	private static long qpcFreq;
	private static LocalPlayer eyePlayer;
	private static float eyeSmoothed;
	private static long frameCounter;
	private static int lastPacedSeq;
	private static boolean cyberpunkStalled;
	// Turned off in Cyberpunk's Mod Settings: paused for it, with this pause screen (null if one was
	// already up).
	private static boolean pausedForDisabled;
	private static Screen disabledPause;
	// paceFrame(): when the next physics tick is due, and when Cyberpunk's next frame should be.
	private static final long TICK_WAKE_SLACK_NANOS = 1_500_000L; // DeltaTracker counts whole milliseconds
	private static long nextTickNanos;
	private static boolean tickWakeArmed;
	private static long lastCyberFrameNanos;
	private static long cyberFrameNanos = 16_666_667L;
	private static int exporterErrors;

	private CyberClient() {
	}

	public static boolean linked() {
		return linked;
	}

	/**
	 * True once Cyberpunk has connected in this session. From then on Minecraft never touches the
	 * real mouse or keyboard again (even if Cyberpunk closes), since its window is hidden.
	 */
	public static boolean tookOver() {
		return tookOver;
	}

	public static CyberLink.CyberState state() {
		return state;
	}

	/** True once the game window is hidden for good: nothing needs drawing into it any more. */
	public static boolean windowHidden() {
		return windowHidden;
	}

	/** Start of Minecraft.runTick: pull state and input from Cyberpunk before anything else runs. */
	public static void beginFrame() {
		CyberLink.poll();
		quitWithCyberpunk(Minecraft.getInstance());
		if (START_HIDDEN && !startedHidden) {
			startedHidden = true;
			Minecraft minecraft = Minecraft.getInstance();
			hideWindowOnce(minecraft);
			minecraft.options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
			minecraft.getMusicManager().stopPlaying();
		}
		boolean nowLinked = CyberLink.active();
		if (nowLinked) {
			CyberLink.readCyberState(state); // on a torn read we simply keep last frame's state
			dev.cybercraft.world.CyberWater.refresh();
		} else {
			dev.cybercraft.world.CyberWater.clear();
		}
		if (nowLinked != linked) {
			linked = nowLinked;
			CyberCraft.LOG.info("CyberCraft: Cyberpunk link {}", linked ? "up" : "down");
			if (linked) {
				tookOver = true;
				unlinkedHold = null;
				CyberCollision.startConsumer();
				applyLinkedOptions();
			} else {
				InputBridge.releaseAll();
				LocalPlayer player = Minecraft.getInstance().player;
				unlinkedHold = player != null ? player.position() : null;
			}
		}
		if (!linked) {
			return;
		}

		Minecraft minecraft = Minecraft.getInstance();
		hideWindowOnce(minecraft);
		applyViewportSize(minecraft);
		MirrorWorld.openWhenReady(minecraft);
		pauseWhileDisabled(minecraft);

		if (state.menuOpen() || state.loading()) {
			InputBridge.releaseAll();
		}
		InputBridge.drain(minecraft);
		ProxySync.frame(minecraft);

		LocalPlayer player = minecraft.player;
		if (player == null) {
			lastPlayer = null;
			return;
		}

		// A new player object means we just joined or respawned: put it where V is.
		if (player != lastPlayer) {
			lastPlayer = player;
			teleportPending = true;
		}
		if (state.teleportSeq != lastTeleportSeq) {
			lastTeleportSeq = state.teleportSeq;
			teleportPending = true;
		}
		if (teleportPending && state.inGame() && !state.loading()) {
			requestTeleport(minecraft, state.x, state.y, state.z, state.yaw, state.pitch);
			teleportAck = state.teleportSeq;
			teleportPending = false;
			holdPos = new Vec3(state.x, state.y, state.z);
		}

		// Look direction is driven by Cyberpunk (zero-latency camera); MC uses it for everything else.
		if (minecraft.gui.screen() == null) {
			player.setYRot(state.yaw);
			player.setXRot(state.pitch);
			player.yRotO = state.yaw;
			player.xRotO = state.pitch;
		}
	}

	// Minecraft is started with Cyberpunk (the plugin launches it, through Explorer rather than as a
	// child process), so it goes by itself when that Cyberpunk has closed for good: saved and shut down
	// the normal way. -Dcybercraft.quitWithCyberpunk=false keeps it running instead (development:
	// restarting Cyberpunk without restarting Minecraft).
	private static final boolean QUIT_WITH_CYBERPUNK = Boolean.parseBoolean(System.getProperty("cybercraft.quitWithCyberpunk", "true"));
	private static long cyberpunkGoneSince;
	private static long nextCyberpunkCheck;
	// Started hidden by the plugin but never connected: nobody can see or use this Minecraft, and it
	// would stop the next Cyberpunk from starting a fresh one ("already running"). It goes after this.
	private static final long NEVER_CONNECTED_QUIT_MS = 10 * 60 * 1000;
	private static final long STARTED_AT = System.currentTimeMillis();
	private static boolean gaveUpWaiting;

	private static void quitWithCyberpunk(Minecraft minecraft) {
		int pid = CyberLink.cyberpunkPid();
		long now = System.currentTimeMillis();
		if (QUIT_WITH_CYBERPUNK && START_HIDDEN && pid == 0 && !tookOver && !gaveUpWaiting && now - STARTED_AT > NEVER_CONNECTED_QUIT_MS) {
			gaveUpWaiting = true;
			CyberCraft.LOG.warn("CyberCraft: started hidden but Cyberpunk never connected in {} minutes; quitting", NEVER_CONNECTED_QUIT_MS / 60000);
			minecraft.stop();
			return;
		}
		if (!QUIT_WITH_CYBERPUNK || pid == 0 || now < nextCyberpunkCheck) {
			return;
		}
		nextCyberpunkCheck = now + 1000;
		if (ProcessHandle.of(pid).map(ProcessHandle::isAlive).orElse(false)) {
			cyberpunkGoneSince = 0;
			return;
		}
		if (cyberpunkGoneSince == 0) {
			cyberpunkGoneSince = now;
		} else if (now - cyberpunkGoneSince > 5000) {
			CyberCraft.LOG.info("CyberCraft: Cyberpunk (pid {}) has closed; saving and quitting", pid);
			minecraft.stop();
		}
	}

	/** Called at the end of every client tick. */
	public static void clientTick(Minecraft minecraft) {
		MirrorWorld.tick(minecraft);
		DiscordPresence.tick(minecraft);
		CyberDigClient.tick(minecraft);
		freezeWhileUnlinked(minecraft);
		holdUntilReady(minecraft);
		if (linked) {
			VehiclePush.tick(minecraft); // before publishTick: the plugin moves V where the push put the player
		}
		publishTick(minecraft);
	}

	/**
	 * Cyberpunk went quiet (a stall, or it closed). No collision arrives meanwhile, so keep the player
	 * exactly where they were instead of letting them fall, until Cyberpunk is back (a new one puts
	 * them where V is, as after any load).
	 */
	private static void freezeWhileUnlinked(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (linked || !tookOver || player == null) {
			return;
		}
		if (unlinkedHold == null) {
			unlinkedHold = player.position();
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(unlinkedHold.x, unlinkedHold.y, unlinkedHold.z);
		player.xo = unlinkedHold.x;
		player.yo = unlinkedHold.y;
		player.zo = unlinkedHold.z;
		player.resetFallDistance();
	}

	/**
	 * Hands the plugin the raw physics tick (previous + latest feet, smoothed eye height, walk bob) with
	 * a QueryPerformanceCounter timestamp. It interpolates between them on Cyberpunk's own frame clock,
	 * exactly like Minecraft's renderer does with partial ticks.
	 */
	private static void publishTick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (qpcFreq == 0) {
			qpcFreq = CyberLink.qpcFrequency();
		}
		float tickMs = minecraft.level != null ? minecraft.level.tickRateManager().millisecondsPerTick() : 50.0F;
		// The tick really "happened" partial ticks ago (DeltaTracker keeps the remainder).
		float remainder = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
		long qpcNow = CyberLink.qpc();
		mc.tickQpc = qpcNow - (long) (remainder * tickMs * qpcFreq / 1000.0);
		mc.tickMs = tickMs;
		// When the next one is due: paceFrame() wakes for it rather than leave it to Cyberpunk's next frame.
		long nextTickQpc = mc.tickQpc + (long) (tickMs * qpcFreq / 1000.0);
		nextTickNanos = System.nanoTime() + (nextTickQpc - qpcNow) * 1_000_000_000L / qpcFreq;
		tickWakeArmed = true;
		mc.prevX = player.xo;
		mc.prevY = player.yo;
		mc.prevZ = player.zo;
		mc.curX = player.getX();
		mc.curY = player.getY();
		mc.curZ = player.getZ();
		// Same smoothing as Camera.tick(): eye height eases halfway toward the target each tick.
		if (player != eyePlayer) {
			eyePlayer = player;
			eyeSmoothed = player.getEyeHeight();
		}
		mc.eyeHeightO = eyeSmoothed;
		eyeSmoothed += (player.getEyeHeight() - eyeSmoothed) * 0.5F;
		mc.eyeHeightT = eyeSmoothed;
		boolean bob = minecraft.options.bobView().get();
		var avatar = player.avatarState();
		mc.walkDistO = bob ? avatar.getInterpolatedWalkDistance(0.0F) : 0.0F;
		mc.walkDist = bob ? avatar.getInterpolatedWalkDistance(1.0F) : 0.0F;
		mc.bobO = bob ? avatar.getInterpolatedBob(0.0F) : 0.0F;
		mc.bob = bob ? avatar.getInterpolatedBob(1.0F) : 0.0F;
		CyberLink.writeMcState(mc);
	}

	/** Freeze the player until the city's collision around them has arrived. */
	private static void holdUntilReady(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (!linked || player == null) {
			return;
		}
		if (!state.inGame() || state.loading()) {
			// V isn't in the city yet (Cyberpunk's menus, a save loading), or CyberCraft is turned off:
			// park the player where they are.
			if (holdPos == null) {
				holdPos = player.position();
			}
			teleportPending = true;
		}
		if (holdPos == null) {
			holdSince = 0;
			return;
		}
		if (holdSince == 0) {
			holdSince = System.currentTimeMillis();
		}
		int bx = (int) Math.floor(holdPos.x), by = (int) Math.floor(holdPos.y), bz = (int) Math.floor(holdPos.z);
		boolean known = CyberCollision.isKnown(bx, by - 1, bz) && CyberCollision.isKnown(bx, by, bz)
			&& CyberCollision.isKnown(bx, by - CyberCollision.REGION_SIZE, bz);
		// Release once there is actual ground below (or after a timeout, e.g. when mid-air on purpose).
		boolean ready = known && (CyberCollision.hasSolidBelow(bx, by, bz, 12) || System.currentTimeMillis() - holdSince > 6000);
		if (ready && state.inGame() && !state.loading()) {
			// V's feet can sit a fraction of a voxel inside our ground layer. Minecraft's collision
			// never pushes you out of a shape, so you'd drop through: lift out first.
			Vec3 safe = liftOutOfGeometry(player, holdPos);
			if (safe.y != holdPos.y) {
				player.setPos(safe.x, safe.y, safe.z);
				player.yo = safe.y;
				CyberCraft.LOG.info("CyberCraft: lifted player {} blocks out of the ground", String.format("%.3f", safe.y - holdPos.y));
			}
			holdPos = null;
			return;
		}
		player.setDeltaMovement(Vec3.ZERO);
		player.setPos(holdPos.x, holdPos.y, holdPos.z);
		player.xo = holdPos.x;
		player.yo = holdPos.y;
		player.zo = holdPos.z;
		player.resetFallDistance();
	}

	private static Vec3 liftOutOfGeometry(LocalPlayer player, Vec3 pos) {
		// Stand on the exact ground (the collision triangles) if it is slightly above the feet (up to
		// 2.5 blocks). The plugin sends voxels only, so this finds none and leaves the feet as they are.
		double ground = CyberCollider.groundAt(pos.x, pos.y, pos.z, 2.5);
		return !Double.isNaN(ground) && ground > pos.y ? new Vec3(pos.x, ground, pos.z) : pos;
	}

	private static void requestTeleport(Minecraft minecraft, double x, double y, double z, float yaw, float pitch) {
		LocalPlayer player = minecraft.player;
		player.setPos(x, y, z);
		player.setDeltaMovement(Vec3.ZERO);
		player.resetFallDistance();
		var server = minecraft.getSingleplayerServer();
		if (server != null) {
			var uuid = player.getUUID();
			server.execute(() -> {
				ServerPlayer sp = server.getPlayerList().getPlayer(uuid);
				if (sp != null) {
					sp.teleportTo(x, y, z);
					sp.setYRot(yaw);
					sp.setXRot(pitch);
					sp.resetFallDistance();
				}
			});
		}
		CyberCraft.LOG.info("CyberCraft: teleported to {} {} {}", x, y, z);
	}

	/** After GameRenderer.render(): report the player to the plugin and ship the overlay frame. */
	public static void afterRender() {
		if (!linked) {
			return;
		}
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		int flags = 0;
		if (player != null && minecraft.level != null) {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false);
			Vec3 feet = player.getPosition(partial);
			Camera camera = minecraft.gameRenderer.mainCamera();
			flags |= Proto.MC_IN_WORLD;
			if (player.onGround()) {
				flags |= Proto.MC_ON_GROUND;
			}
			if (player.isShiftKeyDown()) {
				flags |= Proto.MC_SNEAKING;
			}
			if (player.isSprinting()) {
				flags |= Proto.MC_SPRINTING;
			}
			if (player.isDeadOrDying()) {
				flags |= Proto.MC_DEAD;
			}
			if (player.isSwimming()) {
				flags |= Proto.MC_SWIMMING;
			}
			if (player.getAbilities().flying) {
				flags |= Proto.MC_FLYING;
			}
			// Cyberpunk turns the player as MouseHandler.turnPlayer would: slower through a spyglass.
			if (minecraft.options.getCameraType().isFirstPerson() && player.isScoping()) {
				flags |= Proto.MC_SCOPING;
			}
			mc.x = feet.x;
			mc.y = feet.y;
			mc.z = feet.z;
			mc.yaw = player.getYRot();
			mc.pitch = player.getXRot();
			// The eye, not the camera: in third person Minecraft's camera sits behind or in front.
			Vec3 eye = camera.isDetached() ? player.getEyePosition(partial) : camera.position();
			// Eased like the camera's (sneaking lowers it over a few ticks), not the eye's snap.
			mc.eyeHeight = camera.isDetached() ? mc.eyeHeightO + (mc.eyeHeightT - mc.eyeHeightO) * partial : (float) (eye.y - feet.y);
			mc.eyeX = eye.x;
			mc.eyeY = eye.y;
			mc.eyeZ = eye.z;
			mc.fov = camera.getFov();
			// Minecraft's F5 camera: Cyberpunk puts its camera where Minecraft's would be.
			mc.cameraMode = minecraft.options.getCameraType().ordinal();
			mc.cameraDistance = camera.isDetached() ? (float) camera.position().distanceTo(player.getEyePosition(partial)) : 0.0F;
			// Walk bob, exactly what GameRenderer.bobView() uses this frame.
			var entityState = minecraft.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.entityRenderState;
			boolean bob = minecraft.options.bobView().get() && entityState.isPlayer;
			mc.bobPhase = bob ? entityState.backwardsInterpolatedWalkDistance : 0.0F;
			mc.bobAmount = bob ? entityState.bob : 0.0F;
		}
		if (minecraft.gui.screen() != null) {
			flags |= Proto.MC_SCREEN_OPEN;
		}
		// And with the cinematic camera's easing and inverted axes.
		if (minecraft.options.smoothCamera) {
			flags |= Proto.MC_SMOOTH_CAMERA;
		}
		if (minecraft.options.invertMouseX().get()) {
			flags |= Proto.MC_INVERT_X;
		}
		if (minecraft.options.invertMouseY().get()) {
			flags |= Proto.MC_INVERT_Y;
		}
		mc.flags = flags;
		mc.sensitivity = minecraft.options.sensitivity().get().floatValue();
		// Not "arrived" until we are released, and not while a new player object (joined, respawned,
		// changed dimension) still stands wherever the server spawned it.
		boolean arrived = holdPos == null && !teleportPending && minecraft.player == lastPlayer;
		mc.teleportAck = arrived ? teleportAck : teleportAck - 1;
		mc.guiScale = minecraft.getWindow().getGuiScale();
		mc.frameCounter = ++frameCounter;
		// Keys with a Minecraft control stay Minecraft's; Cyberpunk gets the others.
		InputBridge.boundKeys(minecraft, mc.keys);
		CyberLink.writeMcState(mc);

		if ((flags & Proto.MC_IN_WORLD) != 0) {
			try {
				WorldExporter.frame(minecraft, minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(false));
			} catch (RuntimeException e) {
				if (exporterErrors++ < 5) {
					CyberCraft.LOG.error("CyberCraft: world export failed", e);
				}
			}
			FrameExporter.capture(minecraft);
		}
	}

	/**
	 * End of the frame: render at most once per Cyberpunk frame instead of spinning freely, and once more
	 * when a physics tick falls due well before Cyberpunk's next frame. Left waiting for that frame,
	 * the tick reached Cyberpunk a frame later, and V trailed Minecraft's player by as much. The
	 * overlay just rendered is shipped as soon as its copy back from the GPU is done.
	 */
	public static void paceFrame() {
		if (!linked) {
			return;
		}
		FrameExporter.shipFinished();
		if (state.disabled()) {
			// Turned off in Cyberpunk: none of this is shown, so a few frames a second will do.
			try {
				Thread.sleep(100);
			} catch (InterruptedException e) {
				Thread.currentThread().interrupt();
			}
			return;
		}
		if (cyberpunkStalled && (CyberLink.cyberStateSeq() >>> 1) == lastPacedSeq) {
			return; // Cyberpunk has stalled (or is closing): don't block every frame waiting for it
		}
		cyberpunkStalled = false;
		long start = System.nanoTime();
		long deadline = start + 25_000_000L;
		long tickWake = Long.MAX_VALUE;
		if (tickWakeArmed) {
			// A tick due just before Cyberpunk's next frame waits for that frame: a whole extra render
			// isn't worth a millisecond or two.
			long cyberpunkDue = lastCyberFrameNanos + cyberFrameNanos;
			tickWake = nextTickNanos + TICK_WAKE_SLACK_NANOS;
			if (tickWake > cyberpunkDue - 2_000_000L) {
				tickWake = Math.max(tickWake, cyberpunkDue + 2_000_000L);
			}
		}
		long nextShip = start + 500_000L;
		// CyberState.seq advances by 2 per Cyberpunk frame (odd while writing).
		while ((CyberLink.cyberStateSeq() >>> 1) == lastPacedSeq) {
			long now = System.nanoTime();
			if (now >= deadline) {
				break;
			}
			if (now >= tickWake) {
				// Once per tick, so a wake that came before the tick was due doesn't spin. Cyberpunk's
				// frame is still to come: the next paceFrame() waits for it.
				tickWakeArmed = false;
				return;
			}
			if (now >= nextShip) {
				FrameExporter.shipFinished();
				nextShip = now + 500_000L;
			}
			Thread.onSpinWait();
			if (deadline - now > 2_000_000L) {
				Thread.yield();
			}
		}
		int seqNow = CyberLink.cyberStateSeq() >>> 1;
		cyberpunkStalled = seqNow == lastPacedSeq;
		if (!cyberpunkStalled) {
			long now = System.nanoTime();
			long interval = now - lastCyberFrameNanos;
			if (lastCyberFrameNanos != 0 && interval < 100_000_000L) {
				cyberFrameNanos += (interval - cyberFrameNanos) / 8;
			}
			lastCyberFrameNanos = now;
		}
		lastPacedSeq = seqNow;
	}

	/**
	 * CyberCraft turned off in Cyberpunk's Mod Settings: Minecraft's own pause (a singleplayer world
	 * stops) until it's on again, when that pause screen is closed. holdUntilReady keeps the player
	 * where it stood meanwhile, and it's put where V is once it's back.
	 */
	private static void pauseWhileDisabled(Minecraft minecraft) {
		if (state.disabled()) {
			if (pausedForDisabled || minecraft.player == null) {
				return;
			}
			pausedForDisabled = true;
			InputBridge.releaseAll();
			// What Esc would do to an open screen (an inventory gives back the item held on the cursor).
			Screen open = minecraft.gui.screen();
			if (open != null && !open.isPauseScreen() && open.shouldCloseOnEsc()) {
				open.onClose();
			}
			if (minecraft.gui.screen() == null) {
				minecraft.pauseGame(false);
				disabledPause = minecraft.gui.screen();
			}
			CyberCraft.LOG.info("CyberCraft: turned off in Cyberpunk; paused");
		} else if (pausedForDisabled) {
			pausedForDisabled = false;
			if (disabledPause != null && minecraft.gui.screen() == disabledPause) {
				disabledPause.onClose();
			}
			disabledPause = null;
			CyberCraft.LOG.info("CyberCraft: turned on in Cyberpunk again");
		}
	}

	private static void applyLinkedOptions() {
		Minecraft minecraft = Minecraft.getInstance();
		var options = minecraft.options;
		options.pauseOnLostFocus = false;
		options.vignette().set(false);
		options.enableVsync().set(false);
		options.framerateLimit().set(260);
		// Minecraft doesn't draw the world itself; these only decide how far out placed blocks,
		// arrows and NPC stand-ins stay loaded and simulated.
		options.renderDistance().set(8);
		options.simulationDistance().set(8);
		options.autoJump().set(false);
		options.onboardAccessibility = false;
		if (options.tutorialStep != net.minecraft.client.tutorial.TutorialSteps.NONE) {
			minecraft.getTutorial().setStep(net.minecraft.client.tutorial.TutorialSteps.NONE);
		}
		options.getSoundSourceOptionInstance(net.minecraft.sounds.SoundSource.MUSIC).set(0.0);
		options.save();
	}

	private static void hideWindowOnce(Minecraft minecraft) {
		if (windowHidden || SHOW_WINDOW) {
			return;
		}
		windowHidden = true;
		SDLVideo.SDL_HideWindow(minecraft.getWindow().handle());
		CyberCraft.LOG.info("CyberCraft: game window hidden (run with -Dcybercraft.showWindow=true to keep it)");
	}

	private static void applyViewportSize(Minecraft minecraft) {
		int w = Math.min(state.viewportW, Proto.MAX_OVERLAY_W);
		int h = Math.min(state.viewportH, Proto.MAX_OVERLAY_H);
		if (w <= 0 || h <= 0 || (w == appliedViewportW && h == appliedViewportH)) {
			return;
		}
		appliedViewportW = w;
		appliedViewportH = h;
		minecraft.getWindow().setWindowed(w, h);
		CyberCraft.LOG.info("CyberCraft: sizing overlay to Cyberpunk viewport {}x{}", w, h);
	}
}
