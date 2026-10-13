package dev.cybercraft.client.mixin;

import com.mojang.realmsclient.RealmsAvailability;
import dev.cybercraft.client.SignIn;
import java.util.concurrent.CompletableFuture;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Not signed in, Realms can only answer 401 ("Could not authorize you against Realms server: ...
 * SignedJWT: 0" in the log): don't ask, as Minecraft itself doesn't in its offline developer mode.
 * SignIn says what's wrong instead. RealmsClientMixin does the same for the feature flags.
 */
@Mixin(RealmsAvailability.class)
public abstract class RealmsAvailabilityMixin {
	@Inject(method = "check", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$offline(CallbackInfoReturnable<CompletableFuture<RealmsAvailability.Result>> cir) {
		if (SignIn.offline()) {
			cir.setReturnValue(CompletableFuture.completedFuture(new RealmsAvailability.Result(RealmsAvailability.Type.AUTHENTICATION_ERROR)));
		}
	}
}
