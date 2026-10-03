# CyberCraft roadmap

What to add next, most useful first. Each item says why it matters, how it could be done, and
where in the code it lands. Current state: [CYBERCRAFT.md](CYBERCRAFT.md). Design:
[DESIGN.md](DESIGN.md).

## 1. Confirm what's built but unrun

Written on both sides and built, but not yet in the game: time and weather sync, third person,
lights for light blocks, colliders for builds, and drawing with frame generation on. Third person,
lights and colliders need Codeware. Deploy the DLL, the
Fabric mod, `scripts\CyberCraft.reds` and `CyberCraft.archive` together (protocol 16), then one run
with `bDiagnostics = 1` settles most of it. The log should start with
`scripts: CyberCraft.reds is loaded`; if it says the script isn't loaded, the redscript log
(`<game>\r6\logs\redscript_rCURRENT.log`) says why.

**Time and weather**

- The log names each weather function's signature (`weather: worldWeatherScriptInterface::...`).
- `/time set night` should move Night City to 19:00 (`time: Minecraft set the time ...`). If it
  jumps somewhere else, `SetGameTimeBySeconds` doesn't take absolute seconds.
- `/weather rain` should start rain within `fBlendSeconds`. If quest weather or the natural cycle
  overrides it at once, `iPriority` is too low.
- With Enhanced Weather installed, note the state names the log shows (`weather: Night City's weather
  is ...`) and whether any are new: they can become `sRain` / `sThunder` defaults.

**Torches** (`red4ext/src/Builds.cpp`, `scripts/CyberCraft.reds`)

- Place a torch at night: an orange light should flicker on the wall around it. `builds: ... lit in
  Night City` counts them every 10 s.
- Too dim or too bright: `fLightIntensity`. Too small a pool: `fLightRangePerLevel`.

**NPCs and builds**

- Build a wall across a pavement: walkers should stop at it or go round. Shoot it: bullets should
  hit. Drive into it: the car should stop. The log should say every collider got its component
  (`builds: N of N ... colliders spawned so far got their component`) and sweeps should count rays
  `through CyberCraft's colliders` while boxes stand near V. A run before the `Entity/Initialize`
  fix had 18 colliders, none added, and cars drove through.
- If NPCs walk into it instead of round it, `bNavObstacle` isn't doing what its name says; they'd
  need the navmesh changed, which nothing exposes yet.
- Break a block of the wall and walk through the gap: if something invisible still stops you, the
  collision rays aren't seeing `character_vr.physmat` on the boxes (`Collision.cpp`, `kOwnMaterial`)
  and need another way to tell them apart.
- If the colliders are added but nothing collides, the runtime-built box needs a cooked shape after
  all: ship collider entities in `CyberCraft.archive` instead (copies of
  `sq025_crash_invisible_collider.ent` with a 1-block box), sized per box by spawning several.

**Cars and Minecraft's player** (`Collision::UpdateVehicles`, `VehiclePush`)

- Walk into a parked car: Minecraft's player should stop at its side; jump onto its bonnet and
  roof. `collision: cars: ... columns of car` should be above 0 with a car within 6 blocks. If it
  stays 0, the `Vehicle` layer isn't what the rays need (`sVehicleGroup`; try `Vehicle,Tank`).
- Stand in the road: a car driving into V should shove her aside (or onto it, at a step's height),
  not pass through. If Minecraft logs `moved wrongly` and puts the player back, the integrated
  server sees the car a tick apart from the client; the push may need to run on the server too.
- Note what the scans cost (`fVehicleRadius`, `iVehicleFrames`) and whether bikes are caught.

**Third person** (`red4ext/src/ThirdPerson.cpp`)

- F5: the camera should pull back behind V, V should vanish and Minecraft's body (skin, armour,
  held item) should stand in her place and walk with her. `third person on ...`, and V is already
  hidden from first person (`body: N parts of V hidden`).
- F5 again (in front): the camera should face V (`third person: in front`), and mouse up should
  lift the camera and Minecraft's head together. If it faces away, the game resets the camera's
  local orientation every frame; turn the view round another way (or drop the front view).
- Looking up and down behind V, the camera should stay behind her head. Seen on 2026-10-03 with
  `bOrbitPitch = 1`: it swung twice as far, so 0 is now the default. If it now stays level
  instead, set `bOrbitPitch = 1`.
- `0 parts of V hidden` in that run: the plugin found none of CyberCraftHost's static functions
  (`scripts: ... doesn't match`), so V wasn't hidden and no lights or colliders spawned. It now
  calls global wrappers; expect `scripts: CyberCraft.reds is loaded`. If it still says it doesn't
  match, the log lists every function with CyberCraft in its name.
- Sneaking (Shift), in either view: the camera should sink about a third of a block over a few
  ticks and come back up on release.
- Minecraft's scale (`fMetresPerBlock`, Mod Settings' "Minecraft scale"): at 0.75 the view should
  sit about 0.4 m below V's own, blocks, mobs and Minecraft's body should look the size of props and
  people rather than bigger, and the highlighted block should be the one under the crosshair. If
  it's off vertically, Cyberpunk's camera isn't 1.62 m up (`kCyberpunkEyeHeight`, Game.cpp).
  Moving the slider in game should resend the city and put Minecraft's player back on V.
- Check whether V's held weapon or cyberarms still show; they're separate entities.

**V's body in first person** (`red4ext/src/ThirdPerson.cpp`, `AvatarExporter`)

- Looking down in first person should show Minecraft's body (skin, chestplate, leggings, boots)
  instead of V: `body: N parts of V hidden, Minecraft's body in her place`. No head, no arms on the
  body (the hand is drawn on its own), nothing floating where V's clothes were.
- If clothes, hair or cyberarms still show, they hang in an attachment slot `BodySlots` in
  `CyberCraft.reds` doesn't list.
- `fBack` (0.2 blocks): looking straight down should show the chest's front and the feet. Too
  small and the shoulders' tops fill the view; too large and the feet end up under the camera.
  Cyberpunk's camera may not sit right over V's feet, which moves the body the same way.
- Change clothes in Cyberpunk's inventory: they should be hidden within a second of closing it
  (`body: N more parts of V hidden`). If that line repeats every second, the game keeps turning
  something back on.
- In a car V should be back (`body: N parts of V shown again`), and hidden again on the way out.

**Frame generation** (`red4ext/src/Overlay.cpp`)

- With DLSS frame generation on, the log should say `overlay: the game presents through
  sl.interposer.dll's swap chain; Minecraft is drawn into it before frame generation sees the
  frame`, and the hand, hotbar and blocks should show. With it off, everything should look as
  before (the same path draws both).
- If Minecraft flickers, frame generation takes the game's HUD from a separate UI buffer rather
  than recovering it from the HUD-less colour, and Minecraft has to go into that buffer too:
  `SceneDepth`'s `slSetTag` hook sees every buffer the game tags.
- Turn fast with blocks in view: they're drawn from the real frame's camera, so they step at the
  real frame rate while the city moves at the generated one. Note how much that shows.
- `the game's own swap chain wasn't found` means GpuApi's layout moved in a game update: drawing
  falls back to dxgi's Present, off while frame generation is on.
- FSR's and XeSS's frame generation take the same path; try one of them too.
- Blocks under Cyberpunk's HUD: with DLSS frame generation on, the log should list the buffers the
  game tags (`streamline: the game tags buffer 2 (HUD-less colour) ...`) and then say `world: blocks
  go under the game's HUD`. Build next to the minimap and quest tracker and check the HUD stays on
  top, that blocks away from the HUD aren't faintly see-through, and that nothing flickers. If
  blocks vanish wholesale, the HUD-less picture doesn't match the frame: `bHudMask = 0` and send
  the log.
- Without frame generation blocks still go over the HUD. Check whether the log lists a HUD-less
  buffer anyway; if not, a fix there needs the game's UI pass found some other way.

## 2. Next up: things players hit in the first hour

### Scripted moves of V

Fast travel, quest teleports, elevators and cutscenes move V without Minecraft. While Minecraft
drives V, the next frame's teleport puts her back where Minecraft's player stands. Detect a jump
Minecraft didn't make (V more than a few metres from where the last teleport put her) and teleport
Minecraft's player there instead (`teleportPending` in `Game.cpp`). Check whether fast travel
already passes through the "at the loading spot" gate, which would cover it.

### Fight everyone nearby, not just the crosshair

`GetTargetParts` is already decoded for explosions (`Combat.cpp`, `Explode`). Using it with the NPC
filter in `MirrorTarget` mirrors every NPC within ~30 m as a stand-in, so sweeping attacks, arrows
at anyone and mobs fighting NPCs all work. Also set `kActorEssential` for quest-critical NPCs, so
Minecraft doesn't send killing blows that Cyberpunk then ignores.

### Hide what Minecraft already shows

Cyberpunk's health bar and hotkey HUD duplicate Minecraft's hearts and hotbar. A small redscript
(`@wrapMethod` on the HUD controllers) shipped next to the plugin could hide them while Minecraft
drives V, keeping the minimap, quest tracker and NPC health bars. It belongs in
`scripts/CyberCraft.reds`, which the plugin already loads. Check too whether V's own hands show
under Minecraft's hand (her weapon is holstered while Minecraft runs).

## 3. Then: the rest of the design

- **Contact shadows.** Minecraft sends a soft shadow under the player's and mobs' feet
  (`kWeShadow`), drained today; drawing it grounds the third-person body and mobs.
- **Death.** Minecraft sends the player's body split into parts about once a second
  (`kRenRagdoll`) for hanging on a ragdoll when the player dies. Cyberpunk's death screen shows
  V's body today.
- **Save snapshots.** Loading an old Cyberpunk save should rewind builds and inventory to match:
  store a save id with each Cyberpunk save, snapshot the (small, sparse) Minecraft world under it,
  restore it on load.
- **Hits with a kind and a direction.** Polling V's health loses what hit her. A hit hook would give
  melee, projectile or explosion (`HurtKind`), the attacker, and a direction for Minecraft's
  knockback; a blocked hit in Cyberpunk could become a shield block.
- **"Use" arbitration by distance.** `F` is Cyberpunk's while it shows an interaction prompt and
  Minecraft's otherwise; it could instead go to whichever target is nearer: Minecraft's ray pick
  (blocks, stand-ins) or Cyberpunk's interaction target.
- **Time and weather, further.** Sleeping in a Minecraft bed skips Night City's night (Minecraft
  doesn't skip it with time frozen, so send the morning as a `/time`). Hand the weather back to
  Cyberpunk's own cycle after a while (`ResetWeather`) instead of holding the requested state.
  Send the rain intensity so Minecraft's rain fades with Night City's. List every weather state up
  front with Codeware's `GetEnvironmentDefinition()`, a weather mod's included, instead of learning
  them as Night City shows them.
- **Lights and colliders, further.** Shadows for the few nearest lights only; light that follows a
  torch held in the hand; boxes rebuilt only where a section changed, not the whole section.
- **Depth without DLSS.** FSR and XeSS take the depth buffer too; hooking their dispatch the way
  `SceneDepth` hooks Streamline would give pixel-exact occlusion on every GPU.
- **Thin geometry.** Railings and grilles fall between samples. Denser rays right around V, or
  probes along the direction of movement, would catch more of them.
- **Grid alignment per area.** One grid offset for the whole city means one street flush and the
  next 0.4 m off. Keep offsets per region, or align automatically when the first block in a region
  is placed.

## 4. Housekeeping

- **Ship `CyberCraft.ini`** with every key documented (the defaults are in CYBERCRAFT.md);
  `tools/package.ps1` packs the plugin without one.
- **Register a Discord application** for Rich Presence (with a `cybercraft` art asset) and set its
  id in `DiscordPresence.APP_ID`; until then Rich Presence is off.
- **Drop Minecraft code this host doesn't use:** the exact-triangle collider path (`kColTris`,
  `CyberTri`, `CyberRay`, `TriCollider`) and digging into the host's geometry (`CyberDig`,
  `DigWalls`, craters, `kRenDug`). The plugin sends neither triangles nor dig data.
- **Stale comments** in the plugin: `Puppet.cpp` says pitch reaches the view "in Phase 2" (Cyberpunk
  owns it); `World.h`
  says Cyberpunk's depth is out of reach (`SceneDepth` borrows it).
- **CI:** `.github/workflows/build.yml` builds the plugin and the mod and runs the mod's tests
  (not yet run on GitHub). Still missing: a layout test that checks `Proto.java`'s offsets against
  `cybercraft_protocol.h`.

## Not planned

- **Digging into Night City** (`kRenDug`). Cyberpunk's geometry can't be cut open on screen, so a
  crater only left Minecraft's walls hanging in the air. Destruction stays off.
- **Collision stage C.** Nothing exposes Cyberpunk's physics shapes.
- **Dimensions and interiors.** Night City is one continuous world.
