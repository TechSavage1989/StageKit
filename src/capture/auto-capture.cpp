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
#include "../common/filter-input.hpp"
#include "../common/scene-util.hpp"
#include "../common/easing.hpp"
#include "../common/timing.hpp"

#include "../common/item-transition.hpp"
#include "../common/hid-items.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/tooltip.hpp"
#include "../common/hotkey-rename.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
	GameArea game_area;           // only looked for in windows asked for
};

// The windows whose game area has been asked for, and when (os_gettime_ns).
std::mutex g_area_mutex;
std::map<uint64_t, uint64_t> g_area_wanted;

#ifdef _WIN32
constexpr uint64_t kAreaWantedNs = 5000000000ULL;

// The handles asked for in the last few seconds; the rest are forgotten.
std::set<uint64_t> areas_wanted_now()
{
	std::set<uint64_t> out;
	std::lock_guard lock(g_area_mutex);
	const uint64_t now = os_gettime_ns(); // (under the lock: never before a time noted)
	for (auto it = g_area_wanted.begin(); it != g_area_wanted.end();) {
		if (now - it->second > kAreaWantedNs) {
			it = g_area_wanted.erase(it);
			continue;
		}
		out.insert(it->first);
		++it;
	}
	return out;
}
#endif

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

// A program's own controls, never where it draws its picture. (Window classes
// of the common controls, and the words their kinds are named with in other
// toolkits. Also Chromium's page window, in every Chromium browser and
// Electron app: a browser on the switcher's lists keeps its tabs and address
// bar, as before.)
bool control_class(const std::string &window_class)
{
	const std::string c = lower(window_class);
	for (const char *known :
	     {"scrollbar", "button", "static", "edit", "combobox", "comboboxex32", "listbox", "syslistview32",
	      "systreeview32", "systabcontrol32", "msctls_trackbar32", "msctls_progress32", "msctls_updown32",
	      "sysheader32", "#32768", "#32770", "chrome_renderwidgethosthwnd", "intermediate d3d window"})
		if (c == known)
			return true;
	for (const char *part : {"statusbar", "toolbar", "menubar", "rebar", "tooltip", "richedit", "scrollbar"})
		if (c.find(part) != std::string::npos)
			return true;
	return false;
}

struct AreaLook {
	HWND top = nullptr;
	LONG client_w = 0, client_h = 0;
	long long best = 0; // the area of the best child so far
	RECT rect{};        // its part of the client area, in client pixels
	std::string child_class;
};

BOOL CALLBACK look_at_child(HWND child, LPARAM param)
{
	auto *look = reinterpret_cast<AreaLook *>(param);
	if (!IsWindowVisible(child))
		return TRUE;
	wchar_t name[256];
	const int length = GetClassNameW(child, name, 256);
	const std::string child_class = utf8(name, length);
	if (control_class(child_class))
		return TRUE;
	RECT r;
	if (!GetWindowRect(child, &r))
		return TRUE;
	// From the screen to the top-level window's client area: both are in
	// this process's DPI terms, so they agree whatever the monitor's scaling.
	MapWindowPoints(HWND_DESKTOP, look->top, reinterpret_cast<POINT *>(&r), 2);
	const LONG left = std::max<LONG>(r.left, 0), top = std::max<LONG>(r.top, 0);
	const LONG right = std::min<LONG>(r.right, look->client_w), bottom = std::min<LONG>(r.bottom, look->client_h);
	if (right <= left || bottom <= top)
		return TRUE;
	const LONG w = right - left, h = bottom - top;
	// At least two fifths of the client area, and something of it left out
	// (a child that fills it all is a frame round the picture, not the
	// picture's own part of the window).
	const long long area = (long long)w * h, whole = (long long)look->client_w * look->client_h;
	if (area * 5 < whole * 2 || (look->client_w - w) + (look->client_h - h) < 4)
		return TRUE;
	if (area > look->best) {
		look->best = area;
		look->rect = RECT{left, top, right, bottom};
		look->child_class = child_class;
	}
	return TRUE;
}

// Where in its client area a window draws its picture: the largest visible
// child window that is not a control. Emulators draw into a child of their
// own (Qt's native render widget, an Avalonia native host, Dolphin's render
// window) under their menu bar and above their status bar.
GameArea find_game_area(HWND window)
{
	GameArea area;
	area.looked = true;
	RECT client;
	if (!GetClientRect(window, &client) || client.right < 64 || client.bottom < 64)
		return area;
	area.client_w = (int)client.right;
	area.client_h = (int)client.bottom;
	AreaLook look;
	look.top = window;
	look.client_w = client.right;
	look.client_h = client.bottom;
	EnumChildWindows(window, look_at_child, reinterpret_cast<LPARAM>(&look));
	if (look.best > 0) {
		area.x = (int)look.rect.left;
		area.y = (int)look.rect.top;
		area.w = (int)(look.rect.right - look.rect.left);
		area.h = (int)(look.rect.bottom - look.rect.top);
		area.child_class = look.child_class;
	}
	return area;
}

struct Collect {
	std::vector<Window> windows;
	std::set<uint64_t> areas; // the windows to look for a game area in
	// Every program with a top-level window of any kind, shown or not
	// (lower case), and the processes seen.
	std::set<std::string> running;
	std::set<DWORD> processes;
};

// A process's program file name, by process id, for the programs that are
// running. Only `list_windows` uses it (under its lock); a process not seen
// in a look is forgotten, so a name is looked up once while its process
// lives. (Empty when the process cannot be looked at.)
std::map<DWORD, std::string> g_program_of;

const std::string &program_of(DWORD process_id)
{
	auto it = g_program_of.find(process_id);
	if (it != g_program_of.end())
		return it->second;
	std::string name;
	if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)) {
		wchar_t path[MAX_PATH];
		DWORD path_length = MAX_PATH;
		if (QueryFullProcessImageNameW(process, 0, path, &path_length)) {
			const wchar_t *file = path;
			for (DWORD i = 0; i < path_length; i++)
				if (path[i] == L'\\' || path[i] == L'/')
					file = path + i + 1;
			name = lower(utf8(file, (int)(path + path_length - file)));
		}
		CloseHandle(process);
	}
	return g_program_of.emplace(process_id, name).first->second;
}

BOOL CALLBACK collect_window(HWND window, LPARAM param)
{
	auto *collect = reinterpret_cast<Collect *>(param);
	std::vector<Window> *windows = &collect->windows;
	// A program whose windows are all hidden, minimised or being made again
	// (an emulator going full-screen, starting or stopping a game) is still
	// running: Auto Capture stays aimed at it (`running`).
	DWORD process_id = 0;
	GetWindowThreadProcessId(window, &process_id);
	collect->processes.insert(process_id);
	if (const std::string &program = program_of(process_id); !program.empty())
		collect->running.insert(program);
	// Only real, visible, top-level application windows.
	if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER) != nullptr)
		return TRUE;
	if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
		return TRUE;

	// (Never GetWindowTextW on one of OBS's own windows: for a window of
	// this process it asks the window's thread, which may be waiting for
	// this very look. InternalGetWindowText reads the text as it is kept.)
	wchar_t title[512];
	const int title_length = process_id == GetCurrentProcessId() ? InternalGetWindowText(window, title, 512)
								     : GetWindowTextW(window, title, 512);
	if (title_length <= 0)
		return TRUE;

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
	if (collect->areas.count(found.handle))
		found.game_area = find_game_area(window);
	windows->push_back(found);
	return TRUE;
}

std::vector<Window> list_windows(std::set<std::string> *running = nullptr)
{
	// One look at a time: the thread that keeps the list and a properties
	// window asking for itself share what is remembered about programs.
	static std::mutex one_at_a_time;
	std::lock_guard lock(one_at_a_time);
	Collect collect;
	collect.areas = areas_wanted_now();
	EnumWindows(collect_window, reinterpret_cast<LPARAM>(&collect));
	for (auto it = g_program_of.begin(); it != g_program_of.end();)
		it = collect.processes.count(it->first) ? std::next(it) : g_program_of.erase(it);
	if (running)
		*running = std::move(collect.running);
	return std::move(collect.windows);
}

#else

std::vector<Window> list_windows(std::set<std::string> *running = nullptr)
{
	if (running)
		running->clear();
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

	// The windows, and (asked for) the programs running and which look
	// that was (`generation`, one more at every look).
	std::vector<Window> latest(std::set<std::string> *running = nullptr, uint64_t *generation = nullptr)
	{
		asked_at_ = os_gettime_ns();
		std::unique_lock lock(mutex_);
		if (stopped_)
			return {};
		if (!thread_.joinable()) {
			// The first time: one look here and now, so nobody starts
			// from an empty list, then the thread takes over.
			lock.unlock();
			std::set<std::string> names;
			std::vector<Window> first = list_windows(&names);
			lock.lock();
			if (!thread_.joinable() && !stopped_) {
				windows_ = std::move(first);
				running_ = std::move(names);
				generation_++;
				thread_ = std::thread([this] { run(); });
			}
		}
		if (running)
			*running = running_;
		if (generation)
			*generation = generation_.load();
		return windows_;
	}

	// Which look the list is from (for acting at once on a new one).
	uint64_t generation() const { return generation_.load(); }

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
			std::set<std::string> names;
			std::vector<Window> now = list_windows(&names);
			lock.lock();
			windows_ = std::move(now);
			running_ = std::move(names);
			generation_++;
		}
	}

	std::mutex mutex_;
	std::condition_variable wake_;
	std::thread thread_;
	std::vector<Window> windows_;
	std::set<std::string> running_;
	std::atomic<uint64_t> generation_{0};
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

// The title part of what OBS writes as a window, unescaped.
std::string window_title_of(const std::string &window)
{
	const std::string part = window.substr(0, window.find(':'));
	std::string out;
	for (size_t i = 0; i < part.size(); i++) {
		if (part.compare(i, 3, "#22") == 0) {
			out += '#';
			i += 2;
		} else if (part.compare(i, 3, "#3A") == 0) {
			out += ':';
			i += 2;
		} else {
			out += part[i];
		}
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

// A window title with its digits taken out and in lower case: a frame rate
// or a volume going up and down is not a new title.
std::string title_shape(const std::string &title)
{
	std::string out;
	for (char c : lower(title))
		if (c < '0' || c > '9')
			out += c;
	return out;
}

// What a resting program's Game Capture is aimed at: a window that does
// not exist (title:class:executable), in window mode, so OBS hooks nothing.
constexpr const char *kNoWindow = ":StageKitNoWindow:stagekit-no-window.exe";
// The key a capture aimed at no window is known by (no window's key).
constexpr const char *kRestKey = "\x01rest";
// How many rests a new title may end while the program stays open: a
// title is only new once (`Tried`), and this is the backstop for a title
// whose words keep changing.
constexpr int kTitleTries = 8;
// While a program rests, the capture looks in on it: aimed at it for
// `kProbeFor`, every `kProbeEvery` (from the end of the last look), so a
// game started in an emulator's menu is picked up within seconds. OBS's
// Game Capture tries to hook about every 0.42 s (the owner's log), so a
// look is two or three tries. After `kQuickProbes` empty looks the gap
// grows by half each time to `kProbeEveryMost`. The owner (2026-11-18):
// "game capture is taking quite a while to hook windowed games and
// emulators, but i don't want the overhead processing of OBS to go up".
constexpr double kProbeEvery = 5.0;
constexpr double kProbeFor = 1.0;
constexpr int kQuickProbes = 12;
constexpr double kProbeEveryMost = 15.0;
// (A bound on the titles remembered for one program.)
constexpr size_t kTriedMost = 64;
// How long the capture stays aimed at a program that is not seen running
// at all (no window of any kind), in case it is only making its one window
// again: only a candidate clicked into takes over meanwhile; after it, an
// open candidate or the fallback.
constexpr double kKeepGrace = 3.0;
// How long a program hooked by hand may go unseen, after loading, before
// the Auto Capture it was handed from takes the capture back.
constexpr double kHandGrace = 5.0;

class AutoCapture {
public:
	AutoCapture(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		// For the tests: stand-in windows, one per line,
		// "title|class|exe|front|game|id|hidden" (the last four may be
		// left empty; the id stands for the window's handle; "hidden" is a
		// window that is there and cannot be seen, minimised or hidden: its
		// program is running, and the window is not in the list);
		// `keep_clock`, when set, leaves the next look where it was.
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_test_windows(in string windows)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoCapture *>(data);
				const char *text = calldata_string(cd, "windows");
				std::vector<Window> windows;
				std::set<std::string> running;
				std::set<uint64_t> unlisted;
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
					fields.resize(7);
					w.title = fields[0];
					w.window_class = fields[1];
					w.executable = fields[2];
					w.foreground = fields[3] == "front";
					w.looks_like_game = fields[4] == "game";
					w.handle = strtoull(fields[5].c_str(), nullptr, 10);
					running.insert(lower(w.executable));
					if (fields[6] != "hidden")
						windows.push_back(w);
					else if (w.handle)
						unlisted.insert(w.handle);
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
				self->test_running_ = running;
				self->test_unlisted_ = unlisted;
				self->use_test_windows_ = true;
				// (Looked at at once, unless asked to keep time: every
				// look moves the filter's clock on a quarter second.)
				if (!calldata_bool(cd, "keep_clock"))
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
		unlisten();
		vendor_switch_forget(source_);
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
	}

	// Switched off by a hook by hand (`hand_hooked`, the program it was
	// handed to): while switched off, the program is looked for four times
	// a second, and once no window of it is open (seen open before, or
	// not seen for `kHandGrace` after loading) it switches itself on again
	// and follows games. The owner's Game Capture, hooked by hand on an
	// emulator days before, stayed aimed at the closed program for good.
	// One switched off by the user (no record) is never switched on.
	void hand_watch(float seconds)
	{
		std::string exe;
		std::set<std::string> running;
		bool use_test;
		{
			std::lock_guard lock(mutex_);
			if (hand_exe_.empty() || !scenes_loaded())
				return;
			hand_wait_s_ += seconds;
			hand_timer_ -= seconds;
			if (hand_timer_ > 0.0)
				return;
			hand_timer_ = 0.25;
			exe = hand_exe_;
			use_test = use_test_windows_;
			if (use_test)
				running = test_running_;
		}
		if (!use_test)
			g_window_watch.latest(&running);
		const bool open = running.count(exe) > 0;
		{
			std::lock_guard lock(mutex_);
			if (exe != hand_exe_)
				return;
			if (open) {
				hand_seen_ = true;
				return;
			}
			if (!hand_seen_ && hand_wait_s_ < kHandGrace)
				return;
		}
		hand_forget();
		obs_source_t *parent = obs_filter_get_parent(source_);
		obs_log(LOG_INFO, "Auto Capture: '%s' back to following games: %s closed",
			parent ? obs_source_get_name(parent) : obs_source_get_name(source_), exe.c_str());
		obs_source_set_enabled(source_, true);
	}

	// Drops the record of a hook by hand.
	void hand_forget()
	{
		{
			std::lock_guard lock(mutex_);
			if (hand_exe_.empty())
				return;
			hand_exe_.clear();
			hand_seen_ = false;
			hand_wait_s_ = 0.0;
		}
		obs_data_t *settings = obs_source_get_settings(source_);
		obs_data_erase(settings, "hand_hooked");
		obs_data_release(settings);
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
		hide_change_.kind = obs_data_get_string(settings, "transition");
		hide_change_.ms = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "transition_ms"), 0, 10000);
		hid_pending_ = !hid_items_read(settings).empty();
		{
			const std::string hand = lower(obs_data_get_string(settings, "hand_hooked"));
			if (hand != hand_exe_) {
				hand_exe_ = hand;
				hand_seen_ = false;
				hand_wait_s_ = 0.0;
				hand_timer_ = 0.0;
			}
		}
		cover_style_ = (int)std::clamp<long long>(obs_data_get_int(settings, "standby_style"), -1, 11);
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
		if (obs_source_enabled(source_))
			hand_forget(); // switched on, by the user or by itself: following again
		else if (parent_)
			hand_watch(seconds);
		if (!parent_ || !obs_source_enabled(source_)) {
			// Switched off: a picture it put over the capture goes.
			if (covered_ && parent_) {
				std::lock_guard lock(mutex_);
				standby_cover_set(parent_, false, cover_style_, cover_ms_, cover_curve_, cover_mode_);
			}
			covered_ = hidden_ = false;
			// (What it watched may be aimed elsewhere by the time it is
			// switched on again: a capture hooked by hand. Switched on, it
			// aims afresh, as when first added, which is how a capture
			// hooked by hand is handed back: what it aimed at before is
			// neither kept nor taken for written.)
			captured_ = Captured{};
			aimed_once_ = false;
			applied_key_.clear();
			return;
		}
		Mode mode;
		std::vector<std::string> always, never;
		std::vector<Window> windows;
		// The programs running, by any window they have (`collect_window`).
		std::set<std::string> running;
		// (In the tests: the stand-in windows that are there and not listed.)
		std::set<uint64_t> unlisted;
		bool fallback, use_test;
		double step;
		{
			std::lock_guard lock(mutex_);
			timer_ -= seconds;
			since_s_ += seconds;
			// A new look at the windows is acted on at once, so a program
			// that comes back is followed within one look; the timer is
			// for when no new look comes. (The tests' stand-in windows
			// are looked at by the quarter second, as always.)
			const bool fresh = !use_test_windows_ && g_window_watch.generation() != seen_look_;
			if (timer_ > 0.0 && !fresh)
				return;
			step = use_test_windows_ ? 0.25 : std::clamp(since_s_, 0.0, 1.0);
			since_s_ = 0.0;
			timer_ = use_test_windows_ ? 0.25 : 0.5;
			clock_s_ += step;
			mode = mode_;
			always = always_;
			never = never_;
			fallback = fallback_fullscreen_;
			use_test = use_test_windows_;
			if (use_test) {
				windows = test_windows_;
				running = test_running_;
				unlisted = test_unlisted_;
			}
		}
		if (!use_test)
			windows = g_window_watch.latest(&running, &seen_look_);

		auto key_of = [](const Window &w) {
			return w.window_class + ":" + w.executable;
		};

		const char *parent_id = obs_source_get_unversioned_id(parent_);
		const bool game_capture = parent_id && strcmp(parent_id, "game_capture") == 0;
		// The game's own window closing while its program stays open.
		if (game_capture)
			watch_captured(windows, running, unlisted, use_test);

		// The program the capture is aimed at now (lower case; none while
		// it is aimed at no window), and when it was last seen running.
		const size_t key_colon = applied_key_.rfind(':');
		const std::string aimed = applied_key_.empty() || applied_key_ == kRestKey ||
							  key_colon == std::string::npos
						  ? std::string()
						  : lower(applied_key_.substr(key_colon + 1));
		if (aimed != keep_exe_) {
			keep_exe_ = aimed;
			keep_seen_s_ = clock_s_;
			keep_said_ = 0;
			had_picture_ = false;
		}
		const bool aimed_running = !aimed.empty() && running.count(aimed) > 0;
		if (aimed_running)
			keep_seen_s_ = clock_s_;

		// 1. A game in front wins.  2. Otherwise stay on the game already
		// being captured while its window exists (or another window of its
		// program, or, while its program runs, none to be seen).
		// 3. Otherwise any program from the "always capture" list that is
		// open.
		// A program is only looked into while it is in front. Once it has
		// been seen to be a game it is remembered, so the next time it is
		// picked up at once, in a window, in front or not.
		const Window *in_front = nullptr, *current = nullptr, *listed_open = nullptr, *known_open = nullptr;
		const Window *same_program = nullptr;
		bool learned_something = false;
		// (A program that has closed is no longer being rested from.)
		// Whether any of a resting program's windows is in front: an
		// emulator has several (Dolphin's GBA windows), and one of them
		// behind another is not the program having been left.
		std::set<std::string> rest_in_front;
		// And whether one of its windows shows a title not tried before.
		std::set<std::string> rest_retitled;
		// (Closed is no window of any kind: one whose windows are all
		// minimised or hidden has not closed.)
		for (auto it = rest_.begin(); it != rest_.end();) {
			const bool open = running.count(it->first) > 0;
			const Tried *tried = nullptr;
			if (auto t = tried_.find(it->first); t != tried_.end())
				tried = &t->second;
			for (const Window &w : windows) {
				if (lower(w.executable) != it->first)
					continue;
				if (w.foreground)
					rest_in_front.insert(it->first);
				if (!tried || !tried->shapes.count(title_shape(w.title)))
					rest_retitled.insert(it->first);
			}
			it = open ? std::next(it) : rest_.erase(it);
		}
		for (auto it = tried_.begin(); it != tried_.end();)
			it = running.count(it->first) ? std::next(it) : tried_.erase(it);
		bool aimed_listed = false; // (a window of it in the list, of any kind)
		for (const Window &w : windows) {
			const std::string exe = lower(w.executable);
			aimed_listed = aimed_listed || (!aimed.empty() && exe == aimed);
			// Either list can name a program or part of a window's title:
			// two builds of one program share a file name.
			const std::string title = lower(w.title);
			const bool listed = matches(exe, always) || matches(title, always);
			// (Nor a program an Automatic Window Switcher is for: that is a
			// window to capture as a window, not a game to hook.)
			if (!listed && (matches(exe, never) || matches(title, never) || built_in_never(exe) ||
					app_capture_lists(exe, title))) {
				// Excluded now (put on Never capture while it rested): no
				// longer rested, so the full-screen fallback applies again.
				rest_.erase(exe);
				continue;
			}
			// Always capture never rests, also when put there during a look.
			if (listed)
				rest_.erase(exe);
			// A program the capture got no picture from is left alone for a
			// while, not for good: an emulator with no game running gives
			// none either. It is looked in on every few seconds (`probe_at`),
			// and tried again at once, for the whole patience, when one of
			// its windows shows a title it was never aimed at with (a game was
			// started) or it is clicked into again.
			if (auto rest = rest_.find(exe); !listed && rest != rest_.end() && rest->second.resting) {
				Rest &r = rest->second;
				Tried &tried = tried_[exe];
				// A title only counts once: every title the program showed
				// while the capture was aimed at it is in `tried`, digits
				// taken out. The owner's Dolphin (2026-11-18): its titles
				// change all the time (frame rate, a GBA window's "Volume
				// 100%" or "Muted"), and every rest ended 0.27 s after it
				// began, hundreds of hooking tries a session.
				bool retitled = false;
				if (rest_retitled.count(exe)) {
					if (tried.title_ends < kTitleTries)
						retitled = true;
					for (const Window &o : windows)
						if (lower(o.executable) == exe && tried.shapes.size() < kTriedMost)
							tried.shapes.insert(title_shape(o.title));
				}
				// (Or it has been clicked into again: what one does to start
				// a game in an emulator whose title stays as it is.)
				const bool front = rest_in_front.count(exe) > 0;
				r.away = r.away || !front;
				const bool came_back = front && r.away;
				const bool look = clock_s_ >= r.probe_at;
				if (!retitled && !came_back && !look)
					continue;
				r.resting = false;
				r.probing = !retitled && !came_back;
				if (retitled)
					tried.title_ends++;
				if (!r.probing) {
					// (Something was done: quick looks again after this try.)
					r.probes = 0;
					r.gap = kProbeEvery;
					obs_log(LOG_INFO, "Auto Capture: trying %s again (%s)", w.executable.c_str(),
						retitled ? "its window's title changed" : "it is in front again");
				}
			}
			if (mode == Mode::AnyGame && w.looks_like_game && known_games_.insert(exe).second) {
				learned_something = true;
				// For the log: from here on this program is picked up at once.
				obs_log(LOG_INFO, "Auto Capture: %s is recognised as a game and remembered",
					exe.c_str());
			}
			const bool known = mode == Mode::AnyGame && known_games_.count(exe) > 0;
			// (Nor a window that was open when the game's window of its
			// program closed: the emulator's menu is no game to hook.)
			const bool spent = is_spent(exe, w.handle);
			const bool candidate = (listed || known) && !spent;
			if (w.foreground && candidate)
				in_front = &w;
			// (While the game's window may only be being made again, the
			// capture stays where it is.)
			if ((!spent || captured_.gone_since >= 0.0) && !applied_key_.empty() &&
			    key_of(w) == applied_key_)
				current = &w;
			// (Or another window of the program aimed at: its window made
			// again under another class, aimed at in this very tick. The one
			// in front first.)
			if (!aimed.empty() && exe == aimed && candidate && (!same_program || w.foreground))
				same_program = &w;
			if (listed && !spent && !listed_open)
				listed_open = &w;
			if (known && !spent && !known_open)
				known_open = &w;
		}
		// The program aimed at shows no window that can be seen (all
		// minimised, hidden or being made again: an emulator going
		// full-screen, starting or stopping a game), and is still running,
		// or was a moment ago: the capture stays aimed at it, and OBS hooks
		// it again by itself when it draws. Aimed elsewhere ("any
		// full-screen application"), OBS starts the capture again twice and
		// the game waits for both. The owner (2026-11-19): "auto capture
		// still takes a while to grab some windowed games". Only another
		// candidate clicked into takes over while it runs, and for
		// `kKeepGrace` after it was last seen running (a program making its
		// one window again has none for a moment); after that an open
		// candidate does, or the usual fallback. A game whose window closed
		// with its program still showing one, or still running with none to
		// be seen (`spent_`), is let go of as before.
		Window kept_window;
		const Window *kept = nullptr;
		bool kept_running = false;
		if (game_capture && !aimed.empty() && !aimed_listed && !spent_.count(aimed) &&
		    (aimed_running || clock_s_ - keep_seen_s_ < kKeepGrace - 1e-6)) {
			obs_data_t *settings = obs_source_get_settings(parent_);
			const std::string title = window_title_of(obs_data_get_string(settings, "window"));
			obs_data_release(settings);
			const std::string t = lower(title);
			const bool listed = matches(aimed, always) || matches(t, always);
			const bool excluded = !listed && (matches(aimed, never) || matches(t, never) ||
							  built_in_never(aimed) || app_capture_lists(aimed, t));
			const bool known = mode == Mode::AnyGame && known_games_.count(aimed) > 0;
			if (!excluded && (listed || known)) {
				kept_window.title = title;
				kept_window.window_class = applied_key_.substr(0, key_colon);
				kept_window.executable = applied_key_.substr(key_colon + 1);
				kept = &kept_window;
				kept_running = aimed_running;
			}
		}
		const Window *chosen = in_front       ? in_front
				       : current      ? current
				       : same_program ? same_program
				       : kept         ? kept
				       : listed_open  ? listed_open
						      : known_open;
		// Every title the chosen program shows while the capture is aimed at
		// it has been tried: none of them ends a rest later.
		if (chosen && game_capture) {
			const std::string exe = lower(chosen->executable);
			Tried &tried = tried_[exe];
			for (const Window &w : windows)
				if (lower(w.executable) == exe && tried.shapes.size() < kTriedMost)
					tried.shapes.insert(title_shape(w.title));
		}
		// A quick look in on a resting program is not a game being there
		// (for the Standby Picture): it would come and go every few seconds.
		bool probing = false;
		const std::string chosen_exe = chosen ? lower(chosen->executable) : std::string();
		for (auto &[exe, r] : rest_) {
			if (!r.probing)
				continue;
			if (exe == chosen_exe) {
				probing = true;
			} else {
				// Its look came while another game is captured: no look
				// now, the next one a gap later.
				r.probing = false;
				r.resting = true;
				r.probe_at = clock_s_ + r.gap;
			}
		}
		if (learned_something) {
			std::string text;
			for (const std::string &exe : known_games_)
				text += (text.empty() ? "" : ", ") + exe;
			obs_data_t *own = obs_source_get_settings(source_);
			obs_data_set_string(own, "known_games", text.c_str());
			obs_data_release(own);
		}

		// (Kept on its program is a game being there: the Standby Picture
		// stands in by itself while the capture has no picture.)
		standby_step(chosen != nullptr && !probing, step);

		// The log says when the capture stays aimed at a program whose
		// picture has gone (OBS freed its hook: the game made its window or
		// its drawing again), once each time, and once more when no window
		// of the program can be seen.
		if (game_capture && chosen && !probing && !aimed.empty() && lower(chosen->executable) == aimed) {
			if (obs_source_get_base_width(parent_) > 0) {
				had_picture_ = true;
				keep_said_ = 0;
			} else if (const int said = chosen == kept ? 2 : had_picture_ ? 1 : 0; said > keep_said_) {
				if (said == 2)
					obs_log(LOG_INFO,
						"Auto Capture: '%s' kept on %s while its picture comes back (no window of it can be seen; %s)",
						obs_source_get_name(parent_), chosen->executable.c_str(),
						kept_running ? "it is still running" : "it was running a moment ago");
				else
					obs_log(LOG_INFO, "Auto Capture: '%s' kept on %s while its picture comes back",
						obs_source_get_name(parent_), chosen->executable.c_str());
				keep_said_ = said;
			}
		}

		// Emulators put the frame rate in their title, so what counts as
		// "a different window" is the program and window class only.
		// With nothing to capture while a program rests, a Game Capture is
		// aimed at no window at all: left aimed at the program, OBS goes on
		// trying to hook it about twice a second (the owner's Dolphin, 78
		// tries in 90 s), and "any full-screen application" would hook it
		// too, emulators being full-screen.
		// (A program whose game's window has closed is aimed away from the
		// same way: hooked again, it would keep its last frame.)
		std::string resting = !chosen && game_capture && !spent_.empty() ? spent_.begin()->first
										 : std::string();
		const std::string closed = resting;
		if (!chosen && game_capture)
			for (const auto &[exe, rest] : rest_)
				if (rest.resting) {
					resting = exe;
					break;
				}
		std::string key = chosen ? key_of(*chosen) : !resting.empty() ? kRestKey : std::string();
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
			// A try gets the whole patience (the owner's Ryujinx gave its
			// first picture seven seconds after being aimed at, and tried
			// again for five it was given up on three times running); a
			// look in during a rest gets `kProbeFor`.
			auto rest_it = rest_.find(exe);
			const bool look = rest_it != rest_.end() && rest_it->second.probing;
			const double patience = look ? probe_for_s_ : give_up_s_;
			bool rested_now = false;
			if (!mine || obs_source_get_base_width(parent_) > 0) {
				unhooked_s_ = 0.0;
				if (chosen && obs_source_get_base_width(parent_) > 0 && rest_it != rest_.end()) {
					if (look)
						obs_log(LOG_INFO,
							"Auto Capture: '%s' has a picture from %s, looked in on while it rested",
							obs_source_get_name(parent_), chosen->executable.c_str());
					rest_.erase(rest_it); // it can be hooked after all
					rest_said_ = false;
				}
			} else if ((!use_test || give_up_in_tests_) && (unhooked_s_ += step) >= patience - 1e-6) {
				unhooked_s_ = 0.0;
				Rest &rest = rest_[exe];
				if (look) {
					// Nothing yet: the next look a gap later, a longer gap
					// once many have come to nothing.
					if (++rest.probes > kQuickProbes)
						rest.gap = std::min(rest.gap * 1.5, probe_most_s_);
				} else {
					rest.probes = 0;
					rest.gap = probe_every_s_;
					rest.away = false;
					obs_log(LOG_INFO,
						"Auto Capture: '%s' got no picture from %s in %.0f s; leaving it alone, looking in for %.1f s every %.0f s, and trying again when its window's title changes or it is clicked into",
						obs_source_get_name(parent_), chosen->executable.c_str(), patience,
						probe_for_s_, probe_every_s_);
				}
				rest.gap = std::max(rest.gap, probe_every_s_);
				rest.probe_at = clock_s_ + rest.gap;
				rest.resting = true;
				rest.probing = false;
				rested_now = true;
			}
			if (!rested_now)
				return;
			// Rested: aimed away in this very tick, not the next.
			chosen = nullptr;
			resting = exe;
			key = kRestKey;
			std::lock_guard lock(mutex_);
			current_.clear();
		}
		// Another window of the program aimed at (made again under another
		// class, or another of its windows in front) while the capture has
		// its picture: OBS's hook is in the program, not the window, and
		// writing the capture's settings would start the capture again,
		// the picture gone for a moment. Taken as it is. (With no picture
		// it is aimed at in this very tick, below.)
		if (aimed_once_ && chosen && game_capture && !aimed.empty() && lower(chosen->executable) == aimed &&
		    obs_source_get_base_width(parent_) > 0) {
			obs_log(LOG_INFO,
				"Auto Capture: '%s' stays on %s, whose window changed: its picture is there, so the capture is not started again",
				obs_source_get_name(parent_), chosen->executable.c_str());
			applied_key_ = key;
			unhooked_s_ = 0.0;
			return;
		}
		aimed_once_ = true;
		unhooked_s_ = 0.0;
		if (!chosen && key == kRestKey && !closed.empty()) {
			// No window, as for a rest, but not one.
			obs_data_t *settings = obs_source_get_settings(parent_);
			obs_data_set_string(settings, "capture_mode", "window");
			obs_data_set_string(settings, "window", kNoWindow);
			obs_data_set_int(settings, "priority", 2);
			obs_log(LOG_INFO, "Auto Capture: '%s' aimed at no window while %s shows no game window",
				obs_source_get_name(parent_), closed.c_str());
			obs_source_update(parent_, settings);
			obs_data_release(settings);
			applied_key_ = key;
			return;
		}

		obs_data_t *settings = obs_source_get_settings(parent_);
		bool changed = false;
		if (chosen) {
			// (A look in during a rest is not news for the log, nor the end
			// of the rest's one line.)
			const auto r = rest_.find(lower(chosen->executable));
			const bool look = r != rest_.end() && r->second.probing;
			if (!look)
				rest_said_ = false;
			const std::string window = encode_part(chosen->title) + ":" +
						   encode_part(chosen->window_class) + ":" +
						   encode_part(chosen->executable);
			obs_data_set_string(settings, "window", window.c_str());
			obs_data_set_int(settings, "priority", 2); // match by executable
			if (game_capture)
				obs_data_set_string(settings, "capture_mode", "window");
			if (!look)
				obs_log(LOG_INFO, "Auto Capture: '%s' now follows %s", obs_source_get_name(parent_),
					chosen->executable.c_str());
			changed = true;
		} else if (!resting.empty()) {
			obs_data_set_string(settings, "capture_mode", "window");
			obs_data_set_string(settings, "window", kNoWindow);
			obs_data_set_int(settings, "priority", 2);
			// (Once a rest: the looks in on it come and go without a word.)
			if (!rest_said_) {
				obs_log(LOG_INFO,
					"Auto Capture: '%s' aimed at no window while %s rests, so nothing is hooked",
					obs_source_get_name(parent_), resting.c_str());
				rest_said_ = true;
			}
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
		if (hotkey_ == OBS_INVALID_HOTKEY_ID && parent)
			register_hotkey(parent);
		// Whatever the source is aimed at now is not something we chose.
		aimed_once_ = false;
		applied_key_.clear();
		captured_ = Captured{};
		spent_.clear();
		listen(parent);
	}
	// Registers the hide hotkey under the filter's current name, in place of
	// the one there was (a rename keeps its bindings: `renamed_`).
	void register_hotkey(obs_source_t *parent)
	{
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		hotkey_ = OBS_INVALID_HOTKEY_ID;
		if (!parent)
			return;
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
		unlisten();
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
		windows.erase(std::remove_if(windows.begin(), windows.end(), [](const Window &window) {
			return built_in_never(lower(window.executable));
		}), windows.end());
		return windows;
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
	// OBS's Game Capture says when it starts capturing ("hooked") and stops
	// ("unhooked"), counted in `hook_events_`: a picture that went and came
	// back between two looks of the tick (OBS can hook a game again in a
	// tenth of a second) is a new one, not the last frame of a game whose
	// window closed. Listened to only while on a Game Capture.
	static void on_hook_event(void *data, calldata_t *) { static_cast<AutoCapture *>(data)->hook_events_++; }
	void listen(obs_source_t *parent)
	{
		unlisten();
		const char *id = parent ? obs_source_get_unversioned_id(parent) : nullptr;
		if (!id || strcmp(id, "game_capture") != 0)
			return;
		signal_handler_t *sh = obs_source_get_signal_handler(parent);
		signal_handler_connect(sh, "hooked", on_hook_event, this);
		signal_handler_connect(sh, "unhooked", on_hook_event, this);
		listened_ = parent;
	}
	void unlisten()
	{
		if (!listened_)
			return;
		signal_handler_t *sh = obs_source_get_signal_handler(listened_);
		signal_handler_disconnect(sh, "hooked", on_hook_event, this);
		signal_handler_disconnect(sh, "unhooked", on_hook_event, this);
		listened_ = nullptr;
	}

	// Whether a window was open when the game's window of its program
	// closed (and is no game to aim at while the program stays open).
	bool is_spent(const std::string &exe, uint64_t handle) const
	{
		const auto it = spent_.find(exe);
		return it != spent_.end() && handle && it->second.count(handle);
	}

	// The windows the capture got its picture from. OBS's Game Capture hooks
	// the program, not the window: when an emulator's game window closes
	// and its menu stays open, the hook keeps the last frame and the capture
	// its size, so a Standby Picture never sees nothing. The owner
	// (2026-11-18): "if the captured game closes but the emulator stays open
	// in the menu/game list, the last captured frame of the game remains in
	// the OBS capture, and it doesn't morph into the standby as intended".
	// The game's windows are those of the program that were not open when
	// the capture was aimed at it (`before_`: the menu was), Dolphin's GBA
	// windows among them; failing any, the program's window in front or the
	// one aimed at by title. Once every one has been gone for `kLetGoAfter`
	// with the program open, the capture lets go (aimed at no window, below)
	// and the program's windows seen meanwhile are no game until it closes.
	// A window that opens in that time is followed instead (a switch to full
	// screen makes the window again; a game started at once makes a new
	// one), and any other new window is chosen as usual.
	void watch_captured(const std::vector<Window> &windows, const std::set<std::string> &running,
			    const std::set<uint64_t> &unlisted, bool use_test)
	{
		constexpr double kLetGoAfter = 0.5;
		// A window that is there but not in the list (minimised or hidden):
		// asked of Windows itself, in the tests a stand-in marked hidden.
		auto exists = [&](uint64_t h) {
#ifdef _WIN32
			if (!use_test)
				return IsWindow((HWND)(uintptr_t)h) != FALSE;
#endif
			return unlisted.count(h) > 0;
		};
		// (A picture can come before the window list shows the game's
		// window: a moment to see it.)
		constexpr double kPickAfter = 0.5;
		for (auto it = spent_.begin(); it != spent_.end();) {
			bool open = running.count(it->first) > 0;
#ifdef _WIN32
			// (Minimised, the program's windows are left out of the list.)
			if (!open && !use_test)
				open = std::any_of(it->second.begin(), it->second.end(),
						   [](uint64_t h) { return IsWindow((HWND)(uintptr_t)h) != FALSE; });
#endif
			it = open ? std::next(it) : spent_.erase(it);
		}
		const size_t colon = applied_key_.rfind(':');
		const std::string aimed = applied_key_.empty() || applied_key_ == kRestKey || colon == std::string::npos
						  ? std::string()
						  : lower(applied_key_.substr(colon + 1));
		auto of_it = [&](const Window &w) {
			return w.handle && lower(w.executable) == aimed;
		};
		if (aimed != before_exe_) {
			before_.clear();
			before_exe_ = aimed;
			picture_since_ = -1.0;
			for (const Window &w : windows)
				if (of_it(w))
					before_.insert(w.handle);
		}
		if (!captured_.handles.empty() && captured_.exe != aimed)
			captured_ = Captured{};
		if (captured_.handles.empty()) {
			if (aimed.empty() || obs_source_get_base_width(parent_) == 0) {
				picture_since_ = -1.0;
				return;
			}
			if (picture_since_ < 0.0)
				picture_since_ = clock_s_;
			Captured found;
			for (const Window &w : windows)
				if (of_it(w) && !before_.count(w.handle) && !is_spent(aimed, w.handle))
					found.handles.insert(w.handle);
			if (found.handles.empty()) {
				if (clock_s_ - picture_since_ < kPickAfter - 1e-6)
					return;
				// The game was open when the capture was aimed at it.
				obs_data_t *settings = obs_source_get_settings(parent_);
				const std::string title = window_title_of(obs_data_get_string(settings, "window"));
				obs_data_release(settings);
				const Window *pick = nullptr;
				for (const Window &w : windows) {
					if (!of_it(w))
						continue;
					if (w.foreground) {
						pick = &w;
						break;
					}
					if (!pick || (w.title == title && pick->title != title))
						pick = &w;
				}
				if (!pick)
					return;
				found.handles.insert(pick->handle);
			}
			found.exe = aimed;
			found.hooks = hook_events_.load();
			for (const Window &w : windows)
				if (of_it(w)) {
					found.seen.insert(w.handle);
					if (found.handles.count(w.handle))
						found.shapes.insert(title_shape(w.title));
				}
			captured_ = found;
			return;
		}
		bool open = false;
		std::set<uint64_t> there;
		for (const Window &w : windows) {
			if (lower(w.executable) != captured_.exe)
				continue;
			open = true;
			if (captured_.handles.count(w.handle))
				there.insert(w.handle);
		}
		// None of its windows listed: the program closed, or OBS let go of
		// it (no picture, or a new one since: OBS hooked it again between
		// two looks), the program making its window again: the capture is
		// kept on it in the tick and watched afresh once a window shows.
		// Still running with the same picture there, its windows are
		// minimised or hidden: a window of the game still there (hidden
		// while it makes its drawing again) is the game, and the game's own
		// windows all gone (the menu minimised) is let go of below as with
		// the menu listed. Kept on, OBS would show the game's last frame for
		// as long as the program runs.
		if (!open) {
			if (!running.count(captured_.exe) || obs_source_get_base_width(parent_) == 0 ||
			    hook_events_.load() != captured_.hooks) {
				captured_ = Captured{};
				return;
			}
			for (uint64_t h : captured_.handles)
				if (exists(h))
					there.insert(h);
		}
#ifdef _WIN32
		// Minimised it is left out of the list of windows, and still there.
		if (!use_test)
			for (uint64_t h : captured_.handles) {
				const HWND window = (HWND)(uintptr_t)h;
				if (IsWindow(window) && IsWindowVisible(window))
					there.insert(h);
			}
#else
		(void)use_test;
#endif
		if (!there.empty()) {
			// (One of several closing, a GBA window, is not the game.)
			captured_.handles = there;
			for (const Window &w : windows)
				if (w.handle && lower(w.executable) == captured_.exe)
					captured_.seen.insert(w.handle);
			captured_.gone_since = -1.0;
			captured_.hooks = hook_events_.load();
			return;
		}
		// A new window meanwhile: under the same title first.
		const Window *again = nullptr;
		for (const Window &w : windows) {
			if (!w.handle || lower(w.executable) != captured_.exe || captured_.seen.count(w.handle))
				continue;
			if (!again || (captured_.shapes.count(title_shape(w.title)) &&
				       !captured_.shapes.count(title_shape(again->title))))
				again = &w;
		}
		if (again) {
			obs_log(LOG_INFO,
				"Auto Capture: '%s': the window it captured in %s was made again; following it",
				obs_source_get_name(parent_), captured_.exe.c_str());
			captured_.handles = {again->handle};
			captured_.shapes = {title_shape(again->title)};
			captured_.seen.insert(again->handle);
			captured_.gone_since = -1.0;
			captured_.hooks = hook_events_.load();
			return;
		}
		if (captured_.gone_since < 0.0)
			captured_.gone_since = clock_s_;
		if (clock_s_ - captured_.gone_since < kLetGoAfter - 1e-6)
			return;
		obs_log(LOG_INFO, "Auto Capture: '%s': the window it captured in %s has closed; letting go",
			obs_source_get_name(parent_), captured_.exe.c_str());
		// (Only windows seen while the game was: one opened since is no
		// menu.)
		std::set<uint64_t> &left = spent_[captured_.exe];
		for (const Window &w : windows)
			if (w.handle && lower(w.executable) == captured_.exe && captured_.seen.count(w.handle))
				left.insert(w.handle);
		// (And those seen then that are there unlisted: a menu minimised
		// is no game either when it is brought back.)
		for (uint64_t h : captured_.seen)
			if (h && !captured_.handles.count(h) && exists(h))
				left.insert(h);
		captured_ = Captured{};
	}

	// The Standby Picture over the capture: while it is hidden, and (asked
	// for) while there is no game. Through the Standby Picture filter on the
	// capture, with this filter's transition.
	void standby_step(bool have_game, double step)
	{
		bool no_game, use_standby;
		int style, ms, curve, mode;
		const bool toggle = cmd_hide_.exchange(false);
		HidChange hide_change;
		{
			std::lock_guard lock(mutex_);
			no_game = no_game_standby_;
			use_standby = hide_standby_;
			if (toggle)
				hide_change = hide_change_;
			style = cover_style_;
			ms = cover_ms_;
			curve = cover_curve_;
			mode = cover_mode_;
		}
		if (toggle) {
			// Covered by the command: uncovered. Items it hid: those shown
			// again, and only those. Otherwise covered with the Standby
			// Picture, or its items hidden and written down.
			// (Asking for the state it already has only says whether there
			// is a Standby Picture to hide behind.)
			if (hidden_)
				hidden_ = false; // (uncovered below)
			else if (!hid_pending_ && use_standby &&
				 standby_cover_set(parent_, covered_, style, ms, curve, mode))
				hidden_ = true;
			else
				hid_pending_ = hid_items_press(source_, parent_, hide_change, "Auto Capture");
			state_s_ = 1.0; // (said at once)
		}
		// For a Stream Deck key showing shown or hidden (once a second: the
		// scenes are gone through for it). Items the command hid and has
		// not shown again read hidden.
		if ((state_s_ += step) >= 1.0) {
			state_s_ = 0.0;
			vendor_switch_state(source_, false, hidden_ || hid_pending_ || shown_nowhere(parent_));
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

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	bool no_game_standby_ = false;
	bool hide_standby_ = true;
	HidChange hide_change_;
	int cover_style_ = 0, cover_ms_ = 600, cover_curve_ = 0, cover_mode_ = 0;
	std::atomic<bool> cmd_hide_{false};
	bool hidden_ = false, covered_ = false; // used on the tick only
	std::atomic<bool> hid_pending_{false};  // items hidden by the command, to show again
	double state_s_ = 1.0;                  // since the state was last reported
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
	HotkeyRename renamed_{source_, [this] { return std::vector<obs_hotkey_id *>{&hotkey_}; },
			      [this] { register_hotkey(obs_filter_get_parent(source_)); }};
	Mode mode_ = Mode::AnyGame;
	std::vector<std::string> always_, never_;
	bool fallback_fullscreen_ = true;
	std::atomic<bool> keep_ready_{true};
	bool holding_ = false; // used on the tick only
	double timer_ = 0.0;
	std::vector<Window> test_windows_;
	std::set<std::string> test_running_;
	std::set<uint64_t> test_unlisted_;
	std::atomic<uint64_t> hook_events_{0};
	obs_source_t *listened_ = nullptr; // (only while attached to it)
	bool use_test_windows_ = false;
	// The program a hook by hand was handed (lower case; empty: none), and
	// how its watch goes (`hand_watch`).
	std::string hand_exe_;
	bool hand_seen_ = false;
	double hand_wait_s_ = 0.0, hand_timer_ = 0.0;
	std::string applied_key_;
	// Programs seen to be games, by executable name in lower case. Only the tick uses it.
	std::set<std::string> known_games_;
	bool known_loaded_ = false;
	std::string current_;
	// The window the capture got its picture from (`watch_captured`). Only
	// the tick uses it.
	struct Captured {
		std::set<uint64_t> handles;   // the game's windows
		std::string exe;              // lower case
		std::set<std::string> shapes; // their titles, `title_shape`
		std::set<uint64_t> seen;      // the program's windows while they were there
		double gone_since = -1.0;     // by clock_s_
		uint64_t hooks = 0;           // `hook_events_` when they were last there
	};
	Captured captured_;
	// The program's windows when the capture was first aimed at it (the
	// menu), and since when it has had a picture with no game window known.
	std::set<uint64_t> before_;
	std::string before_exe_;
	double picture_since_ = -1.0;
	// Programs whose game's window closed, still open: the windows open
	// then, which are no game. Only the tick uses it.
	std::map<std::string, std::set<uint64_t>> spent_;
	bool aimed_once_ = false;
	double unhooked_s_ = 0.0; // how long the capture has been aimed with no picture
	double give_up_s_ = 10.0; // and how long that may last
	// Programs the capture got no picture from, by executable in lower case,
	// left alone for now. Never saved. Only the tick uses it.
	struct Rest {
		double probe_at = 0.0; // by clock_s_: when it is next looked in on
		double gap = 0.0;      // between the end of one look and the next
		int probes = 0;        // looks that came to nothing since the last try
		bool resting = false;
		bool probing = false; // aimed at for a look in, not a whole try
		bool away = false;    // seen behind another window since it was given up on
	};
	std::map<std::string, Rest> rest_;
	// Per program while it is open: the titles (`title_shape`) it showed
	// while the capture was aimed at it, and how many rests a new title
	// ended. Kept until it closes, a picture or not.
	struct Tried {
		std::set<std::string> shapes;
		int title_ends = 0;
	};
	std::map<std::string, Tried> tried_;
	bool rest_said_ = false; // "aimed at no window" logged for this rest
	double probe_every_s_ = kProbeEvery, probe_for_s_ = kProbeFor, probe_most_s_ = kProbeEveryMost;
	double clock_s_ = 0.0;
	double since_s_ = 0.0;   // since the last look (the clock's step when not in the tests)
	uint64_t seen_look_ = 0; // the window watch's look acted on last
	// The program the capture is aimed at (lower case), when it was last
	// seen running (by clock_s_), whether its picture has been there, and
	// what the log has said of its picture going (1 kept, 2 no window).
	// Only the tick uses them.
	std::string keep_exe_;
	double keep_seen_s_ = 0.0;
	bool had_picture_ = false;
	int keep_said_ = 0;
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

// The Standby Image's transition is only used by the two standby options:
// with neither ticked its controls do nothing and are hidden. Only
// visibility changes here.
bool standby_modified(void *data, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool used = obs_data_get_bool(settings, "no_game_standby") || obs_data_get_bool(settings, "hide_standby");
	obs_property_set_visible(obs_properties_get(props, "standby_style"), used);
	const int style = (int)obs_data_get_int(settings, "standby_style");
	for (const char *name : {"standby_ms", "standby_ease_curve", "standby_ease_mode"})
		obs_property_set_visible(obs_properties_get(props, name), used && style != -1 && style != 1);
	auto *capture = static_cast<AutoCapture *>(data);
	obs_source_t *parent = capture ? obs_filter_get_parent(capture->source()) : nullptr;
	const bool hide_items = !obs_data_get_bool(settings, "hide_standby") || !parent || !standby_present(parent);
	obs_property_set_visible(obs_properties_get(props, "item_hide_group"), hide_items);
	const std::string kind = obs_data_get_string(settings, "transition");
	obs_property_set_visible(obs_properties_get(props, "transition_ms"), kind != kTransitionKeep && kind != kTransitionCut);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<AutoCapture *>(data);
	obs_properties_t *props = obs_properties_create();

	// What it is doing right now, and one press to undo a wrong choice.
	obs_property_t *status = obs_properties_add_text(props, "status", "", OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);
	obs_property_t *never_now = obs_properties_add_button2(
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
	set_tip(never_now, obs_module_text("Capture.NeverNow.Tip"));
	obs_property_t *check = obs_properties_add_button2(
		props, "check", obs_module_text("Capture.Check"),
		[](obs_properties_t *p, obs_property_t *, void *d) {
			show_status(p, static_cast<AutoCapture *>(d));
			return true;
		},
		data);
	set_tip(check, obs_module_text("Capture.Check.Tip"));

	// Games: the two lists, each filled by choosing from the programs that
	// are open, and how games are found.
	obs_properties_t *games = obs_properties_create();
	obs_source_t *own = self ? self->source() : nullptr;
	auto open_now = [self](std::vector<std::string> &names) {
		if (!self)
			return;
		const auto windows = self->open_programs();
		for (const Window &w : windows)
			if (!w.executable.empty() && std::find(names.begin(), names.end(), w.executable) == names.end())
				names.push_back(w.executable);
		for (const Window &w : windows)
			add_window_title_choices(names, w.title);
	};
	PickList always;
	always.setting = "always";
	always.as_text = true;
	always.title = "Capture.Always";
	always.add = "List.AddProgram";
	always.tip = "Capture.Always.Tip";
	always.choices = open_now;
	pick_list_add(games, own, always);
	PickList never;
	never.setting = "never";
	never.as_text = true;
	never.title = "Capture.Never";
	never.add = "List.AddProgram";
	never.tip = "Capture.Never.Tip";
	never.choices = open_now;
	pick_list_add(games, own, never);
	obs_property_t *mode = obs_properties_add_list(games, "mode", obs_module_text("Capture.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Capture.Mode.AnyGame"), (int)Mode::AnyGame);
	obs_property_list_add_int(mode, obs_module_text("Capture.Mode.ListedOnly"), (int)Mode::ListedOnly);
	set_tip(mode, obs_module_text("Capture.Mode.Tip"));
	obs_property_t *keep_ready = obs_properties_add_bool(games, "keep_ready", obs_module_text("Capture.KeepReady"));
	set_tip(keep_ready, obs_module_text("Capture.KeepReady.Tip"));
	obs_properties_add_group(props, "games_group", obs_module_text("Capture.Lists"), OBS_GROUP_NORMAL, games);

	// When there is no game.
	obs_properties_t *no_game_group = obs_properties_create();
	obs_property_t *fallback =
		obs_properties_add_bool(no_game_group, "fallback_fullscreen", obs_module_text("Capture.Fallback"));
	set_tip(fallback, obs_module_text("Capture.Fallback.Tip"));
	obs_property_t *no_game =
		obs_properties_add_bool(no_game_group, "no_game_standby", obs_module_text("Capture.NoGameStandby"));
	set_tip(no_game, obs_module_text("Capture.NoGameStandby.Tip"));
	obs_property_set_modified_callback2(no_game, standby_modified, self);
	obs_properties_add_group(props, "no_game_group", obs_module_text("Capture.NoGameGroup"), OBS_GROUP_NORMAL,
				 no_game_group);

	// Hiding and showing the capture, in the Window Switcher's group and
	// order: the Standby Image over it, its transition, and the button last.
	obs_properties_t *standby = obs_properties_create();
	obs_property_t *hide_standby =
		obs_properties_add_bool(standby, "hide_standby", obs_module_text("AppCapture.HideStandby"));
	set_tip(hide_standby, obs_module_text("Capture.HideStandby.Tip"));
	obs_property_set_modified_callback2(hide_standby, standby_modified, self);
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
	obs_property_list_add_int(style, obs_module_text("Capture.StandbyOwn"), -1);
	set_tip(style, obs_module_text("Capture.StandbyStyle.Tip"));
	obs_property_set_modified_callback2(style, standby_modified, self);
	obs_property_t *ms =
		obs_properties_add_int_slider(standby, "standby_ms", obs_module_text("Change.Time"), 0, 5000, 50);
	obs_property_int_set_suffix(ms, " ms");
	set_tip(ms, obs_module_text("Capture.StandbyMs.Tip"));
	add_easing_properties(standby, "standby_");
	obs_properties_t *item_hide = obs_properties_create();
	add_item_transition_properties(item_hide);
	obs_properties_add_group(standby, "item_hide_group", obs_module_text("Capture.ItemHide"), OBS_GROUP_NORMAL, item_hide);
	obs_property_t *hide_now = obs_properties_add_button2(
		standby, "hide_now", obs_module_text("AppCapture.Hide.Now"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			if (d)
				static_cast<AutoCapture *>(d)->ask_hide();
			return false;
		},
		data);
	set_tip(hide_now, obs_module_text("Capture.HideNow.Tip"));
	obs_properties_add_group(props, "standby", obs_module_text("AppCapture.Hide"), OBS_GROUP_NORMAL, standby);

	add_help_line(props, obs_module_text("Capture.Help"));
	show_status(props, self);
	return props;
}

} // namespace

void add_window_title_choices(std::vector<std::string> &names, const std::string &title)
{
	auto add = [&](const std::string &value) {
		if (!value.empty() && std::find(names.begin(), names.end(), value) == names.end())
			names.push_back(value);
	};
	add(title);
	for (const char *separator : {" | ", " - "}) {
		const size_t at = title.find(separator);
		if (at != std::string::npos && at >= 3)
			add(title.substr(0, at));
	}
}

std::vector<OpenWindow> open_windows()
{
	std::vector<OpenWindow> out;
	for (const Window &w : g_window_watch.latest())
		out.push_back({w.title, w.executable, w.foreground, w.window_class, w.handle, w.game_area});
	return out;
}

void game_area_want(uint64_t handle)
{
	if (!handle)
		return;
	std::lock_guard lock(g_area_mutex);
	g_area_wanted[handle] = os_gettime_ns();
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

void aim_capture_at_nothing(obs_source_t *capture)
{
	const char *id = capture ? obs_source_get_unversioned_id(capture) : nullptr;
	if (!id)
		return;
	obs_data_t *settings = obs_source_get_settings(capture);
	obs_data_set_string(settings, "window", kNoWindow);
	obs_data_set_int(settings, "priority", 2); // by a program no window has
	if (strcmp(id, "game_capture") == 0)
		obs_data_set_string(settings, "capture_mode", "window");
	obs_source_update(capture, settings);
	obs_data_release(settings);
}

bool capture_aimed_at_nothing(obs_source_t *capture)
{
	if (!capture)
		return false;
	obs_data_t *settings = obs_source_get_settings(capture);
	const bool nothing = strcmp(obs_data_get_string(settings, "window"), kNoWindow) == 0;
	obs_data_release(settings);
	return nothing;
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
	// An Auto Capture here would aim the capture somewhere else again: it
	// is switched off, with the program it was handed to written down, so
	// it takes the capture back when that program closes (`hand_watch`).
	// One switched off by the user is left as it is; one this hook had
	// switched off already is handed the new program.
	const std::string handed = lower(front.executable);
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *kind = obs_source_get_unversioned_id(filter);
			if (!kind || strcmp(kind, kAutoCaptureId) != 0)
				return;
			obs_data_t *settings = obs_source_get_settings(filter);
			const bool had = *obs_data_get_string(settings, "hand_hooked") != '\0';
			obs_data_release(settings);
			const bool on = obs_source_enabled(filter);
			if (!on && !had)
				return;
			if (on)
				obs_source_set_enabled(filter, false);
			obs_data_t *record = obs_data_create();
			obs_data_set_string(record, "hand_hooked", static_cast<const std::string *>(param)->c_str());
			obs_source_update(filter, record);
			obs_data_release(record);
		},
		const_cast<std::string *>(&handed));
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
		obs_data_set_default_string(settings, "transition", kTransitionKeep);
	obs_data_set_default_int(settings, "transition_ms", 300);
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
		skip_filter_input(static_cast<AutoCapture *>(data)->source());
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
