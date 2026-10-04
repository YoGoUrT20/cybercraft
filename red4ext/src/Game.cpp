#include "Game.h"

#include <RED4ext/Scripting/Natives/Generated/Matrix.hpp>
#include <RED4ext/Scripting/Natives/Generated/Transform.hpp>

#include "Builds.h"
#include "Collision.h"
#include "Combat.h"
#include "Config.h"
#include "Environment.h"
#include "Grid.h"
#include "Input.h"
#include "Launcher.h"
#include "Look.h"
#include "ModSettings.h"
#include "Overlay.h"
#include "Puppet.h"
#include "Rtti.h"
#include "Scripts.h"
#include "ThirdPerson.h"
#include "World.h"

namespace cybercraft
{
	Runtime& State()
	{
		static Runtime runtime;
		return runtime;
	}

	namespace
	{
		std::mutex cameraMutex;
		CameraView cameraView;
	}

	void SetCameraView(const CameraView& a_view)
	{
		std::lock_guard lock(cameraMutex);
		cameraView = a_view;
	}

	CameraView GetCameraView()
	{
		std::lock_guard lock(cameraMutex);
		return cameraView;
	}

	namespace Game
	{
		namespace
		{
			rtti::Method getWorldPosition{ "entEntity", "GetWorldPosition" };
			rtti::Method getWorldYaw{ "entEntity", "GetWorldYaw" };
			rtti::Method isPausedState{ "gameTimeSystem", "IsPausedState" };
			rtti::Method simTime{ "gameTimeSystem", "GetSimTime" };

			// Cyberpunk's menus (pause, map, inventory, the "Press Space" prompt) pause its world, so
			// that is what "a Cyberpunk menu is open" means: the time system's paused state, or its
			// sim clock standing still. Read every frame, it can't drift out of step the way a flag
			// flipped on each Esc did.
			bool GamePaused(float a_delta)
			{
				auto* time = rtti::System("gameTimeSystem");
				if (!time) {
					return false;
				}
				// Which of the two says paused, logged when that changes: a run stayed "paused" from the
				// load on, with V in the city and the game running.
				static int lastWhy = -1;
				const auto because = [](int a_why, std::uint64_t a_sim) {
					if (a_why != lastWhy) {
						lastWhy = a_why;
						static constexpr const char* kWhy[]{ "neither", "the time system's paused state", "the sim clock standing still" };
						logger::info("input: paused by {} (sim clock {:#x})", kWhy[a_why], a_sim);
					}
					return a_why != 0;
				};
				bool paused = false;
				if (isPausedState.Get() && isPausedState.Call(time, &paused) && paused) {
					return because(1, 0);
				}
				static std::uint64_t lastSim = 0;
				static float         stalled = 0.0f;
				std::uint64_t        sim = 0;  // EngineTime: 8 opaque bytes, compared as they are
				if (!simTime.Get() || !simTime.Call(time, &sim)) {
					return false;
				}
				stalled = sim == lastSim ? stalled + a_delta : 0.0f;
				lastSim = sim;
				return because(stalled > 0.25f ? 2 : 0, sim);
			}

			proto::McState mc{};
			bool           mcWasAlive = false;
			std::uint32_t  epoch = 1;
			std::uint32_t  teleportSeq = 1;
			bool           teleportPending = true;
			// Seconds until the collision and the blocks' lights and colliders are resent after the
			// arrows last nudged the grid (0: nothing waiting). Held, an arrow nudges it many times a
			// second, and resending all of it on each left Minecraft's player nothing to stand on.
			float          nudgeSettle = 0.0f;
			// A nudge moves Minecraft's player by just what the grid moved under it ("soft"), not to
			// V's feet as a teleport does: same height, same look, and Minecraft keeps driving V while
			// it catches up. A teleport let go of V until Minecraft had arrived, and the camera went
			// back to Cyberpunk's eye height for those few frames: V bobbed up and down on every nudge.
			std::uint32_t  hardTeleportSeq = 1;   // the last teleport that wasn't a nudge
			bool           softPending = false;
			McVec          softTarget{ 0.0, 0.0, 0.0 };
			// Seconds V is still held after Minecraft arrived from a nudge: the tick it reports next
			// still starts where it was, and ticks from before are in the grid's old coordinates.
			float          softHold = 0.0f;
			constexpr std::uint32_t kWorldId = 0x43503737;  // "CP77": Night City is one continuous world
			// Metres above V's feet Cyberpunk's first-person camera stands, taken to be Minecraft's
			// standing eye at a metre a block (1.62): what ThirdPerson moves it from to Minecraft's eye.
			constexpr float kCyberpunkEyeHeight = 1.62f;

			std::chrono::steady_clock::time_point lastFrame{};
			bool                                  wasEnabled = true;
			float                                 inGameSeconds = 0.0f;
			bool                                  probed = false;
			// While a save loads (and behind the main menu) Cyberpunk keeps the player at the world
			// origin, at about (0, 3.6, 0), and only then moves it to the save's spot. Until it has
			// left there and the city around it has had a moment to stream in, nothing drives V and
			// Minecraft is not told the game is on.
			constexpr float kSettleSeconds = 2.0f;
			float           loadedSeconds = 0.0f;
			bool            loggedLoadIn = false;
			std::uint64_t   pressesAtLoad = 0;
			float           continuedSeconds = 0.0f;

			bool AtLoadingSpot(const RED4ext::Vector4& a_pos)
			{
				return std::abs(a_pos.X) < 2.0f && std::abs(a_pos.Y) < 6.0f && std::abs(a_pos.Z) < 2.0f;
			}

			// Minecraft's 20 Hz physics ticks, interpolated on this game's own frame clock, as
			// Minecraft's renderer does with partial ticks. Driving V
			// from Minecraft's per-frame position judders: Minecraft renders hidden, at its own pace,
			// out of phase with Cyberpunk.
			struct Tick
			{
				double       prevX, prevY, prevZ;
				double       curX, curY, curZ;
				std::int64_t at;
			};
			std::deque<Tick> ticks;
			// How far the render point trails now (QPC units), and what each recent frame needed it to:
			// how far past the newest tick's end that frame came, the next tick not here yet.
			double                                            trail = -1.0;
			std::deque<std::pair<std::int64_t, std::int64_t>> needs;  // (frame, need)
			std::int64_t                                      lastFrameQpc = 0;

			McVec SmoothedFeet(const proto::McState& a_mc)
			{
				if (a_mc.tickQpc == 0 || a_mc.tickMs <= 0.0f) {
					return { a_mc.x, a_mc.y, a_mc.z };
				}
				static const std::int64_t qpcFreq = [] {
					LARGE_INTEGER f;
					::QueryPerformanceFrequency(&f);
					return f.QuadPart;
				}();
				const std::int64_t period = std::max<std::int64_t>(1, std::llround(double(a_mc.tickMs) * double(qpcFreq) / 1000.0));
				if (ticks.empty() || ticks.back().at != a_mc.tickQpc) {
					if (!ticks.empty() && a_mc.tickQpc < ticks.back().at) {
						ticks.clear();  // Minecraft restarted
					}
					ticks.push_back({ a_mc.prevX, a_mc.prevY, a_mc.prevZ, a_mc.curX, a_mc.curY, a_mc.curZ, a_mc.tickQpc });
					while (ticks.size() > 8) {
						ticks.pop_front();
					}
				}
				// Interpolation only, never extrapolation, so no hitch when a jump or landing changes the
				// velocity: the render point trails now just far enough that the tick covering it has
				// always arrived. How far that is depends on when Minecraft's ticks get here, so it is
				// measured: the worst need of the last two seconds, plus 2 ms. A fixed tick and a fifth
				// (60 ms) put V that far behind Minecraft's own player; it is still the most this trails.
				// Eased, so V never jumps: she plays her moves at 75-110% speed while it changes.
				LARGE_INTEGER now;
				::QueryPerformanceCounter(&now);
				const double most = double(period + period / 5);
				needs.emplace_back(now.QuadPart, now.QuadPart - (ticks.back().at + period));
				while (now.QuadPart - needs.front().first > 2 * qpcFreq) {
					needs.pop_front();
				}
				std::int64_t worst = 0;
				for (const auto& need : needs) {
					worst = std::max(worst, need.second);
				}
				const double target = std::clamp(double(worst) + double(qpcFreq) / 500.0, 0.0, most);
				const double elapsed = lastFrameQpc ? double(now.QuadPart - lastFrameQpc) : 0.0;
				lastFrameQpc = now.QuadPart;
				if (trail < 0.0) {
					trail = most;
				}
				trail = target > trail ? std::min(target, trail + elapsed * 0.25) : std::max(target, trail - elapsed * 0.1);
				if (Config::Diagnostics()) {
					static std::int64_t nextLog = 0;
					if (now.QuadPart >= nextLog) {
						nextLog = now.QuadPart + 5 * qpcFreq;
						logger::info("puppet: V trails Minecraft's ticks by {:.1f} ms (frames needed up to {:.1f} ms in the last 2 s)",
							trail * 1000.0 / double(qpcFreq), double(worst) * 1000.0 / double(qpcFreq));
					}
				}
				const std::int64_t renderAt = now.QuadPart - std::llround(trail);
				std::size_t        i = 0;
				for (std::size_t k = ticks.size(); k-- > 0;) {
					if (ticks[k].at <= renderAt) {
						i = k;
						break;
					}
				}
				const auto&  t = ticks[i];
				const double into = double(renderAt - t.at) / double(period);
				if (into > 1.0 && i + 1 < ticks.size()) {
					// Past this tick's end and the next one we have starts later: Minecraft ran a tick
					// we never saw. Carry on from this tick's end to the next one's start.
					const auto&  n = ticks[i + 1];
					const double gap = double(n.at - (t.at + period));
					const double u = gap > 0.0 ? std::clamp(double(renderAt - (t.at + period)) / gap, 0.0, 1.0) : 1.0;
					return { t.curX + (n.prevX - t.curX) * u, t.curY + (n.prevY - t.curY) * u, t.curZ + (n.prevZ - t.curZ) * u };
				}
				const double u = std::clamp(into, 0.0, 1.0);
				return { t.prevX + (t.curX - t.prevX) * u, t.prevY + (t.curY - t.prevY) * u, t.prevZ + (t.curZ - t.prevZ) * u };
			}

			// Cyberpunk's FPP camera, read from its world matrix, whose rows are its axes in world
			// space (X right, Y forward, Z up) and W its position. Publishes the view the blocks are
			// drawn from, and returns the pitch as Minecraft's (positive looking down).
			// a_mcYaw: V's heading, Minecraft degrees.
			std::optional<float> ReadCamera(const rtti::Handle<rtti::IScriptable>& a_player, float a_mcYaw)
			{
				static RED4ext::CBaseFunction* getCamera = [] {
					auto* found = rtti::FindFunction("PlayerPuppet", "GetFPPCameraComponent", {}, "handle:gameFPPCameraComponent");
					if (!found) {
						logger::warn("look: PlayerPuppet::GetFPPCameraComponent() not found; Minecraft's pitch follows the mouse, not Cyberpunk's camera");
					}
					return found;
				}();
				static rtti::Method localToWorld{ "entIPlacedComponent", "GetLocalToWorld" };
				static rtti::Method getFov{ "gameCameraComponent", "GetFOV" };
				// GetFOV() is already the vertical FOV: it read ~51 with the 80 (horizontal, 16:9)
				// setting, and converting it again drew the blocks zoomed ~1.8x and off their spot.
				static const bool fovVertical = Config::GetBool(L"World", L"bFovIsVertical", true);
				if (!getCamera || !a_player) {
					SetCameraView({});
					return std::nullopt;
				}
				rtti::Handle<rtti::IScriptable> camera;
				RED4ext::StackArgs_t            args;
				RED4ext::Matrix                 world{};
				if (!RED4ext::ExecuteFunction(a_player.GetPtr(), getCamera, &camera, args) || !camera || !localToWorld.Call(camera.GetPtr(), &world)) {
					SetCameraView({});
					return std::nullopt;
				}

				float fov = 0.0f;
				getFov.Call(camera.GetPtr(), &fov);
				CameraView view{};
				if (fov > 1.0f && fov < 179.0f) {
					const float half = fov * 0.5f * 0.0174532925f;
					view.fovYDeg = fovVertical ? fov : 2.0f * std::atan(std::tan(half) * 9.0f / 16.0f) * 57.2957795f;
					const auto eye = CpToMc(world.W.X, world.W.Y, world.W.Z);
					const auto forward = CpDirToMc(world.Y.X, world.Y.Y, world.Y.Z);
					const auto up = CpDirToMc(world.Z.X, world.Z.Y, world.Z.Z);
					view.eye[0] = eye.x;
					view.eye[1] = eye.y;
					view.eye[2] = eye.z;
					view.forward[0] = float(forward.x);
					view.forward[1] = float(forward.y);
					view.forward[2] = float(forward.z);
					view.up[0] = float(up.x);
					view.up[1] = float(up.y);
					view.up[2] = float(up.z);
					view.valid = true;
				}
				SetCameraView(view);

				// From how far the camera's up leans along V's heading (forward looking down), not
				// from its forward: Minecraft's front view (ThirdPerson) turns the camera round, and
				// its forward then points down when V looks up. The up axis stays put.
				const auto   up = CpDirToMc(world.Z.X, world.Z.Y, world.Z.Z);
				const double yaw = a_mcYaw * 0.0174532925;
				const double lean = -up.x * std::sin(yaw) + up.z * std::cos(yaw);
				return float(std::atan2(lean, up.y) * 57.2957795);
			}

			// In a car the picture is the car's camera (behind it, or the dashboard's), not V's FPP
			// camera: the camera system's active one is published for the blocks instead. False, and
			// the FPP view left as it is, when it can't be read.
			bool ReadActiveCamera()
			{
				static rtti::Method getTransform{ "gameCameraSystem", "GetActiveCameraWorldTransform" };
				static rtti::Method getForward{ "gameCameraSystem", "GetActiveCameraForward" };
				static rtti::Method getUp{ "gameCameraSystem", "GetActiveCameraUp" };
				static rtti::Method getFov{ "gameCameraSystem", "GetActiveCameraFOV" };
				static const bool fovVertical = Config::GetBool(L"World", L"bFovIsVertical", true);
				static bool       warned = false;
				auto*             system = rtti::System("gameICameraSystem");
				if (!system) {
					system = rtti::System("gameCameraSystem");
				}
				RED4ext::Transform transform{};
				RED4ext::Vector4   forward{}, up{};
				bool               found = false;
				float              fov = 0.0f;
				if (!system || !getTransform.Call(system, &found, transform) || !found || !getForward.Call(system, &forward) || !getUp.Call(system, &up) ||
					!getFov.Call(system, &fov) || !(fov > 1.0f && fov < 179.0f)) {
					if (!warned) {
						warned = true;
						logger::warn("vehicle: the active camera couldn't be read (system {}); blocks are drawn from V's FPP camera in cars", system != nullptr);
					}
					return false;
				}
				const float half = fov * 0.5f * 0.0174532925f;
				const auto  eye = CpToMc(transform.position.X, transform.position.Y, transform.position.Z);
				const auto  forwardMc = CpDirToMc(forward.X, forward.Y, forward.Z);
				const auto  upMc = CpDirToMc(up.X, up.Y, up.Z);
				CameraView  view{};
				view.fovYDeg = fovVertical ? fov : 2.0f * std::atan(std::tan(half) * 9.0f / 16.0f) * 57.2957795f;
				view.eye[0] = eye.x;
				view.eye[1] = eye.y;
				view.eye[2] = eye.z;
				view.forward[0] = float(forwardMc.x);
				view.forward[1] = float(forwardMc.y);
				view.forward[2] = float(forwardMc.z);
				view.up[0] = float(upMc.x);
				view.up[1] = float(upMc.y);
				view.up[2] = float(upMc.z);
				view.valid = true;
				SetCameraView(view);
				return true;
			}

			// In a vehicle Cyberpunk drives V; Minecraft's player is only brought along.
			bool   wasMounted = false;
			McVec  lastTeleportMc{ 0.0, 0.0, 0.0 };
			bool   warnedNoGround = false;

			// PlayerPuppet::GetMountedVehicle() -> VehicleObject, null on foot (per the RTTI dump).
			// Matched by its exact signature, so a different overload is never called.
			bool InVehicle(const rtti::Handle<rtti::IScriptable>& a_player)
			{
				static RED4ext::CBaseFunction* fn = [] {
					auto* found = rtti::FindFunction("PlayerPuppet", "GetMountedVehicle", {}, "handle:vehicleBaseObject");
					if (!found) {
						logger::warn("vehicle: PlayerPuppet::GetMountedVehicle() not found; Minecraft is not stopped in cars");
					}
					return found;
				}();
				if (!fn || !a_player) {
					return false;
				}
				rtti::Handle<rtti::IScriptable> vehicle;
				RED4ext::StackArgs_t            args;
				return RED4ext::ExecuteFunction(a_player.GetPtr(), fn, &vehicle, args) && vehicle;
			}

			void StopPuppet(Runtime& a_st)
			{
				if (a_st.puppeting) {
					logger::info("puppet off");
					Input::ReleaseAll();
					Puppet::Release();
				}
				a_st.puppeting = false;
				a_st.drawBlocks = false;
				const auto player = rtti::Player();
				ThirdPerson::Release(player);
				Look::Release(player);
			}

			// No Cyberpunk weapons while Minecraft is in its world, in a car too: keys Minecraft has no
			// control on are Cyberpunk's, and Alt drew a gun over Minecraft's hand. Set again every
			// second, not just when it changes: a quest scene ending can take restrictions off V, and
			// a load starts her without it (it's never saved).
			bool  weaponsWanted = false;
			bool  weaponsBlocked = false;
			float weaponsRecheck = 0.0f;

			void SyncWeapons(bool a_block, float a_delta)
			{
				weaponsRecheck -= a_delta;
				if (a_block == weaponsWanted && weaponsRecheck > 0.0f) {
					return;
				}
				weaponsWanted = a_block;
				weaponsRecheck = 1.0f;
				const auto on = Scripts::SetWeaponsBlocked(a_block);
				if (!on) {
					return;
				}
				if (*on != weaponsBlocked) {
					logger::info("weapons: {}", *on ? "holstered and locked while Minecraft runs" : "V can draw them again");
				}
				weaponsBlocked = *on;
			}

			// One-time discovery dump of the game API this plugin builds on (diagnostics only).
			void Probe()
			{
				for (const char* cls : { "gameSpatialQueriesSystem", "gameTeleportationFacility", "gameFPPCameraComponent", "gameCameraComponent",
						 "entEntity", "gameObject", "physicsTraceResult", "physicsQueryFilter", "gametargetingTargetingSystem", "gameStatPoolsSystem",
						 "gameTimeSystem" }) {
					rtti::DumpClass(cls);
				}
				// Time and weather sync (Environment.cpp): the weather system's real signatures.
				rtti::DumpClass("worldWeatherScriptInterface");
				rtti::DumpClass("worldWeatherState");
				rtti::DumpFunctionsMatching("ScriptGameInstance", { "GetWeatherSystem" });
				// Finding how to tell V is in a vehicle: VehicleComponent has no static
				// IsMountedToVehicle in 2.31.
				rtti::DumpFunctionsMatching(nullptr, { "Mount", "Vehicle" });
				// For TNT: building a TargetSearchQuery's filter and reading GetTargetParts' results.
				rtti::DumpFunctionsMatching(nullptr, { "TSF_", "TSFMV", "TargetPartInfo", "TS_" });
				rtti::DumpClass("gametargetingTargetPartInfo");
				rtti::DumpClass("gameTargetSearchFilter");
				for (const char* cls : { "PlayerPuppet", "ScriptedPuppet", "gameObject", "gamePuppet" }) {
					rtti::DumpFunctionsMatching(cls, { "Mount", "Vehicle", "Drive", "Camera" });
				}
				// For a third-person view with Minecraft's body: Cyberpunk's TPP camera, hiding V,
				// and finding components by name.
				for (const char* cls : { "PlayerPuppet", "ScriptedPuppet", "gamePuppet", "gameObject", "entEntity", "entGameEntity" }) {
					rtti::DumpFunctionsMatching(cls, { "TPP", "Tpp", "ThirdPerson", "Invisib", "Visib", "Appearance", "FindComponent", "Hide", "Render" });
				}
				rtti::DumpClass("gameTPPCameraComponent");
			}
		}

		// What Minecraft hears while V isn't its to play (a save loading, CyberCraft switched off):
		// the world, the collision epoch and the flags saying why, nothing else.
		void WriteIdleState(Link& a_link, std::uint32_t a_flags)
		{
			proto::CyberState idle{};
			idle.flags = a_flags;
			idle.worldId = kWorldId;
			idle.collisionEpoch = epoch;
			a_link.WriteCyberState(idle);
		}

		void Tick()
		{
			auto& st = State();
			auto& link = Link::Get();
			if (!link.Valid()) {
				return;
			}
			link.Heartbeat();
			Input::Install();    // no-op once the game window has been found and subclassed
			Overlay::Install();  // no-op once Present is hooked

			const auto now = std::chrono::steady_clock::now();
			const float delta = lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - lastFrame).count() : 0.0f;
			lastFrame = now;

			// "Enable CyberCraft" in Mod Settings. Off, Cyberpunk is as it is without CyberCraft: once
			// V has loaded in, Minecraft lets go of her and pauses (kCyberDisabled), and nothing of it is
			// drawn or spawned.
			const bool enabled = ModSettings::Enabled();
			st.enabled = enabled;
			if (enabled != wasEnabled) {
				wasEnabled = enabled;
				logger::info("CyberCraft turned {} in Mod Settings", enabled ? "on" : "off");
				if (enabled) {
					// Minecraft's player goes back to where V is now, with the city around her sent afresh.
					++epoch;
					teleportPending = true;
					Collision::Get().Reset(epoch);
					Combat::Reset();
					// Off at load, it wasn't started then.
					if (Launcher::GetStatus() == Launcher::Status::kOff && !Launcher::MinecraftRunning()) {
						Launcher::StartMinecraft();
					}
				} else {
					// The blocks' lights and colliders go with the blocks.
					Builds::Respawn();
				}
			}

			const bool mcAlive = link.McAlive();
			const bool haveMc = mcAlive && link.ReadMcState(mc);
			if (mcAlive && !mcWasAlive) {
				logger::info("Minecraft connected (pid {})", link.McPid());
				link.ResetOverlay();
				++epoch;
				teleportPending = true;
				st.gameMenuOpen = false;  // a fresh Minecraft starts with the controls
				Collision::Get().Reset(epoch);
				Combat::Reset();
			} else if (!mcAlive && mcWasAlive) {
				logger::info("Minecraft disconnected");
				// Its blocks aren't drawn any more, so their lights and colliders go too.
				Builds::Respawn();
			}
			mcWasAlive = mcAlive;
			// "Minecraft scale" changed (Mod Settings, CyberCraft.ini): V is somewhere else in
			// Minecraft's coordinates, as after Insert. The city is sent afresh, the blocks' lights
			// and colliders go where the blocks now are, and Minecraft's player is put back where V is.
			if (Grid::UpdateScale()) {
				++epoch;
				teleportPending = true;
				Collision::Get().Reset(epoch);
				Builds::Respawn();
			}
			st.mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);
			st.mcScreenOpen = haveMc && (mc.flags & proto::kMcScreenOpen);
			for (std::size_t i = 0; i < st.mcKeys.size(); ++i) {
				st.mcKeys[i] = haveMc ? mc.mcKeys[i] : 0u;
			}

			const bool menu = GamePaused(delta);
			if (menu != st.gameMenuOpen.load()) {
				logger::info("input: Cyberpunk {}", menu ? "paused (a menu is open), input is Cyberpunk's" : "running again, input is Minecraft's");
				Input::ReleaseAll();
			}
			st.gameMenuOpen = menu;
			// What the overlay's invert pass needs: Minecraft's crosshair is on screen in first
			// person with no screen open, and its GUI scale sizes the rect around it.
			st.mcGuiScale = haveMc ? static_cast<int>(mc.guiScale) : 0;
			st.mcCrosshair = haveMc && (mc.flags & proto::kMcInWorld) && !(mc.flags & proto::kMcScreenOpen) && mc.cameraMode == 0;

			auto             player = rtti::Player();
			RED4ext::Vector4 pos{};
			float            yaw = 0.0f;
			if (player) {
				getWorldPosition.Call(player.GetPtr(), &pos);
				getWorldYaw.Call(player.GetPtr(), &yaw);
			}
			const bool placed = player && !AtLoadingSpot(pos);
			SyncWeapons(enabled && placed && haveMc && st.mcInWorld, delta);
			if (placed && loadedSeconds == 0.0f) {
				pressesAtLoad = Input::GamePresses();
			}
			loadedSeconds = placed ? loadedSeconds + delta : 0.0f;
			// V is in place once the save has put her somewhere and the city has streamed in, and
			// the player has pressed something since: "Press Space to continue" shows with V already
			// placed, and Minecraft's UI over it was too early.
			// Then a few seconds more (fTakeoverDelay), while Cyberpunk fades the world in.
			static const float takeoverDelay = std::max(0.0f, Config::GetFloat(L"Minecraft", L"fTakeoverDelay", 5.0f));
			const bool         continued = Input::GamePresses() != pressesAtLoad;
			continuedSeconds = placed && continued ? continuedSeconds + delta : 0.0f;
			if (loadedSeconds < kSettleSeconds || !continued || continuedSeconds < takeoverDelay) {
				inGameSeconds = 0.0f;
				SetCameraView({});
				StopPuppet(st);
				// A load ends the session the blocks' lights and colliders lived in.
				Builds::Forget();
				// Wherever the save puts V, Minecraft's player follows once V is settled there.
				teleportPending = true;
				WriteIdleState(link, enabled ? 0u : proto::kCyberDisabled);
				return;
			}
			if (!enabled) {
				SetCameraView({});
				StopPuppet(st);
				// Minecraft's player follows V again from wherever she is once it's back on.
				teleportPending = true;
				WriteIdleState(link, proto::kCyberDisabled);
				// "Open Minecraft settings" has nothing to open now.
				ModSettings::Update(false);
				return;
			}
			if (!loggedLoadIn) {
				loggedLoadIn = true;
				logger::info("V loaded in at Cyberpunk ({:.2f}, {:.2f}, {:.2f})", pos.X, pos.Y, pos.Z);
				if (Config::Diagnostics()) {
					Collision::ProbeRays(pos);
				}
			}
			inGameSeconds += delta;
			if (!probed && inGameSeconds > 5.0f && Config::Diagnostics()) {
				probed = true;
				Probe();
			}

			auto here = CpToMc(pos.X, pos.Y, pos.Z);

			// Insert: shift Minecraft's grid so the ground under V is on a block boundary, and turn it
			// about her so its rows run the way she faces. Everything in Minecraft coordinates moves
			// with it: the collision is resent, and Minecraft's player is put back where V stands.
			if (Input::TakeAlignRequest()) {
				const auto ground = Collision::Get().FirstHitY(McVec{ here.x, here.y + 1.0, here.z }, McVec{ here.x, here.y - 3.0, here.z });
				if (ground) {
					Grid::AlignTo(McToCp(here.x, *ground, here.z).z);
				} else {
					logger::info("grid: no ground under V to align to");
				}
				Grid::TurnTo(pos.X, pos.Y, yaw);
				here = CpToMc(pos.X, pos.Y, pos.Z);
				++epoch;
				Collision::Get().Reset(epoch);
				Builds::Respawn();
				teleportPending = true;
				nudgeSettle = 0.0f;
			}

			// The arrows: move the grid a little forward, back, left or right of the way V faces. V
			// stays where she is in Night City, so Minecraft's player is moved back under her (a soft
			// teleport, below); the city's collision, a few hundredths of a block off meanwhile, is
			// resent once they stop.
			if (const auto [forward, left] = Input::TakeNudge(); forward != 0 || left != 0) {
				static const double kNudgeBlocks = std::clamp(Config::GetFloat(L"World", L"fNudgeBlocks", 1.0f / 16.0f), 0.001f, 1.0f);
				const double        step = kNudgeBlocks * MetresPerBlock();
				const double        heading = yaw * 0.017453292519943295;
				// V's facing (-sin, cos) and her left (-cos, -sin), Cyberpunk x and y.
				const double fx = -std::sin(heading), fy = std::cos(heading);
				const double lx = -fy, ly = fx;
				const auto before = here;
				Grid::Nudge(step * (forward * fx + left * lx), step * (forward * fy + left * ly));
				here = CpToMc(pos.X, pos.Y, pos.Z);
				// Minecraft's player moved the other way under the grid: by as much as V's own
				// coordinates did. From where it is, at its own height.
				const bool  behind = softPending || (teleportSeq != hardTeleportSeq && mc.teleportAck != teleportSeq);
				const McVec from = behind ? softTarget : McVec{ mc.x, mc.y, mc.z };
				softTarget = { from.x + here.x - before.x, from.y, from.z + here.z - before.z };
				softPending = true;
				nudgeSettle = 0.5f;
			}
			if (nudgeSettle > 0.0f) {
				nudgeSettle -= delta;
				if (nudgeSettle <= 0.0f) {
					nudgeSettle = 0.0f;
					++epoch;
					Collision::Get().Reset(epoch);
					Builds::Respawn();
				}
			}

			// In a vehicle: Cyberpunk keeps the controls, Minecraft's player is brought along every
			// 16 blocks, and once more on the way out before Minecraft takes over again.
			const bool mounted = InVehicle(player);
			if (mounted != wasMounted) {
				logger::info("{}", mounted ? "V got in a vehicle; Minecraft lets go" : "V got out of a vehicle");
				wasMounted = mounted;
				if (!mounted) {
					teleportPending = true;
				}
			}
			if (mounted) {
				StopPuppet(st);
				const double drift = std::max({ std::abs(here.x - lastTeleportMc.x), std::abs(here.y - lastTeleportMc.y), std::abs(here.z - lastTeleportMc.z) });
				if (drift > 16.0) {
					teleportPending = true;
				}
			}

			// A nudge before Minecraft drives V (or with a teleport due anyway) is a teleport.
			if (softPending && (teleportPending || !st.puppeting)) {
				softPending = false;
				teleportPending = true;
			}
			if (softPending) {
				++teleportSeq;
				softPending = false;
				softHold = 0.0f;
				if (Config::Diagnostics()) {
					logger::info("teleport #{} (nudge) to MC ({:.3f}, {:.3f}, {:.3f})", teleportSeq, softTarget.x, softTarget.y, softTarget.z);
				}
			}
			if (teleportPending) {
				++teleportSeq;
				hardTeleportSeq = teleportSeq;
				teleportPending = false;
				lastTeleportMc = here;
				st.yaw = CpYawToMc(yaw);
				st.pitch = 0.0f;
				st.lookInitialized = true;
				logger::info("teleport #{} to MC ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f} (Cyberpunk ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f})", teleportSeq,
					here.x, here.y, here.z, st.yaw, pos.X, pos.Y, pos.Z, yaw);
			}

			// Minecraft drives the player once it is in its world and has applied our teleport
			// (DESIGN.md §6). Its own death screen keeps the body still, so stop puppeting then.
			// Also only from where V already is: a stale ack once let Minecraft's spawn point
			// (8, -1024, 8) pull V a kilometre under the city.
			// And never before the rays have found a single surface: with no ground known, Minecraft's
			// player only falls, and V with it.
			const double fromV = std::max({ std::abs(mc.x - here.x), std::abs(mc.y - here.y), std::abs(mc.z - here.z) });
			const bool   ground = Collision::Get().Hits() > 0;
			// Not while V is dead either: Cyberpunk's death screen needs the mouse, and Minecraft's
			// overlay (it only draws while Minecraft drives V) has no business over it.
			// Behind on nudges only, Minecraft is still where V is (give or take a nudge).
			const bool   nudging = teleportSeq != hardTeleportSeq && mc.teleportAck != teleportSeq;
			const bool   arrived = mc.teleportAck == teleportSeq || (nudging && st.puppeting && mc.teleportAck >= hardTeleportSeq && mc.teleportAck < teleportSeq);
			const bool   ready = haveMc && st.mcInWorld && arrived && !(mc.flags & proto::kMcDead) && !mounted && !st.vDead;
			if (ready && !ground && !warnedNoGround) {
				warnedNoGround = true;
				logger::warn("collision: no surface found around V yet; Minecraft won't drive V until one is");
			}
			const bool puppet = ready && ground && (st.puppeting || fromV < 8.0);
			if (puppet != st.puppeting) {
				logger::info("puppet {}", puppet ? "on (Minecraft drives V)" : "off");
				if (!puppet) {
					Input::ReleaseAll();
					Puppet::Release();
					ThirdPerson::Release(player);
				}
			}
			st.puppeting = puppet;
			// In a car the blocks stay: drawn, and the sections (and so the colliders cars meet) still
			// coming in as Minecraft's player is brought along.
			st.drawBlocks = puppet || (mounted && haveMc && st.mcInWorld && !st.vDead);
			// F is Cyberpunk's while it offers something to do with it, Minecraft's otherwise
			// (bFToMinecraft = 0: always Cyberpunk's, as before Minecraft could have it).
			if (puppet) {
				static const bool fToMinecraft = Config::GetBool(L"Input", L"bFToMinecraft", true);
				st.cpInteraction = !fToMinecraft || Scripts::InteractionShown().value_or(true);
			}
			// Cyberpunk's Mod Settings asked for Minecraft's options.
			ModSettings::Update(puppet);

			// This plugin owns the mouse while Minecraft drives the player, so look is integrated
			// here, exactly as Minecraft's mouse turns its player (Look.cpp), and sent in CyberState
			// (DESIGN.md §7).
			static Look::Options lookOptions;
			if (haveMc) {
				lookOptions.sensitivity = mc.sensitivity;
				lookOptions.smooth = (mc.flags & proto::kMcSmoothCamera) != 0;
				lookOptions.scoping = (mc.flags & proto::kMcScoping) != 0;
				lookOptions.invertX = (mc.flags & proto::kMcInvertX) != 0;
				lookOptions.invertY = (mc.flags & proto::kMcInvertY) != 0;
			}
			float lookDx = 0.0f;
			float lookDy = 0.0f;
			Input::ConsumeLook(lookDx, lookDy);
			const bool looking = puppet && !st.mcScreenOpen && !st.gameMenuOpen;
			const auto turn = Look::Integrate(lookDx, lookDy, delta, looking, lookOptions);
			// Turning is this plugin's while Minecraft drives V: it goes out with V's teleport, which
			// would override any turn Cyberpunk made itself. Otherwise V's own heading is the look.
			if (looking) {
				st.yaw = std::fmod(st.yaw + turn.yaw, 360.0f);
			} else {
				st.yaw = CpYawToMc(yaw);
			}
			const auto cameraPitch = ReadCamera(player, st.yaw);
			if (mounted && st.drawBlocks) {
				ReadActiveCamera();
			}
			// Hide the faces of placed blocks that the city stands in front of, a slice per frame.
			if (const auto camera = GetCameraView(); camera.valid && !World::DepthActive()) {
				static const int occlusionRays = std::max(0, static_cast<int>(Config::GetFloat(L"World", L"iOcclusionRays", 400.0f)));
				World::UpdateOcclusion(McVec{ camera.eye[0], camera.eye[1], camera.eye[2] }, occlusionRays);
			}
			// Up and down: Cyberpunk's camera held at Minecraft's pitch.
			st.pitch = Look::Pitch(player, puppet, looking, turn, cameraPitch, st.pitch, delta);
			// V held still from a nudge until Minecraft's player has arrived and ticked there.
			if (nudging) {
				softHold = 0.15f;
			} else if (softHold > 0.0f) {
				softHold -= delta;
				if (softHold <= 0.0f) {
					softHold = 0.0f;
					ticks.clear();  // in the grid's old coordinates, or half way between
				}
			}
			const auto feet = SmoothedFeet(mc);
			// Stuck (bDiagnostics): a movement key held, and Minecraft's player hasn't moved for a
			// quarter second. Once per time, with what Minecraft was sent in front of it, and where V
			// stands.
			if (Config::Diagnostics()) {
				static float stuckFor = 0.0f;
				static McVec stuckAt{ 0.0, 0.0, 0.0 };
				static bool  reported = false;
				const auto [forward, left] = Input::HeldMovement();
				const bool moved = std::abs(mc.x - stuckAt.x) > 0.02 || std::abs(mc.z - stuckAt.z) > 0.02;
				if (!puppet || (forward == 0 && left == 0) || moved || st.mcScreenOpen) {
					stuckFor = 0.0f;
					stuckAt = { mc.x, mc.y, mc.z };
					reported = false;
				} else if ((stuckFor += delta) > 0.25f && !reported) {
					reported = true;
					// Minecraft yaw: facing (-sin, cos) and its left (cos, sin) in x, z.
					const double y = st.yaw * 0.017453292519943295;
					const double dx = forward * -std::sin(y) + left * std::cos(y);
					const double dz = forward * std::cos(y) + left * std::sin(y);
					const auto   offset = Puppet::Offset();
					logger::info("stuck: Minecraft's player at ({:.3f}, {:.3f}, {:.3f}) (on ground {}) hasn't moved for {:.2f} s with keys forward {} left {}, "
								 "look yaw {:.1f}; V at MC ({:.3f}, {:.3f}, {:.3f}), standing ({:+.3f}, {:+.3f}) m off it. Sent ahead (left, middle, right "
								 "edge; a quarter block under the feet to two blocks up):{}",
						mc.x, mc.y, mc.z, (mc.flags & proto::kMcOnGround) != 0, stuckFor, forward, left, st.yaw, here.x, here.y, here.z, offset.x, offset.y,
						Collision::Get().DescribeAhead(McVec{ mc.x, mc.y, mc.z }, dx, dz));
				}
			}
			if (puppet && softHold <= 0.0f) {
				Puppet::Apply(feet, st.yaw, CpVec{ pos.X, pos.Y, pos.Z }, yaw, (mc.flags & proto::kMcOnGround) != 0);
				st.lastPuppeted = now;
			}
			// Minecraft's F5: Cyberpunk's camera pulled back. V hidden (in first person too), and
			// Minecraft's body drawn at the feet V stands on. And in any view, the camera at
			// Minecraft's eye (1.62 blocks standing, so lower than V's own below a metre a block).
			if (puppet) {
				const float eyeShift = mc.eyeHeight > 0.0f ? float(mc.eyeHeight * MetresPerBlock() - kCyberpunkEyeHeight) : 0.0f;
				ThirdPerson::Update(player, mc.cameraMode, mc.cameraDistance, st.yaw, st.pitch, McVec{ feet.x, feet.y + mc.eyeHeight, feet.z }, eyeShift);
			} else {
				ThirdPerson::Release(player);
			}
			// The body stands where V is now, read with the camera this frame, not at the feet she is
			// only teleported to above: those run a frame's walk ahead of the camera, by however long
			// the frame was, and the body jiggled in front of it while walking.
			{
				auto view = GetCameraView();
				view.avatar = ThirdPerson::Active();
				view.feet[0] = here.x;
				view.feet[1] = here.y;
				view.feet[2] = here.z;
				SetCameraView(view);
			}

			proto::CyberState toMc{};
			toMc.flags = proto::kCyberInGame;
			if (st.gameMenuOpen) {
				toMc.flags |= proto::kCyberMenuOpen;
			}
			if (World::DrawsHand()) {
				toMc.flags |= proto::kCyberDrawsHand;
			}
			if (ThirdPerson::FirstPersonBody()) {
				toMc.flags |= proto::kCyberBodyFirstPerson;
				toMc.bodyBack = ThirdPerson::BodyBack();
			}
			toMc.worldId = kWorldId;
			toMc.collisionEpoch = epoch;
			// Where a teleport sends Minecraft's player: V's feet, or for a nudge where it stands.
			const McVec sendTo = nudging ? softTarget : here;
			toMc.posX = sendTo.x;
			toMc.posY = sendTo.y;
			toMc.posZ = sendTo.z;
			toMc.yaw = st.yaw;
			toMc.pitch = st.pitch;
			toMc.teleportSeq = teleportSeq;
			toMc.viewportW = static_cast<std::uint32_t>(st.viewportW.load());
			toMc.viewportH = static_cast<std::uint32_t>(st.viewportH.load());
			// Night City's clock and weather: Minecraft's time and weather follow them (Environment).
			const auto hour = Environment::Hour();
			toMc.gameHour = hour.value_or(12.0f);
			if (hour) {
				toMc.flags |= proto::kCyberClock;
			}
			toMc.weather = Environment::Weather();
			// Full daylight from mid-morning to late afternoon, a moonlit 0.15 around midnight.
			st.daylight = std::clamp(0.5f + std::cos((toMc.gameHour - 12.0f) / 24.0f * 6.2831853f), 0.15f, 1.0f);
			link.WriteCyberState(toMc);

			Collision::Get().Update(here);
			// Cars meet Minecraft's player only while it drives V: in one, it rides inside V's.
			Collision::Get().UpdateVehicles(here, puppet);
			Combat::Update(here);
			if (haveMc && st.mcInWorld) {
				Builds::Update(here);
			}

			static float logTimer = 0.0f;
			logTimer -= delta;
			if (logTimer <= 0.0f) {
				logTimer = 5.0f;
				const auto puppetStats = Puppet::TakeStats();
				logger::info("V at Cyberpunk ({:.2f}, {:.2f}, {:.2f}) yaw {:.1f}; Minecraft {} flags {:#x} at ({:.2f}, {:.2f}, {:.2f}) ack {}; look yaw {:.1f} pitch {:.1f} ({}); {} teleports, V sank up to {:.3f} m between them; "
							 "Cyberpunk pushed her aside {} times, up to {:.3f} m, and she stood up to {:.3f} m off Minecraft's player",
					pos.X, pos.Y, pos.Z, yaw, haveMc ? "linked" : (mcAlive ? "alive" : "absent"), mc.flags, mc.x, mc.y, mc.z, mc.teleportAck, st.yaw,
					st.pitch, Look::PitchSource(), puppetStats.teleports, puppetStats.maxSink, puppetStats.pushes, puppetStats.maxPush, puppetStats.maxOffset);
			}
		}
	}
}
