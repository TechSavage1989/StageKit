/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Standby Picture: a picture in a capture's place while the capture has
// nothing to show, so its spot in the scene is not an empty hole.
//
// "Nothing to show" means the capture has no size: no game or window is
// attached to it. A capture that is attached and showing a black picture is
// something, and is left alone.
//
// The picture is the start image unless another is chosen. While it waits it
// keeps its own shape, as tall as the capture last was. When the capture
// arrives the size glides to the capture's while the picture fades into it;
// when the capture goes, the size glides back to the picture's shape. Nothing
// is ever squashed: both are drawn to cover the size of the moment.

#include "standby.hpp"
#include "app-capture.hpp"
#include "../common/timing.hpp"

#include "../common/easing.hpp"
#include "../common/filter-order.hpp"
#include "../common/holds-hidden.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/stand-in.hpp"
#include "../common/start-image.hpp"
#include "../frame/auto-frame.hpp"
#include "../placement/scene-standby.hpp"
#include "../transition/morph-paint.hpp"
#include "../transition/morph.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

// How the picture gives way to the capture, and comes back. Saved: append only.
enum class Style : int {
	Fade = 0,
	Cut = 1,
	SlideLeft = 2,
	SlideRight = 3,
	SlideUp = 4,
	SlideDown = 5,
	WipeLeft = 6,
	WipeRight = 7,
	WipeUp = 8,
	WipeDown = 9,
	Zoom = 10,
	Morph = 11, // both pictures stretched over the frame as it glides
};

enum class Picture : int {
	StartImage = 0, // saved: append only
	File = 1,
	Source = 2,
};

bool is_video_file(const std::string &path)
{
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = path.substr(dot + 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	for (const char *video : {"mp4", "mov", "mkv", "webm", "avi", "m4v", "flv", "ts", "gif"})
		if (ext == video)
			return true;
	return false;
}

// The size of what comes before a filter. For the capture itself that is its
// own size: asking for its width would come back to this filter.
uint32_t before_width(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	return target == parent ? obs_source_get_base_width(target) : obs_source_get_width(target);
}
uint32_t before_height(obs_source_t *filter)
{
	obs_source_t *target = obs_filter_get_target(filter), *parent = obs_filter_get_parent(filter);
	if (!target)
		return 0;
	return target == parent ? obs_source_get_base_height(target) : obs_source_get_height(target);
}

// A source's place taken while it is hidden. The owner: "I need a way for the
// standby image to be displayed in place of any source I choose when it's
// hidden". A hidden scene item draws nothing, so each item showing the source
// gets a second item straight under it, in the same place and of the same
// size, that shows only while the first is hidden. That second item's source
// (a stand-in, private) draws the source itself with this flag up, which
// makes the source's Standby filter draw its picture and nothing else: the
// picture gets the source's size and every filter after this one, its shadow
// and corners included.
thread_local bool t_stand_in = false;
// The stand-in's kind and the key naming its item's item (kStandInId,
// kStandInFor) are in stand-in.hpp, for the scene filters.

// The sources a Standby filter stands in for while they are hidden, scene by
// scene: their items' transitions in that scene are that filter's to set
// (see `item_transitions_owner`). In a scene where it makes no stand-in the
// item is free for Show in Scenes or a Scene Transitions to set. Pointers are
// keys only, never used.
std::mutex g_stood_in_mutex;
std::set<std::pair<obs_source_t * /*scene*/, obs_source_t * /*source*/>> g_stood_in;

void stood_in_forget(obs_source_t *source)
{
	std::lock_guard lock(g_stood_in_mutex);
	for (auto it = g_stood_in.begin(); it != g_stood_in.end();)
		it = it->second == source ? g_stood_in.erase(it) : std::next(it);
}

// One way an item comes and goes, as text, to see whether it has changed.
std::string change_text(const ItemChange &change)
{
	std::string text;
	for (const ItemWay *way : {&change.show, &change.hide})
		text += way->kind + "|" + std::to_string(way->ms) + "|" + way->own + "|";
	return text;
}

class Standby {
public:
	Standby(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		// For the filters around it: is the picture standing in, or still
		// changing to or from the capture?
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void get_standing(out bool standing, out bool waiting)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<Standby *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_bool(cd, "standing",
						  t_stand_in || self->empty_ || self->mix_ > 0.0 || self->glide_ < 1.0);
				calldata_set_bool(cd, "waiting", self->empty_);
			},
			this);
		// For the tests: how many times the image has been made, and
		// whether the filter is idle (made by a Scene Standby that no longer
		// covers it).
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void test_state(out int made, out bool idle)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<Standby *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_int(cd, "made", self->made_);
				calldata_set_bool(cd, "idle", self->idle_);
			},
			this);
		// Shown on purpose, over a capture that has a picture: by its
		// hotkey, the button, or a window switcher asked to hide the capture.
		proc_handler_add(
			obs_source_get_proc_handler(source_),
			"void cover(in bool toggle, in bool on, out bool covering)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<Standby *>(data);
				// Another filter may lend its own transition for this
				// change in and back out ("style" 0 and up); without
				// one, the filter's own is used.
				long long style = -1;
				if (calldata_get_int(cd, "style", &style) && style >= 0) {
					std::lock_guard lock(self->mutex_);
					Look lent;
					lent.style = static_cast<Style>(std::clamp<long long>(style, 0, 11));
					lent.fade_s = lent.back_s =
						std::clamp<long long>(calldata_int(cd, "ms"), 0, 10000) / 1000.0;
					lent.curve = static_cast<EaseCurve>(calldata_int(cd, "curve"));
					lent.mode = static_cast<EaseMode>(calldata_int(cd, "mode"));
					self->lent_ = true;
					self->use(lent);
				}
				if (calldata_bool(cd, "toggle"))
					self->cover_ = !self->cover_;
				else
					self->cover_ = calldata_bool(cd, "on");
				calldata_set_bool(cd, "covering", self->cover_);
			},
			this);
	}
	void toggle_cover() { cover_ = !cover_; }

	// How the picture comes and goes: the filter's own settings, or ones
	// lent by another filter for the change it asked for.
	struct Look {
		double fade_s = 0.8, back_s = 0.8;
		Style style = Style::Fade;
		EaseCurve curve = EaseCurve::Cubic;
		EaseMode mode = EaseMode::InOut;
	};
	void use(const Look &look) // (with mutex_ held)
	{
		fade_s_ = look.fade_s;
		back_s_ = look.back_s;
		style_ = look.style;
		curve_ = look.curve;
		mode_ = look.mode;
	}

	~Standby()
	{
		stood_in_forget(obs_filter_get_parent(source_));
		forget_pairs();
		// Its stand-in items go with it.
		if (stand_in_) {
			std::vector<obs_sceneitem_t *> twins;
			struct Search {
				std::vector<obs_sceneitem_t *> *found;
				obs_source_t *stand_in;
			} search{&twins, stand_in_};
			obs_enum_scenes(
				[](void *param, obs_source_t *source) {
					if (obs_source_is_group(source))
						return true;
					obs_scene_enum_items(
						obs_scene_from_source(source),
						[](obs_scene_t *, obs_sceneitem_t *item, void *inner) {
							auto *s = static_cast<Search *>(inner);
							if (obs_sceneitem_get_source(item) == s->stand_in) {
								obs_sceneitem_addref(item);
								s->found->push_back(item);
							}
							return true;
						},
						param);
					return true;
				},
				&search);
			for (obs_sceneitem_t *twin : twins) {
				obs_sceneitem_remove(twin);
				obs_sceneitem_release(twin);
			}
			obs_source_release(stand_in_);
		}
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		let_go();
		obs_enter_graphics();
		gs_texrender_destroy(render_);
		gs_texrender_destroy(render_live_);
		gs_texrender_destroy(render_stand_in_);
		gs_effect_destroy(effect_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		const auto kind = static_cast<Picture>(std::clamp((int)obs_data_get_int(settings, "picture"), 0, 2));
		const std::string file = obs_data_get_string(settings, "file");
		const std::string named = obs_data_get_string(settings, "source");
		// The image is made again only when what it shows changes: a Scene
		// Standby pushing a new duration must not make it flash.
		if (kind != kind_ || file != file_ || named != named_)
			changed_ = true;
		kind_ = kind;
		file_ = file;
		named_ = named;
		managed_ = obs_data_get_bool(settings, "managed");
		managed_by_ = obs_data_get_string(settings, "managed_by");
		own_.fade_s = std::clamp<long long>(obs_data_get_int(settings, "fade_ms"), 0, 5000) / 1000.0;
		own_.back_s = std::clamp<long long>(obs_data_get_int(settings, "back_ms"), 0, 5000) / 1000.0;
		own_.style = static_cast<Style>(std::clamp((int)obs_data_get_int(settings, "style"), 0, 11));
		own_.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		own_.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		if (!lent_)
			use(own_);
		glide_size_ = obs_data_get_bool(settings, "glide_size");
		last_w_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "last_width"));
		last_h_ = (uint32_t)std::max<long long>(0, obs_data_get_int(settings, "last_height"));
		placed_ = obs_data_get_bool(settings, "placed");
		own_shape_ = obs_data_get_bool(settings, "own_shape");
		when_hidden_ = obs_data_get_bool(settings, "when_hidden");
		hidden_change_ = item_change_read(settings);
		hidden_changed_ = true;
	}

	void tick(float seconds)
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		obs_source_t *target = obs_filter_get_target(source_);
		if (parent && hotkey_ == OBS_INVALID_HOTKEY_ID) {
			const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
			std::string text = obs_module_text("Hotkey.Standby.Cover");
			const size_t at = text.find("%1");
			if (at != std::string::npos)
				text.replace(at, 2, name);
			hotkey_ = obs_hotkey_register_source(
				parent, ("stagekit.standby.cover." + name).c_str(), text.c_str(),
				[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
					if (pressed)
						static_cast<Standby *>(data)->toggle_cover();
				},
				this);
		}
		if (!parent || !target)
			return;
		// The stand-ins for the source while it is hidden, looked at four
		// times a second.
		stand_in_check_ += seconds;
		if (stand_in_check_ >= 0.25) {
			stand_in_check_ = 0.0;
			// (Not while OBS loads its scenes: see loading.hpp.)
			if (scenes_loaded()) {
				check_managed(parent, 0.25);
				sync_stand_ins(parent);
			}
		}
		// Every frame, for the pairs known: the stand-in shows the moment its
		// item is hidden and goes the moment it is shown, so the two changes
		// are one (and a Morph finds them together).
		for (const Held &pair : pairs_) {
			obs_source_t *scene_source = obs_weak_source_get_source(pair.scene);
			obs_scene_t *scene = obs_scene_from_source(scene_source);
			obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, pair.item) : nullptr;
			obs_sceneitem_t *twin = scene ? obs_scene_find_sceneitem_by_id(scene, pair.twin) : nullptr;
			if (item && twin) {
				const bool want = stands_in(scene_source, item);
				if (obs_sceneitem_visible(twin) != want)
					obs_sceneitem_set_visible(twin, want);
			}
			obs_source_release(scene_source);
		}
		Picture kind = Picture();
		std::string file, named;
		bool changed, placed;
		double fade_s;
		{
			std::lock_guard lock(mutex_);
			changed = changed_;
			changed_ = false;
			// (Only when the image is made again: they are strings.)
			if (changed) {
				kind = kind_;
				file = file_;
				named = named_;
			}
			placed = placed_;
			fade_s = fade_s_;
		}
		// Its place among the filters, the first time only: straight after an
		// Auto Frame, so the picture fills the frame, and otherwise first, so
		// corners, shadows and the rest are put on the picture too.
		if (!placed) {
			obs_source_t *frame = first_filter_of_kind(parent, kAutoFrameId);
			for (int i = 0; i < 32; i++) {
				const int mine = applied_index(parent, source_);
				const int wanted = frame ? applied_index(parent, frame) + 1 : 0;
				if (mine < 0 || mine <= wanted)
					break;
				obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_UP);
				if (applied_index(parent, source_) == mine)
					break;
			}
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_bool(settings, "placed", true);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			placed_ = true;
		}
		// An Auto Frame must come before this, always: put after it, an Auto
		// Frame takes the standby picture for a game and trims it to the
		// last game's shape. Looked at twice a second.
		order_check_ += seconds;
		if (order_check_ >= 0.5) {
			order_check_ = 0.0;
			if (obs_source_t *frame = first_filter_of_kind(parent, kAutoFrameId)) {
				for (int i = 0; i < 32; i++) {
					const int mine = applied_index(parent, source_);
					if (mine < 0 || mine > applied_index(parent, frame))
						break;
					obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_DOWN);
					if (applied_index(parent, source_) == mine)
						break;
				}
			}
		}
		if (changed) {
			make_picture(kind, file, named);
			std::lock_guard lock(mutex_);
			made_++;
		}
		bool idle;
		{
			std::lock_guard lock(mutex_);
			idle = idle_;
		}

		// Nothing to show: the capture itself has no size.
		// (Not while a Capture App in Use is changing from one window to
		// another: it shows the last picture for that moment.)
		// (Or the picture has been asked for over the capture.)
		// (Nor while a Scene Standby that made this filter no longer covers
		// the source: then it is idle.)
		const bool empty =
			cover_ ||
			(!idle && (obs_source_get_base_width(parent) == 0 || obs_source_get_base_height(parent) == 0) &&
			 !app_capture_holding(parent));
		const uint32_t tw = before_width(source_), th = before_height(source_);
		// A window switcher before this glides the capture's size from one
		// window to the next by itself. Gliding after it again, over this
		// filter's own time, the size arrived long after the switch: the
		// owner's "fit still trails the source window switch" (the switcher
		// at 250 ms, this at 800). While the capture is live its size is
		// taken as it is.
		// (Asked only when it can matter: it goes through the filters.)
		const bool led = !empty && tw && th && app_capture_present(parent);
		if (!empty && tw && th && (tw != last_w_ || th != last_h_)) {
			// Remembered, for the next time there is nothing.
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_int(settings, "last_width", tw);
			obs_data_set_int(settings, "last_height", th);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			last_w_ = tw;
			last_h_ = th;
		}
		// The size to be: the capture's while it shows; otherwise the
		// picture's own shape, as tall as the capture last was, or (asked to
		// fill) the size the capture last had.
		const uint32_t pw = picture_ ? obs_source_get_width(picture_) : 0;
		const uint32_t ph = picture_ ? obs_source_get_height(picture_) : 0;
		double want_w, want_h;
		{
			std::lock_guard lock(mutex_);
			const double ref_w = last_w_ ? last_w_ : 1920.0, ref_h = last_h_ ? last_h_ : 1080.0;
			if (!empty && tw && th) {
				want_w = tw;
				want_h = th;
			} else if (own_shape_ && pw && ph) {
				want_h = ref_h;
				want_w = ref_h * (double)pw / (double)ph;
			} else {
				want_w = ref_w;
				want_h = ref_h;
			}
			// It glides from the size it has to the size to be, however that
			// changes and whatever changes it: a game starting or ending, or
			// a filter before this one settling on another shape. Towards
			// the capture it takes the first time; back to the picture, the
			// second.
			const double over = empty ? back_s_ : fade_s_;
			const bool cut = style_ == Style::Cut || over <= 0.0;
			// At the very start the picture may not have loaded yet: wait
			// for it rather than begin in the last capture's shape.
			const bool unknown = empty && own_shape_ && picture_ && !(pw && ph) &&
					     (waited_ += seconds) < 2.0;
			if (unknown) {
				// No size yet.
			} else if (cur_w_ <= 0.0 || cur_h_ <= 0.0) {
				cur_w_ = from_w_ = to_w_ = want_w;
				cur_h_ = from_h_ = to_h_ = want_h;
				glide_ = 1.0;
			} else if (led && !empty && mix_ <= 0.0 && tw && th) {
				cur_w_ = from_w_ = to_w_ = want_w;
				cur_h_ = from_h_ = to_h_ = want_h;
				glide_ = 1.0;
			} else if (std::fabs(want_w - to_w_) > 0.5 || std::fabs(want_h - to_h_) > 0.5) {
				from_w_ = cur_w_;
				from_h_ = cur_h_;
				to_w_ = want_w;
				to_h_ = want_h;
				glide_ = 0.0;
			}
			if (glide_ < 1.0) {
				glide_ = cut || !glide_size_ ? 1.0 : std::min(1.0, glide_ + seconds / over);
				const double t = std::clamp(ease(curve_, mode_, glide_), 0.0, 1.0);
				cur_w_ = from_w_ + (to_w_ - from_w_) * t;
				cur_h_ = from_h_ + (to_h_ - from_h_) * t;
			}
			// How much of the picture shows: all of it with nothing captured,
			// none with a capture, changing over the same times.
			const double step = cut ? 1.0 : seconds / over;
			mix_ = empty ? std::min(1.0, mix_ + step) : std::max(0.0, mix_ - step);
			// A lent transition lasts until the picture is all gone again.
			if (lent_ && !cover_ && mix_ <= 0.0) {
				lent_ = false;
				use(own_);
			}
			// (Shown over a capture that is still running, the capture goes
			// on being drawn under the picture until the picture is all
			// there: the change is from a live picture, not a still one.)
			const bool under = empty && cover_ && mix_ < 1.0 && tw && th &&
					   obs_source_get_base_width(parent) != 0;
			if (empty != empty_)
				obs_log(LOG_INFO, "Standby Picture '%s': %s, over %.0f ms (transition %d)",
					obs_source_get_name(source_),
					empty ? (cover_ ? "shown over the capture" : "the capture has gone")
					      : "back to the capture",
					cut ? 0.0 : over * 1000.0, (int)style_);
			empty_ = empty;
			live_w_ = empty && !under ? 0 : tw;
			live_h_ = empty && !under ? 0 : th;
		}
		double mix;
		{
			std::lock_guard lock(mutex_);
			mix = mix_;
		}
		// The picture only runs while it is wanted.
		const bool wanted = mix > 0.0 && obs_source_showing(parent);
		if (wanted != holding_ && picture_) {
			if (wanted) {
				obs_source_inc_showing(picture_);
				obs_source_inc_active(picture_);
			} else {
				obs_source_dec_active(picture_);
				obs_source_dec_showing(picture_);
			}
			holding_ = wanted;
		}
	}

	uint32_t width() { return size(true); }
	uint32_t height() { return size(false); }

	void render()
	{
		double mix;
		bool empty;
		uint32_t live_w, live_h;
		{
			std::lock_guard lock(mutex_);
			mix = mix_;
			empty = empty_;
			live_w = live_w_;
			live_h = live_h_;
		}
		bool idle;
		{
			std::lock_guard lock(mutex_);
			idle = idle_ && mix <= 0.0;
		}
		// Idle with nothing left to show: as if it were not there.
		if (idle && !t_stand_in) {
			obs_source_skip_video_filter(source_);
			return;
		}
		const uint32_t w = width(), h = height();
		if (!effect_) {
			char *path = obs_module_file("effects/stagekit-fade.effect");
			char *errors = nullptr;
			effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
			if (!effect_)
				obs_log(LOG_ERROR, "Standby Picture: could not load its effect: %s",
					errors ? errors : "file not found");
			bfree(errors);
			bfree(path);
			if (!effect_) {
				if (live_w && live_h && !t_stand_in)
					obs_source_skip_video_filter(source_);
				return;
			}
		}
		// As a stand-in for the source while it is hidden: the picture alone,
		// all of it, whatever the capture is doing.
		if (t_stand_in) {
			obs_source_t *only = picture_;
			const uint32_t ow = only ? obs_source_get_width(only) : 0;
			const uint32_t oh = only ? obs_source_get_height(only) : 0;
			if (!w || !h || !ow || !oh || !effect_)
				return;
			if (!render_stand_in_)
				render_stand_in_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (gs_texture_t *texture = covered(render_stand_in_, w, h, ow, oh, only))
				draw(texture, w, h, 1.0f);
			return;
		}
		const bool live = live_w && live_h;
		bool morph;
		{
			std::lock_guard lock(mutex_);
			morph = style_ == Style::Morph;
		}
		if (!w || !h) {
			if (live)
				obs_source_skip_video_filter(source_);
			return;
		}
		// The capture is always drawn by way of a copy, which is kept: when
		// the capture ends there is nothing left to draw, and the change
		// back to the picture starts from that last frame instead of from
		// nothing.
		// While the size glides, both the capture and the picture are drawn
		// to fill the size of the moment: as large as needed to cover it,
		// centred, what does not fit cut off. Neither is ever squashed.
		if (live) {
			if (!render_live_)
				render_live_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (gs_texture_t *texture = covered(render_live_, w, h, live_w, live_h, nullptr, morph)) {
				draw(texture, w, h, 1.0f);
				kept_w_ = w;
				kept_h_ = h;
			} else {
				kept_w_ = kept_h_ = 0;
			}
		} else if (mix < 1.0 && render_live_ && kept_w_ && kept_h_) {
			if (gs_texture_t *texture = gs_texrender_get_texture(render_live_))
				draw_kept(texture, w, h, morph);
		} else {
			kept_w_ = kept_h_ = 0;
		}
		obs_source_t *picture = picture_;
		const uint32_t pw = picture ? obs_source_get_width(picture) : 0;
		const uint32_t ph = picture ? obs_source_get_height(picture) : 0;
		if (mix > 0.0 && pw && ph) {
			if (!render_)
				render_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (gs_texture_t *texture = covered(render_, w, h, pw, ph, picture, morph))
				draw_styled(texture, w, h, mix);
		}
	}

	bool showing()
	{
		std::lock_guard lock(mutex_);
		return empty_;
	}

	// Made by a Scene Standby: which scene sets it, and whether none covers
	// it any more. False for a filter made by hand.
	bool managed(std::string &by, bool &orphan)
	{
		std::lock_guard lock(mutex_);
		by = owner_scene_.empty() ? managed_by_ : owner_scene_;
		orphan = orphan_;
		return managed_;
	}

	// For a filter a Scene Standby made: is it still covered by one? Not
	// covered for two seconds, it goes idle (no image when the capture is
	// empty) and says so; it never takes itself off.
	void check_managed(obs_source_t *parent, double seconds)
	{
		bool managed;
		std::string by;
		{
			std::lock_guard lock(mutex_);
			managed = managed_;
			by = managed_by_;
		}
		const ManagedState state = managed ? scene_standby_state(parent, by.c_str()) : ManagedState();
		std::lock_guard lock(mutex_);
		uncovered_s_ = !managed || state.covered ? 0.0 : uncovered_s_ + seconds;
		const bool idle = managed && uncovered_s_ >= 2.0;
		if (idle != idle_)
			obs_log(LOG_INFO, "Standby Picture '%s' on '%s': %s", obs_source_get_name(source_),
				obs_source_get_name(parent),
				idle ? "no Scene Standby covers it now, idle" : "covered by a Scene Standby again");
		idle_ = idle;
		orphan_ = managed && !state.listed;
		owner_scene_ = state.owner_found ? state.owner_scene : by;
	}

	// Every scene item showing the source has its stand-in: made when
	// missing, kept in the item's place and straight under it while the item
	// shows, and shown exactly while the item is hidden. With the option
	// off, or the filter, they are taken out again. Called from the tick.
	// The option is the filter's own "Show when source is hidden" (every
	// scene), or a Scene Standby listing the source under "Show when hidden"
	// (its own scene only, with its own transition).
	void sync_stand_ins(obs_source_t *parent)
	{
		bool own_wanted;
		ItemChange own_change;
		bool changed;
		{
			std::lock_guard lock(mutex_);
			own_wanted = when_hidden_;
			own_change = hidden_change_;
			changed = hidden_changed_;
			hidden_changed_ = false;
		}
		const bool enabled = obs_source_enabled(source_);
		own_wanted = own_wanted && enabled;
		const bool wanted = own_wanted || (enabled && scene_standby_any_hidden(parent));
		stood_in_forget(parent);
		forget_pairs();
		if (!wanted && !stand_in_) {
			applied_.clear();
			return;
		}
		if (wanted && !stand_in_) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "of", obs_source_get_name(parent));
			const std::string name = std::string(obs_source_get_name(parent)) + " " +
						 obs_module_text("Standby.StandIn.Suffix");
			stand_in_ = obs_source_create_private(kStandInId, name.c_str(), settings);
			obs_data_release(settings);
			if (!stand_in_)
				return;
		}
		// The items of both, scene by scene, each held. (Scenes only: OBS
		// lists every group as a scene as well, and an item in a group is
		// left out of this for now.)
		struct Pair {
			obs_scene_t *scene;
			obs_sceneitem_t *item;
			bool stand_in;
		};
		struct Search {
			std::vector<Pair> *found;
			obs_source_t *parent, *stand_in;
			obs_scene_t *scene;
		};
		std::vector<Pair> found;
		Search search{&found, parent, stand_in_, nullptr};
		obs_enum_scenes(
			[](void *param, obs_source_t *source) {
				auto *s = static_cast<Search *>(param);
				if (obs_source_is_group(source))
					return true;
				s->scene = obs_scene_from_source(source);
				obs_scene_enum_items(
					s->scene,
					[](obs_scene_t *, obs_sceneitem_t *item, void *inner) {
						auto *s2 = static_cast<Search *>(inner);
						obs_source_t *shown = obs_sceneitem_get_source(item);
						if (shown == s2->parent || shown == s2->stand_in) {
							obs_sceneitem_addref(item);
							s2->found->push_back(
								Pair{s2->scene, item, shown == s2->stand_in});
						}
						return true;
					},
					s);
				return true;
			},
			&search);
		// Which scenes have stand-ins, and with which change: a Scene
		// Standby's own for its scene, or else this filter's.
		std::map<obs_scene_t *, ItemChange> scenes;
		for (const Pair &real : found) {
			if (real.stand_in || !wanted || scenes.count(real.scene))
				continue;
			ItemChange change = own_change;
			if (scene_standby_hidden(obs_scene_get_source(real.scene), parent, &change) || own_wanted)
				scenes[real.scene] = change;
		}
		{
			std::lock_guard lock(g_stood_in_mutex);
			for (const auto &[scene, change] : scenes)
				g_stood_in.insert({obs_scene_get_source(scene), parent});
		}
		std::map<obs_scene_t *, std::string> applied;
		for (const Pair &real : found) {
			if (real.stand_in || !scenes.count(real.scene))
				continue;
			ItemChange change = scenes[real.scene];
			// In the scene's terms, before it is swapped for the twin.
			change = resolve_item_change(real.item, change, obs_scene_get_source(real.scene));
			const std::string text = change_text(change);
			applied[real.scene] = text;
			auto before = applied_.find(real.scene);
			const bool scene_changed = changed || before == applied_.end() || before->second != text;
			obs_sceneitem_t *twin = nullptr;
			for (const Pair &other : found)
				if (other.stand_in && other.scene == real.scene &&
				    stand_in_for_item(other.item) == obs_sceneitem_get_id(real.item))
					twin = other.item;
			const bool shown = obs_sceneitem_visible(real.item);
			if (!twin) {
				twin = obs_scene_add(real.scene, stand_in_);
				if (!twin)
					continue;
				obs_data_t *own = obs_sceneitem_get_private_settings(twin);
				obs_data_set_int(own, kStandInFor, obs_sceneitem_get_id(real.item));
				obs_data_release(own);
				obs_sceneitem_set_locked(twin, true);
				// (As it should be from the first moment, without a
				// transition: it is not something that just appeared.)
				obs_sceneitem_set_visible(twin, stands_in(obs_scene_get_source(real.scene), real.item));
				place_like(twin, real.item);
			}
			// Where the item is, while that can be seen; a hidden item is
			// not moved about, and its stand-in stays.
			if (shown)
				place_like(twin, real.item);
			const int at = obs_sceneitem_get_order_position(real.item);
			const int twin_at = obs_sceneitem_get_order_position(twin);
			if (twin_at != at - 1)
				obs_sceneitem_set_order_position(twin, twin_at < at ? at - 1 : at);
			// The one change chosen is the item's way out and in and its
			// stand-in's: both get it, ahead of time, so whatever hides the
			// item does it this way. With Morph the two are linked, so the
			// one changes into the other.
			ItemChange linked = change;
			for (ItemWay *way : {&linked.show, &linked.hide}) {
				if (way->kind != kMorphId)
					continue;
				obs_data_t *own = way->own.empty() ? obs_data_create()
								   : obs_data_create_from_json(way->own.c_str());
				if (!own)
					own = obs_data_create();
				obs_data_array_t *links = obs_data_get_array(own, "links");
				if (!links)
					links = obs_data_array_create();
				obs_data_t *link = obs_data_create();
				const std::string line = std::string(obs_source_get_name(parent)) + kLinkMark +
							 obs_source_get_name(stand_in_);
				obs_data_set_string(link, "value", line.c_str());
				obs_data_array_push_back(links, link);
				obs_data_release(link);
				obs_data_set_array(own, "links", links);
				obs_data_array_release(links);
				way->own = obs_data_get_json(own);
				obs_data_release(own);
			}
			// The stand-in is shown as the item is hidden: it comes the way the
			// item goes, and goes the way the item comes.
			ItemChange swapped;
			swapped.show = linked.hide;
			swapped.hide = linked.show;
			for (obs_sceneitem_t *each : {real.item, twin}) {
				const ItemChange &its = each == twin ? swapped : linked;
				bool fits = !scene_changed;
				for (bool show : {true, false}) {
					const ItemWay &way = show ? its.show : its.hide;
					if (way.kind == kTransitionKeep || way.kind.empty())
						continue;
					obs_source_t *now = obs_sceneitem_get_transition(each, show);
					const char *id = now ? obs_source_get_unversioned_id(now) : nullptr;
					if (way.kind == kTransitionCut) {
						if (now)
							fits = false;
					} else if (!id || way.kind != id ||
						   obs_sceneitem_get_transition_duration(each, show) != way.ms) {
						fits = false;
					}
				}
				if (!fits)
					apply_item_change_as_owner(each, its, obs_scene_get_source(real.scene));
			}
			const bool want = stands_in(obs_scene_get_source(real.scene), real.item);
			if (obs_sceneitem_visible(twin) != want)
				obs_sceneitem_set_visible(twin, want);
			pairs_.push_back({obs_source_get_weak_source(obs_scene_get_source(real.scene)),
					  obs_sceneitem_get_id(real.item), obs_sceneitem_get_id(twin)});
		}
		applied_ = std::move(applied);
		// A stand-in whose item has gone, or in a scene that no longer has
		// stand-ins, or all of them when they are no longer wanted, is taken
		// out.
		for (const Pair &twin : found) {
			if (!twin.stand_in)
				continue;
			bool has_item = false;
			for (const Pair &real : found)
				has_item = has_item ||
					   (!real.stand_in && real.scene == twin.scene &&
					    obs_sceneitem_get_id(real.item) == stand_in_for_item(twin.item));
			if (!wanted || !has_item || !scenes.count(twin.scene))
				obs_sceneitem_remove(twin.item);
		}
		for (const Pair &pair : found)
			obs_sceneitem_release(pair.item);
		if (!wanted) {
			forget_pairs();
			obs_source_release(stand_in_);
			stand_in_ = nullptr;
		}
	}

	// The same place, size, turn, crop and bounding box as another item.
	static void place_like(obs_sceneitem_t *item, obs_sceneitem_t *like)
	{
		vec2 pos, scale, bounds;
		obs_sceneitem_get_pos(like, &pos);
		obs_sceneitem_get_scale(like, &scale);
		obs_sceneitem_get_bounds(like, &bounds);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(like, &crop);
		vec2 now_pos, now_scale, now_bounds;
		obs_sceneitem_get_pos(item, &now_pos);
		obs_sceneitem_get_scale(item, &now_scale);
		obs_sceneitem_get_bounds(item, &now_bounds);
		obs_sceneitem_crop now_crop;
		obs_sceneitem_get_crop(item, &now_crop);
		// (Only when something differs: setting a transform marks the scene
		// as changed each time.)
		if (vec2_close(&pos, &now_pos, 0.001f) && vec2_close(&scale, &now_scale, 0.00001f) &&
		    vec2_close(&bounds, &now_bounds, 0.001f) && memcmp(&crop, &now_crop, sizeof(crop)) == 0 &&
		    obs_sceneitem_get_rot(item) == obs_sceneitem_get_rot(like) &&
		    obs_sceneitem_get_alignment(item) == obs_sceneitem_get_alignment(like) &&
		    obs_sceneitem_get_bounds_type(item) == obs_sceneitem_get_bounds_type(like) &&
		    obs_sceneitem_get_bounds_alignment(item) == obs_sceneitem_get_bounds_alignment(like))
			return;
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_set_scale(item, &scale);
		obs_sceneitem_set_rot(item, obs_sceneitem_get_rot(like));
		obs_sceneitem_set_alignment(item, obs_sceneitem_get_alignment(like));
		obs_sceneitem_set_bounds_type(item, obs_sceneitem_get_bounds_type(like));
		obs_sceneitem_set_bounds_alignment(item, obs_sceneitem_get_bounds_alignment(like));
		obs_sceneitem_set_bounds(item, &bounds);
		obs_sceneitem_set_crop(item, &crop);
		obs_sceneitem_set_scale_filter(item, obs_sceneitem_get_scale_filter(like));
		obs_sceneitem_defer_update_end(item);
	}
	obs_source_t *source() const { return source_; }

private:
	// The size the source has through this filter: the one it is gliding
	// through, in whole pixels.
	uint32_t size(bool across)
	{
		bool idle;
		{
			std::lock_guard lock(mutex_);
			idle = idle_ && mix_ <= 0.0;
			const double now = across ? cur_w_ : cur_h_;
			if (!idle && now >= 1.0)
				return (uint32_t)std::lround(now);
			if (!idle && last_w_ && last_h_)
				return across ? last_w_ : last_h_;
		}
		// Idle with nothing left to show: the size of what comes before it.
		if (idle)
			return across ? before_width(source_) : before_height(source_);
		return across ? 1920 : 1080;
	}

	// Draws a source (or, with none given, what comes before this filter)
	// into `into` at w x h, covering it. Returns the texture.
	gs_texture_t *covered(gs_texrender_t *into, uint32_t w, uint32_t h, uint32_t sw, uint32_t sh,
			      obs_source_t *source, bool stretch = false)
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
			const float scale = std::max((float)w / (float)sw, (float)h / (float)sh);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_matrix_push();
			if (stretch) {
				// Morph: the whole picture, stretched to the frame.
				gs_matrix_scale3f((float)w / (float)sw, (float)h / (float)sh, 1.0f);
			} else {
				gs_matrix_translate3f(((float)w - (float)sw * scale) / 2.0f,
						      ((float)h - (float)sh * scale) / 2.0f, 0.0f);
				gs_matrix_scale3f(scale, scale, 1.0f);
			}
			if (source)
				obs_source_video_render(source);
			else
				obs_source_skip_video_filter(source_);
			gs_matrix_pop();
			gs_blend_state_pop();
			gs_texrender_end(into);
			drawn = true;
		}
		gs_enable_framebuffer_srgb(was_srgb);
		gs_set_linear_srgb(was_linear);
		return drawn ? gs_texrender_get_texture(into) : nullptr;
	}

	// Draws the picture as far gone as `shown` says (1 all there, 0 gone), in
	// the style chosen. Whatever leaves stays inside the size: a slide or a
	// zoom draws only the part of the picture still in view.
	void draw_styled(gs_texture_t *texture, uint32_t w, uint32_t h, double shown)
	{
		Style style;
		EaseCurve curve;
		EaseMode mode;
		{
			std::lock_guard lock(mutex_);
			style = style_;
			curve = curve_;
			mode = mode_;
		}
		const double v = std::clamp(ease(curve, mode, std::clamp(shown, 0.0, 1.0)), 0.0, 1.0);
		const double fw = w, fh = h;
		// The part of the picture to draw, where to put it, and how strong.
		double sx = 0, sy = 0, sw = fw, sh = fh, dx = 0, dy = 0, scale_x = 1.0, scale_y = 1.0, strength = 1.0;
		switch (style) {
		case Style::Fade:
		case Style::Morph:
			strength = v;
			break;
		case Style::Cut:
			strength = v > 0.0 ? 1.0 : 0.0;
			break;
		case Style::SlideLeft:
			sx = (1.0 - v) * fw;
			sw = fw - sx;
			break;
		case Style::SlideRight:
			sw = v * fw;
			dx = fw - sw;
			break;
		case Style::SlideUp:
			sy = (1.0 - v) * fh;
			sh = fh - sy;
			break;
		case Style::SlideDown:
			sh = v * fh;
			dy = fh - sh;
			break;
		case Style::WipeLeft: // the capture is uncovered from the right
			sw = v * fw;
			break;
		case Style::WipeRight:
			sw = v * fw;
			sx = dx = fw - sw;
			break;
		case Style::WipeUp:
			sh = v * fh;
			break;
		case Style::WipeDown:
			sh = v * fh;
			sy = dy = fh - sh;
			break;
		case Style::Zoom: {
			// It grows as it fades: the middle of it, stretched to the size.
			const double grow = 1.0 + (1.0 - v) * 0.4;
			sw = fw / grow;
			sh = fh / grow;
			sx = (fw - sw) / 2.0;
			sy = (fh - sh) / 2.0;
			scale_x = scale_y = grow;
			strength = v;
			break;
		}
		}
		if (sw < 0.5 || sh < 0.5 || strength <= 0.0)
			return;
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), (float)strength);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_translate3f((float)dx, (float)dy, 0.0f);
		gs_matrix_scale3f((float)scale_x, (float)scale_y, 1.0f);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite_subregion(texture, 0, (uint32_t)std::lround(sx), (uint32_t)std::lround(sy),
						 (uint32_t)std::max(1L, std::lround(sw)),
						 (uint32_t)std::max(1L, std::lround(sh)));
		gs_matrix_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	// Draws a texture over the whole size at a strength from 0 to 1.
	void draw(gs_texture_t *texture, uint32_t w, uint32_t h, float strength)
	{
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), strength);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite(texture, 0, w, h);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	// The last frame of a capture that has ended, covering the size of the
	// moment: as large as needed, centred, what does not fit left out.
	void draw_kept(gs_texture_t *texture, uint32_t w, uint32_t h, bool stretch)
	{
		const float scale = std::max((float)w / (float)kept_w_, (float)h / (float)kept_h_);
		// (Morph stretches all of it over the frame; the others cover it.)
		const uint32_t part_w = stretch ? kept_w_
						: std::clamp((uint32_t)std::lround((float)w / scale), 1u, kept_w_);
		const uint32_t part_h = stretch ? kept_h_
						: std::clamp((uint32_t)std::lround((float)h / scale), 1u, kept_h_);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
		gs_effect_set_float(gs_effect_get_param_by_name(effect_, "opacity"), 1.0f);
		const bool linear = gs_get_linear_srgb();
		const bool was_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, texture);
		else
			gs_effect_set_texture(image, texture);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_scale3f((float)w / (float)part_w, (float)h / (float)part_h, 1.0f);
		while (gs_effect_loop(effect_, "Draw"))
			gs_draw_sprite_subregion(texture, 0, (kept_w_ - part_w) / 2, (kept_h_ - part_h) / 2, part_w,
						 part_h);
		gs_matrix_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(was_srgb);
	}

	void let_go()
	{
		if (picture_ && holding_) {
			obs_source_dec_active(picture_);
			obs_source_dec_showing(picture_);
		}
		holding_ = false;
		obs_source_release(picture_);
		picture_ = nullptr;
	}

	// The source that draws the picture: one of our own for the start image
	// or a file, or the one named.
	void make_picture(Picture kind, const std::string &file, const std::string &named)
	{
		let_go();
		if (kind == Picture::Source) {
			obs_source_t *found = named.empty() ? nullptr : obs_get_source_by_name(named.c_str());
			// Not the capture this is on, which would draw itself for ever.
			if (found && found == obs_filter_get_parent(source_)) {
				obs_source_release(found);
				found = nullptr;
			}
			picture_ = found;
			return;
		}
		if (kind == Picture::File && !file.empty()) {
			obs_data_t *settings = obs_data_create();
			const bool video = is_video_file(file);
			obs_data_set_string(settings, video ? "local_file" : "file", file.c_str());
			if (video) {
				obs_data_set_bool(settings, "is_local_file", true);
				obs_data_set_bool(settings, "looping", true);
				obs_data_set_bool(settings, "restart_on_activate", false);
			}
			picture_ = obs_source_create_private(video ? "ffmpeg_source" : "image_source",
							     "Standby Picture", settings);
			obs_data_release(settings);
			if (picture_ && video)
				obs_source_set_muted(picture_, true);
			return;
		}
		picture_ = obs_source_create_private(kStartImageId, "Standby Picture", nullptr);
	}

	obs_source_t *source_;
	std::mutex mutex_;
	Picture kind_ = Picture::StartImage;
	std::string file_, named_;
	double fade_s_ = 0.8, back_s_ = 0.8;
	Style style_ = Style::Fade;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	bool glide_size_ = true;
	uint32_t last_w_ = 0, last_h_ = 0;
	bool placed_ = false;
	bool changed_ = true;
	double mix_ = 0.0;               // how much of the picture shows
	bool empty_ = false;             // the capture has nothing to show
	std::atomic<bool> cover_{false}; // the picture is shown on purpose, over a capture that has one
	Look own_;                       // the filter's own transition
	bool lent_ = false;              // another's is in use until the picture has gone again
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
	bool own_shape_ = true;            // while waiting, the picture keeps its own shape
	uint32_t live_w_ = 0, live_h_ = 0; // the capture's size while it shows
	// The size, gliding from one to the next.
	double cur_w_ = 0.0, cur_h_ = 0.0, from_w_ = 0.0, from_h_ = 0.0, to_w_ = 0.0, to_h_ = 0.0, glide_ = 1.0;
	// Used on the graphics thread only.
	obs_source_t *picture_ = nullptr;
	bool holding_ = false;
	gs_texrender_t *render_ = nullptr;
	gs_texrender_t *render_live_ = nullptr;
	gs_texrender_t *render_stand_in_ = nullptr;
	// In the source's place while it is hidden: wanted, with which change,
	// and the private source its stand-in items show. (The last two on the
	// tick only.)
	bool when_hidden_ = false;
	ItemChange hidden_change_;
	bool hidden_changed_ = true;
	obs_source_t *stand_in_ = nullptr;
	// The change last given to the items of each scene with stand-ins
	// (scene pointers as keys only; the tick's own).
	std::map<obs_scene_t *, std::string> applied_;
	// Made by a Scene Standby (`managed`, which says in which scene), and
	// whether one covers the source now. Idle: no image for an empty capture.
	bool managed_ = false;
	std::string managed_by_, owner_scene_;
	bool idle_ = false, orphan_ = false;
	double uncovered_s_ = 0.0;
	int made_ = 0; // how many times the image has been made (for the tests)
	// Each item with its stand-in, both held, from the last look.
	// Each item of the source and its stand-in, known by their ids in their
	// scene, never held: the items hold the source this filter is on, so
	// holding them kept the source alive for good once its scenes were
	// cleared, and OBS would not change scene collection or close cleanly
	// ("Not all sources were cleared ... - DSLR", beta139 to beta147).
	// Whether an item's stand-in shows: while the item is hidden, but not
	// when it is hidden because a mode of its scene that is not showing
	// holds it hidden (Link Visibility's Follow the first source). The
	// owner's camera belongs to the Intermission mode of one holding scene;
	// its standby image stood in its place in every other mode.
	static bool stands_in(obs_source_t *scene, obs_sceneitem_t *item)
	{
		return !obs_sceneitem_visible(item) && !mode_holds_hidden(scene, item);
	}

	struct Held {
		obs_weak_source_t *scene;
		int64_t item, twin;
	};
	std::vector<Held> pairs_;
	void forget_pairs()
	{
		for (const Held &pair : pairs_)
			obs_weak_source_release(pair.scene);
		pairs_.clear();
	}
	double stand_in_check_ = 1.0;
	uint32_t kept_w_ = 0, kept_h_ = 0; // the size of the capture's last frame in render_live_
	double order_check_ = 1.0, waited_ = 0.0;
	gs_effect_t *effect_ = nullptr;
};

bool picture_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto kind = static_cast<Picture>(obs_data_get_int(settings, "picture"));
	obs_property_set_visible(obs_properties_get(props, "file"), kind == Picture::File);
	obs_property_set_visible(obs_properties_get(props, "source"), kind == Picture::Source);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<Standby *>(data);
	obs_properties_t *props = obs_properties_create();
	std::string by;
	bool orphan = false;
	const bool managed = self && self->managed(by, orphan);
	// Set by a Scene Standby: it says so, and the image's controls are left
	// out (whatever was set here would be put back from the scene filter).
	if (managed) {
		obs_property_t *status =
			obs_properties_add_text(props, "status",
						orphan ? obs_module_text("Standby.Status.Orphan")
						       : text_with("Standby.Status.Managed", by).c_str(),
						OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(status, true);
		set_tip(status, obs_module_text("Standby.Status.Managed.Tip"));
	} else {
		obs_properties_add_text(props, "status",
					obs_module_text(self && self->showing() ? "Standby.Status.Showing"
										: "Standby.Status.Waiting"),
					OBS_TEXT_INFO);
	}
	obs_property_t *cover = obs_properties_add_button2(
		props, "cover_now", obs_module_text("Standby.Cover"),
		[](obs_properties_t *, obs_property_t *, void *param) {
			if (param)
				static_cast<Standby *>(param)->toggle_cover();
			return false;
		},
		self);
	set_tip(cover, obs_module_text("Standby.Cover.Tip"));
	if (managed) {
		obs_property_t *help =
			obs_properties_add_text(props, "help", obs_module_text("Standby.Help"), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(help, true);
		return props;
	}

	obs_property_t *picture = obs_properties_add_list(props, "picture", obs_module_text("Standby.Picture"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Start"), (int)Picture::StartImage);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.File"), (int)Picture::File);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Source"), (int)Picture::Source);
	set_tip(picture, obs_module_text("Standby.Picture.Tip"));
	obs_property_set_modified_callback(picture, picture_modified);

	obs_properties_add_path(props, "file", obs_module_text("Standby.File"), OBS_PATH_FILE,
				obs_module_text("Standby.File.Filter"), nullptr);
	obs_property_t *source = obs_properties_add_list(props, "source", obs_module_text("Standby.Source"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	struct Fill {
		obs_property_t *list;
		obs_source_t *self;
	} fill{source, self ? obs_filter_get_parent(self->source()) : nullptr};
	obs_property_list_add_string(source, obs_module_text("Standby.Source.None"), "");
	obs_enum_sources(
		[](void *param, obs_source_t *candidate) {
			auto *f = static_cast<Fill *>(param);
			const char *name = obs_source_get_name(candidate);
			if (candidate != f->self && name && *name &&
			    (obs_source_get_output_flags(candidate) & OBS_SOURCE_VIDEO))
				obs_property_list_add_string(f->list, name, name);
			return true;
		},
		&fill);

	obs_property_t *own_shape = obs_properties_add_bool(props, "own_shape", obs_module_text("Standby.OwnShape"));
	set_tip(own_shape, obs_module_text("Standby.OwnShape.Tip"));

	// How the picture gives way to the capture and comes back.
	obs_property_t *style = obs_properties_add_list(props, "style", obs_module_text("Standby.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	const struct {
		Style value;
		const char *text;
	} styles[] = {{Style::Fade, "Standby.Style.Fade"},           {Style::Cut, "Standby.Style.Cut"},
		      {Style::SlideLeft, "Standby.Style.SlideLeft"}, {Style::SlideRight, "Standby.Style.SlideRight"},
		      {Style::SlideUp, "Standby.Style.SlideUp"},     {Style::SlideDown, "Standby.Style.SlideDown"},
		      {Style::WipeLeft, "Standby.Style.WipeLeft"},   {Style::WipeRight, "Standby.Style.WipeRight"},
		      {Style::WipeUp, "Standby.Style.WipeUp"},       {Style::WipeDown, "Standby.Style.WipeDown"},
		      {Style::Zoom, "Standby.Style.Zoom"},           {Style::Morph, "Standby.Style.Morph"}};
	for (const auto &entry : styles)
		obs_property_list_add_int(style, obs_module_text(entry.text), (int)entry.value);
	set_tip(style, obs_module_text("Standby.Style.Tip"));
	obs_property_set_modified_callback(style, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		const bool timed = obs_data_get_int(settings, "style") != (int)Style::Cut;
		for (const char *name : {"fade_ms", "back_ms", "ease_curve", "ease_mode", "glide_size"})
			obs_property_set_visible(obs_properties_get(p, name), timed);
		return true;
	});
	obs_property_t *fade =
		obs_properties_add_int_slider(props, "fade_ms", obs_module_text("Standby.Fade"), 0, 5000, 50);
	obs_property_int_set_suffix(fade, " ms");
	set_tip(fade, obs_module_text("Standby.Fade.Tip"));
	obs_property_t *back =
		obs_properties_add_int_slider(props, "back_ms", obs_module_text("Standby.Back"), 0, 5000, 50);
	obs_property_int_set_suffix(back, " ms");
	set_tip(back, obs_module_text("Standby.Back.Tip"));
	add_easing_properties(props, "");
	obs_property_t *glide = obs_properties_add_bool(props, "glide_size", obs_module_text("Standby.GlideSize"));
	set_tip(glide, obs_module_text("Standby.GlideSize.Tip"));

	// In the source's place while the source is hidden.
	obs_property_t *when_hidden =
		obs_properties_add_bool(props, "when_hidden", obs_module_text("Standby.WhenHidden"));
	set_tip(when_hidden, obs_module_text("Standby.WhenHidden.Tip"));
	add_item_change_properties(props, self ? self->source() : nullptr, nullptr);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Standby.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool standby_cover_toggle(obs_source_t *source)
{
	bool found = false;
	if (!source)
		return false;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (*static_cast<bool *>(param) || !id || strcmp(id, kStandbyId) != 0 ||
			    !obs_source_enabled(filter))
				return;
			calldata_t cd = {};
			calldata_set_bool(&cd, "toggle", true);
			if (proc_handler_call(obs_source_get_proc_handler(filter), "cover", &cd))
				*static_cast<bool *>(param) = true;
			calldata_free(&cd);
		},
		&found);
	return found;
}

bool standby_cover_set(obs_source_t *source, bool on, int style, int ms, int curve, int mode)
{
	struct Ask {
		bool on;
		int style, ms, curve, mode;
		bool found;
	} ask{on, style, ms, curve, mode, false};
	if (!source)
		return false;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *a = static_cast<Ask *>(param);
			const char *id = obs_source_get_unversioned_id(filter);
			if (a->found || !id || strcmp(id, kStandbyId) != 0 || !obs_source_enabled(filter))
				return;
			calldata_t cd = {};
			calldata_set_bool(&cd, "toggle", false);
			calldata_set_bool(&cd, "on", a->on);
			calldata_set_int(&cd, "style", a->style);
			calldata_set_int(&cd, "ms", a->ms);
			calldata_set_int(&cd, "curve", a->curve);
			calldata_set_int(&cd, "mode", a->mode);
			if (proc_handler_call(obs_source_get_proc_handler(filter), "cover", &cd))
				a->found = true;
			calldata_free(&cd);
		},
		&ask);
	return ask.found;
}

bool standby_waiting(obs_source_t *source)
{
	bool waiting = false;
	if (!source)
		return false;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || strcmp(id, kStandbyId) != 0 || !obs_source_enabled(filter))
				return;
			calldata_t cd = {};
			if (proc_handler_call(obs_source_get_proc_handler(filter), "get_standing", &cd) &&
			    calldata_bool(&cd, "waiting"))
				*static_cast<bool *>(param) = true;
			calldata_free(&cd);
		},
		&waiting);
	return waiting;
}

void register_standby()
{
	obs_source_info info = {};
	info.id = kStandbyId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Standby.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Standby(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Standby *>(data);
	};
	// Taken off its source (or the source going): that source's items are
	// no longer this filter's, whatever comes later at the same address.
	// (In `destroy` the filter has no parent any more.)
	info.filter_remove = [](void *, obs_source_t *parent) {
		stood_in_forget(parent);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Standby *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "picture", (int)Picture::StartImage);
		obs_data_set_default_int(settings, "fade_ms", 800);
		obs_data_set_default_int(settings, "back_ms", 800);
		obs_data_set_default_int(settings, "style", (int)Style::Fade);
		obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
		obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
		obs_data_set_default_bool(settings, "glide_size", true);
		obs_data_set_default_bool(settings, "own_shape", true);
		obs_data_set_default_bool(settings, "placed", false);
		obs_data_set_default_bool(settings, "when_hidden", false);
		obs_data_set_default_bool(settings, "managed", false);
		obs_data_set_default_string(settings, "managed_by", "");
		item_change_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<Standby *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Standby *>(data)->render();
	};
	info.get_width = [](void *data) {
		return static_cast<Standby *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<Standby *>(data)->height();
	};
	register_timed(&info);

	// What a stand-in item shows: the source itself, drawn so that its
	// Standby filter gives its picture alone. Not offered in OBS's list of
	// sources: the filter makes it.
	obs_source_info stand_in = {};
	stand_in.id = kStandInId;
	stand_in.type = OBS_SOURCE_TYPE_INPUT;
	stand_in.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CAP_DISABLED;
	stand_in.get_name = [](void *) {
		return obs_module_text("Standby.StandIn");
	};
	stand_in.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		obs_source_t *of = obs_get_source_by_name(obs_data_get_string(settings, "of"));
		obs_weak_source_t *weak = of ? obs_source_get_weak_source(of) : nullptr;
		obs_source_release(of);
		// (Something to hand back even with no source: null means failure.)
		return new obs_weak_source_t *(weak);
	};
	stand_in.destroy = [](void *data) {
		auto *weak = static_cast<obs_weak_source_t **>(data);
		obs_weak_source_release(*weak);
		delete weak;
	};
	auto of = [](void *data) {
		return obs_weak_source_get_source(*static_cast<obs_weak_source_t **>(data));
	};
	static const auto held = of;
	stand_in.get_width = [](void *data) -> uint32_t {
		obs_source_t *source = held(data);
		const uint32_t w = source ? obs_source_get_width(source) : 0;
		obs_source_release(source);
		return w;
	};
	stand_in.get_height = [](void *data) -> uint32_t {
		obs_source_t *source = held(data);
		const uint32_t h = source ? obs_source_get_height(source) : 0;
		obs_source_release(source);
		return h;
	};
	stand_in.video_render = [](void *data, gs_effect_t *) {
		obs_source_t *source = held(data);
		if (!source)
			return;
		t_stand_in = true;
		obs_source_video_render(source);
		t_stand_in = false;
		obs_source_release(source);
	};
	register_timed(&stand_in);
	// Their items' transitions are the Standby filter's to set, in the
	// scenes where it has stand-ins for them (every scene with "Show when
	// source is hidden", a Scene Standby's own scene with its list).
	item_transitions_owner([](obs_sceneitem_t *item) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_scene_t *scene = obs_sceneitem_get_scene(item);
		obs_source_t *scene_source = scene ? obs_scene_get_source(scene) : nullptr;
		std::lock_guard lock(g_stood_in_mutex);
		return g_stood_in.count({scene_source, source}) > 0;
	});
}

} // namespace stagekit
