package dev.cybercraft.client.mixin;

import com.mojang.authlib.minecraft.UserApiService;
import com.mojang.authlib.services.MinecraftServicesDiscoveryService;
import dev.cybercraft.client.SignIn;
import net.minecraft.client.Minecraft;
import net.minecraft.client.main.GameConfig;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Not signed in (SignIn), Minecraft's user services can only answer 401 ("Failed to fetch user
 * properties" and its stack trace in the log) before it falls back to the offline ones: those straight
 * away, as in its own offline developer mode.
 */
@Mixin(Minecraft.class)
public abstract class MinecraftSignInMixin {
	@Inject(method = "createUserApiService", at = @At("HEAD"), cancellable = true)
	private static void cybercraft$offline(MinecraftServicesDiscoveryService services, GameConfig config, CallbackInfoReturnable<UserApiService> cir) {
		if (!SignIn.isSession(config.user.user.getAccessToken())) {
			cir.setReturnValue(UserApiService.OFFLINE);
		}
	}
}
