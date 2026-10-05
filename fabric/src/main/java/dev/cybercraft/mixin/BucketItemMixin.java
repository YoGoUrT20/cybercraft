package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import dev.cybercraft.world.CyberWater;
import net.minecraft.core.BlockPos;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.BucketItem;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.LevelAccessor;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.BucketPickup;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Buckets and the city's water (see {@link CyberWater}): an empty bucket fills from it, as from an
 * endless sea, and a water bucket emptied into it just pours in without leaving a block behind.
 */
@Mixin(BucketItem.class)
public abstract class BucketItemMixin {
	// Picking up: the hit cell is air, so stand water's block in for it (its pickup sound included).
	@WrapOperation(
		method = "use",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/block/state/BlockState;getBlock()Lnet/minecraft/world/level/block/Block;", ordinal = 1)
	)
	private Block cybercraft$cityWaterBlock(BlockState state, Operation<Block> original, @Local(argsOnly = true) Level level, @Local(ordinal = 0) BlockPos pos) {
		return state.isAir() && CyberWater.isCityWater(level, pos) ? Blocks.WATER : original.call(state);
	}

	@WrapOperation(
		method = "use",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/block/BucketPickup;pickupBlock(Lnet/minecraft/world/entity/LivingEntity;Lnet/minecraft/world/level/LevelAccessor;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;)Lnet/minecraft/world/item/ItemStack;"
		)
	)
	private ItemStack cybercraft$fillFromCityWater(
		BucketPickup block, LivingEntity user, LevelAccessor level, BlockPos pos, BlockState state, Operation<ItemStack> original
	) {
		if (state.isAir() && CyberWater.isCityWater(level, pos)) {
			return new ItemStack(Items.WATER_BUCKET);
		}
		return original.call(block, user, level, pos, state);
	}

	@WrapOperation(
		method = "emptyContents",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;setBlock(Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;I)Z")
	)
	private boolean cybercraft$pourIntoCityWater(Level level, BlockPos pos, BlockState state, int flags, Operation<Boolean> original) {
		if (state.getFluidState().is(FluidTags.WATER) && CyberWater.isCityWater(level, pos)) {
			return true;
		}
		return original.call(level, pos, state, flags);
	}
}
