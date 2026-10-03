package dev.cybercraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import com.mojang.blaze3d.vertex.PoseStack;
import dev.cybercraft.CyberCraft;
import dev.cybercraft.client.render.WorldExporter;
import net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.state.level.FirstPersonHandsAndItemsRenderState;
import net.minecraft.client.renderer.state.level.PlayerRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;

/**
 * The first-person hands and held items go to Cyberpunk as geometry instead of into Minecraft's
 * overlay, when the plugin draws them (CYBER_DRAWS_HAND): there they're lit by Cyberpunk's picture
 * like the blocks, rather than painted flat over it with Minecraft's light. Minecraft's own
 * renderer gets nothing to draw.
 */
@Mixin(GameRenderer.class)
public abstract class FirstPersonHandMixin {
	@Unique
	private static boolean cybercraft$warned;

	@WrapOperation(
		method = "renderItemInHand",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/client/renderer/FirstPersonHandsAndItemsRenderer;submitHandsWithItems(FLcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/level/FirstPersonHandsAndItemsRenderState;)V"
		)
	)
	private void cybercraft$handsToCyberpunk(FirstPersonHandsAndItemsRenderer renderer, float partialTick, PoseStack poseStack, SubmitNodeCollector collector,
		PlayerRenderState player, FirstPersonHandsAndItemsRenderState hands, Operation<Void> original, @Local(argsOnly = true) CameraRenderState camera) {
		SubmitNodeCollector capture = WorldExporter.handCollector();
		if (capture == null) {
			original.call(renderer, partialTick, poseStack, collector, player, hands);
			return;
		}
		// Minecraft poses the hands against the inverse view rotation and puts the rotation back in
		// the model-view matrix; folding it into the pose leaves them in view space, where the plugin
		// draws them with Minecraft's hand projection. On a copy, so a failed capture leaves
		// Minecraft's own stack as it was.
		PoseStack view = new PoseStack();
		view.last().set(poseStack.last());
		view.last().pose().mulLocal(camera.viewRotationMatrix);
		try {
			original.call(renderer, partialTick, view, capture, player, hands);
			WorldExporter.handDone(camera.hudFov, true);
		} catch (RuntimeException e) {
			WorldExporter.handDone(camera.hudFov, false);
			if (!cybercraft$warned) {
				cybercraft$warned = true;
				CyberCraft.LOG.warn("CyberCraft: couldn't capture the hands for Cyberpunk; Minecraft draws them", e);
			}
			original.call(renderer, partialTick, poseStack, collector, player, hands);
		}
	}
}
