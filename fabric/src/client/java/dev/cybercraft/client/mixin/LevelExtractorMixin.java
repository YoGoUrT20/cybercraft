package dev.cybercraft.client.mixin;

import dev.cybercraft.client.render.WorldExporter;
import net.minecraft.client.renderer.extract.LevelExtractor;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Every block change (and chunk load) marks its 16^3 section to be meshed again for the plugin. */
@Mixin(LevelExtractor.class)
public abstract class LevelExtractorMixin {
	@Inject(method = "setSectionDirty(IIIZ)V", at = @At("HEAD"))
	private void cybercraft$sectionDirty(int sectionX, int sectionY, int sectionZ, boolean playerChanged, CallbackInfo ci) {
		if (playerChanged) {
			WorldExporter.markDirtyNow(sectionX, sectionY, sectionZ);
		} else {
			WorldExporter.markDirty(sectionX, sectionY, sectionZ);
		}
	}
}
