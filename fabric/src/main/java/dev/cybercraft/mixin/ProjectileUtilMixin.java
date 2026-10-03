package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.world.CyberClip;
import net.minecraft.world.entity.projectile.ProjectileUtil;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/** Thrown projectiles (snowballs, eggs, pearls, potions) and spear reach checks see the city's surfaces. */
@Mixin(ProjectileUtil.class)
public abstract class ProjectileUtilMixin {
	@WrapOperation(
		method = { "getHitResult", "getHitEntitiesAlong" },
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clipIncludingBorder(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private static BlockHitResult cybercraft$clipCity(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return CyberClip.refine(context.getFrom(), context.getTo(), original.call(level, context), CyberClip.Use.PROJECTILE);
	}
}
