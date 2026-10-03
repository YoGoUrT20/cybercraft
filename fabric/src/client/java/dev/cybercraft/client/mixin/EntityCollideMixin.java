package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberClient;
import dev.cybercraft.client.CyberCollider;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * After vanilla has collided the local player's movement with Minecraft blocks, collide it with
 * the exact collision triangles (smooth slopes instead of voxel stair-steps). The plugin sends
 * voxels only, so in Night City CyberCollider finds none and the movement passes through unchanged.
 */
@Mixin(Entity.class)
public abstract class EntityCollideMixin {
	@Inject(method = "collide", at = @At("RETURN"), cancellable = true)
	private void cybercraft$collideWithTriangles(Vec3 movement, CallbackInfoReturnable<Vec3> cir) {
		if ((Object) this instanceof LocalPlayer player && CyberClient.linked() && !player.noPhysics) {
			cir.setReturnValue(CyberCollider.collide(player, cir.getReturnValue()));
		}
	}
}
