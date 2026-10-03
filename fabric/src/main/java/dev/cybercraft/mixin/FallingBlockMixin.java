package dev.cybercraft.mixin;

import dev.cybercraft.world.CyberCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Sand and gravel rest on the city's ground: to Minecraft there's only air under them, so they'd fall. */
@Mixin(FallingBlock.class)
public abstract class FallingBlockMixin {
	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void cybercraft$restOnCity(BlockState state, ServerLevel level, BlockPos pos, RandomSource random, CallbackInfo ci) {
		if (CyberCollision.supportsFromBelow(pos)) {
			ci.cancel();
		}
	}
}
