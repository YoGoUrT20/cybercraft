package dev.cybercraft.client.mixin;

import dev.cybercraft.client.CyberClient;
import dev.cybercraft.client.FrameStats;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Inject(method = "runTick", at = @At("HEAD"))
	private void cybercraft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.frame();
		CyberClient.beginFrame();
	}

	@Inject(method = "renderFrame", at = @At("HEAD"))
	private void cybercraft$beginRender(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.end(FrameStats.TICK);
		FrameStats.begin(FrameStats.RENDER);
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void cybercraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.end(FrameStats.RENDER);
		FrameStats.begin(FrameStats.EXPORT);
		CyberClient.afterRender();
		FrameStats.end(FrameStats.EXPORT);
	}

	@Inject(method = "renderFrame", at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/api/commands/CommandEncoder;submit()V"))
	private void cybercraft$beforeSubmit(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.begin(FrameStats.GPU);
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/api/commands/CommandEncoder;submit()V", shift = At.Shift.AFTER)
	)
	private void cybercraft$afterSubmit(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.end(FrameStats.GPU);
	}

	@Inject(method = "renderFrame", at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/api/device/GpuSurface;present()V"))
	private void cybercraft$beforePresent(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.begin(FrameStats.PRESENT);
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lcom/mojang/renderpearl/api/device/GpuSurface;present()V", shift = At.Shift.AFTER)
	)
	private void cybercraft$afterPresent(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.end(FrameStats.PRESENT);
	}

	@Inject(method = "renderFrame", at = @At("TAIL"))
	private void cybercraft$pace(boolean advanceGameTime, CallbackInfo ci) {
		FrameStats.begin(FrameStats.PACE);
		CyberClient.paceFrame();
		FrameStats.end(FrameStats.PACE);
	}
}
