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
//
// Send To is the same thing as a filter on the source itself ("put this on
// the camera"): it moves that source wherever it is, in the scene on screen
// or in every scene, so there is no scene or item to choose.

#include "place-item.hpp"

#include "../common/easing.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

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
	bool every_scene = false; // Send To only
	std::string in_scene;     // Send To only: this scene instead (set by the dock)
};

// One item gliding from one place to another. Places are the top-left of
// the item's box in scene coordinates, so the same maths serves items in a
// scene and items inside a group.
struct Move {
	obs_sceneitem_t *item = nullptr; // strong reference while moving
	obs_source_t *scene = nullptr;   // strong reference while moving
	double from_x = 0, from_y = 0, to_x = 0, to_y = 0;
	vec2 from_scale, to_scale;
	bool scale = false;
};

void release_move(Move &move)
{
	if (move.item)
		obs_sceneitem_release(move.item);
	if (move.scene)
		obs_source_release(move.scene);
	move.item = nullptr;
	move.scene = nullptr;
}

// One place a source sits: the scene (or nested scene) and its item there.
struct Target {
	obs_source_t *scene;
	obs_sceneitem_t *item;
};

struct Collect {
	obs_source_t *scene;
	obs_source_t *wanted;
	std::vector<Target> *out;
	bool nested;
	int depth;
};

void collect_in(obs_source_t *scene_source, obs_source_t *wanted, std::vector<Target> &out, bool nested, int depth);

bool collect_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *c = static_cast<Collect *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source == c->wanted)
		c->out->push_back({c->scene, item});
	else if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, collect_item, param);
	else if (c->nested && c->depth < 8 && obs_source_is_scene(source))
		collect_in(source, c->wanted, *c->out, true, c->depth + 1);
	return true;
}

void collect_in(obs_source_t *scene_source, obs_source_t *wanted, std::vector<Target> &out, bool nested, int depth)
{
	obs_scene_t *scene = scene_of(scene_source);
	if (!scene)
		return;
	Collect c{scene_source, wanted, &out, nested, depth};
	obs_scene_enum_items(scene, collect_item, &c);
}

// Everywhere a source sits: in the scene on screen (looking inside nested
// scenes and groups), or in every scene.
std::vector<Target> places_of(obs_source_t *wanted, bool every_scene)
{
	std::vector<Target> out;
	if (every_scene) {
		struct All {
			obs_source_t *wanted;
			std::vector<Target> *out;
		} all{wanted, &out};
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				auto *a = static_cast<All *>(param);
				collect_in(scene, a->wanted, *a->out, false, 0);
				return true;
			},
			&all);
	} else {
		obs_source_t *scene = scene_on_screen();
		collect_in(scene, wanted, out, true, 0);
		obs_source_release(scene);
	}
	return out;
}

void apply_move(const Move &move, double t)
{
	if (!move.item)
		return;
	if (move.scale) {
		vec2 scale;
		scale.x = (float)(move.from_scale.x + (move.to_scale.x - move.from_scale.x) * t);
		scale.y = (float)(move.from_scale.y + (move.to_scale.y - move.from_scale.y) * t);
		obs_sceneitem_set_scale(move.item, &scale);
	}
	// Nudge the item from where its box is now to where it should be. Working
	// from the current box keeps this right even when OBS re-fits a group
	// around its moving contents, which shifts the group's own origin.
	const double want_x = move.from_x + (move.to_x - move.from_x) * t;
	const double want_y = move.from_y + (move.to_y - move.from_y) * t;
	const Box now = scene_box(move.scene, move.item);
	const Space space = item_space(move.scene, move.item);
	vec2 pos;
	obs_sceneitem_get_pos(move.item, &pos);
	pos.x += (float)((want_x - now.x) / space.sx);
	pos.y += (float)((want_y - now.y) / space.sy);
	obs_sceneitem_set_pos(move.item, &pos);
}

class PlaceItem {
public:
	PlaceItem(obs_data_t *settings, obs_source_t *source, bool on_source);
	~PlaceItem();

	void update(obs_data_t *settings);
	void tick(float seconds);
	void attach(obs_source_t *parent);
	void detach();
	void trigger() { pending_ = true; }
	bool capture_current();
	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }
	bool on_source() const { return on_source_; }

private:
	void start();
	void plan(const Settings &s, obs_source_t *scene, obs_sceneitem_t *item);
	void finish();
	void register_hotkey();
	void unregister_hotkey();
	void set_enabled_quietly(bool enabled);

	static void enable_cb(void *data, calldata_t *cd);
	static void rename_cb(void *data, calldata_t *cd);

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	const bool on_source_; // a Send To filter on the source it moves
	std::mutex mutex_;
	Settings settings_;
	std::atomic<bool> pending_{false};
	std::atomic<bool> ready_{false}; // ignore the enabled state restored at load

	bool moving_ = false;
	double elapsed_ = 0.0;
	double duration_ = 0.0;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	std::vector<Move> moves_;

	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
};

thread_local bool t_changing_enabled = false;

PlaceItem::PlaceItem(obs_data_t *settings, obs_source_t *source, bool on_source)
	: source_(source),
	  on_source_(on_source)
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
	for (Move &move : moves_)
		release_move(move);
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
	s.every_scene = obs_data_get_int(settings, "where") == 1;
	s.in_scene = obs_data_get_string(settings, "in_scene");

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
	for (Move &move : moves_)
		release_move(move);
	moves_.clear();
	moving_ = false;

	if (on_source_) {
		std::vector<Target> targets;
		if (obs_source_t *named = s.in_scene.empty() ? nullptr : obs_get_source_by_name(s.in_scene.c_str())) {
			collect_in(named, parent_, targets, true, 0);
			obs_source_release(named);
		} else {
			targets = places_of(parent_, s.every_scene);
		}
		for (const Target &target : targets)
			plan(s, target.scene, target.item);
	} else if (obs_sceneitem_t *item = find_item(parent_, s.item)) {
		plan(s, parent_, item);
	}
	if (moves_.empty()) {
		if (on_source_)
			obs_log(LOG_WARNING, "Send To '%s': '%s' is not in the scene on screen",
				obs_source_get_name(source_), obs_source_get_name(parent_));
		else
			obs_log(LOG_WARNING, "Place Item '%s': item '%s' is not in this scene",
				obs_source_get_name(source_), s.item.c_str());
		finish();
		return;
	}

	elapsed_ = 0.0;
	duration_ = s.duration_s;
	curve_ = s.curve;
	mode_ = s.mode;
	moving_ = true;
	if (duration_ <= 0.0) {
		for (const Move &move : moves_)
			apply_move(move, 1.0);
		finish();
	}
}

// Works out where one item (and whatever travels with it) should end up.
void PlaceItem::plan(const Settings &s, obs_source_t *scene, obs_sceneitem_t *item)
{
	Move leader_, follower_;
	const double canvas_w = obs_source_get_width(scene);
	const double canvas_h = obs_source_get_height(scene);

	// Leader.
	obs_sceneitem_addref(item);
	leader_.item = item;
	leader_.scene = obs_source_get_ref(scene);
	obs_sceneitem_get_scale(item, &leader_.from_scale);
	leader_.to_scale = leader_.from_scale;
	leader_.scale = false;
	refresh_item(item);
	// All placement maths is done in the scene's coordinates.
	const Box box = scene_box(scene, item);
	Box target = box;

	if (s.destination == Destination::Saved) {
		// A saved spot is the item's own position (and size). Put it there
		// for a moment to measure the box, then put it back.
		vec2 was_pos, saved_pos;
		obs_sceneitem_get_pos(item, &was_pos);
		vec2_set(&saved_pos, (float)s.saved_x, (float)s.saved_y);
		if (s.restore_size && s.saved_scale_x > 0.0 && s.saved_scale_y > 0.0) {
			vec2_set(&leader_.to_scale, (float)s.saved_scale_x, (float)s.saved_scale_y);
			leader_.scale = true;
			obs_sceneitem_set_scale(item, &leader_.to_scale);
		}
		obs_sceneitem_set_pos(item, &saved_pos);
		target = scene_box(scene, item);
		obs_sceneitem_set_pos(item, &was_pos);
		obs_sceneitem_set_scale(item, &leader_.from_scale);
	} else {
		const int column = static_cast<int>(s.destination) % 3;
		const int row = static_cast<int>(s.destination) / 3;
		target.x = column == 0   ? s.margin
			   : column == 1 ? (canvas_w - box.w) / 2.0
					 : canvas_w - s.margin - box.w;
		target.y = row == 0 ? s.margin : row == 1 ? (canvas_h - box.h) / 2.0 : canvas_h - s.margin - box.h;
	}
	leader_.from_x = box.x;
	leader_.from_y = box.y;
	leader_.to_x = target.x;
	leader_.to_y = target.y;

	// Linked item: keeps its place relative to the leader, mirrored to the
	// other side if that is the only way to stay on screen.
	obs_sceneitem_t *linked = !s.linked.empty() && s.linked != s.item ? find_item(scene, s.linked) : nullptr;
	if (linked) {
		obs_sceneitem_addref(linked);
		follower_.item = linked;
		follower_.scale = false;
		follower_.scene = obs_source_get_ref(scene);
		refresh_item(linked);
		const Box fbox = scene_box(scene, linked);
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
		follower_.from_x = fbox.x;
		follower_.from_y = fbox.y;
		follower_.to_x = tx;
		follower_.to_y = ty;
	}
	moves_.push_back(leader_);
	if (follower_.item)
		moves_.push_back(follower_);
}

void PlaceItem::finish()
{
	moving_ = false;
	for (Move &move : moves_)
		release_move(move);
	moves_.clear();
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
	for (const Move &move : moves_)
		apply_move(move, t);
	if (elapsed_ >= duration_) {
		for (const Move &move : moves_)
			apply_move(move, 1.0);
		finish();
	}
}

// Stores the item's current position and size as the saved spot.
bool PlaceItem::capture_current()
{
	obs_data_t *settings = obs_source_get_settings(source_);
	obs_sceneitem_t *item = nullptr;
	if (on_source_) {
		// Where it is in the scene on screen; failing that, anywhere.
		std::vector<Target> places = places_of(parent_, false);
		if (places.empty())
			places = places_of(parent_, true);
		if (!places.empty())
			item = places.front().item;
	} else {
		item = find_item(parent_, obs_data_get_string(settings, "item"));
	}
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

	const bool on_source = self && self->on_source();
	obs_source_t *parent = self ? self->parent() : nullptr;
	if (!on_source) {
		obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("Place.Item"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		add_scene_items_to_list(parent, item);
	}
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

	if (on_source) {
		obs_property_t *where = obs_properties_add_list(props, "where", obs_module_text("SendTo.Where"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(where, obs_module_text("SendTo.Where.OnScreen"), 0);
		obs_property_list_add_int(where, obs_module_text("SendTo.Where.Every"), 1);
	}

	obs_property_t *linked = obs_properties_add_list(props, "linked", obs_module_text("Place.Linked"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(linked, obs_module_text("Place.Linked.None"), "");
	if (on_source) {
		// Offer what shares a scene with this source right now.
		for (const Target &target : places_of(parent, false))
			add_scene_items_to_list(target.scene, linked);
	} else {
		add_scene_items_to_list(parent, linked);
	}
	obs_property_set_modified_callback(linked, destination_modified);
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

	obs_property_t *help = obs_properties_add_text(
		props, "help", obs_module_text(on_source ? "SendTo.Help" : "Place.Help"), OBS_TEXT_INFO);
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
	obs_data_set_default_int(settings, "where", 0);
}

} // namespace

void register_place_item()
{
	obs_source_info info = {};
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
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

	// On a scene: moves an item chosen from that scene.
	info.id = kPlaceItemId;
	info.get_name = [](void *) {
		return obs_module_text("Place.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new PlaceItem(settings, source, false);
	};
	obs_register_source(&info);

	// On a source: moves that source.
	info.id = kSendToId;
	info.get_name = [](void *) {
		return obs_module_text("SendTo.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new PlaceItem(settings, source, true);
	};
	obs_register_source(&info);
}

} // namespace stagekit
