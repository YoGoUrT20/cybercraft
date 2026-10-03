package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.world.CyberCollision;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.AABB;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;

/**
 * A dropped item that finds itself inside something turns its physics off and is pushed out of
 * the full block it's in. The host's world isn't blocks: there was nothing to push it out of, so
 * an item dropped into a pavement (a broken block sunk into it, say) fell through every surface
 * into the void. Lift it onto the surface instead, or at least keep it colliding.
 */
@Mixin(ItemEntity.class)
public abstract class ItemEntityMixin {
	/** Deeper than this, it isn't resting a little way into a floor: don't lift it onto anything. */
	@Unique
	private static final double MAX_LIFT = 0.6;

	@WrapOperation(
		method = "tick",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;noCollision(Lnet/minecraft/world/entity/Entity;Lnet/minecraft/world/phys/AABB;)Z")
	)
	private boolean cybercraft$liftOutOfHostWorld(Level level, Entity entity, AABB box, Operation<Boolean> original) {
		if (original.call(level, entity, box)) {
			return true;
		}
		double top = CyberCollision.voxelTopIn(box);
		if (Double.isNaN(top)) {
			return false; // inside a real block: vanilla pushes it out
		}
		double lift = top - box.minY + 1.0E-7;
		if (lift <= MAX_LIFT && original.call(level, entity, box.move(0.0, lift, 0.0))) {
			entity.setPos(entity.getX(), entity.getY() + lift, entity.getZ());
		}
		return true;
	}
}
