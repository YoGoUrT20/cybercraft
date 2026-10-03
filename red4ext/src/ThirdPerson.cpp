#include "ThirdPerson.h"

#include "Collision.h"
#include "Config.h"
#include "Scripts.h"

namespace cybercraft::ThirdPerson
{
	namespace
	{
		struct Settings
		{
			bool  enable;
			bool  orbitPitch;
			bool  hideV;          // in first person too
			bool  minecraftBody;  // Minecraft's body in her place in first person
			float back;           // blocks it stands behind the eye
		};

		const Settings& Config()
		{
			static const Settings settings{ Config::GetBool(L"ThirdPerson", L"bEnable", true), Config::GetBool(L"ThirdPerson", L"bOrbitPitch", false),
				Config::GetBool(L"Body", L"bHideV", true), Config::GetBool(L"Body", L"bMinecraftBody", true),
				std::clamp(Config::GetFloat(L"Body", L"fBack", 0.2f), 0.0f, 1.0f) };
			return settings;
		}

		// entIPlacedComponent's local transform, per the RTTI dump: relative to what the camera hangs on.
		rtti::Method getLocalPosition{ "entIPlacedComponent", "GetLocalPosition" };
		rtti::Method getLocalOrientation{ "entIPlacedComponent", "GetLocalOrientation" };
		rtti::Method setLocalPosition{ "entIPlacedComponent", "SetLocalPosition" };
		rtti::Method setLocalOrientation{ "entIPlacedComponent", "SetLocalOrientation" };

		rtti::Handle<rtti::IScriptable> Camera(const rtti::Handle<rtti::IScriptable>& a_player)
		{
			static RED4ext::CBaseFunction* get = rtti::FindFunction("PlayerPuppet", "GetFPPCameraComponent", {}, "handle:gameFPPCameraComponent");
			rtti::Handle<rtti::IScriptable> camera;
			if (!get || !a_player) {
				return camera;
			}
			RED4ext::StackArgs_t args;
			RED4ext::ExecuteFunction(a_player.GetPtr(), get, &camera, args);
			return camera;
		}

		RED4ext::Quaternion Multiply(const RED4ext::Quaternion& a_q, const RED4ext::Quaternion& a_p)
		{
			return RED4ext::Quaternion{ a_q.r * a_p.i + a_q.i * a_p.r + a_q.j * a_p.k - a_q.k * a_p.j, a_q.r * a_p.j - a_q.i * a_p.k + a_q.j * a_p.r + a_q.k * a_p.i,
				a_q.r * a_p.k + a_q.i * a_p.j - a_q.j * a_p.i + a_q.k * a_p.r, a_q.r * a_p.r - a_q.i * a_p.i - a_q.j * a_p.j - a_q.k * a_p.k };
		}

		bool                active = false;     // third person: the camera behind or in front
		bool                hidden = false;     // V's body is off, Minecraft's is drawn in her place
		int                 laterHides = 0;     // times more of V went off while she was hidden
		bool                moved = false;      // the camera is off its own local transform
		std::uint32_t       mode = 0;           // the McState::cameraMode last applied
		RED4ext::Vector4    basePosition{};     // the camera's own local transform, put back on release
		RED4ext::Quaternion baseOrientation{};
		std::chrono::steady_clock::time_point nextHide{};

		// V off, and again every second while she stays off: what comes on since (clothes put on in
		// the inventory) goes off too. Nothing hidden (no Codeware): V stays, and no body is drawn.
		void HideBody()
		{
			const auto now = std::chrono::steady_clock::now();
			if (now < nextHide) {
				return;
			}
			nextHide = now + std::chrono::seconds(1);
			const int count = Scripts::SetPlayerBodyVisible(false);
			if (count <= 0) {
				return;
			}
			if (!hidden) {
				logger::info("body: {} parts of V hidden, Minecraft's body in her place", count);
			} else if (++laterHides <= 8) {
				logger::info("body: {} more parts of V hidden (they came on since)", count);
			}
			hidden = true;
		}

		void ShowBody()
		{
			nextHide = {};
			if (!hidden) {
				return;
			}
			hidden = false;
			laterHides = 0;
			const int shown = Scripts::SetPlayerBodyVisible(true);
			logger::info("body: {} parts of V shown again", shown);
		}

		void RestoreCamera(const rtti::Handle<rtti::IScriptable>& a_player)
		{
			if (!moved) {
				return;
			}
			moved = false;
			if (const auto camera = Camera(a_player)) {
				setLocalPosition.Call(camera.GetPtr(), nullptr, basePosition);
				setLocalOrientation.Call(camera.GetPtr(), nullptr, baseOrientation);
			}
		}
	}

	void Update(const rtti::Handle<rtti::IScriptable>& a_player, std::uint32_t a_mode, float a_distance, float a_mcYaw, float a_mcPitch,
		const McVec& a_headMc, float a_eyeShift)
	{
		const auto& cfg = Config();
		const bool  third = cfg.enable && (a_mode == 1 || a_mode == 2);
		const bool  shifted = std::abs(a_eyeShift) > 0.005f;
		if (third || cfg.hideV) {
			HideBody();
		} else {
			ShowBody();
		}
		if (!third) {
			if (active) {
				active = false;
				logger::info("third person off");
			}
			if (!shifted) {
				RestoreCamera(a_player);
				return;
			}
		}
		const auto camera = Camera(a_player);
		if (!camera) {
			return;
		}
		if (!moved) {
			if (!getLocalPosition.Call(camera.GetPtr(), &basePosition) || !getLocalOrientation.Call(camera.GetPtr(), &baseOrientation)) {
				return;
			}
			moved = true;
		}
		if (third && !active) {
			active = true;
			mode = a_mode;
			logger::info("third person on ({}): the camera moves from ({:.2f}, {:.2f}, {:.2f})", a_mode == 1 ? "behind" : "in front", basePosition.X,
				basePosition.Y, basePosition.Z);
		} else if (third && a_mode != mode) {
			mode = a_mode;
			logger::info("third person: {}", a_mode == 1 ? "behind" : "in front");
		}

		// In the camera's frame (x right, y forward, z up). That frame already pitches with the look
		// (bOrbitPitch = 0), so straight down in the world is tilted in it.
		const double     pitch = a_mcPitch * 0.0174532925;  // Minecraft: positive looking down
		const double     cpPitch = -pitch;                   // Cyberpunk: positive looking up
		RED4ext::Vector4 position = basePosition;
		// To Minecraft's eye: lower at a small scale, and lower still sneaking, crawling, swimming.
		if (shifted) {
			const double shift = a_eyeShift;  // down when negative
			position.Y += cfg.orbitPitch ? 0.0f : float(shift * std::sin(cpPitch));
			position.Z += cfg.orbitPitch ? float(shift) : float(shift * std::cos(cpPitch));
		}
		if (third) {
			// Minecraft's own distance (4 blocks by default, shorter where its blocks are in the way),
			// and shorter still where Night City is.
			double       distance = std::clamp(a_distance > 0.0f ? double(a_distance) : 4.0, 0.5, 16.0);
			const double yaw = a_mcYaw * 0.0174532925;
			const double side = a_mode == 1 ? -1.0 : 1.0;  // behind the look, or out in front of it
			const McVec  look{ -std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
			const McVec  wanted{ a_headMc.x + look.x * side * distance, a_headMc.y + look.y * side * distance, a_headMc.z + look.z * side * distance };
			if (const auto hit = Collision::Get().HitDistance(a_headMc, wanted)) {
				distance = std::max(0.3, std::min(distance, *hit - 0.3));
			}
			// Along the look, so the camera keeps the head in the middle of the picture as it swings
			// up and down: straight back along y, since the frame pitches already. Adding the pitch
			// again (bOrbitPitch) swung the camera twice as far, under V looking up and out in front
			// of her looking down.
			const double orbit = cfg.orbitPitch ? cpPitch : 0.0;
			const double metres = distance * MetresPerBlock();
			position.Y += float(side * metres * std::cos(orbit));
			position.Z += float(side * metres * std::sin(orbit));
		}
		setLocalPosition.Call(camera.GetPtr(), nullptr, position);
		// In front, turned round to face V.
		const RED4ext::Quaternion halfTurn{ 0.0f, 0.0f, 1.0f, 0.0f };
		const RED4ext::Quaternion orientation = third && a_mode == 2 ? Multiply(baseOrientation, halfTurn) : baseOrientation;
		setLocalOrientation.Call(camera.GetPtr(), nullptr, orientation);
	}

	void Release(const rtti::Handle<rtti::IScriptable>& a_player)
	{
		ShowBody();
		active = false;
		RestoreCamera(a_player);
	}

	bool Active()
	{
		return hidden && (active || Config().minecraftBody);
	}

	bool FirstPersonBody()
	{
		return hidden && Config().minecraftBody;
	}

	float BodyBack()
	{
		return Config().back;
	}
}
