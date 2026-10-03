package dev.cybercraft.client.mixin;

import net.minecraft.client.renderer.item.ItemStackRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(ItemStackRenderState.class)
public interface ItemStackRenderStateAccessor {
	@Accessor("activeLayerCount")
	int cybercraft$activeLayerCount();

	@Accessor("layers")
	ItemStackRenderState.LayerRenderState[] cybercraft$layers();
}
