package dev.cybercraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.world.CyberClip;
import net.minecraft.client.Camera;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The third-person camera (F5) pulls in against the city's walls, floors and ceilings as well as
 * Minecraft blocks, the way Minecraft's own camera does against blocks. The distance it ends up at
 * is what Cyberpunk's camera is pulled back by (McState.cameraDistance).
 */
@Mixin(Camera.class)
public abstract class CameraZoomMixin {
	@WrapOperation(
		method = "getMaxZoom",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult cybercraft$zoomAgainstCity(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return CyberClip.refine(context.getFrom(), context.getTo(), original.call(level, context), CyberClip.Use.PROJECTILE);
	}
}
