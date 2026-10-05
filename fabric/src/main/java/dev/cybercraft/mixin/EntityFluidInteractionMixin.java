package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.world.CyberWater;
import net.minecraft.world.entity.EntityFluidInteraction;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Entities look for fluids only in chunk sections that hold some; the city's water (see
 * {@link CyberWater}) is in none, so its columns count too. LevelFluidMixin supplies the water.
 */
@Mixin(EntityFluidInteraction.class)
public abstract class EntityFluidInteractionMixin {
	@WrapOperation(
		method = "update",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/entity/EntityFluidInteraction;hasFluidAndLoaded(Lnet/minecraft/world/level/Level;IIIIII)Z"
		)
	)
	private static boolean cybercraft$cityWaterNearby(Level level, int x0, int y0, int z0, int x1, int y1, int z1, Operation<Boolean> original) {
		return original.call(level, x0, y0, z0, x1, y1, z1) || CyberWater.anyIn(x0, y0, z0, x1, y1, z1);
	}
}
