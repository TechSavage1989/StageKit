/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Active App: a scene has a capture for each of several apps, and one of
// them is shown at a time: the one whose app is in use.
//
// Which app a capture belongs to is read from the capture itself (the
// program its "window" setting names), so there is nothing to type.
//
//  - An app that opens is shown, and so is a capture aimed at another
//    window by hand.
//  - With "Follow the app in use", clicking into one of the apps shows it.
//    Clicking into anything else changes nothing: the last one stays.
//  - When the app that is showing closes, the one used before it comes back.
//    When none of them is open, the first in the list is shown.
//
// It acts only when one of those things happens, so a capture shown by hand
// stays until the next one.
//
// The change itself is OBS's own show and hide transition on each item (the
// ones in an item's right-click menu), which this filter sets to the kind
// and length chosen here. Captures of the other open apps can be kept
// running while the scene is showing, so the one switched to has a picture
// at once.
//
// Finding windows uses the Windows API (shared with Auto Capture). On other
// systems the filter loads and does nothing.

#include "active-app.hpp"

#include "auto-capture.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// "transition" in the settings: one of these, or the id of an OBS transition.
const char *const kCut = "none";  // items change at once; their transitions are taken off
const char *const kKeep = "keep"; // whatever is set on each item is left alone

std::string lowered(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

// The program a capture is aimed at, in lower case: the last part of its
// "window" setting, which OBS writes as title:class:executable with ':' as
// "#3A" and '#' as "#22". Empty for a source that is not aimed at a window.
std::string program_of(obs_sceneitem_t *item)
{
	obs_source_t *source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!source)
		return {};
	obs_data_t *settings = obs_source_get_settings(source);
	const std::string window = obs_data_get_string(settings, "window");
	obs_data_release(settings);
	const size_t colon = window.rfind(':');
	if (colon == std::string::npos)
		return {};
	std::string program;
	for (size_t i = colon + 1; i < window.size(); i++) {
		if (window.compare(i, 3, "#3A") == 0) {
			program += ':';
			i += 2;
		} else if (window.compare(i, 3, "#22") == 0) {
			program += '#';
			i += 2;
		} else {
			program += window[i];
		}
	}
	return lowered(program);
}

// Gives an item the show and hide transition asked for, if it does not have
// them already.
void set_transitions(obs_sceneitem_t *item, const std::string &kind, uint32_t ms)
{
	if (kind == kKeep)
		return;
	for (bool show : {true, false}) {
		obs_source_t *now = obs_sceneitem_get_transition(item, show);
		if (kind == kCut) {
			if (now)
				obs_sceneitem_set_transition(item, show, nullptr);
			continue;
		}
		const char *now_id = now ? obs_source_get_unversioned_id(now) : nullptr;
		if (!now_id || kind != now_id) {
			const char *of = obs_source_get_name(obs_sceneitem_get_source(item));
			const std::string name =
				std::string(of ? of : "") + (show ? " Show Transition" : " Hide Transition");
			obs_source_t *made = obs_source_create_private(kind.c_str(), name.c_str(), nullptr);
			if (!made)
				continue; // that kind of transition is not installed
			obs_sceneitem_set_transition(item, show, made);
			obs_source_release(made);
		}
		if (obs_sceneitem_get_transition_duration(item, show) != ms)
			obs_sceneitem_set_transition_duration(item, show, ms);
	}
}

class ActiveApp {
public:
	ActiveApp(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		// For the tests: stand-in windows, one per line, "title|exe|front".
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_test_windows(in string windows)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<ActiveApp *>(data);
				const char *text = calldata_string(cd, "windows");
				std::vector<OpenWindow> windows;
				std::vector<std::string> fields(1);
				auto flush = [&] {
					fields.resize(3);
					if (!fields[1].empty())
						windows.push_back({fields[0], fields[1], fields[2] == "front"});
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
	}

	~ActiveApp() { keep_ready({}); }

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> items = pick_list_read(settings, "items", false);
		std::lock_guard lock(mutex_);
		items_ = items;
		follow_ = obs_data_get_bool(settings, "follow");
		transition_ = obs_data_get_string(settings, "transition");
		transition_ms_ = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "transition_ms"), 0, 10000);
		ready_wanted_ = obs_data_get_bool(settings, "ready");
		started_ = false; // settings changed: work out what to show afresh
		timer_ = 0.0;
	}

	void tick(float seconds)
	{
		if (!parent_ || !obs_source_enabled(source_)) {
			keep_ready({});
			return;
		}
		std::vector<std::string> names;
		std::vector<OpenWindow> windows;
		std::string transition;
		uint32_t transition_ms;
		bool follow, use_test, first, ready;
		{
			std::lock_guard lock(mutex_);
			timer_ -= seconds;
			if (timer_ > 0.0)
				return;
			timer_ = 0.2;
			names = items_;
			follow = follow_;
			transition = transition_;
			transition_ms = transition_ms_;
			ready = ready_wanted_;
			use_test = use_test_windows_;
			if (use_test)
				windows = test_windows_;
			first = !started_;
		}
#ifndef _WIN32
		// Nothing to go by on other systems.
		if (!use_test)
			return;
#endif
		if (names.empty())
			return;
		if (!use_test)
			windows = open_windows();

		struct Slot {
			obs_sceneitem_t *item = nullptr;
			std::string program;
			bool open = false, visible = false;
		};
		std::string front_program;
		for (const OpenWindow &w : windows)
			if (w.foreground)
				front_program = lowered(w.executable);
		std::vector<Slot> slots(names.size());
		int front = -1, visible_count = 0, first_visible = -1;
		for (size_t i = 0; i < names.size(); i++) {
			Slot &slot = slots[i];
			slot.item = find_item(parent_, names[i]);
			if (!slot.item)
				continue;
			slot.program = program_of(slot.item);
			slot.visible = obs_sceneitem_visible(slot.item);
			if (!slot.program.empty()) {
				for (const OpenWindow &w : windows)
					slot.open = slot.open || lowered(w.executable) == slot.program;
				if (front < 0 && slot.program == front_program)
					front = (int)i;
			}
			if (slot.visible) {
				visible_count++;
				if (first_visible < 0)
					first_visible = (int)i;
			}
			set_transitions(slot.item, transition, transition_ms);
		}
		// The other open apps' captures keep running while this scene is on
		// show, so whichever is switched to has a picture straight away.
		std::vector<obs_source_t *> running;
		if (ready && obs_source_showing(parent_)) {
			for (const Slot &slot : slots) {
				obs_source_t *capture = slot.item && slot.open ? obs_sceneitem_get_source(slot.item)
									       : nullptr;
				if (capture && std::find(running.begin(), running.end(), capture) == running.end())
					running.push_back(capture);
			}
		}
		keep_ready(running);
		auto index_of = [&](const std::string &name) {
			for (size_t i = 0; i < names.size(); i++)
				if (slots[i].item && names[i] == name)
					return (int)i;
			return -1;
		};
		auto used = [&](int index) {
			const std::string name = names[(size_t)index];
			recent_.erase(std::remove(recent_.begin(), recent_.end(), name), recent_.end());
			recent_.insert(recent_.begin(), name);
		};
		// What to go back to: the one used most recently whose app is open,
		// else the first whose app is open, else the first in the list.
		auto fallback = [&]() {
			for (const std::string &name : recent_) {
				const int index = index_of(name);
				if (index >= 0 && slots[(size_t)index].open)
					return index;
			}
			for (size_t i = 0; i < slots.size(); i++)
				if (slots[i].open)
					return (int)i;
			for (size_t i = 0; i < slots.size(); i++)
				if (slots[i].item)
					return (int)i;
			return -1;
		};

		// What is showing now. One shown by hand since the last look is taken as it is.
		int shown = index_of(shown_);
		if (shown < 0 || !slots[(size_t)shown].visible)
			shown = first_visible;
		if (shown >= 0 && names[(size_t)shown] != shown_)
			used(shown);

		int want = shown;
		const char *why = "";
		if (first) {
			if (follow && front >= 0) {
				want = front;
				why = "its app is in use";
			} else if (shown < 0 || (!slots[(size_t)shown].program.empty() && !slots[(size_t)shown].open)) {
				want = fallback();
				why = "nothing else to show";
			}
		} else {
			// An app that has just opened is shown. When following, one that
			// opens behind the listed app in use does not take over from it.
			int opened = -1;
			for (size_t i = 0; i < slots.size(); i++) {
				if (slots[i].open && !was_open_[names[i]] && (opened < 0 || (int)i == front))
					opened = (int)i;
			}
			if (follow && front >= 0 && opened != front)
				opened = -1;
			if (opened >= 0) {
				want = opened;
				why = "its app has opened";
			}
			// A capture aimed at another window by hand is shown.
			for (size_t i = 0; i < slots.size(); i++) {
				const auto before = was_program_.find(names[i]);
				if (slots[i].item && !slots[i].program.empty() && before != was_program_.end() &&
				    before->second != slots[i].program) {
					want = (int)i;
					why = "it was aimed at another window";
				}
			}
			// The app clicked into is shown.
			if (follow && front >= 0 && names[(size_t)front] != last_front_) {
				want = front;
				why = "its app is in use";
			}
			// The app that was showing has closed.
			if (want >= 0 && want == shown && was_open_[names[(size_t)want]] && !slots[(size_t)want].open) {
				want = fallback();
				why = "the app that was showing has closed";
			}
		}
		for (size_t i = 0; i < slots.size(); i++) {
			was_open_[names[i]] = slots[i].open;
			if (slots[i].item)
				was_program_[names[i]] = slots[i].program;
		}
		last_front_ = front >= 0 ? names[(size_t)front] : std::string();

		std::string status;
		const bool act = want >= 0 && (want != shown || (first && visible_count != 1));
		if (act) {
			for (size_t i = 0; i < slots.size(); i++) {
				const bool on = (int)i == want;
				if (slots[i].item && slots[i].visible != on)
					obs_sceneitem_set_visible(slots[i].item, on);
			}
			used(want);
			if (*why)
				obs_log(LOG_INFO, "Active App '%s': showing '%s' (%s)", obs_source_get_name(source_),
					names[(size_t)want].c_str(), why);
			shown = want;
		}
		if (shown >= 0)
			status = names[(size_t)shown];
		shown_ = status;
		std::lock_guard lock(mutex_);
		started_ = true;
		status_ = status;
	}

	// What is showing, for the top line of the controls. Empty when nothing is.
	std::string showing()
	{
		std::lock_guard lock(mutex_);
		return status_;
	}

	void attach(obs_source_t *parent) { parent_ = parent; }
	void detach()
	{
		parent_ = nullptr;
		keep_ready({});
	}
	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }

private:
	// Keeps exactly these sources running (counted as showing) and lets go of
	// any kept before that are not among them.
	void keep_ready(const std::vector<obs_source_t *> &wanted)
	{
		std::lock_guard lock(ready_mutex_);
		for (auto it = ready_.begin(); it != ready_.end();) {
			obs_source_t *source = obs_weak_source_get_source(*it);
			const bool keep = source && std::find(wanted.begin(), wanted.end(), source) != wanted.end();
			if (!keep) {
				if (source)
					obs_source_dec_showing(source);
				obs_weak_source_release(*it);
				it = ready_.erase(it);
			} else {
				++it;
			}
			obs_source_release(source);
		}
		for (obs_source_t *source : wanted) {
			bool have = false;
			for (obs_weak_source_t *weak : ready_)
				have = have || obs_weak_source_references_source(weak, source);
			if (!have) {
				obs_source_inc_showing(source);
				ready_.push_back(obs_source_get_weak_source(source));
			}
		}
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::vector<std::string> items_;
	bool follow_ = true;
	std::string transition_ = "fade_transition";
	uint32_t transition_ms_ = 250;
	bool ready_wanted_ = true;
	std::mutex ready_mutex_;
	std::vector<obs_weak_source_t *> ready_; // captures kept running
	double timer_ = 0.0;
	bool started_ = false;
	std::string status_;
	std::vector<OpenWindow> test_windows_;
	bool use_test_windows_ = false;
	// Only the tick uses these.
	std::string shown_, last_front_;
	std::map<std::string, bool> was_open_;
	std::map<std::string, std::string> was_program_;
	std::vector<std::string> recent_; // most recently shown first
};

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<ActiveApp *>(data);
	obs_source_t *parent = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *status = obs_properties_add_text(props, "status", "", OBS_TEXT_INFO);
	const std::string showing = self ? self->showing() : std::string();
	std::string text = obs_module_text(showing.empty() ? "Active.Status.None" : "Active.Status.Showing");
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, showing);
	obs_property_set_description(status, text.c_str());

	PickList items;
	items.setting = "items";
	items.title = "Active.Items";
	items.add = "List.AddItem";
	items.tip = "Active.Items.Tip";
	items.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, self ? self->source() : nullptr, items);

	obs_property_t *follow = obs_properties_add_bool(props, "follow", obs_module_text("Active.Follow"));
	obs_property_set_long_description(follow, obs_module_text("Active.Follow.Tip"));

	// How the change looks: any transition OBS has, except the ones that need
	// a file of their own.
	obs_property_t *transition = obs_properties_add_list(props, "transition", obs_module_text("Active.Transition"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(transition, obs_module_text("Active.Transition.Cut"), kCut);
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
		if (!id || strcmp(id, "cut_transition") == 0 || strcmp(id, "obs_stinger_transition") == 0)
			continue;
		const char *shown_as = obs_source_get_display_name(id);
		obs_property_list_add_string(transition, shown_as ? shown_as : id, id);
	}
	obs_property_list_add_string(transition, obs_module_text("Active.Transition.Keep"), kKeep);
	obs_property_set_long_description(transition, obs_module_text("Active.Transition.Tip"));
	obs_property_set_modified_callback(transition, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		const std::string kind = obs_data_get_string(settings, "transition");
		obs_property_set_visible(obs_properties_get(p, "transition_ms"), kind != kCut && kind != kKeep);
		return true;
	});
	obs_property_t *time = obs_properties_add_int_slider(props, "transition_ms",
							     obs_module_text("Active.TransitionTime"), 50, 2000, 50);
	obs_property_int_set_suffix(time, " ms");
	obs_property_t *ready = obs_properties_add_bool(props, "ready", obs_module_text("Active.Ready"));
	obs_property_set_long_description(ready, obs_module_text("Active.Ready.Tip"));

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Active.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_active_app()
{
	obs_source_info info = {};
	info.id = kActiveAppId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Active.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ActiveApp(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ActiveApp *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ActiveApp *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_bool(settings, "follow", true);
		obs_data_set_default_string(settings, "transition", "fade_transition");
		obs_data_set_default_int(settings, "transition_ms", 250);
		obs_data_set_default_bool(settings, "ready", true);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<ActiveApp *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<ActiveApp *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<ActiveApp *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<ActiveApp *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
