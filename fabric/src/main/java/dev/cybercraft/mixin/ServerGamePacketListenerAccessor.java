package dev.cybercraft.mixin;

import net.minecraft.server.network.ServerGamePacketListenerImpl;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** Vanilla's pick-block for a block the city shows (CyberNet.PickCity). */
@Mixin(ServerGamePacketListenerImpl.class)
public interface ServerGamePacketListenerAccessor {
	@Invoker("tryPickItem")
	void cybercraft$tryPickItem(ItemStack stack);
}
