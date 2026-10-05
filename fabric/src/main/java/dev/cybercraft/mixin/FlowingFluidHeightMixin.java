package dev.cybercraft.mixin;

import dev.cybercraft.world.CyberWater;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The city's water stands at the city's surface, not at a source block's 8/9: floating, the camera's
 * underwater check, boats and bobbers all meet it there. The shape is built here rather than taken
 * from {@code FlowingFluid}'s per-state cache, which a city height would poison for real water.
 */
@Mixin(FlowingFluid.class)
public abstract class FlowingFluidHeightMixin {
	@Inject(method = "getHeight", at = @At("HEAD"), cancellable = true)
	private void cybercraft$cityWaterHeight(FluidState state, BlockGetter level, BlockPos pos, CallbackInfoReturnable<Float> cir) {
		float height = CyberWater.substitutedHeight(level, pos);
		if (height >= 0.0F) {
			cir.setReturnValue(height);
		}
	}

	@Inject(method = "getShape", at = @At("HEAD"), cancellable = true)
	private void cybercraft$cityWaterShape(FluidState state, BlockGetter level, BlockPos pos, CallbackInfoReturnable<VoxelShape> cir) {
		float height = CyberWater.substitutedHeight(level, pos);
		if (height >= 0.0F) {
			cir.setReturnValue(height >= 1.0F ? Shapes.block() : Shapes.box(0.0, 0.0, 0.0, 1.0, height, 1.0));
		}
	}
}
