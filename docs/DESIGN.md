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
(`[World] fMetresPerBlock`, or Mod Settings' "Minecraft scale"; 1 by default, 0.75 before 0.2.11).
At 1 the Minecraft player is as tall as V and sees from V's eye height, with Minecraft's own
proportions between the player and blocks. Below 1 the whole of Minecraft's world is scaled down:
blocks, mobs, players, and Minecraft's physics with them (V walks, jumps and reaches `s` times as
far in metres).

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
  camera is taken to stand at 1.62 m), so at the default metre a block it stays where V's is.
  Releases before 0.2.11 raised players' eyes to 1.95 blocks to make up for 0.75 m blocks, which
  put the eye above the player's box (inside a block over the head) and off Minecraft's proportions.
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

**Where the shapes come from:** Cyberpunk's own physics is asked, not guessed at.
`gameSpatialQueriesSystem::OverlapByQueryFilter` box queries around V, budgeted per frame and
nearest first, with one query filter for what blocks V: groups `Static`, `Terrain`, `Destructible`
and `PlayerBlocker`, static and moving bodies alike (`sSolidGroups`):

- an octree per 8-block region: a box that touches nothing costs one query; one that does is split
  in eight, down to quarter-block cells. A cell is solid only if its own query found something in
  it. A box whose corners are all inside a solid (the ground under the terrain's surface, a thick
  slab of street, a building's convex bulk) is solid all through, without going down to its cells
  (a hollow mesh only has its shell, and is sent as one)
- each solid cell with air beside it is looked into from that air, one ray along its centre line:
  where the surface is, to the 1/8 block (a floor's height to stand at), following its normal
  across the cell
- every frame, the ground under Minecraft's player (the guard), sent at once if the sweep hasn't
  sent it yet: the cell its surface is in, never more

`/cityprobe` in Minecraft logs what Cyberpunk has in the block looked at, group by group, and the
sweep's queries for it again. Probing what V couldn't walk through found every one of them in
`PlayerBlocker`: static walls and street signs, road barriers (`Destructible`), railings and doors
(moving bodies, which the earlier static-only rays never saw).

On its first floor the sweep works out how the game takes a box's dimension (half extents or
whole) and whether the one filtered query finds what the groups one by one do; if not, it queries
group by group.

The sweep goes region by region (8 blocks a side), nearest first to where the player will be in
half a second, and a region is sent only when all of it is in. One scanned a moment ago isn't
scanned again for a while (3 s round the player, 10 s further out), and rescans get a third of the
frame's budget; the region in progress carries on when the sweep starts over from where the player
has got to. Something thinner than a quarter block (a railing's bar) becomes a quarter-block cell.

Walking the physics world and voxelizing the real shapes ("stage C") isn't possible here: RED4ext
exposes no physics world to walk. Box queries are the nearest thing: they say what's there, not
how it's shaped.

### 5.2 Water

Water isn't solid to the box queries: it's its own collision group (`Water`), or a static surface of
a water material, which the sweep leaves out. So it has a pass of its own: each block column of a
16×16 grid round the player, nearest first as the grid follows the player, is looked down once, by
a ray in the `Water` group and else down through the city's static surfaces to the first of water
material (under a bridge, a pier). Its hits become the grid's water surface heights (`WaterGrid`), and
a Mixin on `Level.getFluidState` reports a water source in every air cell below them, so all of
Minecraft treats it as water: swimming, floating and drowning, buckets (an empty one fills from
it), boats, fishing, waterlogged blocks, lava turning to obsidian. The block state stays air, so the
water never ticks or flows and Minecraft never draws it over Cyberpunk's.

### 5.3 Digging into the city

Cyberpunk's meshes and colliders can't be edited, so a hole exists in three places that agree:

- **Minecraft's world:** mining the city's surface (a voxel hit, as hard as deepslate, dropping what stone does) marks the cell it
  is in dug (`CyberDig`, a chunk attachment synced to every player). `CyberCollision` leaves dug
  cells out of every query, so for Minecraft's physics, crosshair and projectiles the hole is real.
- **Around the hole:** cells next to a dug one are classified from the voxels the plugin sent
  (`CyberDig.classifyVoxel`). One the surface runs through stays the city's, solid up to a floor's
  top; one over a floor within 6 blocks under it, with standing room over that floor (two clear
  cells), is in the open (a player gets there) and stays air, whatever is over it; otherwise one under
  a surface (within 20 blocks) is ground:
  revealed as a real block, dirt for the first few, stone with ores, bedrock from 16 down. A floor
  is told by its slab layers (a quarter of the block or more covered), so posts and railings
  standing on a pavement don't make it a wall. Until a cell is revealed, `DigWalls` draws its face and `wallShape`
  makes it solid, so a hole is always closed. Voxels only show surfaces, so everything behind one
  is ground: a room under a dug floor isn't found.
- **Things standing free:** a dug cell whose geometry stands on a floor (straight down through the
  rest of it) and has open air (or dug cells) beside it on a side or more and ground on none
  (`CyberDig.freeStanding`, `CityVoxels`) is a chair, a cushion, a bin, a low wall, a window's frame,
  a post: there's no ground behind it, so it's just gone, with no walls, floor, wall collision or
  revealed blocks round it. Only the street's surface (nothing under it) and what has ground beside it
  (a hillside) open into stone and dirt. The
  wall probes wrap such things in vertical sheets, which read as walls and lined the cell with stone
  hanging in the air. What stood behind it was never in Cyberpunk's picture, so the cell's sides are
  sent as backdrops (vertex flag bit 8): drawn behind everything else Minecraft draws, only where the
  game's picture is cut, and painted from that picture round the hole (`World.cpp`'s `Backdrop`: the
  first uncut pixel along eight directions, nearer ones and those deeper than the hole weighing
  more). The thing is gone from the picture, with a smooth guess of the street behind it in its
  place.
- **Cyberpunk's picture:** the dug cells reach the plugin (`kRenDug`), which keeps those within 64
  blocks of the camera as a bit grid. The block shader rebuilds where the game's depth puts each
  pixel; if that point is in a dug cell (or 0.2 above one, or 0.25 further along the sight line,
  since the visible surface sits a little off the collision), Cyberpunk's surface there doesn't hide
  the blocks behind it, and the hole's walls and floor are drawn in its place. Without DLSS depth,
  the face rays go on through dug cells instead.

V is puppeted into the hole like anywhere else. The city's own colliders are still there (a dug-out
chair, the street over a hole), and Cyberpunk lifted her out of them every frame while Minecraft put
her back, so while her body (Minecraft's player's box, widened to her capsule and as tall as she is)
reaches into a dug cell, and half a second after, her collision is off: `CyberCraftSetCollisionOff`,
which a wrap of the game's `LocomotionEventsTransition.OnUpdate` turns into the empty simulation
filter the game's own states use (`collisionFilterPresetIsZero`), and back to her state's own filter
after. When Minecraft lets go of her for good (CyberCraft off, Minecraft gone), her collision comes
back first, and the plugin puts her on the ground over the hole, since Cyberpunk's own ground is
still there.

**Holes belong to Night City, not to the grid.** When `Insert` or the scale move the grid, the
plugin sends how Minecraft coordinates moved (`kColGridMoved`) and the host's server moves every
hole (`CyberDig.remap`, `HoleMove`) and the blocks and items in it back to where it was dug,
resampled onto the new grid, each hole raised or lowered whole blocks so it still opens just under
the surface it was dug through (kept per hole: `CyberDig.Opening`). The arrows' nudges are
fractions of a block, so the holes lag behind them (`fHoleLagX`/`Z` in `cybercraft-world.ini`) and
catch up a whole block at a time, with Minecraft's player if it stands in one.

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
`GetTargetParts`, with Minecraft's own falloff. The NPC cries out, and in fun mode (Mod Settings) a
sword hit ragdolls it and pushes it along the knockback (`CyberCraftNpcHit`, `fSwordFling`).

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
  the static city stands in front of. Where the city was dug out, it hides nothing (§5.3).
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
4. **Digging into the city:** by mining and by explosions. The voxels can't tell a room from solid
   ground, so a blast takes the city's surfaces, and the ground under them only where nothing of the
   city lies below (not a room over a floor); a player digging picks one cell at a time (§5.3).

## 14. Risks

| Risk | Mitigation |
|---|---|
| Game calls found by name break with a game update | Each is looked up by exact signature where known and degrades with a log line; `bDiagnostics` dumps the real signatures |
| Box queries cost frame time | Octree (an empty 8-block box is one query), `iQueryBudget` and `fBudgetMs` per frame; pillars further than 12 blocks rescanned only every 10 s |
| Depth only with DLSS | Ray-tested face occlusion as the fallback (holes show a face at a time) |
| V under the city's ground in a hole | Teleported every frame onto `Builds`' colliders; put back on the ground when Minecraft lets go for good |
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
