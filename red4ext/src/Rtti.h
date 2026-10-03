#pragma once

#include <RED4ext/Scripting/Natives/ScriptGameInstance.hpp>
#include <RED4ext/Scripting/Natives/Generated/EulerAngles.hpp>
#include <RED4ext/Scripting/Natives/Generated/Quaternion.hpp>
#include <RED4ext/Scripting/Natives/Generated/Vector4.hpp>

// Calling the game's own functions (native or redscript) by name through its RTTI, with the
// function lookups cached. Main thread only, like everything that touches game objects.
namespace cybercraft::rtti
{
	using RED4ext::Handle;
	using RED4ext::IScriptable;

	// A class method or static function, looked up by short name the first time it's used
	// (searching parent classes too).
	class Method
	{
	public:
		constexpr Method(const char* a_class, const char* a_name) :
			class_(a_class), name_(a_name) {}

		RED4ext::CBaseFunction* Get();
		[[nodiscard]] const char* Name() const { return name_; }

		// Calls it on a_self (nullptr for statics and systems' context), with a_out receiving the
		// return value (nullptr if there is none). Arguments are passed by address, so `out`
		// parameters write straight into the caller's variables.
		template <class... Args>
		bool Call(void* a_self, void* a_out, Args&&... a_args)
		{
			auto* fn = Get();
			if (!fn) {
				return false;
			}
			RED4ext::StackArgs_t args;
			((args.emplace_back(nullptr, const_cast<void*>(static_cast<const void*>(&a_args)))), ...);
			return RED4ext::ExecuteFunction(a_self, fn, a_out, args);
		}

	private:
		const char*             class_;
		const char*             name_;
		RED4ext::CBaseFunction* fn_{ nullptr };
		bool                    missing_{ false };
	};

	// A global function, by its full name (e.g. "GetPlayer;GameInstance").
	class Global
	{
	public:
		constexpr explicit Global(const char* a_fullName) :
			name_(a_fullName) {}

		RED4ext::CBaseFunction* Get();

		template <class... Args>
		bool Call(void* a_out, Args&&... a_args)
		{
			auto* fn = Get();
			if (!fn) {
				return false;
			}
			RED4ext::StackArgs_t args;
			((args.emplace_back(nullptr, const_cast<void*>(static_cast<const void*>(&a_args)))), ...);
			return RED4ext::ExecuteFunction(static_cast<void*>(nullptr), fn, a_out, args);
		}

	private:
		const char*             name_;
		RED4ext::CBaseFunction* fn_{ nullptr };
		bool                    missing_{ false };
	};

	// A class function or static function (searching parent classes too) whose parameters are
	// exactly these RTTI type names ("ScriptGameInstance", "whandle:gameObject", ...), so the right
	// overload is called or none at all, and returning a_returnType when one is given. Null when
	// there is no such function.
	RED4ext::CBaseFunction* FindFunction(const char* a_class, const char* a_name, std::initializer_list<std::string_view> a_paramTypes,
		std::string_view a_returnType = {});

	// The same for a global function, by its short name (no ";" and parameter types).
	RED4ext::CBaseFunction* FindGlobalFunction(const char* a_name, std::initializer_list<std::string_view> a_paramTypes, std::string_view a_returnType = {});

	// The game system of a class (gameSpatialQueriesSystem, gameTeleportationFacility, ...), or null
	// outside a game session.
	IScriptable* System(const char* a_class);

	RED4ext::ScriptGameInstance GameInstance();

	// The player puppet, or an empty handle (main menu, loading).
	Handle<IScriptable> Player();

	// Logs a class's properties and functions with their parameter types (reverse engineering aid).
	void DumpClass(const char* a_class);

	// Logs the functions of one class (own ones only), or every global function when a_class is
	// null, whose full name contains any of the needles.
	void DumpFunctionsMatching(const char* a_class, std::initializer_list<std::string_view> a_needles);
}
