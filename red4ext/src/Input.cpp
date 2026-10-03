#include "Input.h"

#include "Config.h"
#include "Game.h"

namespace cybercraft
{
	namespace
	{
		// PS/2 set 1 scan code (what both DirectInput and WM_KEYDOWN report) -> SDL scancode /
		// USB HID usage (what Minecraft uses). DirectInput codes and Windows' scan codes are the
		// same numbers, extended keys included (0x80 | code).
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		// Keys Cyberpunk keeps while Minecraft drives V, whatever Minecraft binds to them. Beyond
		// these, a key is Minecraft's if it has a control on it and Cyberpunk's if not (CyberpunkKey).
		constexpr std::uint32_t kDikEscape = 0x01;  // Cyberpunk pause menu (or closes an MC screen)
		constexpr std::uint32_t kDikGrave = 0x29;   // CET overlay, so the dev console stays reachable
		constexpr std::uint32_t kDikO = 0x18;       // Minecraft pause / options menu (Esc is Cyberpunk's)
		constexpr std::uint32_t kDikF5 = 0x3F;      // Minecraft's camera view, never Cyberpunk's quicksave (F5Locked)
		constexpr std::uint32_t kDikF9 = 0x43;      // Cyberpunk quickload
		constexpr std::uint32_t kDikT = 0x14;       // Cyberpunk's phone (Minecraft's chat stays on / and opens with it)
		constexpr std::uint32_t kDikF = 0x21;       // Cyberpunk interact while it offers one, else Minecraft's swap hands
		constexpr std::uint32_t kDikInsert = 0xD2;  // this plugin: align Minecraft's grid to the ground here
		std::atomic<bool>       alignRequested{ false };

		bool IsGameKey(std::uint32_t a_code)
		{
			return a_code == kDikEscape || a_code == kDikGrave || a_code == kDikF9 || a_code == kDikT;
		}

		// Minecraft has a control on this key (its key mappings and mods', reported in McState).
		// Until it has reported any, every key is Minecraft's, as it was before it could.
		bool MinecraftBinds(std::uint32_t a_dik)
		{
			const auto& keys = State().mcKeys;
			bool        reported = false;
			for (const auto& word : keys) {
				reported |= word.load(std::memory_order_relaxed) != 0;
			}
			if (!reported) {
				return true;
			}
			const auto sdl = kDikToSdl[a_dik & 0xFF];
			return sdl != 0 && ((keys[sdl >> 5].load(std::memory_order_relaxed) >> (sdl & 31)) & 1) != 0;
		}

		// While Minecraft drives V with no Minecraft screen open: Cyberpunk's own keys, F while
		// Cyberpunk offers something to do with it (a door, a car, loot, a dialogue choice), and
		// every key Minecraft has no control on (V calls the car, M the map, J the journal, Z the
		// radio, ...). The rest are Minecraft's.
		bool CyberpunkKey(std::uint32_t a_dik)
		{
			return IsGameKey(a_dik) || (a_dik == kDikF && State().cpInteraction.load()) || !MinecraftBinds(a_dik);
		}

		// Which game a key's press went to, so its auto-repeats and release (and the legacy message
		// Windows sends after the raw one) go the same way, even when the routing changed while it was
		// held: an interaction prompt showing up under F, a menu opening. Window thread only.
		enum class KeyOwner : std::uint8_t
		{
			kUnknown,
			kCyberpunk,
			kMinecraft,
			kPlugin,  // Insert, O: neither game sees them
		};
		std::array<KeyOwner, 256> keyOwner{};
		std::array<bool, 256>     keyHeld{};

		// Esc goes to Cyberpunk, which opens or closes its menu. Whether a menu is open is read from
		// the game every frame (Game.cpp: the time system's paused state); a flag flipped on each
		// Esc fell out of step whenever a menu closed some other way (Resume, a sub-menu's Back) and
		// stayed inverted from then on. Minecraft only lets go of what it holds.
		void OnEscape()
		{
			Input::ReleaseAll();
		}

		std::atomic<float> lookDx{ 0.0f };
		std::atomic<float> lookDy{ 0.0f };
		// What the vertical movement Cyberpunk reads is scaled by, and how much it has read since the
		// last ConsumeNativeDy (after that scale).
		std::atomic<float> pitchScale{ 1.0f };
		std::atomic<float> nativeDy{ 0.0f };
		HWND               window = nullptr;
		WNDPROC            original = nullptr;
		// Raw input has been seen from this device. Windows still sends the legacy messages
		// (WM_KEYDOWN, WM_LBUTTONDOWN, WM_MOUSEWHEEL) for the same press, and forwarding both made
		// every click, scroll and key reach Minecraft twice: two hotbar slots per wheel notch, and
		// inventory drags that fought themselves.
		bool rawKeyboardSeen = false;
		bool rawMouseSeen = false;
		std::atomic<std::uint64_t> gamePresses{ 0 };

		using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
		void*              rawInputTarget = nullptr;
		GetRawInputDataFn  originalGetRawInputData = nullptr;  // the trampoline, once hooked
		RED4ext::v1::PluginHandle pluginHandle = nullptr;
		const RED4ext::v1::Sdk*   sdk = nullptr;

		// True while input belongs to Minecraft rather than Cyberpunk.
		bool Routed()
		{
			auto& st = State();
			return st.puppeting.load() && !st.gameMenuOpen.load();
		}

		// F5 is Minecraft's camera view and Cyberpunk's quicksave. While CyberCraft is on and Minecraft
		// is in its world it never reaches Cyberpunk: Minecraft's when input is, nobody's otherwise.
		// Presses that slipped through made quicksaves in bursts, and those loaded with Cyberpunk's
		// time system stuck paused: every action blocked, and Minecraft hidden as if behind a menu.
		bool F5Locked(std::uint32_t a_dik)
		{
			auto& st = State();
			return (a_dik & 0xFF) == kDikF5 && st.enabled.load() && st.mcInWorld.load();
		}

		// This plugin's own reads go around the hook, so it still sees the buttons.
		UINT ReadRawInput(HRAWINPUT a_input, UINT a_command, LPVOID a_data, PUINT a_size, UINT a_headerSize)
		{
			return originalGetRawInputData ? originalGetRawInputData(a_input, a_command, a_data, a_size, a_headerSize) :
			                                 ::GetRawInputData(a_input, a_command, a_data, a_size, a_headerSize);
		}

		// What Cyberpunk reads of the mouse while Minecraft drives V: vertical movement only, so it
		// tilts its own camera at zero latency. Turning is this plugin's: V is teleported every
		// frame she moves, and that teleport's heading overrode any turn Cyberpunk made in between.
		// The vertical movement is scaled so Cyberpunk tilts as far per count as this plugin turns
		// (Look.cpp measures its own sensitivity), or taken out while Look.cpp holds the camera at
		// Minecraft's pitch itself. The buttons and wheel are Minecraft's (no gunfire
		// when breaking a block), and with a Minecraft screen open all movement is that screen's cursor.
		UINT WINAPI GetRawInputDataHook(HRAWINPUT a_input, UINT a_command, LPVOID a_data, PUINT a_size, UINT a_headerSize)
		{
			const UINT result = originalGetRawInputData(a_input, a_command, a_data, a_size, a_headerSize);
			if (a_command == RID_INPUT && a_data && result != static_cast<UINT>(-1) && result >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) &&
				Routed()) {
				auto* raw = static_cast<RAWINPUT*>(a_data);
				if (raw->header.dwType == RIM_TYPEMOUSE) {
					auto& mouse = raw->data.mouse;
					mouse.usButtonFlags = 0;
					mouse.usButtonData = 0;
					mouse.lLastX = 0;
					if (State().mcScreenOpen.load()) {
						mouse.lLastY = 0;
					} else if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && mouse.lLastY) {
						// Whole counts only: the fraction left over carries to the next message.
						static float carry = 0.0f;
						carry += static_cast<float>(mouse.lLastY) * pitchScale.load(std::memory_order_relaxed);
						const auto whole = static_cast<LONG>(carry);
						carry -= static_cast<float>(whole);
						mouse.lLastY = whole;
						nativeDy.fetch_add(static_cast<float>(whole), std::memory_order_relaxed);
					}
				}
			}
			return result;
		}

		void PushKey(std::uint32_t a_dik, bool a_down)
		{
			if (const auto sdl = kDikToSdl[a_dik & 0xFF]) {
				Link::Get().PushInput(proto::kInKey, sdl, a_down ? 1 : 0);
			}
		}

		void PushButton(std::uint16_t a_sdlButton, bool a_down)
		{
			Link::Get().PushInput(proto::kInMouseButton, a_sdlButton, a_down ? 1 : 0);
		}

		void MoveMouse(int a_dx, int a_dy)
		{
			auto& st = State();
			if (st.mcScreenOpen.load()) {
				const int x = std::clamp(st.cursorX.load() + a_dx, 0, st.viewportW.load() - 1);
				const int y = std::clamp(st.cursorY.load() + a_dy, 0, st.viewportH.load() - 1);
				st.cursorX = x;
				st.cursorY = y;
				Link::Get().PushInput(proto::kInCursor, 0, x, y);
			} else {
				lookDx.fetch_add(static_cast<float>(a_dx), std::memory_order_relaxed);
				lookDy.fetch_add(static_cast<float>(a_dy), std::memory_order_relaxed);
			}
		}

		// Returns true if the message was consumed (Cyberpunk must not see it). Runs even when input
		// belongs to Cyberpunk, because Esc is what hands it back.
		bool HandleRawInput(LPARAM a_lParam)
		{
			UINT bytes = 0;
			if (ReadRawInput(reinterpret_cast<HRAWINPUT>(a_lParam), RID_INPUT, nullptr, &bytes, sizeof(RAWINPUTHEADER)) != 0 || bytes == 0 ||
				bytes > sizeof(RAWINPUT)) {
				return false;
			}
			RAWINPUT raw{};
			if (ReadRawInput(reinterpret_cast<HRAWINPUT>(a_lParam), RID_INPUT, &raw, &bytes, sizeof(RAWINPUTHEADER)) != bytes) {
				return false;
			}

			if (raw.header.dwType == RIM_TYPEMOUSE) {
				rawMouseSeen = true;
				if (!Routed()) {
					constexpr USHORT kDowns = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN;
					if (raw.data.mouse.usButtonFlags & kDowns) {
						gamePresses.fetch_add(1, std::memory_order_relaxed);
					}
					return false;
				}
				const auto& mouse = raw.data.mouse;
				if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (mouse.lLastX || mouse.lLastY)) {
					MoveMouse(mouse.lLastX, mouse.lLastY);
				}
				const auto flags = mouse.usButtonFlags;
				// SDL button numbers: 1 left, 2 middle, 3 right, 4/5 side buttons.
				if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) PushButton(1, true);
				if (flags & RI_MOUSE_LEFT_BUTTON_UP) PushButton(1, false);
				if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) PushButton(2, true);
				if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) PushButton(2, false);
				if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) PushButton(3, true);
				if (flags & RI_MOUSE_RIGHT_BUTTON_UP) PushButton(3, false);
				if (flags & RI_MOUSE_BUTTON_4_DOWN) PushButton(4, true);
				if (flags & RI_MOUSE_BUTTON_4_UP) PushButton(4, false);
				if (flags & RI_MOUSE_BUTTON_5_DOWN) PushButton(5, true);
				if (flags & RI_MOUSE_BUTTON_5_UP) PushButton(5, false);
				if (flags & RI_MOUSE_WHEEL) {
					Link::Get().PushInput(proto::kInScroll, 0, static_cast<std::int16_t>(mouse.usButtonData));
				}
				// With the hook in, Cyberpunk gets the message too and reads only the vertical
				// movement, scaled or none (Look.cpp): it tilts its camera, or Look.cpp holds it. The
				// deltas above still turn V, feed the pitch fallback and move a Minecraft screen's cursor.
				return !originalGetRawInputData || State().mcScreenOpen.load();
			}

			if (raw.header.dwType == RIM_TYPEKEYBOARD) {
				rawKeyboardSeen = true;
				const auto& key = raw.data.keyboard;
				const bool  down = (key.Flags & RI_KEY_BREAK) == 0;
				const auto  dik = static_cast<std::uint32_t>(key.MakeCode) | ((key.Flags & RI_KEY_E0) ? 0x80u : 0u);
				if (dik == 0 || dik > 0xFF) {
					return false;
				}
				if (down && !Routed() && dik != kDikEscape) {
					gamePresses.fetch_add(1, std::memory_order_relaxed);
				}
				// Auto-repeats and the release go where the press went.
				KeyOwner owner = (!down || keyHeld[dik]) ? keyOwner[dik] : KeyOwner::kUnknown;
				if (!down) {
					keyHeld[dik] = false;
				}
				if (owner == KeyOwner::kUnknown) {
					// With a Minecraft screen up (chat, inventory, options) every key is Minecraft's, so
					// typing works and Esc closes the screen.
					if (F5Locked(dik)) {
						owner = Routed() ? KeyOwner::kMinecraft : KeyOwner::kPlugin;
					} else if (State().mcScreenOpen.load()) {
						owner = Routed() ? KeyOwner::kMinecraft : KeyOwner::kCyberpunk;
					} else if (dik == kDikEscape) {
						if (down) {
							OnEscape();
						}
						owner = KeyOwner::kCyberpunk;  // Cyberpunk opens or closes its menu
					} else if (!Routed()) {
						owner = KeyOwner::kCyberpunk;
					} else if (dik == kDikInsert) {
						if (down) {
							alignRequested = true;
						}
						owner = KeyOwner::kPlugin;
					} else if (dik == kDikO) {
						if (down) {
							Input::ReleaseAll();
							Link::Get().PushInput(proto::kInOpenMenu, 0);
						}
						owner = KeyOwner::kPlugin;
					} else {
						owner = CyberpunkKey(dik) ? KeyOwner::kCyberpunk : KeyOwner::kMinecraft;
					}
					if (down) {
						keyHeld[dik] = true;
					}
					keyOwner[dik] = owner;
					if (down && dik == kDikF && Config::Diagnostics()) {
						logger::info("input: F to {} (Cyberpunk offers an interaction: {})",
							owner == KeyOwner::kCyberpunk ? "Cyberpunk" : owner == KeyOwner::kMinecraft ? "Minecraft" : "neither", State().cpInteraction.load());
					}
				}
				if (owner == KeyOwner::kCyberpunk) {
					return false;
				}
				if (owner == KeyOwner::kMinecraft) {
					PushKey(dik, down);
				}
				return true;
			}
			return false;
		}

		LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			const auto pass = [&] { return ::CallWindowProcW(original, a_hwnd, a_msg, a_wParam, a_lParam); };
			if (a_msg == WM_KILLFOCUS) {
				Input::ReleaseAll();
				keyHeld.fill(false);  // their releases may never come: the next press is routed afresh
				return pass();
			}
			// Raw input is how Cyberpunk reads the mouse and keyboard, so swallowing this is what
			// stops V walking off on her own while Minecraft has the controls. It is handled even
			// when input belongs to Cyberpunk, because Esc is what hands it back.
			if (a_msg == WM_INPUT) {
				return HandleRawInput(a_lParam) ? 0 : pass();
			}
			const bool keyMessage = a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN || a_msg == WM_KEYUP || a_msg == WM_SYSKEYUP;
			if (!Routed()) {
				// The legacy message for F5 is kept from Cyberpunk too.
				return keyMessage && F5Locked(static_cast<std::uint32_t>((a_lParam >> 16) & 0xFF)) ? 0 : pass();
			}

			switch (a_msg) {
			// Window-message fallbacks, in case raw input is not where a build gets its input from.
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYUP:
				{
					const bool down = (a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN);
					const auto dik = static_cast<std::uint32_t>((a_lParam >> 16) & 0xFF) | (((a_lParam >> 24) & 1) ? 0x80u : 0u);
					if (rawKeyboardSeen) {
						// Raw input already routed this press: Cyberpunk gets the legacy message only
						// if it got the press. Esc included: raw input already toggled the menu flag
						// for it, and toggling here too flipped it straight back whenever Esc closed
						// the menu, so Minecraft's overlay and controls never came back.
						return keyOwner[dik & 0xFF] == KeyOwner::kCyberpunk ? pass() : 0;
					}
					if (!State().mcScreenOpen.load() && dik != kDikO && !F5Locked(dik) && (dik == kDikEscape || CyberpunkKey(dik))) {
						return pass();
					}
					if (dik == kDikO && !State().mcScreenOpen.load()) {
						if (down && (a_lParam & (1 << 30)) == 0) {
							Input::ReleaseAll();
							Link::Get().PushInput(proto::kInOpenMenu, 0);
						}
						return 0;
					}
					if ((a_lParam & (1 << 30)) == 0 || !down) {  // ignore auto-repeat: Minecraft repeats itself
						PushKey(dik, down);
					}
					return 0;
				}
			case WM_CHAR:
				if (State().mcScreenOpen.load()) {
					Link::Get().PushInput(proto::kInText, 0, static_cast<std::int32_t>(a_wParam));
				}
				return 0;
			// With raw mouse input these only need keeping from Cyberpunk: raw input sent them.
			case WM_LBUTTONDOWN: if (!rawMouseSeen) PushButton(1, true); return 0;
			case WM_LBUTTONUP: if (!rawMouseSeen) PushButton(1, false); return 0;
			case WM_MBUTTONDOWN: if (!rawMouseSeen) PushButton(2, true); return 0;
			case WM_MBUTTONUP: if (!rawMouseSeen) PushButton(2, false); return 0;
			case WM_RBUTTONDOWN: if (!rawMouseSeen) PushButton(3, true); return 0;
			case WM_RBUTTONUP: if (!rawMouseSeen) PushButton(3, false); return 0;
			case WM_XBUTTONDOWN: if (!rawMouseSeen) PushButton(GET_XBUTTON_WPARAM(a_wParam) == XBUTTON1 ? 4 : 5, true); return 0;
			case WM_XBUTTONUP: if (!rawMouseSeen) PushButton(GET_XBUTTON_WPARAM(a_wParam) == XBUTTON1 ? 4 : 5, false); return 0;
			case WM_MOUSEWHEEL:
				if (!rawMouseSeen) {
					Link::Get().PushInput(proto::kInScroll, 0, GET_WHEEL_DELTA_WPARAM(a_wParam));
				}
				return 0;
			default:
				return pass();
			}
		}

		BOOL CALLBACK FindGameWindow(HWND a_hwnd, LPARAM a_lParam)
		{
			DWORD pid = 0;
			::GetWindowThreadProcessId(a_hwnd, &pid);
			if (pid != ::GetCurrentProcessId() || !::IsWindowVisible(a_hwnd) || ::GetWindow(a_hwnd, GW_OWNER)) {
				return TRUE;
			}
			if (::GetWindowTextLengthW(a_hwnd) == 0) {
				return TRUE;  // splash and helper windows have no title
			}
			*reinterpret_cast<HWND*>(a_lParam) = a_hwnd;
			return FALSE;
		}
	}

	namespace Input
	{
		void Install()
		{
			if (window) {
				return;
			}
			HWND found = nullptr;
			::EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
			if (!found) {
				return;  // the window is not up yet: the next frame tries again
			}
			original = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(found, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
			if (!original) {
				logger::error("input: could not subclass the game window (error {})", ::GetLastError());
				return;
			}
			window = found;
			logger::info("input: hooked the game window ({:#x})", reinterpret_cast<std::uintptr_t>(window));
		}

		void Uninstall()
		{
			if (window && original) {
				::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
			}
			window = nullptr;
			original = nullptr;
		}

		void InstallRawInputHook(RED4ext::v1::PluginHandle a_handle, const RED4ext::v1::Sdk* a_sdk)
		{
			if (!Config::GetBool(L"Input", L"bNativeLook", true)) {
				logger::info("input: bNativeLook = 0, this plugin owns the mouse (the view only turns left and right)");
				return;
			}
			auto* target = reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetRawInputData"));
			void* trampoline = nullptr;
			if (!target || !a_sdk || !a_sdk->hooking ||
				!a_sdk->hooking->Attach(a_handle, target, reinterpret_cast<void*>(&GetRawInputDataHook), &trampoline) || !trampoline) {
				logger::warn("input: could not hook GetRawInputData; this plugin owns the mouse (the view only turns left and right)");
				return;
			}
			rawInputTarget = target;
			originalGetRawInputData = reinterpret_cast<GetRawInputDataFn>(trampoline);
			pluginHandle = a_handle;
			sdk = a_sdk;
			logger::info("input: GetRawInputData hooked; Cyberpunk keeps the mouse look, Minecraft gets the buttons");
		}

		void UninstallRawInputHook()
		{
			if (rawInputTarget && sdk && sdk->hooking) {
				sdk->hooking->Detach(pluginHandle, rawInputTarget);
			}
			rawInputTarget = nullptr;
			originalGetRawInputData = nullptr;
		}

		bool NativeLook()
		{
			return originalGetRawInputData != nullptr;
		}

		bool TakeAlignRequest()
		{
			return alignRequested.exchange(false);
		}

		std::uint64_t GamePresses()
		{
			return gamePresses.load(std::memory_order_relaxed);
		}

		void ConsumeLook(float& a_dx, float& a_dy)
		{
			a_dx = lookDx.exchange(0.0f, std::memory_order_relaxed);
			a_dy = lookDy.exchange(0.0f, std::memory_order_relaxed);
		}

		void SetPitchScale(float a_scale)
		{
			pitchScale.store(a_scale, std::memory_order_relaxed);
		}

		float ConsumeNativeDy()
		{
			return nativeDy.exchange(0.0f, std::memory_order_relaxed);
		}

		void ReleaseAll()
		{
			Link::Get().PushInput(proto::kInReleaseAll, 0);
		}
	}
}
