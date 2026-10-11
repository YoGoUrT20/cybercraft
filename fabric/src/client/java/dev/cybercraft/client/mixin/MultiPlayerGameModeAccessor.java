package dev.cybercraft.client.mixin;

import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Vanilla's pause between broken blocks, which digging into the city keeps to as well (CyberDigClient). */
@Mixin(MultiPlayerGameMode.class)
public interface MultiPlayerGameModeAccessor {
	@Accessor("destroyDelay")
	int cybercraft$destroyDelay();

	@Accessor("destroyDelay")
	void cybercraft$setDestroyDelay(int ticks);
}
