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
 * Attacking the city's surface digs into it, and the middle button on it picks the block it shows
 * (CyberDigClient); anything else goes on to vanilla.
 */
@Mixin(Minecraft.class)
public abstract class MinecraftDigMixin {
	@Inject(method = "startAttack", at = @At("HEAD"), cancellable = true)
	private void cybercraft$digStart(CallbackInfoReturnable<Boolean> cir) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (CyberDigClient.attack(minecraft, true)) {
			// A swing, not a miss: no miss cooldown before mining the block that appears.
			var held = minecraft.player.getItemInHand(InteractionHand.MAIN_HAND);
			minecraft.player.swing(InteractionHand.MAIN_HAND, held.getAttackAnimation(), false);
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "continueAttack", at = @At("HEAD"), cancellable = true)
	private void cybercraft$digHold(boolean down, CallbackInfo ci) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (down && minecraft.player != null && !minecraft.player.isUsingItem() && CyberDigClient.attack(minecraft, false)) {
			// The arm swings all the while, as vanilla swings it at a block it's breaking (the pause
			// between blocks too): vanilla only does for a real block, and the city's cell is air to it.
			minecraft.player.swing(InteractionHand.MAIN_HAND, minecraft.player.getItemInHand(InteractionHand.MAIN_HAND).getAttackAnimation(), false);
			ci.cancel();
		}
	}

	@Inject(method = "pickBlockOrEntity", at = @At("HEAD"), cancellable = true)
	private void cybercraft$pickCity(CallbackInfo ci) {
		if (CyberDigClient.pick((Minecraft) (Object) this)) {
			ci.cancel();
		}
	}
}
