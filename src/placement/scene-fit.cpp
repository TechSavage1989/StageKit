/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Fit: the sources of a scene fitted together. Each entry is a Fit to
// Space (the same engine, the same settings, kept under "fit|<source>|"),
// but they are worked out in one tick, in the order of the list, and again
// (up to four times) while one entry's new place moves another's: so after
// a change every entry sets off once, all at the same moment, instead of one
// filter answering another a tick later.

#include "scene-fit.hpp"
#include "fit-engine.hpp"
#include "fit-item.hpp"

#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/scene-util.hpp"
#include "../common/timing.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

using namespace fit;

namespace {

constexpr int kPasses = 4;          // the most times the entries are worked out in one tick
constexpr uint32_t kIdleEvery = 15; // off screen, a full tick four times a second

std::string prefix_of(const std::string &entry)
{
	return "fit|" + entry + "|";
}

class SceneFit;
std::mutex g_fits_mutex;
std::vector<SceneFit *> g_fits;

class SceneFit {
public:
	SceneFit(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		idle_turn_ = (uint32_t)((reinterpret_cast<uintptr_t>(this) >> 4) % kIdleEvery);
		update(settings);
		std::lock_guard lock(g_fits_mutex);
		g_fits.push_back(this);
	}
	~SceneFit()
	{
		std::lock_guard lock(g_fits_mutex);
		g_fits.erase(std::remove(g_fits.begin(), g_fits.end(), this), g_fits.end());
	}

	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> names = pick_list_read(settings, "entries", false);
		for (const std::string &name : names)
			fit_entry_defaults(settings, prefix_of(name));
		std::vector<std::shared_ptr<Entry>> was;
		{
			std::lock_guard lock(mutex_);
			was = entries_;
		}
		std::vector<std::shared_ptr<Entry>> now;
		for (const std::string &name : names) {
			std::shared_ptr<Entry> entry;
			for (const auto &old : was)
				if (old->name == name)
					entry = old;
			if (entry) {
				entry->engine->update(settings);
			} else {
				entry = std::make_shared<Entry>();
				entry->name = name;
				entry->engine = std::make_unique<FitEngine>(settings, source_, prefix_of(name), name);
				if (parent_)
					entry->engine->attach(parent_, false);
			}
			now.push_back(entry);
		}
		std::lock_guard lock(mutex_);
		entries_ = now;
	}

	void attach(obs_source_t *parent)
	{
		parent_ = parent;
		glance_forget(parent);
		std::lock_guard lock(mutex_);
		for (const auto &entry : entries_)
			entry->engine->attach(parent, false);
		forget_ = true;
	}
	void detach()
	{
		glance_forget(parent_);
		std::lock_guard lock(mutex_);
		for (const auto &entry : entries_)
			entry->engine->detach(false);
		parent_ = nullptr;
	}

	void tick(float seconds);

private:
	struct Entry {
		std::string name;
		std::unique_ptr<FitEngine> engine;
	};
	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::vector<std::shared_ptr<Entry>> entries_; // under mutex_
	// One look at what is in the way for each way of measuring the
	// entries use (normally one): by visible pixels, without shadows.
	std::map<int, FitScan> scans_;
	std::atomic<bool> forget_{false};
	uint32_t idle_turn_ = 0;
	uint64_t unsettled_logged_ = 0;
};

void SceneFit::tick(float seconds)
{
	std::vector<std::shared_ptr<Entry>> entries;
	{
		std::lock_guard lock(mutex_);
		entries = entries_;
	}
	// Not on a group (a group is only as large as what is in it), not
	// while a collection loads, not while switched off.
	if (!parent_ || !obs_source_enabled(source_) || obs_source_is_group(parent_) || !scenes_loaded()) {
		for (const auto &entry : entries)
			claim_clear(entry->engine.get());
		return;
	}
	if (forget_.exchange(false))
		scans_.clear();
	// Off screen with nothing asked of any entry: four times a second only.
	if (!obs_source_showing(parent_)) {
		bool asked = false;
		for (const auto &entry : entries)
			asked = asked || entry->engine->pending();
		if (!asked && ++idle_turn_ % kIdleEvery != 0) {
			for (const auto &entry : entries)
				entry->engine->skip_time(seconds);
			return;
		}
	}
	const size_t n = entries.size();
	// Each entry's item, held for this tick only.
	std::vector<obs_sceneitem_t *> items(n, nullptr);
	struct Release {
		std::vector<obs_sceneitem_t *> *items;
		~Release()
		{
			for (obs_sceneitem_t *item : *items)
				if (item)
					obs_sceneitem_release(item);
		}
	} release{&items};
	for (size_t i = 0; i < n; i++) {
		items[i] = find_item(parent_, entries[i]->name);
		if (items[i])
			obs_sceneitem_addref(items[i]);
	}
	std::vector<int64_t> ids;
	for (obs_sceneitem_t *item : items)
		if (item)
			ids.push_back(obs_sceneitem_get_id(item));

	// A: everything about each item before its space is looked for. A
	// hidden one, one coming back and one with no picture say where their
	// room is, and the others go by that in this same tick.
	std::vector<Prep> preps(n);
	enum class Way { Skip, Place, Hold, Solve };
	std::vector<Way> way(n, Way::Skip);
	for (size_t i = 0; i < n; i++)
		if (entries[i]->engine->prepare(items[i], seconds, preps[i]))
			way[i] = Way::Place;
	// B: whether each in the free space has a space to look for.
	bool any_solve = false;
	for (size_t i = 0; i < n; i++) {
		if (way[i] != Way::Place)
			continue;
		FitEngine &engine = *entries[i]->engine;
		if (!preps[i].free) {
			engine.not_free();
			continue;
		}
		const FitEngine::Motion m = engine.motion(preps[i], seconds);
		way[i] = m == FitEngine::Motion::Stop   ? Way::Skip
			 : m == FitEngine::Motion::Hold ? Way::Hold
							: Way::Solve;
		any_solve = any_solve || way[i] == Way::Solve;
	}
	// C: what is in the way, gathered once, and every entry worked out
	// against it and against the other entries' new places.
	std::vector<Box> spaces(n);
	if (any_solve) {
		auto scan_of = [this](const Measure &how) -> FitScan & {
			return scans_[(how.visible_pixels ? 1 : 0) | (how.leave_out_shadows ? 2 : 0)];
		};
		bool again = false;
		for (size_t i = 0; i < n; i++)
			again = entries[i]->engine->look_again() || again;
		std::map<int, bool> gathered;
		for (size_t i = 0; i < n; i++) {
			if (way[i] != Way::Solve)
				continue;
			const int key = (preps[i].how.visible_pixels ? 1 : 0) |
					(preps[i].how.leave_out_shadows ? 2 : 0);
			if (gathered[key])
				continue;
			gathered[key] = true;
			FitScan &scan = scan_of(preps[i].how);
			if (again)
				scan.forget();
			scan.gather(parent_, nullptr, this);
		}
		std::vector<bool> at_rest(n, false), moved_any(n, false), live(n, false);
		std::vector<Obstacle> in_way;
		bool moved = true;
		int pass = 0;
		for (; pass < kPasses && moved; pass++) {
			moved = false;
			for (size_t i = 0; i < n; i++) {
				if (way[i] != Way::Solve)
					continue;
				FitEngine &engine = *entries[i]->engine;
				Prep &p = preps[i];
				scan_of(p.how).obstacles(parent_, p.item, p.s.skip, p.s.skip_rule, &engine, p.how,
							 in_way, &ids);
				bool rest = false;
				engine.observe(in_way, engine.arriving(), pass == 0 ? seconds : 0.0f, rest);
				if (pass == 0)
					at_rest[i] = rest;
				bool changed = false;
				if (!engine.solve(p, pass == 0 ? seconds : 0.0f, pass == 0, changed)) {
					way[i] = Way::Skip; // no room at all
					continue;
				}
				live[i] = true;
				if (changed) {
					moved = true;
					moved_any[i] = true;
				}
				// Said at once: the entries after it in this pass go by it.
				engine.publish_claim(p);
			}
		}
		if (moved && pass == kPasses) {
			// Kept as the last pass left them; said in the log, now and then.
			const uint64_t now = os_gettime_ns();
			if (now - unsettled_logged_ > 60000000000ULL || !unsettled_logged_) {
				unsettled_logged_ = now;
				obs_log(LOG_INFO, "Scene Fit '%s': its sources did not settle in %d passes",
					obs_source_get_name(source_), kPasses);
			}
		}
		// At rest: one glance a tick for all of them.
		bool all_rest = true;
		for (size_t i = 0; i < n; i++)
			if (way[i] == Way::Solve && !at_rest[i])
				all_rest = false;
		if (all_rest) {
			for (size_t i = 0; i < n; i++) {
				if (way[i] != Way::Solve)
					continue;
				std::vector<obs_sceneitem_t *> mine;
				for (size_t j = 0; j < n; j++)
					if (way[j] == Way::Solve &&
					    preps[j].how.visible_pixels == preps[i].how.visible_pixels &&
					    preps[j].how.leave_out_shadows == preps[i].how.leave_out_shadows)
						mine.push_back(preps[j].item);
				scan_of(preps[i].how).at_rest(parent_, mine, this, preps[i].how);
				break; // one glance a tick
			}
		}
		// Every entry whose place changed sets off now, from where it is.
		for (size_t i = 0; i < n; i++) {
			if (way[i] != Way::Solve || !live[i])
				continue;
			if (moved_any[i])
				entries[i]->engine->count_glide();
			spaces[i] = entries[i]->engine->advance(preps[i], seconds);
		}
	}
	// D: each put in place.
	for (size_t i = 0; i < n; i++) {
		FitEngine &engine = *entries[i]->engine;
		switch (way[i]) {
		case Way::Skip:
			continue;
		case Way::Hold:
			spaces[i] = engine.current();
			engine.publish_claim(preps[i]);
			break;
		case Way::Place:
			engine.publish_claim(preps[i]);
			break;
		case Way::Solve:
			break; // its claim was said while it was worked out
		}
		engine.apply(preps[i], spaces[i]);
	}
}

// ---------------------------------------------------------------- the window

// The entries' groups follow the list: one is added for an entry just added,
// and the one of an entry taken off goes. Only the chosen entry's shows.
void show_chosen(obs_properties_t *top, obs_data_t *settings)
{
	const std::vector<std::string> names = pick_list_read(settings, "entries", false);
	std::string chosen = obs_data_get_string(settings, "fit_for");
	if (std::find(names.begin(), names.end(), chosen) == names.end())
		chosen = names.empty() ? std::string() : names.front();
	for (const std::string &name : names)
		if (obs_property_t *group = obs_properties_get(top, (prefix_of(name) + "g").c_str())) {
			obs_property_set_visible(group, name == chosen);
			fit_controls_shown(top, settings, prefix_of(name), fit_list_prefix(name), true);
		}
	if (obs_property_t *box = obs_properties_get(top, "fit_for"))
		obs_property_set_visible(box, !names.empty());
}

void add_entry_group(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings, const std::string &name)
{
	const std::string prefix = prefix_of(name);
	if (obs_properties_get(top, (prefix + "g").c_str()))
		return;
	fit_entry_defaults(settings, prefix);
	obs_source_t *parent = owner ? obs_filter_get_parent(owner) : nullptr;
	obs_properties_t *content = obs_properties_create();
	fit_add_controls(content, prefix, fit_list_prefix(name), owner, parent, true);
	obs_properties_add_group(top, (prefix + "g").c_str(), name.c_str(), OBS_GROUP_NORMAL, content);
}

void sync_groups(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings)
{
	const std::vector<std::string> names = pick_list_read(settings, "entries", false);
	// Groups of entries no longer listed.
	std::vector<std::string> gone;
	for (obs_property_t *p = obs_properties_first(top); p; obs_property_next(&p)) {
		const std::string name = obs_property_name(p);
		if (name.rfind("fit|", 0) != 0 || name.size() < 6 || name.compare(name.size() - 2, 2, "|g") != 0)
			continue;
		const std::string entry = name.substr(4, name.size() - 6);
		if (std::find(names.begin(), names.end(), entry) == names.end())
			gone.push_back(name);
	}
	for (const std::string &name : gone)
		obs_properties_remove_by_name(top, name.c_str());
	for (const std::string &name : names)
		add_entry_group(top, owner, settings, name);
	if (obs_property_t *box = obs_properties_get(top, "fit_for")) {
		const std::string was = obs_data_get_string(settings, "fit_for");
		obs_property_list_clear(box);
		for (const std::string &name : names)
			obs_property_list_add_string(box, name.c_str(), name.c_str());
		// An entry just added is the one shown.
		if (!names.empty() && std::find(names.begin(), names.end(), was) == names.end())
			obs_data_set_string(settings, "fit_for", names.back().c_str());
	}
	show_chosen(top, settings);
}

// The names of the fit keys a Fit to Space keeps, with what kind each is.
enum class Kind { Int, Double, Bool, String };
struct FitKey {
	const char *name;
	Kind kind;
};
const FitKey kFitKeys[] = {
	{"box", Kind::Int},
	{"box_x", Kind::Double},
	{"box_y", Kind::Double},
	{"box_width", Kind::Double},
	{"box_height", Kind::Double},
	{"margin", Kind::Double},
	{"tolerance", Kind::Double},
	{"resize", Kind::Bool},
	{"toward", Kind::Int},
	{"visible_pixels", Kind::Bool},
	{"no_shadows", Kind::Bool},
	{"glide_ms", Kind::Int},
	{"ease_curve", Kind::Int},
	{"ease_mode", Kind::Int},
	{"skip_use_pct", Kind::Bool},
	{"skip_fill", Kind::Double},
	{"skip_use_px", Kind::Bool},
	{"skip_px_top", Kind::Double},
	{"skip_px_bottom", Kind::Double},
	{"skip_px_left", Kind::Double},
	{"skip_px_right", Kind::Double},
	{"have_anchor", Kind::Bool},
	{"anchor_x", Kind::Double},
	{"anchor_y", Kind::Double},
	{"orig_item", Kind::String},
	{"orig_k", Kind::Double},
	{"keep_item", Kind::String},
	{"keep_k", Kind::Double},
};

// "Take over Fit to Space": each enabled Fit to Space of the scene whose
// source is not listed yet becomes an entry, with all its settings and what
// it has written down (the place it is kept round, the size it keeps), and is
// switched off (not deleted: switched on again, it is as it was).
bool take_over(obs_properties_t *top, obs_property_t *, void *data)
{
	auto *self = static_cast<SceneFit *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *scene = self ? self->parent() : nullptr;
	if (!owner || !scene)
		return false;
	struct Found {
		std::vector<obs_source_t *> fits;
	} found;
	obs_source_enum_filters(
		scene,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (id && std::strcmp(id, kFitItemId) == 0 && obs_source_enabled(filter))
				static_cast<Found *>(param)->fits.push_back(obs_source_get_ref(filter));
		},
		&found);
	obs_data_t *settings = obs_source_get_settings(owner);
	std::vector<std::string> names = pick_list_read(settings, "entries", false);
	const bool had_none = names.empty();
	bool first = true;
	for (obs_source_t *filter : found.fits) {
		obs_data_t *theirs = obs_source_get_settings(filter);
		const std::string item = obs_data_get_string(theirs, "item");
		if (!item.empty() && std::find(names.begin(), names.end(), item) == names.end()) {
			names.push_back(item);
			const std::string prefix = prefix_of(item);
			for (const FitKey &key : kFitKeys) {
				const std::string to = prefix + key.name;
				switch (key.kind) {
				case Kind::Int:
					obs_data_set_int(settings, to.c_str(), obs_data_get_int(theirs, key.name));
					break;
				case Kind::Double:
					obs_data_set_double(settings, to.c_str(),
							    obs_data_get_double(theirs, key.name));
					break;
				case Kind::Bool:
					obs_data_set_bool(settings, to.c_str(), obs_data_get_bool(theirs, key.name));
					break;
				case Kind::String:
					obs_data_set_string(settings, to.c_str(),
							    obs_data_get_string(theirs, key.name));
					break;
				}
			}
			// (Read by the filter's own rule: unticked, an old Fit to
			// Space measured by boxes.)
			if (obs_data_has_user_value(theirs, "as_arranged") &&
			    !obs_data_get_bool(theirs, "as_arranged") &&
			    !obs_data_has_user_value(theirs, "visible_pixels"))
				obs_data_set_bool(settings, (prefix + "visible_pixels").c_str(), false);
			const std::string lists = fit_list_prefix(item);
			pick_list_write(settings, (lists + "skip").c_str(), false,
					pick_list_read(theirs, "skip", false));
			pick_list_write(settings, (lists + "share_scenes").c_str(), false,
					pick_list_read(theirs, "share_scenes", false));
			// The overlap switch is the Scene Fit's own, for all: it is
			// taken from the first one taken over into an empty Scene Fit.
			if (first && had_none)
				obs_data_set_bool(settings, "skip_fill_on", obs_data_get_bool(theirs, "skip_fill_on"));
			first = false;
			obs_source_set_enabled(filter, false);
			obs_log(LOG_INFO, "Scene Fit '%s': took over Fit to Space '%s' (%s)",
				obs_source_get_name(owner), obs_source_get_name(filter), item.c_str());
		}
		obs_data_release(theirs);
		obs_source_release(filter);
	}
	pick_list_write(settings, "entries", false, names);
	obs_source_update(owner, settings);
	if (top) {
		pick_list_refresh(top, owner, "entries", false);
		sync_groups(top, owner, settings);
	}
	obs_data_release(settings);
	return true;
}

bool chosen_modified(obs_properties_t *top, obs_property_t *, obs_data_t *settings)
{
	show_chosen(top, settings);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneFit *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *parent = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();
	if (parent && obs_source_is_group(parent)) {
		obs_property_t *wrong =
			obs_properties_add_text(props, "on_group", obs_module_text("SceneFit.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(wrong, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(wrong, true);
	}
	PickList entries;
	entries.setting = "entries";
	entries.title = "SceneFit.Entries";
	entries.add = "SceneFit.AddSource";
	entries.tip = "SceneFit.Entries.Tip";
	entries.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, owner, entries);
	obs_property_t *take =
		obs_properties_add_button2(props, "take_over", obs_module_text("SceneFit.TakeOver"), take_over, self);
	obs_property_set_long_description(take, obs_module_text("SceneFit.TakeOver.Tip"));
	obs_property_t *rule = obs_properties_add_bool(props, "skip_fill_on", obs_module_text("Fit.SkipFillOn"));
	obs_property_set_long_description(rule, obs_module_text("SceneFit.SkipFillOn.Tip"));
	obs_property_set_modified_callback(rule, chosen_modified);
	obs_property_t *chosen = obs_properties_add_list(props, "fit_for", obs_module_text("SceneFit.For"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_long_description(chosen, obs_module_text("SceneFit.For.Tip"));
	obs_data_t *settings = owner ? obs_source_get_settings(owner) : nullptr;
	if (settings) {
		sync_groups(props, owner, settings);
		obs_data_release(settings);
	}
	obs_property_set_modified_callback(chosen, chosen_modified);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "skip_fill_on", false);
}

// A source is renamed: its entry, its settings and the lists naming it follow.
void on_source_rename(void *, calldata_t *cd)
{
	const char *was_c = calldata_string(cd, "prev_name");
	const char *now_c = calldata_string(cd, "new_name");
	if (!was_c || !now_c || !*was_c || !*now_c)
		return;
	const std::string was = was_c, now = now_c;
	std::vector<obs_source_t *> owners;
	{
		std::lock_guard lock(g_fits_mutex);
		for (SceneFit *each : g_fits)
			if (obs_source_t *held = obs_source_get_ref(each->source()))
				owners.push_back(held);
	}
	auto move = [](obs_data_t *settings, const std::string &from, const std::string &to) {
		std::vector<std::string> keys;
		for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
			const char *name = obs_data_item_get_name(item);
			if (name && std::strncmp(name, from.c_str(), from.size()) == 0 &&
			    obs_data_item_has_user_value(item))
				keys.push_back(name);
		}
		for (const std::string &key : keys) {
			const std::string moved = to + key.substr(from.size());
			obs_data_item_t *item = obs_data_item_byname(settings, key.c_str());
			switch (obs_data_item_gettype(item)) {
			case OBS_DATA_STRING:
				obs_data_set_string(settings, moved.c_str(), obs_data_item_get_string(item));
				break;
			case OBS_DATA_NUMBER:
				if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
					obs_data_set_int(settings, moved.c_str(), obs_data_item_get_int(item));
				else
					obs_data_set_double(settings, moved.c_str(), obs_data_item_get_double(item));
				break;
			case OBS_DATA_BOOLEAN:
				obs_data_set_bool(settings, moved.c_str(), obs_data_item_get_bool(item));
				break;
			case OBS_DATA_OBJECT: {
				obs_data_t *object = obs_data_item_get_obj(item);
				obs_data_set_obj(settings, moved.c_str(), object);
				obs_data_release(object);
				break;
			}
			case OBS_DATA_ARRAY: {
				obs_data_array_t *array = obs_data_item_get_array(item);
				obs_data_set_array(settings, moved.c_str(), array);
				obs_data_array_release(array);
				break;
			}
			default:
				break;
			}
			obs_data_item_release(&item);
			obs_data_erase(settings, key.c_str());
		}
	};
	for (obs_source_t *owner : owners) {
		obs_data_t *settings = obs_source_get_settings(owner);
		std::vector<std::string> names = pick_list_read(settings, "entries", false);
		bool changed = false;
		for (std::string &name : names) {
			if (name == was) {
				name = now;
				changed = true;
			}
		}
		if (changed) {
			move(settings, prefix_of(was), prefix_of(now));
			move(settings, fit_list_prefix(was), fit_list_prefix(now));
			for (const char *key : {"keep_item", "orig_item"}) {
				const std::string full = prefix_of(now) + key;
				if (obs_data_get_string(settings, full.c_str()) == was)
					obs_data_set_string(settings, full.c_str(), now.c_str());
			}
			pick_list_write(settings, "entries", false, names);
			if (obs_data_get_string(settings, "fit_for") == was)
				obs_data_set_string(settings, "fit_for", now.c_str());
		}
		// Every entry's list of ignored sources.
		for (const std::string &name : names) {
			const std::string setting = fit_list_prefix(name) + "skip";
			std::vector<std::string> skip = pick_list_read(settings, setting.c_str(), false);
			bool named = false;
			for (std::string &one : skip)
				if (one == was) {
					one = now;
					named = true;
				}
			if (named) {
				pick_list_write(settings, setting.c_str(), false, skip);
				changed = true;
			}
		}
		if (changed)
			obs_source_update(owner, settings);
		obs_data_release(settings);
		obs_source_release(owner);
	}
}

} // namespace

void scene_fit_stop()
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
}

void register_scene_fit()
{
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
	pick_list_on_change_for(kSceneFitId, sync_groups);
	obs_source_info info = {};
	info.id = kSceneFitId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneFit.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneFit(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<SceneFit *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneFit *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneFit *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<SceneFit *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<SceneFit *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneFit *>(data)->detach();
	};
	register_timed(&info);
}

} // namespace stagekit
