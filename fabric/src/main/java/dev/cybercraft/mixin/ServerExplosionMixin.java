package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import dev.cybercraft.world.CyberDigBlast;
import java.util.List;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft explosions in Night City: the plugin hurts every NPC in their reach (Proto.EV_EXPLOSION).
 * With destruction on (CyberDig.destruction; off by default, as digging into the city is) they also
 * blow the city's ground apart like blocks (CyberDigBlast).
 */
@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	@Unique
	private @Nullable CyberDigBlast cybercraft$blast;

	@Inject(method = "explode", at = @At("HEAD"))
	private void cybercraft$begin(CallbackInfoReturnable<Integer> cir) {
		this.cybercraft$blast = CyberDigBlast.begin((ServerExplosion) (Object) this);
	}

	@WrapOperation(
		method = "calculateExplodedPositions",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getBlockExplosionResistance(Lnet/minecraft/world/level/Explosion;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)Ljava/util/Optional;"
		)
	)
	private Optional<Float> cybercraft$cityResists(
		ExplosionDamageCalculator calculator, Explosion explosion, BlockGetter level, BlockPos pos, BlockState block, FluidState fluid, Operation<Optional<Float>> original
	) {
		Optional<Float> vanilla = original.call(calculator, explosion, level, pos, block, fluid);
		return this.cybercraft$blast != null ? this.cybercraft$blast.resistance(pos, vanilla) : vanilla;
	}

	@ModifyVariable(method = "explode", at = @At("STORE"), ordinal = 0)
	private List<BlockPos> cybercraft$cityBreaks(List<BlockPos> targets) {
		if (this.cybercraft$blast != null) {
			ServerExplosion self = (ServerExplosion) (Object) this;
			this.cybercraft$blast.materialize(targets, self.getBlockInteraction() != Explosion.BlockInteraction.KEEP
				&& self.getBlockInteraction() != Explosion.BlockInteraction.TRIGGER_BLOCK);
		}
		return targets;
	}

	@Inject(method = "explode", at = @At("RETURN"))
	private void cybercraft$tellPlugin(CallbackInfoReturnable<Integer> cir) {
		if (this.cybercraft$blast != null) {
			this.cybercraft$blast.finish();
			this.cybercraft$blast = null;
		}
		if (!CyberLink.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		CyberLink.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0);
	}
}
