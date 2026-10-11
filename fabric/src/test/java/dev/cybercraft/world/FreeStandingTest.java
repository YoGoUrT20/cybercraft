package dev.cybercraft.world;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import dev.cybercraft.link.Proto;
import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.reflect.Method;
import java.util.LinkedHashMap;
import java.util.Map;
import net.minecraft.core.BlockPos;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

/** CityVoxels.freeStanding on voxels as the plugin sends them (CyberCollision's region messages). */
class FreeStandingTest {
	// Sub-voxel layers of one cell (8 x 8 bits each, x + 8z).
	private static final long[] FLOOR = { 0, 0, 0, 0, 0, 0, -1L, -1L }; // a quarter-block slab at the top
	private static final long[] SHEET = { 0xFFL, 0xFFL, 0xFFL, 0xFFL, 0xFFL, 0xFFL, 0xFFL, 0xFFL }; // a wall's sheet, all the way up
	private static final long[] SEAT = { 0, 0, 0, -1L, 0, 0, 0, 0 }; // a seat's top, a little off the floor under it
	private static final long[] LOW = { -1L, -1L, 0, 0, 0, 0, 0, 0 }; // a slab at the bottom (a pavement on the road)

	private final Map<Long, long[]> scene = new LinkedHashMap<>();
	private final LongOpenHashSet dug = new LongOpenHashSet();

	@AfterEach
	void clear() throws Exception {
		this.scene.clear();
		this.dug.clear();
		send(); // nothing left in the box
	}

	private void put(int x, int y, int z, long[] layers) {
		this.scene.put(BlockPos.asLong(x, y, z), layers);
	}

	/** A floor of slab cells at y 9 over x, z 0..9, as on the roof in the screenshot. */
	private void slab() {
		for (int x = 0; x <= 9; x++) {
			for (int z = 0; z <= 9; z++) {
				put(x, 9, z, FLOOR);
			}
		}
	}

	/** The scene as one region message covering (-4, 0, -4) to (19, 39, 19). */
	private void send() throws Exception {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment s = arena.allocate(Proto.COL_REGION_HEADER_BYTES + (long) Proto.COL_BLOCK_BYTES * this.scene.size(), 8);
			int[] header = { -4, 0, -4, 19, 39, 19, 0, this.scene.size() };
			for (int i = 0; i < header.length; i++) {
				s.set(JAVA_INT, i * 4L, header[i]);
			}
			long e = Proto.COL_REGION_HEADER_BYTES;
			for (var cell : this.scene.entrySet()) {
				long pos = cell.getKey();
				s.set(JAVA_INT, e, BlockPos.getX(pos));
				s.set(JAVA_INT, e + 4, BlockPos.getY(pos));
				s.set(JAVA_INT, e + 8, BlockPos.getZ(pos));
				for (int k = 0; k < 8; k++) {
					s.set(JAVA_LONG, e + 16 + k * 8L, cell.getValue()[k]);
				}
				e += Proto.COL_BLOCK_BYTES;
			}
			Method read = CyberCollision.class.getDeclaredMethod("readRegion", MemorySegment.class, long.class);
			read.setAccessible(true);
			read.invoke(null, s, 0L);
		}
		CityVoxels.forget();
	}

	private void dig(int x, int y, int z) {
		this.dug.add(BlockPos.asLong(x, y, z));
	}

	private boolean free(int x, int y, int z) {
		return CityVoxels.freeStanding(x, y, z, this.dug::contains);
	}

	@Test
	void chairOnASlabStandsFree() throws Exception {
		slab();
		put(3, 10, 3, SHEET); // the cell mined
		put(3, 10, 2, SHEET); // the backrest, north
		put(3, 11, 2, SHEET); // poking up over it
		put(4, 10, 3, SHEET); // the seat's foot end, east
		send();
		assertTrue(free(3, 10, 3)); // open west and south
		assertTrue(free(4, 10, 3)); // open north, east and south
		assertTrue(free(3, 10, 2)); // the backrest: two cells tall, still a chair
		assertTrue(free(3, 11, 2)); // its top, standing on the rest of it
		assertFalse(free(3, 9, 3)); // the slab under it: a floor on nothing, ground
	}

	@Test
	void dugNeighboursCountAsOpen() throws Exception {
		slab();
		put(5, 10, 5, SHEET);
		put(4, 10, 5, SHEET);
		put(6, 10, 5, SHEET);
		put(5, 10, 4, SHEET); // open only south
		send();
		assertFalse(free(5, 10, 5));
		dig(4, 10, 5);
		assertTrue(free(5, 10, 5));
	}

	@Test
	void buildingWallIsNot() throws Exception {
		slab();
		for (int y = 10; y <= 14; y++) {
			put(2, y, 2, SHEET);
		}
		send();
		assertFalse(free(2, 10, 2)); // open on every side, but five cells tall
		assertFalse(free(2, 14, 2));
	}

	@Test
	void streetIsNot() throws Exception {
		slab();
		send();
		dig(3, 9, 4);
		dig(3, 9, 2);
		assertFalse(free(3, 9, 3)); // a floor over nothing the voxels show: ground
	}

	@Test
	void pavementOnTheRoadIsNotButASeatIs() throws Exception {
		slab();
		put(3, 10, 3, LOW); // flush on the slab under it
		put(6, 10, 6, SEAT); // a quarter block off it
		send();
		assertFalse(free(3, 10, 3));
		assertTrue(free(6, 10, 6));
	}

	@Test
	void groundBesideItIsNot() throws Exception {
		// A riser on a lone floor cell, with ground beside it (under a surface, no floor under it).
		put(12, 9, 12, FLOOR);
		put(12, 10, 12, SHEET);
		put(13, 12, 12, FLOOR); // over (13, 10, 12): ground there
		send();
		assertFalse(free(12, 10, 12));
	}
}
