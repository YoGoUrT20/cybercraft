# CyberCraft — Design Doc

> Play Cyberpunk 2077 while *being* a Minecraft player: real Minecraft movement physics, inventory,
> items, block placing and combat, inside the real Night City, able to fight its NPCs.

In this document "host" means Cyberpunk 2077 and the RED4ext plugin in it. In the protocol,
`Cyber` names (`CyberState`, `kCyberInGame`) are the host's side and `Mc` names Minecraft's.

What is built and what isn't is in [CYBERCRAFT.md](CYBERCRAFT.md); what to build next is in
[ROADMAP.md](ROADMAP.md).

---

## 1. Core principle

**Neither game is rewritten.** Minecraft runs its own, unmodified game logic: movement, collision
resolution, combat math, inventory, crafting, block logic, rendering of items and hands. Cyberpunk
runs its own world: the city, NPCs and their AI, quests, vehicles, saves.

The two mods only **translate** between them:

- Cyberpunk tells Minecraft *what the world is shaped like*, *where the NPCs are*, *what time it is*
  and *what the weather is*.
- Minecraft tells Cyberpunk *where the player is*, *what the player hit*, and *what to draw*.

If we ever find ourselves re-implementing a Minecraft mechanic in C++ or a Cyberpunk mechanic in
Java, the design has gone wrong.

## 2. Target environment

| Thing | Value | Notes |
|---|---|---|
| Cyberpunk 2077 | **2.31** (developed on it) | D3D12. DLSS for depth occlusion (§9) |
| RED4ext | Current release | The plugin is a RED4ext plugin; RED4ext.SDK is header-only |
| Codeware | Recommended | Spawning lights and colliders, hiding V, setting the weather (`scripts/CyberCraft.reds`) |
| Cyber Engine Tweaks | Optional | Only for the dev console (`tools/cet.ps1`) |
| Minecraft | **26.3 + Fabric** (fabric-api 0.161.0+26.3) | 26.x ships unobfuscated, so Mixins target Mojang names |
| Java | 25 | Minecraft 26.x needs it |
| C++ toolchain | Visual Studio 2022+, CMake 3.25+, Git | |

## 3. Components

```
┌──────────────── Cyberpunk2077.exe ─────────────────┐        ┌──────────────── javaw.exe (Minecraft 26.3) ────────────────┐
│  CyberCraft.dll  (RED4ext plugin)                   │        │  cybercraft (Fabric mod)                                   │
│                                                     │        │                                                            │
│  Collision    ─ ray-cast voxels around V ───────────┼──────▶ │  CyberCollision → injected into MC collision queries        │
│  Combat       ─ NPCs to mirror (pos, box, hp) ──────┼──────▶ │  actor stand-ins (invisible, hittable)                     │
│  Input        ─ raw keyboard / mouse ───────────────┼──────▶ │  input handlers (as if MC window had focus)                │
│  Combat       ─ "V lost X health" ──────────────────┼──────▶ │  player.hurt()                                             │
│  Environment  ─ clock, weather ─────────────────────┼──────▶ │  CyberClock → overworld clock, weather                       │
│                                                     │        │                                                            │
│  Puppet       ◀─ player pos / look ─────────────────┼─────── │  real MC LocalPlayer physics                               │
│  Combat       ◀─ "you hit NPC n for X" ─────────────┼─────── │  stand-in hurt() hook                                      │
│  Environment  ◀─ /time, /weather ───────────────────┼─────── │  CyberClock                                                  │
│  Overlay      ◀─ hand / HUD / GUI image ────────────┼─────── │  offscreen render, triple buffer                          │
│  World        ◀─ block meshes, entities, atlases ───┼─────── │  WorldExporter (MC's own block renderer output)            │
│  Builds       ◀─ light blocks, solid blocks ────────┼─────── │  WorldExporter                                             │
│  ThirdPerson  ◀─ camera mode, the player's body ────┼─────── │  AvatarExporter (F5)                                       │
└─────────────────────────────────────────────────────┘        └────────────────────────────────────────────────────────────┘
                         shared memory (Local\CyberCraft_v1) + rings + seqlocked slots
```

Plus `Launcher` (starts the bundled Minecraft), `SceneDepth` (borrows Cyberpunk's depth buffer),
`Grid` (aligns Minecraft's block grid to the ground), and **`protocol/`**, the byte layout both
sides follow (§10).

## 4. Coordinate mapping

Cyberpunk is Z-up and in metres; Minecraft is Y-up and in blocks. **A block is `s` metres**
(`[World] fMetresPerBlock`, or Mod Settings' "Minecraft scale"; 0.75 by default). At 1 the
Minecraft player is as tall as V, but Minecraft's blocks and its blocky bodies (a head half a block
wide, shoulders a block across) looked oversized next to Night City's people and props, so the
whole of Minecraft's world is scaled down: blocks, mobs, players, and Minecraft's physics with them
(V walks, jumps and reaches `s` times as far in metres).

```
mc.x =  cp.x / s
mc.y = (cp.z - gridOffset) / s
mc.z = -cp.y / s           (Cyberpunk +Y is north; Minecraft -Z is north)
mc.yaw   = 180 - cp.yaw
mc.pitch = -cp.pitch
```

Everything crossing the link is in Minecraft space, so the scale lives in the plugin's
`CpToMc`/`McToCp` alone. The collision rays are cast at Minecraft sample points converted to
metres, the blocks are drawn in Minecraft space from Cyberpunk's camera converted the same way, and
both scale with it unchanged. What's left are lengths that cross between the two: depth tested
against the game's (metres), third person's camera distance, NPC boxes (0.6 × 1.8 m, in blocks),
colliders and light radii for builds, search radii.

- **Eye height.** Cyberpunk's camera is moved to Minecraft's eye, `1.62·s` above V's feet (the
  camera is taken to stand at 1.62 m), so below a metre a block V sees the city from lower down.
  Minecraft picks blocks and entities from its eye; a camera anywhere else would put them off the
  crosshair.
- **Changing the scale** while playing sends everything again, as `Insert` does. Blocks keep their
  Minecraft coordinates, so placed builds move: scaled about Night City's origin, they end up
  elsewhere in the city. Everyone in a shared world needs the same scale; nothing enforces it.
- **Grid offset.** Minecraft blocks start at whole blocks, Night City's ground is anywhere in
  between. `Insert` shifts the whole grid (up to a block) so the ground under V is a block
  boundary, and blocks placed there sit flush. The ground it aligned to belongs to the Minecraft
  world and is saved next to it (`%LOCALAPPDATA%\CyberCraft\cybercraft-world.ini`, `fGridGround`),
  so it stays on a boundary at any scale.
- **Grid heading.** `Insert` also turns the grid about V so its rows run the way she faces, the
  nearest of the four ways to its heading now (`fGridYaw`, plus `fGridShiftX`/`fGridShiftY` so V's
  spot keeps its Minecraft coordinates). Every Cyberpunk <-> Minecraft conversion (`Link.h`) turns
  with it, so the collision rays, camera, NPC stand-ins and block colliders all follow.
- **One world.** Night City (Badlands included) is one continuous space, so there is one Minecraft
  world (the `cybercraft:mirror` preset: a void overworld whose dimension type spans y −1024 to
  1024), and `worldId` is the constant `"CP77"`. There are no load doors to follow.

## 5. The mirror world (Minecraft side)

The Fabric mod runs a normal singleplayer world:

- **Void generator:** no Minecraft blocks except the ones the player places.
- Time and weather don't advance on their own; they follow the host (§9).
- Mob spawning is off. Minecraft's player physics runs on the client as usual; the integrated
  server sees the same injected collision, so it doesn't rubber-band the player.

### 5.1 Collision: how Night City reaches Minecraft physics

A Mixin appends extra shapes to Minecraft's collision queries. Minecraft's own collision
resolution, step-up, gravity, sprint-jumping and sneaking at edges then run unchanged against
them. The shapes aren't blocks, so blocks can be placed against them freely. (The mod can also walk
the local player on a host's exact triangles, `CyberCollider` and `PlayerEdgeMixin`, but the plugin
sends voxels only, so that path is idle.)

**Where the shapes come from:** `gameSpatialQueriesSystem::SyncRaycastByCollisionGroup` rays
(groups `Static,Terrain`) around V, at 1/8-block resolution, budgeted per frame and nearest first:

- one downward multi-hit ray per sample column: floors, roads, ledges, overhangs, ceilings; a ray
  that starts inside a solid steps on down through it instead of ending the column
- short horizontal probes at knee, waist and head height: walls, because down rays only graze
  vertical faces and Night City is mostly vertical faces
- surfaces steeper than ~50° become wall columns, so Minecraft's step-up refuses them
- every frame, the ground under Minecraft's player (the guard), sent at once if the sweep hasn't
  sent it yet

Each hit fills the whole cell its sample stands for (half a sample spacing each way), following
the surface's slope at 1/8-block resolution: a floor is a continuous quarter-block slab and a wall
a continuous sheet, not one 1/8-block column per sample. Players stood on the columns, but arrows,
thrown projectiles, dropped items and the crosshair slipped between them.

A region is sent only when all of its samples are in. Thin geometry (railings, grilles, wires) can
fall between samples.

Walking the physics world and voxelizing the real shapes ("stage C") isn't possible here: RED4ext
exposes no physics world to walk. Rays are the design, not a stopgap.

### 5.2 Water

Water is its own collision group (`Water`, material `water.physmat`), apart from the `Static` and
`Terrain` the down rays use, so the sweep casts one more ray per block column against that group
alone. Its hits become a 16×16 grid of water surface heights around the player (`WaterGrid`), and
a Mixin on `Level.getFluidState` reports a water source in every air cell below them, so all of
Minecraft treats it as water: swimming, floating and drowning, buckets (an empty one fills from
it), boats, fishing, waterlogged blocks, lava turning to obsidian. The block state stays air, so the
water never ticks or flows and Minecraft never draws it over Cyberpunk's.

## 6. The player

**Minecraft is authoritative for the player's position.**

1. Each Minecraft frame and physics tick, the mod writes `McState`: position (previous and current
   tick, plus the partial-tick render position), look, pose, flags.
2. The plugin (**Puppet**) interpolates Minecraft's 20 Hz ticks on Cyberpunk's own frame clock and
   places V there through `gameTeleportationFacility::Teleport`, on every frame she isn't there
   already (standing still, she isn't teleported). V stays in the world
   as a puppet, so NPC awareness, trigger volumes and quest location checks keep working.
3. **Camera:** Cyberpunk keeps its own first-person camera. Turning is the plugin's (it goes out
   with V's teleport); looking up and down is Cyberpunk's (only vertical mouse movement reaches it)
   and is read back from its FPP camera into Minecraft. Blocks are drawn from Cyberpunk's camera, so
   they stay put in the picture Cyberpunk shows.
4. **Takeover:** Minecraft only drives V once a save has placed her, the city has streamed in, the
   player has pressed something, the collision rays have found ground, and Minecraft has
   acknowledged the teleport to where V stands.
5. **Vehicles:** in a car Cyberpunk keeps the controls; Minecraft's player is brought along every
   16 blocks and once more on the way out. The blocks stay drawn, from the car's camera, and keep
   their colliders; Minecraft's hand and HUD don't show.
6. **Third person:** when Minecraft's camera is detached (F5), **ThirdPerson** moves Cyberpunk's FPP
   camera component back along the look (or in front, turned round) by Minecraft's camera distance,
   short of the city's walls, hides V's body, and Minecraft's own body is drawn at V's feet (§9).
   V is hidden in first person too, and Minecraft's body, without head and arms, is drawn a little
   behind the eye, so looking down shows the skin rather than V. In any view it also puts the
   camera at Minecraft's eye (§4; lower again sneaking, crawling, swimming).

## 7. Input

- Cyberpunk's window has focus. **Input** subclasses it and hooks `GetRawInputData`; while
  Minecraft drives V, keys and buttons are swallowed from Cyberpunk and forwarded to Minecraft. The
  Minecraft window is hidden but told it's focused.
- **Keys Cyberpunk keeps:** `Esc` (its menu, or closing an open Minecraft screen), `F9`
  (quickload), `T` (the phone), `` ` `` (the CET overlay). `O` opens Minecraft's menu; `Insert` aligns the grid
  (§4). Beyond those, a key is Minecraft's if Minecraft has a control on it (its key mappings and
  mods', reported in `McState::mcKeys`; the F3 combinations only while F3 is held) and Cyberpunk's
  if not: `V` calls the car, `M` the map, `J` the journal, `Z` the radio. A key's auto-repeats and
  release go where its press went.
- **Routing:** gameplay → Minecraft; a Minecraft screen open → Minecraft, with its cursor in the
  overlay; a Cyberpunk menu open (the time system is paused) → Cyberpunk, and Minecraft drops
  everything held.
- **`F`** is Cyberpunk's while it offers something to do with it (an interaction, loot or dialogue
  choices, read from the `UIInteractions` blackboard by `CyberCraftInteractionShown` in
  `CyberCraft.reds`) and Minecraft's (swap hands) the rest of the time.
- **No Cyberpunk weapons:** keys Minecraft has no control on include Cyberpunk's weapon keys (`Alt`
  draws one), so while Minecraft is in its world `CyberCraftSetWeaponsBlocked` puts the game's own
  `GameplayRestriction.NoCombat` on V (unsaved, CyberCraft as its source): what she holds is
  holstered, and weapons, grenades and arm cyberware are refused.

## 8. Combat

### 8.1 Cyberpunk NPCs inside Minecraft

NPCs are mirrored as invisible, hittable `LivingEntity` stand-ins carrying an id, a health fraction
and hostile / dead / in-combat flags. Swing, cooldown, crits, sweeping, enchantments, bows,
tridents and splash potions all work on them with vanilla Minecraft code.

Which NPCs: the one under the crosshair (`gametargetingTargetingSystem::GetLookAtObject`, within
`fTargetReach`), plus up to eight seen or hit in the last `fRememberSeconds`, so effects and
projectiles on their stand-ins carry on. Cyberpunk's 64-bit entity ids get small handles that fit
the protocol's 32-bit id.

### 8.2 You hit an NPC

Minecraft resolves the hit on the stand-in and sends `HitActor {id, damage, knockback}`. The plugin
applies it as a health-pool change (`gameStatPoolsSystem::RequestChangingStatPoolValue`, Health),
scaled by `fDamageToGame`. Explosions (`kEvExplosion`) hurt every NPC in reach, found through
`GetTargetParts`, with Minecraft's own falloff.

### 8.3 An NPC hits you

V's health is polled, not hooked: a drop becomes a Minecraft hurt event scaled by
`fDamageFromGame`, and Minecraft's armour, shields, totems and i-frames apply. **Minecraft health
is authoritative**: when Minecraft's player dies, V is killed (only if Minecraft was driving her).

## 9. Rendering

Cyberpunk renders the world. Minecraft renders only its own things, offscreen:

| Layer | Contents | How it reaches the screen |
|---|---|---|
| **World** | Placed blocks (Minecraft's own meshes, atlas, tint, AO), block entities, dropped items, arrows, particles, cracks, the player's own body in third person, and the first-person arm and held item | Drawn by the plugin in D3D12 from Cyberpunk's camera (the arm and held item with Minecraft's hand projection), with its own depth buffer, into the finished frame under the game's HUD (when frame generation is handed the frame without it; the arm and held item over everything) |
| **GUI** | Hotbar, hearts, every Minecraft screen, crosshair (and the arm and held item when the plugin doesn't draw them) | Minecraft's image in a shared-memory triple buffer, composited over the back buffer before Present |

- **Occlusion:** with DLSS on, `SceneDepth` borrows the depth buffer the game hands NVIDIA
  Streamline and copies it, so blocks are hidden pixel by pixel behind the city, cars and people.
  Without it, `World::UpdateOcclusion` ray-tests block faces from the camera and hides whole faces
  the static city stands in front of.
- **Frame generation** (DLSS, FSR or XeSS) owns the back buffers between the game's Present and
  dxgi's. Both layers are drawn into the swap chain the game presents (Streamline's proxy, found
  through RED4ext's GpuApi) before its Present, so frame generation takes Minecraft in with the
  game's HUD.
- **Lighting:** blocks, entities, the third-person body and the first-person hand take their light
  from Cyberpunk's own picture: its colour and brightness around each face and out along its
  normal, a little of Minecraft's face shading, haze with distance. Minecraft's sky light only marks
  sealed builds, and its block light adds a warm glow near torches. Without the picture, Minecraft's
  block light and the daylight from Night City's clock.
- **The hand:** while the plugin draws it (`kCyberDrawsHand`), Minecraft sends its first-person
  hands and held items as geometry in view space (`kRenHand`) instead of drawing them into the
  overlay; the plugin draws them last, with Minecraft's hand projection.
- **Time and weather** go both ways. Night City's clock drives Minecraft's overworld clock, and its
  weather (state name and rain intensity) becomes Minecraft's clear, rain or thunder. `/time` and
  `/weather` in Minecraft are noticed as changes nobody else made and sent to Cyberpunk, which
  moves its clock forward or sets the matching weather state. Weather mods are picked up by name
  (see CYBERCRAFT.md).

## 10. Protocol / IPC

- **Shared memory** `Local\CyberCraft_v1` holds:
  - a header (magic, version, both PIDs, heartbeats)
  - seqlocked latest-value slots: `CyberState` (host → MC), `McState`, `WaterGrid`, the actor table,
    world entities
  - rings: input and collision (host → MC); events and render data (MC → host)
  - the overlay triple buffer
- Heartbeats detect a dead side; the host stops puppeting V when Minecraft's goes quiet.
- **Schema:** `protocol/cybercraft_protocol.h` is the single source of truth, mirrored by hand in
  `fabric/.../link/Proto.java`. Little-endian, fixed-size structs. Any layout change bumps
  `kVersion` on both sides (and in `tools/fake_cyberpunk.py`).

## 11. Other systems

- **Launching:** the plugin starts a bundled portable Prism Launcher with a ready "CyberCraft"
  instance as soon as it loads (Minecraft takes ~30 s to open its world). The Fabric mod waits on
  its title screen, hides its window, opens the world by itself and quits with Cyberpunk.
- **Builds in Night City:** Minecraft says per section which blocks give light and which are solid.
  **Builds** spawns, through Codeware (`scripts/CyberCraft.reds`, the empty entity
  `cybercraft\empty.ent` in `CyberCraft.archive`), a point light per light block and a box collider
  per merged run of solid blocks, nearest the player first and within limits, so torches light the
  city and NPCs, cars and bullets meet the walls.
- **Save/load snapshots** are not built (ROADMAP.md).

## 12. Phases

Each phase ends in something playable. State per phase: CYBERCRAFT.md.

| # | Phase | "Done" when |
|---|---|---|
| 0 | **Link** | Both mods handshake; walking in Minecraft moves V |
| 1 | **Walk Night City in MC physics** | Ray-cast collision, input, puppet; streets, stairs and kerbs behave |
| 2 | **Overlay** | Minecraft's hand, hotbar and inventory work over Cyberpunk |
| 3 | **Combat** | Fight a gang with a Minecraft sword and shield, and die properly |
| 4 | **Blocks** | Place and break blocks on Night City's surfaces, hidden behind the city |
| 5 | **Full world** | Water, interaction, vehicles |
| 6 | **Persistence & polish** | Time and weather sync, save snapshots, third person |

## 13. Decisions

1. **Damage scaling.** Minecraft → Cyberpunk: 1 Minecraft damage = `fDamageToGame` (5) percent of
   the NPC's health, so a diamond sword crit (~10) takes half a health bar. Cyberpunk → Minecraft:
   each percent of V's health lost = `fDamageFromGame` (0.2) Minecraft damage, so losing all of it
   is 20 (ten hearts). Both are config values.
2. **Interaction:** `F` is Cyberpunk's only while it shows a prompt, rather than arbitrating
   between the two games' targets by distance.
3. **Cyberpunk's inventory, cyberware, quickhacks and skills:** off or ignored for now.
4. **Digging into the city:** off. Cyberpunk's geometry can't be cut open on screen.

## 14. Risks

| Risk | Mitigation |
|---|---|
| Game calls found by name break with a game update | Each is looked up by exact signature where known and degrades with a log line; `bDiagnostics` dumps the real signatures |
| Rays miss thin geometry | `iSamplesPerBlock`, horizontal probes; stage C isn't available |
| Depth only with DLSS | Ray-tested face occlusion as the fallback |
| Frame generation hides the overlay | Drawn into the game's own swap chain before frame generation; skipped with it on only if that swap chain isn't found |
| Two games' RAM and GPU cost | Minecraft renders almost nothing (void world); JVM heap capped at 4 GB in the bundled instance |

## 15. Repo layout

```
docs/          DESIGN.md (this), CYBERCRAFT.md (status), ROADMAP.md
protocol/      cybercraft_protocol.h: the shared byte layout
red4ext/       the RED4ext plugin (CMake, C++23, RED4ext.SDK submodule)
fabric/        the Fabric mod (Gradle, Loom, MC 26.3)
tools/         packaging, the Minecraft bundle, test stand-ins, diagnostics
```
