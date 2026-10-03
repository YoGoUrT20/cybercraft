package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberDigClient;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** A dug block breaking reveals what's around it (CyberDigClient). */
@Mixin(ClientLevel.class)
public abstract class ClientLevelDigMixin {
	@Inject(method = "setBlocksDirty", at = @At("HEAD"))
	private void cybercraft$blockChanged(BlockPos pos, BlockState oldState, BlockState newState, CallbackInfo ci) {
		CyberDigClient.blockChanged((ClientLevel) (Object) this, pos, oldState, newState);
	}
}
