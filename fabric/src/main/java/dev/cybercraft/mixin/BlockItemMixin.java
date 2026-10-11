package dev.cybercraft.mixin;

import dev.cybercraft.world.CyberDig;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.context.BlockPlaceContext;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Stone put back where the city was dug out gives the city back (CyberDig.placedBack). */
@Mixin(BlockItem.class)
public abstract class BlockItemMixin {
	@Inject(method = "placeBlock", at = @At("RETURN"))
	private void cybercraft$placedBack(BlockPlaceContext context, BlockState state, CallbackInfoReturnable<Boolean> cir) {
		if (cir.getReturnValueZ() && context.getLevel() instanceof ServerLevel level) {
			CyberDig.placedBack(level, context.getClickedPos());
		}
	}
}
