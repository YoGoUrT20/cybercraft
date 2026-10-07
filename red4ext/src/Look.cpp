#include "Look.h"

#include "Config.h"
#include "Input.h"

namespace cybercraft::Look
{
	namespace
	{
		// net.minecraft.util.SmoothDouble, the cinematic camera's easing (one per axis).
		struct SmoothDouble
		{
			double targetValue = 0.0;
			double remainingValue = 0.0;
			double lastAmount = 0.0;

			double GetNewDeltaValue(double a_accumulated, double a_amount)
			{
				targetValue += a_accumulated;
				double       delta = targetValue - remainingValue;
				const double lerped = lastAmount + 0.5 * (delta - lastAmount);  // Mth.lerp(0.5, lastAmount, delta)
				const double sign = delta > 0.0 ? 1.0 : (delta < 0.0 ? -1.0 : 0.0);
				if (sign * delta > sign * lastAmount) {
					delta = lerped;
				}
				lastAmount = lerped;
				remainingValue += delta * a_amount;
				return delta * a_amount;
			}

			void Reset() { *this = {}; }
		};
		SmoothDouble smoothTurnX;
		SmoothDouble smoothTurnY;

		enum class Mode
		{
			kPin,     // Cyberpunk's camera held at Minecraft's pitch through its limits
			kCounts,  // Cyberpunk tilts it from the mouse, scaled to Minecraft's speed; its pitch is read back
			kOwn,     // nothing tilts it (bNativeLook = 0 and the limits didn't hold): Minecraft's pitch alone
		};

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

		// gameFPPCameraComponent's pitchMin and pitchMax, Float properties per the RTTI dump.
		struct Limits
		{
			RED4ext::CProperty* min{ nullptr };
			RED4ext::CProperty* max{ nullptr };
		};

		const Limits& LimitProperties()
		{
			static const Limits limits = [] {
				auto* rtti = RED4ext::CRTTISystem::Get();
				auto* cls = rtti ? rtti->GetClass("gameFPPCameraComponent") : nullptr;
				const auto find = [&](const char* a_name) -> RED4ext::CProperty* {
					auto* prop = cls ? cls->GetProperty(a_name) : nullptr;
					const char* type = prop && prop->type ? prop->type->GetName().ToString() : nullptr;
					return type && std::string_view(type) == "Float" ? prop : nullptr;
				};
				Limits found{ find("pitchMin"), find("pitchMax") };
				if (!found.min || !found.max) {
					logger::warn("look: gameFPPCameraComponent has no Float pitchMin and pitchMax; Cyberpunk tilts its camera from the mouse");
				}
				return found;
			}();
			return limits;
		}

		// Holding the camera's pitch limits at Minecraft's pitch, and checking that the camera goes there.
		struct Pin
		{
			RED4ext::IScriptable* camera{ nullptr };  // whose limits are held; only compared once let go
			float                 ownMin{ 0.0f };     // its own limits, put back when Minecraft lets go
			float                 ownMax{ 0.0f };
			float                 held{ 0.0f };       // what both are set to
			// Learned from the first camera and kept: degrees or radians, and which way is up
			// (Cyberpunk's pitch is positive looking up, Minecraft's looking down).
			bool                  unitKnown{ false };
			float                 perDegree{ 1.0f };
			float                 up{ -1.0f };
			bool                  flipped{ false };
			bool                  verified{ false };
			// The check: Minecraft's pitch held the last frames (newest first), whether it has moved
			// since holding began, and how long the camera has followed it or not.
			std::array<float, 3>  recent{};
			int                   recentCount{ 0 };
			float                 start{ 0.0f };
			bool                  moved{ false };
			float                 followed{ 0.0f };
			float                 missed{ 0.0f };
		} pin;

		Mode                 mode = Mode::kPin;
		bool                 modeChosen = false;
		std::optional<float> target;  // Minecraft's pitch while it drives V (pin mode)

		bool Hold(const rtti::Handle<rtti::IScriptable>& a_camera, float a_mcPitch)
		{
			const auto& props = LimitProperties();
			auto*       camera = a_camera.GetPtr();
			if (!props.min || !props.max || !camera) {
				return false;
			}
			const float min = props.min->GetValue<float>(camera);
			const float max = props.max->GetValue<float>(camera);
			if (camera != pin.camera) {
				pin.camera = camera;
				pin.ownMin = min;
				pin.ownMax = max;
				pin.recentCount = 0;
				pin.start = a_mcPitch;
				pin.moved = false;
				pin.followed = 0.0f;
				pin.missed = 0.0f;
				if (!pin.unitKnown) {
					pin.unitKnown = true;
					const bool radians = max > min && std::max(std::abs(min), std::abs(max)) <= 1.6f;
					pin.perDegree = radians ? 0.0174532925f : 1.0f;
					logger::info("look: Cyberpunk's camera pitch limits are {:.3f} .. {:.3f} ({}); held at Minecraft's pitch while it drives V", min, max,
						radians ? "radians" : "degrees");
				}
			} else if (min != pin.held || max != pin.held) {
				// The game set limits of its own meanwhile (a scene, a state): those go back instead.
				pin.ownMin = min;
				pin.ownMax = max;
			}
			const float value = pin.up * a_mcPitch * pin.perDegree;
			props.min->SetValue<float>(camera, value);
			props.max->SetValue<float>(camera, value);
			pin.held = value;
			pin.recent = { a_mcPitch, pin.recent[0], pin.recent[1] };
			pin.recentCount = std::min(pin.recentCount + 1, 3);
			pin.moved |= std::abs(a_mcPitch - pin.start) > 2.0f;
			return true;
		}

		void LetGo(const rtti::Handle<rtti::IScriptable>& a_player)
		{
			if (!pin.camera) {
				return;
			}
			const auto& props = LimitProperties();
			const auto  camera = Camera(a_player);
			// Only on the camera they came from, and only if nothing has set them since.
			if (camera && camera.GetPtr() == pin.camera && props.min->GetValue<float>(pin.camera) == pin.held &&
				props.max->GetValue<float>(pin.camera) == pin.held) {
				props.min->SetValue<float>(pin.camera, pin.ownMin);
				props.max->SetValue<float>(pin.camera, pin.ownMax);
			}
			pin.camera = nullptr;
			pin.recentCount = 0;
		}

		// Whether the camera follows: as V looks up and down it should sit at Minecraft's pitch of
		// one of the last frames (it can trail by a frame or two). Not near straight up or down, where
		// it may stop short of 90. Returns false once it plainly doesn't.
		bool Check(float a_cameraPitch, float a_seconds)
		{
			if (pin.recentCount == 0 || std::abs(pin.recent[0]) > 60.0f) {
				return true;
			}
			float lo = pin.recent[0];
			float hi = pin.recent[0];
			for (int i = 1; i < pin.recentCount; ++i) {
				lo = std::min(lo, pin.recent[i]);
				hi = std::max(hi, pin.recent[i]);
			}
			lo -= 0.5f;
			hi += 0.5f;
			if (a_cameraPitch >= lo && a_cameraPitch <= hi) {
				pin.missed = 0.0f;
				if (pin.moved && !pin.verified && (pin.followed += a_seconds) >= 0.25f) {
					pin.verified = true;
					logger::info("look: Cyberpunk's camera follows Minecraft's pitch");
				}
				return true;
			}
			// Only judged once Minecraft's pitch has moved: a camera standing a little off a pitch that
			// hasn't (a fall or a landing at the start nudges it) isn't one ignoring its limits, and
			// giving up on them lasts the whole session.
			if (pin.verified || !pin.moved || (pin.missed += a_seconds) < 1.0f) {
				return true;
			}
			if (!pin.flipped && -a_cameraPitch >= lo && -a_cameraPitch <= hi) {
				// Mirrored: the limits count up the other way.
				pin.flipped = true;
				pin.up = -pin.up;
				pin.missed = 0.0f;
				logger::info("look: Cyberpunk's camera tilted the other way; its pitch limits count up as Minecraft's does");
				return true;
			}
			logger::warn("look: Cyberpunk's camera stays at pitch {:.1f} with its limits held at Minecraft's {:.1f}; {}", a_cameraPitch, pin.recent[0],
				Input::NativeLook() ? "it tilts from the mouse again, scaled to Minecraft's speed" : "bNativeLook = 0, so the view only turns left and right");
			return false;
		}

		// Cyberpunk tilts its camera at its own vertical sensitivity, not Minecraft's. How far it tilts
		// per count it reads is measured as V looks around, and what it reads is scaled to match
		// (Input.cpp). Only away from the pitch limits, where movement stops tilting.
		struct PitchMatch
		{
			float                gain = 0.0f;  // Minecraft degrees per count Cyberpunk reads; 0 until measured
			float                dy = 0.0f;    // the measurement so far: counts read, and the tilt they made
			float                tilt = 0.0f;
			std::optional<float> last;         // the camera's pitch the frame before
		} pitchMatch;

		// a_perCount: Minecraft degrees one count tilts by (negative with the mouse inverted);
		// a_read: counts Cyberpunk read since the last frame.
		void MatchPitch(bool a_looking, const std::optional<float>& a_cameraPitch, float a_perCount, float a_read)
		{
			static const bool enable = Config::GetBool(L"Input", L"bMatchPitch", true);
			const float       sign = a_perCount < 0.0f ? -1.0f : 1.0f;
			if (!enable) {
				Input::SetPitchScale(sign);
				return;
			}
			const float factor = std::abs(a_perCount);
			auto&       m = pitchMatch;
			const bool  clear = a_looking && a_cameraPitch && m.last && std::abs(*a_cameraPitch) < 60.0f && std::abs(*m.last) < 60.0f;
			if (clear) {
				m.dy += a_read;
				m.tilt += *a_cameraPitch - *m.last;
				// Over several frames' worth, so a frame's counts tilting the camera a frame later
				// hardly matters.
				if (std::abs(m.dy) >= 500.0f) {
					const float sample = std::abs(m.tilt / m.dy);
					// One needing more than the scale's own range is a camera that barely moved (still
					// settling, or held), not Cyberpunk's sensitivity: kept out, or it would set a 20x tilt.
					if (sample > 1e-4f && sample < 10.0f && factor / sample <= 20.0f && factor / sample >= 0.05f) {
						if (m.gain == 0.0f) {
							logger::info("look: Cyberpunk tilts {:.4f} degrees per count, turning is {:.4f}: up and down scaled by {:.2f}", sample, factor,
								factor / sample);
						}
						m.gain = m.gain > 0.0f ? m.gain + (sample - m.gain) * 0.25f : sample;
					}
					m.dy = 0.0f;
					m.tilt = 0.0f;
				}
			} else {
				m.dy = 0.0f;
				m.tilt = 0.0f;
			}
			m.last = a_looking ? a_cameraPitch : std::nullopt;
			Input::SetPitchScale(sign * (m.gain > 0.0f ? std::clamp(factor / m.gain, 0.05f, 20.0f) : 1.0f));
		}
	}

	Turn Integrate(float a_dx, float a_dy, float a_seconds, bool a_looking, const Options& a_options)
	{
		// MouseHandler.turnPlayer: the slider through a cube, eight times that per count unless the
		// spyglass is up. Its float constants, widened to double as Java does.
		const double s = double(a_options.sensitivity) * 0.6000000238418579 + 0.20000000298023224;
		const double scoped = s * s * s;
		const double normal = scoped * 8.0;
		Turn         turn;
		turn.pitchPerCount = float(a_options.scoping && !a_options.smooth ? scoped : normal) * 0.15f * (a_options.invertY ? -1.0f : 1.0f);
		if (!a_looking) {
			return turn;
		}
		double x = 0.0;
		double y = 0.0;
		if (a_options.smooth) {
			x = smoothTurnX.GetNewDeltaValue(double(a_dx) * normal, double(a_seconds) * normal);
			y = smoothTurnY.GetNewDeltaValue(double(a_dy) * normal, double(a_seconds) * normal);
		} else {
			smoothTurnX.Reset();
			smoothTurnY.Reset();
			const double k = a_options.scoping ? scoped : normal;
			x = double(a_dx) * k;
			y = double(a_dy) * k;
		}
		// Entity.turn, in floats as Minecraft has them.
		turn.yaw = float(a_options.invertX ? -x : x) * 0.15f;
		turn.pitch = float(a_options.invertY ? -y : y) * 0.15f;
		return turn;
	}

	float Pitch(const rtti::Handle<rtti::IScriptable>& a_player, bool a_driving, bool a_looking, const Turn& a_turn,
		const std::optional<float>& a_cameraPitch, float a_pitch, float a_seconds)
	{
		if (!modeChosen) {
			modeChosen = true;
			const bool pinned = Config::GetBool(L"Input", L"bPinPitch", true) && LimitProperties().min && LimitProperties().max;
			mode = pinned ? Mode::kPin : (Input::NativeLook() ? Mode::kCounts : Mode::kOwn);
		}
		const float read = Input::ConsumeNativeDy();
		if (!a_driving) {
			Release(a_player);
			return a_cameraPitch.value_or(a_pitch);
		}

		if (mode == Mode::kPin) {
			// Cyberpunk reads no vertical movement: its camera goes where it is held.
			Input::SetPitchScale(0.0f);
			if (!target) {
				target = a_cameraPitch.value_or(a_pitch);
			}
			if (a_looking && a_cameraPitch && pin.camera && !Check(*a_cameraPitch, a_seconds)) {
				LetGo(a_player);
				target.reset();
				mode = Input::NativeLook() ? Mode::kCounts : Mode::kOwn;
			} else {
				if (a_looking) {
					target = std::clamp(*target + a_turn.pitch, -90.0f, 90.0f);
				}
				Hold(Camera(a_player), *target);
				return *target;
			}
		}

		if (mode == Mode::kCounts) {
			// Cyberpunk tilted its camera itself, from the scaled counts: Minecraft looks up and down
			// the same way.
			MatchPitch(a_looking, a_cameraPitch, a_turn.pitchPerCount, read);
			if (a_cameraPitch) {
				return *a_cameraPitch;
			}
		}
		return a_looking ? std::clamp(a_pitch + a_turn.pitch, -90.0f, 90.0f) : a_pitch;
	}

	void Release(const rtti::Handle<rtti::IScriptable>& a_player)
	{
		LetGo(a_player);
		target.reset();
		smoothTurnX.Reset();
		smoothTurnY.Reset();
	}

	const char* PitchSource()
	{
		switch (mode) {
		case Mode::kPin: return pin.verified ? "held, followed" : "held";
		case Mode::kCounts: return "Cyberpunk's, scaled";
		default: return "this plugin's";
		}
	}
}
