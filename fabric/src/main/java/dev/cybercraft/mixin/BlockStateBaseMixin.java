package dev.cybercraft.mixin;

import dev.cybercraft.world.CyberCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.state.BlockBehaviour;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The city's ground and walls hold things up: torches, lanterns, rails, carpets and the like can be
 * placed on the ground and against walls, and stay there.
 */
@Mixin(BlockBehaviour.BlockStateBase.class)
public abstract class BlockStateBaseMixin {
	@Inject(
		method = "isFaceSturdy(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/core/Direction;Lnet/minecraft/world/level/block/SupportType;)Z",
		at = @At("HEAD"),
		cancellable = true
	)
	private void cybercraft$cityIsSturdy(BlockGetter level, BlockPos pos, Direction direction, SupportType type, CallbackInfoReturnable<Boolean> cir) {
		if (!((BlockBehaviour.BlockStateBase) (Object) this).isAir()) {
			return;
		}
		boolean sturdy = direction == Direction.UP ? CyberCollision.supportsFromBelow(pos.above()) : CyberCollision.solidFraction(pos) >= 0.4F;
		if (sturdy) {
			cir.setReturnValue(true);
		}
	}
}
