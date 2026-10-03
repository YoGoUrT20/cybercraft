package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberDigClient;
import net.minecraft.client.Minecraft;
import net.minecraft.world.InteractionHand;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Attacking diggable geometry digs into it (CyberDigClient). The plugin sends nothing diggable, so in
 * Night City every attack goes on to vanilla.
 */
@Mixin(Minecraft.class)
public abstract class MinecraftDigMixin {
	@Inject(method = "startAttack", at = @At("HEAD"), cancellable = true)
	private void cybercraft$digStart(CallbackInfoReturnable<Boolean> cir) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (CyberDigClient.attack(minecraft)) {
			// A swing, not a miss: no miss cooldown before mining the block that appears.
			var held = minecraft.player.getItemInHand(InteractionHand.MAIN_HAND);
			minecraft.player.swing(InteractionHand.MAIN_HAND, held.getAttackAnimation(), false);
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "continueAttack", at = @At("HEAD"))
	private void cybercraft$digHold(boolean down, CallbackInfo ci) {
		if (down) {
			CyberDigClient.attack((Minecraft) (Object) this);
		}
	}
}
