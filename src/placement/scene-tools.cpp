/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Three small filters for scenes. See scene-tools.hpp.

#include "scene-tools.hpp"

#include "../common/easing.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Set while this thread changes visibility or a filter's enabled state
// itself, so the resulting signal is not mistaken for the user's action.
thread_local bool t_self_change = false;

std::string with_name(const char *text_key, const std::string &name)
{
	std::string text = obs_module_text(text_key);
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, name);
	return text;
}

std::string source_name(obs_source_t *source)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	return name ? name : "";
}

// ======================================================= Show/Hide Animation

enum class ShowStyle : int { NearestEdge = 0, Left = 1, Right = 2, Top = 3, Bottom = 4, Pop = 5 };

class ShowHide {
public:
	ShowHide(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }
	~ShowHide()
	{
		detach();
		std::lock_guard lock(mutex_);
		if (item_) {
			apply(1.0); // never leave the item parked off screen
			obs_sceneitem_release(item_);
		}
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		item_name_ = obs_data_get_string(settings, "item");
		style_ = static_cast<ShowStyle>(obs_data_get_int(settings, "style"));
		in_s_ = std::max<long long>(0, obs_data_get_int(settings, "in_ms")) / 1000.0;
		out_s_ = std::max<long long>(0, obs_data_get_int(settings, "out_ms")) / 1000.0;
		curve_ = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	}

	void attach(obs_source_t *parent)
	{
		detach();
		parent_ = parent;
		signal_handler_connect(obs_source_get_signal_handler(parent_), "item_visible", visible_cb, this);
	}

	void detach()
	{
		if (parent_)
			signal_handler_disconnect(obs_source_get_signal_handler(parent_), "item_visible", visible_cb,
						  this);
		parent_ = nullptr;
	}

	void tick(float seconds)
	{
		std::lock_guard lock(mutex_);
		if (!item_)
			return;
		if (reshow_) {
			reshow_ = false;
			t_self_change = true;
			obs_sceneitem_set_visible(item_, true);
			t_self_change = false;
		}
		elapsed_ = std::min(duration_, elapsed_ + seconds);
		const double t = duration_ > 0.0 ? elapsed_ / duration_ : 1.0;
		// Arrive decelerating, leave accelerating.
		k_ = k_from_ + (k_to_ - k_from_) * ease(curve_, entering_ ? EaseMode::Out : EaseMode::In, t);
		apply(k_);
		if (elapsed_ < duration_)
			return;

		if (!entering_) {
			t_self_change = true;
			obs_sceneitem_set_visible(item_, false);
			t_self_change = false;
		}
		apply(1.0); // rest at home, shown or hidden
		obs_sceneitem_release(item_);
		item_ = nullptr;
	}

	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }

private:
	static void visible_cb(void *data, calldata_t *cd)
	{
		if (t_self_change)
			return;
		auto *self = static_cast<ShowHide *>(data);
		if (!obs_source_enabled(self->source_))
			return;
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		self->on_visible(item, calldata_bool(cd, "visible"));
	}

	// k = 1 is the item's normal place, k = 0 is fully out.
	void apply(double k)
	{
		vec2 pos, scale = home_scale_;
		if (pop_) {
			const double s = std::max(k, 0.001);
			vec2_set(&scale, (float)(home_scale_.x * s), (float)(home_scale_.y * s));
			vec2_set(&pos, (float)(center_.x + (home_pos_.x - center_.x) * s),
				 (float)(center_.y + (home_pos_.y - center_.y) * s));
			obs_sceneitem_set_scale(item_, &scale);
		} else {
			vec2_set(&pos, (float)(off_pos_.x + (home_pos_.x - off_pos_.x) * k),
				 (float)(off_pos_.y + (home_pos_.y - off_pos_.y) * k));
		}
		obs_sceneitem_set_pos(item_, &pos);
	}

	void capture_home(obs_sceneitem_t *item)
	{
		obs_sceneitem_addref(item);
		item_ = item;
		obs_sceneitem_get_pos(item, &home_pos_);
		obs_sceneitem_get_scale(item, &home_scale_);
		refresh_item(item);
		const Box box = item_box(item);
		vec2_set(&center_, (float)(box.x + box.w / 2.0), (float)(box.y + box.h / 2.0));
		const double w = obs_source_get_width(parent_), h = obs_source_get_height(parent_);

		// How far the item must travel to be fully out past each edge.
		const double left = box.x + box.w, right = w - box.x, top = box.y + box.h, bottom = h - box.y;
		ShowStyle style = style_;
		pop_ = style == ShowStyle::Pop && obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE;
		if (style == ShowStyle::Pop && !pop_)
			style = ShowStyle::NearestEdge; // a fixed bounding box cannot be scaled
		if (style == ShowStyle::NearestEdge) {
			const double nearest = std::min({left, right, top, bottom});
			style = nearest == left    ? ShowStyle::Left
				: nearest == right ? ShowStyle::Right
				: nearest == top   ? ShowStyle::Top
						   : ShowStyle::Bottom;
		}
		off_pos_ = home_pos_;
		if (style == ShowStyle::Left)
			off_pos_.x -= (float)left;
		else if (style == ShowStyle::Right)
			off_pos_.x += (float)right;
		else if (style == ShowStyle::Top)
			off_pos_.y -= (float)top;
		else if (style == ShowStyle::Bottom)
			off_pos_.y += (float)bottom;
	}

	void begin(bool entering)
	{
		entering_ = entering;
		k_from_ = k_;
		k_to_ = entering ? 1.0 : 0.0;
		elapsed_ = 0.0;
		duration_ = (entering ? in_s_ : out_s_) * std::fabs(k_to_ - k_from_);
	}

	void on_visible(obs_sceneitem_t *item, bool visible)
	{
		std::lock_guard lock(mutex_);
		if (!parent_ || !item || source_name(obs_sceneitem_get_source(item)) != item_name_)
			return;
		if (item_ && item_ != item)
			return;

		if (visible) {
			if (!item_) {
				capture_home(item);
				k_ = 0.0;
				apply(0.0); // start out of view, on the same frame it is shown
			}
			reshow_ = false;
			begin(true);
			return;
		}

		// Hidden. A hidden item is not drawn, so show it again while it leaves.
		const bool leaving = item_ && !entering_;
		if (!item_) {
			capture_home(item);
			k_ = 1.0;
		}
		// OBS finishes hiding the item after this signal returns, so it is
		// shown again at the start of the next frame, before anything is drawn.
		reshow_ = true;
		// "Hide" pressed again while already leaving brings it back instead.
		begin(leaving);
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::string item_name_;
	ShowStyle style_ = ShowStyle::NearestEdge;
	double in_s_ = 0.4, out_s_ = 0.3;
	EaseCurve curve_ = EaseCurve::Cubic;

	obs_sceneitem_t *item_ = nullptr; // non-null while animating
	bool reshow_ = false;
	bool entering_ = true;
	bool pop_ = false;
	vec2 home_pos_ = {}, home_scale_ = {}, off_pos_ = {}, center_ = {};
	double k_ = 1.0, k_from_ = 1.0, k_to_ = 1.0;
	double elapsed_ = 0.0, duration_ = 0.0;
};

obs_properties_t *show_hide_properties(void *data)
{
	auto *self = static_cast<ShowHide *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("ShowHide.Item"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_scene_items_to_list(self ? self->parent() : nullptr, item);
	obs_property_t *style = obs_properties_add_list(props, "style", obs_module_text("ShowHide.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	const char *styles[] = {"ShowHide.Nearest", "ShowHide.Left",   "ShowHide.Right",
				"ShowHide.Top",     "ShowHide.Bottom", "ShowHide.Pop"};
	for (int i = 0; i < 6; i++)
		obs_property_list_add_int(style, obs_module_text(styles[i]), i);
	obs_property_t *in = obs_properties_add_int_slider(props, "in_ms", obs_module_text("ShowHide.In"), 0, 5000, 50);
	obs_property_int_set_suffix(in, " ms");
	obs_property_t *out =
		obs_properties_add_int_slider(props, "out_ms", obs_module_text("ShowHide.Out"), 0, 5000, 50);
	obs_property_int_set_suffix(out, " ms");
	obs_property_t *curve = obs_properties_add_list(props, "ease_curve", obs_module_text("Easing.Curve"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	const char *curves[] = {"Easing.Linear", "Easing.Sine",    "Easing.Quad",  "Easing.Cubic",
				"Easing.Quart",  "Easing.Quint",   "Easing.Expo",  "Easing.Circ",
				"Easing.Back",   "Easing.Elastic", "Easing.Bounce"};
	for (int i = 0; i < 11; i++)
		obs_property_list_add_int(curve, obs_module_text(curves[i]), i);
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("ShowHide.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

// ================================================================ Match Item

enum class MatchMode : int { CopyTransform = 0, FitBox = 1 };

bool differs(float a, float b)
{
	return std::fabs(a - b) > 0.01f;
}

class MatchItem {
public:
	MatchItem(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		item_ = obs_data_get_string(settings, "item");
		master_scene_ = obs_data_get_string(settings, "master_scene");
		master_item_ = obs_data_get_string(settings, "master_item");
		mode_ = static_cast<MatchMode>(obs_data_get_int(settings, "mode"));
		fit_align_ = (int)obs_data_get_int(settings, "fit_align");
	}

	void tick()
	{
		if (!parent_ || !obs_source_enabled(source_))
			return;
		std::string item_name, scene_name, master_name;
		MatchMode mode;
		int fit_align;
		{
			std::lock_guard lock(mutex_);
			item_name = item_;
			scene_name = master_scene_;
			master_name = master_item_;
			mode = mode_;
			fit_align = fit_align_;
		}
		if (item_name.empty() || scene_name.empty() || master_name.empty())
			return;
		obs_sceneitem_t *item = find_item(parent_, item_name);
		obs_source_t *master_source = obs_get_source_by_name(scene_name.c_str());
		obs_sceneitem_t *master = find_item(master_source, master_name);
		if (item && master && item != master)
			mode == MatchMode::FitBox ? fit(parent_, item, master_source, master, fit_align)
						  : copy(item, master);
		obs_source_release(master_source);
	}

	void attach(obs_source_t *parent) { parent_ = parent; }
	void detach() { parent_ = nullptr; }
	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }

private:
	// Everything is only written when it differs, so an idle scene is left alone.
	static void set_pos(obs_sceneitem_t *item, float x, float y)
	{
		vec2 cur;
		obs_sceneitem_get_pos(item, &cur);
		if (differs(cur.x, x) || differs(cur.y, y)) {
			vec2_set(&cur, x, y);
			obs_sceneitem_set_pos(item, &cur);
		}
	}

	static void set_bounds(obs_sceneitem_t *item, obs_bounds_type type, float w, float h, uint32_t alignment)
	{
		vec2 cur;
		obs_sceneitem_get_bounds(item, &cur);
		if (obs_sceneitem_get_bounds_type(item) != type)
			obs_sceneitem_set_bounds_type(item, type);
		if (differs(cur.x, w) || differs(cur.y, h)) {
			vec2_set(&cur, w, h);
			obs_sceneitem_set_bounds(item, &cur);
		}
		if (obs_sceneitem_get_bounds_alignment(item) != alignment)
			obs_sceneitem_set_bounds_alignment(item, alignment);
	}

	static void copy(obs_sceneitem_t *item, obs_sceneitem_t *master)
	{
		vec2 a, b;
		obs_sceneitem_get_pos(master, &a);
		set_pos(item, a.x, a.y);
		obs_sceneitem_get_scale(master, &a);
		obs_sceneitem_get_scale(item, &b);
		if (differs(a.x, b.x) || differs(a.y, b.y))
			obs_sceneitem_set_scale(item, &a);
		if (differs(obs_sceneitem_get_rot(master), obs_sceneitem_get_rot(item)))
			obs_sceneitem_set_rot(item, obs_sceneitem_get_rot(master));
		if (obs_sceneitem_get_alignment(master) != obs_sceneitem_get_alignment(item))
			obs_sceneitem_set_alignment(item, obs_sceneitem_get_alignment(master));
		obs_sceneitem_get_bounds(master, &a);
		set_bounds(item, obs_sceneitem_get_bounds_type(master), a.x, a.y,
			   obs_sceneitem_get_bounds_alignment(master));
	}

	// Scales the item to fit inside the box the master occupies, so text of
	// any length lands in the same place.
	static void fit(obs_source_t *scene, obs_sceneitem_t *item, obs_source_t *master_scene, obs_sceneitem_t *master,
			int fit_align)
	{
		// Where the master sits on its scene, whether or not it is in a group...
		Box box = scene_box(master_scene, master);
		if (box.w < 1.5 && box.h < 1.5) {
			refresh_item(master); // never measured yet
			box = scene_box(master_scene, master);
		}
		if (box.w < 1.0 || box.h < 1.0)
			return;
		// ...expressed in this item's own coordinates, in case it is in a group.
		const Space space = item_space(scene, item);
		box = {(box.x - space.ox) / space.sx, (box.y - space.oy) / space.sy, box.w / space.sx,
		       box.h / space.sy};
		if (obs_sceneitem_get_alignment(item) != (OBS_ALIGN_LEFT | OBS_ALIGN_TOP))
			obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		if (differs(obs_sceneitem_get_rot(item), 0.0f))
			obs_sceneitem_set_rot(item, 0.0f);
		const uint32_t align = fit_align == 0   ? OBS_ALIGN_LEFT
				       : fit_align == 2 ? OBS_ALIGN_RIGHT
							: OBS_ALIGN_CENTER;
		set_bounds(item, OBS_BOUNDS_SCALE_INNER, (float)box.w, (float)box.h, align);
		set_pos(item, (float)box.x, (float)box.y);
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::string item_, master_scene_, master_item_;
	MatchMode mode_ = MatchMode::FitBox;
	int fit_align_ = 0;
};

bool master_scene_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *list = obs_properties_get(props, "master_item");
	obs_property_list_clear(list);
	obs_source_t *scene = obs_get_source_by_name(obs_data_get_string(settings, "master_scene"));
	add_scene_items_to_list(scene, list);
	obs_source_release(scene);
	obs_property_set_visible(obs_properties_get(props, "fit_align"),
				 obs_data_get_int(settings, "mode") == (int)MatchMode::FitBox);
	return true;
}

obs_properties_t *match_item_properties(void *data)
{
	auto *self = static_cast<MatchItem *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("Match.Item"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_scene_items_to_list(self ? self->parent() : nullptr, item);

	obs_property_t *scene = obs_properties_add_list(props, "master_scene", obs_module_text("Match.MasterScene"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			if (name && *name)
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		scene);
	obs_property_set_modified_callback(scene, master_scene_modified);
	obs_properties_add_list(props, "master_item", obs_module_text("Match.MasterItem"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	obs_property_t *mode = obs_properties_add_list(props, "mode", obs_module_text("Match.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Match.Mode.Fit"), (int)MatchMode::FitBox);
	obs_property_list_add_int(mode, obs_module_text("Match.Mode.Copy"), (int)MatchMode::CopyTransform);
	obs_property_set_modified_callback(mode, master_scene_modified);
	obs_property_t *align = obs_properties_add_list(props, "fit_align", obs_module_text("Match.FitAlign"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(align, obs_module_text("Match.FitAlign.Left"), 0);
	obs_property_list_add_int(align, obs_module_text("Match.FitAlign.Center"), 1);
	obs_property_list_add_int(align, obs_module_text("Match.FitAlign.Right"), 2);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Match.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

// ==================================================================== Layout

struct Placement {
	std::string name;
	vec2 pos, scale;
	float rot = 0.0f;
};

struct Glide {
	obs_sceneitem_t *item;
	Placement from, to;
};

class Layout {
public:
	Layout(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		signal_handler_t *sh = obs_source_get_signal_handler(source_);
		signal_handler_connect(sh, "enable", enable_cb, this);
		signal_handler_connect(sh, "rename", rename_cb, this);
		proc_handler_t *ph = obs_source_get_proc_handler(source_);
		proc_handler_add(
			ph, "void trigger()",
			[](void *data, calldata_t *) { static_cast<Layout *>(data)->pending_ = true; }, this);
		proc_handler_add(
			ph, "void save()",
			[](void *data, calldata_t *) { static_cast<Layout *>(data)->save_current(); }, this);
	}

	~Layout()
	{
		signal_handler_t *sh = obs_source_get_signal_handler(source_);
		signal_handler_disconnect(sh, "enable", enable_cb, this);
		signal_handler_disconnect(sh, "rename", rename_cb, this);
		unregister_hotkey();
		release_glides();
	}

	void update(obs_data_t *settings)
	{
		std::vector<Placement> saved;
		obs_data_array_t *array = obs_data_get_array(settings, "saved");
		for (size_t i = 0; i < obs_data_array_count(array); i++) {
			obs_data_t *entry = obs_data_array_item(array, i);
			Placement p;
			p.name = obs_data_get_string(entry, "name");
			vec2_set(&p.pos, (float)obs_data_get_double(entry, "x"),
				 (float)obs_data_get_double(entry, "y"));
			vec2_set(&p.scale, (float)obs_data_get_double(entry, "sx"),
				 (float)obs_data_get_double(entry, "sy"));
			p.rot = (float)obs_data_get_double(entry, "rot");
			saved.push_back(p);
			obs_data_release(entry);
		}
		obs_data_array_release(array);

		std::lock_guard lock(mutex_);
		saved_ = std::move(saved);
		duration_s_ = std::max<long long>(0, obs_data_get_int(settings, "duration_ms")) / 1000.0;
		curve_ = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		mode_ = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	}

	// Stores where the scene's items currently are: all of them, or only the
	// ones named in "only_items" when that list is not empty. Items inside
	// groups are included.
	bool save_current()
	{
		obs_scene_t *scene = scene_of(parent_);
		if (!scene)
			return false;

		struct Capture {
			obs_data_array_t *array;
			std::vector<std::string> only;
		} capture{obs_data_array_create(), {}};
		{
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_array_t *only = obs_data_get_array(settings, "only_items");
			for (size_t i = 0; i < obs_data_array_count(only); i++) {
				obs_data_t *entry = obs_data_array_item(only, i);
				const std::string name = obs_data_get_string(entry, "value");
				if (!name.empty())
					capture.only.push_back(name);
				obs_data_release(entry);
			}
			obs_data_array_release(only);
			obs_data_release(settings);
		}

		static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Capture *>(param);
			const std::string name = source_name(obs_sceneitem_get_source(item));
			if (!c->only.empty() && std::find(c->only.begin(), c->only.end(), name) == c->only.end())
				return true;
			vec2 pos, scale;
			obs_sceneitem_get_pos(item, &pos);
			obs_sceneitem_get_scale(item, &scale);
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "name", name.c_str());
			obs_data_set_double(entry, "x", pos.x);
			obs_data_set_double(entry, "y", pos.y);
			obs_data_set_double(entry, "sx", scale.x);
			obs_data_set_double(entry, "sy", scale.y);
			obs_data_set_double(entry, "rot", obs_sceneitem_get_rot(item));
			obs_data_array_push_back(c->array, entry);
			obs_data_release(entry);
			return true;
		};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
				add(sc, item, param);
				if (obs_sceneitem_is_group(item))
					obs_sceneitem_group_enum_items(item, add, param);
				return true;
			},
			&capture);
		obs_data_array_t *array = capture.array;
		obs_data_t *settings = obs_source_get_settings(source_);
		obs_data_set_array(settings, "saved", array);
		obs_data_set_int(settings, "saved_count", (long long)obs_data_array_count(array));
		obs_source_update(source_, settings);
		obs_data_release(settings);
		obs_data_array_release(array);
		return true;
	}

	void tick(float seconds)
	{
		if (!ready_) {
			set_enabled_quietly(false); // do not fire when OBS loads
			ready_ = true;
			return;
		}
		if (pending_.exchange(false))
			start();
		if (glides_.empty())
			return;
		elapsed_ = std::min(duration_, elapsed_ + seconds);
		const double t = ease(run_curve_, run_mode_, duration_ > 0.0 ? elapsed_ / duration_ : 1.0);
		apply(elapsed_ >= duration_ ? 1.0 : t);
		if (elapsed_ >= duration_) {
			release_glides();
			set_enabled_quietly(false);
		}
	}

	void attach(obs_source_t *parent)
	{
		parent_ = parent;
		register_hotkey();
	}
	void detach()
	{
		unregister_hotkey();
		parent_ = nullptr;
	}
	void trigger() { pending_ = true; }
	obs_source_t *source() const { return source_; }

private:
	void start()
	{
		release_glides();
		std::vector<Placement> saved;
		{
			std::lock_guard lock(mutex_);
			saved = saved_;
			duration_ = duration_s_;
			run_curve_ = curve_;
			run_mode_ = mode_;
		}
		for (const Placement &to : saved) {
			obs_sceneitem_t *item = find_item(parent_, to.name);
			if (!item)
				continue;
			obs_sceneitem_addref(item);
			Glide g{item, to, to};
			obs_sceneitem_get_pos(item, &g.from.pos);
			obs_sceneitem_get_scale(item, &g.from.scale);
			g.from.rot = obs_sceneitem_get_rot(item);
			glides_.push_back(g);
		}
		elapsed_ = 0.0;
		if (glides_.empty())
			set_enabled_quietly(false);
	}

	void apply(double t)
	{
		for (const Glide &g : glides_) {
			vec2 pos, scale;
			vec2_set(&pos, (float)(g.from.pos.x + (g.to.pos.x - g.from.pos.x) * t),
				 (float)(g.from.pos.y + (g.to.pos.y - g.from.pos.y) * t));
			vec2_set(&scale, (float)(g.from.scale.x + (g.to.scale.x - g.from.scale.x) * t),
				 (float)(g.from.scale.y + (g.to.scale.y - g.from.scale.y) * t));
			obs_sceneitem_set_pos(g.item, &pos);
			obs_sceneitem_set_scale(g.item, &scale);
			obs_sceneitem_set_rot(g.item, (float)(g.from.rot + (g.to.rot - g.from.rot) * t));
		}
	}

	void release_glides()
	{
		for (Glide &g : glides_)
			obs_sceneitem_release(g.item);
		glides_.clear();
	}

	void set_enabled_quietly(bool enabled)
	{
		if (obs_source_enabled(source_) == enabled)
			return;
		t_self_change = true;
		obs_source_set_enabled(source_, enabled);
		t_self_change = false;
	}

	static void enable_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<Layout *>(data);
		if (!t_self_change && self->ready_ && calldata_bool(cd, "enabled"))
			self->pending_ = true;
	}

	void register_hotkey()
	{
		unregister_hotkey();
		if (!parent_)
			return;
		const std::string name = source_name(source_);
		hotkey_ = obs_hotkey_register_source(
			parent_, ("stagekit.layout." + name).c_str(), with_name("Hotkey.Layout", name).c_str(),
			[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
				if (pressed)
					static_cast<Layout *>(data)->pending_ = true;
			},
			this);
	}

	void unregister_hotkey()
	{
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		hotkey_ = OBS_INVALID_HOTKEY_ID;
	}

	static void rename_cb(void *data, calldata_t *)
	{
		auto *self = static_cast<Layout *>(data);
		if (!self->parent_)
			return;
		obs_data_array_t *bindings = obs_hotkey_save(self->hotkey_);
		self->register_hotkey();
		obs_hotkey_load(self->hotkey_, bindings);
		obs_data_array_release(bindings);
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::vector<Placement> saved_;
	double duration_s_ = 0.6;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;

	std::atomic<bool> pending_{false};
	std::atomic<bool> ready_{false};
	std::vector<Glide> glides_;
	double elapsed_ = 0.0, duration_ = 0.0;
	EaseCurve run_curve_ = EaseCurve::Cubic;
	EaseMode run_mode_ = EaseMode::InOut;
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
};

obs_properties_t *layout_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_button2(
		props, "save", obs_module_text("Layout.Save"),
		[](obs_properties_t *, obs_property_t *, void *d) { return static_cast<Layout *>(d)->save_current(); },
		data);
	obs_properties_add_editable_list(props, "only_items", obs_module_text("Layout.OnlyItems"),
					 OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
	obs_property_t *count =
		obs_properties_add_int(props, "saved_count", obs_module_text("Layout.SavedCount"), 0, 100000, 1);
	obs_property_set_enabled(count, false);
	obs_property_t *duration =
		obs_properties_add_int_slider(props, "duration_ms", obs_module_text("Place.Duration"), 0, 5000, 50);
	obs_property_int_set_suffix(duration, " ms");
	add_easing_properties(props, "");
	obs_properties_add_button2(
		props, "go", obs_module_text("Layout.Go"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			static_cast<Layout *>(d)->trigger();
			return false;
		},
		data);
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Layout.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

// A controller filter never changes the picture.
template<typename T> void pass_through(void *data, gs_effect_t *)
{
	obs_source_skip_video_filter(static_cast<T *>(data)->source());
}

} // namespace

void register_show_hide()
{
	obs_source_info info = {};
	info.id = kShowHideId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("ShowHide.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ShowHide(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ShowHide *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ShowHide *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "style", (int)ShowStyle::NearestEdge);
		obs_data_set_default_int(settings, "in_ms", 400);
		obs_data_set_default_int(settings, "out_ms", 300);
		obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	};
	info.get_properties = show_hide_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<ShowHide *>(data)->tick(seconds);
	};
	info.video_render = pass_through<ShowHide>;
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<ShowHide *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<ShowHide *>(data)->detach();
	};
	obs_register_source(&info);
}

void register_match_item()
{
	obs_source_info info = {};
	info.id = kMatchItemId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Match.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new MatchItem(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<MatchItem *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<MatchItem *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "mode", (int)MatchMode::FitBox);
		obs_data_set_default_int(settings, "fit_align", 0);
	};
	info.get_properties = match_item_properties;
	info.video_tick = [](void *data, float) {
		static_cast<MatchItem *>(data)->tick();
	};
	info.video_render = pass_through<MatchItem>;
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<MatchItem *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<MatchItem *>(data)->detach();
	};
	obs_register_source(&info);
}

void register_layout()
{
	obs_source_info info = {};
	info.id = kLayoutId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Layout.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Layout(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Layout *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Layout *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "duration_ms", 600);
		obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
		obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
	};
	info.get_properties = layout_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<Layout *>(data)->tick(seconds);
	};
	info.video_render = pass_through<Layout>;
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<Layout *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<Layout *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
