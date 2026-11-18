/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Modes (StageKit): one filter on a scene names its modes and the
// sources that belong to each. One mode is on at a time, or none when that is
// allowed. Turning a mode on hides the sources of the other modes and shows
// its own; a source in both the old and the new mode is not touched, and a
// source in no mode is never touched. The owner: "scene modes should not have
// specific names built in. instead scene modes should be able to be made
// manually and customized", with templates to start from.
//
// Modes are known by an id that never changes ("m1", "m2", ...): a mode's
// name and type can be changed without breaking a key or a hotkey. Settings:
//
//   modes               array {value}   the ids, in order (the order of "next")
//   mode|<id>|name      string          what the mode is called
//   mode|<id>|type      string          what it shows, for a Stream Deck key's image
//   mode|<id>|go_to     string          a scene to change to on a key's press, "" to stay
//   mode|<id>|sources   array {value}   its sources, by name
//   mode_now            string          the id of the mode on now (written by the filter)
//   mode_before         string          the one before, for Toggle (written by the filter)
//   allow_none, share_scenes, lock_hidden, and the fuller Show/Hide transition.
//
// Presses (hotkeys, obs-websocket, the window's "Mode now") are queued and
// acted on in the filter's tick, never on the thread that made them. Nothing
// is polled: the filter listens to "item_visible" on its scenes, and puts
// back a member of a mode that is not on when something else shows it.

#include "scene-modes.hpp"

#include "visibility-link.hpp"

#include "../api/vendor-api.hpp"
#include "../common/go-to-scene.hpp"
#include "../common/holds-hidden.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

// What a mode shows, for the image of a Stream Deck Scene Mode key left on
// Automatic. Kept by id, never by number: the same ids as the Stream Deck
// plugin's Source type option. "" is Automatic.
struct TypeInfo {
	const char *id, *text;
};
const TypeInfo kTypes[] = {
	{"", "Modes.Type.Auto"},
	{"game", "Modes.Type.Game"},
	{"window", "Modes.Type.Window"},
	{"art_app", "Modes.Type.ArtApp"},
	{"code_editor", "Modes.Type.CodeEditor"},
	{"browser", "Modes.Type.Browser"},
	{"camera", "Modes.Type.Camera"},
	{"capture_card", "Modes.Type.CaptureCard"},
	{"display", "Modes.Type.Display"},
	{"phone", "Modes.Type.Phone"},
	{"tablet", "Modes.Type.Tablet"},
	{"chat", "Modes.Type.Chat"},
	{"music", "Modes.Type.Music"},
	{"alerts", "Modes.Type.Alerts"},
	{"title", "Modes.Type.Title"},
	{"image", "Modes.Type.Image"},
	{"video", "Modes.Type.Video"},
	{"starting", "Modes.Type.Starting"},
	{"brb", "Modes.Type.Brb"},
	{"intermission", "Modes.Type.Intermission"},
	{"ending", "Modes.Type.Ending"},
};

// The templates a mode can be made from: a starting name and a type, both
// editable afterwards. Saved by number in the window's box only; append only.
struct Template {
	const char *name, *type;
};
const Template kTemplates[] = {
	{"Modes.Template.Starting", "starting"},
	{"Modes.Template.Game", "game"},
	{"Modes.Template.Display", "display"},
	{"Modes.Template.Window", "window"},
	{"Modes.Template.Camera", "camera"},
	{"Modes.Template.Chat", "chat"},
	{"Modes.Template.Intermission", "intermission"},
	{"Modes.Template.Brb", "brb"},
	{"Modes.Template.Ending", "ending"},
};

std::string key(const std::string &id, const char *part)
{
	return "mode|" + id + "|" + part;
}

std::string trimmed(const std::string &text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

bool has(const std::vector<std::string> &list, const char *name)
{
	return name && std::find(list.begin(), list.end(), name) != list.end();
}

struct Mode {
	std::string id, name, type, go_to;
	std::vector<std::string> sources;
};

std::vector<Mode> read_modes(obs_data_t *settings)
{
	std::vector<Mode> modes;
	for (const std::string &id : pick_list_read(settings, "modes", false)) {
		Mode mode;
		mode.id = id;
		mode.name = trimmed(obs_data_get_string(settings, key(id, "name").c_str()));
		if (mode.name.empty())
			mode.name = id;
		mode.type = obs_data_get_string(settings, key(id, "type").c_str());
		mode.go_to = obs_data_get_string(settings, key(id, "go_to").c_str());
		mode.sources = pick_list_read(settings, key(id, "sources").c_str(), false);
		modes.push_back(std::move(mode));
	}
	return modes;
}

const Mode *find_mode(const std::vector<Mode> &modes, const std::string &id)
{
	if (id.empty())
		return nullptr;
	for (const Mode &mode : modes)
		if (mode.id == id)
			return &mode;
	return nullptr;
}

// Is the source in some mode?
bool member(const std::vector<Mode> &modes, const char *name)
{
	for (const Mode &mode : modes)
		if (has(mode.sources, name))
			return true;
	return false;
}

// Goes through a scene's items, straight in it and inside its groups (a group
// itself is an item too).
void each_item(obs_source_t *scene, const std::function<void(obs_sceneitem_t *item, const char *name)> &act)
{
	obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr;
	if (!s)
		return;
	static const auto look = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
		if (name && *name)
			(*static_cast<const std::function<void(obs_sceneitem_t *, const char *)> *>(param))(item, name);
		return true;
	};
	obs_scene_enum_items(
		s,
		[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
			look(sc, item, param);
			if (obs_sceneitem_is_group(item))
				obs_sceneitem_group_enum_items(item, look, param);
			return true;
		},
		const_cast<std::function<void(obs_sceneitem_t *, const char *)> *>(&act));
}

// What other filters ask of one Scene Modes, copied out under the locks.
struct ModesLook {
	obs_source_t *parent = nullptr; // compared, never used
	std::string scene_name;
	std::vector<std::string> share;
	std::vector<Mode> modes;
	std::string current;
	bool settling = false;
	const void *self = nullptr;
};

class SceneModes;

std::mutex g_modes_mutex;
std::vector<SceneModes *> g_modes;

// "source_rename" is listened to while there is at least one filter.
int g_rename_users = 0;
void on_source_rename(void *, calldata_t *cd);

bool covers(const ModesLook &look, obs_source_t *scene)
{
	if (!scene)
		return false;
	if (scene == look.parent)
		return true;
	const char *name = obs_source_get_name(scene);
	return name && has(look.share, name);
}

// Does this filter's mode hold the item's source hidden?
bool look_holds(const ModesLook &look, const char *name)
{
	if (!member(look.modes, name))
		return false;
	const Mode *now = find_mode(look.modes, look.current);
	return !now || !has(now->sources, name);
}

std::vector<ModesLook> looks_of_enabled(const void *except);

// Is the item held hidden for a reason other than this filter's own mode:
// a Link Visibility mode that is not showing, or another Scene Modes of the
// scene? A Show in Scenes switch that is off is not a hold: it is a command,
// acted on when pressed (the owner: "show in scenes should not prevent any
// other filter from turning on or off a source later").
bool held_elsewhere(obs_source_t *scene, obs_sceneitem_t *item, const void *self)
{
	if (link_visibility_holds_hidden(scene, item))
		return true;
	const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
	for (const ModesLook &look : looks_of_enabled(self))
		if (covers(look, scene) && look_holds(look, name))
			return true;
	return false;
}

enum class How { On, Toggle, Next, None, Window };

struct Press {
	std::string mode; // an id
	How how = How::On;
};

// An item shown or hidden by something else, in a scene the filter covers.
struct Seen {
	std::string scene;
	int64_t item = 0;
	bool visible = false;
};

class SceneModes {
public:
	SceneModes(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		{
			std::lock_guard lock(g_modes_mutex);
			g_modes.push_back(this);
			if (g_rename_users++ == 0)
				signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename,
						       nullptr);
		}
		// Copying a filter or loading a collection: say what it now offers.
		emit_ = true;
	}

	~SceneModes()
	{
		detach();
		std::lock_guard lock(g_modes_mutex);
		g_modes.erase(std::remove(g_modes.begin(), g_modes.end(), this), g_modes.end());
		if (--g_rename_users == 0)
			signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
	}

	obs_source_t *source() const { return source_; }

	obs_source_t *parent()
	{
		std::lock_guard lock(g_modes_mutex);
		return parent_;
	}

	// For other filters' questions: called with g_modes_mutex held.
	ModesLook look()
	{
		ModesLook look;
		look.parent = parent_;
		look.scene_name = parent_ && obs_source_get_name(parent_) ? obs_source_get_name(parent_) : "";
		look.self = this;
		look.settling = settling_;
		std::lock_guard lock(mutex_);
		look.share = share_;
		look.modes = modes_;
		look.current = current_;
		return look;
	}

	// For the vendor requests: called with g_modes_mutex held.
	SceneModesLook vendor_look()
	{
		SceneModesLook look;
		look.scene = parent_ && obs_source_get_name(parent_) ? obs_source_get_name(parent_) : "";
		look.filter = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
		look.enabled = obs_source_enabled(source_);
		std::lock_guard lock(mutex_);
		look.current = current_;
		look.previous = previous_;
		look.allow_none = allow_none_;
		look.scenes = share_;
		for (const Mode &mode : modes_)
			look.modes.push_back({mode.id, mode.name, mode.type});
		return look;
	}

	void update(obs_data_t *settings)
	{
		std::vector<Mode> modes = read_modes(settings);
		const std::vector<std::string> share = pick_list_read(settings, "share_scenes", false);
		const bool allow_none = obs_data_get_bool(settings, "allow_none");
		const bool lock_hidden = obs_data_get_bool(settings, "lock_hidden");
		const ItemChange change = item_change_read(settings);
		bool keys_changed = false;
		{
			std::lock_guard lock(mutex_);
			if (lock_hidden && !lock_hidden_)
				sweep_ = true;
			lock_hidden_ = lock_hidden;
			// Keys and their names follow the list of modes.
			if (modes.size() != modes_.size())
				keys_changed = true;
			for (size_t i = 0; !keys_changed && i < modes.size(); i++)
				keys_changed = modes[i].id != modes_[i].id || modes[i].name != modes_[i].name;
			keys_changed = keys_changed || allow_none != allow_none_;
			modes_ = std::move(modes);
			share_ = share;
			allow_none_ = allow_none;
			change_ = change;
			watch_timer_ = 0.0;
			const std::string now = obs_data_get_string(settings, "mode_now");
			const std::string before = obs_data_get_string(settings, "mode_before");
			if (first_update_) {
				// What was saved: the scene shows it already.
				first_update_ = false;
				current_ = find_mode(modes_, now) ? now : "";
				previous_ = find_mode(modes_, before) ? before : "";
			} else {
				// A mode taken off the list is no longer on; nothing is
				// shown or hidden for that.
				if (!find_mode(modes_, current_))
					current_.clear();
				if (!find_mode(modes_, previous_))
					previous_.clear();
				// Chosen in the window's "Mode now": a press, as a key's,
				// that changes no scene.
				if (now != current_ && (find_mode(modes_, now) || now.empty()))
					presses_.push_back({now, How::Window});
			}
		}
		if (keys_changed) {
			emit_ = true;
			sync_hotkeys();
		}
	}

	void attach(obs_source_t *parent)
	{
		detach();
		{
			std::lock_guard lock(g_modes_mutex);
			parent_ = parent;
		}
		signal_handler_connect(obs_source_get_signal_handler(parent), "item_visible", visible_cb, this);
		sync_hotkeys();
	}

	void detach()
	{
		obs_source_t *was = nullptr;
		{
			std::lock_guard lock(g_modes_mutex);
			was = parent_;
			parent_ = nullptr;
		}
		if (was)
			signal_handler_disconnect(obs_source_get_signal_handler(was), "item_visible", visible_cb, this);
		unwatch();
		unregister_hotkeys();
	}

	// A press from a key, a hotkey or the window. Checked here, acted on in
	// the next tick.
	bool queue(const std::string &mode, How how, std::string &why)
	{
		if (!obs_source_enabled(source_)) {
			why = "Scene Modes is off";
			return false;
		}
		std::lock_guard lock(mutex_);
		std::string id;
		if (how == How::On || how == How::Toggle) {
			for (const Mode &m : modes_)
				if (m.id == mode) {
					id = m.id;
					break;
				}
			for (const Mode &m : modes_)
				if (id.empty() && m.name == mode) {
					id = m.id;
					break;
				}
			if (id.empty()) {
				why = "No mode '" + mode + "' in that Scene Modes";
				return false;
			}
		}
		if (how == How::None && !allow_none_) {
			why = "That Scene Modes does not allow no mode";
			return false;
		}
		if (how == How::Next && modes_.empty()) {
			why = "That Scene Modes has no modes";
			return false;
		}
		presses_.push_back({id, how});
		return true;
	}

	void tick(float seconds)
	{
		settling_ = false;
		obs_source_t *parent = this->parent();
		if (!parent)
			return;
		if (emit_.exchange(false))
			emit_changed();
		const bool enabled = obs_source_enabled(source_);
		std::vector<Press> presses;
		std::vector<Seen> seen;
		bool sweep;
		{
			std::lock_guard lock(mutex_);
			seen.swap(pending_);
			if (enabled && scenes_loaded())
				presses.swap(presses_);
			else if (!enabled)
				presses_.clear(); // keys are refused while off; the window's choice too
			sweep = sweep_ || !was_on_;
			sweep_ = false;
		}
		// Off, nothing is done; on again, the items are taken as they are.
		if (!enabled) {
			was_on_ = false;
			return;
		}
		// While OBS loads its scenes, or changes collection, nothing is
		// looked at: only some scenes have their items. A press is kept for
		// then (above).
		if (!scenes_loaded()) {
			unwatch();
			was_on_ = false;
			return;
		}
		was_on_ = true;
		if ((watch_timer_ -= seconds) <= 0.0) {
			watch_timer_ = 2.0;
			watch_shared();
		}
		if (!ready_) {
			ready_ = true;
			put_right_after_loading();
		}
		if (sweep)
			lock_sweep();
		if (!seen.empty())
			put_back(seen);
		for (const Press &press : presses)
			act(press);
		// A change left under way in a scene that was left with a cut is
		// ended, once a second, in the scenes not on screen.
		if ((stale_timer_ -= seconds) <= 0.0) {
			stale_timer_ = 1.0;
			end_stale();
		}
	}

	// Every key's name, for the window's lists and the hotkeys.
	std::vector<Mode> modes()
	{
		std::lock_guard lock(mutex_);
		return modes_;
	}

	std::string current()
	{
		std::lock_guard lock(mutex_);
		return current_;
	}

	bool allows_none()
	{
		std::lock_guard lock(mutex_);
		return allow_none_;
	}

private:
	struct Snap {
		std::vector<Mode> modes;
		std::vector<std::string> share;
		bool allow_none = false, lock_hidden = false;
		ItemChange change;
		std::string current, previous;
	};

	Snap snap()
	{
		std::lock_guard lock(mutex_);
		Snap s;
		s.modes = modes_;
		s.share = share_;
		s.allow_none = allow_none_;
		s.lock_hidden = lock_hidden_;
		s.change = change_;
		s.current = current_;
		s.previous = previous_;
		return s;
	}

	// The scenes it acts on, each with a reference the caller lets go of:
	// its own, and those listed under "Same modes in scenes" (scenes only,
	// never groups).
	std::vector<obs_source_t *> covered(const std::vector<std::string> &share)
	{
		std::vector<obs_source_t *> scenes;
		obs_source_t *parent = this->parent();
		if (obs_source_t *own = parent ? obs_source_get_ref(parent) : nullptr)
			scenes.push_back(own);
		for (const std::string &name : share) {
			obs_source_t *scene = obs_get_source_by_name(name.c_str());
			if (scene && obs_scene_from_source(scene) && !obs_source_is_group(scene) &&
			    std::find(scenes.begin(), scenes.end(), scene) == scenes.end())
				scenes.push_back(scene);
			else
				obs_source_release(scene);
		}
		return scenes;
	}

	static void release_all(std::vector<obs_source_t *> &scenes)
	{
		for (obs_source_t *scene : scenes)
			obs_source_release(scene);
		scenes.clear();
	}

	static void lock_as_shown(obs_sceneitem_t *item)
	{
		if (item && obs_sceneitem_locked(item) == obs_sceneitem_visible(item))
			obs_sceneitem_set_locked(item, !obs_sceneitem_visible(item));
	}

	// One item to show or hide, held for this call only.
	struct Work {
		obs_sceneitem_t *item;
		obs_source_t *scene;
		bool show;
	};

	void apply(std::vector<Work> &work, const Snap &s, bool cut)
	{
		// Hides before shows, all in this tick, so an item Morph pairs a
		// capture going with the one coming.
		std::stable_sort(work.begin(), work.end(),
				 [](const Work &a, const Work &b) { return !a.show && b.show; });
		applying_ = std::this_thread::get_id();
		for (Work &w : work) {
			if (cut)
				set_item_transitions(w.item, kTransitionCut, 0);
			else
				apply_item_change(w.item, s.change, w.scene);
			set_item_visible(w.item, w.show);
			// The way the next change is made, after a change made as a cut.
			if (cut)
				apply_item_change(w.item, s.change, w.scene);
			if (s.lock_hidden)
				lock_as_shown(w.item);
		}
		applying_ = std::thread::id();
		for (Work &w : work)
			obs_sceneitem_release(w.item);
		work.clear();
	}

	// What changing from mode `from` to mode `to` shows and hides, in every
	// scene covered.
	std::vector<Work> plan(const Snap &s, const std::string &from, const std::string &to,
			       std::vector<obs_source_t *> &scenes)
	{
		const Mode *was = find_mode(s.modes, from);
		const Mode *now = find_mode(s.modes, to);
		std::vector<Work> work;
		for (obs_source_t *scene : scenes)
			each_item(scene, [&](obs_sceneitem_t *item, const char *name) {
				if (!member(s.modes, name))
					return;
				const bool in_now = now && has(now->sources, name);
				// In both: not touched (a camera switched off stays off).
				if (in_now && was && has(was->sources, name))
					return;
				if (obs_sceneitem_visible(item) == in_now)
					return;
				if (in_now && held_elsewhere(scene, item, this))
					return;
				obs_sceneitem_addref(item);
				work.push_back({item, scene, in_now});
			});
		return work;
	}

	void act(const Press &press)
	{
		const Snap s = snap();
		std::string to;
		bool by_key = true;
		switch (press.how) {
		case How::On:
			to = press.mode;
			break;
		case How::Toggle:
			if (press.mode != s.current)
				to = press.mode;
			else if (s.allow_none)
				to.clear();
			else if (find_mode(s.modes, s.previous) && s.previous != s.current)
				to = s.previous;
			else
				return;
			break;
		case How::Next: {
			if (s.modes.empty())
				return;
			size_t at = 0;
			for (size_t i = 0; i < s.modes.size(); i++)
				if (s.modes[i].id == s.current)
					at = (i + 1) % s.modes.size();
			to = s.modes[at].id;
			break;
		}
		case How::None:
			if (!s.allow_none)
				return;
			to.clear();
			break;
		case How::Window:
			by_key = false;
			to = press.mode;
			if (to.empty() && !s.allow_none) {
				write_state(s.current, s.previous); // the window shows the mode on again
				return;
			}
			break;
		}
		if (!to.empty() && !find_mode(s.modes, to))
			return;
		if (to != s.current) {
			std::vector<obs_source_t *> scenes = covered(s.share);
			std::vector<Work> work = plan(s, s.current, to, scenes);
			const size_t count = work.size();
			apply(work, s, false);
			release_all(scenes);
			const std::string previous = s.current.empty() && !s.previous.empty() ? s.previous : s.current;
			write_state(to, previous);
			settling_ = true;
			const Mode *now = find_mode(s.modes, to);
			obs_log(LOG_INFO, "Scene Modes '%s': mode '%s' on, %zu source(s) shown or hidden",
				obs_source_get_name(source_), now ? now->name.c_str() : "(none)", count);
			vendor_emit_scene_mode_changed(source_, to, s.current);
		}
		// "Switch to scene": a key's press turning a mode on, even the one on
		// already, changes scene unless the scene on screen is one of this
		// filter's.
		const Mode *now = find_mode(s.modes, to);
		if (by_key && now && !now->go_to.empty()) {
			GoTo go;
			go.kind = "Scene Modes";
			go.filter = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
			go.target = now->go_to;
			obs_source_t *parent = this->parent();
			if (parent && obs_source_get_name(parent))
				go.stay_in.push_back(obs_source_get_name(parent));
			go.stay_in.insert(go.stay_in.end(), s.share.begin(), s.share.end());
			go_to_scene(std::move(go));
		}
	}

	// The mode on and the one before, kept in the settings (saved with the
	// collection, never copied to another filter).
	void write_state(const std::string &now, const std::string &before)
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		std::lock_guard lock(mutex_);
		current_ = now;
		previous_ = before;
		obs_data_set_string(settings, "mode_now", now.c_str());
		obs_data_set_string(settings, "mode_before", before.c_str());
		obs_data_release(settings);
	}

	// At the first look after loading: the scene shows the saved mode
	// already, as OBS saved each item's eye. Any that disagree (a collection
	// edited by hand or by a script) are put right with a cut.
	void put_right_after_loading()
	{
		const Snap s = snap();
		if (s.current.empty())
			return; // no mode on: nothing to go by
		const Mode *now = find_mode(s.modes, s.current);
		std::vector<obs_source_t *> scenes = covered(s.share);
		std::vector<Work> work;
		for (obs_source_t *scene : scenes)
			each_item(scene, [&](obs_sceneitem_t *item, const char *name) {
				if (!member(s.modes, name))
					return;
				const bool want = now && has(now->sources, name);
				if (obs_sceneitem_visible(item) == want)
					return;
				if (want && held_elsewhere(scene, item, this))
					return;
				obs_sceneitem_addref(item);
				work.push_back({item, scene, want});
			});
		const size_t count = work.size();
		apply(work, s, true);
		release_all(scenes);
		if (count)
			obs_log(LOG_INFO, "Scene Modes '%s': put %zu source(s) right for mode '%s' after loading",
				obs_source_get_name(source_), count, now ? now->name.c_str() : "");
	}

	// A member of a mode that is not on, shown by something else (the eye, an
	// old Source key), is hidden again. Hiding a member of the mode on is
	// left alone: a camera switched off stays off.
	void put_back(const std::vector<Seen> &seen)
	{
		const Snap s = snap();
		const Mode *now = find_mode(s.modes, s.current);
		std::vector<Work> work;
		std::vector<obs_sceneitem_t *> relock;
		for (const Seen &one : seen) {
			obs_source_t *scene = obs_get_source_by_name(one.scene.c_str());
			obs_scene_t *sc = scene ? obs_scene_from_source(scene) : nullptr;
			obs_sceneitem_t *item = sc ? obs_scene_find_sceneitem_by_id(sc, one.item) : nullptr;
			const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
			if (item && member(s.modes, name)) {
				const bool shown = obs_sceneitem_visible(item);
				const bool belongs = now && has(now->sources, name);
				obs_sceneitem_addref(item);
				if (shown && !belongs)
					work.push_back({item, obs_source_get_ref(scene), false});
				else
					relock.push_back(item);
			}
			obs_source_release(scene);
		}
		if (!work.empty())
			obs_log(LOG_INFO, "Scene Modes '%s': hid %zu source(s) of a mode that is not on",
				obs_source_get_name(source_), work.size());
		std::vector<obs_source_t *> scenes;
		for (Work &w : work)
			scenes.push_back(w.scene);
		apply(work, s, false);
		release_all(scenes);
		for (obs_sceneitem_t *item : relock) {
			if (s.lock_hidden)
				lock_as_shown(item);
			obs_sceneitem_release(item);
		}
	}

	// The members hidden now are locked: when the option is switched on,
	// after loading and when the filter is switched on again.
	void lock_sweep()
	{
		const Snap s = snap();
		if (!s.lock_hidden)
			return;
		std::vector<obs_source_t *> scenes = covered(s.share);
		for (obs_source_t *scene : scenes)
			each_item(scene, [&](obs_sceneitem_t *item, const char *name) {
				if (member(s.modes, name) && !obs_sceneitem_visible(item) &&
				    !obs_sceneitem_locked(item))
					obs_sceneitem_set_locked(item, true);
			});
		release_all(scenes);
	}

	void end_stale()
	{
		const Snap s = snap();
		std::vector<obs_source_t *> scenes = covered(s.share);
		for (obs_source_t *scene : scenes) {
			if (obs_source_showing(scene))
				continue;
			each_item(scene, [&](obs_sceneitem_t *item, const char *name) {
				if (member(s.modes, name) && end_stale_transition(item))
					obs_log(LOG_INFO, "Scene Modes '%s': ended a transition left under way on '%s'",
						obs_source_get_name(source_), name);
			});
		}
		release_all(scenes);
	}

	// Listens to the scenes listed under "Same modes in scenes", each held
	// weakly; scenes made later are found within two seconds. Only while
	// there are any.
	void watch_shared()
	{
		std::vector<std::string> share;
		{
			std::lock_guard lock(mutex_);
			share = share_;
		}
		if (share.empty()) {
			unwatch();
			return;
		}
		obs_source_t *parent = this->parent();
		for (const std::string &name : share) {
			obs_source_t *scene = obs_get_source_by_name(name.c_str());
			bool known = !scene || scene == parent || !obs_scene_from_source(scene);
			for (obs_weak_source_t *weak : watched_)
				known = known || obs_weak_source_references_source(weak, scene);
			if (!known) {
				signal_handler_connect(obs_source_get_signal_handler(scene), "item_visible", visible_cb,
						       this);
				watched_.push_back(obs_source_get_weak_source(scene));
			}
			obs_source_release(scene);
		}
	}

	void unwatch()
	{
		for (obs_weak_source_t *weak : watched_) {
			if (obs_source_t *scene = obs_weak_source_get_source(weak)) {
				signal_handler_disconnect(obs_source_get_signal_handler(scene), "item_visible",
							  visible_cb, this);
				obs_source_release(scene);
			}
			obs_weak_source_release(weak);
		}
		watched_.clear();
	}

	static void visible_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<SceneModes *>(data);
		if (self->applying_.load() == std::this_thread::get_id())
			return; // its own doing
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		auto *scene = static_cast<obs_scene_t *>(calldata_ptr(cd, "scene"));
		const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
		const char *scene_name = scene ? obs_source_get_name(obs_scene_get_source(scene)) : nullptr;
		if (!name || !scene_name)
			return;
		std::lock_guard lock(self->mutex_);
		if (member(self->modes_, name))
			self->pending_.push_back(
				{scene_name, obs_sceneitem_get_id(item), calldata_bool(cd, "visible")});
	}

	// ---- hotkeys: one per mode, "next", and "no mode" while allowed. On
	// the scene, named after the filter and the mode's id, so a renamed mode
	// keeps its binding. Their callbacks only queue a press.
	struct Key {
		SceneModes *self;
		std::string mode;
		How how;
		obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	};

	static void key_cb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
	{
		auto *k = static_cast<Key *>(data);
		std::string why;
		if (pressed && !k->self->queue(k->mode, k->how, why))
			obs_log(LOG_INFO, "Scene Modes '%s': key not acted on: %s",
				obs_source_get_name(k->self->source_), why.c_str());
	}

	std::string key_text(const char *locale, const std::string &mode_name)
	{
		std::string text = obs_module_text(locale);
		const char *filter = obs_source_get_name(source_);
		size_t at = text.find("%1");
		if (at != std::string::npos)
			text.replace(at, 2, filter ? filter : "");
		at = text.find("%2");
		if (at != std::string::npos)
			text.replace(at, 2, mode_name);
		return text;
	}

	void sync_hotkeys()
	{
		obs_source_t *parent = this->parent();
		std::vector<Mode> modes;
		bool allow_none;
		{
			std::lock_guard lock(mutex_);
			modes = modes_;
			allow_none = allow_none_;
		}
		std::lock_guard lock(keys_mutex_);
		if (!parent) {
			return;
		}
		const std::string filter = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
		std::map<std::string, std::pair<std::string, How>> wanted; // hotkey name -> (mode, how)
		std::map<std::string, std::string> texts;
		for (const Mode &mode : modes) {
			const std::string name = "stagekit.mode." + filter + "." + mode.id;
			wanted[name] = {mode.id, How::On};
			texts[name] = key_text("Hotkey.Modes.On", mode.name);
		}
		const std::string next = "stagekit.mode." + filter + ".next";
		wanted[next] = {"", How::Next};
		texts[next] = key_text("Hotkey.Modes.Next", "");
		if (allow_none) {
			const std::string none = "stagekit.mode." + filter + ".none";
			wanted[none] = {"", How::None};
			texts[none] = key_text("Hotkey.Modes.None", "");
		}
		for (auto it = keys_.begin(); it != keys_.end();) {
			const auto want = wanted.find(it->first);
			if (want == wanted.end() || keys_parent_ != parent) {
				obs_hotkey_unregister(it->second->id);
				it = keys_.erase(it);
				continue;
			}
			obs_hotkey_set_description(it->second->id, texts[it->first].c_str());
			++it;
		}
		keys_parent_ = parent;
		for (const auto &[name, what] : wanted) {
			if (keys_.count(name))
				continue;
			auto k = std::make_unique<Key>();
			k->self = this;
			k->mode = what.first;
			k->how = what.second;
			k->id = obs_hotkey_register_source(parent, name.c_str(), texts[name].c_str(), key_cb, k.get());
			keys_[name] = std::move(k);
		}
	}

	void unregister_hotkeys()
	{
		std::lock_guard lock(keys_mutex_);
		for (auto &[name, k] : keys_)
			if (k->id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(k->id);
		keys_.clear();
		keys_parent_ = nullptr;
	}

	void emit_changed()
	{
		std::string now;
		{
			std::lock_guard lock(mutex_);
			now = current_;
		}
		vendor_emit_scene_mode_changed(source_, now, now);
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr; // under g_modes_mutex
	std::mutex mutex_;
	std::vector<Mode> modes_;
	std::vector<std::string> share_;
	bool allow_none_ = false, lock_hidden_ = false;
	ItemChange change_;
	std::string current_, previous_; // mode ids
	bool first_update_ = true;
	std::vector<Press> presses_;
	std::vector<Seen> pending_;
	bool sweep_ = false; // lock what is hidden: the option was just switched on

	// The tick's own.
	std::vector<obs_weak_source_t *> watched_;
	double watch_timer_ = 0.0, stale_timer_ = 1.0;
	bool ready_ = false;  // looked at once after loading
	bool was_on_ = false; // enabled and loaded at the last tick

	std::atomic<bool> settling_{false};
	std::atomic<bool> emit_{false};
	std::atomic<std::thread::id> applying_{};

	std::mutex keys_mutex_; // never held with mutex_
	std::map<std::string, std::unique_ptr<Key>> keys_;
	obs_source_t *keys_parent_ = nullptr;
};

std::vector<ModesLook> looks_of_enabled(const void *except)
{
	std::vector<ModesLook> looks;
	std::lock_guard lock(g_modes_mutex);
	for (SceneModes *each : g_modes)
		if (each != except && obs_source_enabled(each->source()))
			looks.push_back(each->look());
	return looks;
}

void on_source_rename(void *, calldata_t *cd)
{
	const char *was = calldata_string(cd, "prev_name");
	const char *now = calldata_string(cd, "new_name");
	if (!was || !now || !*was || !*now)
		return;
	std::vector<obs_source_t *> filters;
	{
		std::lock_guard lock(g_modes_mutex);
		for (SceneModes *each : g_modes)
			if (obs_source_t *held = obs_source_get_ref(each->source()))
				filters.push_back(held);
	}
	for (obs_source_t *filter : filters) {
		obs_data_t *settings = obs_source_get_settings(filter);
		bool changed = false;
		std::vector<std::string> lists;
		for (const std::string &id : pick_list_read(settings, "modes", false))
			lists.push_back(key(id, "sources"));
		lists.push_back("share_scenes");
		for (const std::string &list : lists) {
			std::vector<std::string> names = pick_list_read(settings, list.c_str(), false);
			bool here = false;
			for (std::string &name : names)
				if (name == was) {
					name = now;
					here = true;
				}
			if (here) {
				pick_list_write(settings, list.c_str(), false, names);
				changed = true;
			}
		}
		for (const std::string &id : pick_list_read(settings, "modes", false)) {
			const std::string go = key(id, "go_to");
			if (strcmp(obs_data_get_string(settings, go.c_str()), was) == 0) {
				obs_data_set_string(settings, go.c_str(), now);
				changed = true;
			}
		}
		if (changed)
			obs_source_update(filter, settings);
		obs_data_release(settings);
		obs_source_release(filter);
	}
}

// ---------------------------------------------------------------- window

void scene_names(std::vector<std::string> &names)
{
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			const char *name = obs_source_get_name(scene);
			if (name && *name && !obs_source_is_group(scene))
				static_cast<std::vector<std::string> *>(param)->push_back(name);
			return true;
		},
		&names);
}

std::string group_title(const std::string &name)
{
	std::string text = obs_module_text("Modes.Group");
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, name);
	return text;
}

void fill_mode_now(obs_property_t *list, obs_data_t *settings)
{
	obs_property_list_clear(list);
	const std::string now = obs_data_get_string(settings, "mode_now");
	if (obs_data_get_bool(settings, "allow_none") || now.empty())
		obs_property_list_add_string(list, obs_module_text("Modes.None"), "");
	for (const Mode &mode : read_modes(settings))
		obs_property_list_add_string(list, mode.name.c_str(), mode.id.c_str());
}

void add_modes_and_rest(obs_properties_t *props, obs_source_t *owner, obs_data_t *settings);

// Takes off everything after the template button, and adds it again: the
// modes' groups, then the controls under them.
void rebuild(obs_properties_t *props, obs_source_t *owner, obs_data_t *settings)
{
	std::vector<std::string> after;
	bool past = false;
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		const char *name = obs_property_name(p);
		if (past)
			after.push_back(name);
		else if (name && strcmp(name, "add_template") == 0)
			past = true;
	}
	for (const std::string &name : after)
		obs_properties_remove_by_name(props, name.c_str());
	if (obs_property_t *now = obs_properties_get(props, "mode_now"))
		fill_mode_now(now, settings);
	add_modes_and_rest(props, owner, settings);
}

std::string new_id(obs_data_t *settings)
{
	long long next = std::max<long long>(1, obs_data_get_int(settings, "mode_seq"));
	for (const std::string &id : pick_list_read(settings, "modes", false))
		if (id.size() > 1 && id[0] == 'm')
			next = std::max(next, std::atoll(id.c_str() + 1) + 1);
	obs_data_set_int(settings, "mode_seq", next + 1);
	return "m" + std::to_string(next);
}

void add_mode(obs_source_t *owner, obs_data_t *settings, const std::string &name, const std::string &type)
{
	const std::string id = new_id(settings);
	std::vector<std::string> ids = pick_list_read(settings, "modes", false);
	ids.push_back(id);
	pick_list_write(settings, "modes", false, ids);
	obs_data_set_string(settings, key(id, "name").c_str(), name.c_str());
	obs_data_set_string(settings, key(id, "type").c_str(), type.c_str());
	obs_data_set_string(settings, key(id, "go_to").c_str(), "");
	obs_source_update(owner, settings);
}

bool add_mode_cb(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	if (!owner)
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	const std::string name = trimmed(obs_data_get_string(settings, "new_mode"));
	bool done = false;
	if (!name.empty()) {
		obs_data_set_string(settings, "new_mode", "");
		add_mode(owner, settings, name, "");
		rebuild(props, owner, settings);
		done = true;
	}
	obs_data_release(settings);
	return done;
}

bool add_template_cb(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	if (!owner)
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	const long long which = obs_data_get_int(settings, "template");
	bool done = false;
	if (which >= 0 && which < (long long)(sizeof(kTemplates) / sizeof(kTemplates[0]))) {
		add_mode(owner, settings, obs_module_text(kTemplates[which].name), kTemplates[which].type);
		rebuild(props, owner, settings);
		done = true;
	}
	obs_data_release(settings);
	return done;
}

// "mode|<id>|up" and "mode|<id>|remove".
std::string id_of(obs_property_t *property)
{
	const std::string name = obs_property_name(property);
	const size_t first = name.find('|'), last = name.rfind('|');
	return first == std::string::npos || last <= first ? std::string() : name.substr(first + 1, last - first - 1);
}

bool move_up_cb(obs_properties_t *props, obs_property_t *property, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	const std::string id = id_of(property);
	if (!owner || id.empty())
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	std::vector<std::string> ids = pick_list_read(settings, "modes", false);
	const auto at = std::find(ids.begin(), ids.end(), id);
	bool moved = false;
	if (at != ids.end() && at != ids.begin()) {
		std::iter_swap(at, at - 1);
		pick_list_write(settings, "modes", false, ids);
		obs_source_update(owner, settings);
		rebuild(props, owner, settings);
		moved = true;
	}
	obs_data_release(settings);
	return moved;
}

bool remove_cb(obs_properties_t *props, obs_property_t *property, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	const std::string id = id_of(property);
	if (!owner || id.empty())
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	std::vector<std::string> ids = pick_list_read(settings, "modes", false);
	ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
	pick_list_write(settings, "modes", false, ids);
	for (const char *part : {"name", "type", "go_to", "sources"})
		obs_data_erase(settings, key(id, part).c_str());
	// Its sources are left as they are, no longer looked after.
	if (id == obs_data_get_string(settings, "mode_now"))
		obs_data_set_string(settings, "mode_now", "");
	if (id == obs_data_get_string(settings, "mode_before"))
		obs_data_set_string(settings, "mode_before", "");
	obs_source_update(owner, settings);
	rebuild(props, owner, settings);
	obs_data_release(settings);
	return true;
}

// A name typed: the mode's heading and the "Mode now" list follow at once.
bool renamed_cb(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)priv;
	const std::string id = id_of(property);
	if (id.empty())
		return false;
	std::string name = trimmed(obs_data_get_string(settings, key(id, "name").c_str()));
	if (name.empty())
		name = id;
	if (obs_property_t *group = obs_properties_get(props, key(id, "g").c_str()))
		obs_property_set_description(group, group_title(name).c_str());
	if (obs_property_t *now = obs_properties_get(props, "mode_now"))
		fill_mode_now(now, settings);
	return true;
}

bool allow_none_cb(void *, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	if (obs_property_t *now = obs_properties_get(props, "mode_now"))
		fill_mode_now(now, settings);
	return true;
}

void add_modes_and_rest(obs_properties_t *props, obs_source_t *owner, obs_data_t *settings)
{
	obs_source_t *parent = owner ? obs_filter_get_parent(owner) : nullptr;
	std::vector<std::string> scenes;
	scene_names(scenes);
	const std::vector<Mode> modes = settings ? read_modes(settings) : std::vector<Mode>();
	for (size_t i = 0; i < modes.size(); i++) {
		const Mode &mode = modes[i];
		obs_properties_t *content = obs_properties_create();

		obs_property_t *name = obs_properties_add_text(content, key(mode.id, "name").c_str(),
							       obs_module_text("Modes.ModeName"), OBS_TEXT_DEFAULT);
		obs_property_set_long_description(name, obs_module_text("Modes.ModeName.Tip"));
		obs_property_set_modified_callback2(name, renamed_cb, owner);

		obs_property_t *type = obs_properties_add_list(content, key(mode.id, "type").c_str(),
							       obs_module_text("Modes.Type"), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
		for (const TypeInfo &t : kTypes)
			obs_property_list_add_string(type, obs_module_text(t.text), t.id);
		obs_property_set_long_description(type, obs_module_text("Modes.Type.Tip"));

		obs_property_t *go_to = obs_properties_add_list(content, key(mode.id, "go_to").c_str(),
								obs_module_text("Modes.GoTo"), OBS_COMBO_TYPE_LIST,
								OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(go_to, obs_module_text("Modes.GoTo.Stay"), "");
		std::vector<std::string> targets = scenes;
		// A scene chosen before and gone since stays chosen, by name.
		if (!mode.go_to.empty() && std::find(targets.begin(), targets.end(), mode.go_to) == targets.end())
			targets.push_back(mode.go_to);
		for (const std::string &scene : targets)
			obs_property_list_add_string(go_to, scene.c_str(), scene.c_str());
		obs_property_set_long_description(go_to, obs_module_text("Modes.GoTo.Tip"));

		const std::string sources_setting = key(mode.id, "sources");
		PickList sources;
		sources.setting = sources_setting.c_str();
		sources.title = "Modes.Sources";
		sources.add = "Modes.Sources.Add";
		sources.tip = "Modes.Sources.Tip";
		sources.choices = [parent](std::vector<std::string> &names) {
			scene_item_names(parent, names);
		};
		pick_list_add(content, owner, sources);

		if (i > 0)
			obs_properties_add_button2(content, key(mode.id, "up").c_str(), obs_module_text("Modes.MoveUp"),
						   move_up_cb, owner);
		obs_property_t *remove = obs_properties_add_button2(content, key(mode.id, "remove").c_str(),
								    obs_module_text("Modes.Remove"), remove_cb, owner);
		obs_property_set_long_description(remove, obs_module_text("Modes.Remove.Tip"));

		obs_properties_add_group(props, key(mode.id, "g").c_str(), group_title(mode.name).c_str(),
					 OBS_GROUP_NORMAL, content);
	}

	obs_property_t *none = obs_properties_add_bool(props, "allow_none", obs_module_text("Modes.AllowNone"));
	obs_property_set_long_description(none, obs_module_text("Modes.AllowNone.Tip"));
	obs_property_set_modified_callback2(none, allow_none_cb, owner);

	PickList share;
	share.setting = "share_scenes";
	share.title = "Modes.Share";
	share.add = "List.AddScene";
	share.tip = "Modes.Share.Tip";
	share.choices = [parent](std::vector<std::string> &names) {
		scene_names(names);
		const char *own = parent ? obs_source_get_name(parent) : nullptr;
		if (own)
			names.erase(std::remove(names.begin(), names.end(), own), names.end());
	};
	pick_list_add(props, owner, share);

	obs_property_t *lock = obs_properties_add_bool(props, "lock_hidden", obs_module_text("Modes.LockHidden"));
	obs_property_set_long_description(lock, obs_module_text("Modes.LockHidden.Tip"));

	add_item_change_properties(props, owner, nullptr);
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneModes *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = owner ? obs_source_get_settings(owner) : nullptr;
	obs_source_t *parent = owner ? obs_filter_get_parent(owner) : nullptr;

	if (parent && obs_source_is_group(parent)) {
		obs_property_t *on_group =
			obs_properties_add_text(props, "on_group", obs_module_text("Modes.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(on_group, true);
	}

	obs_property_t *now = obs_properties_add_list(props, "mode_now", obs_module_text("Modes.Now"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(now, obs_module_text("Modes.Now.Tip"));
	if (settings)
		fill_mode_now(now, settings);

	obs_property_t *typed =
		obs_properties_add_text(props, "new_mode", obs_module_text("Modes.NewName"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(typed, obs_module_text("Modes.NewName.Tip"));
	obs_property_t *add =
		obs_properties_add_button2(props, "add_mode", obs_module_text("Modes.Add"), add_mode_cb, owner);
	obs_property_set_long_description(add, obs_module_text("Modes.Add.Tip"));

	obs_property_t *templates = obs_properties_add_list(props, "template", obs_module_text("Modes.Template"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(templates, obs_module_text("Modes.Template.Choose"), -1);
	for (size_t i = 0; i < sizeof(kTemplates) / sizeof(kTemplates[0]); i++)
		obs_property_list_add_int(templates, obs_module_text(kTemplates[i].name), (long long)i);
	obs_property_set_long_description(templates, obs_module_text("Modes.Template.Tip"));
	obs_properties_add_button2(props, "add_template", obs_module_text("Modes.AddTemplate"), add_template_cb, owner);

	add_modes_and_rest(props, owner, settings);
	obs_data_release(settings);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "allow_none", false);
	obs_data_set_default_bool(settings, "lock_hidden", false);
	obs_data_set_default_string(settings, "mode_now", "");
	obs_data_set_default_string(settings, "mode_before", "");
	obs_data_set_default_int(settings, "template", -1);
	item_change_defaults(settings);
}

// The filters there are, with a reference each the caller lets go of.
std::vector<obs_source_t *> filters_on(const char *scene, const char *filter)
{
	std::vector<obs_source_t *> out;
	std::lock_guard lock(g_modes_mutex);
	for (SceneModes *each : g_modes) {
		obs_source_t *parent = each->look().parent;
		const char *scene_name = parent ? obs_source_get_name(parent) : nullptr;
		if (!scene_name || (scene && *scene && strcmp(scene, scene_name) != 0))
			continue;
		if (filter && *filter && strcmp(filter, obs_source_get_name(each->source())) != 0)
			continue;
		if (obs_source_t *held = obs_source_get_ref(each->source()))
			out.push_back(held);
	}
	return out;
}

} // namespace

std::vector<SceneModesLook> scene_modes_list(const char *scene)
{
	std::vector<SceneModesLook> out;
	std::lock_guard lock(g_modes_mutex);
	for (SceneModes *each : g_modes) {
		SceneModesLook look = each->vendor_look();
		if (look.scene.empty() || (scene && *scene && look.scene != scene))
			continue;
		out.push_back(std::move(look));
	}
	return out;
}

bool scene_modes_press(const char *scene, const char *filter, const char *mode, const char *how, std::string &why)
{
	if (!scene || !*scene) {
		why = "scene is required";
		return false;
	}
	const std::string kind = how && *how ? how : "on";
	How h;
	if (kind == "on")
		h = How::On;
	else if (kind == "toggle")
		h = How::Toggle;
	else if (kind == "next")
		h = How::Next;
	else if (kind == "none")
		h = How::None;
	else {
		why = "how must be on, toggle, next or none";
		return false;
	}
	std::vector<obs_source_t *> found = filters_on(scene, filter);
	if (found.empty()) {
		why = filter && *filter ? "No Scene Modes with that name on that scene"
					: "No Scene Modes on that scene";
		return false;
	}
	bool done = false;
	if (void *data = obs_obj_get_data(found[0]))
		done = static_cast<SceneModes *>(data)->queue(mode ? mode : "", h, why);
	for (obs_source_t *f : found)
		obs_source_release(f);
	return done;
}

bool scene_modes_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item)
{
	const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
	if (!name || !scene)
		return false;
	for (const ModesLook &look : looks_of_enabled(nullptr))
		if (covers(look, scene) && look_holds(look, name))
			return true;
	return false;
}

bool scene_modes_settling(obs_source_t *scene, obs_sceneitem_t *item)
{
	const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
	if (!name || !scene)
		return false;
	for (const ModesLook &look : looks_of_enabled(nullptr))
		if (look.settling && covers(look, scene) && member(look.modes, name))
			return true;
	return false;
}

void register_scene_modes()
{
	obs_source_info info = {};
	info.id = kSceneModesId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Listed with a scene's effect filters; it never changes the picture.
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneModes.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneModes(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<SceneModes *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneModes *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneModes *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<SceneModes *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		// On a group it does nothing: its place is the scene holding the group.
		if (!obs_source_is_group(parent))
			static_cast<SceneModes *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneModes *>(data)->detach();
	};
	register_timed(&info);
	holds_hidden_register(scene_modes_holds_hidden, scene_modes_settling);
	go_to_scene_add_test_calls();
}

} // namespace stagekit
