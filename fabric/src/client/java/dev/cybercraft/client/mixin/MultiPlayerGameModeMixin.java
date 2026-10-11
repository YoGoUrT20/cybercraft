package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberDigClient;
import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.Constant;
import org.spongepowered.asm.mixin.injection.ModifyConstant;

/**
 * The pause after a block breaks before the next starts, held down: 0.6 s instead of vanilla's
 * quarter second, the same for Minecraft's blocks and the city's (CyberDigClient keeps to this same
 * counter), so breaking one doesn't go straight on into the other behind it.
 */
@Mixin(MultiPlayerGameMode.class)
public abstract class MultiPlayerGameModeMixin {
	@ModifyConstant(method = { "startDestroyBlock", "continueDestroyBlock" }, constant = @Constant(intValue = 5))
	private int cybercraft$pause(int vanilla) {
		return CyberDigClient.PAUSE_TICKS;
	}

}
