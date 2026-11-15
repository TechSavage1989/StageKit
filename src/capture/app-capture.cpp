/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Capture App in Use: one Window Capture for several apps.
//
// The owner had a Window Capture for each app and a scene filter showing
// the one in use: "I'd like to have a single Window capture, and a new
// filter added that can have multiple apps associated with the Window
// capture and effectively emulate my current process and visibility
// functionality based on the open and captured apps, but with only a single
// window capture source."
//
// The filter sits on the Window Capture and aims it, by the rules of the
// scene filter it stands in for:
//
//  - An app on the list that opens is taken.
//  - With "Follow the app in use", clicking into one of the apps takes it.
//    Clicking into anything else changes nothing: the last one stays.
//  - When the app being captured closes, the one used before it comes back.
//
// It acts only when one of those things happens, so a capture aimed by hand
// stays until the next one; the Capture Front Window key switches the filter
// off, and switching it on again hands the capture back.
//
// One capture cannot show two windows at once, so the change between them is
// drawn here. The filter keeps the last picture the capture had; while the
// capture has none (it is being aimed at the other window, or its app has
// just closed) that picture stays, and the new one comes in over it in the
// way chosen. Standby Picture is told meanwhile (`app_capture_holding`), or
// it would stand in for that moment.
//
// Finding windows uses the Windows API (shared with Auto Capture). On other
// systems the filter loads and does nothing but pass the picture on.

#include "app-capture.hpp"
#include "auto-capture.hpp"
#include "standby.hpp"
#include "../common/item-transition.hpp"
#include "../frame/auto-frame.hpp"
#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"
#include "../transition/morph-paint.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

// How the new picture comes in over the last one. Saved by number: new ones
// go at the end.
enum class Style : int {
	Fade = 0,
	Cut = 1,
	Zoom = 2, // fades in as it grows
	FromLeft = 3,
	FromRight = 4,
	FromTop = 5,
	FromBottom = 6,
	Morph = 7, // the frame changes shape and size as the one turns into the other
};

// How long the last picture is kept when the capture loses its own with
// nothing said about another window yet (its app has closed: which one to
// go back to is known a moment later).
constexpr double kLostFor = 0.8;
// After being aimed, a capture goes on showing its old window for a moment:
// the new picture is believed once the capture has been seen with none, or
// after this long.
constexpr double kSettle = 0.6;
// A capture kept ready stands in until the capture itself has let go of the
// old window and has had the new one's picture for this long, or, if it was
// never seen to let go, for this long after being aimed.
constexpr double kSteady = 0.25;
constexpr double kBridgeFor = 2.0;
// And if no picture comes at all, the wait ends.
constexpr double kWaitFor = 3.0;

std::string lowered(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

// One part of a capture's "window" setting, which OBS writes as
// title:class:executable with ':' as "#3A" and '#' as "#22".
std::string decoded(const std::string &part)
{
	std::string out;
	for (size_t i = 0; i < part.size(); i++) {
		if (part.compare(i, 3, "#3A") == 0) {
			out += ':';
			i += 2;
		} else if (part.compare(i, 3, "#22") == 0) {
			out += '#';
			i += 2;
		} else {
			out += part[i];
		}
	}
	return out;
}

// How well a window answers to an entry of the list: 2 by its program's
// file name, 1 by part of its title, 0 not at all.
int answers(const std::string &entry, const OpenWindow &w)
{
	const std::string e = lowered(entry);
	if (e.empty())
		return 0;
	if (lowered(w.executable) == e)
		return 2;
	return lowered(w.title).find(e) != std::string::npos ? 1 : 0;
}

// The size of what comes before a filter. For the capture itself that is its
// own size: asking for its width would come back to this filter.
uint32_t before_size(obs_source_t *filter, bool across)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	if (target == parent)
		return across ? obs_source_get_base_width(target) : obs_source_get_base_height(target);
	return across ? obs_source_get_width(target) : obs_source_get_height(target);
}

// Where a filter comes in its source's list, counted from the one applied first.
int applied_index(obs_source_t *parent, obs_source_t *filter)
{
	struct Find {
		obs_source_t *wanted;
		int at, found;
	} find{filter, 0, -1};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (f == x->wanted)
				x->found = x->at;
			x->at++;
		},
		&find);
	return find.found;
}

// Every scene item showing a source, inside groups too, each held.
std::vector<obs_sceneitem_t *> items_showing(obs_source_t *wanted)
{
	struct Find {
		obs_source_t *wanted;
		std::vector<obs_sceneitem_t *> out;
	} find{wanted, {}};
	static const auto visit = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *f = static_cast<Find *>(param);
		if (obs_sceneitem_get_source(item) == f->wanted) {
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

// What every enabled switcher lists, for Auto Capture to leave alone.
std::mutex g_listed_mutex;
std::map<const void *, std::vector<std::string>> g_listed;

class AppCapture : public MorphPaint {
public:
	AppCapture(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		last_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		next_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		update(settings);
		// For the tests: stand-in windows, one per line, "title|exe|front".
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_test_windows(in string windows)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AppCapture *>(data);
				const char *text = calldata_string(cd, "windows");
				std::vector<OpenWindow> windows;
				std::vector<std::string> fields(1);
				auto flush = [&] {
					fields.resize(3);
					if (!fields[1].empty())
						windows.push_back({fields[0], fields[1], fields[2] == "front", ""});
					fields.assign(1, std::string());
				};
				for (const char *p = text ? text : ""; *p; p++) {
					if (*p == '\n')
						flush();
					else if (*p == '|')
						fields.emplace_back();
					else
						fields.back() += *p;
				}
				flush();
				std::lock_guard lock(self->mutex_);
				self->test_windows_ = windows;
				self->use_test_windows_ = true;
				self->timer_ = 0.0;
			},
			this);
		// For the tests, which cannot press a hotkey: "next", "back", "hide".
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void command(in string what)",
			[](void *data, calldata_t *cd) {
				const char *what = calldata_string(cd, "what");
				static_cast<AppCapture *>(data)->command(what ? what : "");
			},
			this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void hold(in string title, in string executable)",
			[](void *data, calldata_t *cd) {
				OpenWindow window;
				window.title = calldata_string(cd, "title") ? calldata_string(cd, "title") : "";
				window.executable =
					calldata_string(cd, "executable") ? calldata_string(cd, "executable") : "";
				static_cast<AppCapture *>(data)->hold(window);
			},
			this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void ready_count(out int count)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AppCapture *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_int(cd, "count", (long long)self->ready_.size());
			},
			this);
	}

	~AppCapture()
	{
		{
			std::lock_guard lock(g_listed_mutex);
			g_listed.erase(this);
		}
		detach();
		keep_ready({});
		set_bridge(nullptr);
		obs_enter_graphics();
		gs_texrender_destroy(last_);
		gs_texrender_destroy(next_);
		obs_leave_graphics();
	}

	obs_source_t *source() const { return source_; }

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> apps = pick_list_read(settings, "apps", false);
		const std::vector<std::string> by_hand = pick_list_read(settings, "by_hand", false);
		{
			std::vector<std::string> all;
			for (const std::vector<std::string> *list : {&apps, &by_hand})
				for (const std::string &entry : *list)
					all.push_back(lowered(entry));
			std::lock_guard lock(g_listed_mutex);
			g_listed[this] = all;
		}
		std::lock_guard lock(mutex_);
		apps_ = apps;
		by_hand_ = by_hand;
		follow_ = obs_data_get_bool(settings, "follow");
		ready_wanted_ = obs_data_get_bool(settings, "ready");
		style_ = static_cast<Style>(std::clamp<long long>(obs_data_get_int(settings, "change"), 0, 7));
		change_s_ = std::clamp<long long>(obs_data_get_int(settings, "change_ms"), 0, 10000) / 1000.0;
		curve_ = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		mode_ = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		transition_ = item_transition_kind(settings);
		transition_ms_ = item_transition_ms(settings);
		started_ = false; // settings changed: work out what to capture afresh
		timer_ = 0.0;
	}

	// The hotkeys and keys: on to the next app switched to by hand, back to
	// following the apps, or the capture hidden and shown. Acted on at once.
	void command(const std::string &what)
	{
		std::lock_guard lock(mutex_);
		if (what == "next")
			cmd_step_ = true;
		else if (what == "back")
			cmd_back_ = true;
		else if (what == "hide")
			cmd_hide_ = true;
		else if (what == "toggle")
			// Between the app held by hand and following the apps: back
			// from one that is held, else on to the one held last.
			(held_.empty() ? cmd_step_ : cmd_back_) = true;
		timer_ = 0.0;
	}

	// A window picked by hand (the Capture Front Window key): it is held like
	// an app switched to, and its program joins that list so that it can be
	// gone back to.
	void hold(const OpenWindow &window)
	{
		std::lock_guard lock(mutex_);
		hold_window_ = window;
		have_hold_ = true;
		timer_ = 0.0;
	}

	uint64_t changes() const { return changes_.load(); }

	// The entry being shown, as far as this filter knows.
	std::string current()
	{
		std::lock_guard lock(mutex_);
		return current_;
	}

	// The app held by hand, if any.
	std::string held()
	{
		std::lock_guard lock(mutex_);
		return held_;
	}

	bool holding()
	{
		std::lock_guard lock(mutex_);
		return phase_ == Phase::Lost || phase_ == Phase::Wait || bridge_;
	}

	uint32_t size(bool across)
	{
		obs_source_t *bridge = bridge_ref();
		const uint32_t live = live_size(bridge, across);
		obs_source_release(bridge);
		std::lock_guard lock(mutex_);
		const uint32_t kept = across ? kept_w_ : kept_h_;
		if (phase_ == Phase::Lost || phase_ == Phase::Wait)
			return kept ? kept : live;
		if (phase_ == Phase::Mix) {
			if (!live || !kept)
				return live ? live : kept;
			if (style_ == Style::Cut)
				return live;
			// Its size is on the way too, whatever comes in over it: a size
			// that jumped at the start made the item jump on screen.
			const double t = std::clamp(ease(curve_, mode_, mix_), 0.0, 1.0);
			return (uint32_t)std::lround(kept + ((double)live - kept) * t);
		}
		return live;
	}

	void attach(obs_source_t *parent)
	{
		// Hotkeys on the capture, named after this filter, for the keyboard
		// or a Stream Deck key. (The names begin as Show Source in Use's do:
		// one Stream Deck key serves both.)
		const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
		auto named = [&](const char *key) {
			std::string text = obs_module_text(key);
			const size_t at = text.find("%1");
			if (at != std::string::npos)
				text.replace(at, 2, name);
			return text;
		};
		static const struct {
			const char *id, *text, *what;
		} keys[4] = {{"stagekit.active.next.", "Hotkey.AppCapture.Next", "next"},
			     {"stagekit.active.back.", "Hotkey.AppCapture.Back", "back"},
			     {"stagekit.active.hide.", "Hotkey.AppCapture.Hide", "hide"},
			     {"stagekit.active.toggle.", "Hotkey.AppCapture.Toggle", "toggle"}};
		for (size_t i = 0; i < 4; i++) {
			struct Pressed {
				AppCapture *self;
				const char *what;
			};
			pressed_[i] = {this, keys[i].what};
			hotkeys_[i] = obs_hotkey_register_source(
				parent, (keys[i].id + name).c_str(), named(keys[i].text).c_str(),
				[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
					auto *p = static_cast<std::pair<AppCapture *, const char *> *>(data);
					if (pressed)
						p->first->command(p->second);
				},
				&pressed_[i]);
		}
	}
	void detach()
	{
		for (obs_hotkey_id &id : hotkeys_) {
			if (id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(id);
			id = OBS_INVALID_HOTKEY_ID;
		}
	}

	void tick(float seconds);
	void render();

private:
	// A capture of one of the other open apps, kept running so that its
	// picture is there the moment it is switched to.
	struct Ready {
		std::string entry;
		obs_source_t *capture = nullptr; // held, and kept showing
	};

	void follow(obs_source_t *parent);
	void aim(obs_source_t *parent, const OpenWindow &window, const std::string &entry, bool by_title,
		 bool remember);
	void keep_ready(const std::vector<std::pair<std::string, const OpenWindow *>> &wanted,
			obs_source_t *parent = nullptr);
	void toggle_hidden(obs_source_t *parent);
	void keep_first(obs_source_t *parent);
	bool copy_live(obs_source_t *bridge, gs_texrender_t *into, uint32_t w, uint32_t h);
	void put(gs_texture_t *texture, double x, double y, double w, double h, float strength);

	obs_source_t *bridge_ref()
	{
		std::lock_guard lock(mutex_);
		return bridge_ ? obs_source_get_ref(bridge_) : nullptr;
	}
	void set_bridge(obs_source_t *capture)
	{
		obs_source_t *old;
		{
			std::lock_guard lock(mutex_);
			old = bridge_;
			bridge_ = capture ? obs_source_get_ref(capture) : nullptr;
		}
		obs_source_release(old);
	}
	// The size of the live picture: the bridging capture's while there is
	// one, else what comes before this filter.
	uint32_t live_size(obs_source_t *bridge, bool across)
	{
		if (bridge)
			return across ? obs_source_get_width(bridge) : obs_source_get_height(bridge);
		{
			// (The capture itself is left where it was while a ready
			// capture is drawn: when that one goes, its window is not
			// the one wanted, and must not flash up.)
			std::lock_guard lock(mutex_);
			if (!own_ok_)
				return 0;
		}
		return before_size(source_, across);
	}

	enum class Phase {
		Idle, // the live picture as it is
		Lost, // there is none all of a sudden: the last one stays for a moment
		Wait, // aimed at another window: the last one stays until the new one is there
		Mix,  // the new one comes in over the last
	};

	obs_source_t *source_;
	std::mutex mutex_;
	// Settings.
	std::vector<std::string> apps_, by_hand_;
	bool follow_ = true;
	bool ready_wanted_ = true;
	Style style_ = Style::Fade;
	double change_s_ = 0.3;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	std::string transition_ = kTransitionFade;
	uint32_t transition_ms_ = 300;
	// Following the apps (the tick, under the lock where another thread reads it).
	double timer_ = 0.0;
	bool started_ = false;
	std::string current_;             // the entry being captured, as far as this filter knows
	std::string held_;                // the entry switched to by hand, while it is held
	std::vector<std::string> recent_; // entries, the one used last first
	std::vector<std::string> was_open_;
	std::vector<OpenWindow> test_windows_;
	bool use_test_windows_ = false;
	bool cmd_step_ = false, cmd_back_ = false, cmd_hide_ = false;
	OpenWindow hold_window_; // handed over by hand, to be taken at the next look
	bool have_hold_ = false;
	std::string last_held_; // the app last held by hand: the first to go back to
	double order_check_ = 10.0;
	std::vector<Ready> ready_;
	obs_hotkey_id hotkeys_[4] = {OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
				     OBS_INVALID_HOTKEY_ID};
	std::pair<AppCapture *, const char *> pressed_[4];
	// The change between two windows.
	Phase phase_ = Phase::Idle;
	double phase_s_ = 0.0;
	double mix_ = 0.0;
	bool seen_empty_ = false;
	double settle_s_ = 0.0;            // since the capture was last aimed
	double own_s_ = 0.0;               // how long the capture itself has had a picture without a break
	bool own_ok_ = true;               // the capture itself is aimed at what is to be shown
	std::atomic<uint64_t> changes_{0}; // how many times the window has been changed
	obs_source_t *bridge_ = nullptr;   // held: the ready capture drawn until the capture itself has the window
	uint32_t kept_w_ = 0, kept_h_ = 0; // the size of the picture in `last_`
	double drawn_ago_ = 10.0;          // seconds since this filter last drew
	gs_texrender_t *last_ = nullptr;   // the last picture
	gs_texrender_t *next_ = nullptr;   // the new one, while it comes in
};

// The captures to keep running: one for each entry given, none for the rest.
void AppCapture::keep_ready(const std::vector<std::pair<std::string, const OpenWindow *>> &wanted, obs_source_t *parent)
{
	std::vector<Ready> now, gone;
	{
		std::lock_guard lock(mutex_);
		now = ready_;
	}
	std::vector<Ready> kept;
	for (const Ready &r : now) {
		const bool still =
			std::any_of(wanted.begin(), wanted.end(), [&](const auto &w) { return w.first == r.entry; });
		(still ? kept : gone).push_back(r);
	}
	for (const auto &w : wanted) {
		if (!parent ||
		    std::any_of(kept.begin(), kept.end(), [&](const Ready &r) { return r.entry == w.first; }))
			continue;
		// A capture like this one, aimed at that app's window.
		obs_data_t *like = obs_data_create();
		obs_data_t *own = obs_source_get_settings(parent);
		obs_data_apply(like, own);
		obs_data_release(own);
		const std::string name = std::string(obs_source_get_name(parent)) + " (" + w.first + ")";
		obs_source_t *capture = obs_source_create_private(obs_source_get_id(parent), name.c_str(), like);
		obs_data_release(like);
		if (!capture)
			continue;
		OpenWindow window = *w.second;
		aim_capture_at(capture, window, answers(w.first, window) == 1);
		obs_source_inc_showing(capture);
		kept.push_back(Ready{w.first, capture});
	}
	{
		std::lock_guard lock(mutex_);
		ready_ = kept;
	}
	for (const Ready &r : gone) {
		obs_source_t *bridge = bridge_ref();
		if (bridge == r.capture)
			set_bridge(nullptr);
		obs_source_release(bridge);
		obs_source_dec_showing(r.capture);
		obs_source_release(r.capture);
	}
}

void AppCapture::aim(obs_source_t *parent, const OpenWindow &window, const std::string &entry, bool by_title,
		     bool remember)
{
	obs_log(LOG_INFO, "Capture App in Use: '%s' now captures %s (%s%s)", obs_source_get_name(parent),
		window.executable.c_str(), entry.c_str(), remember ? "" : ", by hand");
	// Its picture may be here already: a capture kept ready for it.
	obs_source_t *ready = nullptr;
	{
		std::lock_guard lock(mutex_);
		for (const Ready &r : ready_)
			if (r.entry == entry && obs_source_get_width(r.capture) && obs_source_get_height(r.capture))
				ready = obs_source_get_ref(r.capture);
	}
	// With one, the capture itself is left alone: aiming it starts its
	// capturing afresh, which costs a frame or more.
	if (!ready)
		aim_capture_at(parent, window, by_title);
	const bool drawn = [&] {
		std::lock_guard lock(mutex_);
		return kept_w_ && kept_h_ && drawn_ago_ < 0.5;
	}();
	changes_++;
	// The capture drawn from and the change to it are set as one: with the
	// new capture in place a frame before the change began, that frame took
	// its picture for the last one, and the new window was there at a
	// stroke before its size had begun to change.
	obs_source_t *old_bridge;
	std::unique_lock lock(mutex_);
	old_bridge = bridge_;
	bridge_ = ready; // (its reference goes with it)
	own_ok_ = !ready;
	struct Release {
		obs_source_t *source;
		std::unique_lock<std::mutex> *lock;
		~Release()
		{
			lock->unlock();
			obs_source_release(source);
		}
	} release{old_bridge, &lock};
	current_ = entry;
	if (remember) {
		recent_.erase(std::remove(recent_.begin(), recent_.end(), entry), recent_.end());
		recent_.insert(recent_.begin(), entry);
	}
	seen_empty_ = false;
	settle_s_ = 0.0;
	if (!drawn)
		return;
	if (phase_ == Phase::Mix) {
		// One change runs into the next: the new picture so far is what
		// the next one comes in over.
		std::swap(last_, next_);
	}
	if (bridge_) {
		// At once.
		phase_ = Phase::Mix;
		mix_ = 0.0;
	} else {
		// The last picture stays until the new window's is there.
		phase_ = Phase::Wait;
		phase_s_ = 0.0;
	}
}

// Hides the capture everywhere it is shown, or shows it again.
void AppCapture::toggle_hidden(obs_source_t *parent)
{
	std::string transition;
	uint32_t ms;
	{
		std::lock_guard lock(mutex_);
		transition = transition_;
		ms = transition_ms_;
	}
	const std::vector<obs_sceneitem_t *> items = items_showing(parent);
	const bool any = std::any_of(items.begin(), items.end(),
				     [](obs_sceneitem_t *item) { return obs_sceneitem_visible(item); });
	for (obs_sceneitem_t *item : items) {
		set_item_transitions(item, transition, ms);
		obs_sceneitem_set_visible(item, !any);
		obs_sceneitem_release(item);
	}
	obs_log(LOG_INFO, "Capture App in Use: '%s' %s", obs_source_get_name(parent), any ? "hidden" : "shown");
}

// It belongs before whatever stands in for the picture or frames it: a
// Standby Picture or an Auto Frame applied before it is passed.
void AppCapture::keep_first(obs_source_t *parent)
{
	for (int i = 0; i < 32; i++) {
		struct Find {
			int first = -1, at = 0;
		} find;
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *f, void *param) {
				auto *x = static_cast<Find *>(param);
				const char *id = obs_source_get_unversioned_id(f);
				if (x->first < 0 && id &&
				    (strcmp(id, kStandbyId) == 0 || strcmp(id, kAutoFrameId) == 0))
					x->first = x->at;
				x->at++;
			},
			&find);
		const int mine = applied_index(parent, source_);
		if (mine < 0 || find.first < 0 || mine < find.first)
			break;
		obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_UP);
		if (applied_index(parent, source_) == mine)
			break;
	}
}

// Which app to capture, looked at twenty times a second.
void AppCapture::follow(obs_source_t *parent)
{
	std::vector<std::string> apps, by_hand, recent, was_open;
	std::vector<OpenWindow> windows;
	std::string current, held, last_held;
	OpenWindow picked;
	bool follow, use_test, first, ready, step, back, have_picked = false;
	{
		std::lock_guard lock(mutex_);
		apps = apps_;
		by_hand = by_hand_;
		follow = follow_;
		ready = ready_wanted_;
		use_test = use_test_windows_;
		if (use_test)
			windows = test_windows_;
		first = !started_;
		started_ = true;
		current = current_;
		held = held_;
		last_held = last_held_;
		recent = recent_;
		was_open = was_open_;
		step = cmd_step_;
		back = cmd_back_;
		cmd_step_ = cmd_back_ = false;
		if (have_hold_) {
			picked = hold_window_;
			have_picked = true;
			have_hold_ = false;
		}
	}
	if (have_picked && !picked.executable.empty()) {
		// Its program goes on the list of apps switched to, if it is not there.
		const std::string entry = picked.executable;
		if (std::none_of(by_hand.begin(), by_hand.end(),
				 [&](const std::string &e) { return lowered(e) == lowered(entry); })) {
			by_hand.push_back(entry);
			obs_data_t *settings = obs_source_get_settings(source_);
			pick_list_write(settings, "by_hand", false, by_hand);
			obs_data_release(settings);
		}
		{
			std::lock_guard lock(mutex_);
			by_hand_ = by_hand;
			held_ = last_held_ = entry;
		}
		aim(parent, picked, entry, false, false);
		return;
	}
#ifndef _WIN32
	// Nothing to go by on other systems.
	if (!use_test)
		return;
#endif
	if (apps.empty() && by_hand.empty())
		return;
	if (!use_test)
		windows = open_windows();

	// For an entry: the window to take (the one in front if it is one of
	// its windows, else the first), and whether one of them is in front.
	struct Slot {
		const OpenWindow *window = nullptr;
		int how = 0;
		bool front = false;
	};
	auto slot_of = [&](const std::string &entry) {
		Slot slot;
		for (const OpenWindow &w : windows) {
			const int how = answers(entry, w);
			if (!how)
				continue;
			if (!slot.window || (w.foreground && !slot.front) || (how > slot.how && !slot.front)) {
				slot.window = &w;
				slot.how = how;
			}
			slot.front = slot.front || w.foreground;
		}
		return slot;
	};
	std::vector<Slot> slots(apps.size()), hand(by_hand.size());
	for (size_t i = 0; i < apps.size(); i++)
		slots[i] = slot_of(apps[i]);
	for (size_t i = 0; i < by_hand.size(); i++)
		hand[i] = slot_of(by_hand[i]);

	// The other open apps' captures keep running while this one is on
	// show, so whichever is switched to has its picture at once.
	auto ready_for = [&](const std::string &) {
		// Every open app on the two lists has one, the one being shown
		// too: it is what is drawn. Nothing is started or stopped when
		// the app changes, only when one opens or closes. (Up to beta118
		// each change started one capture, aimed another and stopped a
		// third, on OBS's drawing thread: the owner saw the frame time
		// jump at every change.)
		const std::string but;
		std::vector<std::pair<std::string, const OpenWindow *>> wanted;
		if (ready && obs_source_showing(parent)) {
			for (size_t i = 0; i < apps.size(); i++)
				if (slots[i].window && apps[i] != but)
					wanted.emplace_back(apps[i], slots[i].window);
			for (size_t i = 0; i < by_hand.size(); i++)
				if (hand[i].window && by_hand[i] != but &&
				    std::none_of(wanted.begin(), wanted.end(),
						 [&](const auto &w) { return w.first == by_hand[i]; }))
					wanted.emplace_back(by_hand[i], hand[i].window);
		}
		keep_ready(wanted, parent);
	};

	// By hand: on to the next of the apps switched to that is open, and
	// after the last, back to following. While one is held nothing follows.
	bool released = false;
	auto let_go = [&] {
		held.clear();
		current.clear();
		first = true;
		released = true;
		std::lock_guard lock(mutex_);
		held_.clear();
		current_.clear();
	};
	if (back && !held.empty())
		let_go();
	if (step) {
		size_t from = 0;
		for (size_t i = 0; i < by_hand.size(); i++)
			if (by_hand[i] == held)
				from = i + 1;
		int next = -1;
		// From following the apps: the one held last, so that one press
		// goes back to it; else the first that is open.
		for (size_t i = 0; held.empty() && i < by_hand.size(); i++)
			if (by_hand[i] == last_held && hand[i].window)
				next = (int)i;
		for (size_t i = held.empty() ? 0 : from; i < by_hand.size() && next < 0; i++)
			if (hand[i].window && (held.empty() || by_hand[i] != last_held || i >= from))
				next = (int)i;
		if (next >= 0) {
			held = by_hand[(size_t)next];
			{
				std::lock_guard lock(mutex_);
				held_ = last_held_ = held;
			}
			aim(parent, *hand[(size_t)next].window, held, hand[(size_t)next].how == 1, false);
			current = held;
		} else if (!held.empty()) {
			let_go();
		}
	}
	if (!held.empty()) {
		// It stays until it is let go, or its window closes.
		bool open = false;
		for (size_t i = 0; i < by_hand.size(); i++)
			open = open || (by_hand[i] == held && hand[i].window);
		if (open) {
			ready_for(held);
			return;
		}
		let_go();
	}

	auto index_of = [&](const std::string &entry) {
		for (size_t i = 0; i < apps.size(); i++)
			if (apps[i] == entry)
				return (int)i;
		return -1;
	};
	// The window in front may answer to two entries (a program, and a title
	// of that program's): the title, which says more, comes first.
	int front = -1;
	for (size_t i = 0; i < apps.size(); i++)
		if (slots[i].front && (front < 0 || slots[i].how < slots[(size_t)front].how))
			front = (int)i;
	// What to go back to: the one used most recently that is open, else the
	// first that is open.
	auto fallback = [&]() {
		for (const std::string &entry : recent) {
			const int index = index_of(entry);
			if (index >= 0 && slots[(size_t)index].window)
				return index;
		}
		for (size_t i = 0; i < slots.size(); i++)
			if (slots[i].window)
				return (int)i;
		return -1;
	};

	int now = index_of(current);
	// (Not on the way back from a manual window: what the capture itself is
	// aimed at is not what is showing then, and taking it for that put its
	// picture up at a stroke.)
	if (first && now < 0 && !released) {
		// What the capture is aimed at already, if that is one of the apps.
		obs_data_t *settings = obs_source_get_settings(parent);
		const std::string window = obs_data_get_string(settings, "window");
		obs_data_release(settings);
		const size_t colon = window.rfind(':');
		OpenWindow aimed;
		aimed.title = decoded(window.substr(0, window.find(':')));
		aimed.executable = colon == std::string::npos ? std::string() : decoded(window.substr(colon + 1));
		int best = 0;
		for (size_t i = 0; i < apps.size() && !aimed.executable.empty(); i++) {
			const int how = slots[i].window ? answers(apps[i], aimed) : 0;
			if (how && (!best || how < best)) {
				now = (int)i;
				best = how;
			}
		}
		if (now >= 0) {
			std::lock_guard lock(mutex_);
			current_ = current = apps[(size_t)now];
			own_ok_ = true;
		}
	}
	int target = -1;
	if (first) {
		if (now < 0 || (follow && front >= 0 && front != now))
			target = front >= 0 ? front : fallback();
	} else if (follow && front >= 0 && front != now) {
		target = front;
	} else {
		// One that has just opened.
		for (size_t i = 0; i < apps.size(); i++)
			if (slots[i].window && (int)i != now &&
			    std::find(was_open.begin(), was_open.end(), apps[i]) == was_open.end())
				target = (int)i;
	}
	// The one being captured has closed: back to the one before it.
	if (target < 0 && now >= 0 && !slots[(size_t)now].window) {
		target = fallback();
		if (target < 0) {
			std::lock_guard lock(mutex_);
			current_.clear();
		}
	}
	std::vector<std::string> open_now;
	for (size_t i = 0; i < apps.size(); i++)
		if (slots[i].window)
			open_now.push_back(apps[i]);
	{
		std::lock_guard lock(mutex_);
		was_open_ = open_now;
	}
	if (target >= 0 && target != now && slots[(size_t)target].window) {
		aim(parent, *slots[(size_t)target].window, apps[(size_t)target], slots[(size_t)target].how == 1, true);
		current = apps[(size_t)target];
	}
	ready_for(current);
}

void AppCapture::tick(float seconds)
{
	obs_source_t *parent = obs_filter_get_parent(source_);
	if (!parent)
		return;
	const bool on = obs_source_enabled(source_);
	if (!on) {
		keep_ready({});
		set_bridge(nullptr);
		std::lock_guard lock(mutex_);
		phase_ = Phase::Idle;
		started_ = false;
		own_ok_ = true;
		held_.clear();
		cmd_step_ = cmd_back_ = false;
		const bool hide = cmd_hide_;
		cmd_hide_ = false;
		if (!hide)
			return;
	}
	obs_source_t *bridge = bridge_ref();
	const bool has = live_size(bridge, true) && live_size(bridge, false);
	const bool own = before_size(source_, true) && before_size(source_, false);
	obs_source_release(bridge);
	bool look = false, hide = false, order = false, done_bridging = false;
	{
		std::lock_guard lock(mutex_);
		drawn_ago_ += seconds;
		const double change_s = style_ == Style::Cut ? 0.0 : change_s_;
		phase_s_ += seconds;
		settle_s_ += seconds;
		seen_empty_ = seen_empty_ || !own;
		own_s_ = own ? own_s_ + seconds : 0.0;
		switch (phase_) {
		case Phase::Idle:
			// The capture itself has the window now: the capture that was
			// kept ready for it has done its part.
			// (Only once it has settled there: it has been seen to let go
			// of the old window and has had a picture for a moment since,
			// or a good while has gone by. Believed sooner, it could still
			// be showing the old window: a flash of the app just left.)
			done_bridging = bridge_ && own_ok_ && own &&
					((seen_empty_ && own_s_ >= kSteady) || settle_s_ >= kBridgeFor);
			break;
		case Phase::Lost:
			if (has) {
				// It is back by itself.
				phase_ = Phase::Mix;
				mix_ = 0.0;
			} else if (phase_s_ >= kLostFor) {
				phase_ = Phase::Idle;
				kept_w_ = kept_h_ = 0;
			}
			break;
		case Phase::Wait:
			if (has && ((seen_empty_ && own_s_ >= 0.1) || phase_s_ >= kSettle)) {
				phase_ = Phase::Mix;
				mix_ = 0.0;
			} else if (phase_s_ >= kWaitFor) {
				phase_ = Phase::Idle;
				kept_w_ = kept_h_ = 0;
			}
			break;
		case Phase::Mix:
			mix_ = change_s > 0.0 ? std::min(1.0, mix_ + seconds / change_s) : 1.0;
			if (mix_ >= 1.0) {
				// The new picture is the last one from here.
				std::swap(last_, next_);
				phase_ = Phase::Idle;
			}
			break;
		}
		hide = cmd_hide_;
		cmd_hide_ = false;
		timer_ -= seconds;
		if (on && timer_ <= 0.0) {
			timer_ = 0.05;
			look = true;
		}
		order_check_ += seconds;
		if (order_check_ >= 0.5) {
			order_check_ = 0.0;
			order = true;
		}
	}
	if (done_bridging)
		set_bridge(nullptr);
	if (order)
		keep_first(parent);
	if (hide)
		toggle_hidden(parent);
	if (look)
		follow(parent);
}

// The live picture into a picture of its own, as it is: what comes before
// this filter, or the capture kept ready that stands in for it.
bool AppCapture::copy_live(obs_source_t *bridge, gs_texrender_t *into, uint32_t w, uint32_t h)
{
	gs_texrender_reset(into);
	const bool was_linear = gs_set_linear_srgb(false);
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	bool drawn = false;
	if (gs_texrender_begin(into, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (bridge)
			obs_source_video_render(bridge);
		else
			obs_source_skip_video_filter(source_);
		gs_blend_state_pop();
		gs_texrender_end(into);
		drawn = true;
	}
	gs_enable_framebuffer_srgb(was_srgb);
	gs_set_linear_srgb(was_linear);
	return drawn;
}

void AppCapture::put(gs_texture_t *texture, double x, double y, double w, double h, float strength)
{
	if (!texture || !effect_ || strength <= 0.0f)
		return;
	const bool linear = gs_get_linear_srgb();
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	if (linear)
		gs_effect_set_texture_srgb(p_image_, texture);
	else
		gs_effect_set_texture(p_image_, texture);
	gs_effect_set_float(p_opacity_, strength);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	set_quad(Quad{w, 0.0, 0.0, h, x, y});
	while (gs_effect_loop(effect_, "Draw"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(was_srgb);
}

void AppCapture::render()
{
	if (!effect_ || !obs_source_enabled(source_)) {
		obs_source_skip_video_filter(source_);
		return;
	}
	obs_source_t *bridge = bridge_ref();
	const uint32_t bw = live_size(bridge, true), bh = live_size(bridge, false);
	const bool has = bw && bh;
	Phase phase;
	Style style;
	double mix;
	uint32_t kw, kh;
	EaseCurve curve;
	EaseMode mode;
	{
		std::lock_guard lock(mutex_);
		drawn_ago_ = 0.0;
		// The picture has gone all of a sudden: the last one stays while it
		// is found out what to capture next.
		if (phase_ == Phase::Idle && !has && kept_w_ && kept_h_) {
			phase_ = Phase::Lost;
			phase_s_ = 0.0;
		}
		phase = phase_;
		style = style_;
		mix = mix_;
		kw = kept_w_;
		kh = kept_h_;
		curve = curve_;
		mode = mode_;
	}
	gs_texture_t *last = gs_texrender_get_texture(last_);
	if (phase == Phase::Idle) {
		if (has && copy_live(bridge, last_, bw, bh)) {
			{
				std::lock_guard lock(mutex_);
				kept_w_ = bw;
				kept_h_ = bh;
			}
			put(gs_texrender_get_texture(last_), 0.0, 0.0, bw, bh, 1.0f);
		} else if (has) {
			obs_source_skip_video_filter(source_);
		}
	} else if (phase != Phase::Mix || !has || !copy_live(bridge, next_, bw, bh)) {
		put(last, 0.0, 0.0, kw, kh, 1.0f);
	} else {
		const double t = std::clamp(ease(curve, mode, mix), 0.0, 1.0);
		gs_texture_t *next = gs_texrender_get_texture(next_);
		// The frame's size is on the way from the one picture's to the
		// other's (a cut apart).
		const bool glides = style != Style::Cut && kw && kh;
		const double w = glides ? kw + ((double)bw - kw) * t : bw, h = glides ? kh + ((double)bh - kh) * t : bh;
		if (style == Style::Morph && kw && kh) {
			// Both stretched over it: the one turns into the other.
			put(last, 0.0, 0.0, w, h, 1.0f);
			put(next, 0.0, 0.0, w, h, (float)t);
		} else {
			// Each keeps its own shape, fitted inside the frame: the last
			// picture, and the new one coming in over it.
			struct Fit {
				double x, y, w, h;
			};
			auto fitted = [&](double pw, double ph) {
				const double k = std::min(w / pw, h / ph);
				return Fit{(w - pw * k) / 2.0, (h - ph * k) / 2.0, pw * k, ph * k};
			};
			if (kw && kh) {
				const Fit f = fitted(kw, kh);
				put(last, f.x, f.y, f.w, f.h, 1.0f);
			}
			const Fit f = fitted(bw, bh);
			switch (style) {
			case Style::Zoom: {
				const double k = 0.85 + 0.15 * t;
				put(next, f.x + f.w * (1.0 - k) / 2.0, f.y + f.h * (1.0 - k) / 2.0, f.w * k, f.h * k,
				    (float)t);
				break;
			}
			case Style::FromLeft:
				put(next, f.x - w * (1.0 - t), f.y, f.w, f.h, 1.0f);
				break;
			case Style::FromRight:
				put(next, f.x + w * (1.0 - t), f.y, f.w, f.h, 1.0f);
				break;
			case Style::FromTop:
				put(next, f.x, f.y - h * (1.0 - t), f.w, f.h, 1.0f);
				break;
			case Style::FromBottom:
				put(next, f.x, f.y + h * (1.0 - t), f.w, f.h, 1.0f);
				break;
			default:
				put(next, f.x, f.y, f.w, f.h, (float)t);
				break;
			}
		}
	}
	obs_source_release(bridge);
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<AppCapture *>(data);
	obs_properties_t *props = obs_properties_create();
	// What the capture is aimed at now, and whether that is by hand.
	std::string now;
	if (obs_source_t *parent = self ? obs_filter_get_parent(self->source()) : nullptr) {
		obs_data_t *aimed = obs_source_get_settings(parent);
		const std::string window = obs_data_get_string(aimed, "window");
		obs_data_release(aimed);
		const size_t colon = window.rfind(':');
		if (colon != std::string::npos)
			now = decoded(window.substr(colon + 1));
	}
	// (What this filter is showing says more, when it knows: the capture
	// itself is left where it was while another one is drawn for it.)
	if (self && !self->current().empty())
		now = self->current();
	const bool by_hand = self && !self->held().empty();
	std::string status = obs_module_text(now.empty() ? "AppCapture.Status.None"
					     : by_hand   ? "AppCapture.Status.Held"
							 : "AppCapture.Status.Now");
	const size_t at = status.find("%1");
	if (at != std::string::npos)
		status.replace(at, 2, now);
	obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);

	PickList apps;
	apps.setting = "apps";
	apps.title = "AppCapture.Apps";
	apps.add = "AppCapture.Apps.Add";
	apps.tip = "AppCapture.Apps.Tip";
	apps.choices = [](std::vector<std::string> &names) {
		for (const std::string &name : open_program_names())
			if (std::find(names.begin(), names.end(), name) == names.end())
				names.push_back(name);
		// And the open windows by title, for telling apart two programs
		// with the same file name.
		for (const OpenWindow &w : open_windows())
			if (!w.title.empty() && std::find(names.begin(), names.end(), w.title) == names.end())
				names.push_back(w.title);
	};
	pick_list_add(props, self ? self->source() : nullptr, apps);
	PickList by_hand_list;
	by_hand_list.setting = "by_hand";
	by_hand_list.title = "AppCapture.ByHand";
	by_hand_list.add = "AppCapture.Apps.Add";
	by_hand_list.tip = "AppCapture.ByHand.Tip";
	by_hand_list.choices = apps.choices;
	pick_list_add(props, self ? self->source() : nullptr, by_hand_list);
	obs_properties_add_button2(
		props, "by_hand_toggle", obs_module_text("AppCapture.ByHand.Toggle"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			if (data)
				static_cast<AppCapture *>(data)->command("toggle");
			return false;
		},
		self);
	obs_properties_add_button2(
		props, "by_hand_next", obs_module_text("AppCapture.ByHand.Next"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			if (data)
				static_cast<AppCapture *>(data)->command("next");
			return false;
		},
		self);
	obs_properties_add_button2(
		props, "by_hand_back", obs_module_text("AppCapture.ByHand.Back"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			if (data)
				static_cast<AppCapture *>(data)->command("back");
			return false;
		},
		self);

	obs_property_t *follow = obs_properties_add_bool(props, "follow", obs_module_text("AppCapture.Follow"));
	obs_property_set_long_description(follow, obs_module_text("AppCapture.Follow.Tip"));
	obs_property_t *ready = obs_properties_add_bool(props, "ready", obs_module_text("AppCapture.Ready"));
	obs_property_set_long_description(ready, obs_module_text("AppCapture.Ready.Tip"));

	obs_property_t *change = obs_properties_add_list(props, "change", obs_module_text("AppCapture.Change"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Fade"), (int)Style::Fade);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Morph"), (int)Style::Morph);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Cut"), (int)Style::Cut);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.Zoom"), (int)Style::Zoom);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromLeft"), (int)Style::FromLeft);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromRight"), (int)Style::FromRight);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromTop"), (int)Style::FromTop);
	obs_property_list_add_int(change, obs_module_text("AppCapture.Change.FromBottom"), (int)Style::FromBottom);
	obs_property_set_long_description(change, obs_module_text("AppCapture.Change.Tip"));
	obs_property_set_modified_callback(change, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		const bool timed = obs_data_get_int(settings, "change") != (int)Style::Cut;
		for (const char *name : {"change_ms", "ease_curve", "ease_mode"})
			if (obs_property_t *one = obs_properties_get(p, name))
				obs_property_set_visible(one, timed);
		return true;
	});
	obs_property_t *time =
		obs_properties_add_int_slider(props, "change_ms", obs_module_text("Change.Time"), 50, 5000, 50);
	obs_property_int_set_suffix(time, " ms");
	add_easing_properties(props, "");

	// Hiding the capture altogether, and showing it again.
	obs_properties_t *hiding = obs_properties_create();
	add_item_transition_properties(hiding);
	obs_properties_add_button2(
		hiding, "hide_now", obs_module_text("AppCapture.Hide.Now"),
		[](obs_properties_t *, obs_property_t *, void *data) {
			if (data)
				static_cast<AppCapture *>(data)->command("hide");
			return false;
		},
		self);
	obs_properties_add_group(props, "hiding", obs_module_text("AppCapture.Hide"), OBS_GROUP_NORMAL, hiding);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "follow", true);
	obs_data_set_default_bool(settings, "ready", true);
	item_transition_defaults(settings);
	obs_data_set_default_int(settings, "change", (int)Style::Fade);
	obs_data_set_default_int(settings, "change_ms", 300);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

bool app_capture_hold(obs_source_t *capture, const OpenWindow &window)
{
	struct Hand {
		const OpenWindow *window;
		bool taken;
	} hand{&window, false};
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *h = static_cast<Hand *>(param);
			const char *id = obs_source_get_unversioned_id(filter);
			if (h->taken || !id || strcmp(id, kAppCaptureId) != 0 || !obs_source_enabled(filter))
				return;
			if (auto *self = static_cast<AppCapture *>(obs_obj_get_data(filter))) {
				self->hold(*h->window);
				h->taken = true;
			}
		},
		&hand);
	return hand.taken;
}

uint64_t app_capture_changes(obs_source_t *capture)
{
	uint64_t changes = 0;
	if (!capture)
		return changes;
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kAppCaptureId) != 0)
				return;
			if (auto *self = static_cast<AppCapture *>(obs_obj_get_data(filter)))
				*static_cast<uint64_t *>(param) += self->changes();
		},
		&changes);
	return changes;
}

bool app_capture_lists(const std::string &executable, const std::string &title)
{
	std::lock_guard lock(g_listed_mutex);
	for (const auto &one : g_listed)
		for (const std::string &entry : one.second)
			if (!entry.empty() && (entry == executable || title.find(entry) != std::string::npos))
				return true;
	return false;
}

bool app_capture_holding(obs_source_t *capture)
{
	bool holding = false;
	obs_source_enum_filters(
		capture,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kAppCaptureId) != 0 || !obs_source_enabled(filter))
				return;
			auto *self = static_cast<AppCapture *>(obs_obj_get_data(filter));
			if (self && self->holding())
				*static_cast<bool *>(param) = true;
		},
		&holding);
	return holding;
}

void register_app_capture()
{
	obs_source_info info = {};
	info.id = kAppCaptureId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("AppCapture.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AppCapture(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AppCapture *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AppCapture *>(data)->update(settings);
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AppCapture *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AppCapture *>(data)->render();
	};
	info.get_width = [](void *data) {
		return static_cast<AppCapture *>(data)->size(true);
	};
	info.get_height = [](void *data) {
		return static_cast<AppCapture *>(data)->size(false);
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<AppCapture *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<AppCapture *>(data)->detach();
	};
	info.get_properties = get_properties;
	info.get_defaults = get_defaults;
	register_timed(&info);
}

} // namespace stagekit
