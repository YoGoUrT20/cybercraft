// CyberCraft's objects in Night City, made through Codeware: a light for each of Minecraft's
// light-emitting blocks, a collider for each run of its solid blocks (so NPCs, cars and bullets
// meet the player's builds), and V's body hidden while Minecraft's own body is shown in her place.
//
// The plugin (Builds.cpp, Game.cpp) calls the global functions at the end through the game's RTTI.
// Entities are spawned from cybercraft\empty.ent (CyberCraft.archive: an entity with nothing in it)
// and get their light or collider once they have their entity ID (Entity/Initialize: at
// Entity/Assemble it isn't assigned yet, so nothing matched and no collider was ever added; World
// Builder adds its components at Initialize too). Without Codeware none of this is compiled,
// and the plugin finds nothing to call. The last two functions (is F Cyberpunk's right now, V's
// weapons locked away) are vanilla and always there.

@if(ModuleExists("Codeware"))
public class CyberCraftSpawn {
  public let id: EntityID;
  public let isLight: Bool;
  // Light
  public let color: Vector4;  // 0..1
  public let radius: Float;
  public let intensity: Float;
  public let flicker: Float;  // 0: steady
  public let shadows: Bool;
  // Collider
  public let halfExtents: Vector4;
  public let yaw: Float;  // degrees, counter-clockwise seen from above
  public let obstacle: Bool;
}

@if(ModuleExists("Codeware"))
public class CyberCraftHost extends ScriptableService {
  private let m_pending: array<ref<CyberCraftSpawn>>;
  private let m_hidden: array<wref<IComponent>>;
  private let m_lightsMade: Int32;
  private let m_collidersMade: Int32;

  private cb func OnLoad() {
    let callbacks = GameInstance.GetCallbackSystem();
    // Matched by ID, which an entity only has from Initialize on: no target (the static entity
    // system's tags are kept by ID too), as World Builder does. Assemble stays in case a game
    // version assigns the ID earlier; whichever finds the entity pending first adds the component.
    callbacks.RegisterCallback(n"Entity/Initialize", this, n"OnAssemble");
    callbacks.RegisterCallback(n"Entity/Assemble", this, n"OnAssemble")
      .AddTarget(StaticEntityTarget.Tag(n"CyberCraft"));
  }

  public static func Get() -> ref<CyberCraftHost> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"CyberCraftHost") as CyberCraftHost;
  }

  // Ready to spawn: a game session is running and Codeware's static entity system is up.
  public static func IsReady() -> Bool {
    let system = GameInstance.GetStaticEntitySystem();
    return IsDefined(CyberCraftHost.Get()) && IsDefined(system) && system.IsReady();
  }

  // A point light at a Cyberpunk position. color: RGB 0..1. flicker: 0 steady, else its strength.
  public static func SpawnLight(position: Vector4, color: Vector4, radius: Float, intensity: Float, flicker: Float, shadows: Bool) -> EntityID {
    let data = new CyberCraftSpawn();
    data.isLight = true;
    data.color = color;
    data.radius = radius;
    data.intensity = intensity;
    data.flicker = flicker;
    data.shadows = shadows;
    return CyberCraftHost.Spawn(position, data);
  }

  // A box collider centred on a Cyberpunk position, half extents in metres, turned yaw degrees
  // about its centre (Minecraft's grid can be: Grid.cpp).
  public static func SpawnCollider(position: Vector4, halfExtents: Vector4, yaw: Float, obstacle: Bool) -> EntityID {
    let data = new CyberCraftSpawn();
    data.isLight = false;
    data.halfExtents = halfExtents;
    data.yaw = yaw;
    data.obstacle = obstacle;
    return CyberCraftHost.Spawn(position, data);
  }

  public static func Despawn(id: EntityID) -> Bool {
    let host = CyberCraftHost.Get();
    if IsDefined(host) {
      host.Forget(id);
    }
    return GameInstance.GetStaticEntitySystem().DespawnEntity(id);
  }

  public static func DespawnAll() {
    let host = CyberCraftHost.Get();
    if IsDefined(host) {
      ArrayClear(host.m_pending);
    }
    GameInstance.GetStaticEntitySystem().DespawnTagged(n"CyberCraft");
  }

  // V's body off while Minecraft's body stands in for it, and back on: every visual component of the
  // player entity, and of the items she wears (her head, cyberarms and clothes are entities of their
  // own, in her attachment slots). Only the components this turned off are turned back on. Hiding
  // again hides only what came on since (clothes put on in the inventory). Returns how many changed.
  public static func SetPlayerBodyVisible(visible: Bool) -> Int32 {
    let host = CyberCraftHost.Get();
    let player = GetPlayer(GetGameInstance());
    if !IsDefined(host) || !IsDefined(player) {
      return 0;
    }
    let count = 0;
    if visible {
      for component in host.m_hidden {
        if IsDefined(component) {
          component.Toggle(true);
          count += 1;
        }
      }
      ArrayClear(host.m_hidden);
      return count;
    }
    count = host.Hide(player.GetComponents());
    let transactions = GameInstance.GetTransactionSystem(player.GetGame());
    for slot in CyberCraftHost.BodySlots() {
      let item = transactions.GetItemInSlot(player, slot);
      if IsDefined(item) {
        count += host.Hide(item.GetComponents());
      }
    }
    return count;
  }

  // The slots of what V wears, all in the game's TweakDB (2.31). Weapons aren't here: CyberCraft
  // keeps them holstered.
  private static func BodySlots() -> array<TweakDBID> {
    return [
      t"AttachmentSlots.TppHead", t"AttachmentSlots.Head", t"AttachmentSlots.Face", t"AttachmentSlots.Eyes",
      t"AttachmentSlots.Chest", t"AttachmentSlots.Torso", t"AttachmentSlots.Legs", t"AttachmentSlots.Feet",
      t"AttachmentSlots.Outfit", t"AttachmentSlots.UnderwearTop", t"AttachmentSlots.UnderwearBottom",
      t"AttachmentSlots.RightArm", t"AttachmentSlots.LeftArm", t"AttachmentSlots.ArmsCyberwareGeneralSlot",
      t"AttachmentSlots.PersonalLink"
    ];
  }

  private func Hide(components: array<ref<IComponent>>) -> Int32 {
    let count = 0;
    for component in components {
      let weak: wref<IComponent> = component;
      if component.IsA(n"entIVisualComponent") && component.IsEnabled() && !ArrayContains(this.m_hidden, weak) {
        component.Toggle(false);
        ArrayPush(this.m_hidden, weak);
        count += 1;
      }
    }
    return count;
  }

  private static func Spawn(position: Vector4, data: ref<CyberCraftSpawn>) -> EntityID {
    let host = CyberCraftHost.Get();
    let system = GameInstance.GetStaticEntitySystem();
    if !IsDefined(host) || !IsDefined(system) {
      let none: EntityID;
      return none;
    }
    let spec = new StaticEntitySpec();
    spec.templatePath = r"cybercraft\\empty.ent";
    spec.position = position;
    // About the vertical: (0, 0, sin, cos) of half the turn. Lights don't turn.
    let half = data.isLight ? 0.0 : Deg2Rad(data.yaw) * 0.5;
    spec.orientation = new Quaternion(0.0, 0.0, SinF(half), CosF(half));
    spec.attached = true;
    spec.tags = [n"CyberCraft"];
    data.id = system.SpawnEntity(spec);
    if EntityID.IsDefined(data.id) {
      ArrayPush(host.m_pending, data);
    }
    return data.id;
  }

  private func Forget(id: EntityID) {
    let i = ArraySize(this.m_pending) - 1;
    while i >= 0 {
      if this.m_pending[i].id == id {
        ArrayErase(this.m_pending, i);
      }
      i -= 1;
    }
  }

  // Every entity the game initializes comes through here (no target), so nothing pending is the
  // quick way out.
  private cb func OnAssemble(event: ref<EntityLifecycleEvent>) {
    if ArraySize(this.m_pending) == 0 {
      return;
    }
    let entity = event.GetEntity();
    if !IsDefined(entity) {
      return;
    }
    let id = entity.GetEntityID();
    if !EntityID.IsDefined(id) {
      return;
    }
    let i = 0;
    while i < ArraySize(this.m_pending) {
      let data = this.m_pending[i];
      if data.id == id {
        ArrayErase(this.m_pending, i);
        if data.isLight {
          entity.AddComponent(CyberCraftHost.MakeLight(data));
          this.m_lightsMade += 1;
        } else {
          entity.AddComponent(CyberCraftHost.MakeCollider(data));
          this.m_collidersMade += 1;
        }
        return;
      }
      i += 1;
    }
  }

  // How many lights and colliders have been given their component so far, against how many were
  // spawned: none made means the entities never came through OnAssemble.
  public static func Made(lights: Bool) -> Int32 {
    let host = CyberCraftHost.Get();
    if !IsDefined(host) {
      return 0;
    }
    return lights ? host.m_lightsMade : host.m_collidersMade;
  }

  private static func MakeLight(data: ref<CyberCraftSpawn>) -> ref<LightComponent> {
    let light = new LightComponent();
    light.name = n"cybercraft_light";
    light.type = ELightType.LT_Point;
    light.color = new Color(CyberCraftHost.Byte(data.color.X), CyberCraftHost.Byte(data.color.Y), CyberCraftHost.Byte(data.color.Z), Cast<Uint8>(255));
    light.radius = data.radius;
    light.intensity = data.intensity;
    light.enableLocalShadows = data.shadows;
    light.useInParticles = true;
    light.useInTransparents = true;
    light.sceneDiffuse = true;
    light.sceneSpecularScale = Cast<Uint8>(100);
    light.sourceRadius = 0.05;
    light.attenuation = rendLightAttenuation.LA_InverseSquare;
    if data.flicker > 0.0 {
      let flicker: rendSLightFlickering;
      flicker.positionOffset = 0.02;
      flicker.flickerStrength = data.flicker;
      flicker.flickerPeriod = 0.15;
      light.flicker = flicker;
    }
    light.isEnabled = true;
    return light;
  }

  private static func MakeCollider(data: ref<CyberCraftSpawn>) -> ref<ColliderComponent> {
    let box = new physicsColliderBox();
    box.halfExtents = new Vector3(data.halfExtents.X, data.halfExtents.Y, data.halfExtents.Z);
    box.isObstacle = data.obstacle;
    // A real material the city itself doesn't use (only the VR scenes do): the plugin's own collision
    // rays recognise these boxes by it and pass through them (Collision.cpp, kOwnMaterial).
    box.material = n"character_vr.physmat";
    // What the game's own invisible walls use (sq025_crash_invisible_collider.ent): preset and masks
    // (mask1 the groups it's in: Static, PlayerBlocker, VehicleBlocker, TankBlocker, NPCBlocker;
    // mask2 the groups it collides with). Bits are the game's collision layers in order: Player,
    // AI, Static, Dynamic, Vehicle, Tank, ... PlayerBlocker 14, VehicleBlocker 15, TankBlocker 16,
    // NPCBlocker 18. Two bodies meet if either lists the other: a car's chassis collides with
    // VehicleBlocker, which is how the city's own World Static stops cars. Vehicle and Tank (bits
    // 4, 5) are in mask2 as well, as the game's Moving Kinematic preset has them. (Cars drove
    // through the boxes because the collider was never added: see OnLoad.)
    let simulation: SimulationFilter;
    simulation.mask1 = 376836ul;
    simulation.mask2 = 5118ul;
    let query: QueryFilter;
    query.mask1 = 0ul;
    query.mask2 = 71942148ul;
    let filter = new physicsFilterData();
    filter.preset = n"World Static";
    filter.simulationFilter = simulation;
    filter.queryFilter = query;
    let shapes: array<ref<physicsICollider>>;
    ArrayPush(shapes, box);
    let collider = new ColliderComponent();
    collider.name = n"cybercraft_collider";
    collider.colliders = shapes;
    collider.filterData = filter;
    collider.simulationType = physicsSimulationType.Static;
    collider.isEnabled = true;
    return collider;
  }

  private static func Byte(value: Float) -> Uint8 {
    return Cast<Uint8>(Cast<Int32>(ClampF(value, 0.0, 1.0) * 255.0));
  }
}

// What the plugin calls. Global functions, because the game's RTTI lists a script class's own
// functions but none of its static ones: the plugin found CyberCraftHost with OnLoad, Forget and
// OnAssemble and nothing else. Global ones are listed by name, like GetPlayer.

@if(ModuleExists("Codeware"))
public static func CyberCraftIsReady() -> Bool {
  return CyberCraftHost.IsReady();
}

@if(ModuleExists("Codeware"))
public static func CyberCraftSpawnLight(position: Vector4, color: Vector4, radius: Float, intensity: Float, flicker: Float, shadows: Bool) -> EntityID {
  return CyberCraftHost.SpawnLight(position, color, radius, intensity, flicker, shadows);
}

@if(ModuleExists("Codeware"))
public static func CyberCraftSpawnCollider(position: Vector4, halfExtents: Vector4, yaw: Float, obstacle: Bool) -> EntityID {
  return CyberCraftHost.SpawnCollider(position, halfExtents, yaw, obstacle);
}

@if(ModuleExists("Codeware"))
public static func CyberCraftDespawn(id: EntityID) -> Bool {
  return CyberCraftHost.Despawn(id);
}

@if(ModuleExists("Codeware"))
public static func CyberCraftDespawnAll() {
  CyberCraftHost.DespawnAll();
}

@if(ModuleExists("Codeware"))
public static func CyberCraftSetPlayerBodyVisible(visible: Bool) -> Int32 {
  return CyberCraftHost.SetPlayerBodyVisible(visible);
}

@if(ModuleExists("Codeware"))
public static func CyberCraftComponentsMade(lights: Bool) -> Int32 {
  return CyberCraftHost.Made(lights);
}

// Cyberpunk offers something to do with F right now: an interaction (doors, terminals, vehicles,
// pick-ups), loot or dialogue choices, read the way the game's own interaction HUD reads them
// (InteractionUIBase). The plugin (Input.cpp) gives F to Cyberpunk only then, and to Minecraft
// (swap hands) the rest of the time. Vanilla only, so it's there without Codeware too.
public static func CyberCraftInteractionShown() -> Bool {
  let defs = GetAllBlackboardDefs().UIInteractions;
  let board = GameInstance.GetBlackboardSystem(GetGameInstance()).Get(defs);
  if !IsDefined(board) {
    return false;
  }
  let interaction = FromVariant<InteractionChoiceHubData>(board.GetVariant(defs.InteractionChoiceHub));
  let loot = FromVariant<LootData>(board.GetVariant(defs.LootData));
  let dialogs = FromVariant<DialogChoiceHubs>(board.GetVariant(defs.DialogChoiceHubs));
  return interaction.active || loot.isActive || ArraySize(dialogs.choiceHubs) > 0;
}

// No Cyberpunk weapons while Minecraft runs. Keys Minecraft has no control on stay Cyberpunk's
// (Input.cpp), and Alt (SwitchItem, the weapon wheel) pulled a gun out in the middle of Minecraft.
// GameplayRestriction.NoCombat is what the game puts on V where fighting isn't allowed: it holsters
// what she holds and refuses weapons, grenades and arm cyberware. Applied unsaved, so a save never
// keeps it, and with CyberCraft as its source, so turning it off leaves the game's own alone; while
// the game's own is on, it isn't added. Vanilla only. Returns whether CyberCraft's is on.
public static func CyberCraftSetWeaponsBlocked(blocked: Bool) -> Bool {
  let player = GetPlayer(GetGameInstance());
  if !IsDefined(player) {
    return false;
  }
  let system = GameInstance.GetStatusEffectSystem(player.GetGame());
  let id = player.GetEntityID();
  let effect = t"GameplayRestriction.NoCombat";
  let source = t"CyberCraft.Minecraft";
  let applied: array<ref<StatusEffect>>;
  system.GetAppliedEffectsWithID(id, effect, applied);
  let ours = false;
  for status in applied {
    if status.GetInstigatorStaticDataID() == source {
      ours = true;
    }
  }
  if blocked && ArraySize(applied) == 0 {
    let direction: Vector4;
    return system.ApplyStatusEffect(id, effect, source, id, 1u, direction, false);
  }
  if !blocked && ours {
    system.RemoveStatusEffect(id, effect, 1u);
    return false;
  }
  return ours;
}
