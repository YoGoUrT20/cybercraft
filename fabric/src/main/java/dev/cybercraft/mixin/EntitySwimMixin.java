package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.world.CyberWater;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Sprint-swimming starts in the city's water too (see {@link CyberWater}). */
@Mixin(Entity.class)
public abstract class EntitySwimMixin {
	@WrapOperation(
		method = "updateSwimming",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;getFluidState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/material/FluidState;")
	)
	private FluidState cybercraft$swimInCityWater(Level level, BlockPos pos, Operation<FluidState> original) {
		FluidState state = original.call(level, pos);
		if (state.isEmpty() && CyberWater.active()) {
			FluidState water = CyberWater.fluidAt(level, pos);
			if (water != null) {
				return water;
			}
		}
		return state;
	}
}
