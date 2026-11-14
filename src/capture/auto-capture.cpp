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
#include "../common/timing.hpp"

#include "../common/pick-list.hpp"

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
		if (!parent_ || !obs_source_enabled(source_))
			return;
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

		auto excluded = [&](const std::string &exe) {
			return matches(exe, never) || built_in_never(exe);
		};
		auto key_of = [](const Window &w) {
			return w.window_class + ":" + w.executable;
		};

		// 1. A game in front wins.  2. Otherwise stay on the game already
		// being captured while its window exists.  3. Otherwise any program
		// from the "always capture" list that is open.
		// A program is only looked into while it is in front. Once it has
		// been seen to be a game it is remembered, so the next time it is
		// picked up at once, in a window, in front or not.
		const Window *in_front = nullptr, *current = nullptr, *listed_open = nullptr, *known_open = nullptr;
		bool learned_something = false;
		for (const Window &w : windows) {
			const std::string exe = lower(w.executable);
			if (excluded(exe) && !matches(exe, always))
				continue;
			const bool listed = matches(exe, always);
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

		const char *parent_id = obs_source_get_unversioned_id(parent_);
		const bool game_capture = parent_id && strcmp(parent_id, "game_capture") == 0;

		// Emulators put the frame rate in their title, so what counts as
		// "a different window" is the program and window class only.
		const std::string key = chosen ? key_of(*chosen) : std::string();
		{
			std::lock_guard lock(mutex_);
			current_ = chosen ? chosen->executable : std::string();
		}
		if (aimed_once_ && key == applied_key_)
			return;
		aimed_once_ = true;

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
		// Whatever the source is aimed at now is not something we chose.
		aimed_once_ = false;
		applied_key_.clear();
	}
	void detach()
	{
		if (holding_ && parent_)
			obs_source_dec_showing(parent_);
		holding_ = false;
		parent_ = nullptr;
	}
	obs_source_t *source() const { return source_; }

	// The program being captured now; empty when there is none.
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
	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
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
};

void show_status(obs_properties_t *props, AutoCapture *self)
{
	const std::string current = self ? self->current() : std::string();
	std::string text = obs_module_text(current.empty() ? "Capture.Status.None" : "Capture.Status.Now");
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
	obs_property_set_long_description(keep_ready, obs_module_text("Capture.KeepReady.Tip"));
	obs_properties_add_group(props, "lists", obs_module_text("Capture.Lists"), OBS_GROUP_NORMAL, lists);

	show_status(props, self);
	return props;
}

} // namespace

std::vector<OpenWindow> open_windows()
{
	std::vector<OpenWindow> out;
	for (const Window &w : g_window_watch.latest())
		out.push_back({w.title, w.executable, w.foreground});
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

bool hook_front_window(obs_source_t *capture, std::string &what, std::string &error)
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
	// An Auto Capture here would aim the capture somewhere else again.
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *) {
			const char *kind = obs_source_get_unversioned_id(filter);
			if (kind && strcmp(kind, kAutoCaptureId) == 0 && obs_source_enabled(filter))
				obs_source_set_enabled(filter, false);
		},
		nullptr);
	obs_data_t *settings = obs_source_get_settings(capture);
	const std::string window =
		encode_part(front.title) + ":" + encode_part(front.window_class) + ":" + encode_part(front.executable);
	obs_data_set_string(settings, "window", window.c_str());
	obs_data_set_int(settings, "priority", 2); // match by executable
	if (game)
		obs_data_set_string(settings, "capture_mode", "window");
	obs_source_update(capture, settings);
	obs_data_release(settings);
	obs_log(LOG_INFO, "Hook: '%s' now captures %s, by hand", obs_source_get_name(capture),
		front.executable.c_str());
	what = front.executable;
	return true;
}

void stop_window_watch()
{
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
		obs_get_proc_handler(), "void stagekit_hook_front(in string source, out bool success, out string said)",
		[](void *, calldata_t *cd) {
			obs_source_t *source = obs_get_source_by_name(calldata_string(cd, "source"));
			std::string what, error;
			const bool ok = hook_front_window(source, what, error);
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
