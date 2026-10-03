package dev.cybercraft.link;

import static dev.cybercraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.*;

import dev.cybercraft.CyberCraft;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

/**
 * The Minecraft end of the shared-memory link. The Cyberpunk plugin owns the mapping; we open it
 * when it appears and treat it as gone when the plugin's heartbeat stops.
 */
public final class CyberLink {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;
	// A loading screen or a save can stall the plugin's heartbeat for several seconds.
	private static final long HEARTBEAT_TIMEOUT_MS = 8000;

	private static final VarHandle INT = JAVA_INT.varHandle();
	private static final VarHandle LONG = JAVA_LONG.varHandle();

	private static final MethodHandle OPEN_FILE_MAPPING;
	// OpenFileMappingW's GetLastError, captured right after the call (the JVM may change it later).
	private static final java.lang.foreign.StructLayout CALL_STATE = Linker.Option.captureStateLayout();
	private static final VarHandle LAST_ERROR = CALL_STATE.varHandle(java.lang.foreign.MemoryLayout.PathElement.groupElement("GetLastError"));
	private static final MemorySegment OPEN_STATE = Arena.global().allocate(CALL_STATE);
	private static int lastOpenError = -1;
	private static final MethodHandle MAP_VIEW_OF_FILE;
	private static final MethodHandle GET_TICK_COUNT64;
	private static final MethodHandle GET_CURRENT_PROCESS_ID;
	private static final MethodHandle QUERY_PERFORMANCE_COUNTER;
	private static final MethodHandle QUERY_PERFORMANCE_FREQUENCY;
	private static final MethodHandle CREATE_MUTEX;
	private static MemorySegment runningMutex;
	private static final MemorySegment QPC_OUT = Arena.global().allocate(JAVA_LONG);

	static {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		OPEN_FILE_MAPPING = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS), Linker.Option.captureCallState("GetLastError")
		);
		MAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
		);
		GET_TICK_COUNT64 = linker.downcallHandle(k32.find("GetTickCount64").orElseThrow(), FunctionDescriptor.of(JAVA_LONG));
		GET_CURRENT_PROCESS_ID = linker.downcallHandle(k32.find("GetCurrentProcessId").orElseThrow(), FunctionDescriptor.of(JAVA_INT));
		QUERY_PERFORMANCE_COUNTER = linker.downcallHandle(k32.find("QueryPerformanceCounter").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
		QUERY_PERFORMANCE_FREQUENCY = linker.downcallHandle(k32.find("QueryPerformanceFrequency").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
		CREATE_MUTEX = linker.downcallHandle(k32.find("CreateMutexW").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
	}

	/**
	 * Holds a named mutex ("<link name>_minecraft") for as long as this Minecraft runs, so the
	 * plugin knows not to start another one (even before the two have linked up).
	 */
	public static synchronized void announceRunning() {
		if (runningMutex != null) {
			return;
		}
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment name = arena.allocateFrom(MAPPING_NAME + "_minecraft", StandardCharsets.UTF_16LE);
			runningMutex = (MemorySegment) CREATE_MUTEX.invokeExact(MemorySegment.NULL, 0, name);
		} catch (Throwable t) {
			CyberCraft.LOG.warn("CyberCraft: couldn't create the running-Minecraft mutex", t);
		}
	}

	private static volatile MemorySegment shm;
	private static long lastOpenAttempt;
	private static int cyberpunkPid;
	private static volatile int generation;

	private CyberLink() {
	}

	/** True when a live Cyberpunk is on the other end. Cheap; safe from any thread. */
	public static boolean active() {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long beat = (long) LONG.getAcquire(s, OFF_HEADER + H_CYBERPUNK_HEARTBEAT);
		return tickCount() - beat < HEARTBEAT_TIMEOUT_MS;
	}

	/** Bumps whenever a (new) Cyberpunk is on the other end: everything the plugin caches must be resent. */
	public static int generation() {
		return generation;
	}

	/** Process id of the Cyberpunk we're linked to (0 before the first link). */
	public static int cyberpunkPid() {
		return cyberpunkPid;
	}

	/** The mapping if it has been opened (whether or not Cyberpunk is still alive). */
	public static MemorySegment segment() {
		return shm;
	}

	/** Try to open the mapping at most once a second. Call regularly from the render thread. */
	public static void poll() {
		if (shm != null) {
			LONG.setRelease(shm, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			int pid = shm.get(JAVA_INT, OFF_HEADER + H_CYBERPUNK_PID);
			if (pid != cyberpunkPid) {
				// Cyberpunk restarted and reset the shared state; start our side over too.
				cyberpunkPid = pid;
				overlayBack = 1;
				generation++;
				CyberCraft.LOG.info("CyberCraft: Cyberpunk instance changed (pid {})", pid);
			}
			return;
		}
		long now = System.currentTimeMillis();
		if (now - lastOpenAttempt < 1000) {
			return;
		}
		lastOpenAttempt = now;
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment name = arena.allocateFrom(MAPPING_NAME, StandardCharsets.UTF_16LE);
			MemorySegment handle = (MemorySegment) OPEN_FILE_MAPPING.invokeExact(OPEN_STATE, FILE_MAP_ALL_ACCESS, 0, name);
			if (handle.address() == 0) {
				// Say why, once per reason: 2 is "the plugin hasn't made it yet" (normal while Cyberpunk
				// starts), 5 is "not allowed" (usually Cyberpunk running as administrator).
				int error = (int) LAST_ERROR.get(OPEN_STATE, 0L);
				if (error != lastOpenError) {
					lastOpenError = error;
					CyberCraft.LOG.info("CyberCraft: can't open Cyberpunk's shared memory yet (Windows error {}{})", error,
						error == 2 ? ": the plugin hasn't created it yet" : error == 5 ? ": access denied; is Cyberpunk running as administrator?" : "");
				}
				return;
			}
			MemorySegment view = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
			if (view.address() == 0) {
				CyberCraft.LOG.error("CyberCraft: MapViewOfFile failed");
				return;
			}
			MemorySegment seg = view.reinterpret(MAPPING_BYTES);
			int magic = seg.get(JAVA_INT, OFF_HEADER + H_MAGIC);
			int version = seg.get(JAVA_INT, OFF_HEADER + H_VERSION);
			if (magic != MAGIC || version != VERSION) {
				CyberCraft.LOG.error("CyberCraft: protocol mismatch (magic {} version {}); expected version {}", Integer.toHexString(magic), version, VERSION);
				return;
			}
			seg.set(JAVA_INT, OFF_HEADER + H_MC_PID, (int) GET_CURRENT_PROCESS_ID.invokeExact());
			LONG.setRelease(seg, OFF_HEADER + H_MC_HEARTBEAT, tickCount());
			cyberpunkPid = seg.get(JAVA_INT, OFF_HEADER + H_CYBERPUNK_PID);
			generation++;
			shm = seg;
			CyberCraft.LOG.info("CyberCraft: linked to Cyberpunk (pid {})", cyberpunkPid);
		} catch (Throwable t) {
			CyberCraft.LOG.error("CyberCraft: failed to open shared memory", t);
		}
	}

	/** QueryPerformanceCounter: the same clock the plugin reads, so tick timestamps line up across processes. */
	public static synchronized long qpc() {
		try {
			int ok = (int) QUERY_PERFORMANCE_COUNTER.invokeExact(QPC_OUT);
			return QPC_OUT.get(JAVA_LONG, 0);
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	/** Ticks per second of {@link #qpc()}. */
	public static synchronized long qpcFrequency() {
		try {
			int ok = (int) QUERY_PERFORMANCE_FREQUENCY.invokeExact(QPC_OUT);
			return QPC_OUT.get(JAVA_LONG, 0);
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	public static long tickCount() {
		try {
			return (long) GET_TICK_COUNT64.invokeExact();
		} catch (Throwable t) {
			throw new RuntimeException(t);
		}
	}

	// ---- CyberState (read) -------------------------------------------------------------------

	/** Plain snapshot of CyberState. */
	public static final class CyberState {
		public int seq;
		public int flags;
		public int worldId;
		public int collisionEpoch;
		public double x, y, z;
		public float yaw, pitch;
		public int teleportSeq;
		public int viewportW, viewportH;
		public float gameHour;
		public int weather;
		public float bodyBack;

		public boolean inGame() {
			return (this.flags & CYBER_IN_GAME) != 0;
		}

		public boolean menuOpen() {
			return (this.flags & CYBER_MENU_OPEN) != 0;
		}

		public boolean loading() {
			return (this.flags & CYBER_LOADING) != 0;
		}

		/** gameHour is Night City's clock, and Minecraft's time follows it. */
		public boolean clock() {
			return (this.flags & CYBER_CLOCK) != 0;
		}

		/** CyberCraft is turned off in Cyberpunk's Mod Settings: Minecraft pauses until it's on again. */
		public boolean disabled() {
			return (this.flags & CYBER_DISABLED) != 0;
		}

		/** The plugin draws the first-person hands and held items (REN_HAND), lit by the city. */
		public boolean drawsHand() {
			return (this.flags & CYBER_DRAWS_HAND) != 0;
		}

		/**
		 * V's own body is hidden in first person too: the player's body goes to the plugin there as
		 * well (REN_AVATAR), without head, arms or held items, {@link #bodyBack} blocks behind the eye.
		 */
		public boolean bodyFirstPerson() {
			return (this.flags & CYBER_BODY_FIRST_PERSON) != 0;
		}
	}

	/** Night City's water surface around V (see WaterGrid in the protocol). */
	public static final class WaterGrid {
		public int originX, originZ, worldId;
		public final int size = WATER_GRID_SIZE;
		public final float[] surface = new float[WATER_GRID_SIZE * WATER_GRID_SIZE];
	}

	/** A consistent copy of the water grid, or null (no link, or the plugin mid-write). */
	public static WaterGrid readWaterGrid() {
		MemorySegment s = shm;
		if (s == null) {
			return null;
		}
		long b = OFF_WATER_GRID;
		WaterGrid out = new WaterGrid();
		for (int attempt = 0; attempt < 100; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + WG_SEQ);
			if ((seq1 & 1) != 0 || seq1 == 0) {
				Thread.onSpinWait();
				if (seq1 == 0) {
					return null; // the plugin hasn't written one yet
				}
				continue;
			}
			out.originX = s.get(JAVA_INT, b + WG_ORIGIN_X);
			out.originZ = s.get(JAVA_INT, b + WG_ORIGIN_Z);
			out.worldId = s.get(JAVA_INT, b + WG_WORLD_ID);
			for (int i = 0; i < out.surface.length; i++) {
				out.surface[i] = s.get(JAVA_FLOAT, b + WG_SURFACE + i * 4L);
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + WG_SEQ) == seq1) {
				return out;
			}
		}
		return null;
	}

	/** Seqlock read of CyberState into {@code out}. Returns false if the link is down. */
	public static boolean readCyberState(CyberState out) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long b = OFF_CYBER_STATE;
		for (int attempt = 0; attempt < 1000; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + CS_SEQ);
			if ((seq1 & 1) != 0) {
				if (attempt > 100) {
					Thread.yield();
				} else {
					Thread.onSpinWait();
				}
				continue;
			}
			out.flags = s.get(JAVA_INT, b + CS_FLAGS);
			out.worldId = s.get(JAVA_INT, b + CS_WORLD_ID);
			out.collisionEpoch = s.get(JAVA_INT, b + CS_COLLISION_EPOCH);
			out.x = s.get(JAVA_DOUBLE, b + CS_POS_X);
			out.y = s.get(JAVA_DOUBLE, b + CS_POS_Y);
			out.z = s.get(JAVA_DOUBLE, b + CS_POS_Z);
			out.yaw = s.get(JAVA_FLOAT, b + CS_YAW);
			out.pitch = s.get(JAVA_FLOAT, b + CS_PITCH);
			out.teleportSeq = s.get(JAVA_INT, b + CS_TELEPORT_SEQ);
			out.viewportW = s.get(JAVA_INT, b + CS_VIEWPORT_W);
			out.viewportH = s.get(JAVA_INT, b + CS_VIEWPORT_H);
			out.gameHour = s.get(JAVA_FLOAT, b + CS_GAME_HOUR);
			out.weather = s.get(JAVA_INT, b + CS_WEATHER);
			out.bodyBack = s.get(JAVA_FLOAT, b + CS_BODY_BACK);
			VarHandle.loadLoadFence();
			int seq2 = (int) INT.getAcquire(s, b + CS_SEQ);
			if (seq1 == seq2) {
				out.seq = seq1;
				return true;
			}
		}
		return false;
	}

	/** Raw CyberState sequence number; changes once per Cyberpunk frame. */
	public static int cyberStateSeq() {
		MemorySegment s = shm;
		return s == null ? 0 : (int) INT.getAcquire(s, OFF_CYBER_STATE + CS_SEQ);
	}

	// ---- McState (write) -------------------------------------------------------------------

	public static final class McState {
		public int flags;
		public double x, y, z;
		public float yaw, pitch;
		public float eyeHeight;
		public float sensitivity;
		public int teleportAck;
		public int guiScale;
		public long frameCounter;
		public float fov;
		public float bobPhase;
		public float bobAmount;
		public double eyeX, eyeY, eyeZ;
		public long tickQpc;
		public double prevX, prevY, prevZ;
		public double curX, curY, curZ;
		public float eyeHeightO, eyeHeightT;
		public float walkDistO, walkDist;
		public float bobO, bob;
		public float tickMs = 50.0F;
		public int cameraMode;
		public float cameraDistance;
		/** Bit n of keys[n / 32]: SDL scancode n has a Minecraft control (the rest go to Cyberpunk). */
		public final int[] keys = new int[8];
	}

	public static void writeMcState(McState st) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_MC_STATE;
		int seq = s.get(JAVA_INT, b + MS_SEQ);
		INT.setRelease(s, b + MS_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		s.set(JAVA_INT, b + MS_FLAGS, st.flags);
		s.set(JAVA_DOUBLE, b + MS_X, st.x);
		s.set(JAVA_DOUBLE, b + MS_Y, st.y);
		s.set(JAVA_DOUBLE, b + MS_Z, st.z);
		s.set(JAVA_FLOAT, b + MS_YAW, st.yaw);
		s.set(JAVA_FLOAT, b + MS_PITCH, st.pitch);
		s.set(JAVA_FLOAT, b + MS_EYE_HEIGHT, st.eyeHeight);
		s.set(JAVA_FLOAT, b + MS_SENSITIVITY, st.sensitivity);
		s.set(JAVA_INT, b + MS_TELEPORT_ACK, st.teleportAck);
		s.set(JAVA_INT, b + MS_GUI_SCALE, st.guiScale);
		s.set(JAVA_LONG, b + MS_FRAME_COUNTER, st.frameCounter);
		s.set(JAVA_FLOAT, b + MS_FOV, st.fov);
		s.set(JAVA_FLOAT, b + MS_BOB_PHASE, st.bobPhase);
		s.set(JAVA_FLOAT, b + MS_BOB_AMOUNT, st.bobAmount);
		s.set(JAVA_DOUBLE, b + MS_EYE_X, st.eyeX);
		s.set(JAVA_DOUBLE, b + MS_EYE_Y, st.eyeY);
		s.set(JAVA_DOUBLE, b + MS_EYE_Z, st.eyeZ);
		s.set(JAVA_LONG, b + MS_TICK_QPC, st.tickQpc);
		s.set(JAVA_DOUBLE, b + MS_PREV_X, st.prevX);
		s.set(JAVA_DOUBLE, b + MS_PREV_X + 8, st.prevY);
		s.set(JAVA_DOUBLE, b + MS_PREV_X + 16, st.prevZ);
		s.set(JAVA_DOUBLE, b + MS_CUR_X, st.curX);
		s.set(JAVA_DOUBLE, b + MS_CUR_X + 8, st.curY);
		s.set(JAVA_DOUBLE, b + MS_CUR_X + 16, st.curZ);
		s.set(JAVA_FLOAT, b + MS_EYE_HEIGHT_O, st.eyeHeightO);
		s.set(JAVA_FLOAT, b + MS_EYE_HEIGHT_T, st.eyeHeightT);
		s.set(JAVA_FLOAT, b + MS_WALK_O, st.walkDistO);
		s.set(JAVA_FLOAT, b + MS_WALK, st.walkDist);
		s.set(JAVA_FLOAT, b + MS_BOB_O, st.bobO);
		s.set(JAVA_FLOAT, b + MS_BOB, st.bob);
		s.set(JAVA_FLOAT, b + MS_TICK_MS, st.tickMs);
		s.set(JAVA_INT, b + MS_CAMERA_MODE, st.cameraMode);
		s.set(JAVA_FLOAT, b + MS_CAMERA_DISTANCE, st.cameraDistance);
		for (int i = 0; i < st.keys.length; i++) {
			s.set(JAVA_INT, b + MS_MC_KEYS + i * 4L, st.keys[i]);
		}
		INT.setRelease(s, b + MS_SEQ, seq + 2);
	}

	// ---- input ring (consume) --------------------------------------------------------------

	public interface InputSink {
		void accept(int type, int code, int a, int b, int c);
	}

	/** Drains every pending input event. Render thread only. */
	public static void drainInput(InputSink sink) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long base = OFF_INPUT_RING;
		long head = (long) LONG.getAcquire(s, base + IR_HEAD);
		long tail = s.get(JAVA_LONG, base + IR_TAIL);
		if (head - tail > INPUT_RING_ENTRIES) {
			tail = head - INPUT_RING_ENTRIES; // producer lapped us; drop the oldest
		}
		while (tail < head) {
			long e = base + IR_DATA + (tail & (INPUT_RING_ENTRIES - 1)) * 16L;
			int type = Short.toUnsignedInt(s.get(JAVA_SHORT, e));
			int code = Short.toUnsignedInt(s.get(JAVA_SHORT, e + 2));
			int a = s.get(JAVA_INT, e + 4);
			int b = s.get(JAVA_INT, e + 8);
			int c = s.get(JAVA_INT, e + 12);
			tail++;
			sink.accept(type, code, a, b, c);
		}
		LONG.setRelease(s, base + IR_TAIL, tail);
	}

	// ---- actor table (read) ----------------------------------------------------------------

	/** One NPC near V (see ActorRecord in the protocol header). */
	public record Actor(int actorId, int flags, float x, float y, float z, float yaw, float width, float height, float healthFrac, int level, String name) {
		public boolean dead() {
			return (this.flags & ACTOR_DEAD) != 0;
		}

		public boolean hostile() {
			return (this.flags & ACTOR_HOSTILE) != 0;
		}
	}

	/** Seqlock read of the actor table. Returns false (leaving {@code out} empty) on a torn read. */
	public static boolean readActors(java.util.List<Actor> out) {
		out.clear();
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		long b = OFF_ACTOR_TABLE;
		for (int attempt = 0; attempt < 16; attempt++) {
			int seq1 = (int) INT.getAcquire(s, b + AT_SEQ);
			if ((seq1 & 1) != 0) {
				Thread.onSpinWait();
				continue;
			}
			int count = Math.min(s.get(JAVA_INT, b + AT_COUNT), MAX_ACTORS);
			for (int i = 0; i < count; i++) {
				long r = b + AT_RECORDS + i * ACTOR_RECORD_BYTES;
				out.add(new Actor(
					s.get(JAVA_INT, r), s.get(JAVA_INT, r + 4),
					s.get(JAVA_FLOAT, r + 8), s.get(JAVA_FLOAT, r + 12), s.get(JAVA_FLOAT, r + 16),
					s.get(JAVA_FLOAT, r + 20), s.get(JAVA_FLOAT, r + 24), s.get(JAVA_FLOAT, r + 28),
					s.get(JAVA_FLOAT, r + 32), Short.toUnsignedInt(s.get(JAVA_SHORT, r + 36)), readName(s, r + 40, 24)
				));
			}
			VarHandle.loadLoadFence();
			if ((int) INT.getAcquire(s, b + AT_SEQ) == seq1) {
				return true;
			}
			out.clear();
		}
		return false;
	}

	private static String readName(MemorySegment s, long off, int max) {
		byte[] bytes = new byte[max];
		int n = 0;
		while (n < max) {
			byte b = s.get(JAVA_BYTE, off + n);
			if (b == 0) {
				break;
			}
			bytes[n++] = b;
		}
		return new String(bytes, 0, n, StandardCharsets.UTF_8);
	}

	// ---- event ring (produce) --------------------------------------------------------------

	/**
	 * Queues an event for the plugin. {@code id} depends on the type (an actorId, a WEATHER_*, ...).
	 * Safe from any thread. Drops the event if the plugin is a full ring behind.
	 */
	public static void pushEvent(int type, int id, float a, float b, float c, float d, int flags) {
		pushEvent(type, id, a, b, c, d, flags, 0);
	}

	public static synchronized void pushEvent(int type, int id, float a, float b, float c, float d, int flags, int weapon) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long base = OFF_EVENT_RING;
		long head = s.get(JAVA_LONG, base + ER_HEAD);
		long tail = (long) LONG.getAcquire(s, base + ER_TAIL);
		if (head - tail >= EVENT_RING_ENTRIES) {
			return;
		}
		long e = base + ER_DATA + (head & (EVENT_RING_ENTRIES - 1)) * EVENT_BYTES;
		s.set(JAVA_INT, e, type);
		s.set(JAVA_INT, e + 4, id);
		s.set(JAVA_FLOAT, e + 8, a);
		s.set(JAVA_FLOAT, e + 12, b);
		s.set(JAVA_FLOAT, e + 16, c);
		s.set(JAVA_FLOAT, e + 20, d);
		s.set(JAVA_INT, e + 24, flags);
		s.set(JAVA_INT, e + 28, weapon);
		LONG.setRelease(s, base + ER_HEAD, head + 1);
	}

	// ---- world entities (write) ------------------------------------------------------------

	/**
	 * One Minecraft thing for the plugin to draw (see WorldEntity in the protocol header). {@code uv}
	 * holds up to three atlas rects {u0, v0, u1, v1}: sprite/side, top, bottom.
	 */
	public record WorldEntity(int kind, int id, float x, float y, float z, float yaw, float pitch, float scale, float[] ext, float[] uv, int tint) {
	}

	/** Seqlock write of the world-entity table and block selection. Render thread only. */
	public static void writeWorldEntities(java.util.List<WorldEntity> entities, float[] selection) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long b = OFF_WORLD_ENTITIES;
		int seq = s.get(JAVA_INT, b + WE_SEQ);
		INT.setRelease(s, b + WE_SEQ, seq + 1);
		VarHandle.storeStoreFence();
		int count = Math.min(entities.size(), MAX_WORLD_ENTITIES);
		s.set(JAVA_INT, b + WE_COUNT, count);
		s.set(JAVA_INT, b + WE_HAS_SELECTION, selection != null ? 1 : 0);
		if (selection != null) {
			for (int i = 0; i < 6; i++) {
				s.set(JAVA_FLOAT, b + WE_SEL_MIN + i * 4L, selection[i]);
			}
		}
		for (int i = 0; i < count; i++) {
			WorldEntity w = entities.get(i);
			long r = b + WE_RECORDS + i * WORLD_ENTITY_BYTES;
			s.set(JAVA_INT, r, w.kind());
			s.set(JAVA_INT, r + 4, w.id());
			s.set(JAVA_FLOAT, r + 8, w.x());
			s.set(JAVA_FLOAT, r + 12, w.y());
			s.set(JAVA_FLOAT, r + 16, w.z());
			s.set(JAVA_FLOAT, r + 20, w.yaw());
			s.set(JAVA_FLOAT, r + 24, w.pitch());
			s.set(JAVA_FLOAT, r + 28, w.scale());
			for (int k = 0; k < 3; k++) {
				s.set(JAVA_FLOAT, r + 32 + k * 4L, w.ext() != null ? w.ext()[k] : 0.0F);
			}
			for (int k = 0; k < 12; k++) {
				s.set(JAVA_FLOAT, r + 44 + k * 4L, w.uv() != null && k < w.uv().length ? w.uv()[k] : 0.0F);
			}
			s.set(JAVA_INT, r + 92, w.tint());
		}
		INT.setRelease(s, b + WE_SEQ, seq + 2);
	}

	// ---- render ring (produce) -------------------------------------------------------------

	/**
	 * Writes one render message ({@code header} bytes then {@code body} bytes) into the render ring,
	 * waiting briefly for space. Single producer. Returns false if it never fit.
	 */
	public static boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 500);
	}

	/** Like writeRender, but gives up at once if the ring is full (per-frame data that the next frame replaces). */
	public static boolean tryWriteRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body) {
		return writeRender(type, header, body, 1);
	}

	private static synchronized boolean writeRender(int type, java.nio.ByteBuffer header, java.nio.ByteBuffer body, int attempts) {
		MemorySegment s = shm;
		if (s == null) {
			return false;
		}
		int payload = header.remaining() + (body != null ? body.remaining() : 0);
		long msgBytes = (8 + payload + 7) & ~7L;
		if (msgBytes > RR_DATA_BYTES / 2) {
			CyberCraft.LOG.warn("CyberCraft: render message too large ({} bytes)", msgBytes);
			return false;
		}
		long base = OFF_RENDER_RING;
		for (int attempt = 0; attempt < attempts; attempt++) {
			long head = s.get(JAVA_LONG, base + RR_HEAD);
			long tail = (long) LONG.getAcquire(s, base + RR_TAIL);
			long pos = head % RR_DATA_BYTES;
			long pad = pos + msgBytes > RR_DATA_BYTES ? RR_DATA_BYTES - pos : 0;
			if (RR_DATA_BYTES - (head - tail) < msgBytes + pad) {
				if (attempt + 1 >= attempts) {
					break;
				}
				try {
					Thread.sleep(2);
				} catch (InterruptedException e) {
					return false;
				}
				continue;
			}
			if (pad > 0) {
				s.set(JAVA_INT, base + RR_DATA + pos, REN_PAD);
				s.set(JAVA_INT, base + RR_DATA + pos + 4, 0);
				head += pad;
				pos = 0;
			}
			long at = base + RR_DATA + pos;
			s.set(JAVA_INT, at, type);
			s.set(JAVA_INT, at + 4, payload);
			MemorySegment.copy(MemorySegment.ofBuffer(header), 0, s, at + 8, header.remaining());
			if (body != null && body.remaining() > 0) {
				MemorySegment.copy(MemorySegment.ofBuffer(body), 0, s, at + 8 + header.remaining(), body.remaining());
			}
			LONG.setRelease(s, base + RR_HEAD, head + msgBytes);
			return true;
		}
		return false;
	}

	// ---- overlay (publish) -----------------------------------------------------------------

	private static int overlayBack = 1; // writer's private slot; the plugin's front starts at 2, middle at 0

	/** Returns the byte offset where the next overlay frame should be written. */
	public static long overlayBackSlotOffset() {
		return OFF_OVERLAY_PIXELS + overlayBack * OVERLAY_SLOT_BYTES;
	}

	/** Publishes the frame just written into the back slot. */
	public static void publishOverlay(int width, int height, boolean bottomUp, long frameId) {
		MemorySegment s = shm;
		if (s == null) {
			return;
		}
		long hdr = OFF_OVERLAY_SLOT_HDR + overlayBack * SLOT_HDR_SIZE;
		s.set(JAVA_INT, hdr + SH_WIDTH, width);
		s.set(JAVA_INT, hdr + SH_HEIGHT, height);
		s.set(JAVA_INT, hdr + SH_FLAGS, bottomUp ? 1 : 0);
		s.set(JAVA_LONG, hdr + SH_FRAME_ID, frameId);
		int old = (int) INT.getAndSet(s, OFF_OVERLAY_CTL + OC_STATE, overlayBack | OVERLAY_DIRTY);
		overlayBack = old & 3;
		LONG.getAndAdd(s, OFF_OVERLAY_CTL + OC_FRAMES_PUBLISHED, 1L);
	}

	// ---- collision ring (consume) ----------------------------------------------------------

	public static long collisionHead() {
		MemorySegment s = shm;
		return s == null ? 0 : (long) LONG.getAcquire(s, OFF_COLLISION_RING + CR_HEAD);
	}

	public static long collisionTail() {
		MemorySegment s = shm;
		return s == null ? 0 : s.get(JAVA_LONG, OFF_COLLISION_RING + CR_TAIL);
	}

	public static void setCollisionTail(long tail) {
		MemorySegment s = shm;
		if (s != null) {
			LONG.setRelease(s, OFF_COLLISION_RING + CR_TAIL, tail);
		}
	}
}
