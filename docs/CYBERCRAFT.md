# CyberCraft — status and limits

How the pieces fit is in [DESIGN.md](DESIGN.md); what to build next is in [ROADMAP.md](ROADMAP.md).
This file is what exists today, what has been seen working, and where Cyberpunk forced a different
answer than the design would like.

The plugin has been run in Cyberpunk 2077 **2.31**: V walks Night City on Minecraft physics, the
overlay and blocks draw, and the game calls below were checked against the live game's RTTI dump.
It is still early and experimental. **Seven things are new and haven't been run in the game yet:**
time and weather sync, third person, Minecraft's body in first person, lights for light-emitting
blocks, colliders for builds, drawing with frame generation on, and blocks under Cyberpunk's HUD.
The last three, and setting the weather, need [Codeware](https://github.com/psiberx/cp2077-codeware)
(they go through `scripts/CyberCraft.reds`); without it they're off and everything else works.

## Phase status

| # | Phase | State |
|---|---|---|
| 0 | Link | Done. Shared memory, Minecraft started by the plugin, V's position and look sent, Minecraft's state read. |
| 1 | Walk Night City in MC physics | Done. Ray-cast collision, input, puppet. Looking is Minecraft's own mouse math; Cyberpunk's camera is held at its pitch (unrun). |
| 2 | Overlay | Done (D3D12). Hand, hotbar, GUI screens, cursor, crosshair invert pass. Drawn before frame generation (unrun with it on). |
| 3 | Combat | **Subset:** the NPC under the crosshair plus up to eight recently fought, not everyone in a radius. Explosions do hit everyone in reach. |
| 4 | Blocks | Blocks, entities and particles draw from Cyberpunk's camera, hidden behind the city (pixel-exact with DLSS, per face without). Light-emitting blocks get Cyberpunk lights and solid blocks get colliders that NPCs, cars and bullets meet (unrun, Codeware). |
| 5 | Full world | Water, interaction (`F`) and vehicles done. Stage C not portable; dimensions not applicable. |
| 6 | Persistence & polish | Time and weather sync, both ways, and third person with Minecraft's body (unrun). No save snapshots. |

## Game calls

RED4ext exposes the game's RTTI but no catalogue of what each function does, so every call is made
by name, guarded, and degrades with a log line if it's missing. `bDiagnostics = 1` makes
`Game::Probe` dump the classes involved (properties, functions, parameter types) to
`<game>\red4ext\logs\CyberCraft.log` five seconds after a save loads. `tools/cet.ps1` plus the CET
dev console can poke at the same API live.

| Call | Used for | Status |
|---|---|---|
| `gameSpatialQueriesSystem::SyncRaycastByCollisionGroup` | collision, occlusion, grid alignment | Signature from the RTTI dump; working |
| `gameTeleportationFacility::Teleport` | moving V | Looked up by name; working |
| `gametargetingTargetingSystem::GetLookAtObject` | the NPC under the crosshair | Exact signature from the dump |
| `gametargetingTargetingSystem::GetTargetParts` | NPCs in an explosion's reach | Exact signature from the dump |
| `gameStatPoolsSystem::GetStatPoolValue`, `RequestChangingStatPoolValue` | reading and changing health | From the dump; `Health == 17` is reflection data |
| `PlayerPuppet::GetFPPCameraComponent`, `entIPlacedComponent::GetLocalToWorld`, `gameCameraComponent::GetFOV` | the camera blocks are drawn from, pitch | From the dump; working |
| `PlayerPuppet::GetMountedVehicle` | handing control back in cars | From the dump |
| `gameTimeSystem::GetGameTime`, `IsPausedState`, `GetSimTime` | clock, "a menu is open" | From the dump |
| `gameTimeSystem::SetGameTimeBySeconds(Int32)` | `/time` in Minecraft | In the dump; **not yet called in a run** |
| `worldWeatherScriptInterface::GetWeatherState() -> ref<worldWeatherState>`, `SetWeather(CName, Float, Uint32) -> Bool` | reading and setting the weather | **Added by Codeware** (its declarations give these signatures); without Codeware the weather is only read from the rain intensity. Not yet called in a run |
| `ScriptGameInstance::GetWeatherSystem` (static, not a global like `GetPlayer`) | the weather system | Global lookup failed in a run (`/weather` did nothing); found as a static since |
| `worldWeatherScriptInterface::GetRainIntensity` | how hard it rains | Exists (the game's city lights call it); `() -> Float` checked at runtime |
| `entIPlacedComponent::GetLocalPosition`, `SetLocalPosition`, `GetLocalOrientation`, `SetLocalOrientation` | third person: moving the FPP camera | In the dump; not yet called in a run |
| `gameFPPCameraComponent.pitchMin`, `pitchMax` (Float properties) | holding the camera at Minecraft's pitch | In the dump; not yet written in a run |
| Codeware `StaticEntitySystem.SpawnEntity`, `Entity.AddComponent`, the `Entity/Assemble` callback; `entIComponent::Toggle` | lights, colliders, hiding V (`CyberCraft.reds`) | The script compiles against Codeware 1.20.5 (checked with the game's own `scc`); not yet run |
| `gameTransactionSystem::GetItemInSlot` | what V wears (head, cyberarms, clothes are item entities in her attachment slots), hidden with her | The slots are all in 2.31's TweakDB; not yet called in a run |
| NVIDIA Streamline `slSetTag`, `slSetConstants`, `slEvaluateFeature` | borrowing the depth buffer | Hooked; working with DLSS |
| `IDXGISwapChain::Present`, `ID3D12CommandQueue::ExecuteCommandLists` (vtable) | drawing the overlay and blocks | Working |

## Where Cyberpunk forced a different answer

### No collision stage C

The ideal is walking the host's physics world and voxelizing the real shapes. RED4ext exposes no
physics world to walk, so the ray casts are the design: down rays per sample
column with multi-hit (floors, ledges, overhangs, ceilings) plus short horizontal probes at knee,
waist and head height. Each hit fills its sample's whole cell (half a block square at the
default two samples per block), so floors and walls are continuous for arrows, items and the
crosshair; an edge (a kerb, a ledge) can sit up to a quarter block off. Thin geometry (railings,
grilles, wires) can be missed between samples. `iSamplesPerBlock` trades frame time for fidelity.

V fell through the floor in much of the city, wherever something stood over it: inside buildings,
under balconies, signs and overpasses. Three things in the sweep let the floor go missing:

- A down ray took at most 6 surfaces from the top of the window (24 blocks over V), and what was
  overhead used them up. It's 16 now (`iMaxHitsPerColumn`).
- The next ray starts just under each hit, so under a thick solid (a box or convex collider) it
  starts inside it. PhysX reports that as a hit where the ray starts, which ended the column. Now a
  hit at distance 0 counts as "inside": the ray steps on down (⅛, ¼, ½, then 1 block at a time) until
  it's out. After 8 blocks of solid it gives up on the column.
- The column the frame's ray budget ran out in was left with only its first few surfaces. A column
  once started is finished now, a few rays over budget.

And a guard: every frame, one ray per sample cell under Minecraft's player and half a block round
it, from half a block up to `fGuardDepth` (8) blocks down. Whatever it finds is kept and sent with
every flush of that region, and right away if Minecraft doesn't have it yet. A pillar only reaches
Minecraft once all of it is scanned, so in one not scanned yet (a fall, a sprint) the guard's ground
is all there is. Up and down, the sweep now restarts only after a third of the window
(8 blocks; 4 across), so a fall leaves it time to finish the pillar under V.

With `bDiagnostics = 1` each sweep line counts the rays that started inside a solid, the columns that
ran out of surfaces and the regions the guard had to fill in after the sweep sent them without
V's ground. The load probe also casts from just under V's feet (`probe: inside ...`): `dist 0.00`
there means the game reports rays that start inside a solid. **Not yet confirmed in a run:** that the
floor stays under V everywhere now, and which of the three it was.

Stairs stuck V: a wall probe hit marks a band 3/4 block tall about the probe, and at knee height
(a quarter block up) that reached 0.625 blocks, over the 0.6 Minecraft's player steps up. A stair
riser a little over a quarter block (~19 cm at 0.75) was hit by it and became a wall: only some
steps, depending on their height and where the samples fell. A knee hit now casts one ray down just
past the face for the step's top and ends the band there; a real wall is still solid that high and
keeps the band.

Stairs also grew fences once the grid was turned along them. A down ray that hits a steep surface
raises a 2.5-block wall column there (so Minecraft won't climb steep slopes), and a step's rounded
front edge reads as steep. With the grid at an angle to a staircase only the odd sample landed on
an edge; turned along it, a whole row of samples lands on the same edge, and the posts stood half a
block apart with 3/8 of a block between them, too narrow for Minecraft's player. A steep hit now
casts one ray down just behind the edge: flat ground there no more than 0.6 blocks higher is a step,
and raises no post. (A wall probe that starts inside something still marks a wall where it starts:
the probes only run +x and +z, and that is how a wall facing -x or -z is found at all. Leaving those
out took out half the walls, and V was thrown back out of Cyberpunk's ones she walked into.) With
`bDiagnostics = 1`, when Minecraft's player is held against something with a movement key down for a
quarter second, the log says `stuck:` with what it was sent ahead of it. Some stair spots still
stop Minecraft's player (they can be jumped).

### Cyberpunk keeps the camera

There is no camera override (third person only moves the camera; see below). The look is the
plugin's, worked out from raw mouse counts exactly as Minecraft 26.3's `MouseHandler.turnPlayer` and
`Entity.turn` do (`Look.cpp`): the sensitivity slider's cubic curve, the cinematic camera's easing,
the spyglass's slower look, inverted axes, pitch held to ±90. Minecraft reads raw counts too (SDL
relative mode), so a count turns V as far as it turns Minecraft's player. Turning goes out with V's
teleport. Looking up and down is Cyberpunk's first-person camera held at Minecraft's pitch through
its pitch limits (`pitchMin = pitchMax`), with Cyberpunk reading no vertical movement itself; its own
limits go back when Minecraft lets go of V. Whether the camera follows is checked as V looks up and
down (`look: Cyberpunk's camera follows Minecraft's pitch` in the log). If it doesn't, or with
`bPinPitch = 0`, Cyberpunk tilts the camera from the vertical movement again (the `GetRawInputData`
hook lets only that through), scaled so it tilts as far per count as Minecraft: its own tilt per count
is measured the first time V looks up or down far enough, so until then up and down go at
Cyberpunk's speed (`bMatchPitch = 0` keeps it), and the cinematic camera's easing is lost on that
axis. Either way the pitch Minecraft gets is the camera's. Blocks and entities are drawn from
Cyberpunk's camera, not Minecraft's, so they don't swim when Minecraft's camera runs ahead of V's
smoothed position. With `bNativeLook = 0` the plugin owns the whole mouse; held pitch still tilts the
view, and without it the view only turns left and right.

**Not yet confirmed in a run:** that the camera follows held limits (and how they count: the log
names them), and that it reaches straight up and down. The spyglass slows the look as in Minecraft,
but Cyberpunk's view doesn't zoom with it.

### Occlusion needs DLSS

Cyberpunk hands its depth buffer to NVIDIA Streamline for DLSS. `SceneDepth` hooks Streamline's
exports, copies that depth in the game's own command list, and the block pass depth-tests against
it: walls, cars and people hide blocks pixel by pixel. Without DLSS (or with `bSceneDepth = 0`)
there's nothing to borrow, and `World::UpdateOcclusion` ray-tests block faces from the camera
instead (`iOcclusionRays` per frame): only the static city hides blocks, and a face is hidden whole.

### Under the HUD needs frame generation

Blocks are drawn at Present, into a frame that already has Cyberpunk's HUD (minimap, health,
quest tracker, prompts) in it, so they used to cover it. The game draws no frame without the HUD
that the plugin can reach, except the one it hands DLSS frame generation: its picture without the
HUD (`kBufferTypeHUDLessColor`) and, in some games, the HUD alone (`kBufferTypeUIColorAndAlpha`).
`SceneDepth`'s `slSetTag` hook copies whichever it gets, at Present when the game says it's valid
until then, where it's tagged when only valid there.

Blocks, mobs and particles are drawn into a target of their own, over nothing, and `PSMix` puts
them into the frame: over the HUD-less picture, with the HUD over them. The HUD's coverage is its
alpha when the game hands the HUD over; with only the HUD-less picture it's worked out per pixel
from how far the HUD moved each channel of the frame against how far that channel could have gone
that way (down to black or up to white), which is exact for an opaque HUD and for a dark panel over
a bright picture; a faint panel over a picture already about as dark can't be told from the picture,
and blocks show through it. The blocks also take their light from the HUD-less picture. The first-person hand
stays over everything, the HUD too, as when it was part of Minecraft's overlay.

Without frame generation the game hands over neither, and blocks are drawn over the HUD as before;
the log says which it is (`world: blocks go under the game's HUD ...` or `... drawn over the
game's HUD`), and lists every buffer type the game tags (`streamline: the game tags buffer ...`).
`bHudMask = 0` turns this off. **Not yet confirmed in a run:** that the copy's resource state is
right at Present (a wrong one can remove the device), and that the HUD-less picture matches the
frame pixel for pixel where there's no HUD (film grain applied after the HUD would show as a faint
HUD everywhere; blocks would look slightly see-through).

### Lit by Cyberpunk's picture, not its lights

Blocks, mobs, particles, V's Minecraft body in third person, and the first-person hand with what it
holds should look lit by Night City, not by Minecraft. Cyberpunk's lights can't be read, so the
light comes from its finished picture: each frame, before anything of Minecraft's is drawn, the
back buffer is copied and shrunk to 80x45, and every corner of every face takes the colour and
brightness around it (`fLightGain`, `fLightFloor`), plus the picture a little way out along its
normal (`fLightProbe` blocks), so a face turned to a pink sign goes pink and the face turned away
takes the street's light. Most of the picture's colour is kept (`fLightColor`); once only 30% was,
because reading it sharp made blocks look see-through, but per corner and that blurred it reads as
light. Minecraft's own light is mostly gone:

- **Face shading** (top bright, bottom dark) is kept at `fFaceShade` of Minecraft's, enough for
  cubes to read as cubes. Models and items send each face's normal (`RenVertex` flags, normal code
  7), so mobs and bodies are lit from the side they face as well.
- **Sky light** only says how shut in a face is: inside a sealed build Night City's light doesn't
  reach it.
- **Block light** adds a warm glow near torches and lamps (`fBlockLight`); the real light is the
  Cyberpunk light `Builds` spawns, which reaches blocks through the picture. Blocks that give light,
  and whatever Minecraft draws at full block light (flames, a blaze, glowing eyes), glow by
  themselves.
- **Haze:** further off, a block fades into the picture behind it (`fHaze` per metre, up to 85%),
  as Night City's own buildings fade into its fog.

The hand and held item used to be part of Minecraft's overlay picture, lit by Minecraft and laid
flat over the frame. Now, while the plugin draws them (`kCyberDrawsHand`, `bLitHand`), the Fabric mod
diverts them from Minecraft's renderer (`FirstPersonHandMixin`) and sends them as geometry in view
space (`kRenHand`), and the plugin draws them last, with Minecraft's hand projection and bobbing,
lit the same way. Without the flag (an older plugin, `bLitHand = 0`, the block pass not ready)
Minecraft keeps drawing them in the overlay.

**Not yet confirmed in a run:** how close the defaults come to Cyberpunk's look (the knobs above
are guesses); that the hands sit where Minecraft drew them (same field of view, view space folded
from Minecraft's `viewRotationMatrix`); a map held in hand (drawn as custom geometry, which isn't
captured) and enchantment glint are missing from the hand. Lighting from the picture can't see
what's off screen or behind the camera, so a face lit only from there gets the nearest edge's
light.

### Frame generation: drawn before it

With DLSS frame generation on, the game presents to NVIDIA Streamline's proxy swap chain and renders
into back buffers frame generation owns; frame generation then presents real and generated frames
to dxgi itself, from its own thread. The overlay used to draw at dxgi's Present, into back buffers
frame generation was still using, and that removed the device (`0x887a002b`), so it was skipped
while frame generation was on.

Now the plugin hooks the swap chain the game actually presents (found through RED4ext's GpuApi,
where Cyber Engine Tweaks draws too) and draws into its current back buffer just before the game's
Present, on the game's direct queue. Frame generation gets Minecraft with the rest of the frame. The
log names the swap chain (`overlay: the game presents through sl.interposer.dll's swap chain`). If
it isn't found (a game update moved GpuApi), drawing falls back to dxgi's Present and stops while
the game's settings say frame generation is on.

Minecraft is missing from the HUD-less colour the game hands frame generation, so frame generation
should treat all of it, blocks included, as HUD: laid over each generated frame as the last real
frame had it. **Not yet confirmed in a run:** that it doesn't flicker (if the game hands over a
separate UI buffer instead, Minecraft would only be on real frames), how visibly blocks step at the
real frame rate in a fast turn, and FSR's and XeSS's frame generation, which take the same path.

### Lights and colliders are spawned entities

Minecraft sends, per section, its light-emitting blocks (`kRenLights`: position, level, colour,
flame or lava) and the blocks NPCs should collide with (`kRenSolids`: a 16×16×16 bitset). The plugin
can't add lights or physics to Cyberpunk's world by itself, so `Builds.cpp` asks
`scripts/CyberCraft.reds`, which spawns entities through Codeware's `StaticEntitySystem` and gives
each its component once it has its entity ID (`Entity/Initialize`), the way World Builder
(entSpawner) builds its lights and colliders:

- **Lights:** a point `LightComponent` per light block, coloured as Minecraft colours it, its radius
  from the light level (`fLightRangePerLevel`), flickering for flames. Only the `iMaxLights` (48)
  nearest within `fLightRadius` (64 blocks) are lit; Cyberpunk lights aren't free. Shadows are off
  unless `bLightShadows = 1`.
- **Colliders:** solid blocks merged into as few boxes as possible (runs along x, grown along z,
  then y), each a `ColliderComponent` box with the **World Static** preset and the collision masks of
  the game's own invisible walls (`sq025_crash_invisible_collider.ent`). NPCs, cars, bullets and V
  herself meet them; boxes are flagged as navigation obstacles (`bNavObstacle`) in the hope NPCs path
  around them. Only sections within `fColliderRadius` (48 blocks) get them, up to `iMaxColliders`.
  They're made of `character_vr.physmat`, a real material the city itself doesn't use, and the
  plugin's own rays pass through anything made of it: otherwise Minecraft would get its blocks back
  as Night City's collision (lingering after a block breaks) and the city would hide their faces.
- **The entity:** `cybercraft\empty.ent`, an entity with nothing in it (the game's
  `base\quest\main_quests\part1\q115\test\empty_entity.ent` without its weapon appearances), in
  `CyberCraft.archive` under `<game>\archive\pc\mod`. Its source is
  `red4ext/archive/source/cybercraft/empty.ent.json`; `tools/build_archive.ps1` rebuilds the archive
  with WolvenKit.

Spawns are budgeted (`iSpawnsPerFrame`), nearest first. Everything is despawned when the grid moves
(`Insert`) or Minecraft goes, and forgotten on a load (the session takes its entities with it).

A run spawned 18 colliders and cars still drove through the build, and no collision ray ever met
one (`0 through CyberCraft's colliders`): no collider had been added at all. The script matched each
entity to its spawn by ID at `Entity/Assemble`, and an entity only gets its ID at
`Entity/Initialize`, after it assembles. It now listens to Initialize (as entSpawner does), with
Assemble kept in case a game version assigns the ID earlier. With `bDiagnostics = 1` the log
compares the two every 10 s (`builds: N of M lights and N of M colliders spawned so far got their
component`).

The masks were never the problem. The game's collision layers are, by bit: Player, AI, Static,
Dynamic, Vehicle, Tank, Destructible, Terrain, Collider, Particle, Ragdoll, Ragdoll Inner, Debris,
Cloth, PlayerBlocker, VehicleBlocker, TankBlocker, DestructibleCluster, NPCBlocker, then the query
layers from bit 19 (Visibility, Audible, Interaction, Shooting, Water, ...), as the WolvenKit
Blender add-on's preset table has them. Two bodies meet when either lists the other's layer: a car's
chassis collides with VehicleBlocker, and that's how World Static (the city, and these boxes) stops
it.

**Not yet confirmed in a run:** that colliders built at runtime need no cooking (entSpawner's are
built the same way), that they're added now (and cars stop at builds), whether `isObstacle` makes NPCs walk around boxes rather than into them, and
what light intensity looks right (`fLightIntensity`, 40 for a level-15 light, is a guess).

### Third person moves the first-person camera

Cyberpunk has no third-person camera on foot. When Minecraft's camera is detached (F5), the plugin
moves Cyberpunk's FPP camera component instead (`SetLocalPosition`), the way third-person mods do:
back along the look by Minecraft's own camera distance, which Minecraft already shortens where its
blocks are in the way, and shorter still where a ray from V's eyes meets the city. The camera's
local frame already pitches with the look, so straight back along its forward axis swings it up and
down around the head by itself. Pitching the offset as well (`bOrbitPitch`, once the default)
swung it twice as far: a run logged the camera above and in front of V while she looked down. In
front (F5 twice) it also turns the camera round. Its forward then points down when V looks up, so
the pitch Minecraft gets is read from the camera's up axis against V's heading, which the turn
leaves alone. V's body is hidden (below) and Minecraft's own body, skin and armour (`kRenAvatar`)
is drawn at the feet V stands on, lit and hidden behind the city like the blocks.
Those feet are V's as the game has them when the camera is read, not the ones she is teleported to
that frame: those ran a frame's walk ahead of the camera, by however long the frame took, and the
body jiggled while walking.

A run hid V (55 parts) but drew no body in her place: the skin, armour and cape reach the plugin
in its first frames of drawing, and entity textures were refused a descriptor slot until frame 8
(a freed slot waits 8 frames before reuse, and never-used slots were treated as freed at frame 0).
Minecraft sends each texture once, so they never came back. Never-used slots are free at once now;
not yet confirmed in a run.

In every view the camera also stands at Minecraft's eye, its height in blocks times the scale
(see "Minecraft's scale"): 1.62 standing, which at less than a metre a block is below V's own
camera, and lower still sneaking (1.27), crawling and swimming. Straight down in the world, which in
the pitched frame is down and along the look. Minecraft eases its eye down over a few ticks; in first person
`McState::eyeHeight` is the camera's eased height already, and in third person the Fabric side
eases it the same way instead of sending the eye's snap.

**Not yet confirmed in a run:** whether the game keeps the camera's local orientation (the front
view relies on it), whether V's held weapon shows (items are separate entities, not hidden), that
the camera now stays behind the head at every pitch with `bOrbitPitch = 0`, and whether V's own
first-person body clips into view with the camera lower than hers (always, below a metre a block).

### V is hidden in first person too

Looking down used to show V's legs and clothes under Minecraft's hand. Now V is hidden whenever
Minecraft drives her (`bHideV`), and Minecraft's body is drawn in her place in first person as well
(`bMinecraftBody`, `kCyberBodyFirstPerson`): the same capture as third person, without the head (the
camera is in it), the arms (Minecraft's hand is drawn on its own), what's in hand, on the head or on
the shoulders. It stands `fBack` blocks behind the eye along the way the body faces, so looking
straight down shows the chest's front and the feet rather than the tops of the shoulders, as
first-person body mods for Minecraft do. Swimming, gliding and asleep it isn't drawn: lying down,
it would be all round the camera.

Hiding V means every visual component of the player entity, and of the items she wears: her head,
cyberarms and clothes are entities of their own, in her attachment slots (`BodySlots` in
`CyberCraft.reds`), and hiding only the player's own components would leave her clothes walking
about. Only what was toggled off is toggled back on (in a car, a menu that stops Minecraft, a load).
While she's hidden it's done again every second, so clothes put on in the inventory go too. Without
Codeware nothing can be hidden, and V stays with no body drawn over her.

**Not yet confirmed in a run:** that nothing of V is left (hair, tattoos, cyberware in a slot not
listed), how `fBack` looks with Cyberpunk's camera where it is relative to V's feet, and V's own
first-person animations (vaulting, climbing, a quest scene's hands) now playing unseen.

### The plugin calls global script functions, not CyberCraftHost's

`CyberCraft.reds` keeps its state in `CyberCraftHost` (a Codeware `ScriptableService`), but the
plugin calls global functions (`CyberCraftIsReady`, `CyberCraftSpawnLight`, ...) that pass through
to its static ones. A run found `CyberCraftHost` in the RTTI with its member functions (`OnLoad`,
`Forget`, `OnAssemble`) and none of its static ones, in either list, so the plugin found nothing to
call and V never hid. Global script functions are listed by name, like `GetPlayer;GameInstance`.

### Combat mirrors a few NPCs, not a crowd

Rather than every actor in a radius, the NPC under the crosshair is mirrored, and stays mirrored for `fRememberSeconds` (30) after it was last under the
crosshair or hit, up to eight at a time, through a weak handle. Without that, its stand-in went the
moment you looked away, and everything Minecraft had put on it went too: a splash potion's effect
and swirls, poison's ticks, an arrow or thrown potion still in flight. Being hit (poison included)
keeps an NPC remembered. An NPC Minecraft sets alight (Fire Aspect, flame arrows) burns with
Minecraft's fire, drawn on its stand-in like the fire on a burning mob (not yet confirmed in a run).

`GetTargetParts` is now decoded for explosions (every NPC in reach takes Minecraft's blast damage
for its distance), so the radius sweep for mirroring is the same call with a different filter.

Damage crosses the link as the design intends: Minecraft resolves the swing (reach, cooldown,
crits, shields) and only the result is applied, as a health-pool change. V's own health is polled
rather than hooked; a drop becomes a Minecraft hurt event.

### Particles

Nothing Cyberpunk-specific. Minecraft's particles (crits, potion swirls, splash bursts, smoke,
block debris) come with the entities in `kRenScene` and are drawn with the rest of the scene. Crits
need a falling hit, as in Minecraft.

To check them in one run, set `bDiagnostics = 1`: every five seconds the log has
`world: scene ... up to N particle vertices in the last 5 s`, and `world: entity texture ...
arrived` once the particle atlas is in. `/particle minecraft:crit ^ ^1.5 ^2 0.3 0.3 0.3 0.2 60`
puts some two blocks in front of you.

### One world, no load doors

Night City is one continuous world: `worldId` is the constant `"CP77"` and there is nothing to
switch.

### Turning CyberCraft off in Mod Settings

**Enable CyberCraft** (`cyberCraftEnabled` in `scripts/CyberCraftModSettings.reds`) is an ordinary
Mod Settings toggle. The plugin reads it from Mod Settings' `user.ini` (`ModSettings.cpp`), where Mod
Settings writes what is accepted, at load and twice a second after: a script would only know it once
a save runs, too late to decide whether to start Minecraft. Without Mod Settings, or until the row
has been saved once, CyberCraft is on.

Off, the plugin stops driving V (input, overlay, blocks, camera, look all go back to Cyberpunk),
unblocks V's weapons, despawns the blocks' lights and colliders, and stops casting collision rays.
Minecraft gets `kCyberDisabled` without `kCyberInGame`: its player is parked where it stood, as on a
loading screen, and it opens its own pause screen (a singleplayer world stops; one opened to friends
keeps running) and renders ten frames a second. On again, the collision is sent afresh and
Minecraft's player is teleported to V; the pause screen it opened is closed. Off at load, Minecraft
isn't started; turning it on starts it. **Not yet confirmed in a run.**

### Minecraft's scale

A Minecraft block is `fMetresPerBlock` metres in Night City, 0.75 by default, or what Mod Settings'
**Minecraft scale** slider (0.4 to 1) last saved. At 1 blocks, mobs and Minecraft's player looked
oversized next to Night City: the player as tall as V but twice as broad, a cow or a chicken far
bigger than life. The whole of Minecraft's world is scaled, physics included, so blocks still line
up with the player and mobs with blocks; V walks, jumps and reaches less far in metres, NPCs are
taller in blocks (their boxes are 0.6 × 1.8 m), and Cyberpunk's camera sits at Minecraft's eye,
`1.62 × scale` above V's feet. That assumes Cyberpunk's own camera stands 1.62 m up; if it doesn't,
blocks picked are slightly off the crosshair vertically, as they were at a metre a block.

The plugin re-reads the scale twice a second; a change resends the city's collision, respawns the
builds' lights and colliders and puts Minecraft's player back where V is, as `Insert` does. Placed
blocks keep their Minecraft coordinates and so move: a build 1000 blocks from Night City's origin
sits 750 m out at 0.75, 600 m at 0.6. The grid stays aligned to the ground last aligned to
(`fGridGround`); a save from before the scale only has the offset, so press `Insert` once. Everyone
in a shared world needs the same scale. **Not yet confirmed in a run.**

### Minecraft's options from Mod Settings

With jackhumbert's Mod Settings installed, its menu lists **CyberCraft** with a second row, **Open
Minecraft settings** (`scripts/CyberCraftModSettings.reds`). Mod Settings has no button, so the row
is a Bool that never toggles. redscript won't let one mod's annotations touch another mod's classes,
so the click is caught on the game's own `SettingsSelectorController` (`OnLeft`, `OnRight`,
`OnShortcutPress`) by the row's variable name. The click leaves a request in `CyberCraftRequests` (a
vanilla `ScriptableSystem`), accepts Mod Settings' pending changes, as Back does, and queues
`OnClosePauseMenu` through the UI system, which is what Resume sends. The plugin takes the request
every frame in game (the global `CyberCraftTakeMinecraftSettingsRequest`, `ModSettings.cpp`) and
sends `kInOpenMenu` with `kMenuOptions`; Minecraft opens its `OptionsScreen`. In the main menu the
row says it works in game; in a car or dead (Minecraft's screens don't show then) the request is
dropped. An earlier version registered a native function for the click instead; the game crashed
once at startup with it (heap corruption, no CyberCraft frame on the stack), so the plugin registers
no natives. **Not yet confirmed in a run.**

### Interaction

`F` is Cyberpunk's while Cyberpunk shows an interaction, loot or dialogue choices, and Minecraft's
(swap hands) the rest of the time. `CyberCraftInteractionShown` in `CyberCraft.reds` reads that the
way the game's own interaction HUD does (`InteractionUIBase`: the `UIInteractions` blackboard's
`InteractionChoiceHub.active`, `LootData.isActive`, any `DialogChoiceHubs`). It's vanilla script, so
it works without Codeware; without the script `F` stays Cyberpunk's. `Esc`, `` ` `` (so the CET
console stays reachable), `F9` and `T` (the phone; Minecraft's chat opens with `/`) are always
Cyberpunk's.

`F5` is never Cyberpunk's (its quicksave) while CyberCraft is on and Minecraft is in its world: it's
Minecraft's camera view while Minecraft has the controls, and goes nowhere while a Cyberpunk menu
does. Presses that reached Cyberpunk made quicksaves in bursts, and those loaded with Cyberpunk's
time system stuck paused: every action "Action Blocked", and CyberCraft, taking that for a menu,
kept Minecraft hidden and gave Cyberpunk the keys. Save from the pause menu instead.

### No Cyberpunk weapons

`Alt` is Cyberpunk's `SwitchItem` and weapon wheel, and Minecraft has no control on it, so it drew a
gun over Minecraft's hand. While Minecraft is in its world (in a car too), `Game::SyncWeapons` has
`CyberCraftSetWeaponsBlocked` put `GameplayRestriction.NoCombat` on V: the game's own no-fighting
restriction, which holsters what she holds and refuses weapons, grenades and arm cyberware. It's
applied unsaved (`ApplyStatusEffect`'s `isSavable` false) with `CyberCraft.Minecraft` as its source,
so no save keeps V locked and taking it off leaves a quest's own alone; it's set again every second
in case a scene clears it. Vanilla script, so no Codeware needed. **Not yet confirmed in a run.**

Other keys go by what Minecraft binds: Minecraft reports the keys it has a control on (its key
mappings and mods', `McState::mcKeys`) and the rest are Cyberpunk's, so `V` calls the car, `M` opens
the map, `J` the journal and `Z` the radio. **Not yet confirmed in a run:** the interaction check,
and that keys like `R` or `N` reaching Cyberpunk while Minecraft drives V do nothing unwelcome.

### Cars and V's teleports

A run had no cars in Night City once Minecraft took over, and a called car (`V`) never came. Let
There Be Flight's log showed four vehicles set up while the save loaded and none after. V was
teleported every frame then, standing still too (about 50 a second). Now `Puppet::Apply` leaves her
alone while Minecraft's player stands still on the ground and Cyberpunk holds her at that pose by
itself. Once she moves off it on her own (Cyberpunk's gravity on a block without a collider, or
the voxel ground's top), she is placed every frame until Minecraft's player moves again, and in
the air always. A first try that skipped any frame she was within 1 mm of the pose brought back
the standing and flying bob: at a high frame rate a frame's first fall is under a millimetre, so
she kept sinking and being snapped back. **Not yet confirmed in a run:** that teleports were what
kept cars away. If they were, walking (still a teleport a frame) may thin traffic out where V goes.

### Cars are solid to Minecraft's player

Minecraft's player walked through cars, and cars through it: the collision sweep's rays only see
static geometry (`Static,Terrain`, static bodies only), and a car swept in with the city would
leave a ghost wall where it was. Cars get a layer of their own instead (`Collision::UpdateVehicles`):
every `iVehicleFrames` (2) frames, while Minecraft drives V, a ray in the `Vehicle` collision layer
(`sVehicleGroup`, moving bodies included) goes down at every sample column within `fVehicleRadius`
(6 blocks) of V, and where it meets a car, a second goes back up from below to its underside. The
voxels between, a sample's cell wide as the city's are, go to Minecraft as one `kColVehicles`
message that replaces the last (since protocol 15); one that hasn't changed (parked cars, or none) isn't
sent again. Minecraft adds them to every block collision query (`CyberCollision.vehicleShapeAt`), so
its player walks into a car's side and stands on its roof.

A car that drives into the player would leave it standing inside the car, since Minecraft never
moves anyone out of a shape that came to them, so each client tick `VehiclePush` puts the player
the shortest way out to where nothing's in the way: up, if that's no more than a step (0.6), else
sideways up to 2 blocks, and keeps half the push as velocity, so it reads as a shove. In a car the
layer is emptied (Minecraft's player rides inside V's own car).

With `bDiagnostics = 1` the log says every 5 s how many columns of car the rays found
(`collision: cars: ...`). **Not yet confirmed in a run:** that rays in the `Vehicle` layer see
traffic and parked cars (and bikes, narrower than a block), what the extra rays (about 500 a scan)
cost, and that a car hitting the player doesn't make the integrated server put it back (`moved
wrongly`). Cars do no damage.

### Blocks in a car

In a car Cyberpunk drives and Minecraft's player is only brought along, and everything of
Minecraft's used to go with the puppeting: no blocks were drawn, and the render ring wasn't drained,
so no section Minecraft sent from there on reached `Builds` either, and cars drove through builds
they came to. Now `Runtime::drawBlocks` is on while Minecraft drives V and in a car: the blocks are
drawn from the camera system's active camera (the car's, behind it or at the dashboard;
`ReadActiveCamera`), hidden behind the city as on foot, without Minecraft's hand, hotbar or GUI.
The boxes also list Vehicle and Tank among the groups they collide with (`simulation.mask2`), as
the game's Moving Kinematic preset does; they never needed to (above, *Lights and colliders are
spawned entities*: the collider was never added). Every collision sweep logs how many rays went through CyberCraft's colliders (`...
through CyberCraft's colliders`): none while boxes stand around V means Cyberpunk's physics never
got them. **Not yet confirmed in a run:** that the active camera's FOV is vertical like the FPP
camera's (blocks zoomed against the city if not: `bFovIsVertical`), and that cars now stop at
builds.

### The grid is aligned by hand

Minecraft's blocks start at whole blocks while Night City's ground is anywhere in between, so a
block on a pavement at 7.3 m sat 0.3 m sunk. `Insert` shifts the whole grid so the ground under V
is a block boundary. The ground it aligned to is saved with the Minecraft world and applies
everywhere (at any scale), so one street is flush and another may not be.

Night City's streets don't run north-south, and a wall along one went up as a staircase. `Insert`
also turns the grid about V so its rows run the way she faces: of the four headings along her look,
the one nearest the grid's current heading, so builds near V turn as little as they can (at most
45°) and V keeps her Minecraft coordinates. Builds farther off swing round with it. The heading
(`fGridYaw`, with `fGridShiftX`/`fGridShiftY`) is saved next to the ground; the colliders under
the blocks are turned to match. Working in a run, as are the arrow keys below.

The arrow keys fine-tune it: each press (and each repeat while held) moves the grid a pixel
(`fNudgeBlocks`, 1/16 block) forward, back, left or right of the way V faces, and saves it. V stays
where she is, so Minecraft's player is moved back under her by what the grid moved, at its own height
and look; Minecraft keeps driving V meanwhile, and she is held still until it has arrived. (A full
teleport, as `Insert` makes, lets go of V until then: for a few frames the camera is at Cyberpunk's
eye height instead of Minecraft's, a bob up and down.) The city's collision and the builds' lights
and colliders are resent half a second after the last nudge, not on every one.

### V near walls

V's body in Cyberpunk is wider than Minecraft's player (0.3 blocks each way: 0.225 m at 0.75), so
Minecraft walks her closer to a wall than Cyberpunk lets her stand. Cyberpunk pushed her out, the
next teleport put her back, every frame she walked or turned: the view shook near walls. A teleport
lands in the frame it is made (V never sank between them), so whatever else moves her sideways is
Cyberpunk, and she is now placed where it pushed her: up to `fMaxPushback` (0.25 m) off Minecraft's
player, wearing off a couple of millimetres a frame once nothing pushes. Only while Minecraft's
player moves or turns: standing still, Cyberpunk nudging her every frame (a slope) carried her off
half a metre, the limit snapped her back, and over again. A single push past the limit isn't kept
either, and no longer drops the offset (that was the snap). The 5-second log line counts
the pushes and how far off she stood. Tried in a run: much less shake near walls, and no drift
standing still.

## Time and weather

Both directions, on the host's integrated server only (in a shared world, guests' games don't
drive it):

- **Night City → Minecraft.** Every frame the plugin sends Night City's hour (`kCyberClock`) and its
  weather sorted into clear, rain or thunder. The Fabric mod (`CyberClock`) sets Minecraft's overworld
  clock (the mirror dimension uses it) when it's 10 ticks or more off, and Minecraft's weather when
  the kind changes. Night falls in Minecraft with Night City's, and it rains when Night City rains.
- **Minecraft → Night City.** Minecraft's time and weather don't advance on their own, so a change
  nobody here made came from `/time` or `/weather` (or another mod). It's sent to Cyberpunk instead
  of being overwritten: `/time set night` moves Night City's clock forward to 19:00, the way waiting
  does (never back), and `/weather rain` sets a rain state. Minecraft holds its new value until
  Cyberpunk has followed (5 s for time, 30 s for weather, which blends in).
- **`/time` leaves the weather alone.** Moving Night City's clock rolls its weather schedule forward
  too, and a jump of hours often lands in another state. So the state showing before the jump (or
  the one `/weather` asked for) is set again right after it, with no blend (`bKeepOnTimeChange`).
  Like `/weather`, that holds the state: Night City's own weather stops changing until the next
  `/weather` or a quest's weather.

How weather is read: the current state's name (`24h_weather_rain`, ...) and the rain intensity.
Names with "rain" are rain, names with "thunder", "lightning" or "storm" (not "sandstorm") are
thunder, and anything else is clear unless it's raining harder than `fRainThreshold`. Cyberpunk
has no thunderstorm state of its own, so `/weather thunder` sets rain and Minecraft keeps its
thunder while Night City shows that state.

**Weather mods.** At the first weather read the log lists every installed archive or REDmod with
"weather" in its name (`weather: weather mods installed: ...`).

- **Enhanced Weather** (the one on the dev machine) is a single small `.archive` with no API to
  call; it reworks the game's weather and is described as adding new conditions. Both halves of
  that are covered by the two cases below.
- A mod that changes the game's own states needs nothing: every state the plugin sets or reads is
  the mod's version of it.
- A mod that adds states of its own is picked up by name. When Night City switches to a modded rain
  or storm state, it's remembered for the session, and `/weather rain` or `/weather thunder` then
  sets that state instead of the game's rain. `sRain` and `sThunder` pin a state by name. Only use
  names the game has shown in the log: an unknown name may crash the game, the way an unknown
  collision preset did.

**Needs Codeware:** `SetWeather` and `GetWeatherState` are added to the weather system by Codeware,
not the game. Without it, Minecraft still follows Night City's rain (from `GetRainIntensity`, which
the game has), but `/weather` can't change Night City and modded states aren't recognised.

**Not yet confirmed in a run:** how `iPriority` is weighed against quest weather, and that
`SetGameTimeBySeconds` takes absolute seconds. The first run logs each weather function's
signature (`weather: worldWeatherScriptInterface::...`). Codeware's `GetEnvironmentDefinition()`
could list every weather state up front, a weather mod's included, instead of learning them as
Night City shows them.

## Configuration

`CyberCraft.ini`, next to the plugin DLL (`<game>\red4ext\plugins\CyberCraft\`). Every key is
optional; the defaults are below.

```ini
[Minecraft]
bStartWithCyberpunk = 1            ; 0: start Minecraft yourself
sLauncher =                        ; empty: the bundled Prism Launcher
sArguments = --launch CyberCraft
fTakeoverDelay = 5                 ; seconds after the first key press before Minecraft drives V

[Collision]
bEnable = 1
sGroup = Static,Terrain            ; collision groups the rays hit
fRadius = 32                       ; blocks around V
fUp = 24
fDown = 24
iSamplesPerBlock = 2
iRayBudget = 2000                  ; rays per frame
iMaxHitsPerColumn = 16             ; surfaces per down ray, top of the window to the bottom
fSteepNormalY = 0.64               ; steeper than ~50 degrees is a wall
fWallHeight = 2.5
fRebuildDist = 4                   ; across; up and down a third of the smaller of fUp / fDown
bGuard = 1                         ; the ground under Minecraft's player, cast and sent every frame
fGuardDepth = 8                    ; blocks under the feet the guard looks
bVehicles = 1                      ; cars are solid to Minecraft's player
sVehicleGroup = Vehicle            ; collision layers cars are in
fVehicleRadius = 6                 ; blocks around V
iVehicleFrames = 2                 ; frames between car scans

[Combat]
fDamageToGame = 5                  ; Minecraft damage -> NPC health percent
fDamageFromGame = 0.2              ; V's health percent lost -> Minecraft damage
fTargetReach = 30                  ; metres
fRememberSeconds = 30

[World]
fMetresPerBlock = 0.75             ; Minecraft's scale: metres per block (Mod Settings' slider wins once saved)
bSceneDepth = 1                    ; borrow Cyberpunk's depth through Streamline (DLSS)
bHudMask = 1                       ; blocks under Cyberpunk's HUD (needs frame generation on); 0: over it
iOcclusionRays = 400               ; face rays per frame without it
bFovIsVertical = 1
fLightGain = 1.7                   ; Cyberpunk's picture -> light on blocks, entities and the hand
fLightFloor = 0.03
fLightColor = 0.85                 ; how much of the picture's colour the light keeps (0 grey, 1 all)
fLightProbe = 1.5                  ; blocks out along a face's normal its side light is read from
fFaceShade = 0.35                  ; how much of Minecraft's face shading is kept
fBlockLight = 0.5                  ; warm glow near Minecraft's torches and lamps
fHaze = 0.005                      ; fade into the picture behind, per metre of distance
bLitHand = 1                       ; draw the hand and held item lit like the blocks; 0: Minecraft's overlay
fNudgeBlocks = 0.0625              ; how far an arrow key moves the grid, in blocks

[Puppet]
fMaxPushback = 0.25                ; metres V may stand off Minecraft's player where Cyberpunk pushes her (walls); 0 off

[Input]
bNativeLook = 1                    ; 0: the plugin owns the whole mouse, no looking up and down
bPinPitch = 1                      ; Cyberpunk's camera held at Minecraft's pitch; 0: tilted from the mouse
bMatchPitch = 1                    ; then, up and down as fast as left and right; 0: Cyberpunk's own speed
bFToMinecraft = 1                  ; F swaps hands in Minecraft when Cyberpunk offers no interaction; 0: F is always Cyberpunk's

[Weather]
bSync = 1
sClear = 24h_weather_sunny
sRain =                            ; empty: a weather mod's rain state once seen, else 24h_weather_rain
sThunder =                         ; empty: a weather mod's storm state once seen, else sRain's
fBlendSeconds = 10
iPriority = 5
fRainThreshold = 0.1
bKeepOnTimeChange = 1              ; /time sets the weather showing before it again

[Builds]
bLights = 1
iMaxLights = 48                    ; nearest first
fLightRadius = 64                  ; blocks around V
fLightIntensity = 40               ; a level-15 light; lower levels scale down
fLightRangePerLevel = 0.8          ; blocks of radius per light level
fFlicker = 0.3                     ; torches and fire
bLightShadows = 0
bColliders = 1
iMaxColliders = 1024
fColliderRadius = 48               ; blocks around V
bNavObstacle = 1
iSpawnsPerFrame = 16

[ThirdPerson]
bEnable = 1
bOrbitPitch = 0                    ; 1 if the camera stays level while V looks up and down

[Body]
bHideV = 1                         ; V hidden in first person too (always in third person)
bMinecraftBody = 1                 ; Minecraft's body in her place when you look down; 0: nothing
fBack = 0.2                        ; blocks it stands behind the eye in first person

[Debug]
bDiagnostics = 0                   ; per-frame logs and the RTTI dumps
```

## Build

```
cd red4ext
cmake --preset default
cmake --build --preset release
cd ..\fabric
gradlew build
```

`tools\package.ps1` builds both and packs a release. Set `CYBERCRAFT_DEPLOY_DIR` to
`<game>\red4ext\plugins\CyberCraft` before configuring to have each build copied into the game:
the DLL, `scripts\CyberCraft.reds` next to it (the plugin adds that folder to the redscript
compilation), and `CyberCraft.archive` into `<game>\archive\pc\mod`.

To check the script without starting the game, compile it with the game's own compiler against a
copy of the script cache, never the game's own (it shows an error box when compilation fails):
`<game>\engine\tools\scc.exe -compile <empty folder> -customCacheDir <copy of r6\cache>
-compilePathsFile <list: Codeware's Scripts folder, red4ext\scripts\CyberCraft.reds>
-outputCacheFile <scratch>\out.redscripts`.

The plugin and the Fabric mod must speak the same protocol version (`kVersion` in
`cybercraft_protocol.h`, `VERSION` in `Proto.java`): update both together.
