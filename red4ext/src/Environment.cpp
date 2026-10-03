#include "Environment.h"

#include "Config.h"
#include "Rtti.h"

#include <cctype>

namespace cybercraft::Environment
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// gameTimeSystem::GetGameTime() -> GameTime and SetGameTimeBySeconds(seconds: Int32), per the
		// RTTI dump (2.31). GameTime is one uint32: seconds since the playthrough's first midnight.
		rtti::Method gameTime{ "gameTimeSystem", "GetGameTime" };

		// The weather system is GameInstance.GetWeatherSystem(), a static function of ScriptGameInstance
		// (not a global like GetPlayer). None of the game's own scripts call its SetWeather or
		// GetWeatherState, so their parameter lists aren't confirmed: each is found by name and checked
		// against the types it should take before anything calls it, and its real signature is logged
		// once.
		constexpr const char* kWeatherClass = "worldWeatherScriptInterface";

		// The game's own weather states (from its TweakDB). Any other name came from a weather mod.
		constexpr std::array<std::string_view, 9> kVanillaStates = { "24h_weather_sunny", "24h_weather_light_clouds", "24h_weather_cloudy",
			"24h_weather_heavy_clouds", "24h_weather_fog", "24h_weather_rain", "24h_weather_toxic_rain", "24h_weather_pollution",
			"24h_weather_sandstorm" };

		struct Settings
		{
			bool          sync;
			std::string   clear, rain, thunder;  // empty: decided at the time (a mod's state, else the game's)
			float         blend;
			std::uint64_t priority;
			float         rainThreshold;
			bool          keepOnTimeChange;
		};

		const Settings& Config()
		{
			static const Settings settings = [] {
				auto narrow = [](const std::wstring& a_wide) {
					std::string out;
					for (const wchar_t c : a_wide) {
						out += static_cast<char>(c);
					}
					return out;
				};
				Settings s{};
				s.sync = Config::GetBool(L"Weather", L"bSync", true);
				s.clear = narrow(Config::GetString(L"Weather", L"sClear", L"24h_weather_sunny"));
				s.rain = narrow(Config::GetString(L"Weather", L"sRain", L""));
				s.thunder = narrow(Config::GetString(L"Weather", L"sThunder", L""));
				s.blend = std::max(0.0f, Config::GetFloat(L"Weather", L"fBlendSeconds", 10.0f));
				s.priority = static_cast<std::uint64_t>(std::max(0.0f, Config::GetFloat(L"Weather", L"iPriority", 5.0f)));
				s.rainThreshold = Config::GetFloat(L"Weather", L"fRainThreshold", 0.1f);
				s.keepOnTimeChange = Config::GetBool(L"Weather", L"bKeepOnTimeChange", true);
				return s;
			}();
			return settings;
		}

		std::string TypeName(RED4ext::rtti::IType* a_type)
		{
			return a_type ? std::string(a_type->GetName().ToString()) : std::string("void");
		}

		std::string Signature(RED4ext::CBaseFunction* a_fn)
		{
			std::string params;
			for (auto* p : a_fn->params) {
				params += std::format("{}{}", params.empty() ? "" : ", ", TypeName(p->type));
			}
			return std::format("{}({}) -> {}", a_fn->shortName.ToString(), params, a_fn->returnType ? TypeName(a_fn->returnType->type) : "void");
		}

		// A weather system function by name, or null (logged, with the class's real functions).
		RED4ext::CBaseFunction* WeatherFunction(const char* a_name)
		{
			auto*                 cls = RED4ext::CRTTISystem::Get()->GetClass(kWeatherClass);
			const RED4ext::CName  name(a_name);
			for (auto* c = cls; c; c = c->parent) {
				for (auto* fn : c->funcs) {
					if (fn->shortName == name) {
						logger::info("weather: {}::{}", kWeatherClass, Signature(fn));
						return fn;
					}
				}
			}
			logger::warn("weather: {}::{} not found", kWeatherClass, a_name);
			static bool dumped = false;
			if (!dumped) {
				dumped = true;
				rtti::DumpClass(kWeatherClass);
			}
			return nullptr;
		}

		// GameInstance.GetWeatherSystem(): taking the game instance, or nothing.
		RED4ext::CBaseFunction* WeatherSystemGetter()
		{
			static RED4ext::CBaseFunction* fn = [] {
				auto* found = rtti::FindFunction("ScriptGameInstance", "GetWeatherSystem", { "ScriptGameInstance" });
				if (!found) {
					found = rtti::FindFunction("ScriptGameInstance", "GetWeatherSystem", {});
				}
				if (!found) {
					found = rtti::FindGlobalFunction("GetWeatherSystem", { "ScriptGameInstance" });
				}
				if (found && (!found->returnType || !TypeName(found->returnType->type).starts_with("handle:"))) {
					logger::warn("weather: GetWeatherSystem doesn't return a handle");
					found = nullptr;
				}
				if (found) {
					logger::info("weather: GameInstance::{}", Signature(found));
				} else {
					logger::warn("weather: GameInstance.GetWeatherSystem not found; Night City's weather isn't synced");
					rtti::DumpFunctionsMatching("ScriptGameInstance", { "Weather" });
				}
				return found;
			}();
			return fn;
		}

		rtti::Handle<rtti::IScriptable> WeatherSystem()
		{
			rtti::Handle<rtti::IScriptable> system;
			auto                            game = rtti::GameInstance();
			auto*                           get = WeatherSystemGetter();
			if (!game.instance || !get) {
				return system;
			}
			RED4ext::StackArgs_t args;
			if (get->params.Size() == 1) {
				args.emplace_back(nullptr, &game);
			}
			RED4ext::ExecuteFunction(static_cast<void*>(nullptr), get, &system, args);
			return system;
		}

		// The current weather state's name (GetWeatherState() -> worldWeatherState, its `name`).
		std::optional<std::string> StateName(const rtti::Handle<rtti::IScriptable>& a_system)
		{
			static RED4ext::CBaseFunction* fn = [] {
				auto* found = WeatherFunction("GetWeatherState");
				if (found && (found->params.Size() != 0 || !found->returnType || !TypeName(found->returnType->type).starts_with("handle:"))) {
					logger::warn("weather: GetWeatherState doesn't take nothing and return a handle; the weather's name is not read");
					found = nullptr;
				}
				return found;
			}();
			if (!fn) {
				return std::nullopt;
			}
			RED4ext::Handle<RED4ext::ISerializable> state;
			RED4ext::StackArgs_t                    args;
			if (!RED4ext::ExecuteFunction(a_system.GetPtr(), fn, &state, args) || !state) {
				return std::nullopt;
			}
			auto* type = state.GetPtr()->GetType();
			auto* prop = type ? type->GetProperty("name") : nullptr;
			if (!prop || TypeName(prop->type) != "CName") {
				return std::nullopt;
			}
			const char* name = prop->GetValue<RED4ext::CName>(state.GetPtr()).ToString();
			return name ? std::optional<std::string>(name) : std::nullopt;
		}

		// How hard it rains right now (GetRainIntensity() -> Float, which the game's city lights read).
		std::optional<float> RainIntensity(const rtti::Handle<rtti::IScriptable>& a_system)
		{
			static RED4ext::CBaseFunction* fn = [] {
				auto* found = WeatherFunction("GetRainIntensity");
				if (found && (found->params.Size() != 0 || !found->returnType || TypeName(found->returnType->type) != "Float")) {
					logger::warn("weather: GetRainIntensity doesn't return a Float; rain is told by the weather's name alone");
					found = nullptr;
				}
				return found;
			}();
			float                rain = 0.0f;
			RED4ext::StackArgs_t args;
			if (!fn || !RED4ext::ExecuteFunction(a_system.GetPtr(), fn, &rain, args)) {
				return std::nullopt;
			}
			return rain;
		}

		bool IsInteger(const std::string& a_type)
		{
			return a_type == "Uint8" || a_type == "Uint16" || a_type == "Uint32" || a_type == "Uint64" || a_type == "Int8" || a_type == "Int16" ||
			       a_type == "Int32" || a_type == "Int64";
		}

		// SetWeather(weather: CName, blendTime: Float, priority: an integer), as far as it takes them.
		// False when it couldn't be called or the weather system refused the state.
		bool CallSetWeather(const rtti::Handle<rtti::IScriptable>& a_system, const std::string& a_state, float a_blend)
		{
			static RED4ext::CBaseFunction* fn = [] {
				auto* found = WeatherFunction("SetWeather");
				if (!found) {
					return found;
				}
				const auto n = found->params.Size();
				bool       ok = n >= 1 && n <= 3 && TypeName(found->params[0]->type) == "CName";
				ok = ok && (n < 2 || TypeName(found->params[1]->type) == "Float");
				ok = ok && (n < 3 || IsInteger(TypeName(found->params[2]->type)));
				ok = ok && (!found->returnType || found->returnType->type->GetSize() <= 16);
				if (!ok) {
					logger::warn("weather: SetWeather isn't (CName, Float, integer); Minecraft can't change Night City's weather");
					return static_cast<RED4ext::CBaseFunction*>(nullptr);
				}
				return found;
			}();
			if (!fn) {
				return false;
			}
			RED4ext::CName                 state(a_state.c_str());
			float                          blend = a_blend;
			std::uint64_t                  priority = Config().priority;  // read as whatever width it is
			std::array<std::uint64_t, 2>   result{};
			RED4ext::StackArgs_t           args;
			args.emplace_back(nullptr, &state);
			if (fn->params.Size() > 1) {
				args.emplace_back(nullptr, &blend);
			}
			if (fn->params.Size() > 2) {
				args.emplace_back(nullptr, &priority);
			}
			if (!RED4ext::ExecuteFunction(a_system.GetPtr(), fn, fn->returnType ? result.data() : nullptr, args)) {
				return false;
			}
			return !fn->returnType || TypeName(fn->returnType->type) != "Bool" || static_cast<std::uint8_t>(result[0]) != 0;
		}

		std::string Lower(std::string_view a_text)
		{
			std::string out(a_text);
			std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// Clear, rain or thunder from a state's name, and how hard it rains if that's known.
		proto::Weather Classify(std::string_view a_name, std::optional<float> a_rain)
		{
			const auto name = Lower(a_name);
			auto has = [&](std::string_view a_part) { return name.find(a_part) != std::string::npos; };
			// Not a sandstorm or a dust storm: those are dry.
			const bool storm = has("thunder") || has("lightning") || (has("storm") && !has("sand") && !has("dust"));
			const bool rain = has("rain") || (a_rain && *a_rain > Config().rainThreshold);
			if (storm) {
				return proto::kWeatherThunder;
			}
			if (rain) {
				return proto::kWeatherRain;
			}
			return proto::kWeatherClear;
		}

		const char* KindName(proto::Weather a_kind)
		{
			switch (a_kind) {
			case proto::kWeatherClear:
				return "clear";
			case proto::kWeatherRain:
				return "rain";
			case proto::kWeatherThunder:
				return "thunder";
			default:
				return "unknown";
			}
		}

		// Rain and storm states a weather mod added, as Night City switched to them this session.
		std::string learnedRain, learnedThunder;

		// The state Minecraft asked for: it stands for what Minecraft asked (thunder given as plain rain
		// when there's no storm state) while Night City shows it.
		struct Request
		{
			std::string       state;
			proto::Weather    kind;
			bool              shown;
			Clock::time_point at;
		};
		std::optional<Request> request;

		proto::Weather    current = proto::kWeatherUnknown;
		std::string       lastState;
		Clock::time_point nextRead{};

		void LogWeatherMods()
		{
			std::vector<std::string> found;
			std::error_code          ec;
			for (const auto& dir : { Config::GameDir() / L"archive" / L"pc" / L"mod", Config::GameDir() / L"mods" }) {
				for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
					const auto name = entry.path().filename().string();
					if (Lower(name).find("weather") != std::string::npos) {
						found.push_back(name);
					}
				}
			}
			if (found.empty()) {
				logger::info("weather: no weather mod installed; Minecraft gets the game's own weather");
				return;
			}
			std::string list;
			for (const auto& name : found) {
				list += std::format("{}{}", list.empty() ? "" : ", ", name);
			}
			logger::info("weather: weather mods installed: {}. Their weather is what Minecraft follows, and states they add are used for /weather once Night City has shown them", list);
		}
	}

	std::optional<float> Hour()
	{
		auto* time = rtti::System("gameTimeSystem");
		std::uint32_t seconds = 0;
		if (!time || !gameTime.Get() || !gameTime.Call(time, &seconds)) {
			return std::nullopt;
		}
		return float(seconds % 86400u) / 3600.0f;
	}

	proto::Weather Weather()
	{
		if (!Config().sync) {
			return proto::kWeatherUnknown;
		}
		const auto now = Clock::now();
		if (now < nextRead) {
			return current;
		}
		nextRead = now + 500ms;
		static bool modsLogged = false;
		if (!modsLogged) {
			modsLogged = true;
			LogWeatherMods();
		}

		const auto system = WeatherSystem();
		if (!system) {
			current = proto::kWeatherUnknown;
			return current;
		}
		const auto state = StateName(system);
		const auto rain = RainIntensity(system);
		if (!state && !rain) {
			current = proto::kWeatherUnknown;
			return current;
		}
		const std::string name = state.value_or("");
		if (name != lastState) {
			lastState = name;
			const auto kind = Classify(name, std::nullopt);
			const bool modded = !name.empty() && std::find(kVanillaStates.begin(), kVanillaStates.end(), name) == kVanillaStates.end();
			if (modded && kind == proto::kWeatherThunder) {
				learnedThunder = name;
			} else if (modded && kind == proto::kWeatherRain) {
				learnedRain = name;
			}
			logger::info("weather: Night City's weather is {} ({}{})", name.empty() ? "unnamed" : name, KindName(Classify(name, rain)),
				modded ? ", from a weather mod" : "");
		}

		if (request) {
			if (!name.empty() && name == request->state) {
				request->shown = true;
				current = request->kind;
				return current;
			}
			// Night City moved on (its own weather, a quest), or never took the request up.
			if (request->shown || now - request->at > 60s) {
				request.reset();
			} else {
				current = request->kind;
				return current;
			}
		}
		current = Classify(name, rain);
		return current;
	}

	void SetHour(float a_hour)
	{
		static RED4ext::CBaseFunction* set = [] {
			auto* found = rtti::FindFunction("gameTimeSystem", "SetGameTimeBySeconds", { "Int32" });
			if (!found) {
				logger::warn("time: gameTimeSystem::SetGameTimeBySeconds(Int32) not found; Minecraft can't change Night City's clock");
			}
			return found;
		}();
		auto*         time = rtti::System("gameTimeSystem");
		std::uint32_t seconds = 0;
		if (!set || !time || !gameTime.Get() || !gameTime.Call(time, &seconds)) {
			return;
		}
		// Forward to the next time the clock shows that hour, the way waiting does: never back.
		const auto wanted = static_cast<std::uint32_t>(std::clamp(a_hour, 0.0f, 24.0f) * 3600.0f) % 86400u;
		const auto ahead = (wanted + 86400u - seconds % 86400u) % 86400u;
		if (ahead < 60u || ahead > 86400u - 60u) {
			return;  // already there
		}
		// Moving the clock rolls Night City's weather forward with it, often into another state.
		// Minecraft only asked for the time, so the weather showing now (or the one Minecraft asked
		// for) is set again straight after, without a blend.
		const auto        system = Config().sync && Config().keepOnTimeChange ? WeatherSystem() : rtti::Handle<rtti::IScriptable>{};
		std::string       keep;
		proto::Weather    keepKind = proto::kWeatherUnknown;
		if (request) {
			keep = request->state;
			keepKind = request->kind;
		} else if (system) {
			keep = StateName(system).value_or("");
			keepKind = Classify(keep, RainIntensity(system));
		}

		std::int32_t         target = static_cast<std::int32_t>(seconds + ahead);
		RED4ext::StackArgs_t args;
		args.emplace_back(nullptr, &target);
		if (!RED4ext::ExecuteFunction(time, set, nullptr, args)) {
			return;
		}
		logger::info("time: Minecraft set the time to {:02}:{:02}; Night City's clock moved {:.1f} h forward", wanted / 3600u, wanted % 3600u / 60u,
			ahead / 3600.0f);
		if (!system || keep.empty()) {
			return;
		}
		if (!CallSetWeather(system, keep, 0.0f)) {
			logger::warn("weather: keeping {} through the time change failed", keep);
			return;
		}
		request = Request{ keep, keepKind, false, Clock::now() };
		nextRead = {};
		logger::info("weather: kept {} through the time change", keep);
	}

	void SetWeather(proto::Weather a_weather)
	{
		if (!Config().sync) {
			return;
		}
		const auto& cfg = Config();
		const std::string rain = !cfg.rain.empty() ? cfg.rain : !learnedRain.empty() ? learnedRain : std::string("24h_weather_rain");
		std::string state;
		switch (a_weather) {
		case proto::kWeatherClear:
			state = cfg.clear;
			break;
		case proto::kWeatherRain:
			state = rain;
			break;
		case proto::kWeatherThunder:
			// Night City has no thunderstorm of its own: a mod's storm if there is one, else rain.
			state = !cfg.thunder.empty() ? cfg.thunder : !learnedThunder.empty() ? learnedThunder : rain;
			break;
		default:
			return;
		}
		const auto system = WeatherSystem();
		if (!system || state.empty()) {
			return;
		}
		if (!CallSetWeather(system, state, cfg.blend)) {
			logger::warn("weather: setting {} failed (Night City may not have that state)", state);
			return;
		}
		request = Request{ state, a_weather, false, Clock::now() };
		nextRead = {};
		logger::info("weather: Minecraft asked for {}; Night City changes to {} over {:.0f} s", KindName(a_weather), state, cfg.blend);
	}
}
