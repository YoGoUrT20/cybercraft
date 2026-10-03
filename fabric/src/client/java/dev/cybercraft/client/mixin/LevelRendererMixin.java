package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberClient;
import net.minecraft.client.renderer.LevelRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Cyberpunk draws the world. While linked, Minecraft renders nothing of its own level (no sky,
 * clouds, fog or terrain), so the overlay is just the HUD (and the hand, unless the plugin draws
 * it) on a transparent background.
 */
@Mixin(LevelRenderer.class)
public abstract class LevelRendererMixin {
	@Inject(
		method = "render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V",
		at = @At("HEAD"),
		cancellable = true
	)
	private void cybercraft$skipLevel(CallbackInfo ci) {
		if (CyberClient.linked()) {
			ci.cancel();
		}
	}
}
