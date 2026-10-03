package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.cybercraft.client.CyberClient;
import dev.cybercraft.client.InputBridge;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Once Cyberpunk has taken over, keyboard state comes from the keys the plugin forwards
 * (InputBridge), not from SDL, and Minecraft never grabs or releases the real mouse.
 */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (CyberClient.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (CyberClient.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (CyberClient.tookOver()) {
			ci.cancel();
		}
	}
}
