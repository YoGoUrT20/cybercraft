package dev.cybercraft.client.mixin;

import com.mojang.blaze3d.platform.Window;
import dev.cybercraft.client.CyberClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The MC window is hidden while linked; Cyberpunk has the real focus, so pretend we do too. */
@Mixin(Window.class)
public abstract class WindowMixin {
	@Inject(method = "isFocused", at = @At("HEAD"), cancellable = true)
	private void cybercraft$focused(CallbackInfoReturnable<Boolean> cir) {
		if (CyberClient.tookOver()) {
			// Focused while Cyberpunk is connected; if it goes away, act unfocused so MC never
			// tries to grab the (hidden) mouse.
			cir.setReturnValue(CyberClient.linked());
		}
	}

	@Inject(method = "isIconified", at = @At("HEAD"), cancellable = true)
	private void cybercraft$notIconified(CallbackInfoReturnable<Boolean> cir) {
		if (CyberClient.linked()) {
			cir.setReturnValue(false);
		}
	}
}
