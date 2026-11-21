/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Show in Scenes: one switch for showing and hiding sources across scenes.
//
// The filter goes on any source and is the switch: a Stream Deck Filter key,
// the eye beside it or a hotkey turns it on and off. Turned on, the listed
// sources are shown in the scenes chosen (or hidden, if set the other way);
// turned off, the opposite. With no source listed it acts on the source it
// is on; with "every scene", wherever those sources are.
//
// It replaces a Stream Deck multi-action holding one Source Visibility step
// for every scene. It acts when it is switched, not all the time, so an item
// shown or hidden by hand stays that way until the next press.
//
// "Follow what is shown" turns it round as well: the switch reads on or off
// by what is shown in its scenes, so a key's light stays true when something
// else (a Link Visibility, the eye) shows or hides its sources. Correcting
// the switch never shows or hides anything.
//
// An item that a Link Visibility set to "Follow the first" holds hidden (it
// belongs to a mode of its scene that is not showing) is passed over when
// the switch shows things, and does not count for "Follow what is shown".
//
// "Switch to scene" (`go_to`, empty as it comes): a press that shows the
// sources (turns the switch on, or off when "On means" is hidden), once they
// are shown, also changes OBS to that scene with OBS's current transition
// (the preview in Studio Mode), unless the scene on screen already has one
// of the sources. It goes by the scene's name, so it works with any
// collection. Not on a press hiding them, not for "Do it now", not when
// "Follow what is shown" corrects the switch, not for the state restored at
// load.
//
// "Hide shows Standby Image" (`hide_standby`, off as it comes): a press that
// hides the sources covers each listed source that has an enabled Standby
// Image filter with its image instead (that filter's own transition; the
// items stay shown, so nothing round them moves), and a press that shows
// them takes the image away. A covered source counts as hidden for "Follow
// what is shown". The cover is the source's, so it shows in every scene.
// A Stream Deck key can choose for its own presses ("ShowInScenesPress",
// `hide_as`: as the filter says, hide, or cover); a show undoes what the
// hide did, whichever way that was (`covered_`: the sources this filter
// covered).
//
// "Show only what it hid" (`restore_hidden`): a press that
// hides notes each item it actually hid (its scene's name, the name of the
// scene or group holding it and its id, never the item itself) and each
// source it covered, in its own settings (`hidden_record`, kept over a
// restart), and a press that shows brings back only those, and only while
// they are still hidden; with nothing noted it does nothing. Nothing hidden
// by anything else is ever shown by it. The switch then means "has hidden
// things": on (with "When this is on" shown) while nothing it hid is still
// hidden, so a toggle key always hides and then restores. "Follow what is
// shown" puts the switch that way, and forgets a noted item once something
// else has shown it (or it is gone).

#include "show-in-scenes.hpp"
#include "../common/filter-input.hpp"
#include "../common/new-filter-settings.hpp"
#include "../common/timing.hpp"

#include "place-item.hpp"

#include "../capture/standby.hpp"

#include "../common/go-to-scene.hpp"
#include "../common/holds-hidden.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-rev.hpp"
#include "../common/tooltip.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <callback/proc.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace stagekit {

namespace {

// How a press hides the sources: as the filter's "Hide shows Standby Image"
// says, or, for one key's presses, by hiding them or covering them.
enum class HideAs { Filter, Hide, Cover };

enum class Where : int {
	Every = 0, // saved: append only
	Listed = 1,
};

// Goes through the items showing one of `sources` in the scenes covered:
// every scene, or the ones named. A group's own items are reached through the
// scene that holds the group, and `scene` is that scene for them too.
//
// The items are found first, each held, and `act` is called for them after:
// never while a scene is gone through. Going through a scene holds its locks
// (and OBS's list of sources), and `act` changes transitions and shows and
// hides items, which makes and unmakes sources and runs every listener's
// handler: with those locks held, that is how two threads come to wait for
// each other for good.
void each_item(const std::vector<std::string> &sources, const std::vector<std::string> &scenes, bool every,
	       const std::function<void(obs_sceneitem_t *item, const char *name, obs_source_t *scene)> &act)
{
	struct Found {
		obs_sceneitem_t *item; // held
		obs_source_t *scene;   // held
	};
	struct Work {
		const std::vector<std::string> *sources, *scenes;
		bool every;
		std::vector<Found> found;
		obs_source_t *scene = nullptr; // the scene being gone through
		void look(obs_sceneitem_t *item)
		{
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			if (!name || std::find(sources->begin(), sources->end(), name) == sources->end())
				return;
			obs_sceneitem_addref(item);
			found.push_back({item, obs_source_get_ref(scene)});
		}
	} work{&sources, &scenes, every, {}};
	static const auto each = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		static_cast<Work *>(param)->look(item);
		if (obs_sceneitem_is_group(item))
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
					static_cast<Work *>(p)->look(inner);
					return true;
				},
				param);
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			auto *w = static_cast<Work *>(param);
			// OBS lists each group as a scene too; its items are reached
			// through the scene that holds it.
			if (obs_source_is_group(scene))
				return true;
			const char *name = obs_source_get_name(scene);
			if (!w->every &&
			    (!name || std::find(w->scenes->begin(), w->scenes->end(), name) == w->scenes->end()))
				return true;
			w->scene = scene;
			obs_scene_enum_items(obs_scene_from_source(scene), each, param);
			return true;
		},
		&work);
	for (const Found &f : work.found) {
		// (Taken out of its scene, or its scene on its way out, meanwhile.)
		const char *name = obs_source_get_name(obs_sceneitem_get_source(f.item));
		if (f.scene && name && obs_sceneitem_get_scene(f.item))
			act(f.item, name, f.scene);
	}
	for (const Found &f : work.found) {
		obs_sceneitem_release(f.item);
		obs_source_release(f.scene);
	}
}

// Adds to `sources` the items their Send To filters bring along, and those
// Place Item filters that move them bring along.
void add_linked(std::vector<std::string> &sources)
{
	const std::vector<std::string> leaders = sources;
	auto add = [&sources](const char *name) {
		if (name && *name && std::find(sources.begin(), sources.end(), name) == sources.end())
			sources.push_back(name);
	};
	for (const std::string &leader : leaders) {
		obs_source_t *found = obs_get_source_by_name(leader.c_str());
		if (!found)
			continue;
		std::vector<std::string> linked;
		obs_source_enum_filters(
			found,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				const char *id = obs_source_get_unversioned_id(filter);
				if (!id || strcmp(id, kSendToId) != 0)
					return;
				obs_data_t *settings = obs_source_get_settings(filter);
				static_cast<std::vector<std::string> *>(param)->push_back(
					obs_data_get_string(settings, "linked"));
				obs_data_release(settings);
			},
			&linked);
		obs_source_release(found);
		for (const std::string &name : linked)
			add(name.c_str());
	}
	struct Scan {
		const std::vector<std::string> *leaders;
		std::vector<std::string> linked;
	} scan{&leaders, {}};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			obs_source_enum_filters(
				scene,
				[](obs_source_t *, obs_source_t *filter, void *p) {
					auto *sc = static_cast<Scan *>(p);
					const char *id = obs_source_get_unversioned_id(filter);
					if (!id || strcmp(id, kPlaceItemId) != 0)
						return;
					obs_data_t *settings = obs_source_get_settings(filter);
					const char *moved = obs_data_get_string(settings, "item");
					if (std::find(sc->leaders->begin(), sc->leaders->end(), moved) !=
					    sc->leaders->end())
						sc->linked.push_back(obs_data_get_string(settings, "linked"));
					obs_data_release(settings);
				},
				param);
			return true;
		},
		&scan);
	for (const std::string &name : scan.linked)
		add(name.c_str());
}

class ShowInScenes {
public:
	ShowInScenes(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		assumed_new_ = !has_saved_settings(settings);
		if (assumed_new_)
			obs_data_set_bool(settings, "restore_hidden", true);
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", enabled_cb, this);
	}

	~ShowInScenes()
	{
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", enabled_cb, this);
	}

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> sources = pick_list_read(settings, "sources", false);
		const std::vector<std::string> scenes = pick_list_read(settings, "scenes", false);
		std::lock_guard lock(mutex_);
		sources_ = sources;
		scenes_ = scenes;
		where_ = static_cast<Where>(std::clamp((int)obs_data_get_int(settings, "where"), 0, 1));
		on_hides_ = obs_data_get_int(settings, "on_means") == 1;
		with_linked_ = obs_data_get_bool(settings, "with_linked");
		follow_ = obs_data_get_bool(settings, "follow");
		hide_standby_ = obs_data_get_bool(settings, "hide_standby");
		restore_ = obs_data_get_bool(settings, "restore_hidden");
		record_ = read_record(obs_data_get_string(settings, "hidden_record"));
		go_to_ = obs_data_get_string(settings, "go_to");
		look_again_ = true;
		// How items change: the usual way, and each listed source's own.
		change_ = item_change_read(settings);
		own_change_.clear();
		for (const std::string &name : sources)
			own_change_[name] = item_change_read(settings, name.c_str());
	}

	// The state OBS restores when it loads a collection is not a press.
	// A press is acted on here, in the tick, not on the thread that made it
	// (a Stream Deck key's comes from obs-websocket's): a Link Visibility
	// acts on the same items from its tick, and two threads setting an
	// item's transitions at once could each let go of the other's. It also
	// keeps "Follow what is shown" from looking at a press half done.
	void tick(float seconds)
	{
		ready_ = true;
		const int press = press_.exchange(-1);
		if (press >= 0)
			apply((press & kPressOn) != 0, (press & kPressHide)    ? HideAs::Hide
						       : (press & kPressCover) ? HideAs::Cover
									       : HideAs::Filter);
		// A press that showed the sources also changes scene, when set to.
		if (press >= 0 && !(press & kPressStay) && scenes_loaded())
			switch_scene((press & kPressOn) != 0);
		if (press >= 0)
			look_again_ = true;
		if (!follow_staggered_) {
			// Each switch on a quarter second of its own, so a dozen of
			// them do not all look in the same frame.
			follow_staggered_ = true;
			follow_timer_ = stagger_phase(source_, 0.25);
		}
		if ((follow_timer_ -= seconds) > 0.0)
			return;
		follow_timer_ = 0.25;
		// Only when something it goes by may have changed: an item in any
		// scene shown, hidden, added, removed (`scene_revision`), a press,
		// its settings, loading, or a look that found a mode settling. And
		// every two seconds anyway, for what no signal tells of (a filter
		// that holds items hidden switched on or off).
		const uint64_t rev = scene_revision();
		const bool loaded = scenes_loaded();
		follow_safety_ -= 0.25;
		if (!look_again_.exchange(false) && rev == follow_rev_ && loaded == follow_loaded_ &&
		    follow_safety_ > 0.0)
			return;
		follow_rev_ = rev;
		follow_loaded_ = loaded;
		follow_safety_ = 2.0;
		follow();
	}

	// "Follow what is shown": the switch is put the way its sources are in
	// the scenes it covers, shown only when every one of their items is. That
	// only corrects the switch: nothing is shown or hidden for it.
	void follow()
	{
		std::vector<std::string> sources, scenes;
		Where where;
		bool on_hides, hide_standby;
		{
			std::lock_guard lock(mutex_);
			if (!follow_)
				return;
			sources = sources_;
			scenes = scenes_;
			where = where_;
			on_hides = on_hides_;
			hide_standby = hide_standby_;
		}
		count_look(Look::SwitchFollow);
		// (Not while OBS loads its scenes: only some have their items yet.)
		if (!scenes_loaded())
			return;
		bool restore;
		{
			std::lock_guard lock(mutex_);
			restore = restore_;
		}
		if (restore) {
			// "Show only what it hid": on while nothing it hid is still
			// hidden (what something else has shown since is forgotten).
			const size_t held = prune_record();
			const bool on = (held == 0) != on_hides;
			if (obs_source_enabled(source_) == on)
				return;
			correcting_ = std::this_thread::get_id();
			obs_source_set_enabled(source_, on);
			correcting_ = std::thread::id();
			obs_log(LOG_INFO, "Show in Scenes '%s': now reads %s, as %zu item(s) it hid are still hidden",
				obs_source_get_name(source_), on ? "on" : "off", held);
			return;
		}
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (sources.empty() && parent && obs_source_get_name(parent))
			sources.push_back(obs_source_get_name(parent));
		// (A source covered with its Standby Image is hidden, as a press sees it.)
		// (Those this filter covered too, still covered now.)
		std::vector<std::string> candidates = hide_standby ? sources : std::vector<std::string>();
		{
			std::lock_guard lock(mutex_);
			candidates.insert(candidates.end(), covered_.begin(), covered_.end());
		}
		const std::set<std::string> covered = with_standby(candidates, true);
		int found = 0, shown = 0;
		bool settling = false;
		each_item(sources, scenes, where == Where::Every,
			  [&](obs_sceneitem_t *item, const char *name, obs_source_t *scene) {
				  // A mode on its way in or out is not read yet, and an item
				  // a mode not showing holds hidden is one a press would
				  // not show either.
				  if (mode_settling(scene, item))
					  settling = true;
				  if (mode_holds_hidden(scene, item))
					  return;
				  found++;
				  if (obs_sceneitem_visible(item) && !covered.count(name))
					  shown++;
			  });
		if (settling)
			look_again_ = true; // looked at again in a quarter second
		if (settling || !found)
			return; // in none of its scenes: nothing to go by
		const bool on = (shown == found) != on_hides;
		if (obs_source_enabled(source_) == on)
			return;
		// OBS tells the filter of this change as of any other; it is known
		// by the thread it comes from, so a press made at the same moment
		// from elsewhere still counts.
		correcting_ = std::this_thread::get_id();
		obs_source_set_enabled(source_, on);
		correcting_ = std::thread::id();
		obs_log(LOG_INFO, "Show in Scenes '%s': now reads %s, as %d of %d source(s) are shown",
			obs_source_get_name(source_), on ? "on" : "off", shown, found);
	}

	// Shows or hides every item it is set to, and says how many. `as`: how
	// a hide is done for this press (a key's choice), or as the filter says.
	int apply(bool enabled, HideAs as = HideAs::Filter)
	{
		std::vector<std::string> sources, scenes;
		Where where;
		bool show, with_linked, hide_standby, restore = false;
		ItemChange change;
		std::map<std::string, ItemChange> own_change;
		{
			std::lock_guard lock(mutex_);
			change = change_;
			own_change = own_change_;
			sources = sources_;
			scenes = scenes_;
			where = where_;
			show = enabled != on_hides_;
			with_linked = with_linked_;
			hide_standby = as == HideAs::Filter ? hide_standby_ : as == HideAs::Cover;
			restore = restore_;
		}
		// "Show only what it hid": a show brings back what the last hides
		// noted, nothing else.
		if (restore && show)
			return restore_record(change, own_change);
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (sources.empty() && parent && obs_source_get_name(parent))
			sources.push_back(obs_source_get_name(parent));
		// "Bring-along items too": whatever the sources' Send To keys, and
		// Place Item filters that move them, are set to bring along.
		if (with_linked)
			add_linked(sources);
		// "Hide shows Standby Image": a source with an enabled Standby Image
		// is covered with it instead of hidden, and uncovered by a show.
		// (A show also takes away what an earlier hide covered, whichever
		// way that press was set.)
		std::set<std::string> standby = hide_standby ? with_standby(sources, false) : std::set<std::string>();
		if (show) {
			std::lock_guard lock(mutex_);
			standby.insert(covered_.begin(), covered_.end());
			covered_.clear();
		}
		int covered = 0;
		Record noted; // what this hide did, for "Show only what it hid"
		for (const std::string &name : standby) {
			obs_source_t *found = obs_get_source_by_name(name.c_str());
			if (restore && !show && found && !standby_covered(found) && standby_present(found))
				noted.covers.insert(name);
			if (found && standby_cover_set(found, !show, -1, 0, 0, 0)) {
				covered++;
				if (!show) {
					std::lock_guard lock(mutex_);
					covered_.insert(name);
				}
			}
			obs_source_release(found);
		}
		int changed = 0;
		each_item(sources, scenes, where == Where::Every,
			  [&](obs_sceneitem_t *item, const char *name, obs_source_t *scene) {
				  if (obs_sceneitem_visible(item) == show)
					  return;
				  // Covered with its Standby Image instead (above).
				  if (!show && standby.count(name))
					  return;
				  // Not shown in a mode of its scene that is not showing
				  // (Link Visibility's "Follow the first"); hiding is never
				  // passed over.
				  if (show && mode_holds_hidden(scene, item))
					  return;
				  // A listed source's own change, or the usual one.
				  const auto own = own_change.find(name);
				  if (!set_item_visible(item, show, own == own_change.end() ? change : own->second,
							scene))
					  return;
				  changed++;
				  if (restore && !show) {
					  obs_source_t *owner = obs_scene_get_source(obs_sceneitem_get_scene(item));
					  noted.items.push_back(
						  {obs_source_get_name(scene) ? obs_source_get_name(scene) : "",
						   owner && obs_source_get_name(owner) ? obs_source_get_name(owner)
										       : "",
						   obs_sceneitem_get_id(item)});
				  }
			  });
		if (restore && !show) {
			// Added to what earlier hides noted and is still hidden.
			prune_record();
			std::lock_guard lock(mutex_);
			for (const Noted &n : noted.items)
				record_.items.push_back(n);
			record_.covers.insert(noted.covers.begin(), noted.covers.end());
			save_record();
		}
		obs_log(LOG_INFO, "Show in Scenes '%s': %s %d item(s)", obs_source_get_name(source_),
			show ? "showed" : "hid", changed);
		if (covered)
			obs_log(LOG_INFO, "Show in Scenes '%s': %s the Standby Image of %d source(s)",
				obs_source_get_name(source_), show ? "took away" : "showed", covered);
		return changed;
	}

	// "Switch to scene", after a press that showed the sources (turned the
	// switch on, or off when "On means" is hidden).
	void switch_scene(bool enabled)
	{
		GoTo go;
		{
			std::lock_guard lock(mutex_);
			if (enabled == on_hides_)
				return; // that press hid them
			go.target = go_to_;
			go.sources = sources_;
		}
		if (go.target.empty())
			return;
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (go.sources.empty() && parent && obs_source_get_name(parent))
			go.sources.push_back(obs_source_get_name(parent));
		go.filter = obs_source_get_name(source_);
		go_to_scene(std::move(go));
	}

	// "Show only what it hid": an item by its scene's name, the name of the
	// scene or group that holds it and its id; and the sources covered.
	struct Noted {
		std::string scene, owner;
		int64_t id = 0;
	};
	struct Record {
		std::vector<Noted> items;
		std::set<std::string> covers;
		bool empty() const { return items.empty() && covers.empty(); }
	};

	// One line each: "i<TAB>scene<TAB>owner<TAB>id" or "c<TAB>source".
	static Record read_record(const char *text)
	{
		Record out;
		std::string all = text ? text : "";
		size_t at = 0;
		while (at < all.size()) {
			size_t end = all.find('\n', at);
			if (end == std::string::npos)
				end = all.size();
			std::vector<std::string> parts;
			size_t from = at;
			for (size_t tab; (tab = all.find('\t', from)) != std::string::npos && tab < end; from = tab + 1)
				parts.push_back(all.substr(from, tab - from));
			parts.push_back(all.substr(from, end - from));
			if (parts.size() == 4 && parts[0] == "i")
				out.items.push_back({parts[1], parts[2], std::strtoll(parts[3].c_str(), nullptr, 10)});
			else if (parts.size() == 2 && parts[0] == "c")
				out.covers.insert(parts[1]);
			at = end + 1;
		}
		return out;
	}

	// Writes the record into the filter's own settings (with mutex_ held).
	static std::string record_text(const Record &record)
	{
		std::string text;
		for (const Noted &n : record.items)
			text += "i\t" + n.scene + "\t" + n.owner + "\t" + std::to_string(n.id) + "\n";
		for (const std::string &c : record.covers)
			text += "c\t" + c + "\n";
		return text;
	}
	void save_record()
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		obs_data_set_string(settings, "hidden_record", record_text(record_).c_str());
		obs_data_release(settings);
	}

	// The item noted, with a reference the caller lets go of, or nullptr.
	static obs_sceneitem_t *find_noted(const Noted &n)
	{
		obs_source_t *owner = obs_get_source_by_name(n.owner.c_str());
		obs_scene_t *scene = owner ? obs_group_or_scene_from_source(owner) : nullptr;
		obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, n.id) : nullptr;
		if (item)
			obs_sceneitem_addref(item);
		obs_source_release(owner);
		return item;
	}

	// Forgets what is no longer hidden (shown by something else, or gone)
	// or no longer covered; says how much is still held.
	size_t prune_record()
	{
		Record record;
		{
			std::lock_guard lock(mutex_);
			record = record_;
		}
		Record kept;
		for (const Noted &n : record.items) {
			obs_sceneitem_t *item = find_noted(n);
			if (item && !obs_sceneitem_visible(item))
				kept.items.push_back(n);
			obs_sceneitem_release(item);
		}
		for (const std::string &name : record.covers) {
			obs_source_t *found = obs_get_source_by_name(name.c_str());
			if (found && standby_covered(found))
				kept.covers.insert(name);
			obs_source_release(found);
		}
		std::lock_guard lock(mutex_);
		const bool changed = kept.items.size() != record_.items.size() || kept.covers != record_.covers;
		if (changed) {
			record_ = kept;
			save_record();
		}
		return record_.items.size() + record_.covers.size();
	}

	// A show press with "Show only what it hid": brings back what was noted
	// and is still hidden, then forgets it all.
	int restore_record(const ItemChange &change, const std::map<std::string, ItemChange> &own_change)
	{
		Record record;
		{
			std::lock_guard lock(mutex_);
			record = record_;
			record_ = Record();
			save_record();
		}
		if (record.empty()) {
			obs_log(LOG_INFO, "Show in Scenes '%s': nothing it hid to show again, so nothing is shown",
				obs_source_get_name(source_));
			return 0;
		}
		int shown = 0, uncovered = 0;
		for (const Noted &n : record.items) {
			obs_sceneitem_t *item = find_noted(n);
			obs_source_t *scene = obs_get_source_by_name(n.scene.c_str());
			if (item && scene && !obs_sceneitem_visible(item) && !mode_holds_hidden(scene, item)) {
				const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
				const auto own = own_change.find(name ? name : "");
				if (set_item_visible(item, true, own == own_change.end() ? change : own->second, scene))
					shown++;
			}
			obs_source_release(scene);
			obs_sceneitem_release(item);
		}
		for (const std::string &name : record.covers) {
			obs_source_t *found = obs_get_source_by_name(name.c_str());
			if (found && standby_covered(found) && standby_cover_set(found, false, -1, 0, 0, 0))
				uncovered++;
			obs_source_release(found);
			std::lock_guard lock(mutex_);
			covered_.erase(name);
		}
		obs_log(LOG_INFO,
			"Show in Scenes '%s': showed again %d item(s) and took away %d Standby Image(s) it had hidden them with",
			obs_source_get_name(source_), shown, uncovered);
		return shown;
	}

	// Of `sources`, those with an enabled Standby Image filter (with
	// `covered_only`, only those it is covering now).
	static std::set<std::string> with_standby(const std::vector<std::string> &sources, bool covered_only)
	{
		std::set<std::string> out;
		for (const std::string &name : sources) {
			obs_source_t *found = obs_get_source_by_name(name.c_str());
			if (found && (covered_only ? standby_covered(found) : standby_present(found)))
				out.insert(name);
			obs_source_release(found);
		}
		return out;
	}

	// Loading a saved filter with an empty settings object uses the historical default.
	static bool rename_record(obs_data_t *settings, const std::string &was, const std::string &now)
	{
		auto record = read_record(obs_data_get_string(settings, "hidden_record"));
		bool changed = false;
		for (auto &item : record.items) {
			for (auto *name : {&item.scene, &item.owner}) {
				if (*name == was) {
					*name = now;
					changed = true;
				}
			}
		}
		if (record.covers.erase(was)) {
			record.covers.insert(now);
			changed = true;
		}
		if (changed)
			obs_data_set_string(settings, "hidden_record", record_text(record).c_str());
		return changed;
	}

	void loaded(obs_data_t *settings)
	{
		if (assumed_new_) {
			obs_data_unset_user_value(settings, "restore_hidden");
			assumed_new_ = false;
			update(settings);
		}
	}

	obs_source_t *source() const { return source_; }
	bool hides_when_on()
	{
		std::lock_guard lock(mutex_);
		return on_hides_;
	}

private:
	static void enabled_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<ShowInScenes *>(data);
		// The switch's state has changed: whatever goes by the scenes looks again.
		scene_revision_bump();
		// A key's own way of hiding for this press ("ShowInScenesPress"),
		// set on this thread just before it switched the filter.
		const int as = self->pressing_thread_.load() == std::this_thread::get_id() ? self->pressing_as_.load()
											   : 0;
		// (Its own correction of the switch is not a press.)
		if (self->ready_ && self->correcting_.load() != std::this_thread::get_id())
			self->press_ = (calldata_bool(cd, "enabled") ? kPressOn : 0) | as;
	}

public:
	// Acted on at the next tick; of several presses in one frame, the last.
	// "Do it now" in the filter's window (`by_key` false) never changes scene.
	// kPressHide and kPressCover: a key's own way of hiding for that press.
	static constexpr int kPressOn = 1, kPressStay = 2, kPressHide = 4, kPressCover = 8;
	void press(bool enabled, bool by_key = true) { press_ = (enabled ? kPressOn : 0) | (by_key ? 0 : kPressStay); }

	// A key's press with its own way of hiding: the filter is switched as
	// any key switches it, and that press hides as `as` says.
	void press_as(bool enabled, HideAs as)
	{
		pressing_as_ = as == HideAs::Hide ? kPressHide : as == HideAs::Cover ? kPressCover : 0;
		pressing_thread_ = std::this_thread::get_id();
		obs_source_set_enabled(source_, enabled);
		pressing_thread_ = std::thread::id();
	}

private:
	obs_source_t *source_;
	std::mutex mutex_;
	std::vector<std::string> sources_, scenes_;
	Where where_ = Where::Every;
	bool on_hides_ = false;
	bool with_linked_ = false;
	ItemChange change_;                            // how items change, usually
	std::map<std::string, ItemChange> own_change_; // and for each listed source
	std::atomic<bool> ready_{false};
	bool follow_ = false;       // the switch goes by what is shown
	bool hide_standby_ = false; // a hide covers a source with its Standby Image instead
	bool assumed_new_ = false;
	bool restore_ = false;      // a show brings back only what the hides noted
	Record record_;             // what they noted (also in the settings, `hidden_record`)
	std::string go_to_;         // the scene a press turning it on goes to
	double follow_timer_ = 0.0; // until the next look, in seconds
	bool follow_staggered_ = false;
	std::atomic<bool> look_again_{true}; // the next look is not skipped
	uint64_t follow_rev_ = 0;            // scene_revision() at the last look
	bool follow_loaded_ = false;
	double follow_safety_ = 0.0;                     // until a look comes whatever changed
	std::atomic<std::thread::id> correcting_{};      // the thread putting the switch right, if any
	std::atomic<int> press_{-1};                     // a press (kPressOn, kPressStay), or -1 for none
	std::set<std::string> covered_;                  // the sources this filter covered with their Standby Image
	std::atomic<std::thread::id> pressing_thread_{}; // a key's press with its own way of hiding, on this thread
	std::atomic<int> pressing_as_{0};                // and that way (kPressHide, kPressCover)
};

bool where_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *scenes = obs_properties_get(props, "scenes|alist");
	if (scenes)
		obs_property_set_visible(scenes, obs_data_get_int(settings, "where") == (int)Where::Listed);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<ShowInScenes *>(data);
	obs_source_t *parent = self ? obs_filter_get_parent(self->source()) : nullptr;
	obs_properties_t *props = obs_properties_create();

	const bool on = self && obs_source_enabled(self->source());
	const bool shown = self ? on != self->hides_when_on() : true;
	obs_properties_add_text(props, "status",
				obs_module_text(shown ? "ShowIn.Status.Shown" : "ShowIn.Status.Hidden"), OBS_TEXT_INFO);

	// Two groups: what the switch acts on, then what a press does. Then the
	// transition, then the button. The groups hold no setting.
	obs_properties_t *which = obs_properties_create();
	PickList sources;
	sources.setting = "sources";
	sources.title = "ShowIn.Sources";
	sources.add = "ShowIn.Sources.Add";
	sources.tip = "ShowIn.Sources.Tip";
	sources.choices = [parent](std::vector<std::string> &names) {
		struct Fill {
			std::vector<std::string> *names;
		} fill{&names};
		const auto add = [](void *param, obs_source_t *source) {
			auto *f = static_cast<Fill *>(param);
			const char *name = obs_source_get_name(source);
			if (name && *name && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) &&
			    std::find(f->names->begin(), f->names->end(), name) == f->names->end())
				f->names->push_back(name);
			return true;
		};
		// This source first, then the rest, then scenes and groups (which
		// can be items in other scenes).
		if (parent && obs_source_get_name(parent))
			names.push_back(obs_source_get_name(parent));
		obs_enum_sources(add, &fill);
		obs_enum_scenes(add, &fill);
	};
	pick_list_add(which, self ? self->source() : nullptr, sources);

	obs_property_t *where = obs_properties_add_list(which, "where", obs_module_text("ShowIn.Where"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(where, obs_module_text("ShowIn.Where.Every"), (int)Where::Every);
	obs_property_list_add_int(where, obs_module_text("ShowIn.Where.Listed"), (int)Where::Listed);
	set_tip(where, obs_module_text("ShowIn.Where.Tip"));
	obs_property_set_modified_callback(where, where_modified);

	PickList scenes;
	scenes.setting = "scenes";
	scenes.title = "ShowIn.Scenes";
	scenes.add = "List.AddScene";
	scenes.tip = "ShowIn.Scenes.Tip";
	scenes.choices = [](std::vector<std::string> &names) {
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				const char *name = obs_source_get_name(scene);
				if (name && *name && !obs_source_is_group(scene))
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
	};
	pick_list_add(which, self ? self->source() : nullptr, scenes);

	obs_property_t *with_linked =
		obs_properties_add_bool(which, "with_linked", obs_module_text("ShowIn.WithLinked"));
	set_tip(with_linked, obs_module_text("ShowIn.WithLinked.Tip"));
	obs_properties_add_group(props, "sources_group", obs_module_text("ShowIn.SourcesGroup"), OBS_GROUP_NORMAL,
				 which);

	obs_properties_t *each_press = obs_properties_create();
	obs_property_t *means = obs_properties_add_list(each_press, "on_means", obs_module_text("ShowIn.OnMeans"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(means, obs_module_text("ShowIn.OnMeans.Shown"), 0);
	obs_property_list_add_int(means, obs_module_text("ShowIn.OnMeans.Hidden"), 1);
	set_tip(means, obs_module_text("ShowIn.OnMeans.Tip"));

	obs_property_t *hide_standby =
		obs_properties_add_bool(each_press, "hide_standby", obs_module_text("ShowIn.HideStandby"));
	set_tip(hide_standby, obs_module_text("ShowIn.HideStandby.Tip"));

	obs_property_t *restore =
		obs_properties_add_bool(each_press, "restore_hidden", obs_module_text("ShowIn.Restore"));
	set_tip(restore, obs_module_text("ShowIn.Restore.Tip"));

	obs_property_t *follow = obs_properties_add_bool(each_press, "follow", obs_module_text("ShowIn.Follow"));
	set_tip(follow, obs_module_text("ShowIn.Follow.Tip"));

	obs_property_t *go_to = obs_properties_add_list(each_press, "go_to", obs_module_text("ShowIn.GoTo"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(go_to, obs_module_text("ShowIn.GoTo.None"), "");
	{
		std::vector<std::string> names;
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				const char *name = obs_source_get_name(scene);
				if (name && *name && !obs_source_is_group(scene))
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
		// A scene chosen before and gone since stays chosen, by name.
		if (self) {
			obs_data_t *settings = obs_source_get_settings(self->source());
			const std::string chosen = obs_data_get_string(settings, "go_to");
			obs_data_release(settings);
			if (!chosen.empty() && std::find(names.begin(), names.end(), chosen) == names.end())
				names.push_back(chosen);
		}
		for (const std::string &name : names)
			obs_property_list_add_string(go_to, name.c_str(), name.c_str());
	}
	set_tip(go_to, obs_module_text("ShowIn.GoTo.Tip"));
	obs_properties_add_group(props, "press_group", obs_module_text("ShowIn.PressGroup"), OBS_GROUP_NORMAL,
				 each_press);

	add_item_change_properties(props, self ? self->source() : nullptr, "sources");

	obs_property_t *apply_now = obs_properties_add_button2(
		props, "apply_now", obs_module_text("ShowIn.ApplyNow"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			auto *filter = static_cast<ShowInScenes *>(d);
			if (filter)
				filter->press(obs_source_enabled(filter->source()), false);
			return false;
		},
		self);
	set_tip(apply_now, obs_module_text("ShowIn.ApplyNow.Tip"));

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("ShowIn.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool show_in_scenes_press(obs_source_t *source, const std::string &filter_name, const std::string &how,
			  const std::string &hide_as, bool &enabled, std::string &why)
{
	obs_source_t *filter = source ? obs_source_get_filter_by_name(source, filter_name.c_str()) : nullptr;
	const char *id = filter ? obs_source_get_unversioned_id(filter) : nullptr;
	if (!filter || !id || strcmp(id, kShowInScenesId) != 0) {
		why = "No Show in Scenes filter of that name on the source";
		obs_source_release(filter);
		return false;
	}
	const HideAs as = hide_as == "hide" ? HideAs::Hide : hide_as == "cover" ? HideAs::Cover : HideAs::Filter;
	if (how != "toggle" && how != "on" && how != "off") {
		why = "'how' is on, off or toggle";
		obs_source_release(filter);
		return false;
	}
	const bool now = obs_source_enabled(filter);
	enabled = how == "toggle" ? !now : how == "on";
	auto *self = static_cast<ShowInScenes *>(obs_obj_get_data(filter));
	if (self)
		self->press_as(enabled, as);
	else
		obs_source_set_enabled(filter, enabled);
	obs_source_release(filter);
	return true;
}

bool show_in_rename_record(obs_data_t *settings, const char *was, const char *now)
{
	return ShowInScenes::rename_record(settings, was, now);
}

void register_show_in_scenes()
{
	obs_source_info info = {};
	info.id = kShowInScenesId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("ShowIn.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ShowInScenes(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ShowInScenes *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ShowInScenes *>(data)->update(settings);
	};
	info.load = [](void *data, obs_data_t *settings) {
		static_cast<ShowInScenes *>(data)->loaded(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "where", (int)Where::Every);
		obs_data_set_default_int(settings, "on_means", 0);
		obs_data_set_default_bool(settings, "follow", false);
		obs_data_set_default_bool(settings, "hide_standby", false);
		obs_data_set_default_bool(settings, "restore_hidden", false);
		obs_data_set_default_string(settings, "go_to", "");
		item_change_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<ShowInScenes *>(data)->tick(seconds);
	};
	// A switch: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		skip_filter_input(static_cast<ShowInScenes *>(data)->source());
	};
	register_timed(&info);

	go_to_scene_add_test_calls();
}

} // namespace stagekit
