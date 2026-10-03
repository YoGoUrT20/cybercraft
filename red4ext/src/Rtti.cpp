#include "Rtti.h"

namespace cybercraft::rtti
{
	RED4ext::CBaseFunction* Method::Get()
	{
		if (fn_ || missing_) {
			return fn_;
		}
		auto* rtti = RED4ext::CRTTISystem::Get();
		auto* cls = rtti ? rtti->GetClass(class_) : nullptr;
		fn_ = cls ? cls->GetFunction(name_) : nullptr;
		if (!fn_) {
			missing_ = true;
			logger::warn("rtti: {}::{} not found", class_, name_);
		}
		return fn_;
	}

	RED4ext::CBaseFunction* Global::Get()
	{
		if (fn_ || missing_) {
			return fn_;
		}
		auto* rtti = RED4ext::CRTTISystem::Get();
		fn_ = rtti ? rtti->GetFunction(name_) : nullptr;
		if (!fn_) {
			missing_ = true;
			logger::warn("rtti: global {} not found", name_);
		}
		return fn_;
	}

	namespace
	{
		bool Matches(RED4ext::CBaseFunction* a_fn, RED4ext::CName a_name, std::initializer_list<std::string_view> a_paramTypes, std::string_view a_returnType)
		{
			if (!a_fn || a_fn->shortName != a_name || a_fn->params.Size() != a_paramTypes.size()) {
				return false;
			}
			if (!a_returnType.empty() &&
				(!a_fn->returnType || !a_fn->returnType->type || std::string_view(a_fn->returnType->type->GetName().ToString()) != a_returnType)) {
				return false;
			}
			std::uint32_t i = 0;
			for (const auto type : a_paramTypes) {
				const auto* p = a_fn->params[i++];
				if (!p->type || std::string_view(p->type->GetName().ToString()) != type) {
					return false;
				}
			}
			return true;
		}
	}

	RED4ext::CBaseFunction* FindFunction(const char* a_class, const char* a_name, std::initializer_list<std::string_view> a_paramTypes,
		std::string_view a_returnType)
	{
		auto* rtti = RED4ext::CRTTISystem::Get();
		auto* cls = rtti ? rtti->GetClass(a_class) : nullptr;
		const RED4ext::CName name(a_name);
		auto matches = [&](RED4ext::CBaseFunction* a_fn) { return Matches(a_fn, name, a_paramTypes, a_returnType); };
		for (auto* c = cls; c; c = c->parent) {
			for (auto* fn : c->funcs) {
				if (matches(fn)) {
					return fn;
				}
			}
			for (auto* fn : c->staticFuncs) {
				if (matches(fn)) {
					return fn;
				}
			}
		}
		return nullptr;
	}

	RED4ext::CBaseFunction* FindGlobalFunction(const char* a_name, std::initializer_list<std::string_view> a_paramTypes, std::string_view a_returnType)
	{
		auto* rtti = RED4ext::CRTTISystem::Get();
		if (!rtti) {
			return nullptr;
		}
		const RED4ext::CName    name(a_name);
		RED4ext::CBaseFunction* found = nullptr;
		rtti->funcs.ForEach([&](const RED4ext::CName&, RED4ext::CGlobalFunction*& a_fn) {
			if (!found && Matches(a_fn, name, a_paramTypes, a_returnType)) {
				found = a_fn;
			}
		});
		return found;
	}

	IScriptable* System(const char* a_class)
	{
		auto* engine = RED4ext::CGameEngine::Get();
		if (!engine || !engine->framework || !engine->framework->gameInstance) {
			return nullptr;
		}
		auto* cls = RED4ext::CRTTISystem::Get()->GetClass(a_class);
		return cls ? engine->framework->gameInstance->GetSystem(cls) : nullptr;
	}

	RED4ext::ScriptGameInstance GameInstance()
	{
		return RED4ext::ScriptGameInstance();
	}

	Handle<IScriptable> Player()
	{
		static Global getPlayer("GetPlayer;GameInstance");
		Handle<IScriptable> player;
		auto                game = GameInstance();
		if (!game.instance) {
			return player;
		}
		getPlayer.Call(&player, game);
		return player;
	}

	void DumpClass(const char* a_class)
	{
		auto* rtti = RED4ext::CRTTISystem::Get();
		auto* cls = rtti ? rtti->GetClass(a_class) : nullptr;
		if (!cls) {
			logger::info("dump: no class {}", a_class);
			return;
		}
		auto typeName = [](RED4ext::CBaseRTTIType* a_type) -> std::string {
			return a_type ? std::string(a_type->GetName().ToString()) : std::string("void");
		};
		for (auto* c = cls; c; c = c->parent) {
			logger::info("dump: class {} (size 0x{:X})", c->GetName().ToString(), c->size);
			for (auto* prop : c->props) {
				logger::info("dump:   prop {} : {} @0x{:X}", prop->name.ToString(), typeName(prop->type), prop->valueOffset);
			}
			auto dumpFn = [&](RED4ext::CBaseFunction* a_fn, bool a_static) {
				std::string params;
				for (auto* p : a_fn->params) {
					if (!params.empty()) {
						params += ", ";
					}
					if (p->flags.isOut) {
						params += "out ";
					}
					params += std::format("{}: {}", p->name.ToString(), typeName(p->type));
				}
				logger::info("dump:   {}func {}({}) -> {}   [{}]{}", a_static ? "static " : "", a_fn->shortName.ToString(), params,
					a_fn->returnType ? typeName(a_fn->returnType->type) : std::string("void"), a_fn->fullName.ToString(),
					a_fn->flags.isNative ? " native" : "");
			};
			for (auto* fn : c->staticFuncs) {
				dumpFn(fn, true);
			}
			for (auto* fn : c->funcs) {
				dumpFn(fn, false);
			}
		}
	}

	void DumpFunctionsMatching(const char* a_class, std::initializer_list<std::string_view> a_needles)
	{
		auto* rtti = RED4ext::CRTTISystem::Get();
		if (!rtti) {
			return;
		}
		auto typeName = [](RED4ext::CBaseRTTIType* a_type) -> std::string {
			return a_type ? std::string(a_type->GetName().ToString()) : std::string("void");
		};
		auto log = [&](std::string_view a_owner, RED4ext::CBaseFunction* a_fn, bool a_static) {
			const std::string_view full = a_fn->fullName.ToString() ? a_fn->fullName.ToString() : "";
			if (std::none_of(a_needles.begin(), a_needles.end(), [&](std::string_view a_needle) { return full.find(a_needle) != std::string_view::npos; })) {
				return;
			}
			std::string params;
			for (auto* p : a_fn->params) {
				if (!params.empty()) {
					params += ", ";
				}
				params += std::format("{}{}: {}", p->flags.isOut ? "out " : "", p->name.ToString(), typeName(p->type));
			}
			logger::info("dump: {} {}func {}({}) -> {}   [{}]", a_owner, a_static ? "static " : "", a_fn->shortName.ToString(), params,
				a_fn->returnType ? typeName(a_fn->returnType->type) : std::string("void"), full);
		};
		if (!a_class) {
			rtti->funcs.ForEach([&](const RED4ext::CName&, RED4ext::CGlobalFunction*& a_fn) {
				log("global", a_fn, true);
			});
			return;
		}
		auto* cls = rtti->GetClass(a_class);
		if (!cls) {
			logger::info("dump: no class {}", a_class);
			return;
		}
		for (auto* fn : cls->staticFuncs) {
			log(a_class, fn, true);
		}
		for (auto* fn : cls->funcs) {
			log(a_class, fn, false);
		}
	}
}
