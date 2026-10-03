package dev.cybercraft.client;

import dev.cybercraft.world.CyberCollision;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * A car the host drives into the player shoves them out of its way. The host's cars are solid
 * (CyberCollision's car layer), but they move between scans, and Minecraft never moves a player out
 * of something that came to them: they'd stand inside the car. Each tick, if a car is in the
 * player, they're put the shortest way out of it to where nothing is in the way (sideways, or up
 * onto it when that's only a step), and carried on a little with the push.
 */
final class VehiclePush {
	private static final double STEP_UP = 1.0 / 16.0;
	private static final double STEP_SIDEWAYS = 1.0 / 8.0; // one car voxel cell is 1/2 block; this is plenty and cheap
	// The furthest out a car can have moved into the player in one scan. In blocks, and a car covers
	// more of them where the plugin makes a block less than a metre (fMetresPerBlock).
	private static final double MAX_SIDEWAYS = 3.0;
	private static final double MAX_UP = 0.6;       // standing on a bonnet's edge, not lifted onto a roof
	private static final double KEEP = 0.5;         // of the push kept as velocity, so it reads as a shove
	private static final Vec3[] SIDEWAYS = {
		new Vec3(1, 0, 0), new Vec3(-1, 0, 0), new Vec3(0, 0, 1), new Vec3(0, 0, -1),
		new Vec3(0.7071, 0, 0.7071), new Vec3(-0.7071, 0, 0.7071), new Vec3(0.7071, 0, -0.7071), new Vec3(-0.7071, 0, -0.7071),
	};

	private VehiclePush() {
	}

	static void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (player == null || minecraft.level == null || player.isSpectator() || player.isPassenger() || player.noPhysics) {
			return;
		}
		AABB box = player.getBoundingBox();
		if (!CyberCollision.vehicleIn(box)) {
			return;
		}
		Vec3 best = null;
		double bestDistance = Double.MAX_VALUE;
		for (double d = STEP_UP; d <= MAX_UP && best == null; d += STEP_UP) {
			if (free(minecraft, player, box, 0, d, 0)) {
				best = new Vec3(0, d, 0);
				bestDistance = d;
			}
		}
		for (Vec3 dir : SIDEWAYS) {
			for (double d = STEP_SIDEWAYS; d < bestDistance && d <= MAX_SIDEWAYS; d += STEP_SIDEWAYS) {
				if (free(minecraft, player, box, dir.x * d, 0, dir.z * d)) {
					best = dir.scale(d);
					bestDistance = d;
					break;
				}
			}
		}
		if (best == null) {
			return; // boxed in: nowhere to go until the car does
		}
		player.setPos(player.getX() + best.x, player.getY() + best.y, player.getZ() + best.z);
		if (best.y == 0) {
			Vec3 motion = player.getDeltaMovement();
			player.setDeltaMovement(motion.x + best.x * KEEP, motion.y, motion.z + best.z * KEEP);
		}
	}

	private static boolean free(Minecraft minecraft, LocalPlayer player, AABB box, double dx, double dy, double dz) {
		return minecraft.level.noCollision(player, box.move(dx, dy, dz));
	}
}
