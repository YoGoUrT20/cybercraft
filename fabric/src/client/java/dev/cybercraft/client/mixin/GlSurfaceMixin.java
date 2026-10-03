package dev.cybercraft.client.mixin;

import com.mojang.renderpearl.backend.opengl.GlSurface;
import dev.cybercraft.client.CyberClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Nobody sees the hidden window, so nothing is copied into it or swapped: the overlay reaches
 * Cyberpunk by readback. The swap held the render thread for as long as the GPU, busy with
 * Cyberpunk, took to get to it: 2 ms a frame on an idle GPU, most of Minecraft's frame under load,
 * which left the overlay well behind Cyberpunk's frame rate (menus included).
 */
@Mixin(GlSurface.class)
public abstract class GlSurfaceMixin {
	@Inject(method = "blitFromTexture", at = @At("HEAD"), cancellable = true)
	private void cybercraft$noBlit(CallbackInfo ci) {
		if (CyberClient.windowHidden()) {
			ci.cancel();
		}
	}

	@Inject(method = "present", at = @At("HEAD"), cancellable = true)
	private void cybercraft$noPresent(CallbackInfo ci) {
		if (CyberClient.windowHidden()) {
			ci.cancel();
		}
	}
}
