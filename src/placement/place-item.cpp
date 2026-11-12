/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Place Item: a filter you add to a scene. Each one is a destination for an
// item in that scene: "send the camera to the top right". Switching the
// filter on (eye icon, Stream Deck OBS plugin "Filter" key, hotkey,
// obs-websocket) glides the item there; the filter then switches itself
// back off, ready for next time.
//
// Destinations are a 3x3 grid of screen positions with a margin (no
// coordinates to type) or a saved spot captured from wherever the item
// currently is. A linked item (a frame, a widget) can travel with it and
// flips to the other side when it would otherwise leave the screen.

#include "place-item.hpp"

#include "../common/easing.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

enum class Destination : int {
	TopLeft = 0,
	TopCenter = 1,
	TopRight = 2,
	MiddleLeft = 3,
	Center = 4,
	MiddleRight = 5,
	BottomLeft = 6,
	BottomCenter = 7,
	BottomRight = 8,
	Saved = 9,
};

struct Settings {
	std::string item;
	Destination destination = Destination::TopRight;
	double margin = 30.0;
	double saved_x = 0.0;
	double saved_y = 0.0;
	bool restore_size = false;
	double saved_scale_x = 1.0;
	double saved_scale_y = 1.0;
	double duration_s = 0.5;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	std::string linked;
	bool flip_linked = true;
};

struct Box {
	double x = 0, y = 0, w = 0, h = 0;
};

// The item's bounding box on the scene canvas, whatever its alignment,
// crop, bounds or rotation.
Box item_box(obs_sceneitem_t *item)
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(item, &transform);
	double min_x = 1e18, min_y = 1e18, max_x = -1e18, max_y = -1e18;
	for (int i = 0; i < 4; i++) {
		vec3 corner, out;
		vec3_set(&corner, (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, 0.0f);
		vec3_transform(&out, &corner, &transform);
		min_x = std::min(min_x, (double)out.x);
		min_y = std::min(min_y, (double)out.y);
		max_x = std::max(max_x, (double)out.x);
		max_y = std::max(max_y, (double)out.y);
	}
	return {min_x, min_y, max_x - min_x, max_y - min_y};
}

struct Move {
	obs_sceneitem_t *item = nullptr; // strong reference while moving
	vec2 from_pos, to_pos;
	vec2 from_scale, to_scale;
	bool scale = false;
};

void release_move(Move &move)
{
	if (move.item)
		obs_sceneitem_release(move.item);
	move.item = nullptr;
}

void apply_move(const Move &move, double t)
{
	if (!move.item)
		return;
	vec2 pos;
	pos.x = (float)(move.from_pos.x + (move.to_pos.x - move.from_pos.x) * t);
	pos.y = (float)(move.from_pos.y + (move.to_pos.y - move.from_pos.y) * t);
	obs_sceneitem_set_pos(move.item, &pos);
	if (move.scale) {
		vec2 scale;
		scale.x = (float)(move.from_scale.x + (move.to_scale.x - move.from_scale.x) * t);
		scale.y = (float)(move.from_scale.y + (move.to_scale.y - move.from_scale.y) * t);
		obs_sceneitem_set_scale(move.item, &scale);
	}
}

class PlaceItem {
public:
	PlaceItem(obs_data_t *settings, obs_source_t *source);
	~PlaceItem();

	void update(obs_data_t *settings);
	void tick(float seconds);
	void attach(obs_source_t *parent);
	void detach();
	void trigger() { pending_ = true; }
	bool capture_current();
	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }

private:
	void start();
	void finish();
	void register_hotkey();
	void unregister_hotkey();
	void set_enabled_quietly(bool enabled);

	static void enable_cb(void *data, calldata_t *cd);
	static void rename_cb(void *data, calldata_t *cd);

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	Settings settings_;
	std::atomic<bool> pending_{false};
	std::atomic<bool> ready_{false}; // ignore the enabled state restored at load

	bool moving_ = false;
	double elapsed_ = 0.0;
	double duration_ = 0.0;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	Move leader_;
	Move follower_;

	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
};

thread_local bool t_changing_enabled = false;

PlaceItem::PlaceItem(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	update(settings);
	signal_handler_t *sh = obs_source_get_signal_handler(source_);
	signal_handler_connect(sh, "enable", enable_cb, this);
	signal_handler_connect(sh, "rename", rename_cb, this);
	proc_handler_add(
		obs_source_get_proc_handler(source_), "void trigger()",
		[](void *data, calldata_t *) { static_cast<PlaceItem *>(data)->trigger(); }, this);
}

PlaceItem::~PlaceItem()
{
	signal_handler_t *sh = obs_source_get_signal_handler(source_);
	signal_handler_disconnect(sh, "enable", enable_cb, this);
	signal_handler_disconnect(sh, "rename", rename_cb, this);
	unregister_hotkey();
	release_move(leader_);
	release_move(follower_);
}

void PlaceItem::update(obs_data_t *settings)
{
	Settings s;
	s.item = obs_data_get_string(settings, "item");
	s.destination = static_cast<Destination>(obs_data_get_int(settings, "destination"));
	s.margin = obs_data_get_double(settings, "margin");
	s.saved_x = obs_data_get_double(settings, "saved_x");
	s.saved_y = obs_data_get_double(settings, "saved_y");
	s.restore_size = obs_data_get_bool(settings, "restore_size");
	s.saved_scale_x = obs_data_get_double(settings, "saved_scale_x");
	s.saved_scale_y = obs_data_get_double(settings, "saved_scale_y");
	s.duration_s = std::max<long long>(0, obs_data_get_int(settings, "duration_ms")) / 1000.0;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	s.linked = obs_data_get_string(settings, "linked");
	s.flip_linked = obs_data_get_bool(settings, "flip_linked");

	std::lock_guard lock(mutex_);
	settings_ = s;
}

void PlaceItem::set_enabled_quietly(bool enabled)
{
	if (obs_source_enabled(source_) == enabled)
		return;
	t_changing_enabled = true;
	obs_source_set_enabled(source_, enabled);
	t_changing_enabled = false;
}

void PlaceItem::enable_cb(void *data, calldata_t *cd)
{
	auto *self = static_cast<PlaceItem *>(data);
	if (!t_changing_enabled && self->ready_ && calldata_bool(cd, "enabled"))
		self->trigger();
}

// Works out where everything should end up and starts the glide. Runs on
// the graphics thread, between frames.
void PlaceItem::start()
{
	Settings s;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
	}
	release_move(leader_);
	release_move(follower_);
	moving_ = false;

	obs_scene_t *scene = parent_ ? obs_scene_from_source(parent_) : nullptr;
	if (!scene)
		scene = parent_ ? obs_group_from_source(parent_) : nullptr;
	obs_sceneitem_t *item = scene && !s.item.empty() ? obs_scene_find_source(scene, s.item.c_str()) : nullptr;
	if (!item) {
		obs_log(LOG_WARNING, "Place Item '%s': item '%s' is not in this scene", obs_source_get_name(source_),
			s.item.c_str());
		finish();
		return;
	}
	const double canvas_w = obs_source_get_width(parent_);
	const double canvas_h = obs_source_get_height(parent_);

	// Leader.
	obs_sceneitem_addref(item);
	leader_.item = item;
	obs_sceneitem_get_pos(item, &leader_.from_pos);
	obs_sceneitem_get_scale(item, &leader_.from_scale);
	leader_.to_scale = leader_.from_scale;
	leader_.scale = false;
	const Box box = item_box(item);
	Box target = box;

	if (s.destination == Destination::Saved) {
		vec2_set(&leader_.to_pos, (float)s.saved_x, (float)s.saved_y);
		if (s.restore_size && s.saved_scale_x > 0.0 && s.saved_scale_y > 0.0) {
			vec2_set(&leader_.to_scale, (float)s.saved_scale_x, (float)s.saved_scale_y);
			leader_.scale = true;
		}
		// Measure the box at the destination, then put the item back.
		apply_move(leader_, 1.0);
		target = item_box(item);
		apply_move(leader_, 0.0);
	} else {
		const int column = static_cast<int>(s.destination) % 3;
		const int row = static_cast<int>(s.destination) / 3;
		target.x = column == 0   ? s.margin
			   : column == 1 ? (canvas_w - box.w) / 2.0
					 : canvas_w - s.margin - box.w;
		target.y = row == 0 ? s.margin : row == 1 ? (canvas_h - box.h) / 2.0 : canvas_h - s.margin - box.h;
		leader_.to_pos.x = leader_.from_pos.x + (float)(target.x - box.x);
		leader_.to_pos.y = leader_.from_pos.y + (float)(target.y - box.y);
	}

	// Linked item: keeps its place relative to the leader, mirrored to the
	// other side if that is the only way to stay on screen.
	obs_sceneitem_t *linked =
		!s.linked.empty() && s.linked != s.item ? obs_scene_find_source(scene, s.linked.c_str()) : nullptr;
	if (linked) {
		obs_sceneitem_addref(linked);
		follower_.item = linked;
		follower_.scale = false;
		obs_sceneitem_get_pos(linked, &follower_.from_pos);
		const Box fbox = item_box(linked);
		const double dx = fbox.x - box.x;
		const double dy = fbox.y - box.y;
		double tx = target.x + dx;
		double ty = target.y + dy;
		if (s.flip_linked) {
			auto off_screen = [](double pos, double size, double limit) {
				return std::max(0.0, -pos) + std::max(0.0, pos + size - limit);
			};
			const double mirrored_x = target.x + target.w - (dx + fbox.w);
			const double mirrored_y = target.y + target.h - (dy + fbox.h);
			if (off_screen(mirrored_x, fbox.w, canvas_w) < off_screen(tx, fbox.w, canvas_w))
				tx = mirrored_x;
			if (off_screen(mirrored_y, fbox.h, canvas_h) < off_screen(ty, fbox.h, canvas_h))
				ty = mirrored_y;
		}
		follower_.to_pos.x = follower_.from_pos.x + (float)(tx - fbox.x);
		follower_.to_pos.y = follower_.from_pos.y + (float)(ty - fbox.y);
	}

	elapsed_ = 0.0;
	duration_ = s.duration_s;
	curve_ = s.curve;
	mode_ = s.mode;
	moving_ = true;
	if (duration_ <= 0.0) {
		apply_move(leader_, 1.0);
		apply_move(follower_, 1.0);
		finish();
	}
}

void PlaceItem::finish()
{
	moving_ = false;
	release_move(leader_);
	release_move(follower_);
	// Off again, so the same key or switch can trigger the next move.
	set_enabled_quietly(false);
}

void PlaceItem::tick(float seconds)
{
	if (!ready_) {
		// A filter saved in the "on" state should not fire when OBS loads.
		set_enabled_quietly(false);
		ready_ = true;
		return;
	}
	if (pending_.exchange(false))
		start();
	if (!moving_)
		return;

	elapsed_ = std::min(duration_, elapsed_ + seconds);
	const double t = ease(curve_, mode_, duration_ > 0.0 ? elapsed_ / duration_ : 1.0);
	apply_move(leader_, t);
	apply_move(follower_, t);
	if (elapsed_ >= duration_) {
		apply_move(leader_, 1.0);
		apply_move(follower_, 1.0);
		finish();
	}
}

// Stores the item's current position and size as the saved spot.
bool PlaceItem::capture_current()
{
	obs_data_t *settings = obs_source_get_settings(source_);
	const std::string name = obs_data_get_string(settings, "item");
	obs_scene_t *scene = parent_ ? obs_scene_from_source(parent_) : nullptr;
	if (!scene)
		scene = parent_ ? obs_group_from_source(parent_) : nullptr;
	obs_sceneitem_t *item = scene && !name.empty() ? obs_scene_find_source(scene, name.c_str()) : nullptr;
	if (item) {
		vec2 pos, scale;
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_get_scale(item, &scale);
		obs_data_set_double(settings, "saved_x", pos.x);
		obs_data_set_double(settings, "saved_y", pos.y);
		obs_data_set_double(settings, "saved_scale_x", scale.x);
		obs_data_set_double(settings, "saved_scale_y", scale.y);
		obs_data_set_int(settings, "destination", (int)Destination::Saved);
		obs_source_update(source_, settings);
	}
	obs_data_release(settings);
	return item != nullptr;
}

// ------------------------------------------------------------------ hotkey

void PlaceItem::register_hotkey()
{
	unregister_hotkey();
	if (!parent_)
		return;
	const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
	const std::string id = "stagekit.place." + name;
	std::string text = obs_module_text("Hotkey.Place");
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, name);
	hotkey_ = obs_hotkey_register_source(
		parent_, id.c_str(), text.c_str(),
		[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				static_cast<PlaceItem *>(data)->trigger();
		},
		this);
}

void PlaceItem::unregister_hotkey()
{
	if (hotkey_ != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hotkey_);
	hotkey_ = OBS_INVALID_HOTKEY_ID;
}

void PlaceItem::rename_cb(void *data, calldata_t *)
{
	auto *self = static_cast<PlaceItem *>(data);
	if (!self->parent_)
		return;
	obs_data_array_t *bindings = obs_hotkey_save(self->hotkey_);
	self->register_hotkey();
	obs_hotkey_load(self->hotkey_, bindings);
	obs_data_array_release(bindings);
}

void PlaceItem::attach(obs_source_t *parent)
{
	parent_ = parent;
	register_hotkey();
}

void PlaceItem::detach()
{
	unregister_hotkey();
	parent_ = nullptr;
}

// -------------------------------------------------------------- properties

bool destination_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool saved = static_cast<Destination>(obs_data_get_int(settings, "destination")) == Destination::Saved;
	const bool linked = *obs_data_get_string(settings, "linked") != 0;
	obs_property_set_visible(obs_properties_get(props, "margin"), !saved);
	obs_property_set_visible(obs_properties_get(props, "saved_x"), saved);
	obs_property_set_visible(obs_properties_get(props, "saved_y"), saved);
	obs_property_set_visible(obs_properties_get(props, "restore_size"), saved);
	obs_property_set_visible(obs_properties_get(props, "flip_linked"), linked);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<PlaceItem *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("Place.Item"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_t *linked = obs_properties_add_list(props, "linked", obs_module_text("Place.Linked"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(linked, obs_module_text("Place.Linked.None"), "");
	obs_source_t *parent = self ? self->parent() : nullptr;
	obs_scene_t *scene = parent ? obs_scene_from_source(parent) : nullptr;
	if (!scene)
		scene = parent ? obs_group_from_source(parent) : nullptr;
	if (scene) {
		struct Lists {
			obs_property_t *item, *linked;
		} lists = {item, linked};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *scene_item, void *param) {
				auto *l = static_cast<Lists *>(param);
				const char *name = obs_source_get_name(obs_sceneitem_get_source(scene_item));
				if (name && *name) {
					obs_property_list_add_string(l->item, name, name);
					obs_property_list_add_string(l->linked, name, name);
				}
				return true;
			},
			&lists);
	}
	obs_property_set_modified_callback(linked, destination_modified);

	obs_property_t *dest = obs_properties_add_list(props, "destination", obs_module_text("Place.Destination"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	const char *names[] = {"Place.TopLeft",     "Place.TopCenter",   "Place.TopRight",   "Place.MiddleLeft",
			       "Place.Center",      "Place.MiddleRight", "Place.BottomLeft", "Place.BottomCenter",
			       "Place.BottomRight", "Place.Saved"};
	for (int i = 0; i < 10; i++)
		obs_property_list_add_int(dest, obs_module_text(names[i]), i);
	obs_property_set_modified_callback(dest, destination_modified);

	obs_property_t *margin =
		obs_properties_add_float_slider(props, "margin", obs_module_text("Place.Margin"), 0.0, 400.0, 1.0);
	obs_property_float_set_suffix(margin, " px");
	obs_properties_add_float(props, "saved_x", obs_module_text("Place.SavedX"), -20000.0, 20000.0, 1.0);
	obs_properties_add_float(props, "saved_y", obs_module_text("Place.SavedY"), -20000.0, 20000.0, 1.0);
	obs_properties_add_bool(props, "restore_size", obs_module_text("Place.RestoreSize"));
	obs_properties_add_button2(
		props, "capture", obs_module_text("Place.Capture"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			return static_cast<PlaceItem *>(d)->capture_current();
		},
		data);

	obs_properties_add_bool(props, "flip_linked", obs_module_text("Place.FlipLinked"));

	obs_property_t *duration =
		obs_properties_add_int_slider(props, "duration_ms", obs_module_text("Place.Duration"), 0, 5000, 50);
	obs_property_int_set_suffix(duration, " ms");
	add_easing_properties(props, "");

	obs_properties_add_button2(
		props, "go", obs_module_text("Place.Go"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			static_cast<PlaceItem *>(d)->trigger();
			return false;
		},
		data);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Place.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "destination", (int)Destination::TopRight);
	obs_data_set_default_double(settings, "margin", 30.0);
	obs_data_set_default_bool(settings, "restore_size", false);
	obs_data_set_default_double(settings, "saved_scale_x", 1.0);
	obs_data_set_default_double(settings, "saved_scale_y", 1.0);
	obs_data_set_default_int(settings, "duration_ms", 500);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
	obs_data_set_default_bool(settings, "flip_linked", true);
}

} // namespace

void register_place_item()
{
	obs_source_info info = {};
	info.id = kPlaceItemId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Place.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new PlaceItem(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<PlaceItem *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<PlaceItem *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<PlaceItem *>(data)->tick(seconds);
	};
	// Purely a controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<PlaceItem *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<PlaceItem *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<PlaceItem *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
