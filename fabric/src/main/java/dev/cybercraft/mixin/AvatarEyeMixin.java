package dev.cybercraft.mixin;

import net.minecraft.world.entity.Avatar;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.Pose;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Players' eyes sit higher: 1.89 blocks standing instead of 1.62, every other pose by the same ratio.
 * The plugin puts Cyberpunk's camera at Minecraft's eye, and at 0.75 m a block (fMetresPerBlock)
 * vanilla's 1.62 put it 1.22 m up, at the shoulders of Night City's people. 1.89 is 20 cm higher
 * there. Raised here rather than in the plugin, so the block Minecraft picks from its eye stays the
 * one under Cyberpunk's crosshair. Still under the 2-block ceiling of a dug tunnel. Both sides run
 * it (the server checks reach from the eye), and every player in a shared world has CyberCraft.
 */
@Mixin(Avatar.class)
public abstract class AvatarEyeMixin {
	private static final float EYE_RATIO = 1.89F / 1.62F;

	@Inject(method = "getDefaultDimensions", at = @At("RETURN"), cancellable = true)
	private void cybercraft$higherEyes(Pose pose, CallbackInfoReturnable<EntityDimensions> cir) {
		if (pose == Pose.DYING || pose == Pose.SLEEPING) {
			return;
		}
		EntityDimensions dims = cir.getReturnValue();
		cir.setReturnValue(dims.withEyeHeight(dims.eyeHeight() * EYE_RATIO));
	}
}
