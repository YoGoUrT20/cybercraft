package dev.cybercraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import net.minecraft.core.BlockPos;
import org.junit.jupiter.api.Test;

class DugColumnTest {
	private static final int CP77 = 0x43503737;
	private static final int OTHER = 7;

	/** Chunk (1, -2): cells in two sections of CP77, and one of another world. */
	private static CyberDig.DugColumn column() {
		return CyberDig.DugColumn.EMPTY
			.with(CP77, 17, 5, -30)
			.with(CP77, 18, 5, -30)
			.with(CP77, 17, 20, -30)
			.with(OTHER, 17, 5, -30);
	}

	@Test
	void withoutDropsOnlyPickedCellsOfThatWorld() {
		var left = column().without(CP77, 1, -2, p -> BlockPos.getX(p) == 17 && BlockPos.getY(p) == 5);
		assertFalse(left.isDug(CP77, 17, 5, -30));
		assertTrue(left.isDug(CP77, 18, 5, -30));
		assertTrue(left.isDug(CP77, 17, 20, -30));
		assertTrue(left.isDug(OTHER, 17, 5, -30));
	}

	@Test
	void withoutLosesSectionsLeftEmpty() {
		var left = column().without(CP77, 1, -2, p -> BlockPos.getY(p) >= 16);
		assertEquals(2, left.sections().size()); // CP77's section 0, the other world's
		var none = column().without(CP77, 1, -2, p -> true).without(OTHER, 1, -2, p -> true);
		assertTrue(none.sections().isEmpty());
	}

	@Test
	void withoutPickingNothingIsTheSameColumn() {
		var column = column();
		assertSame(column, column.without(CP77, 1, -2, p -> false));
		assertSame(column, column.without(12345, 1, -2, p -> true));
	}

	@Test
	void withoutSeesWorldPositions() {
		var seen = new it.unimi.dsi.fastutil.longs.LongOpenHashSet();
		column().without(CP77, 1, -2, p -> {
			seen.add(p);
			return false;
		});
		assertEquals(3, seen.size());
		assertTrue(seen.contains(BlockPos.asLong(17, 5, -30)));
		assertTrue(seen.contains(BlockPos.asLong(18, 5, -30)));
		assertTrue(seen.contains(BlockPos.asLong(17, 20, -30)));
	}
}
