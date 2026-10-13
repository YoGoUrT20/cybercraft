package dev.cybercraft.client.mixin;

import com.mojang.realmsclient.client.RealmsClient;
import dev.cybercraft.client.SignIn;
import java.util.Set;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Not signed in, Realms' feature flags (asked for as Minecraft starts) can only fail, with an error
 * and its stack trace in the log: none, as in Minecraft's own offline developer mode. See
 * RealmsAvailabilityMixin.
 */
@Mixin(RealmsClient.class)
public abstract class RealmsClientMixin {
	@Inject(method = "fetchFeatureFlags", at = @At("HEAD"), cancellable = true)
	private void cybercraft$offline(CallbackInfoReturnable<Set<String>> cir) {
		if (SignIn.offline()) {
			cir.setReturnValue(Set.of());
		}
	}
}
