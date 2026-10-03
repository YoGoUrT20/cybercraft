package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberClient;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Cyberpunk keeps running while a Minecraft screen (pause menu, options, inventory) is open, so the
 * Minecraft world must too: arrows keep flying and NPCs' hits still land.
 */
@Mixin(Screen.class)
public abstract class ScreenMixin {
	@Inject(method = "isPauseScreen", at = @At("HEAD"), cancellable = true)
	private void cybercraft$neverPauseWhileLinked(CallbackInfoReturnable<Boolean> cir) {
		if (CyberClient.linked()) {
			cir.setReturnValue(false);
		}
	}
}
