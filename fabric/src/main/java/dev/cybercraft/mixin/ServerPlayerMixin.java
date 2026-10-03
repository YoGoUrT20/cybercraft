package dev.cybercraft.mixin;

import dev.cybercraft.CyberCraft;
import dev.cybercraft.combat.CyberCombat;
import dev.cybercraft.combat.NpcEntity;
import dev.cybercraft.link.CyberLink;
import dev.cybercraft.link.Proto;
import dev.cybercraft.net.CyberNet;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.server.level.ServerPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ServerPlayer.class)
public abstract class ServerPlayerMixin {
	/**
	 * Critical hits on an NPC are flagged on the hit sent to the plugin (Proto.HIT_CRITICAL). The
	 * plugin doesn't use the flag yet; the crit's extra damage is in the hit either way.
	 */
	@Inject(method = "crit", at = @At("HEAD"))
	private void cybercraft$critNpc(Entity entity, CallbackInfo ci) {
		if (entity instanceof NpcEntity npc) {
			npc.markCritical();
		}
	}

	/**
	 * Dying in Minecraft kills V (if Minecraft was driving her): the host's through the link, a
	 * guest's through their own.
	 */
	@Inject(method = "die", at = @At("HEAD"))
	private void cybercraft$killV(DamageSource source, CallbackInfo ci) {
		ServerPlayer self = (ServerPlayer) (Object) this;
		int attacker = CyberCombat.attackerId(source);
		if (!CyberNet.isHost(self)) {
			if (ServerPlayNetworking.canSend(self, CyberNet.Died.TYPE)) {
				ServerPlayNetworking.send(self, new CyberNet.Died(attacker));
			}
			CyberCraft.LOG.info("CyberCraft: guest {} died ({}); telling their plugin", self.getPlainTextName(), source.getMsgId());
			return;
		}
		if (CyberLink.active()) {
			CyberLink.pushEvent(Proto.EV_PLAYER_DIED, attacker, 0, 0, 0, 0, 0);
			CyberCraft.LOG.info("CyberCraft: player died ({}); telling the plugin", source.getMsgId());
		}
	}
}
