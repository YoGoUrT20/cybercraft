package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.item.FireChargeItem;
import net.minecraft.world.item.FlintAndSteelItem;
import net.minecraft.world.item.context.UseOnContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Flint and steel and fire charges light the city's surface. They light the cell in front of the
 * block clicked; on the city, what's clicked is already that cell (the one a block placed against
 * the surface goes in, CyberClip), so one further out was in the air and nothing lit. A clicked
 * block that's air is only ever the city's (vanilla always clicks a real block): lit where it is.
 */
@Mixin({ FlintAndSteelItem.class, FireChargeItem.class })
public abstract class FireStarterMixin {
	@WrapOperation(method = "useOn", at = @At(value = "INVOKE", target = "Lnet/minecraft/core/BlockPos;relative(Lnet/minecraft/core/Direction;)Lnet/minecraft/core/BlockPos;"))
	private BlockPos cybercraft$lightTheCity(BlockPos clicked, Direction face, Operation<BlockPos> original, @Local(argsOnly = true) UseOnContext context) {
		return context.getLevel().getBlockState(clicked).isAir() ? clicked : original.call(clicked, face);
	}
}
