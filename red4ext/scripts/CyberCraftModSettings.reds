// CyberCraft's entry in Cyberpunk's Mod Settings (jackhumbert's Mod Settings mod): "Enable
// CyberCraft", which turns the whole mod off and on, an "Open Minecraft settings" row that opens
// Minecraft's options screen over the game, and "Minecraft scale", how big Minecraft's world is
// against Night City (the plugin reads it from user.ini, Grid.cpp).
//
// Mod Settings has no button, so the row is a Bool that never changes: clicking it, or left and
// right on it, leaves a request in CyberCraftRequests and closes the pause menu the way its Resume
// does. The plugin collects the request on its next frame (ModSettings.cpp) and Minecraft opens its
// options, which show now that the menu is gone. Mod Settings' own classes can't be wrapped
// (redscript only lets annotations touch the game's), so this hooks the game's
// SettingsSelectorController, which every row extends. Without Mod Settings there's no row.

// Requests from Cyberpunk's menus for the plugin, which takes them every frame a save is loaded.
public class CyberCraftRequests extends ScriptableSystem {
  private let m_minecraftSettings: Bool;

  public static func Get() -> ref<CyberCraftRequests> {
    return GameInstance.GetScriptableSystemsContainer(GetGameInstance()).Get(n"CyberCraftRequests") as CyberCraftRequests;
  }

  public func AskMinecraftSettings() {
    this.m_minecraftSettings = true;
  }

  public func TakeMinecraftSettings() -> Bool {
    let asked = this.m_minecraftSettings;
    this.m_minecraftSettings = false;
    return asked;
  }
}

// What the plugin calls (a global function: the game's RTTI lists those by name).
public static func CyberCraftTakeMinecraftSettingsRequest() -> Bool {
  let requests = CyberCraftRequests.Get();
  return IsDefined(requests) && requests.TakeMinecraftSettings();
}

// The plugin reads "Enable CyberCraft" from Mod Settings' user.ini, where it saves what is accepted
// (ModSettings.cpp): in the main menu too, and before Minecraft would be started.
@if(ModuleExists("ModSettingsModule"))
public class CyberCraftModSettings extends IScriptable {
  @runtimeProperty("ModSettings.mod", "CyberCraft")
  @runtimeProperty("ModSettings.displayName", "Enable CyberCraft")
  @runtimeProperty("ModSettings.description", "Off: Cyberpunk as it is without CyberCraft. Minecraft lets go of V, nothing of it is drawn, and its world pauses until this is on again. Off when the game starts: Minecraft isn't started either.")
  public let cyberCraftEnabled: Bool = true;

  @runtimeProperty("ModSettings.mod", "CyberCraft")
  @runtimeProperty("ModSettings.displayName", "Open Minecraft settings")
  @runtimeProperty("ModSettings.description", "Click to open Minecraft's options (video, sound, controls, skin) over the game. In game only, while Minecraft drives V.")
  public let cyberCraftOpenMinecraftSettings: Bool = false;

  @runtimeProperty("ModSettings.mod", "CyberCraft")
  @runtimeProperty("ModSettings.displayName", "Minecraft scale")
  @runtimeProperty("ModSettings.description", "Metres of Night City per Minecraft block. Blocks, mobs, players and you scale with it, and your view sits at Minecraft's eye. 1: a block is a metre and Minecraft's player as tall as V. Blocks you placed move with it, and everyone in a shared world needs the same.")
  @runtimeProperty("ModSettings.step", "0.05")
  @runtimeProperty("ModSettings.min", "0.4")
  @runtimeProperty("ModSettings.max", "1.0")
  public let cyberCraftMetresPerBlock: Float = 0.75;
}

@if(ModuleExists("ModSettingsModule"))
@addMethod(SettingsSelectorController)
private func CyberCraftIsMinecraftSettings() -> Bool {
  return Equals(this.GetVarName(), n"cyberCraftOpenMinecraftSettings");
}

// The row was pressed: no toggle, Minecraft's settings instead.
@if(ModuleExists("ModSettingsModule"))
@addMethod(SettingsSelectorController)
private func CyberCraftPressed(e: ref<inkPointerEvent>, pressed: Bool) {
  if !pressed || e.IsHandled() {
    return;
  }
  e.Handle();  // the row and its switch both hear a click on the switch
  let requests = CyberCraftRequests.Get();
  if this.m_IsPreGame || !IsDefined(requests) {
    inkTextRef.SetText(this.m_LabelText, "Minecraft settings open in game, once Minecraft drives V");
    return;
  }
  this.PlaySound(n"Button", n"OnPress");
  requests.AskMinecraftSettings();
  // Leaving through here keeps what was changed, as Back does.
  ModSettings.AcceptChanges();
  // What the pause menu's Resume sends: back to the game, where Minecraft's screen shows.
  GameInstance.GetUISystem(GetGameInstance()).QueueMenuEvent(n"OnClosePauseMenu");
}

@if(ModuleExists("ModSettingsModule"))
@wrapMethod(SettingsSelectorController)
protected cb func OnLeft(e: ref<inkPointerEvent>) -> Bool {
  if this.CyberCraftIsMinecraftSettings() {
    this.CyberCraftPressed(e, e.IsAction(n"click"));
    return true;
  }
  return wrappedMethod(e);
}

@if(ModuleExists("ModSettingsModule"))
@wrapMethod(SettingsSelectorController)
protected cb func OnRight(e: ref<inkPointerEvent>) -> Bool {
  if this.CyberCraftIsMinecraftSettings() {
    this.CyberCraftPressed(e, e.IsAction(n"click"));
    return true;
  }
  return wrappedMethod(e);
}

@if(ModuleExists("ModSettingsModule"))
@wrapMethod(SettingsSelectorController)
protected cb func OnShortcutPress(e: ref<inkPointerEvent>) -> Bool {
  if this.CyberCraftIsMinecraftSettings() {
    this.CyberCraftPressed(e, e.IsAction(n"click") || e.IsAction(n"option_switch_prev_settings") || e.IsAction(n"option_switch_next_settings"));
    return true;
  }
  return wrappedMethod(e);
}

// Holding left or right on the row doesn't toggle it either.
@if(ModuleExists("ModSettingsModule"))
@wrapMethod(SettingsSelectorController)
protected cb func OnShortcutRepeat(e: ref<inkPointerEvent>) -> Bool {
  if this.CyberCraftIsMinecraftSettings() {
    return true;
  }
  return wrappedMethod(e);
}
