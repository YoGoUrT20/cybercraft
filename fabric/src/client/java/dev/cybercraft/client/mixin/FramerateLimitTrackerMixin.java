package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import dev.cybercraft.client.CyberClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** CyberClient.paceFrame() locks us to Cyberpunk's frame rate; don't let MC throttle on its own. */
@Mixin(FramerateLimitTracker.class)
public abstract class FramerateLimitTrackerMixin {
	@Inject(method = "getFramerateLimit", at = @At("HEAD"), cancellable = true)
	private void cybercraft$unlimited(CallbackInfoReturnable<Integer> cir) {
		if (CyberClient.linked()) {
			cir.setReturnValue(260);
		}
	}
}
