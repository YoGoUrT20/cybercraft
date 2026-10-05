package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.ModifyReturnValue;
import dev.cybercraft.world.CyberWater;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Night City's water is Minecraft water to every fluid query: air under the city's surface reads as
 * a water source (see {@link CyberWater}). Fluid ticks and Minecraft's own drawing go through the
 * block state instead, which stays air, so the city's water never flows and isn't drawn twice.
 */
@Mixin(Level.class)
public abstract class LevelFluidMixin {
	@ModifyReturnValue(method = "getFluidState", at = @At("RETURN"))
	private FluidState cybercraft$cityWater(FluidState state, BlockPos pos) {
		if (state.isEmpty() && CyberWater.active()) {
			FluidState water = CyberWater.fluidAt((Level) (Object) this, pos);
			if (water != null) {
				return water;
			}
		}
		return state;
	}
}
