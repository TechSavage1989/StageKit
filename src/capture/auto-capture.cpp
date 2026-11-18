/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Auto Capture: a filter for a Game Capture (or Window Capture) source that
// aims the capture by itself.
//
// About once a second it looks at the window in front. If that window is a
// game, the source is aimed at it. A game capture aimed at a window captures
// it whether the game is windowed or full-screen, so one source covers both.
// Once aimed, it stays on that game until the game closes or another game
// comes to the front. With nothing to aim at, a Game Capture is switched
// back to "capture any full-screen application".
//
// "A game" is judged from what the program has loaded: a graphics engine
// (Direct3D, OpenGL or Vulkan) together with game-controller support, or a
// graphics engine in a window that fills its screen. Programs on the "always
// capture" list count regardless; programs on the "never capture" list, and
// a built-in list of everyday programs, never count.
//
// Finding windows uses the Windows API. On other systems the filter loads
// and does nothing.

// windows.h goes first, with NOMINMAX, so its min/max macros never reach
// the standard library headers below.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <map>
#endif

#include "auto-capture.hpp"
#include "app-capture.hpp"
#include "standby.hpp"
#include "../api/vendor-api.hpp"
#include "../common/scene-util.hpp"
#include "../common/easing.hpp"
#include "../common/timing.hpp"

#include "../common/item-transition.hpp"
#include "../common/pick-list.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace stagekit {

namespace {

enum class Mode : int {
	AnyGame = 0,    // work out what is a game
	ListedOnly = 1, // only the "always capture" list
};

struct Window {
	std::string title, window_class, executable;
	bool foreground = false;
	bool looks_like_game = false; // only worked out for the window in front
	uint64_t handle = 0;          // which window it is, while it is open
};

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

// Everyday programs that draw with a graphics engine but are not games.
const char *const kNeverCapture[] = {
	"obs64.exe",
	"obs32.exe",
	"explorer.exe",
	"dwm.exe",
	"steam.exe",
	"steamwebhelper",
	"discord",
	"chrome.exe",
	"msedge.exe",
	"firefox.exe",
	"opera",
	"brave.exe",
	"spotify.exe",
	"epicgameslauncher",
	"galaxyclient",
	"battle.net",
	"streamdeck",
	"applicationframehost",
	"searchhost",
	"textinputhost",
	"systemsettings",
	"shellexperiencehost",
	"startmenuexperiencehost",
	"lockapp",
	"code.exe",
	"devenv.exe",
	"teams",
	"slack.exe",
	"zoom.exe",
};

#ifdef _WIN32

std::string utf8(const wchar_t *text, int length)
{
	if (length <= 0)
		return {};
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
	std::string out(size > 0 ? (size_t)size : 0, '\0');
	if (size > 0)
		WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
	return out;
}

// Whether a program has a graphics engine and controller support loaded.
// Returns false when the program cannot be looked into (it runs with more
// rights than OBS, or is protected by anti-cheat).
// Which engines a program has loaded. This reads the program's module list
// from outside. It must not use a toolhelp module snapshot: for another
// process that works by starting a thread inside it, which can upset or crash
// a game, and this is asked about whatever program is in front.
bool read_engines(DWORD process_id, bool &graphics, bool &controller)
{
	graphics = controller = false;
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, process_id);
	if (!process)
		return false;
	HMODULE modules[1024];
	DWORD needed = 0;
	const BOOL listed = EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL);
	if (listed) {
		const DWORD listed_count = (DWORD)(needed / sizeof(HMODULE));
		const DWORD count = listed_count < 1024 ? listed_count : 1024;
		for (DWORD i = 0; i < count; i++) {
			wchar_t base[MAX_PATH];
			const DWORD length = GetModuleBaseNameW(process, modules[i], base, MAX_PATH);
			if (!length)
				continue;
			const std::string name = lower(utf8(base, (int)length));
			if (name == "d3d9.dll" || name == "d3d10.dll" || name == "d3d11.dll" || name == "d3d12.dll" ||
			    name == "opengl32.dll" || name == "vulkan-1.dll")
				graphics = true;
			if (name.rfind("xinput", 0) == 0 || name == "dinput8.dll" || name == "dinput.dll" ||
			    name == "sdl2.dll" || name == "sdl3.dll" || name == "gameinput.dll" ||
			    name == "windows.gaming.input.dll")
				controller = true;
			// What games ship with and everyday programs do not: a store's
			// game library, a game engine's player, game audio and video.
			// It counts the same as controller support.
			if (name == "steam_api64.dll" || name == "steam_api.dll" ||
			    name == "gameoverlayrenderer64.dll" || name == "gameoverlayrenderer.dll" ||
			    name == "unityplayer.dll" || name == "eossdk-win64-shipping.dll" ||
			    name == "galaxy64.dll" || name == "galaxy.dll" || name == "fmod.dll" ||
			    name == "fmodstudio.dll" || name == "bink2w64.dll" || name == "nvngx_dlss.dll")
				controller = true;
		}
	}
	CloseHandle(process);
	return listed != FALSE;
}

// A program is looked into a handful of times while it starts up (engines
// load late), then left alone: the answer is remembered for as long as that
// process lives. The first looks are a second apart, so a game started in a
// window is recognised soon after its engines are in; later ones are spaced
// out for programs that load them late.
bool loaded_engines(DWORD process_id, bool &graphics, bool &controller)
{
	struct Known {
		bool readable = false, graphics = false, controller = false;
		int looks = 0;
		ULONGLONG last_look = 0;
	};
	static std::map<DWORD, Known> known;
	constexpr int kLooks = 12;
	const ULONGLONG now = GetTickCount64();
	if (known.size() > 256)
		known.clear();
	Known &k = known[process_id];
	const bool settled = k.readable && k.graphics && k.controller;
	const ULONGLONG apart = k.looks < 5 ? 1000ULL : 3000ULL; // milliseconds
	if (!settled && k.looks < kLooks && (k.looks == 0 || now - k.last_look >= apart)) {
		k.looks++;
		k.last_look = now;
		bool g = false, c = false;
		if (read_engines(process_id, g, c)) {
			k.readable = true;
			k.graphics = k.graphics || g;
			k.controller = k.controller || c;
		}
	}
	graphics = k.graphics;
	controller = k.controller;
	return k.readable;
}

// Window classes that game engines give their windows.
bool engine_window(const std::string &window_class)
{
	for (const char *known :
	     {"UnityWndClass", "UnrealWindow", "SDL_app", "GLFW30", "Valve001", "YYGameMakerYY", "grcWindow"})
		if (window_class == known)
			return true;
	return false;
}

bool fills_its_screen(HWND window)
{
	RECT rect;
	MONITORINFO info;
	info.cbSize = sizeof(info);
	HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (!GetWindowRect(window, &rect) || !GetMonitorInfoW(monitor, &info))
		return false;
	return rect.left <= info.rcMonitor.left && rect.top <= info.rcMonitor.top &&
	       rect.right >= info.rcMonitor.right && rect.bottom >= info.rcMonitor.bottom;
}

BOOL CALLBACK collect_window(HWND window, LPARAM param)
{
	auto *windows = reinterpret_cast<std::vector<Window> *>(param);
	// Only real, visible, top-level application windows.
	if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER) != nullptr)
		return TRUE;
	if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		return TRUE;

	wchar_t title[512];
	const int title_length = GetWindowTextW(window, title, 512);
	if (title_length <= 0)
		return TRUE;

	DWORD process_id = 0;
	GetWindowThreadProcessId(window, &process_id);
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
	if (!process)
		return TRUE;
	wchar_t path[MAX_PATH];
	DWORD path_length = MAX_PATH;
	const BOOL have_path = QueryFullProcessImageNameW(process, 0, path, &path_length);
	CloseHandle(process);
	if (!have_path)
		return TRUE;
	const wchar_t *name = path;
	for (DWORD i = 0; i < path_length; i++)
		if (path[i] == L'\\' || path[i] == L'/')
			name = path + i + 1;

	wchar_t window_class[256];
	const int class_length = GetClassNameW(window, window_class, 256);

	Window found;
	found.title = utf8(title, title_length);
	found.window_class = utf8(window_class, class_length);
	found.executable = utf8(name, (int)(path + path_length - name));
	found.foreground = window == GetForegroundWindow();
	found.handle = (uint64_t)(uintptr_t)window;
	if (found.foreground) {
		// Looking inside a program is the costly part, so it is only done
		// for the one window in front.
		RECT client;
		const bool big_enough = GetClientRect(window, &client) && client.right >= 320 && client.bottom >= 200;
		bool graphics = false, controller = false;
		const bool readable = loaded_engines(process_id, graphics, controller);
		// A program that cannot be looked into and fills its screen is
		// most likely a protected game.
		const bool game_like = controller || engine_window(found.window_class) || fills_its_screen(window);
		found.looks_like_game = big_enough && (readable ? graphics && game_like : fills_its_screen(window));
	}
	windows->push_back(found);
	return TRUE;
}

std::vector<Window> list_windows()
{
	// One look at a time: the thread that keeps the list and a properties
	// window asking for itself share what is remembered about programs.
	static std::mutex one_at_a_time;
	std::lock_guard lock(one_at_a_time);
	std::vector<Window> windows;
	EnumWindows(collect_window, reinterpret_cast<LPARAM>(&windows));
	return windows;
}

#else

std::vector<Window> list_windows()
{
	return {};
}

#endif

// Looking at every open window (and inside the program in front) takes
// anything from a few milliseconds to a third of a second, and the filters
// that need the list tick on OBS's drawing thread: done there four times a
// second, it held frames up (the owner's log, beta76: ticks of 20 to 360 ms).
// So one thread of its own keeps the list, and a tick only takes a copy.
// The thread looks while somebody has asked in the last few seconds.
class WindowWatch {
public:
	~WindowWatch() { stop(); }

	std::vector<Window> latest()
	{
		asked_at_ = os_gettime_ns();
		std::unique_lock lock(mutex_);
		if (stopped_)
			return {};
		if (!thread_.joinable()) {
			// The first time: one look here and now, so nobody starts
			// from an empty list, then the thread takes over.
			lock.unlock();
			std::vector<Window> first = list_windows();
			lock.lock();
			if (!thread_.joinable() && !stopped_) {
				windows_ = std::move(first);
				thread_ = std::thread([this] { run(); });
			}
		}
		return windows_;
	}

	void stop()
	{
		{
			std::lock_guard lock(mutex_);
			stopped_ = true;
		}
		wake_.notify_all();
		if (thread_.joinable())
			thread_.join();
	}

private:
	void run()
	{
		os_set_thread_name("stagekit: open windows");
		std::unique_lock lock(mutex_);
		while (!stopped_) {
			wake_.wait_for(lock, std::chrono::milliseconds(250));
			if (stopped_)
				break;
			if (os_gettime_ns() - asked_at_.load() > 3000000000ULL)
				continue; // nobody is asking
			lock.unlock();
			std::vector<Window> now = list_windows();
			lock.lock();
			windows_ = std::move(now);
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::thread thread_;
	std::vector<Window> windows_;
	std::atomic<uint64_t> asked_at_{0};
	bool stopped_ = false;
};
WindowWatch g_window_watch;

// OBS writes a window as "title:class:executable", with ':' and '#' in
// each part escaped.
std::string encode_part(const std::string &text)
{
	std::string out;
	for (char c : text) {
		if (c == '#')
			out += "#22";
		else if (c == ':')
			out += "#3A";
		else
			out += c;
	}
	return out;
}

// A list of program names. 0.1.0-beta8 stored these as arrays; they are
// still read, and the one-line text is what the controls edit now.
std::vector<std::string> read_list(obs_data_t *settings, const char *name, const char *text_name)
{
	std::vector<std::string> out;
	obs_data_array_t *list = obs_data_get_array(settings, name);
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const std::string value = lower(obs_data_get_string(entry, "value"));
		if (!value.empty())
			out.push_back(value);
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	// The list as typed in one line: "blender, vlc".
	std::string word;
	auto flush = [&] {
		const size_t first = word.find_first_not_of(" \t");
		const size_t last = word.find_last_not_of(" \t");
		if (first != std::string::npos)
			out.push_back(lower(word.substr(first, last - first + 1)));
		word.clear();
	};
	for (const char *c = obs_data_get_string(settings, text_name); *c; c++) {
		if (*c == ',' || *c == ';' || *c == '\n')
			flush();
		else
			word += *c;
	}
	flush();
	return out;
}

bool built_in_never(const std::string &executable)
{
	for (const char *name : kNeverCapture)
		if (executable.find(name) != std::string::npos)
			return true;
	return false;
}

bool matches(const std::string &executable, const std::vector<std::string> &list)
{
	return std::any_of(list.begin(), list.end(),
			   [&](const std::string &part) { return executable.find(part) != std::string::npos; });
}

class AutoCapture {
public:
	AutoCapture(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		// For the tests: stand-in windows, one per line,
		// "title|class|exe|front|game" (the last two may be left empty).
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_test_windows(in string windows)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoCapture *>(data);
				const char *text = calldata_string(cd, "windows");
				std::vector<Window> windows;
				std::string line;
				auto flush = [&] {
					if (line.empty())
						return;
					Window w;
					std::vector<std::string> fields(1);
					for (char c : line) {
						if (c == '|')
							fields.emplace_back();
						else
							fields.back() += c;
					}
					fields.resize(5);
					w.title = fields[0];
					w.window_class = fields[1];
					w.executable = fields[2];
					w.foreground = fields[3] == "front";
					w.looks_like_game = fields[4] == "game";
					windows.push_back(w);
					line.clear();
				};
				for (const char *p = text ? text : ""; *p; p++) {
					if (*p == '\n')
						flush();
					else
						line += *p;
				}
				flush();
				std::lock_guard lock(self->mutex_);
				self->test_windows_ = windows;
				self->use_test_windows_ = true;
				self->timer_ = 0.0;
			},
			this);
		// For the tests, which cannot press a hotkey.
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void hide()",
			[](void *data, calldata_t *) { static_cast<AutoCapture *>(data)->ask_hide(); }, this);
	}

	~AutoCapture()
	{
		vendor_switch_forget(source_);
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
	}

	// Hide or show the capture: its Standby Picture over it, or its items.
	void ask_hide()
	{
		cmd_hide_ = true;
		std::lock_guard lock(mutex_);
		timer_ = 0.0;
	}

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> always = read_list(settings, "programs", "always");
		const std::vector<std::string> never = read_list(settings, "exclude", "never");
		std::lock_guard lock(mutex_);
		mode_ = static_cast<Mode>(obs_data_get_int(settings, "mode"));
		always_ = always;
		never_ = never;
		fallback_fullscreen_ = obs_data_get_bool(settings, "fallback_fullscreen");
		keep_ready_ = obs_data_get_bool(settings, "keep_ready");
		no_game_standby_ = obs_data_get_bool(settings, "no_game_standby");
		hide_standby_ = obs_data_get_bool(settings, "hide_standby");
		cover_style_ = (int)std::clamp<long long>(obs_data_get_int(settings, "standby_style"), 0, 11);
		cover_ms_ = (int)std::clamp<long long>(obs_data_get_int(settings, "standby_ms"), 0, 10000);
		cover_curve_ = (int)obs_data_get_int(settings, "standby_ease_curve");
		cover_mode_ = (int)obs_data_get_int(settings, "standby_ease_mode");
		// For the tests: giving up sooner, and at all.
		if (obs_data_has_user_value(settings, "give_up_ms")) {
			give_up_s_ = std::max(0.25, obs_data_get_int(settings, "give_up_ms") / 1000.0);
			give_up_in_tests_ = true;
		}
		if (!known_loaded_) {
			known_loaded_ = true;
			for (const std::string &exe : read_list(settings, "known_games_list", "known_games"))
				known_games_.insert(lower(exe));
		}
		timer_ = 0.0;
	}

	void tick(float seconds)
	{
		// "Keep capturing in other scenes": OBS stops a capture a moment
		// after the last scene showing it is left and starts it again on
		// the way back, which the owner's log showed at every scene switch.
		// Held as shown, it carries on and is there at once.
		const bool hold = parent_ && obs_source_enabled(source_) && keep_ready_.load();
		if (hold != holding_) {
			if (hold)
				obs_source_inc_showing(parent_);
			else if (parent_)
				obs_source_dec_showing(parent_);
			holding_ = hold;
		}
		if (!parent_ || !obs_source_enabled(source_)) {
			// Switched off: a picture it put over the capture goes.
			if (covered_ && parent_) {
				std::lock_guard lock(mutex_);
				standby_cover_set(parent_, false, cover_style_, cover_ms_, cover_curve_, cover_mode_);
			}
			covered_ = hidden_ = false;
			return;
		}
		Mode mode;
		std::vector<std::string> always, never;
		std::vector<Window> windows;
		bool fallback, use_test;
		{
			std::lock_guard lock(mutex_);
			timer_ -= seconds;
			if (timer_ > 0.0)
				return;
			timer_ = 0.25;
			clock_s_ += 0.25;
			mode = mode_;
			always = always_;
			never = never_;
			fallback = fallback_fullscreen_;
			use_test = use_test_windows_;
			if (use_test)
				windows = test_windows_;
		}
		if (!use_test)
			windows = g_window_watch.latest();

		auto key_of = [](const Window &w) {
			return w.window_class + ":" + w.executable;
		};

		const char *parent_id = obs_source_get_unversioned_id(parent_);
		const bool game_capture = parent_id && strcmp(parent_id, "game_capture") == 0;

		// 1. A game in front wins.  2. Otherwise stay on the game already
		// being captured while its window exists.  3. Otherwise any program
		// from the "always capture" list that is open.
		// A program is only looked into while it is in front. Once it has
		// been seen to be a game it is remembered, so the next time it is
		// picked up at once, in a window, in front or not.
		const Window *in_front = nullptr, *current = nullptr, *listed_open = nullptr, *known_open = nullptr;
		bool learned_something = false;
		// (A program that has closed is no longer being rested from.)
		for (auto it = rest_.begin(); it != rest_.end();) {
			const bool open = std::any_of(windows.begin(), windows.end(), [&](const Window &w) {
				return lower(w.executable) == it->first;
			});
			it = open ? std::next(it) : rest_.erase(it);
		}
		for (const Window &w : windows) {
			const std::string exe = lower(w.executable);
			// Either list can name a program or part of a window's title:
			// two builds of one program share a file name.
			const std::string title = lower(w.title);
			const bool listed = matches(exe, always) || matches(title, always);
			// (Nor a program an Automatic Window Switcher is for: that is a
			// window to capture as a window, not a game to hook.)
			if (!listed && (matches(exe, never) || matches(title, never) || built_in_never(exe) ||
					app_capture_lists(exe, title)))
				continue;
			// A program the capture got no picture from is left alone for a
			// while, not for good: an emulator with no game running gives
			// none either. It is tried again when its window's title changes
			// (a game was started) and, failing that, after a wait that
			// grows each time.
			if (auto rest = rest_.find(exe); !listed && rest != rest_.end() && rest->second.resting) {
				const bool retitled = w.title != rest->second.title && rest->second.title_tries < 3;
				// (Or it has been clicked into again: what one does to start
				// a game in an emulator whose title stays as it is.)
				rest->second.away = rest->second.away || !w.foreground;
				const bool came_back = w.foreground && rest->second.away;
				if (!retitled && !came_back && clock_s_ - rest->second.since < rest->second.wait)
					continue;
				rest->second.resting = false;
				if (retitled)
					rest->second.title_tries++;
				obs_log(LOG_INFO, "Auto Capture: trying %s again (%s)", w.executable.c_str(),
					retitled    ? "its window's title changed"
					: came_back ? "it is in front again"
						    : "after a wait");
			}
			if (mode == Mode::AnyGame && w.looks_like_game && known_games_.insert(exe).second) {
				learned_something = true;
				// For the log: from here on this program is picked up at once.
				obs_log(LOG_INFO, "Auto Capture: %s is recognised as a game and remembered",
					exe.c_str());
			}
			const bool known = mode == Mode::AnyGame && known_games_.count(exe) > 0;
			const bool candidate = listed || known;
			if (w.foreground && candidate)
				in_front = &w;
			if (!applied_key_.empty() && key_of(w) == applied_key_)
				current = &w;
			if (listed && !listed_open)
				listed_open = &w;
			if (known && !known_open)
				known_open = &w;
		}
		const Window *chosen = in_front ? in_front : current ? current : listed_open ? listed_open : known_open;
		if (learned_something) {
			std::string text;
			for (const std::string &exe : known_games_)
				text += (text.empty() ? "" : ", ") + exe;
			obs_data_t *own = obs_source_get_settings(source_);
			obs_data_set_string(own, "known_games", text.c_str());
			obs_data_release(own);
		}

		standby_step(chosen != nullptr);

		// Emulators put the frame rate in their title, so what counts as
		// "a different window" is the program and window class only.
		const std::string key = chosen ? key_of(*chosen) : std::string();
		{
			std::lock_guard lock(mutex_);
			current_ = chosen ? chosen->executable : std::string();
		}
		if (aimed_once_ && key == applied_key_) {
			// A program taken for a game that the capture cannot hook: OBS
			// goes on trying several times a second for as long as the
			// capture is aimed at it, and every try stalls the picture. The
			// owner's 3D modelling program was tried 3,404 times in twelve
			// minutes, and one frame in fourteen ran late. After a while
			// without a picture the program is left alone for a time. (It
			// went on the "never" list for good up to beta134; the owner's
			// Dolphin, open with no game running, was put there again each
			// time they took it off.)
			const bool mine = chosen && game_capture && !matches(lower(chosen->executable), always) &&
					  !matches(lower(chosen->title), always);
			const std::string exe = chosen ? lower(chosen->executable) : std::string();
			// (As long each time: the owner's Ryujinx gave its first picture
			// seven seconds after being aimed at, and tried again for five
			// it was given up on three times running.)
			const double patience = give_up_s_;
			if (!mine || obs_source_get_base_width(parent_) > 0) {
				unhooked_s_ = 0.0;
				if (chosen && obs_source_get_base_width(parent_) > 0)
					rest_.erase(exe); // it can be hooked after all
			} else if ((!use_test || give_up_in_tests_) && (unhooked_s_ += 0.25) >= patience) {
				unhooked_s_ = 0.0;
				Rest &rest = rest_[exe];
				rest.title = chosen->title;
				rest.since = clock_s_;
				// (Twice the patience, then half as long again each time up
				// to a minute: an emulator with no game running gives no
				// picture, and one whose title never changes is only found
				// again by trying. Up to beta136 the wait grew to five
				// minutes.)
				rest.wait = rest.wait > 0.0 ? std::min(rest.wait * 1.5, 60.0) : give_up_s_ * 2.0;
				rest.resting = true;
				rest.away = false;
				obs_log(LOG_INFO,
					"Auto Capture: '%s' got no picture from %s in %.0f s; leaving it alone until its window's title changes, or for %.0f s",
					obs_source_get_name(parent_), chosen->executable.c_str(), patience, rest.wait);
			}
			return;
		}
		aimed_once_ = true;
		unhooked_s_ = 0.0;

		obs_data_t *settings = obs_source_get_settings(parent_);
		bool changed = false;
		if (chosen) {
			const std::string window = encode_part(chosen->title) + ":" +
						   encode_part(chosen->window_class) + ":" +
						   encode_part(chosen->executable);
			obs_data_set_string(settings, "window", window.c_str());
			obs_data_set_int(settings, "priority", 2); // match by executable
			if (game_capture)
				obs_data_set_string(settings, "capture_mode", "window");
			obs_log(LOG_INFO, "Auto Capture: '%s' now follows %s", obs_source_get_name(parent_),
				chosen->executable.c_str());
			changed = true;
		} else if (game_capture && fallback) {
			obs_data_set_string(settings, "capture_mode", "any_fullscreen");
			obs_log(LOG_INFO, "Auto Capture: '%s' back to any full-screen application",
				obs_source_get_name(parent_));
			changed = true;
		}
		if (changed)
			obs_source_update(parent_, settings);
		obs_data_release(settings);
		applied_key_ = key;
	}

	void attach(obs_source_t *parent)
	{
		parent_ = parent;
		// "Hide or show the capture", under the capture in OBS's hotkeys.
		// (The same beginning as the window switchers' own, so the Stream
		// Deck's Manual Window / Source key presses it too.)
		if (hotkey_ == OBS_INVALID_HOTKEY_ID && parent) {
			const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
			std::string text = obs_module_text("Hotkey.AppCapture.Hide");
			const size_t at = text.find("%1");
			if (at != std::string::npos)
				text.replace(at, 2, name);
			hotkey_ = obs_hotkey_register_source(
				parent, ("stagekit.active.hide." + name).c_str(), text.c_str(),
				[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
					if (pressed)
						static_cast<AutoCapture *>(data)->ask_hide();
				},
				this);
		}
		// Whatever the source is aimed at now is not something we chose.
		aimed_once_ = false;
		applied_key_.clear();
	}
	void detach()
	{
		// Taken off the capture: a Standby Picture it had put over it goes.
		if (covered_ && parent_) {
			std::lock_guard lock(mutex_);
			standby_cover_set(parent_, false, cover_style_, cover_ms_, cover_curve_, cover_mode_);
		}
		covered_ = hidden_ = false;
		if (holding_ && parent_)
			obs_source_dec_showing(parent_);
		holding_ = false;
		parent_ = nullptr;
	}
	obs_source_t *source() const { return source_; }

	// The program being captured now; empty when there is none.
	// Aimed at a program whose picture has not come yet.
	bool waiting_for_picture() const { return parent_ && obs_source_get_base_width(parent_) == 0; }
	std::string current()
	{
		std::lock_guard lock(mutex_);
		return current_;
	}

	// The programs that are open, for choosing from instead of typing.
	std::vector<Window> open_programs()
	{
		std::vector<Window> windows;
		bool use_test;
		{
			std::lock_guard lock(mutex_);
			use_test = use_test_windows_;
			if (use_test)
				windows = test_windows_;
		}
		if (!use_test)
			windows = list_windows();
		std::vector<Window> out;
		for (const Window &w : windows) {
			const std::string exe = lower(w.executable);
			const bool seen = std::any_of(out.begin(), out.end(),
						      [&](const Window &o) { return lower(o.executable) == exe; });
			if (!seen && !built_in_never(exe))
				out.push_back(w);
		}
		return out;
	}

	// Adds a program to the "never" or "always" line and takes it off the other.
	void add_to(const char *list, const std::string &executable)
	{
		if (executable.empty())
			return;
		const std::string exe = lower(executable);
		obs_data_t *settings = obs_source_get_settings(source_);
		for (const char *name : {"never", "always"}) {
			std::string kept;
			for (const std::string &entry : read_list(settings, "", name)) {
				if (entry == exe)
					continue;
				kept += (kept.empty() ? "" : ", ") + entry;
			}
			if (strcmp(name, list) == 0)
				kept += (kept.empty() ? "" : ", ") + exe;
			obs_data_set_string(settings, name, kept.c_str());
		}
		obs_source_update(source_, settings);
		obs_data_release(settings);
	}

private:
	// The Standby Picture over the capture: while it is hidden, and (asked
	// for) while there is no game. Through the Standby Picture filter on the
	// capture, with this filter's transition.
	void standby_step(bool have_game)
	{
		bool no_game, use_standby;
		int style, ms, curve, mode;
		{
			std::lock_guard lock(mutex_);
			no_game = no_game_standby_;
			use_standby = hide_standby_;
			style = cover_style_;
			ms = cover_ms_;
			curve = cover_curve_;
			mode = cover_mode_;
		}
		if (cmd_hide_.exchange(false)) {
			// (Asking for the state it already has only says whether there
			// is a Standby Picture to hide behind.)
			if (use_standby && standby_cover_set(parent_, covered_, style, ms, curve, mode))
				hidden_ = !hidden_;
			else
				hide_items();
			state_s_ = 1.0; // (said at once)
		}
		// For a Stream Deck key showing shown or hidden (once a second: the
		// scenes are gone through for it).
		if ((state_s_ += 0.25) >= 1.0) {
			state_s_ = 0.0;
			vendor_switch_state(source_, false, hidden_ || shown_nowhere(parent_));
		}
		const bool want = hidden_ || (no_game && !have_game);
		if (want != covered_) {
			if (standby_cover_set(parent_, want, style, ms, curve, mode))
				obs_log(LOG_INFO, "Auto Capture: '%s' %s", obs_source_get_name(parent_),
					want ? (hidden_ ? "hidden behind its Standby Picture"
							: "has no game: its Standby Picture is shown")
					     : "shown again");
			covered_ = want;
		}
	}

	// Without a Standby Picture: the capture's items, in every scene and
	// group, hidden or shown with their own show and hide transitions.
	void hide_items()
	{
		struct Found {
			obs_source_t *capture;
			std::vector<obs_sceneitem_t *> items;
		} found{parent_, {}};
		obs_enum_scenes(
			[](void *param, obs_source_t *scene_source) {
				auto *f = static_cast<Found *>(param);
				obs_scene_t *scene = obs_group_or_scene_from_source(scene_source);
				if (scene)
					obs_scene_enum_items(
						scene,
						[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
							auto *f2 = static_cast<Found *>(p);
							if (obs_sceneitem_get_source(item) == f2->capture) {
								obs_sceneitem_addref(item);
								f2->items.push_back(item);
							}
							return true;
						},
						f);
				return true;
			},
			&found);
		const bool any = std::any_of(found.items.begin(), found.items.end(),
					     [](obs_sceneitem_t *item) { return obs_sceneitem_visible(item); });
		for (obs_sceneitem_t *item : found.items) {
			obs_sceneitem_set_visible(item, !any);
			obs_sceneitem_release(item);
		}
		obs_log(LOG_INFO, "Auto Capture: '%s' %s", obs_source_get_name(parent_), any ? "hidden" : "shown");
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	bool no_game_standby_ = false;
	bool hide_standby_ = true;
	int cover_style_ = 0, cover_ms_ = 600, cover_curve_ = 0, cover_mode_ = 0;
	std::atomic<bool> cmd_hide_{false};
	bool hidden_ = false, covered_ = false; // used on the tick only
	double state_s_ = 1.0;                  // since the state was last reported
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
	Mode mode_ = Mode::AnyGame;
	std::vector<std::string> always_, never_;
	bool fallback_fullscreen_ = true;
	std::atomic<bool> keep_ready_{true};
	bool holding_ = false; // used on the tick only
	double timer_ = 0.0;
	std::vector<Window> test_windows_;
	bool use_test_windows_ = false;
	std::string applied_key_;
	// Programs seen to be games, by executable name in lower case. Only the tick uses it.
	std::set<std::string> known_games_;
	bool known_loaded_ = false;
	std::string current_;
	bool aimed_once_ = false;
	double unhooked_s_ = 0.0; // how long the capture has been aimed with no picture
	double give_up_s_ = 10.0; // and how long that may last
	// Programs the capture got no picture from, by executable in lower case,
	// left alone for now. Never saved. Only the tick uses it.
	struct Rest {
		std::string title;  // its window's title when it was given up on
		double since = 0.0; // by clock_s_
		double wait = 0.0;  // how long before it is tried again whatever its title
		int title_tries = 0;
		bool resting = false;
		bool away = false; // seen behind another window since it was given up on
	};
	std::map<std::string, Rest> rest_;
	double clock_s_ = 0.0;
	bool give_up_in_tests_ = false; // (the tests' stand-in capture never has a picture)
};

void show_status(obs_properties_t *props, AutoCapture *self)
{
	const std::string current = self ? self->current() : std::string();
	// (Aimed at a program that gives no picture yet, an emulator with no game
	// running for one, it says so: "Now capturing" over an empty capture read
	// as a fault.)
	const bool empty = !current.empty() && self && self->waiting_for_picture();
	std::string text = obs_module_text(current.empty() ? "Capture.Status.None"
					   : empty         ? "Capture.Status.Waiting"
							   : "Capture.Status.Now");
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, current);
	obs_property_set_description(obs_properties_get(props, "status"), text.c_str());
	obs_property_set_visible(obs_properties_get(props, "never_now"), !current.empty());
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<AutoCapture *>(data);
	obs_properties_t *props = obs_properties_create();

	// What it is doing right now, and one press to undo a wrong choice.
	obs_property_t *status = obs_properties_add_text(props, "status", "", OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);
	obs_properties_add_button2(
		props, "never_now", obs_module_text("Capture.NeverNow"),
		[](obs_properties_t *p, obs_property_t *, void *d) {
			auto *filter = static_cast<AutoCapture *>(d);
			filter->add_to("never", filter->current());
			pick_list_refresh(p, filter->source(), "never", true);
			pick_list_refresh(p, filter->source(), "always", true);
			obs_property_set_description(obs_properties_get(p, "status"),
						     obs_module_text("Capture.Status.None"));
			obs_property_set_visible(obs_properties_get(p, "never_now"), false);
			return true;
		},
		data);
	obs_properties_add_button2(
		props, "check", obs_module_text("Capture.Check"),
		[](obs_properties_t *p, obs_property_t *, void *d) {
			show_status(p, static_cast<AutoCapture *>(d));
			return true;
		},
		data);

	// Two lists, each filled by choosing from the programs that are open.
	obs_source_t *own = self ? self->source() : nullptr;
	auto open_now = [self](std::vector<std::string> &names) {
		if (!self)
			return;
		for (const Window &w : self->open_programs())
			names.push_back(w.executable);
	};
	PickList always;
	always.setting = "always";
	always.as_text = true;
	always.title = "Capture.Always";
	always.add = "List.AddProgram";
	always.tip = "Capture.Always.Tip";
	always.choices = open_now;
	pick_list_add(props, own, always);
	PickList never;
	never.setting = "never";
	never.as_text = true;
	never.title = "Capture.Never";
	never.add = "List.AddProgram";
	never.tip = "Capture.Never.Tip";
	never.choices = open_now;
	pick_list_add(props, own, never);

	obs_properties_t *lists = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(lists, "mode", obs_module_text("Capture.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Capture.Mode.AnyGame"), (int)Mode::AnyGame);
	obs_property_list_add_int(mode, obs_module_text("Capture.Mode.ListedOnly"), (int)Mode::ListedOnly);
	obs_properties_add_bool(lists, "fallback_fullscreen", obs_module_text("Capture.Fallback"));
	obs_property_t *keep_ready = obs_properties_add_bool(lists, "keep_ready", obs_module_text("Capture.KeepReady"));
	set_tip(keep_ready, obs_module_text("Capture.KeepReady.Tip"));
	obs_property_t *no_game =
		obs_properties_add_bool(lists, "no_game_standby", obs_module_text("Capture.NoGameStandby"));
	set_tip(no_game, obs_module_text("Capture.NoGameStandby.Tip"));
	obs_properties_add_group(props, "lists", obs_module_text("Capture.Lists"), OBS_GROUP_NORMAL, lists);

	// The Standby Picture over the capture: hiding it, and the transition.
	obs_properties_t *standby = obs_properties_create();
	obs_property_t *hide_standby =
		obs_properties_add_bool(standby, "hide_standby", obs_module_text("AppCapture.HideStandby"));
	set_tip(hide_standby, obs_module_text("Capture.HideStandby.Tip"));
	obs_property_t *style = obs_properties_add_list(standby, "standby_style", obs_module_text("Change.With"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	{
		// The Standby Picture's own transitions, by its numbers.
		static const struct {
			int value;
			const char *text;
		} styles[] = {{0, "Standby.Style.Fade"},      {11, "Standby.Style.Morph"},
			      {10, "Standby.Style.Zoom"},     {1, "Standby.Style.Cut"},
			      {2, "Standby.Style.SlideLeft"}, {3, "Standby.Style.SlideRight"},
			      {4, "Standby.Style.SlideUp"},   {5, "Standby.Style.SlideDown"},
			      {6, "Standby.Style.WipeLeft"},  {7, "Standby.Style.WipeRight"},
			      {8, "Standby.Style.WipeUp"},    {9, "Standby.Style.WipeDown"}};
		for (const auto &one : styles)
			obs_property_list_add_int(style, obs_module_text(one.text), one.value);
	}
	set_tip(style, obs_module_text("Capture.StandbyStyle.Tip"));
	obs_property_t *ms =
		obs_properties_add_int_slider(standby, "standby_ms", obs_module_text("Change.Time"), 0, 5000, 50);
	obs_property_int_set_suffix(ms, " ms");
	add_easing_properties(standby, "standby_");
	obs_properties_add_button2(
		standby, "hide_now", obs_module_text("AppCapture.Hide.Now"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			if (d)
				static_cast<AutoCapture *>(d)->ask_hide();
			return false;
		},
		data);
	obs_properties_add_group(props, "standby", obs_module_text("Standby.Name.Short"), OBS_GROUP_NORMAL, standby);

	show_status(props, self);
	return props;
}

} // namespace

std::vector<OpenWindow> open_windows()
{
	std::vector<OpenWindow> out;
	for (const Window &w : g_window_watch.latest())
		out.push_back({w.title, w.executable, w.foreground, w.window_class, w.handle});
	return out;
}

namespace {
std::mutex g_test_front_mutex;
Window g_test_front; // set by the tests; no executable means "not set"
} // namespace

void set_front_window_for_tests(const std::string &title, const std::string &window_class,
				const std::string &executable)
{
	std::lock_guard lock(g_test_front_mutex);
	g_test_front = Window();
	g_test_front.title = title;
	g_test_front.window_class = window_class;
	g_test_front.executable = executable;
	g_test_front.foreground = true;
}

namespace {
std::mutex g_hook_mutex;
std::thread g_hook_thread; // the hide, aim, show of the last hook, when it has a transition

// Every scene item showing a source, held, for the hook's transition.
std::vector<obs_sceneitem_t *> items_of(obs_source_t *wanted)
{
	struct Find {
		obs_source_t *wanted;
		std::vector<obs_sceneitem_t *> out;
	} find{wanted, {}};
	static const auto visit = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *f = static_cast<Find *>(param);
		if (obs_sceneitem_get_source(item) == f->wanted && obs_sceneitem_visible(item)) {
			obs_sceneitem_addref(item);
			f->out.push_back(item);
		}
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene_source) {
			if (obs_source_is_group(scene_source))
				return true; // reached through the scene that holds it
			if (obs_scene_t *scene = obs_scene_from_source(scene_source))
				obs_scene_enum_items(
					scene,
					[](obs_scene_t *s, obs_sceneitem_t *item, void *p) {
						visit(s, item, p);
						if (obs_sceneitem_is_group(item))
							obs_sceneitem_group_enum_items(item, visit, p);
						return true;
					},
					param);
			return true;
		},
		&find);
	return find.out;
}

void aim(obs_source_t *capture, const Window &front, bool game)
{
	obs_data_t *settings = obs_source_get_settings(capture);
	const std::string window =
		encode_part(front.title) + ":" + encode_part(front.window_class) + ":" + encode_part(front.executable);
	obs_data_set_string(settings, "window", window.c_str());
	obs_data_set_int(settings, "priority", 2); // match by executable
	if (game)
		obs_data_set_string(settings, "capture_mode", "window");
	obs_source_update(capture, settings);
	obs_data_release(settings);
}
} // namespace

void aim_capture_at(obs_source_t *capture, const OpenWindow &window, bool by_title)
{
	const char *id = capture ? obs_source_get_unversioned_id(capture) : nullptr;
	if (!id)
		return;
	obs_data_t *settings = obs_source_get_settings(capture);
	const std::string text = encode_part(window.title) + ":" + encode_part(window.window_class) + ":" +
				 encode_part(window.executable);
	obs_data_set_string(settings, "window", text.c_str());
	obs_data_set_int(settings, "priority", by_title ? 1 : 2); // by title, or by executable
	if (strcmp(id, "game_capture") == 0)
		obs_data_set_string(settings, "capture_mode", "window");
	obs_source_update(capture, settings);
	obs_data_release(settings);
}

bool hook_front_window(obs_source_t *capture, std::string &what, std::string &error, const std::string &transition,
		       uint32_t transition_ms)
{
	const char *id = capture ? obs_source_get_unversioned_id(capture) : nullptr;
	const bool game = id && strcmp(id, "game_capture") == 0;
	if (!game && !(id && strcmp(id, "window_capture") == 0)) {
		error = "That source is not a Game Capture or a Window Capture";
		return false;
	}
	Window front;
	{
		std::lock_guard lock(g_test_front_mutex);
		front = g_test_front;
	}
	if (front.executable.empty()) {
		for (const Window &w : g_window_watch.latest())
			if (w.foreground)
				front = w;
	}
	if (front.executable.empty()) {
		error = "No window is in front";
		return false;
	}
	const std::string exe = lower(front.executable);
	if (exe == "obs64.exe" || exe == "obs.exe" || exe == "obs") {
		error = "OBS itself is in front";
		return false;
	}
	// A Capture App in Use on it takes the window itself: it changes over
	// in its own way, with the last picture kept until the new one is
	// there, and holds it. (Switched off instead, as up to beta117, the
	// picture vanished at once.)
	if (app_capture_hold(capture,
			     OpenWindow{front.title, front.executable, true, front.window_class, front.handle})) {
		obs_log(LOG_INFO, "Hook: '%s' is handed %s, by hand", obs_source_get_name(capture),
			front.executable.c_str());
		what = front.executable;
		return true;
	}
	// An Auto Capture here would aim the capture somewhere else again.
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *) {
			const char *kind = obs_source_get_unversioned_id(filter);
			if (kind && strcmp(kind, kAutoCaptureId) == 0 && obs_source_enabled(filter))
				obs_source_set_enabled(filter, false);
		},
		nullptr);
	obs_log(LOG_INFO, "Hook: '%s' now captures %s, by hand", obs_source_get_name(capture),
		front.executable.c_str());
	what = front.executable;

	const bool at_once = transition.empty() || transition == kTransitionCut || transition_ms == 0;
	std::vector<obs_sceneitem_t *> items = at_once ? std::vector<obs_sceneitem_t *>() : items_of(capture);
	if (items.empty()) {
		aim(capture, front, game);
		return true;
	}
	// Out with the transition chosen, aimed while out of sight, back in with
	// it: on a thread of its own, which holds what it uses.
	for (obs_sceneitem_t *item : items) {
		set_item_transitions(item, transition, transition_ms);
		obs_sceneitem_set_visible(item, false);
	}
	obs_source_t *held = obs_source_get_ref(capture);
	std::lock_guard lock(g_hook_mutex);
	if (g_hook_thread.joinable())
		g_hook_thread.join();
	g_hook_thread = std::thread([held, items, front, game, transition_ms] {
		os_set_thread_name("stagekit: hook");
		os_sleep_ms(transition_ms);
		aim(held, front, game);
		os_sleep_ms(350); // the capture takes a moment to find its window
		for (obs_sceneitem_t *item : items) {
			obs_sceneitem_set_visible(item, true);
			obs_sceneitem_release(item);
		}
		obs_source_release(held);
	});
	return true;
}

void stop_window_watch()
{
	{
		std::lock_guard lock(g_hook_mutex);
		if (g_hook_thread.joinable())
			g_hook_thread.join();
	}
	g_window_watch.stop();
}

std::vector<std::string> open_program_names()
{
	std::vector<std::string> names;
	for (const Window &w : list_windows()) {
		if (built_in_never(lower(w.executable)))
			continue;
		if (std::find(names.begin(), names.end(), w.executable) == names.end())
			names.push_back(w.executable);
	}
	return names;
}

void register_auto_capture()
{
	// For the tests: a window to take as the one in front, and the hook
	// itself as the remote request runs it.
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_test_front_window(in string title, in string window_class, in string executable)",
		[](void *, calldata_t *cd) {
			set_front_window_for_tests(calldata_string(cd, "title"), calldata_string(cd, "window_class"),
						   calldata_string(cd, "executable"));
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_hook_front(in string source, in string transition, in int ms, out bool success, out string said)",
		[](void *, calldata_t *cd) {
			obs_source_t *source = obs_get_source_by_name(calldata_string(cd, "source"));
			std::string what, error;
			const char *transition = calldata_string(cd, "transition");
			const bool ok = hook_front_window(source, what, error, transition ? transition : "",
							  (uint32_t)calldata_int(cd, "ms"));
			calldata_set_bool(cd, "success", ok);
			calldata_set_string(cd, "said", ok ? what.c_str() : error.c_str());
			obs_source_release(source);
		},
		nullptr);

	obs_source_info info = {};
	info.id = kAutoCaptureId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Capture.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AutoCapture(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AutoCapture *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AutoCapture *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "mode", (int)Mode::AnyGame);
		obs_data_set_default_bool(settings, "fallback_fullscreen", true);
		obs_data_set_default_bool(settings, "keep_ready", true);
		obs_data_set_default_bool(settings, "no_game_standby", false);
		obs_data_set_default_bool(settings, "hide_standby", true);
		obs_data_set_default_int(settings, "standby_style", 0);
		obs_data_set_default_int(settings, "standby_ms", 600);
		obs_data_set_default_int(settings, "standby_ease_curve", (int)EaseCurve::Cubic);
		obs_data_set_default_int(settings, "standby_ease_mode", (int)EaseMode::InOut);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<AutoCapture *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<AutoCapture *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<AutoCapture *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<AutoCapture *>(data)->detach();
	};
	register_timed(&info);
}

} // namespace stagekit
