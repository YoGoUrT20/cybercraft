package dev.cybercraft.mixin;

import dev.cybercraft.world.CyberCollision;
import dev.cybercraft.world.CyberTri;
import dev.cybercraft.world.TriCollider;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.phys.AABB;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Crouching stops at the edges of triangle ground as well as Minecraft's blocks. Minecraft decides
 * where an edge is by looking for block collision under the player; a player walking on exact
 * triangles (CyberCollider) has none there, so every direction looked like a drop and crouching
 * froze the player. Ground the triangles would hold the player on, within a step below the feet,
 * now counts too: the same footprint TriCollider stands the player on. The plugin sends voxels only,
 * which vanilla's check already sees, so this acts only once there are triangles.
 */
@Mixin(Player.class)
public abstract class PlayerEdgeMixin {
	@Inject(method = "canFallAtLeast", at = @At("RETURN"), cancellable = true)
	private void cybercraft$stopAtTriangleGroundEdges(double dx, double dz, double drop, CallbackInfoReturnable<Boolean> cir) {
		Player player = (Player) (Object) this;
		if (!cir.getReturnValueZ() || !CyberCollision.usesSmoothCollider(player)) {
			return;
		}
		AABB box = player.getBoundingBox();
		double x = (box.minX + box.maxX) * 0.5 + dx;
		double z = (box.minZ + box.maxZ) * 0.5 + dz;
		double feet = box.minY;
		List<CyberTri> tris = new ArrayList<>();
		CyberCollision.trianglesNear(new AABB(x - 1.0, feet - drop - 1.0, z - 1.0, x + 1.0, feet + 1.0, z + 1.0), tris);
		double ground = TriCollider.groundAt(tris, x, feet, z, 1.0E-4);
		if (!Double.isNaN(ground) && ground >= feet - drop) {
			cir.setReturnValue(false);
		}
	}
}
