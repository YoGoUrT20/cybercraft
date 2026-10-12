#include "Launcher.h"

#include "Config.h"
#include "Link.h"

#include <ExDisp.h>
#include <ShlDisp.h>
#include <ShlObj.h>
#include <TlHelp32.h>
#include <servprov.h>

#include <fstream>

// Minecraft starts with Cyberpunk. The Fabric mod then waits on its title screen until Cyberpunk's
// world is up, hides its own window and opens its world by itself, and quits again when this
// Cyberpunk closes. What to start comes from CyberCraft.ini; by default the bundled Prism
// Launcher's "CyberCraft" instance (Prism signs in to the player's Microsoft account; the official
// launcher can't be started into a profile from outside). The bundle comes either as
// CyberCraft-Minecraft.zip next to the plugin (a manual install: unpacked here), or already in
// %LOCALAPPDATA%\CyberCraft (Melty puts it there, and signs Prism in before the game starts).
namespace cybercraft::Launcher
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		std::wstring ExpandEnv(const std::wstring& a_path)
		{
			wchar_t     buf[MAX_PATH * 2];
			const DWORD n = ::ExpandEnvironmentStringsW(a_path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
			return n > 0 && n <= std::size(buf) ? std::wstring(buf) : a_path;
		}

		std::string Narrow(const std::wstring& a_wide)
		{
			if (a_wide.empty()) {
				return {};
			}
			const int   n = ::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<std::size_t>(n), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), out.data(), n, nullptr, nullptr);
			return out;
		}

		// Runs a program the way double-clicking it would: started by the desktop's Explorer, not by
		// Cyberpunk. Minecraft then isn't a child of the game (no Steam overlay or game-launcher
		// hooks in it, and nothing waits on it when the game exits).
		bool OpenFromDesktop(const std::wstring& a_file, const std::wstring& a_args, const std::wstring& a_dir, int a_show)
		{
			ComPtr<IShellWindows> windows;
			if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) {
				return false;
			}
			VARIANT location{};
			location.vt = VT_I4;
			location.lVal = CSIDL_DESKTOP;
			VARIANT           empty{};
			long              hwnd = 0;
			ComPtr<IDispatch> desktop;
			if (FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop)) || !desktop) {
				return false;
			}
			ComPtr<IServiceProvider>     services;
			ComPtr<IShellBrowser>        browser;
			ComPtr<IShellView>           view;
			ComPtr<IDispatch>            background;
			ComPtr<IShellFolderViewDual> folderView;
			ComPtr<IDispatch>            application;
			ComPtr<IShellDispatch2>      shell;
			if (FAILED(desktop.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
				FAILED(browser->QueryActiveShellView(&view)) || FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) ||
				FAILED(background.As(&folderView)) || FAILED(folderView->get_Application(&application)) || FAILED(application.As(&shell))) {
				return false;
			}
			BSTR    file = ::SysAllocString(a_file.c_str());
			VARIANT args{}, dir{}, operation{}, show{};
			args.vt = dir.vt = operation.vt = VT_BSTR;
			args.bstrVal = ::SysAllocString(a_args.c_str());
			dir.bstrVal = ::SysAllocString(a_dir.c_str());
			operation.bstrVal = ::SysAllocString(L"open");
			show.vt = VT_I4;
			show.lVal = a_show;
			const HRESULT hr = shell->ShellExecute(file, args, dir, operation, show);
			::SysFreeString(file);
			::VariantClear(&args);
			::VariantClear(&dir);
			::VariantClear(&operation);
			return SUCCEEDED(hr);
		}

		std::filesystem::path FindPrism()
		{
			for (const wchar_t* candidate : { L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe", L"%ProgramFiles%\\PrismLauncher\\prismlauncher.exe" }) {
				std::filesystem::path p = ExpandEnv(candidate);
				if (std::filesystem::exists(p)) {
					return p;
				}
			}
			return {};
		}

		std::atomic<Status> status{ Status::kOff };

		// The Minecraft CyberCraft ships: portable Prism Launcher with a ready "CyberCraft" instance.
		std::filesystem::path Bundle() { return Config::PluginDir() / L"CyberCraft-Minecraft.zip"; }

		std::filesystem::path InstallDir() { return ExpandEnv(L"%LOCALAPPDATA%\\CyberCraft"); }

		// Runs Windows' own tar (bsdtar, which reads zips) in a_dir, without a window; its exit code.
		DWORD Tar(const std::wstring& a_args, const std::filesystem::path& a_dir)
		{
			std::wstring        command = L"\"" + ExpandEnv(L"%SystemRoot%\\System32\\tar.exe") + L"\" " + a_args;
			STARTUPINFOW        si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			DWORD               code = 1;
			if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, a_dir.c_str(), &si, &pi)) {
				::WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
				::GetExitCodeProcess(pi.hProcess, &code);
				::CloseHandle(pi.hThread);
				::CloseHandle(pi.hProcess);
			}
			return code;
		}

		// The release a bundle came with, from its bundle-version.txt ("CyberCraft 0.2.3 (mod ...), ...").
		std::string BundleVersion(const std::filesystem::path& a_file)
		{
			std::string line;
			if (std::ifstream in{ a_file }; in) {
				std::getline(in, line);
			}
			constexpr std::string_view kPrefix = "CyberCraft ";
			if (!line.starts_with(kPrefix)) {
				return {};
			}
			line.erase(0, kPrefix.size());
			return line.substr(0, line.find(' '));
		}

		// Which release CyberCraft-Minecraft.zip next to the plugin is from. An older install can leave
		// one there (Vortex, or Melty's releases before 0.2.3) after Melty has put this release's
		// Minecraft in place, and unpacking it would put the old mod over the new one.
		std::string ZipVersion()
		{
			const auto      check = InstallDir() / ".bundle-check";
			std::error_code ec;
			std::filesystem::create_directories(check, ec);
			Tar(L"-xf \"" + Bundle().wstring() + L"\" bundle-version.txt", check);
			auto version = BundleVersion(check / "bundle-version.txt");
			std::filesystem::remove_all(check, ec);
			return version;
		}

		// Unpacks the bundle to %LOCALAPPDATA%\CyberCraft (outside the game folder) the first time,
		// and again whenever this CyberCraft brings a different one. Prism's own data there (the
		// signed-in account, downloaded Minecraft and Java, the world) is kept; the instance and its
		// mod jars are replaced, so both halves always match.
		std::filesystem::path EnsureBundle()
		{
			const auto      bundle = Bundle();
			const auto      dir = InstallDir();
			const auto      prism = dir / "Prism" / "prismlauncher.exe";
			std::error_code ec;
			const auto      stamp = std::format("{} {}", std::filesystem::file_size(bundle, ec),
					 std::filesystem::last_write_time(bundle, ec).time_since_epoch().count());
			std::string     installed;
			if (std::ifstream in{ dir / "bundle.stamp" }; in) {
				std::getline(in, installed);
			}
			if (installed == stamp && std::filesystem::exists(prism)) {
				return prism;
			}
			logger::info("Minecraft: unpacking CyberCraft's Minecraft to {}", Narrow(dir.wstring()));
			std::filesystem::create_directories(dir, ec);
			for (const auto& entry : std::filesystem::directory_iterator(dir / "Prism" / "instances" / "CyberCraft" / ".minecraft" / "mods", ec)) {
				const auto name = entry.path().filename().string();
				if (name.starts_with("cybercraft-") || name.starts_with("fabric-api-") || name.starts_with("e4mc-")) {
					std::filesystem::remove(entry.path(), ec);
				}
			}
			const auto copy = dir / "bundle.zip";
			if (!std::filesystem::copy_file(bundle, copy, std::filesystem::copy_options::overwrite_existing, ec)) {
				logger::warn("Minecraft: couldn't copy {} ({})", Narrow(bundle.wstring()), ec.message());
				return {};
			}
			const DWORD code = Tar(L"-xf \"" + copy.wstring() + L"\" -C \"" + dir.wstring() + L"\"", dir);
			std::filesystem::remove(copy, ec);
			if (code != 0 || !std::filesystem::exists(prism)) {
				logger::warn("Minecraft: unpacking failed (tar exit code {})", code);
				return {};
			}
			std::ofstream(dir / "bundle.stamp") << stamp;
			return prism;
		}

		// Old releases' mod jars: the bundle's mods.txt names the ones this release brings. An
		// installer that puts the bundle in place (Melty) adds the new jars beside the old ones,
		// and Fabric won't start with two CyberCraft or Fabric API jars.
		void TidyMods(const std::filesystem::path& a_dir)
		{
			std::ifstream list{ a_dir / "mods.txt" };
			if (!list) {
				return;
			}
			std::vector<std::string> keep;
			for (std::string line; std::getline(list, line);) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
					line.pop_back();
				}
				if (!line.empty()) {
					keep.push_back(line);
				}
			}
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(a_dir / "Prism" / "instances" / "CyberCraft" / ".minecraft" / "mods", ec)) {
				const auto name = entry.path().filename().string();
				const bool ours = name.starts_with("cybercraft-") || name.starts_with("fabric-api-") || name.starts_with("e4mc-");
				if (ours && std::ranges::find(keep, name) == keep.end()) {
					logger::info("Minecraft: removing {} (an older release's)", name);
					std::filesystem::remove(entry.path(), ec);
				}
			}
		}

		// Prism's settings are the player's once it has them (an installer may have written some
		// already, Melty signing it in), but CyberCraft needs its defaults (no window left open, Java
		// downloaded by itself): any of those still missing are added.
		void MergeDefaults(const std::filesystem::path& a_dir)
		{
			const auto      defaults = a_dir / "defaults" / "prismlauncher.cfg";
			const auto      cfg = a_dir / "Prism" / "prismlauncher.cfg";
			std::error_code ec;
			if (!std::filesystem::exists(defaults, ec)) {
				return;
			}
			if (!std::filesystem::exists(cfg, ec)) {
				std::filesystem::copy_file(defaults, cfg, ec);
				return;
			}
			wchar_t section[8192]{};
			::GetPrivateProfileSectionW(L"General", section, static_cast<DWORD>(std::size(section)), defaults.c_str());
			for (const wchar_t* pair = section; *pair; pair += std::wcslen(pair) + 1) {
				const std::wstring line = pair;
				const auto         eq = line.find(L'=');
				if (eq == std::wstring::npos) {
					continue;
				}
				const std::wstring key = line.substr(0, eq);
				wchar_t            have[16]{};
				::GetPrivateProfileStringW(L"General", key.c_str(), L"<unset>", have, static_cast<DWORD>(std::size(have)), cfg.c_str());
				if (std::wcscmp(have, L"<unset>") == 0) {
					::WritePrivateProfileStringW(L"General", key.c_str(), line.substr(eq + 1).c_str(), cfg.c_str());
				}
			}
		}

		// Runs a program the way double-clicking it would (see OpenFromDesktop), or directly if
		// there's no desktop shell to ask. Batch files go through cmd, without a console window.
		bool Start(const std::filesystem::path& a_program, const std::wstring& a_args)
		{
			const auto         ext = a_program.extension().wstring();
			const bool         script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
			const std::wstring dir = a_program.parent_path().wstring();
			const std::string  shown = Narrow(a_program.wstring() + L" " + a_args);
			const HRESULT      com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool         viaDesktop = OpenFromDesktop(a_program.wstring(), a_args, dir, script ? SW_HIDE : SW_SHOWNORMAL);
			if (SUCCEEDED(com)) {
				::CoUninitialize();
			}
			if (viaDesktop) {
				logger::info("Minecraft: started {}", shown);
				return true;
			}
			std::wstring command = script ? L"cmd.exe /c \"\"" + a_program.wstring() + L"\" " + a_args + L"\"" : L"\"" + a_program.wstring() + L"\" " + a_args;
			STARTUPINFOW        si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, script ? CREATE_NO_WINDOW : 0, nullptr, dir.c_str(), &si, &pi)) {
				logger::warn("Minecraft: couldn't start {} (error {})", shown, ::GetLastError());
				return false;
			}
			::CloseHandle(pi.hThread);
			::CloseHandle(pi.hProcess);
			logger::info("Minecraft: started {} (directly)", shown);
			return true;
		}
	}

	Status GetStatus() { return status.load(); }

	bool MinecraftRunning()
	{
		HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, proto::kMinecraftMutexName);
		if (mutex) {
			::CloseHandle(mutex);
			return true;
		}
		return false;
	}

	bool PrismRunning()
	{
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return false;
		}
		PROCESSENTRY32W entry{ sizeof(entry) };
		bool            found = false;
		for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry)) {
			found = _wcsicmp(entry.szExeFile, L"prismlauncher.exe") == 0;
		}
		::CloseHandle(snapshot);
		return found;
	}

	void StartMinecraft()
	{
		if (!Config::GetBool(L"Minecraft", L"bStartWithCyberpunk", true)) {
			logger::info("Minecraft: not started with Cyberpunk (bStartWithCyberpunk = 0)");
			return;
		}
		if (MinecraftRunning()) {
			logger::info("Minecraft: already running");
			status = Status::kRunning;
			// It may be the last game's Minecraft on its way out (it quits a few seconds after that
			// game closes): if it goes in the next minute, start one for this game.
			std::thread([] {
				for (int i = 0; i < 60; ++i) {
					std::this_thread::sleep_for(1s);
					if (!MinecraftRunning()) {
						logger::info("Minecraft: the one that was running has quit; starting another");
						std::this_thread::sleep_for(3s);
						StartMinecraft();
						return;
					}
				}
			}).detach();
			return;
		}
		const std::filesystem::path chosen = ExpandEnv(Config::GetString(L"Minecraft", L"sLauncher", L""));
		const std::wstring          args = Config::GetString(L"Minecraft", L"sArguments", L"--launch CyberCraft");
		const bool                  bundled = chosen.empty() && std::filesystem::exists(Bundle());
		const auto                  placed = InstallDir() / "Prism" / "prismlauncher.exe";
		const bool                  inPlace = chosen.empty() && !bundled && std::filesystem::exists(placed);
		const std::filesystem::path installed = chosen.empty() && !bundled && !inPlace ? FindPrism() : std::filesystem::path{};
		if (chosen.empty() && !bundled && !inPlace && installed.empty()) {
			logger::warn("Minecraft: not started: no CyberCraft-Minecraft.zip next to the plugin, no bundled Minecraft in %LOCALAPPDATA%\\CyberCraft and no Prism Launcher where its installer puts it; set sLauncher in CyberCraft.ini");
			status = Status::kNoLauncher;
			return;
		}
		if (!chosen.empty() && !std::filesystem::exists(chosen)) {
			logger::warn("Minecraft: not started: {} doesn't exist (sLauncher in CyberCraft.ini)", Narrow(chosen.wstring()));
			status = Status::kNoLauncher;
			return;
		}
		status = Status::kStarting;
		// Off the main thread: unpacking and talking to Explorer take a moment.
		std::thread([chosen, installed, bundled, inPlace, placed, args] {
			std::filesystem::path program = !chosen.empty() ? chosen : inPlace ? placed : installed;
			if (bundled || inPlace) {
				if (bundled) {
					const auto version = ZipVersion();
					if (version == CYBERCRAFT_VERSION || !std::filesystem::exists(placed)) {
						program = EnsureBundle();
					} else {
						logger::info("Minecraft: CyberCraft-Minecraft.zip next to the plugin is {}'s, not CyberCraft {}'s: left alone, using the Minecraft already in {}",
							version.empty() ? "an old release" : "CyberCraft " + version, CYBERCRAFT_VERSION, Narrow(InstallDir().wstring()));
						program = placed;
					}
				}
				if (program.empty()) {
					status = Status::kFailed;
					return;
				}
				TidyMods(InstallDir());
				MergeDefaults(InstallDir());
				if (!std::filesystem::exists(program.parent_path() / "accounts.json")) {
					status = Status::kSignIn;  // first time: Prism asks for the Microsoft account
				}
			}
			if (!Start(program, args)) {
				status = Status::kFailed;
			}
		}).detach();
	}
}
