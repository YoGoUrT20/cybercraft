package dev.cybercraft.mixin;

import dev.cybercraft.world.CyberDigBlast;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.boss.wither.WitherBoss;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * The wither, hurt, breaks every block round it: the city's cells there break too, as an explosion
 * breaks them (CyberDigBlast), before Minecraft's own loop over the same box takes them like blocks.
 */
@Mixin(WitherBoss.class)
public abstract class WitherBossMixin {
	@Inject(
		method = "customServerAiStep",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/core/BlockPos;betweenClosed(IIIIII)Ljava/lang/Iterable;")
	)
	private void cybercraft$breakCity(ServerLevel level, CallbackInfo ci) {
		WitherBoss self = (WitherBoss) (Object) this;
		// The box Minecraft's loop goes over (WitherBoss.customServerAiStep).
		int width = Mth.floor(self.getBbWidth() / 2.0F + 1.0F);
		int height = Mth.floor(self.getBbHeight());
		CyberDigBlast.breakBox(level, new BlockPos(self.getBlockX() - width, self.getBlockY(), self.getBlockZ() - width),
			new BlockPos(self.getBlockX() + width, self.getBlockY() + height, self.getBlockZ() + width));
	}
}
