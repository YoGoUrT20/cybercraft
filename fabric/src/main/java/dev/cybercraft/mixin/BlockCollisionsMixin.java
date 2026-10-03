package dev.cybercraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.cybercraft.world.CyberCollision;
import dev.cybercraft.world.CyberDig;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockCollisions;
import net.minecraft.world.level.CollisionGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Adds the city's geometry (the plugin's voxels, see CyberCollision) to every block-collision
 * query. Vanilla movement, step-up, onGround and fall-damage logic then run unchanged against it.
 */
@Mixin(BlockCollisions.class)
public abstract class BlockCollisionsMixin {
	@WrapOperation(
		method = "computeNext",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/phys/shapes/CollisionContext;getCollisionShape(Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/CollisionGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"
		)
	)
	private VoxelShape cybercraft$addCityShape(
		CollisionContext context, BlockState state, CollisionGetter level, BlockPos pos, Operation<VoxelShape> original
	) {
		VoxelShape blockShape = original.call(context, state, level, pos);
		// The walls of holes dug into the city (CyberDig): solid for everyone. Digging into the city
		// is off by design (DESIGN.md, Decisions), so normally there are none.
		if (state.isAir()) {
			VoxelShape wall = CyberDig.wallShape(level, pos);
			if (wall != null) {
				blockShape = blockShape.isEmpty() ? wall : Shapes.or(blockShape, wall);
			}
		}
		// The host's cars move, so they come as a layer of their own (Proto.COL_VEHICLES), never in
		// its voxels or triangles: solid for everyone.
		VoxelShape car = CyberCollision.vehicleShapeAt(pos);
		if (car != null) {
			blockShape = blockShape.isEmpty() ? car : Shapes.or(blockShape, car);
		}
		if (context instanceof EntityCollisionContext entityContext && CyberCollision.usesSmoothCollider(entityContext.getEntity())) {
			return blockShape; // this entity collides with the triangles instead (CyberCollider), once there are any
		}
		VoxelShape city = CyberCollision.shapeAt(pos);
		if (city == null) {
			return blockShape;
		}
		return blockShape.isEmpty() ? city : Shapes.or(blockShape, city);
	}
}
