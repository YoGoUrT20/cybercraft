package dev.cybercraft.world;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import it.unimi.dsi.fastutil.longs.LongOpenHashSet;
import java.util.List;
import net.minecraft.core.BlockPos;
import org.junit.jupiter.api.Test;

class HoleMoveTest {
	private static final int CP77 = 0x43503737;

	/** A shaft 1 x 3 x 1 opened from a street at y 10.95 (its top cell is 10). */
	private static LongOpenHashSet shaft(int x, int z) {
		LongOpenHashSet cells = new LongOpenHashSet();
		for (int y = 8; y <= 10; y++) {
			cells.add(BlockPos.asLong(x, y, z));
		}
		return cells;
	}

	private static HoleMove.Plan plan(LongOpenHashSet old, List<CyberDig.Opening> openings, HoleMove.GridMove move) {
		return HoleMove.plan(old, openings, move, pos -> false);
	}

	private static HoleMove.GridMove turn(double degrees, double tx, double ty, double tz) {
		double a = Math.toRadians(degrees), c = Math.cos(a), s = Math.sin(a);
		return new HoleMove.GridMove(c, -s, s, c, 1.0, tx, ty, tz, false);
	}

	@Test
	void shiftMovesEveryCellByWholeBlocks() {
		LongOpenHashSet old = shaft(5, 7);
		var opening = new CyberDig.Opening(CP77, BlockPos.asLong(5, 10, 7), 10.95F);
		var plan = plan(old, List.of(opening), new HoleMove.GridMove(1, 0, 0, 1, 1, -1, 0, 2, true));
		assertEquals(3, plan.cells().size());
		assertTrue(plan.cells().containsKey(BlockPos.asLong(4, 9, 9)));
		assertEquals(BlockPos.asLong(4, 10, 9), plan.openings().getFirst().pos());
		assertEquals(BlockPos.asLong(4, 8, 9), plan.moved(BlockPos.asLong(5, 8, 7)));
	}

	@Test
	void groundMovingUpLiftsTheHoleUnderIt() {
		// The grid moved 0.3 blocks down under Night City: the street is at 11.25 now.
		var opening = new CyberDig.Opening(CP77, BlockPos.asLong(5, 10, 7), 10.95F);
		var plan = plan(shaft(5, 7), List.of(opening), turn(0, 0, 0.3, 0));
		for (int y = 9; y <= 11; y++) {
			assertTrue(plan.cells().containsKey(BlockPos.asLong(5, y, 7)), "row " + y);
		}
		assertEquals(3, plan.cells().size());
		var moved = plan.openings().getFirst();
		assertEquals(11, BlockPos.getY(moved.pos()));
		assertEquals(11.25F, moved.surface(), 1e-4F);
	}

	@Test
	void groundMovingDownLowersTheHoleUnderIt() {
		// A street at 10.2 (in cell 10) seen 0.3 blocks lower: 9.9, in cell 9.
		var opening = new CyberDig.Opening(CP77, BlockPos.asLong(5, 10, 7), 10.2F);
		var plan = plan(shaft(5, 7), List.of(opening), turn(0, 0, -0.3, 0));
		for (int y = 7; y <= 9; y++) {
			assertTrue(plan.cells().containsKey(BlockPos.asLong(5, y, 7)), "row " + y);
		}
		assertEquals(3, plan.cells().size());
	}

	@Test
	void aQuarterTurnAboutTheHoleKeepsItInPlace() {
		// Turned about the shaft's own middle (5.5, 7.5): it maps onto itself.
		var move = turn(90, 5.5 + 7.5, 0, 7.5 - 5.5);
		var plan = plan(shaft(5, 7), List.of(), move);
		assertEquals(3, plan.cells().size());
		for (int y = 8; y <= 10; y++) {
			assertTrue(plan.cells().containsKey(BlockPos.asLong(5, y, 7)), "row " + y);
		}
	}

	@Test
	void aTurnedTrenchHasNoGaps() {
		// A trench 6 long, 1 deep, turned 30 degrees: every new cell's neighbours along it are there.
		LongOpenHashSet old = new LongOpenHashSet();
		for (int x = 0; x < 6; x++) {
			old.add(BlockPos.asLong(x, 10, 0));
		}
		var plan = plan(old, List.of(), turn(30, 0, 0, 0));
		assertTrue(plan.cells().size() >= 6, "cells " + plan.cells().size());
		// Joined face to face: one piece.
		LongOpenHashSet seen = new LongOpenHashSet();
		java.util.ArrayDeque<Long> queue = new java.util.ArrayDeque<>();
		long first = plan.cells().keySet().iterator().nextLong();
		queue.add(first);
		seen.add(first);
		while (!queue.isEmpty()) {
			long pos = queue.poll();
			for (net.minecraft.core.Direction d : net.minecraft.core.Direction.values()) {
				long n = BlockPos.offset(pos, d.getStepX(), d.getStepY(), d.getStepZ());
				if (plan.cells().containsKey(n) && seen.add(n)) {
					queue.add(n);
				}
			}
		}
		assertEquals(plan.cells().size(), seen.size());
	}

	@Test
	void aBlockWinsOverAirWhereTwoCellsMeet() {
		// Scaled to half: two old cells share one new one; the solid one is kept.
		LongOpenHashSet old = new LongOpenHashSet();
		old.add(BlockPos.asLong(0, 10, 0));
		old.add(BlockPos.asLong(1, 10, 0));
		long solid = BlockPos.asLong(1, 10, 0);
		var move = new HoleMove.GridMove(0.5, 0, 0, 0.5, 1.0, 0, 0, 0, false);
		var plan = HoleMove.plan(old, List.of(), move, pos -> pos == solid);
		assertEquals(solid, plan.cells().get(BlockPos.asLong(0, 10, 0)));
	}
}
